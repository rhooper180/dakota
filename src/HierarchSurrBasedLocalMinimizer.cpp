/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:       HierarchSurrBasedLocalMinimizer
//- Description: Implementation code for HierarchSurrBasedLocalMinimizer class
//- Owner:       Mike Eldred, Sandia National Laboratories
//- Checked by:

#include "dakota_system_defs.hpp"
#include "dakota_data_io.hpp"
#include "HierarchSurrBasedLocalMinimizer.hpp"
#include "ProblemDescDB.hpp"
#include "ParallelLibrary.hpp"
#include "ParamResponsePair.hpp"
#include "PRPMultiIndex.hpp"
#include "DakotaGraphics.hpp"
#include "DiscrepancyCorrection.hpp"

namespace Dakota {

// initialization of statics
//HierarchSurrBasedLocalMinimizer*
//HierarchSurrBasedLocalMinimizer::mlmfInstance(NULL);


HierarchSurrBasedLocalMinimizer::
HierarchSurrBasedLocalMinimizer(ProblemDescDB& problem_db, Model& model):
  SurrBasedLocalMinimizer(problem_db, model), minimizeIndex(0),
  nestedTrustRegions(true), multiLev(false)//, multiFid(false)
{
  // check iteratedModel for model form hierarchy and/or discretization levels
  if (iteratedModel.surrogate_type() != "hierarchical") {
    Cerr << "Error: HierarchSurrBasedLocalMinimizer requires a hierarchical "
         << "surrogate model specification." << std::endl;
    abort_handler(METHOD_ERROR);
  }

  // Get number of model fidelities and number of levels for each fidelity:
  ModelList& models = iteratedModel.subordinate_models(false);
  numFid = models.size(); numLev.resize(numFid);
  //if (numFid > 1) multiFid = true;
  ModelLIter ml_iter; size_t i;
  for (ml_iter=models.begin(), i=0; i<numFid; ++ml_iter, ++i) {
    numLev[i] = ml_iter->solution_levels();
    if (numLev[i] > 1) multiLev = true;
  }

  // TODO: Only 1D for multifidelity -- need to support ML & MLMF
  size_t num_tr = numFid - 1; // no TR for truth model (valid for global bnds)
  trustRegions.resize(num_tr);
  for (ml_iter=models.begin(), i=0; i<numFid-1; ++i) {
    // size the trust region bounds to allow individual updates
    trustRegions[i].initialize_bounds(numContinuousVars);
    // assign approx and truth for this level
    trustRegions[i].initialize_responses(ml_iter->current_response(),
					 (++ml_iter)->current_response());
    // assign the approx / truth model forms
    trustRegions[i].initialize_indices(i, i+1);
  }

  // Simpler case than DFSBLM:
  short corr_order = (correctionType) ?
    probDescDB.get_short("model.surrogate.correction_order") : -1;
  approxSetRequest = truthSetRequest = 1;
  if (corr_order >= 1 )// || meritFnType      == LAGRANGIAN_MERIT
                       // || approxSubProbObj == LAGRANGIAN_OBJECTIVE )
    truthSetRequest |= 2;
  if (corr_order >= 1 )// || approxSubProbCon == LINEARIZED_CONSTRAINTS )
    approxSetRequest |= 2;
  if (corr_order == 2)
    { truthSetRequest |= 4; approxSetRequest |= 4; }

  // if needed, reshape origTrustRegionFactor and assign defaults
  // Note: user should specify ordered profile consistent with ordered models
  // from LF to HF-1, e.g. initial_size = .125 .25 .5
  size_t num_factors = origTrustRegionFactor.length();
  if (num_factors != num_tr) {
    if (num_factors <= 1) {
      // apply tr_0 factor recursively, from largest at HF-1 to smallest at LF:
      Real tr_0 = (num_factors) ? origTrustRegionFactor[0] : 0.5;
      Real tr_factor = tr_0;
      origTrustRegionFactor.sizeUninitialized(num_tr);
      for (int i=num_tr-1; i>=0; --i) {
	origTrustRegionFactor[i] = tr_factor;
	tr_factor *= tr_0;
      }
    }
    else {
      Cerr << "Error: wrong length for trust region initial_size ("
	   << num_factors << " specified, " << num_tr << " expected)"
	   << std::endl;
      abort_handler(METHOD_ERROR);
    }
  }

  // Instantiate the Model and Minimizer for the approximate sub-problem
  initialize_sub_model();
  initialize_sub_minimizer();
  // Initialize any Lagrange multiplier arrays
  initialize_multipliers();
}


HierarchSurrBasedLocalMinimizer::~HierarchSurrBasedLocalMinimizer()
{ }


void HierarchSurrBasedLocalMinimizer::pre_run()
{
  // TODO; then this step if active sets will be passed below...
  SurrBasedLocalMinimizer::pre_run();

  // static pointer to HierarchSurrBasedLocalMinimizer instance
  //mlmfInstance = this;

  // initialize the trust region factors top-down with HF at origTRFactor
  // and lower fidelities nested and reduced by 2x each level.
  size_t i, num_tr = numFid - 1;
  for (i=0; i<num_tr; ++i) {
    SurrBasedLevelData& sbl_data = trustRegions[i];

    //sbl_data.new_center(true); // vars_center() now sets newCenterFlag
    sbl_data.vars_center(iteratedModel.current_variables());
    //sbl_data.tr_lower_bounds(globalLowerBnds);// rely on update_trust_region()
    //sbl_data.tr_upper_bounds(globalLowerBnds);// rely on update_trust_region()
    sbl_data.trust_region_factor(origTrustRegionFactor[i]);

    sbl_data.active_set_star(1, APPROX_RESPONSE);
    sbl_data.active_set_star(1,  TRUTH_RESPONSE);
    sbl_data.active_set_center(approxSetRequest, APPROX_RESPONSE);
    sbl_data.active_set_center(truthSetRequest,   TRUTH_RESPONSE);
  }
}


void HierarchSurrBasedLocalMinimizer::post_run(std::ostream& s)
{
  // restore original/global bounds
  //approxSubProbModel.continuous_variables(initialPoint);
  //if (recastSubProb) iteratedModel.continuous_variables(initialPoint);
  approxSubProbModel.continuous_lower_bounds(globalLowerBnds);
  approxSubProbModel.continuous_upper_bounds(globalUpperBnds);

  bestVariablesArray.front().active_variables(
    trustRegions[minimizeIndex].vars_center());
  bestResponseArray.front().function_values(
    trustRegions[minimizeIndex].response_center(CORR_TRUTH_RESPONSE).
    function_values());

  SurrBasedLocalMinimizer::post_run(s);
}


/** Step 1 in SurrBasedLocalMinimizer::core_run(). */
void HierarchSurrBasedLocalMinimizer::update_trust_region()
{
  // recur top down to enforce strict bound inter-relationships:
  // > Nested case: all levels are strict subsets of previous
  // > Non-nested case: only the bottom (LF) level is nested; other levels
  //   constrained by global bounds; intermediate levels can evolve
  //   independently based on the accuracy of their individual discrepancies.

  int index, j, k, num_tr_m1 = trustRegions.size() - 1;
  bool new_tr_factor = trustRegions[num_tr_m1].new_factor(),
       parent_update = new_tr_factor;
  // Top level always has parent bnds = global bnds
  if (new_tr_factor)
    update_trust_region_data(trustRegions[num_tr_m1], globalLowerBnds,
			     globalUpperBnds);
  // Loop over all subordinate levels
  for (index=num_tr_m1-1; index>=0; --index) {

    new_tr_factor = trustRegions[index].new_factor();
    if (new_tr_factor)// nested levels at or below this level must update TR bnds
      parent_update = true;

    // if nested at all levels, only need to constraint from one level above:
    if (nestedTrustRegions) {
      if (parent_update) // update if any TR factors at/above level have changed
	update_trust_region_data(trustRegions[index],
				 trustRegions[index+1].tr_lower_bounds(),
				 trustRegions[index+1].tr_upper_bounds());
    }
    // if !nested and !minimizeIndex, then no parent constraints, only global
    else if (index != minimizeIndex) {
      if (new_tr_factor) // update only if this level's TR factor has changed
	update_trust_region_data(trustRegions[index], globalLowerBnds,
				 globalUpperBnds);
    }
    // in this case (!nested and index == minimizeIndex), there was no recursive
    // enforcement for previous levels, but level 0 must recur because it is the
    // level where optimization over LF + \Sum_j delta_j,j+1 is performed
    // --> enforce all trust regions for all individual deltas.
    else if (parent_update) {
      RealVector parent_upper_bnds(numContinuousVars, false),
	         parent_lower_bnds(numContinuousVars, false);
      for (j=0; j<numContinuousVars; ++j) {
        Real min_up_bnd = globalUpperBnds[j], max_lo_bnd = globalLowerBnds[j];
        for (k=index+1; k<num_tr_m1; ++k) {
          Real up_bnd = trustRegions[k].tr_upper_bound(j),
	       lo_bnd = trustRegions[k].tr_lower_bound(j);
          if (up_bnd < min_up_bnd) min_up_bnd = up_bnd;
          if (lo_bnd > max_lo_bnd) max_lo_bnd = lo_bnd;
        }
	if (max_lo_bnd > min_up_bnd) {
	  Cerr << "Error: inconsistent parent bounds in HierarchSurrBasedLocal"
	       << "Minimizer::update_trust_region()." << std::endl;
	  abort_handler(METHOD_ERROR);
	  // TODO Recourse: target levels that are causing an undesirable
	  // truncation and recenter w/ new evals w/i hierarchy
	}
	else {
	  parent_upper_bnds[j] = min_up_bnd;
	  parent_lower_bnds[j] = max_lo_bnd;
	}
      }
      // Compute new trust region and assign to approxSubProbModel
      // (for the lowest level with opt over LF + \Sum_j delta_j,j+1)
      // As enforced above, the bounds for the lowest level are a strict
      // subset of all level bounds --> candidate iterate that gets
      // generated by the sub-problem solver will lie within all higher
      // level TRs and not require any recentering.
      update_trust_region_data(trustRegions[index], parent_lower_bnds,
			       parent_upper_bnds);
    }
  }
}


/** Step 2 in SurrBasedLocalMinimizer::core_run(). */
void HierarchSurrBasedLocalMinimizer::build()
{
  // Recursively build new approximations according to tr[i].new_center():
  // > if new center at current level, build new approximation
  // > if new center at or above current level, update corrected responses.

  // Loop TRs top-down so that correction logic detects new centers at/above
  size_t j, num_tr = trustRegions.size();
  bool update_corr = false;
  for (int i=num_tr-1; i>=0; --i) {

    SurrBasedLevelData& tr_data = trustRegions[i];
    bool new_level_center  = tr_data.new_center();
    Variables& center_vars = tr_data.vars_center();

    if (new_level_center) {

      // all levels at or below this level must update corrected responses
      update_corr = true;

      // Set the trust region center and bounds
      iteratedModel.active_variables(center_vars);
      iteratedModel.continuous_lower_bounds(tr_data.tr_lower_bounds());
      iteratedModel.continuous_upper_bounds(tr_data.tr_upper_bounds());

      // This only evaluates the high fidelity model
      set_model_states(i);
      iteratedModel.build_approximation();

      // Extract truth model evaluation.
      // Note: code from DFSBLM case does lookup, which makes sense if last HF
      // eval was a rejected validation, but if find_center_truth() always
      // follows build_approximation(), then this lookup is not necessary.
      find_center_truth(i); // find/eval *uncorrected* center truth
    }

    if (update_corr) {

      // Recursively correct truth response (all levels above, excepting
      // current level) and store in tr_data
      if (i+1 < num_tr) {
	Cout << "\nRecursively correcting truth model response (form "
	     << tr_data.truth_model_form();
	if (tr_data.truth_model_level() != _NPOS)
	  Cout << ", level " << tr_data.truth_model_level();
	Cout << ") for trust region center.\n";
	Response corrected_resp
	  = tr_data.response_center(UNCORR_TRUTH_RESPONSE).copy();
	for (j=i+1; j<num_tr; ++j)
	  iteratedModel.single_apply(center_vars, corrected_resp,
				     trustRegions[j].indices());
	tr_data.response_center(corrected_resp, CORR_TRUTH_RESPONSE);
      }
      else
	tr_data.response_center(tr_data.response_center(UNCORR_TRUTH_RESPONSE),
				CORR_TRUTH_RESPONSE);
    }
  }

  // TODO: don't stop until hard conv at top level, so this must proliferate
  //       up the TR hierarchy to update and recenter one or more TR
  // TODO: RECURSIVE ASSESSMENT NEEDS TO BE BOTTOM-UP I/O TOP-DOWN !!

  SurrBasedLevelData& tr_min = trustRegions[minimizeIndex];
  size_t min_p1 = minimizeIndex + 1;
  const RealVector& parent_l_bnds = (num_tr > min_p1) ?
    trustRegions[min_p1].tr_lower_bounds() : globalLowerBnds;
  const RealVector& parent_u_bnds = (num_tr > min_p1) ?
    trustRegions[min_p1].tr_upper_bounds() : globalUpperBnds;
  hard_convergence_check(tr_min.response_center(CORR_TRUTH_RESPONSE),
			 tr_min.c_vars_center(), parent_l_bnds,
			 parent_u_bnds);

  if (convergenceFlag) // TODO: finalConvFlag?  convFlag per TR?
    return;

  // Loop TRs top-down so that correction logic detects new centers at/above
  update_corr = false;
  for (int i=num_tr-1; i>=0; --i) {

    SurrBasedLevelData& tr_data = trustRegions[i];
    bool new_level_center  = tr_data.new_center();
    Variables& center_vars = tr_data.vars_center();

    if (new_level_center) {

      // all levels at or below this level must update corrected responses
      update_corr = true;

      // Find approx response.  If not found, evaluate approx model.
      find_center_approx(i); // find/eval *uncorrected* center approx

      // Compute additive/multiplicative correction
      DiscrepancyCorrection& delta = iteratedModel.discrepancy_correction();
      delta.compute(center_vars,
		    tr_data.response_center(UNCORR_TRUTH_RESPONSE),
		    tr_data.response_center(UNCORR_APPROX_RESPONSE));
    }

    if (update_corr) {
      // Recursively correct approx response and store in tr_data
      Cout << "\nRecursively correcting surrogate model response (form "
	   << tr_data.approx_model_form();
      if (tr_data.approx_model_level() != _NPOS)
	Cout << ", level " << tr_data.approx_model_level();
      Cout << ") for trust region center.\n";
      // correct approximation across all levels above i
      Response corrected_resp
	= tr_data.response_center(UNCORR_APPROX_RESPONSE).copy();
      for (j=i; j<num_tr; ++j)
	iteratedModel.single_apply(center_vars, corrected_resp,
				   trustRegions[j].indices());
      tr_data.response_center(corrected_resp, CORR_APPROX_RESPONSE);
    }

    // new center now computed, deactivate flag
    if (new_level_center) tr_data.new_center(false);
  }
}


/** Step 3 in SurrBasedLocalMinimizer::core_run(). */
void HierarchSurrBasedLocalMinimizer::minimize()
{
  // *********************************
  // Optimize at (fully corrected) lowest fidelity only:
  // *********************************

  // Set truth and surrogate models for optimization to be performed on:
  set_model_states(minimizeIndex);

  // set up recursive corrections across all model forms
  ((HierarchSurrModel*)(iteratedModel.model_rep()))->
    correction_mode(FULL_MODEL_FORM_CORRECTION);

  // Set the trust region center and bounds for approxSubProbOptimizer
  SurrBasedLevelData& tr_min = trustRegions[minimizeIndex];
  update_approx_sub_problem(tr_min);

  // solve the approximate optimization sub-problem:
  SurrBasedLocalMinimizer::minimize();

  // Retrieve vars_star and responseStarCorrected[lf_model_form]
  // Corrections are applied recursively during the minimization, so this
  // response is corrected to the highest fidelity level.
  const Variables& v_star = approxSubProbMinimizer.variables_results();
  tr_min.vars_star(v_star);
  if (recastSubProb) {
    // Can't back out eval from recast data and can't assume last iteratedModel
    // eval was the final solution, but can use a DB search for hierarchical
    // (with fallback to new eval if not found).
    find_star_approx(minimizeIndex); // -> uncorrected resp_star_approx
    // apply correction and store
    Response corr_resp(tr_min.response_star(UNCORR_APPROX_RESPONSE).copy());
    iteratedModel.recursive_apply(v_star, corr_resp);
    tr_min.response_star(corr_resp, CORR_APPROX_RESPONSE);
  }
  else // retrieve corrected final results
    tr_min.response_star(approxSubProbMinimizer.response_results(),
			 CORR_APPROX_RESPONSE); // Note: fn values only
}


/** Step 4 in SurrBasedLocalMinimizer::core_run(). */
void HierarchSurrBasedLocalMinimizer::verify()
{
  // ****************************
  // Validate candidate point
  // ****************************

  Cout << "\n>>>>> Evaluating approximate solution with actual model.\n";

  set_model_states(minimizeIndex);

  Model& truth_model  = iteratedModel.truth_model();
  Model& approx_model = iteratedModel.surrogate_model();
  SurrBasedLevelData& tr_data = trustRegions[minimizeIndex];

  // Candidate iterate:
  Variables& vars_star = tr_data.vars_star();

  iteratedModel.component_parallel_mode(TRUTH_MODEL);
  truth_model.active_variables(vars_star);
  truth_model.evaluate(tr_data.active_set_star(TRUTH_RESPONSE));
  const Response& truth_resp = truth_model.current_response();

  // Does not appear to be required:
  //tr_data.response_star(truth_resp, UNCORR_TRUTH_RESPONSE);

  // Apply correction recursively so that this truth response is consistent
  // with the highest fidelity level.
  size_t j, num_tr = trustRegions.size();
  if (num_tr > 1) {
    Response resp_star_truth_tmp = truth_resp.copy();
    for (j=1; j<num_tr; ++j)
      iteratedModel.single_apply(vars_star, resp_star_truth_tmp,
				 trustRegions[j].indices());
    tr_data.response_star(resp_star_truth_tmp, CORR_TRUTH_RESPONSE);
  }
  else
    tr_data.response_star(truth_resp, CORR_TRUTH_RESPONSE);

  // TODO: TR updates needs to recur

  compute_trust_region_ratio(tr_data); // no check_interior

  // If the candidate optimum (vars_star) is accepted, then update the
  // center variables and response data.
  if (tr_data.new_center()) {
    tr_data.vars_center(vars_star);

    // Does not appear to be required:
    //tr_data.response_center(tr_data.response_star(UNCORR_TRUTH_RESPONSE),
    //			      UNCORR_TRUTH_RESPONSE);

    /* TODO: re-eval for new corr is handled later? */
    tr_data.response_center(tr_data.response_star(CORR_TRUTH_RESPONSE),
			    CORR_TRUTH_RESPONSE);
  }

  // Check for soft convergence:
  if (softConvCount >= softConvLimit)
    convergenceFlag = 3; // soft convergence
  // terminate SBLM if trustRegionFactor is less than its minimum value
  else if (tr_data.trust_region_factor() < minTrustRegionFactor)
    convergenceFlag = 1;
  // terminate SBLM if the maximum number of iterations has been reached
  else if (sbIterNum >= maxIterations)
    convergenceFlag = 2;
}

// Note: find() implies a DB lookup and DB entries are uncorrected, so employ
// this convention consistently and correct after find() when needed.

void HierarchSurrBasedLocalMinimizer::find_center_truth(size_t tr_index)
{
  SurrBasedLevelData& tr_data = trustRegions[tr_index];
  Model& truth_model = iteratedModel.truth_model();

  // TODO: hard-coded for now, see note in build()
  tr_data.response_center(truth_model.current_response(),UNCORR_TRUTH_RESPONSE);
  bool truth_found = true;

  if (!truth_found) {
    Cout << "\n>>>>> Evaluating actual model at trust region center.\n";

    // since we're bypassing iteratedModel, iteratedModel.serve()
    // must be in the correct server mode.
    iteratedModel.component_parallel_mode(TRUTH_MODEL);
    truth_model.active_variables(tr_data.vars_center());
    truth_model.evaluate(tr_data.active_set_center(TRUTH_RESPONSE));

    tr_data.response_center(truth_model.current_response(),
                            UNCORR_TRUTH_RESPONSE);
  }
}


void HierarchSurrBasedLocalMinimizer::find_center_approx(size_t tr_index)
{
  SurrBasedLevelData& tr_data = trustRegions[tr_index];
  const Variables&   v_center = tr_data.vars_center();

  bool approx_found
    = find_approx_response(v_center,
			   tr_data.response_center(UNCORR_APPROX_RESPONSE));

  if (!approx_found) {
    Cout <<"\n>>>>> Evaluating approximation at trust region center.\n";
    iteratedModel.component_parallel_mode(SURROGATE_MODEL);
    iteratedModel.surrogate_response_mode(UNCORRECTED_SURROGATE);
    iteratedModel.active_variables(v_center);
    iteratedModel.evaluate(tr_data.active_set_center(APPROX_RESPONSE));
    tr_data.response_center(iteratedModel.current_response(),
			    UNCORR_APPROX_RESPONSE);
  }
}


void HierarchSurrBasedLocalMinimizer::find_star_approx(size_t tr_index)
{
  SurrBasedLevelData& tr_data = trustRegions[tr_index];
  const Variables&     v_star = tr_data.vars_star();

  bool approx_found
    = find_approx_response(v_star,
			   tr_data.response_star(UNCORR_APPROX_RESPONSE));

  if (!approx_found) {
    Cout <<"\n>>>>> Evaluating approximation at candidate optimum.\n";
    iteratedModel.component_parallel_mode(SURROGATE_MODEL);
    iteratedModel.surrogate_response_mode(UNCORRECTED_SURROGATE);
    iteratedModel.active_variables(v_star);
    iteratedModel.evaluate(); // fn values only
    tr_data.response_star(iteratedModel.current_response(),
			  UNCORR_APPROX_RESPONSE);
  }
}


/*
void HierarchSurrBasedLocalMinimizer::
compute_trust_region_ratio(SurrBasedLevelData& tr_data)
{
  // TODO: we need both data sets consistently corrected, but for > 2 levels,
  // do we want both sets corrected all the way to HF, or the approx corrected
  // 1 level only for consistency with uncorrected truth?

  const RealVector& fns_center_truth
    = tr_data.response_center(UNCORR_TRUTH_RESPONSE).function_values();
  const RealVector& fns_star_truth
    = tr_data.response_star(UNCORR_TRUTH_RESPONSE).function_values();
  const RealVector& fns_center_approx
    = tr_data.response_center(CORR_APPROX_RESPONSE).function_values();
  const RealVector& fns_star_approx
    = tr_data.response_star(CORR_APPROX_RESPONSE).function_values();

  // no constraints...
  merit_fn_center_truth = objective(fns_center_truth, sense, wts);
  merit_fn_star_truth = objective(fns_star_truth, sense, wts);
  merit_fn_center_approx = objective(fns_center_approx, sense, wts);
  merit_fn_star_approx = objective(fns_star_approx, sense, wts);

   // no FILTER...
  bool accept_step = (numerator > 0.);

  // Same from here down, except no FILTER, globalApprox options ...
}
*/


// top level: not bound to core_run() yet...
void HierarchSurrBasedLocalMinimizer::
MG_Opt_driver(const Variables &x0)
{
  RealVector vars_star = x0.continuous_variables();

  int max_iter = 10, iter = 0;
  while (!convergenceFlag && iter < max_iter) {
    // Perform one complete V cycle:
    // recursively applied MG/Opt to all levels w/ line search
    // (no prolongation/restriction at this pt)
    //
    // vars_star returned has already been validated at the HF level
    // (from final line search or final opt post-smoothing)
    vars_star = MG_Opt(vars_star, numLev[0] - 1);

    // Only need is to check for hard + soft convergence at top level (no TR
    // updates).  If not converged, then continue V cycles until max iter.
    // On exit of V cycle, we have performed a line search at HF level, but 
    // no gradient evaluation assuming value-based line search.

    // TODO: add code here

    ++iter;
  }

  // TODO: warm starting of Hessian approximations were critical to render this
  // algorithm competitive in Jason's MATLAB prototype
  // > 1st-order MG/Opt was ~2x better than 1st-order single-level opt.
  // > Quasi-2nd-order MG/Opt required warm starts to achieve ~2x again,
  //   relative to 2nd-order single-level opt.  W/o warm starts, 2nd-order
  //   MG/Opt was similar to 1st-order MG/Opt since secant updates did not 
  //   have sufficient iters to accumulate.

  // Note: if remove pre- and post-optimization and replace line search with
  // TR logic, then basically recover TRMM.
  // Jason's feeling is that TR usage is the most fruitful direction for MG/Opt:
  // > pre-optimization w/o max_iters limited by TR
  // > instead of line search, compute TR ratios, accept/reject, adapt
  // An important question is whether pre-optimization adds anything relative
  // to TRMM without it.
}


RealVector HierarchSurrBasedLocalMinimizer::
MG_Opt(const RealVector& x0_k, int k)
// TODO: pass full Vars object i/o continuous only
{
  // TODO: flatten and iterate across V cycle to replace fn recursion at ***

  // V cycle:
  // > comes in with k = n-1 and recurs top down with partial opt (max_iter = 3)
  //   until k = 0 --> full/more extensive opt (max_iter = 30)
  // > then begins to unroll nested call stack, bottom up, from k=0 to k=n-1

  if (k == 0) { // Full optimization on corrected lowest level
    // Some authors: run until convergence; others: tune as well.
    // Care is needed due to embedded use of local corrections.
    // **********************************************************************
    // Either need to add trust region control or continue poor-man's control
    // with max iterations to avoid leaving region of correction accuracy.
    // **********************************************************************
    int max_iter = 30; // until convergence...  (tune or expose?)
    return optimize(x0_k, max_iter, k); // Steps 3,4
  }
  else { // Partial optimization

    // Step 6: pre-optimization
    int max_iter = 3;
    RealVector x1_k = optimize(x0_k, max_iter, k); // pre-opt / step 1, level k

    // Step 7: Restriction: x1_km1 = R[x1_k]

    // Step 8: Build discrepancy correction between levels k and k-1:
    // TODO: add code here to compute discrepancy corrections
    //   reuse same trustRegions arrays (but support multidimensional MLMF case)

    // Step 9: Recursively call MG_Opt
    RealVector x2_km1 = MG_Opt(x1_k, k-1); // use x1_km1 if restriction

    // Step 10a: prolongation x2_k = P[x2_km1]

    // Step 10b: compute search direction from difference in iterates
    RealVector p_k = x2_km1; // use x2_k if prolongation
    p_k -= x1_k;

    // Step 11: perform line search:
    Real alpha0 = 1.;
    RealVector x2_k = linesearch(x1_k, p_k, alpha0);

    // Optional additional step 12: "post smoothing" (partial opt)
    // This step not represented in Jason's graphic; but does appear in
    // some papers (Lewis & Nash?, Borzi?)
    //return optimize(x2_k, max_iter, k);
    // ...OR...
    return x2_k;
  }
}


// alpha0 is initial step size;
// pk is search direction defined from change in x;
// xk comes in as x^{k-1}, return as x^k
RealVector HierarchSurrBasedLocalMinimizer::
linesearch(const RealVector &xk, const RealVector &pk, double alpha0)
{
  // TODO: add a real line search algorithm here...

  // For now, always accept full step defined from predicted change in x:
  RealVector new_xk = xk;
  // Perform alpha step.  Note: s = -grad f (take care with sign of pk)
  size_t i, len = xk.length();
  for (i=0; i<len; ++i)
    new_xk[i] += alpha0 * pk[i];
  return new_xk;
}


RealVector HierarchSurrBasedLocalMinimizer::
optimize(const RealVector &x, int max_iter, int index)
{
  // Update starting point for optimization:
  approxSubProbModel.continuous_variables(x);

  // Set the max iterations for this level:
  approxSubProbMinimizer.maximum_iterations(max_iter);

  // Set truth and surrogate models for optimization to be performed on:
  set_model_states(index);

  // set up recursive corrections across all solution levels
  ((HierarchSurrModel*)(iteratedModel.model_rep()))->
    correction_mode(FULL_SOLUTION_LEVEL_CORRECTION);

  iteratedModel.surrogate_response_mode(AUTO_CORRECTED_SURROGATE);
  ParLevLIter pl_iter = methodPCIter->mi_parallel_level_iterator(miPLIndex);
  approxSubProbMinimizer.run(pl_iter); // pl_iter required for hierarchical

  // Return candidate point:
  return approxSubProbMinimizer.variables_results().continuous_variables();
}

} // namespace Dakota
