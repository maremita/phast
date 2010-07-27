/* Dmotif implementation using a sampling strategy for both parameter
   estimation and path prediction. Written by Adam Diehl, Copyright 2008. */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>
#include <misc.h>
#include <maf.h>
#include <sufficient_stats.h>
#include <tree_likelihoods.h>
#include <phylo_hmm.h>
#include <indel_history.h>
#include <dmotif_indel_mod.h>
#include <subst_distrib.h>
#include <dmotif_phmm.h>
#include <pssm.h>
#include <hashtable.h>
#include "dmsample.help"
#include <pthr.h>

#define DEFAULT_RHO 0.3
#define DEFAULT_PHI 0.5
#define DEFAULT_MU 0.01
#define DEFAULT_NU 0.01
#define DEFAULT_ZETA 0.001
#define DEFAULT_XI 0.0001
#define DEFAULT_BSAMPLES 200
#define DEFAULT_NSAMPLES 9000
#define DEFAULT_SAMPLE_INTERVAL 1
#define DEFAULT_CACHE_INTERVAL 200
#define DEFAULT_NTHREADS 0
#define DEFAULT_MMOD_TYPE "F81"

int main(int argc, char *argv[]) {
  char c, *key;
  int opt_idx, i, old_nnodes, **priors, *counts, cbstate, max_seqlen, csamples;
/*   int j; */
  int found = FALSE;
  double **tuple_scores;
  PooledMSA *blocks = NULL;
  MSA *msa;
  List *pruned_names = lst_new_ptr(5), *tmpl, *keys, *seqnames, 
    *cache_files = NULL, **zeroed_states = NULL;
  IndelHistory **ih = NULL;
  DMotifPmsaStruct *dmpmsa;
  DMotifPhyloHmm *dm;
  GFF_Set *predictions, *reference = NULL;
  GFF_Feature *f;
  PSSM *motif;
  Hashtable *path_counts;
  String *cbname;
  ThreadPool *pool;

#ifdef RPHAST
  GetRNGstate(); //seed R's random number generator
#endif

  struct option long_opts[] = {
    {"refidx", 1, 0, 'r'},
    {"rho", 1, 0, 'R'},
    {"burn-in-samples", 1, 0, 'b'},
    {"samples", 1, 0, 's'},
    {"sample-interval", 1, 0, 'v'},
    {"seqname", 1, 0, 'N'},
    {"idpref", 1, 0, 'P'},
    {"indel-model", 1, 0, 'I'},
    {"log", 1, 0, 'l'},
    {"reference-gff", 1, 0, 'g'},
    {"ref-as-prior", 0, 0, 'u'},
    {"force_priors", 0, 0, 'p'},
    {"dump-hash", 1, 0, 'D'},
    {"precomputed-hash", 1, 0, 'd'},
    {"recover-temp", 1, 0, 'T'},
    {"cache-fname", 1, 0, 'c'},
    {"cache-int", 1, 0, 'i'},
    {"quiet", 0, 0, 'q'},
    {"threads", 1, 0, 't'},
    {"hash-debug", 0, 0, 'm'},
    {"revcomp", 0, 0, 'C'},
    {"cond-on-subs", 0, 0, 'X'},
    {"cond-on-species", 1, 0, 'x'},
    {"xi-off", 0, 0, 'F'},
    {"mot-mod-type", 1, 0, 'S'},
    {"scale-by-branch", 0, 0, 'B'},
    {"nc-mot-indel-mode", 0, 0, 'j'},
    {"fix-params", 1, 0, 'f'},
    {"help", 0, 0, 'h'},
    {0, 0, 0, 0}
  };

  /* arguments and defaults for options */
  FILE *msa_f = NULL, *source_mod_f, *motif_f = NULL, *prior_f = NULL,
    *log = NULL, *ref_gff_f = NULL, *hash_f = NULL, *tmp_lst_f = NULL,
    *cond_spec_f = NULL;;
  TreeModel *source_mod;
  double rho = DEFAULT_RHO, mu = DEFAULT_MU, nu = DEFAULT_NU, 
    phi = DEFAULT_PHI, zeta = DEFAULT_ZETA, xi = DEFAULT_XI,
    alpha_c = -1, beta_c = -1, tau_c = -1, epsilon_c = -1,
    alpha_n = -1, beta_n = -1, tau_n = -1, epsilon_n = -1;
  int refidx = 0, bsamples = DEFAULT_BSAMPLES, nsamples = DEFAULT_NSAMPLES, 
    sample_interval = DEFAULT_SAMPLE_INTERVAL, do_ih = 0, 
    ref_as_prior = FALSE, force_priors = FALSE, precomputed_hash = FALSE,
    quiet = FALSE, cache_int = DEFAULT_CACHE_INTERVAL,
    nthreads = DEFAULT_NTHREADS, hash_debug = FALSE, revcomp = FALSE,
    do_zeroed = FALSE, xi_mode = TRUE, scale_by_branch = FALSE,
    ncm_idl_mode = 0, fix_params = FALSE;
  char *seqname = NULL, *idpref = NULL,
    *cache_fname = (char*)smalloc(STR_MED_LEN * sizeof(char));
  subst_mod_type mmod_type = tm_get_subst_mod_type(DEFAULT_MMOD_TYPE);

  snprintf(cache_fname, STR_MED_LEN, "dmsample_%ld", (long) time(0));
  
  while ((c = getopt_long(argc, argv,
			  "R:b:s:r:N:P:I:l:v:g:u:p:D:d:q:c:i:j:t:m:T:C:M:S:B:f:h",
			  long_opts, &opt_idx)) != -1) {
    switch (c) {
    case 'R':
      rho = get_arg_dbl_bounds(optarg, 0, 1);
      break;
    case 'b':
      bsamples = atoi(optarg);
      break;
    case 's':
      nsamples = atoi(optarg);
      break;
    case 'r':
      refidx = get_arg_int_bounds(optarg, 0, INFTY);
      break;
    case 'N':
      seqname = optarg;
      break;
    case 'P':
      idpref = optarg;
      break;
    case 'I':
      tmpl = get_arg_list_dbl(optarg);
      if (lst_size(tmpl) != 4 && lst_size(tmpl) != 8)
        die("ERROR: bad argument to --indel-model.\n");
      alpha_n = lst_get_dbl(tmpl, 0);
      beta_n = lst_get_dbl(tmpl, 1);
      tau_n = lst_get_dbl(tmpl, 2);
      epsilon_n = lst_get_dbl(tmpl, 3);
      if (lst_size(tmpl) == 6) {
        alpha_c = lst_get_dbl(tmpl, 4);
        beta_c = lst_get_dbl(tmpl, 5);
        tau_c = lst_get_dbl(tmpl, 6);
	epsilon_c = lst_get_dbl(tmpl, 7);
      } else {
        alpha_c = alpha_n; beta_c = beta_n; tau_c = tau_n;
	epsilon_c = epsilon_n;
      }
      lst_free(tmpl);    
      if (alpha_c <= 0 || alpha_c >= 1 || beta_c <= 0 || beta_c >= 1 ||
          tau_c <= 0 || tau_c >= 1 || epsilon_c <= 0 || epsilon_c >= 1 ||
	  alpha_n <= 0 || alpha_n >= 1 || beta_n <= 0 || beta_n >= 1 || 
	  tau_n <= 0 || tau_n >= 1 || epsilon_n <= 0 || epsilon_n >= 1)
        die("ERROR: bad argument to --indel-model.\n");
      do_ih = 1;
      break;
    case 'l':
      log = fopen_fname(optarg, "w");
      break;
    case 'v':
      sample_interval = get_arg_int_bounds(optarg, 0, INFTY);
      break;
    case 'g':
      ref_gff_f = fopen_fname(optarg, "r");
      fprintf(stderr, "Reading reference features from %s...\n", optarg);
      reference = gff_read_set(ref_gff_f);
      break;
    case 'u':
      ref_as_prior = TRUE;
      break;
    case 'p':
      force_priors = TRUE;
      break;
    case 'D':
      hash_f = fopen_fname(optarg, "w");
      break;
    case 'd':
      precomputed_hash = TRUE;
      hash_f = fopen_fname(optarg, "r");
      break;
    case 'c':
      snprintf(cache_fname, STR_MED_LEN, "%s", optarg);
      break;
    case 'i':
      cache_int = atoi(optarg);
      break;
    case 'q':
      quiet = TRUE;
      break;
    case 't':
      nthreads = atoi(optarg);
      break;
    case 'm':
      hash_debug = TRUE;
      break;
    case 'T':
      tmp_lst_f = fopen_fname(optarg, "r");
      precomputed_hash = TRUE;
      break;
    case 'C':
      revcomp = TRUE;
      break;
    case 'X':
      do_zeroed = TRUE;
      break;
    case 'x':
      cond_spec_f = fopen_fname(optarg, "r");
      do_zeroed = TRUE;
      break;
    case 'F':
      xi_mode = FALSE;
      break;
    case 'S':
      mmod_type = tm_get_subst_mod_type(optarg);
      break;
    case 'B':
      scale_by_branch = TRUE;
      break;
    case 'j':
      ncm_idl_mode = 1;
      break;
    case 'f':
      fix_params = TRUE;
      tmpl = get_arg_list_dbl(optarg);
      if (lst_size(tmpl) != 1 && lst_size(tmpl) != 4 && lst_size(tmpl) != 5)
	die("ERROR: bad argument to --fix-params.\n");
      if (lst_size(tmpl) == 1) {
	break;
      } else {
	mu = lst_get_dbl(tmpl, 0);
	nu = lst_get_dbl(tmpl, 1);
	phi = lst_get_dbl(tmpl, 2);
	zeta = lst_get_dbl(tmpl, 3);
	if (lst_size(tmpl) == 5)
	  xi = lst_get_dbl(tmpl, 4);
      }
      lst_free(tmpl);
      break;
    case 'h':
      printf(HELP);
      exit(0);
    case '?':
      die("Bad argument.  Try 'dmsample -h'.\n");
    }
  }

  if (optind != argc - 4)
    die("Four arguments required.  Try 'dmsample -h'.\n");

  if (ref_gff_f != NULL && log == NULL) {
    if (!ref_as_prior)
      fprintf(stderr, "WARNING: Useless application of --reference-gff. Try 'dmsample -h'\n");
  }
  
  if (ref_as_prior && ref_gff_f == NULL)
    die("ERROR: --ref-as-prior requires --reference-gff. Try 'dmsample -h'\n");

  if (force_priors) {
    if (!ref_gff_f)
      die("ERROR: --force-priors requires --reference-gff. Try 'dmsample -h'\n");
    if (!ref_as_prior)
      ref_as_prior = TRUE;
  }

  /* Handle arguments */
  msa_f = fopen_fname(argv[optind], "r");
  source_mod_f = fopen_fname(argv[optind+1], "r");
  motif_f = fopen_fname(argv[optind+2], "r");
  prior_f = fopen_fname(argv[optind+3], "r");

  /* Read in the tree model and do some sanity checks */
  fprintf(stderr, "Reading tree model from %s...\n", argv[optind+1]);
  source_mod = tm_new_from_file(source_mod_f);

  if (source_mod->nratecats > 1)
    die("ERROR: rate variation not currently supported.\n");

  if (source_mod->order > 0)
    die("ERROR: only single nucleotide models are currently supported.\n");

  if (!tm_is_reversible(source_mod->subst_mod))
    fprintf(stderr, "WARNING: p-value computation assumes reversibility and your model is non-reversible.\n");

  /* Read in the motif model */
  fprintf(stderr, "Reading motif model from %s...\n", argv[optind+2]);
  motif = mot_read(motif_f);

  /* Instead of reading in a sequence at a time, all sequences must be read in
     and stored in memory -- a path must be sampled for all alignments during
     each sampling iteration. Emissions only need to be computed once for all
     alignments, however, since we are not sampling rho. */

  /* read alignments */
  fprintf(stderr, "Reading alignments from %s...\n", argv[optind]);
  dmpmsa = dms_read_alignments(msa_f, do_ih, quiet, revcomp, do_zeroed,
			       cond_spec_f);

  blocks = dmpmsa->pmsa;
  seqnames = dmpmsa->seqnames;
  if (do_ih)
    ih = dmpmsa->ih;
  if (do_zeroed)
    zeroed_states = dmpmsa->zeroed_states;
  max_seqlen = dmpmsa->max_seqlen;
  
/*   fprintf(stderr, "max_seqlen %d\n", max_seqlen); */
/*   fprintf(stderr, "ntuples %d\n", blocks->pooled_msa->ss->ntuples); */

  /* Read in priors for parameter estimation */
  if (!precomputed_hash) {
    fprintf(stderr, "Reading transition parameter priors from %s...\n", 
	    argv[optind + 3]);
    priors = (int**)smalloc((xi_mode == FALSE ? 4 : 5) * sizeof(int*));
    for (i = 0; i < 5; i++) {
      if (xi_mode == FALSE && i == 4)
	continue;
      priors[i] = (int*)smalloc(2 * sizeof(int));
      priors[i][0] = priors[i][1] = 0;
    }
    dms_read_priors(priors, prior_f, xi_mode);
  }
  /* Do some cleanup of files we no longer need */
  if (ref_gff_f != NULL)
    fclose(ref_gff_f);
  fclose(msa_f);
  fclose(source_mod_f);
  fclose(motif_f);
  fclose(prior_f);
  if (cond_spec_f != NULL)
    fclose(cond_spec_f);
  
  /* prune tree, if necessary */
  old_nnodes = source_mod->tree->nnodes;
  tm_prune(source_mod, lst_get_ptr(blocks->source_msas, 0), pruned_names);

  if (lst_size(pruned_names) == (old_nnodes + 1) / 2)
    die("ERROR: no match for leaves of tree in alignment (leaf names must match alignment names).\n");
  if (lst_size(pruned_names) > 0) {
    fprintf(stderr, "WARNING: pruned away leaves of tree with no match in alignment (");
    for (i = 0; i < lst_size(pruned_names); i++)
      fprintf(stderr, "%s%s", ((String*)lst_get_ptr(pruned_names, i))->chars,
              i < lst_size(pruned_names) - 1 ? ", " : ").\n");
  }
  lst_free_strings(pruned_names);
  lst_free(pruned_names);

  /* this has to be done after pruning tree */
  tr_name_ancestors(source_mod->tree);

  /* also make sure match for reference sequence in tree */
  if (refidx > 0) {
    for (i = 0, found = FALSE; !found && i < source_mod->tree->nnodes; i++) {
      TreeNode *n = lst_get_ptr(source_mod->tree->nodes, i);
      if (!strcmp(n->name, ((MSA*)lst_get_ptr(blocks->source_msas, 0))->names[refidx-1]))
        found = TRUE;
    }
    if (!found) die("ERROR: no match for reference sequence in tree.\n");
  }
  
  dm = dm_new(source_mod, motif, rho, mu, nu, phi, zeta, xi, xi_mode, 
	      alpha_c, beta_c, tau_c, epsilon_c, alpha_n, beta_n, tau_n, 
	      epsilon_n, FALSE, FALSE, FALSE, FALSE, xi_mode, mmod_type,
	      scale_by_branch, ncm_idl_mode);

/*   for (i = 0; i < dm->phmm->hmm->nstates; i++) { */
/*     fprintf(stderr, "state %d, inform_reqd %d\n", */
/* 	    i, dm->phmm->mods[i]->inform_reqd); */
/*   } */
/*   for (i = 0; i < dm->phmm->hmm->nstates; i++) { */
/*     fprintf(stderr, "state %d, cat %s\n", i, */
/* 	    ((String*)cm_get_feature(dm->phmm->cm, i))->chars); */
/*   } */
/*   return 0; */

/*   for (i = 0; i < lst_size(blocks->source_msas); i++) */
/*     dms_write_zeroed_states(stdout, zeroed_states[i]); */
/*   return 0; */
/*   for (i = 0; i < lst_size(blocks->source_msas); i++) { */
/*     fprintf(stderr, "zeroed states for sequence %d:\n", i); */
/*     dms_print_zeroed_states(stderr, dm, source_mod->tree, zeroed_states[i]); */
/*   } */
/*   return 0; */
  
  /* Precompute P matrices for all states -- this avoids collisions between
     threads later */
  if (!quiet && !precomputed_hash)
    fprintf(stderr, "Initializing P-matrices in phylogenetic models...\n");
  dm_set_subst_mods(dm);

  /* Prepare a thread pool to keep all cores working */
  pool = thr_pool_init(nthreads);

  /* If reconstructing a run from temp files, build the cache_files list */
  if (precomputed_hash == TRUE && tmp_lst_f != NULL) {
    cache_files = dms_read_tmp_from_file(tmp_lst_f);
  }
  
  /* Prepare the emissions by tuple and state. Later, emissions for each seq
     will be reconsitituted on the fly for the forward and stochastic
     traceback algorithms. This requires a dummy MSA of all tuples, in order,
     for emissions computation, an nstates * ntuples matrix for the tuple-wise
     emission scores and a nstates * max_seqlen matrix for the sequence-wise
     emission scores. Not needed if using a precomputed hash. */

  if (!precomputed_hash) {
    /* Contains the non-redundant col_tuples matrix */
    msa = blocks->pooled_msa;

    fprintf(stderr,
	    "Computing emission probabilities for %d distinct tuples...\n",
	    msa->ss->ntuples);
    
    /* Some hacks to please tl_compute_log_likelihood -- avoids having to use
       a dummy msa to compute emissions. */
    msa->length = msa->ss->ntuples;
    msa->ss->tuple_idx = (int*)smalloc(msa->ss->ntuples 
				       * sizeof(int));
    for (i = 0; i < msa->ss->ntuples; i++)
      msa->ss->tuple_idx[i] = i; /* One to one mapping of column to tuple */

    /* tuple-wise emissions matrix */
    tuple_scores = (double**)smalloc(dm->phmm->hmm->nstates * sizeof(double*));
    for (i = 0; i < dm->phmm->hmm->nstates; i++)
      tuple_scores[i] = (double*)smalloc(msa->ss->ntuples * sizeof(double));

    /* Assign dm->phmm->emissions as the tuple-wise table for computation
       purposes */
    dm->phmm->emissions = tuple_scores;
    dm->phmm->alloc_len = msa->ss->ntuples;
    
    /* Compute the tuple-wise emissions matrix */
    dms_compute_emissions(dm->phmm, msa, quiet, pool, nthreads);

/*     return 0; */

/*     char tuple[100]; */
/*     for (i = 0; i < msa->length; i++) { */
/*       tuple_to_string_pretty(tuple, msa, i); */
/*       fprintf(stderr, "tuple_idx %d: %s\n", i, tuple); */
/*     } */
/*     return 0; */

/*     dms_dump_matrices(stderr, tuple_scores, NULL, NULL, msa->ss->ntuples, */
/* 		      dm->phmm->hmm->nstates); */

    /* Adjust for missing data */
    fprintf(stderr, "Adjusting emissions for missing data...\n");
    dm_handle_missing_data(dm, msa);

/*     dms_dump_matrices(stderr, tuple_scores, NULL, NULL, msa->ss->ntuples, */
/* 		      dm->phmm->hmm->nstates); */
/*     return 0; */
    
    /** Call the sampler **/
    fprintf(stderr, "Sampling state paths...\n");
    
/*     cache_files = dms_sample_paths(dm, blocks, tuple_scores, ih, seqnames, */
/* 				   max_seqlen, bsamples, nsamples, */
/* 				   sample_interval, priors, log, reference, */
/* 				   ref_as_prior, force_priors, quiet, */
/* 				   cache_fname, cache_int); */
    cache_files = dms_sample_paths_pthr(dm, blocks, tuple_scores, ih, seqnames,
					max_seqlen, bsamples, nsamples,
					sample_interval, priors, log, 
					reference, ref_as_prior, force_priors,
					quiet, cache_fname, cache_int, pool,
					nthreads, zeroed_states, xi_mode,
					scale_by_branch, fix_params);
    

    /* Free emissions matrix */
    for (i = 0; i < dm->phmm->hmm->nstates; i++) {
      free(tuple_scores[i]);
    }
    free(tuple_scores);
    dm->phmm->emissions = NULL;

    fprintf(stderr, "Reloading cached data from disk...\n");
    path_counts = dms_uncache(cache_files, 
			      max(10*lst_size(blocks->source_msas), 10000),
			      ((2*dm->k)+2), &csamples, 1);
/*     fprintf(stderr, "nsamples %d, csamples %d\n", nsamples, csamples); */
    
  } else if (precomputed_hash && cache_files == NULL) {
    fprintf(stderr, "Reading sampling data from disk...\n");
/*     fprintf(stderr, "nsamples_init %d\t", nsamples); */
    path_counts = dms_read_hash(hash_f, ((2*dm->k)+2), &nsamples);
  } else {
    fprintf(stderr, "Reloading cached data from disk...\n");
/*     fprintf(stderr, "nsamples_init %d\t", nsamples); */
    path_counts = dms_uncache(cache_files, 
			      max(10*lst_size(blocks->source_msas), 10000),
			      ((2*dm->k)+2), &nsamples, 0);
  }
  
/*   fprintf(stderr, "nsamples_new %d\n", nsamples); */
  
  /* Can free priors */
  if (!precomputed_hash) {
    for (i = 0; i < 5; i++) {
      if (xi_mode == FALSE && i == 4)
	continue;
      free(priors[i]);
    }
    free(priors);
  }

  /* Dump hash, for debugging purposes. */
  if (hash_f != NULL && !precomputed_hash) {
    dms_write_hash(path_counts, hash_f, ((2*dm->k)+2), nsamples);
    /*     return 0; */
  } else {
    
    /* Generate a GFF from the features hash */
    fprintf(stderr, "Formatting output as GFF...\n");
    predictions = gff_new_set();
    cbname = str_new(STR_SHORT_LEN);
    str_append_charstr(cbname, "conserved-background");
    cbstate = cm_get_category(dm->phmm->cm, cbname);
    str_free(cbname);
    keys = hsh_keys(path_counts);
    for (i = 0; i < lst_size(keys); i++) {
      /* go through entries, build data for gff feture from each */
      key = lst_get_ptr(keys, i);
      /*     fprintf(stderr, "i %d lst_size %d key %s\n", i, lst_size(keys), key); */
      counts = hsh_get(path_counts, key);
      f = dms_motif_as_gff_feat(dm, blocks, seqnames, key, counts, nsamples,
				sample_interval, refidx);
      lst_push_ptr(predictions->features, f);
    }
    
    /* Free up some memory */
    lst_free(keys);  
    
    /* now output predictions */
    fprintf(stderr, "Writing GFF to stdout...\n");
    gff_print_set(stdout, predictions);

    gff_free_set(predictions);    
  }
  
  /* Free up all our allocated storage */
  dm_free(dm);
  dms_free_dmpmsa_struct(dmpmsa);
  if (reference != NULL)
    gff_free_set(reference);
  
  /* Clean up temp files and file list */
  if (!precomputed_hash) {
    for (i = 0; i < lst_size(cache_files); i++) {
      if (!hash_debug) {
	snprintf(cache_fname, STR_MED_LEN, "rm %s",
		((String*)lst_get_ptr(cache_files, i))->chars);
	/* delete the file */
	system(cache_fname);
      }
      str_free(lst_get_ptr(cache_files, i));
    }
    lst_free(cache_files); 
  }
  free(cache_fname);
  thr_pool_free(pool);
  hsh_free_with_vals(path_counts);
  fprintf(stderr, "Done.\n");
  
  return 0;
}

