/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014-2020
    National Technology & Engineering Solutions of Sandia, LLC (NTESS).
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:	 NonDMultifidelitySampling
//- Description: Implementation code for NonDMultifidelitySampling class
//- Owner:       Mike Eldred
//- Checked by:
//- Version:

#include "dakota_system_defs.hpp"
#include "dakota_data_io.hpp"
//#include "dakota_tabular_io.hpp"
#include "DakotaModel.hpp"
#include "DakotaResponse.hpp"
#include "NonDMultifidelitySampling.hpp"
#include "ProblemDescDB.hpp"
#include "ActiveKey.hpp"
#include "DakotaIterator.hpp"

static const char rcsId[]="@(#) $Id: NonDMultifidelitySampling.cpp 7035 2010-10-22 21:45:39Z mseldre $";

namespace Dakota {


/** This constructor is called for a standard letter-envelope iterator 
    instantiation.  In this case, set_db_list_nodes has been called and 
    probDescDB can be queried for settings from the method specification. */
NonDMultifidelitySampling::
NonDMultifidelitySampling(ProblemDescDB& problem_db, Model& model):
  NonDNonHierarchSampling(problem_db, model)
{ }


NonDMultifidelitySampling::~NonDMultifidelitySampling()
{ }


/** The primary run function manages the general case: a hierarchy of model 
    forms (from the ordered model fidelities within a HierarchSurrModel), 
    each of which may contain multiple discretization levels. */
void NonDMultifidelitySampling::core_run()
{
  //sequence_models(); // enforce correlation condition (*** AFTER PILOT ***)
  multifidelity_mc();

  // Notes on ACV + ensemble model classes:
  // > HierarchSurrModel is limiting (see MFMC) such that we may want to
  //   subsume it with NonHierarchSurrModel --> consider generalizations
  //   that can be deployed across the algorithm set:
  //   >> enhanced parallel usage --> avoid sync points in ACV clients
  //   >> enable sampling over shared + distinct model variable sets
}


/** This function performs control variate MC across two combinations of 
    model form and discretization level. */
void NonDMultifidelitySampling::multifidelity_mc()
{
  // Performs pilot + LF increment and then iterates with additional shared
  // increment + LF increment batches until prescribed MSE reduction is obtained

  IntRealVectorMap sum_H;
  IntRealMatrixMap sum_L_baseline, sum_LL, sum_LH;
  RealVector sum_HH, var_H, mse_iter0, mse_ratios, hf_targets;
  RealMatrix rho2_LH, eval_ratios;
  Sizet2DArray N_L_baseline, N_LH;  SizetArray num_H;
  size_t num_steps = numApprox + 1;
  initialize_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH);
  initialize_counts(N_L_baseline, num_H, N_LH);

  // Initialize for pilot sample
  numSamples = pilotSamples[numApprox]; // last in array

  while (numSamples && mlmfIter <= maxIterations) {

    // ------------------------------------------------------------------------
    // Compute shared increment targeting specified budget and/or MSE reduction
    // ------------------------------------------------------------------------
    // Scale sample profile based on maxFunctionEvals or convergenceTol,
    // but not both (for now)
    if (mlmfIter)
      update_hf_targets(eval_ratios, sequenceCost, mse_ratios, var_H, num_H,
			mse_iter0, hf_targets);

    // --------------------------------------------------------------------
    // Evaluate shared increment and update correlations, {eval,MSE}_ratios
    // --------------------------------------------------------------------
    if (numSamples) {
      shared_increment(mlmfIter); // spans ALL models, blocking
      accumulate_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH,
			 N_L_baseline, num_H, N_LH);
      increment_equivalent_cost(numSamples, sequenceCost, 0, num_steps);

      // First, compute the LF/HF evaluation ratio using shared samples,
      // averaged over QoI.  This includes updating var_H and rho2_LH.  Then,
      // compute the ratio of MC and ACV mean squared errors (for convergence).
      // This ratio incorporates the anticipated variance reduction from the
      // upcoming application of eval_ratios.
      compute_ratios(sum_L_baseline[1], sum_H[1], sum_LL[1], sum_LH[1], sum_HH,
		     sequenceCost, N_L_baseline, num_H, N_LH, var_H, rho2_LH,
		     eval_ratios, mse_ratios);
      // mse_iter0 only uses HF pilot since CV terms (sum_L_shared / N_shared -
      // sum_L_refined / N_refined) are zero prior to sample refinement.
      // (This differs from MLMC MSE^0 which uses pilot for all levels.)
      if (mlmfIter == 0) compute_mc_estimator_variance(var_H, num_H, mse_iter0);
    }
    //else
    //  Cout << "\nMFMC iteration " << mlmfIter
    //       << ": no shared sample increment" << std::endl;

    ++mlmfIter;
  }

  // -----------------------------------------------------------------
  // Compute N_L increments based on eval ratio applied to final num_H
  // -----------------------------------------------------------------
  // Note: these results do not affect the iteration above and can be performed
  // after num_H has converged, which simplifies maxFnEvals / convTol logic
  // (no need to further interrogate these throttles below)

  if (hf_targets.empty()) update_hf_targets(num_H, hf_targets); // pilot only
  // Pyramid/nested sampling: at step i, we sample approximation range
  // [0,numApprox-1-i] using the delta relative to the previous step
  IntRealMatrixMap sum_L_shared  = sum_L_baseline,
                   sum_L_refined = sum_L_baseline; // copies
  Sizet2DArray N_L_shared = N_L_baseline, N_L_refined = N_L_baseline; // copies
  for (size_t approx=numApprox; approx>0; --approx) {
    // *** TO DO NON_BLOCKING: PERFORM 2ND PASS ACCUMULATE AFTER 1ST PASS LAUNCH
    if (approx_increment(eval_ratios, N_L_refined, hf_targets, mlmfIter,
			 0, approx)) {
      // MFMC samples on [0, approx) --> sum_L_{shared,refined}
      accumulate_mf_sums(sum_L_shared, sum_L_refined, N_L_shared, N_L_refined,
			 0, approx);
      increment_equivalent_cost(numSamples, sequenceCost, 0, approx);
    }
  }

  // Compute/apply control variate parameter to estimate uncentered raw moments
  RealMatrix H_raw_mom(numFunctions, 4);
  mf_raw_moments(sum_L_baseline, sum_L_shared, sum_L_refined, sum_H, sum_LL,
		 sum_LH, N_L_baseline, N_L_shared, N_L_refined, num_H, N_LH,
		 H_raw_mom);
  // Convert uncentered raw moment estimates to final moments (central or std)
  convert_moments(H_raw_mom, momentStats);

  // post final sample counts back to NLev (needed for final eval summary)
  N_L_refined.push_back(num_H); // aggregate into a single Sizet2DArray
  bool multilev = (sequenceType == Pecos::RESOLUTION_LEVEL_SEQUENCE);
  inflate_final_samples(N_L_refined, multilev, secondaryIndex, NLev);
}


void NonDMultifidelitySampling::
update_hf_targets(const RealMatrix& eval_ratios, const RealVector& cost,
		  const RealVector& mse_ratios,  const RealVector& var_H,
		  const SizetArray& N_H, const RealVector& mse_iter0,
		  RealVector& hf_targets)
{
  // Full budget allocation: pilot sample + addtnl N_H; then optimal N_L 
  // > could consider under-relaxing the budget allocation to enable
  //   additional N_H increments + associated updates to shared samples
  //   for improving rho2_LH et al.
  if (maxFunctionEvals != SZ_MAX) {
    Cout << "Scaling profile for maxFunctionEvals = " << maxFunctionEvals;
    allocate_budget(eval_ratios, cost, hf_targets);
  }
  // MSE target = convTol * mse_iter0 = mse_ratio * var_H / N_H
  // --> N_H = mse_ratio * var_H / convTol / mse_iter0
  // Note: don't simplify further since mse_iter0 is fixed based on pilot
  else { //if (convergenceTol != -DBL_MAX) { // *** TO DO: to support both, need to retain default special value (-DBL_MAX) to detect a user spec
    Cout << "Scaling profile for convergenceTol = " << convergenceTol;
    hf_targets = mse_ratios;
    for (size_t qoi=0; qoi<numFunctions; ++qoi)
      hf_targets[qoi] *= var_H[qoi] / mse_iter0[qoi] / convergenceTol;
  }
  // numSamples is relative to N_H, but the approx_increments() below are
  // computed relative to hf_targets (independent of sunk cost for pilot)
  Cout << ": average HF target = " << average(hf_targets) << std::endl;
  numSamples = one_sided_delta(N_H, hf_targets, 1);
  //numSamples = std::min(num_samp_budget, num_samp_ctol); // enforce both
}


bool NonDMultifidelitySampling::
approx_increment(const RealMatrix& eval_ratios, const Sizet2DArray& N_L_refined,
		 const RealVector& hf_targets, size_t iter,
		 size_t start, size_t end)
{
  // Update LF samples based on evaluation ratio
  //   r = N_L/N_H -> N_L = r * N_H -> delta = N_L - N_H = (r-1) * N_H
  // Notes:
  // > the sample increment for the approx range is determined by approx[end-1]
  //   (helpful to refer to Figure 2(b) in ACV paper, noting index differences)
  // > N_L is updated prior to each call to approx_increment (*** if BLOCKING),
  //   allowing use of one_sided_delta() with latest counts
  // > eval_ratios have been scaled if needed to satisfy specified budget
  //   (maxFunctionEvals) in
  size_t qoi, approx = end - 1;
  RealVector lf_targets(numFunctions, false);
  for (qoi=0; qoi<numFunctions; ++qoi)
    lf_targets[qoi] = eval_ratios(qoi, approx) * hf_targets[qoi];

  // Choose avg, RMS, max? (trade-off: possible overshoot vs. more iteration)
  numSamples = one_sided_delta(N_L_refined[approx], lf_targets, 1); // average

  if (numSamples && start < end) {
    Cout << "\nMFMC sample increment = " << numSamples
	 << " for approximations [" << start+1 << ", " << end << ']';
    if (outputLevel >= DEBUG_OUTPUT)
      Cout << " computed from average delta between target:\n" << lf_targets
	   << "and current counts:\n" << N_L_refined[approx];
    Cout << std::endl;
    size_t start_qoi = start * numFunctions, end_qoi = end * numFunctions;
    activeSet.request_values(0);
    //activeSet.request_values(0, 0, start_qoi);
    activeSet.request_values(1, start_qoi, end_qoi);
    //activeSet.request_values(0, end_qoi, iteratedModel.response_size());
    ensemble_sample_increment(iter, start); // NON-BLOCK
    return true;
  }
  else {
    Cout << "\nNo MFMC approx sample increment for approximations ["
	 << start+1 << ", " << end << ']' << std::endl;
    return false;
  }
}


/** This version used by MFMC following shared_increment() */
void NonDMultifidelitySampling::
accumulate_mf_sums(IntRealMatrixMap& sum_L_baseline, IntRealVectorMap& sum_H,
		   IntRealMatrixMap& sum_LL, // each L with itself
		   IntRealMatrixMap& sum_LH, // each L with H
		   RealVector& sum_HH, Sizet2DArray& num_L_baseline,
		   SizetArray& num_H,  Sizet2DArray& num_LH)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // ordered by unorderedModels[i-1], i=1:numApprox --> truthModel

  using std::isfinite;
  Real lf_fn, hf_fn, lf_prod, hf_prod;
  IntRespMCIter r_it; IntRVMIter h_it; IntRMMIter lb_it, ll_it, lh_it;
  int lb_ord, h_ord, ll_ord, lh_ord, active_ord, m;
  size_t qoi, approx, lf_index, hf_index;
  bool hf_is_finite;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();

    if (outputLevel >= DEBUG_OUTPUT) { // sample dump for MATLAB checking
      size_t index = 0;
      for (approx=0; approx<=numApprox; ++approx)
	for (qoi=0; qoi<numFunctions; ++qoi, ++index)
	  Cout << fn_vals[index] << ' ';
      Cout << '\n';
    }

    hf_index = numApprox * numFunctions;
    for (qoi=0; qoi<numFunctions; ++qoi, ++hf_index) {
      hf_fn = fn_vals[hf_index];
      hf_is_finite = isfinite(hf_fn);
      // High accumulations:
      if (hf_is_finite) { // neither NaN nor +/-Inf
	++num_H[qoi];
	// High-High:
	sum_HH[qoi] += hf_fn * hf_fn; // a single vector for ord 1
	// High:
	h_it = sum_H.begin();  h_ord = (h_it == sum_H.end()) ? 0 : h_it->first;
	hf_prod = hf_fn;       active_ord = 1;
	while (h_ord) {
	  if (h_ord == active_ord) { // support general key sequence
	    h_it->second[qoi] += hf_prod;
	    ++h_it; h_ord = (h_it == sum_H.end()) ? 0 : h_it->first;
	  }
	  hf_prod *= hf_fn;  ++active_ord;
	}
      }
	
      for (approx=0; approx<numApprox; ++approx) {
	lf_index = approx * numFunctions + qoi;
	lf_fn = fn_vals[lf_index];

	// Low accumulations:
	if (isfinite(lf_fn)) {
	  ++num_L_baseline[approx][qoi];
	  if (hf_is_finite) ++num_LH[approx][qoi];

	  lb_it = sum_L_baseline.begin();
	  ll_it = sum_LL.begin();  lh_it = sum_LH.begin();
	  lb_ord = (lb_it == sum_L_baseline.end()) ? 0 : lb_it->first;
	  ll_ord = (ll_it == sum_LL.end())         ? 0 : ll_it->first;
	  lh_ord = (lh_it == sum_LH.end())         ? 0 : lh_it->first;
	  lf_prod = lf_fn;  active_ord = 1;
	  while (lb_ord || ll_ord || lh_ord) {
     
	    // Low baseline
	    if (lb_ord == active_ord) { // support general key sequence
	      lb_it->second(qoi,approx) += lf_prod;  ++lb_it;
	      lb_ord = (lb_it == sum_L_baseline.end()) ? 0 : lb_it->first;
	    }
	    // Low-Low
	    if (ll_ord == active_ord) { // support general key sequence
	      ll_it->second(qoi,approx) += lf_prod * lf_prod;  ++ll_it;
	      ll_ord = (ll_it == sum_LL.end()) ? 0 : ll_it->first;
	    }
	    // Low-High
	    if (lh_ord == active_ord) {
	      if (hf_is_finite) {
		hf_prod = hf_fn;
		for (m=1; m<active_ord; ++m)
		  hf_prod *= hf_fn;
		lh_it->second(qoi,approx) += lf_prod * hf_prod;
	      }
	      ++lh_it; lh_ord = (lh_it == sum_LH.end()) ? 0 : lh_it->first;
	    }

	    lf_prod *= lf_fn;  ++active_ord;
	  }
	}
      }
    }
  }
}


/** This version used by MFMC following approx_increment() */
void NonDMultifidelitySampling::
accumulate_mf_sums(IntRealMatrixMap& sum_L_shared,
		   IntRealMatrixMap& sum_L_refined, Sizet2DArray& num_L_shared,
		   Sizet2DArray& num_L_refined,
		   size_t approx_start, size_t approx_end)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // led by the approx Model responses of interest

  using std::isfinite;
  Real fn_val, prod;
  int ls_ord, lr_ord, active_ord;
  size_t qoi, fn_index, approx, shared_end = approx_end - 1;
  IntRespMCIter r_it; IntRMMIter ls_it, lr_it;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();
    fn_index = 0;

    // accumulate for leading set of models (omit trailing truth),
    // but note that resp and asv are full aggregated length
    for (approx=approx_start; approx<approx_end; ++approx) {

      SizetArray& num_L_sh_a  = num_L_shared[approx];
      SizetArray& num_L_ref_a = num_L_refined[approx];
      for (qoi=0; qoi<numFunctions; ++qoi, ++fn_index) {
	//if (asv[fn_index] & 1) {
	  prod = fn_val = fn_vals[fn_index];
	  if (isfinite(fn_val)) { // neither NaN nor +/-Inf
	    ++num_L_ref_a[qoi];  active_ord = 1;
	    lr_it = sum_L_refined.begin();
	    lr_ord = (lr_it == sum_L_refined.end()) ? 0 : lr_it->first;
	    // for pyramid sampling, shared range is one less than refined, i.e.
	    // sum_L_{shared,refined} are both accumulated for all approx except
	    // approx_end-1, which accumulates only sum_L_refined.  See z^1 sets
	    // in Fig. 2b of ACV paper.
	    if (approx < shared_end) {
	      ++num_L_sh_a[qoi];
	      ls_it = sum_L_shared.begin();
	      ls_ord = (ls_it == sum_L_shared.end()) ? 0 : ls_it->first;
	    }
	    else
	      ls_ord = 0;
	    while (ls_ord || lr_ord) {
	      // Low shared
	      if (ls_ord == active_ord) { // support general key sequence
		ls_it->second(qoi,approx) += prod;  ++ls_it;
		ls_ord = (ls_it == sum_L_shared.end()) ? 0 : ls_it->first;
	      }
	      // Low refined
	      if (lr_ord == active_ord) { // support general key sequence
		lr_it->second(qoi,approx) += prod;  ++lr_it;
		lr_ord = (lr_it == sum_L_refined.end()) ? 0 : lr_it->first;
	      }
	      prod *= fn_val;  ++active_ord;
	    }
	  }
	//}
      }
    }
  }
}


void NonDMultifidelitySampling::
compute_LH_correlation(const RealMatrix& sum_L_shared, const RealVector& sum_H,
		       const RealMatrix& sum_LL, const RealMatrix& sum_LH,
		       const RealVector& sum_HH, const Sizet2DArray& N_L_shared,
		       const SizetArray& N_H,    const Sizet2DArray& N_LH,
		       RealVector& var_H,        RealMatrix& rho2_LH)
{
  if (var_H.empty())     var_H.sizeUninitialized(numFunctions);
  if (rho2_LH.empty()) rho2_LH.shapeUninitialized(numFunctions, numApprox);

  size_t approx, qoi;
  for (approx=0; approx<numApprox; ++approx) {
    const Real* sum_L_shared_a = sum_L_shared[approx];
    const Real*       sum_LL_a =       sum_LL[approx];
    const Real*       sum_LH_a =       sum_LH[approx];
    const SizetArray&    N_L_a =   N_L_shared[approx];
    const SizetArray&   N_LH_a =         N_LH[approx];
    Real*            rho2_LH_a =      rho2_LH[approx];
    for (qoi=0; qoi<numFunctions; ++qoi)
      compute_correlation(sum_L_shared_a[qoi], sum_H[qoi], sum_LL_a[qoi],
			  sum_LH_a[qoi], sum_HH[qoi], N_L_a[qoi], N_H[qoi],
			  N_LH_a[qoi], var_H[qoi], rho2_LH_a[qoi]);
  }
}


void NonDMultifidelitySampling::
compute_ratios(const RealMatrix& sum_L_baseline, const RealVector& sum_H,
	       const RealMatrix& sum_LL, const RealMatrix& sum_LH,
	       const RealVector& sum_HH, const RealVector& cost,
	       const Sizet2DArray& N_L_baseline, const SizetArray& N_H,
	       const Sizet2DArray& N_LH, RealVector& var_H, RealMatrix& rho2_LH,
	       RealMatrix& eval_ratios,  RealVector& mse_ratios)
{
  compute_LH_correlation(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH,
			 N_L_baseline, N_H, N_LH, var_H, rho2_LH);

  mfmc_eval_ratios(rho2_LH, cost, eval_ratios);
  //RealVector avg_eval_ratios;
  //average(eval_ratios, 0, avg_eval_ratios);// average over qoi for each approx
  //Cout << "Average eval ratios from MFMC:\n" << avg_eval_ratios << std::endl;

  // Compute ratio of MSE for single-fidelity MC and MFMC
  // > Estimator Var for   MC = var_H / N_H = MSE (neglect HF bias)
  // > Estimator Var for MFMC = var_H (1-rho_LH(am1)^2) p / N_H^2 cost_H
  //   where budget p = cost^T eval_ratios N_H,  am1 = most-correlated approx
  //   --> EstVar = var_H (1-rho_LH(am1)^2) cost^T eval_ratios / N_H cost_H
  // > MSE ratio = EstVar_MFMC / EstVar_MC
  //   = (1-rho_LH(am1)^2) cost^T eval_ratios / cost_H
  if (mse_ratios.empty()) mse_ratios.sizeUninitialized(numFunctions);
  Real inner_prod, cost_H = cost[numApprox];
  size_t qoi, approx, num_am1 = numApprox - 1;
  for (qoi=0; qoi<numFunctions; ++qoi) {
    inner_prod = cost_H; // include cost_H * w_H
    for (approx=0; approx<numApprox; ++approx)
      inner_prod += cost[approx] * eval_ratios(qoi, approx); // cost_i * w_i
    mse_ratios[qoi] = (1. - rho2_LH(qoi, num_am1)) * inner_prod / cost_H;
  }

  if (outputLevel >= NORMAL_OUTPUT) {
    for (qoi=0; qoi<numFunctions; ++qoi) {
      for (approx=0; approx<numApprox; ++approx)
	Cout << "  QoI " << qoi+1 << " Approx " << approx+1
	   //<< ": cost_ratio = " << cost_H / cost_L
	     << ": rho2_LH = "    <<     rho2_LH(qoi,approx)
	     << " eval_ratio = "  << eval_ratios(qoi,approx) << '\n';
      Cout << "QoI " << qoi+1 << ": variance reduction factor = "
	   << mse_ratios[qoi] << '\n';
    }
    Cout << std::endl;
  }
}


void NonDMultifidelitySampling::
mf_raw_moments(IntRealMatrixMap& sum_L_baseline, IntRealMatrixMap& sum_L_shared,
	       IntRealMatrixMap& sum_L_refined,  IntRealVectorMap& sum_H,
	       IntRealMatrixMap& sum_LL,         IntRealMatrixMap& sum_LH,
	       //const RealMatrix& rho2_LH,
	       const Sizet2DArray& N_L_baseline, const Sizet2DArray& N_L_shared,
	       const Sizet2DArray& N_L_refined,  const SizetArray& N_H,
	       const Sizet2DArray& N_LH,         RealMatrix& H_raw_mom)
{
  if (H_raw_mom.empty()) H_raw_mom.shapeUninitialized(numFunctions, 4);

  Real beta, sum_H_mq;
  size_t approx, qoi, N_H_q;//, N_shared;
  for (int mom=1; mom<=4; ++mom) {
    RealMatrix& sum_L_base_m = sum_L_baseline[mom];
    RealMatrix& sum_L_sh_m   = sum_L_shared[mom];
    RealMatrix& sum_L_ref_m  = sum_L_refined[mom];
    RealVector& sum_H_m      = sum_H[mom];
    RealMatrix& sum_LL_m     = sum_LL[mom];
    RealMatrix& sum_LH_m     = sum_LH[mom];

    if (outputLevel >= NORMAL_OUTPUT)
      Cout << "Moment " << mom << ":\n";
    for (qoi=0; qoi<numFunctions; ++qoi) {
      sum_H_mq = sum_H_m[qoi];  N_H_q = N_H[qoi];
      Real& H_raw_mq = H_raw_mom(qoi, mom-1);
      H_raw_mq = sum_H_mq / N_H_q; // first term to be augmented
      for (approx=0; approx<numApprox; ++approx) {
	// Uses a baseline {sum,N}_L consistent with H,LL,LH accumulators:
	compute_mf_control(sum_L_base_m(qoi,approx), sum_H_mq,
			   sum_LL_m(qoi,approx), sum_LH_m(qoi,approx),
			   N_L_baseline[approx][qoi], N_H_q,
			   N_LH[approx][qoi], beta); // shared HF baseline
	if (outputLevel >= NORMAL_OUTPUT)
	  Cout << "   QoI " << qoi+1 << " Approx " << approx+1
	       << ": control variate beta = " << std::setw(9) << beta << '\n';
	// For MFMC, shared accumulators and counts telescope
	//N_shared = (approx == numApprox-1) ? N_H_q : N_L[approx+1][qoi];
	// Uses telescoping {sum,N}_L_shared from pyramid sampling:
	apply_control(sum_L_sh_m(qoi,approx),  N_L_shared[approx][qoi],
		      sum_L_ref_m(qoi,approx), N_L_refined[approx][qoi],
		      beta, H_raw_mq);
      }
    }
  }
}

} // namespace Dakota
