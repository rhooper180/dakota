/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014-2020
    National Technology & Engineering Solutions of Sandia, LLC (NTESS).
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:	 NonDMultilevelSampling
//- Description: Implementation code for NonDMultilevelSampling class
//- Owner:       Mike Eldred
//- Checked by:
//- Version:

#include "dakota_system_defs.hpp"
#include "dakota_data_io.hpp"
#include "dakota_tabular_io.hpp"
#include "DakotaModel.hpp"
#include "DakotaResponse.hpp"
#include "NonDMultilevelSampling.hpp"
#include "ProblemDescDB.hpp"
#include "ActiveKey.hpp"
#include "DakotaIterator.hpp"

static const char rcsId[]="@(#) $Id: NonDMultilevelSampling.cpp 7035 2010-10-22 21:45:39Z mseldre $";


namespace Dakota {

/** This constructor is called for a standard letter-envelope iterator 
    instantiation.  In this case, set_db_list_nodes has been called and 
    probDescDB can be queried for settings from the method specification. */
NonDMultilevelSampling::
NonDMultilevelSampling(ProblemDescDB& problem_db, Model& model):
  NonDHierarchSampling(problem_db, model),
  allocationTarget(problem_db.get_short("method.nond.allocation_target")),
  useTargetVarianceOptimizationFlag(problem_db.get_bool("method.nond.allocation_target.optimization")),
  qoiAggregation(problem_db.get_short("method.nond.qoi_aggregation")),
  convergenceTolType(problem_db.get_short("method.nond.convergence_tolerance_type")),
  convergenceTolTarget(problem_db.get_short("method.nond.convergence_tolerance_target"))
{
  ModelList& ordered_models = iteratedModel.subordinate_models(false);
  size_t i, j, num_mf = ordered_models.size(), num_lev,
    prev_lev = std::numeric_limits<size_t>::max(),
    pilot_size = pilotSamples.size();
  ModelLRevIter ml_rit; bool err_flag = false;
  NLev.resize(num_mf);
  for (i=num_mf-1, ml_rit=ordered_models.rbegin();
       ml_rit!=ordered_models.rend(); --i, ++ml_rit) { // high fid to low fid
    // for now, only SimulationModel supports solution_{levels,costs}()
    num_lev = ml_rit->solution_levels(); // lower bound is 1 soln level

    if (num_lev > prev_lev) {
      Cerr << "\nWarning: unused solution levels in multilevel sampling for "
	   << "model " << ml_rit->model_id() << ".\n         Ignoring "
	   << num_lev - prev_lev << " of " << num_lev << " levels."<< std::endl;
      num_lev = prev_lev;
    }

    // Ensure there is consistent cost data available as SimulationModel must
    // be allowed to have empty solnCntlCostMap (when optional solution control
    // is not specified).  Passing false bypasses lower bound of 1.
    if (num_lev > ml_rit->solution_levels(false)) { // default is 0 soln costs
      Cerr << "Error: insufficient cost data provided for multilevel sampling."
	   << "\n       Please provide solution_level_cost estimates for model "
	   << ml_rit->model_id() << '.' << std::endl;
      err_flag = true;
    }

    //Sizet2DArray& Nl_i = NLev[i];
    NLev[i].resize(num_lev); //Nl_i.resize(num_lev);
    //for (j=0; j<num_lev; ++j)
    //  Nl_i[j].resize(numFunctions); // defer to pre_run()

    prev_lev = num_lev;
  }
  if (err_flag)
    abort_handler(METHOD_ERROR);

  if( !std::all_of( std::begin(pilotSamples), std::end(pilotSamples), [](int i){ return i > 0; }) ){
    Cerr << "\nError: Some levels have pilot samples of size 0 in "
       << method_enum_to_string(methodName) << "." << std::endl;
    abort_handler(METHOD_ERROR);
  }

  switch (pilot_size) {
    case 0: maxEvalConcurrency *= 100;             break;
    case 1: maxEvalConcurrency *= pilotSamples[0]; break;
    default: {
      size_t max_ps = 0;
      for (i=0; i<pilot_size; ++i)
        if (pilotSamples[i] > max_ps)
  	max_ps = pilotSamples[i];
      if (max_ps)
        maxEvalConcurrency *= max_ps;
      break;
    }
  }

  // For testing multilevel_mc_Qsum():
  //subIteratorFlag = true;

  if(allocationTarget == TARGET_SCALARIZATION){
    if(qoiAggregation == QOI_AGGREGATION_SUM){
      Cerr << "\n Error: Scalarization not available with setting qoi_aggregation=sum. "
     << "Use qoi_aggregation=max instead." << std::endl;
      abort_handler(METHOD_ERROR);
    }
    // Retrieve the variable mapping inputs
    const RealVector& scalarization_response_vector
      = probDescDB.get_rv("method.nond.scalarization_response_mapping");
    if (scalarization_response_vector.empty() || scalarization_response_vector.length() != numFunctions*(2*numFunctions) ) {
      Cerr << "\n Warning: no or incomplete mappings provided for scalarization mapping in "
     << "multilevel sampling initialization. Checking for nested model." << std::endl;
    }else{
      scalarizationCoeffs.reshape(numFunctions, 2*numFunctions);
      size_t vec_ctr = 0;
      for(size_t i = 0; i < numFunctions; ++i){
        for(size_t j = 0; j < numFunctions; ++j){
          scalarizationCoeffs(i, 2*j) = scalarization_response_vector[vec_ctr++];
          scalarizationCoeffs(i, 2*j+1) = scalarization_response_vector[vec_ctr++];
        }
      }
    }
  }
 
}


NonDMultilevelSampling::~NonDMultilevelSampling()
{ }


/*
void NonDMultilevelSampling::pre_run()
{
  NonDSampling::pre_run();

  // reset sample counters to 0
  size_t i, j, num_mf = NLev.size(), num_lev;
  for (i=0; i<num_mf; ++i) {
    Sizet2DArray& Nl_i = NLev[i];
    num_lev = Nl_i.size();
    for (j=0; j<num_lev; ++j)
      Nl_i[j].assign(numFunctions, 0);
  }
}
*/


/** The primary run function manages the general case: a hierarchy of model 
    forms (from the ordered model fidelities within a HierarchSurrModel), 
    each of which may contain multiple discretization levels. */
void NonDMultilevelSampling::core_run()
{
  //model,
  //  surrogate hierarchical
  //    ordered_model_fidelities = 'LF' 'MF' 'HF'
  //
  // Future: include peer alternatives (1D list --> matrix)
  //         For MLMC, could seek adaptive selection of most correlated
  //         alternative (or a convex combination of alternatives).

  // TO DO: hierarchy incl peers (not peers each optionally incl hierarchy)
  //   num_mf     = iteratedModel.model_hierarchy_depth();
  //   num_peer_i = iteratedModel.model_peer_breadth(i);

  // TO DO: this initial logic is limiting:
  // > allow MLMC and CVMC for either model forms or discretization levels
  // > separate method specs that both map to NonDMultilevelSampling ???

  // TO DO: following pilot sample across levels and fidelities in mixed case,
  // could pair models for CVMC based on estimation of rho2_LH.

  // For two-model control variate methods, select lowest,highest fidelities
  size_t num_mf = NLev.size();
  unsigned short lf_form = 0, hf_form = num_mf - 1; // ordered_models = low:high
  if (num_mf > 1) {
    size_t num_hf_lev = NLev.back().size();

  }
  else { // multiple solutions levels (only) --> traditional ML-MC
    if (true){ //(subIteratorFlag)
      multilevel_mc_Qsum(hf_form); // w/ error est, unbiased central moments
    }
    else
      multilevel_mc_Ysum(hf_form); // lighter weight
  }
}


/** This function performs "geometrical" MLMC on a single model form
    with multiple discretization levels. */
void NonDMultilevelSampling::multilevel_mc_Ysum(unsigned short form)
{
  // Formulate as a coordinated progression towards convergence, where, e.g.,
  // time step is inferred from the spatial discretization (NOT an additional
  // solution control) based on stability criteria, e.g. CFL condition.  Can
  // we reliably capture runtime estimates as part of pilot run w/i Dakota?
  // Ultimately seems desirable to support either online or offline cost
  // estimates, to allow more accurate resource allocation when possible
  // or necessary (e.g., combustion processes with expense that is highly
  // parameter dependent).
  //   model
  //     id_model = 'LF'
  //     simulation
  //       # point to state vars; ordered based on set values for h, delta-t
  //       solution_level_control = 'dssiv1'
  //       # relative cost estimates in same order as state set values
  //       # --> re-sort into map keyed by increasing cost
  //       solution_level_cost = 10 2 200

  // How to manage the set of MLMC statistics:
  // 1. Simplest: proposal is to use the mean estimator to drive the algorithm,
  //    but carry along other estimates.
  // 2. Later: could consider a refinement for converging the estimator of the
  //    variance after convergence of the mean estimator.

  // How to manage the vector of QoI:
  // 1. Worst case: select N_l based only on QoI w/ highest total variance
  //      from pilot run --> fix for all levels and don't allow switching
  //      across major iterations (possible oscillation?  Or simple overlay
  //      of resolution reqmts?)
  // 2. Better: select N_l based on convergence in aggregated variance.

  // assign truth model form (solution level assignment is deferred until loop)
  Pecos::ActiveKey truth_key;
  short seq_type = Pecos::RESOLUTION_LEVEL_SEQUENCE;
  size_t lev = std::numeric_limits<size_t>::max(); // updated in loop below
  truth_key.form_key(0, form, lev);
  iteratedModel.active_model_key(truth_key);
  Model& truth_model = iteratedModel.truth_model();

  size_t qoi, num_steps = truth_model.solution_levels();// 1 model form
  size_t& step = lev;//(true) ? lev : form; // model form option not active
  size_t max_iter = (maxIterations < 0) ? 25 : maxIterations; // default = -1
  Real eps_sq_div_2, sum_sqrt_var_cost, estimator_var0 = 0., lev_cost;
  // retrieve cost estimates across soln levels for a particular model form
  RealVector cost = truth_model.solution_level_costs(), agg_var(num_steps);
  // For moment estimation, we accumulate telescoping sums for Q^i using
  // discrepancies Yi = Q^i_{lev} - Q^i_{lev-1} (sum_Y[i] for i=1:4).
  // For computing N_l from estimator variance, we accumulate square of Y1
  // estimator (YY[i] = (Y^i)^2 for i=1).
  IntRealMatrixMap sum_Y; RealMatrix sum_YY(numFunctions, num_steps);
  initialize_ml_Ysums(sum_Y, num_steps);

  // Initialize for pilot sample
  SizetArray delta_N_l;
  load_pilot_sample(pilotSamples, NLev, delta_N_l);

  // raw eval counts are accumulation of allSamples irrespective of resp faults
  SizetArray raw_N_l(num_steps, 0);
  RealVectorArray mu_hat(num_steps);
  Sizet2DArray& N_l = NLev[form];

  // now converge on sample counts per level (N_l)
  mlmfIter = 0;
  while (Pecos::l1_norm(delta_N_l) && mlmfIter <= max_iter) {

    sum_sqrt_var_cost = 0.;
    for (step=0; step<num_steps; ++step) { // step is reference to lev

      configure_indices(step, form, lev, seq_type);
      lev_cost = level_cost(cost, step);

      // set the number of current samples from the defined increment
      numSamples = delta_N_l[step];

      // aggregate variances across QoI for estimating N_l (justification:
      // for independent QoI, sum of QoI variances = variance of QoI sum)
      Real& agg_var_l = agg_var[step]; // carried over from prev iter if no samp
      if (numSamples) {

	// advance any sequence specifications (seed_sequence)
	assign_specification_sequence(step);
	// generate new MC parameter sets
	get_parameter_sets(iteratedModel);// pull dist params from any model

	// export separate output files for each data set.  truth_model()
	// has the correct data when in bypass-surrogate mode.
	if (exportSampleSets)
	  export_all_samples("ml_", truth_model, mlmfIter, step);

	// compute allResponses from allVariables using hierarchical model
	evaluate_parameter_sets(iteratedModel, true, false);

	// process allResponses: accumulate new samples for each qoi and
	// update number of successful samples for each QoI
	//if (mlmfIter == 0) accumulate_offsets(mu_hat[lev]);
	accumulate_ml_Ysums(sum_Y, sum_YY, lev, mu_hat[step], N_l[step]);
	if (outputLevel == DEBUG_OUTPUT)
	  Cout << "Accumulated sums (Y1, Y2, Y3, Y4, Y1sq):\n" << sum_Y[1]
	       << sum_Y[2] << sum_Y[3] << sum_Y[4] << sum_YY << std::endl;
	// update raw evaluation counts
	raw_N_l[step] += numSamples;

	// compute estimator variance from current sample accumulation:
	if (outputLevel >= DEBUG_OUTPUT)
	  Cout << "variance of Y[" << step << "]: ";
	agg_var_l
	  = aggregate_variance_Ysum(sum_Y[1][step], sum_YY[step], N_l[step]);
      }

      sum_sqrt_var_cost += std::sqrt(agg_var_l * lev_cost);
      // MSE reference is MLMC with pilot sample, prior to any N_l adaptation:
      if (mlmfIter == 0)
	estimator_var0
	  += aggregate_mse_Ysum(sum_Y[1][step], sum_YY[step], N_l[step]);
    }
    // compute epsilon target based on relative tolerance: total MSE = eps^2
    // which is equally apportioned (eps^2 / 2) among discretization MSE and
    // estimator variance (\Sum var_Y_l / N_l).  Since we do not know the
    // discretization error, we compute an initial estimator variance from MLMC
    // on the pilot sample and then seek to reduce it by a relative_factor <= 1.
    if (mlmfIter == 0) { // eps^2 / 2 = var * relative factor
      eps_sq_div_2 = estimator_var0 * convergenceTol;
      if (outputLevel == DEBUG_OUTPUT)
	Cout << "Epsilon squared target = " << eps_sq_div_2 << std::endl;
    }

    // update targets based on variance estimates
    Real fact = sum_sqrt_var_cost / eps_sq_div_2, N_target;
    for (step=0; step<num_steps; ++step) {
      // Equation 3.9 in CTR Annual Research Briefs:
      // "A multifidelity control variate approach for the multilevel Monte 
      // Carlo technique," Geraci, Eldred, Iaccarino, 2015.
      N_target = std::sqrt(agg_var[step] / level_cost(cost, step)) * fact;
      delta_N_l[step] = one_sided_delta(average(N_l[step]), N_target);
    }
    ++mlmfIter;
    Cout << "\nMLMC iteration " << mlmfIter << " sample increments:\n"
	 << delta_N_l << std::endl;
  }

  // aggregate expected value of estimators for Y, Y^2, Y^3, Y^4. Final expected
  // value is sum of expected values from telescopic sum.  Note: raw moments
  // have no bias correction (no additional variance from an estimated center).
  RealMatrix Q_raw_mom(numFunctions, 4);
  RealMatrix &sum_Y1 = sum_Y[1], &sum_Y2 = sum_Y[2],
	     &sum_Y3 = sum_Y[3], &sum_Y4 = sum_Y[4];
  for (qoi=0; qoi<numFunctions; ++qoi) {
    for (step=0; step<num_steps; ++step) {
      size_t Nlq = N_l[lev][qoi];
      Q_raw_mom(qoi,0) += sum_Y1(qoi,step) / Nlq;
      Q_raw_mom(qoi,1) += sum_Y2(qoi,step) / Nlq;
      Q_raw_mom(qoi,2) += sum_Y3(qoi,step) / Nlq;
      Q_raw_mom(qoi,3) += sum_Y4(qoi,step) / Nlq;
    }
  }
  // Convert uncentered raw moment estimates to final moments (central or std)
  convert_moments(Q_raw_mom, momentStats);

  // compute the equivalent number of HF evaluations (includes any sim faults)
  equivHFEvals = raw_N_l[0] * cost[0]; // first level is single eval
  for (step=1; step<num_steps; ++step) // subsequent levels incur 2 model costs
    equivHFEvals += raw_N_l[step] * (cost[step] + cost[step-1]);
  equivHFEvals /= cost[num_steps-1]; // normalize into equivalent HF evals
}

/** This function performs "geometrical" MLMC on a single model form
    with multiple discretization levels. */
void NonDMultilevelSampling::multilevel_mc_Qsum(unsigned short form)
{
    if(allocationTarget == TARGET_SCALARIZATION){
      if (scalarizationCoeffs.empty()) {
        Cerr << "\n Warning: no or incomplete mappings provided for scalarization mapping in "
       << "multilevel sampling initialization. Has to be specified via scalarization_response_mapping or nested model." << std::endl;
        abort_handler(METHOD_ERROR);
      }
    }
    // assign truth model form (solution level assignment is deferred until loop)
    Pecos::ActiveKey truth_key;
    short seq_type = Pecos::RESOLUTION_LEVEL_SEQUENCE;
    size_t lev = std::numeric_limits<size_t>::max(); // updated in loop below
    truth_key.form_key(0, form, lev);
    iteratedModel.active_model_key(truth_key);
    Model& truth_model = iteratedModel.truth_model();

    size_t qoi, num_steps = truth_model.solution_levels();//1 model form
    size_t& step = lev;//(true) ? lev : form; // model form option not active

    size_t max_iter = (maxIterations < 0) ? 25 : maxIterations; // default = -1
    Real eps_sq_div_2, sum_sqrt_var_cost, estimator_var0 = 0., lev_cost, place_holder;

    // retrieve cost estimates across soln levels for a particular model form
    RealVector cost = truth_model.solution_level_costs(), agg_var(num_steps), agg_var_of_var(num_steps),
       estimator_var0_qoi(numFunctions), eps_sq_div_2_qoi(numFunctions),
      sum_sqrt_var_cost_qoi(numFunctions), sum_sqrt_var_cost_var_qoi(numFunctions), sum_sqrt_var_cost_mean_qoi(numFunctions),
       agg_var_l_qoi(numFunctions), level_cost_vec(num_steps);
    RealMatrix agg_var_qoi(numFunctions, num_steps), agg_var_mean_qoi(numFunctions, num_steps),
       agg_var_var_qoi(numFunctions, num_steps);

    // For moment estimation, we accumulate telescoping sums for Q^i using
    // discrepancies Yi = Q^i_{lev} - Q^i_{lev-1} (Y_diff_Qpow[i] for i=1:4).
    // For computing N_l from estimator variance, we accumulate square of Y1
    // estimator (YY[i] = (Y^i)^2 for i=1).
    IntRealMatrixMap sum_Ql, sum_Qlm1;
    IntIntPairRealMatrixMap sum_QlQlm1;
    initialize_ml_Qsums(sum_Ql, sum_Qlm1, sum_QlQlm1, num_steps);

    // Initialize for pilot sample
    SizetArray delta_N_l;
    load_pilot_sample(pilotSamples, NLev, delta_N_l);

    // raw eval counts are accumulation of allSamples irrespective of resp faults
    SizetArray raw_N_l(num_steps, 0);
    RealVectorArray mu_hat(num_steps);
    Sizet2DArray &N_l = NLev[form];

    // Useful for future extensions when convergence tolerance can be a vector
    convergenceTolVec.resize(numFunctions);
    for(size_t qoi = 0; qoi < numFunctions; ++qoi){
      convergenceTolVec[qoi] = convergenceTol;
    }

    // now converge on sample counts per level (N_l)
    mlmfIter = 0;

    while (Pecos::l1_norm(delta_N_l) && mlmfIter <= max_iter) {
      for (step=0; step<num_steps; ++step) {

        configure_indices(step, form, lev, seq_type);
        lev_cost = level_cost(cost, step);
        level_cost_vec[step] = lev_cost;

        // set the number of current samples from the defined increment
        numSamples = delta_N_l[step];

        // aggregate variances across QoI for estimating N_l (justification:
        // for independent QoI, sum of QoI variances = variance of QoI sum)
        //Real &agg_var_l = agg_var[step]; // carried over from prev iter if no samp
        if (numSamples) {
          evaluate_sample_increment(step);

          accumulate_sums(sum_Ql, sum_Qlm1, sum_QlQlm1, step, mu_hat, N_l);

          // update raw evaluation counts
          raw_N_l[step] += numSamples;

          aggregate_variance_target_Qsum(sum_Ql, sum_Qlm1, sum_QlQlm1, N_l, step, agg_var_qoi);
          // MSE reference is MC applied to HF
          if (mlmfIter == 0) {
            aggregate_mse_target_Qsum(agg_var_qoi, N_l, step, estimator_var0_qoi);
          }
        }
      }
      if (mlmfIter == 0) { // eps^2 / 2 = var * relative factor
        set_convergence_tol(estimator_var0_qoi, cost, convergenceTolVec, eps_sq_div_2_qoi);
      }

      // update targets based on variance estimates
      //if(target_mean){
      if (outputLevel == DEBUG_OUTPUT) Cout << "N_target: " << std::endl;

      compute_sample_allocation_target(sum_Ql, sum_Qlm1, sum_QlQlm1, eps_sq_div_2_qoi, agg_var_qoi, cost, N_l, delta_N_l);

      ++mlmfIter;
      Cout << "\nMLMC iteration " << mlmfIter << " sample increments:\n"
	   << delta_N_l << std::endl;
    }
    ////
    compute_moments(sum_Ql, sum_Qlm1, sum_QlQlm1, N_l);

    // populate finalStatErrors
    compute_error_estimates(sum_Ql, sum_Qlm1, sum_QlQlm1, N_l);

    // compute the equivalent number of HF evaluations (includes any sim faults)
    compute_equiv_HF_evals(raw_N_l, cost);
}


void NonDMultilevelSampling::evaluate_sample_increment(const unsigned short& step)
{
  // advance any sequence specifications (seed_sequence)
  assign_specification_sequence(step);
  // generate new MC parameter sets
  get_parameter_sets(iteratedModel);// pull dist params from any model

  // export separate output files for each data set.  truth_model()
  // has the correct data when in bypass-surrogate mode.
  if (exportSampleSets)
  export_all_samples("ml_", iteratedModel.truth_model(),
       mlmfIter, step);

  // compute allResponses from allVariables using hierarchical model
  evaluate_parameter_sets(iteratedModel, true, false);
}


void NonDMultilevelSampling::
accumulate_sums(IntRealMatrixMap& sum_Ql, IntRealMatrixMap& sum_Qlm1,
		IntIntPairRealMatrixMap& sum_QlQlm1, const size_t step,
		const RealVectorArray& offset, Sizet2DArray& N_l)
{
  // process allResponses: accumulate new samples for each qoi and
  // update number of successful samples for each QoI
  //if (mlmfIter == 0) accumulate_offsets(mu_hat[step]);
  accumulate_ml_Qsums(sum_Ql, sum_Qlm1, sum_QlQlm1, step,
                      offset[step], N_l[step]);

  if (outputLevel == DEBUG_OUTPUT)
    Cout << "Accumulated sums (Ql[1,2], Qlm1[1,2]):\n" << sum_Ql[1]
         << sum_Ql[2] << sum_Qlm1[1] << sum_Qlm1[2] << std::endl;
}


void NonDMultilevelSampling::
initialize_ml_Ysums(IntRealMatrixMap& sum_Y, size_t num_lev)
{
  // sum_* are running sums across all increments
  std::pair<int, RealMatrix> empty_pr;
  for (int i=1; i<=4; ++i) {
    empty_pr.first = i;
    // std::map::insert() returns std::pair<IntRMMIter, bool>:
    // use iterator to shape RealMatrix in place and init sums to 0
    sum_Y.insert(empty_pr).first->second.shape(numFunctions, num_lev);
  }
}

  
void NonDMultilevelSampling::
initialize_ml_Qsums(IntRealMatrixMap& sum_Ql, IntRealMatrixMap& sum_Qlm1,
		    IntIntPairRealMatrixMap& sum_QlQlm1, size_t num_lev)
{
  // sum_* are running sums across all increments
  std::pair<int, RealMatrix> empty_irm_pr; int i, j;
  for (i=1; i<=4; ++i) {
    empty_irm_pr.first = i;
    // std::map::insert() returns std::pair<IntRMMIter, bool>:
    // use iterator to shape RealMatrix in place and init sums to 0
    sum_Ql.insert(empty_irm_pr).first->second.shape(numFunctions, num_lev);
    sum_Qlm1.insert(empty_irm_pr).first->second.shape(numFunctions, num_lev);
  }
  std::pair<IntIntPair, RealMatrix> empty_iirm_pr;
  IntIntPair& ii = empty_iirm_pr.first;
  for (i=1; i<=2; ++i)
    for (j=1; j<=2; ++j) {
      ii.first = i; ii.second = j;
      // std::map::insert() returns std::pair<IIPRMMap::iterator, bool>
      sum_QlQlm1.insert(empty_iirm_pr).first->
	second.shape(numFunctions, num_lev);
    }
}


void NonDMultilevelSampling::
accumulate_ml_Qsums(IntRealMatrixMap& sum_Q, size_t lev,
		    const RealVector& offset, SizetArray& num_Q)
{
  using std::isfinite;
  Real q_l, q_l_prod;
  int ord, active_ord; size_t qoi;
  IntRespMCIter r_it; IntRMMIter q_it;
  bool os = !offset.empty();

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const RealVector& fn_vals = r_it->second.function_values();

    for (qoi=0; qoi<numFunctions; ++qoi) {
      q_l_prod = q_l = (os) ? fn_vals[qoi] - offset[qoi] : fn_vals[qoi];

      if (isfinite(q_l)) { // neither NaN nor +/-Inf
	q_it = sum_Q.begin(); ord = q_it->first;
	active_ord = 1;
	while (q_it!=sum_Q.end()) {

	  if (ord == active_ord) {
	    q_it->second(qoi,lev) += q_l_prod; ++q_it;
	    ord = (q_it == sum_Q.end()) ? 0 : q_it->first;
	  }

	  q_l_prod *= q_l; ++active_ord;
	}
	++num_Q[qoi];
      }
    }
    //Cout << r_it->first << ": " << sum_Q[1];
  }
}


void NonDMultilevelSampling::
accumulate_ml_Qsums(IntRealMatrixMap& sum_Ql, IntRealMatrixMap& sum_Qlm1,
		    IntIntPairRealMatrixMap& sum_QlQlm1, size_t lev,
		    const RealVector& offset, SizetArray& num_Q)
{
  if (lev == 0)
    accumulate_ml_Qsums(sum_Ql, lev, offset, num_Q);
  else {
    using std::isfinite;
    Real q_l, q_lm1, q_l_prod, q_lm1_prod, qq_prod;
    int l1_ord, l2_ord, active_ord; size_t qoi;
    IntRespMCIter r_it; IntRMMIter l1_it, l2_it; IntIntPair pr;
    bool os = !offset.empty();

    for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
      const RealVector& fn_vals = r_it->second.function_values();

      for (qoi=0; qoi<numFunctions; ++qoi) {
	// response mode AGGREGATED_MODELS orders HF (active model key)
	// followed by LF (previous/decremented model key)
	q_l_prod   = q_l   = (os) ?  fn_vals[qoi] - offset[qoi] : fn_vals[qoi];
	q_lm1_prod = q_lm1 = (os) ?
	  fn_vals[qoi+numFunctions] - offset[qoi+numFunctions] :
	  fn_vals[qoi+numFunctions];

	// sync sample counts for Ql and Qlm1
	if (isfinite(q_l) && isfinite(q_lm1)) { // neither NaN nor +/-Inf

	  // covariance terms: products of q_l and q_lm1
	  pr.first = pr.second = 1;
	  qq_prod = q_l_prod * q_lm1_prod;
	  sum_QlQlm1[pr](qoi,lev) += qq_prod;
	  pr.second = 2;
	  sum_QlQlm1[pr](qoi,lev) += qq_prod * q_lm1_prod;
	  pr.first = 2; pr.second = 1;
	  qq_prod *= q_l_prod;
	  sum_QlQlm1[pr](qoi,lev) += qq_prod;
	  pr.second = 2;
	  sum_QlQlm1[pr](qoi,lev) += qq_prod * q_lm1_prod;

	  // mean,variance terms: products of q_l or products of q_lm1
	  l1_it  = sum_Ql.begin();
	  l2_it  = sum_Qlm1.begin();
	  l1_ord = (l1_it == sum_Ql.end())   ? 0 : l1_it->first;
	  l2_ord = (l2_it == sum_Qlm1.end()) ? 0 : l2_it->first;
	  active_ord = 1;
	  while (l1_it != sum_Ql.end() || l2_it != sum_Qlm1.end()) {

	    // Low: Ll, Llm1
	    if (l1_ord == active_ord) {
	      l1_it->second(qoi,lev) += q_l_prod; ++l1_it;
	      l1_ord = (l1_it == sum_Ql.end()) ? 0 : l1_it->first;
	    }
	    if (l2_ord == active_ord) {
	      l2_it->second(qoi,lev) += q_lm1_prod; ++l2_it;
	      l2_ord = (l2_it == sum_Qlm1.end()) ? 0 : l2_it->first;
	    }

	    q_l_prod *= q_l; q_lm1_prod *= q_lm1; ++active_ord;
	  }
	  ++num_Q[qoi];
	}
      }
      //Cout << r_it->first << ": " << sum_Ql[1] << sum_Qlm1[1];
    }
  }
}


void NonDMultilevelSampling::
accumulate_ml_Ysums(IntRealMatrixMap& sum_Y, RealMatrix& sum_YY, size_t lev,
		    const RealVector& offset, SizetArray& num_Y)
{
  using std::isfinite;
  Real lf_fn, lf_prod;
  int y_ord, active_ord; size_t qoi;
  IntRespMCIter r_it; IntRMMIter y_it;
  bool os = !offset.empty();

  if (lev == 0) {
    for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
      const RealVector& fn_vals = r_it->second.function_values();
      for (qoi=0; qoi<numFunctions; ++qoi) {

	lf_prod = lf_fn = (os) ? fn_vals[qoi] - offset[qoi] : fn_vals[qoi];
	if (isfinite(lf_fn)) { // neither NaN nor +/-Inf
	  // add to sum_YY: running sums across all sample increments
	  sum_YY(qoi,lev) += lf_prod * lf_prod;

	  // add to sum_Y: running sums across all sample increments
	  y_it = sum_Y.begin(); y_ord = y_it->first;
	  active_ord = 1;
	  while (y_it!=sum_Y.end() || active_ord <= 1) {
	    if (y_ord == active_ord) {
	      y_it->second(qoi,lev) += lf_prod; ++y_it;
	      y_ord = (y_it == sum_Y.end()) ? 0 : y_it->first;
	    }
	    lf_prod *= lf_fn; ++active_ord;
	  }
	  ++num_Y[qoi];
	}
      }
    }
  }
  else {
    Real hf_fn, hf_prod;
    for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
      const RealVector& fn_vals = r_it->second.function_values();
      for (qoi=0; qoi<numFunctions; ++qoi) {

	// response mode AGGREGATED_MODELS orders HF (active model key)
	// followed by LF (previous/decremented model key)
	hf_prod = hf_fn = (os) ? fn_vals[qoi] - offset[qoi] : fn_vals[qoi];
	lf_prod = lf_fn = (os) ?
	  fn_vals[qoi+numFunctions] - offset[qoi+numFunctions] :
	  fn_vals[qoi+numFunctions];
	if (isfinite(lf_fn) && isfinite(hf_fn)) { // neither NaN nor +/-Inf

	  // add to sum_YY: running sums across all sample increments
	  Real delta_prod = hf_prod - lf_prod;
	  sum_YY(qoi,lev) += delta_prod * delta_prod; // (HF^p-LF^p)^2 for p=1

	  // add to sum_Y: running sums across all sample increments
	  y_it = sum_Y.begin();  y_ord = y_it->first;
	  active_ord = 1;
	  while (y_it!=sum_Y.end() || active_ord <= 1) {
	    if (y_ord == active_ord) {
	      y_it->second(qoi,lev) += hf_prod - lf_prod; // HF^p-LF^p
	      ++y_it; y_ord = (y_it == sum_Y.end()) ? 0 : y_it->first;
	    }
	    hf_prod *= hf_fn; lf_prod *= lf_fn; ++active_ord;
	  }
	  ++num_Y[qoi];
	}
      }
    }
  }
}


void NonDMultilevelSampling::
compute_error_estimates(IntRealMatrixMap &sum_Ql, IntRealMatrixMap &sum_Qlm1,
			IntIntPairRealMatrixMap &sum_QlQlm1,
			Sizet2DArray &num_Q) 
{
  if (!finalMomentsType)
    return;

  if (finalStatErrors.empty())
    finalStatErrors.size(finalStatistics.num_functions()); // init to 0.

  Real agg_estim_var, var_Yl, cm1l, cm2l, cm3l, cm4l, cm1lm1, cm2lm1,
      cm3lm1, cm4lm1, cm1l_sq, cm1lm1_sq, cm2l_sq, cm2lm1_sq, var_Ql, var_Qlm1,
      mu_Q2l, mu_Q2lm1, mu_Q2lQ2lm1,
      mu_Q1lm1_mu_Q2lQ1lm1, mu_Q1lm1_mu_Q1lm1_muQ2l, mu_Q1l_mu_Q1lQ2lm1, mu_Q1l_mu_Q1l_mu_Q2lm1,
      mu_Q1l_mu_Qlm1_mu_Q1lQ1lm1, mu_Q1l_mu_Q1l_mu_Q1lm1_muQ1lm1, mu_Q2l_muQ2lm1, mu_Q1lQ1lm1_mu_Q1lQ1lm1,
      mu_P2lP2lm1, var_P2l, var_P2lm1, covar_P2lP2lm1, term, bessel_corr;
  size_t lev, qoi, cntr = 0, Nlq,
      num_lev = iteratedModel.truth_model().solution_levels();
  IntIntPair pr11(1, 1), pr12(1, 2), pr21(2, 1), pr22(2, 2);
  RealMatrix &sum_Q1l = sum_Ql[1], &sum_Q1lm1 = sum_Qlm1[1],
      &sum_Q2l = sum_Ql[2], &sum_Q2lm1 = sum_Qlm1[2],
      &sum_Q3l = sum_Ql[3], &sum_Q3lm1 = sum_Qlm1[3],
      &sum_Q4l = sum_Ql[4], &sum_Q4lm1 = sum_Qlm1[4],
      &sum_Q1lQ1lm1 = sum_QlQlm1[pr11], &sum_Q1lQ2lm1 = sum_QlQlm1[pr12],
      &sum_Q2lQ1lm1 = sum_QlQlm1[pr21], &sum_Q2lQ2lm1 = sum_QlQlm1[pr22];
  for (qoi = 0; qoi < numFunctions; ++qoi) {

    // std error in mean estimate
    agg_estim_var = 0;
    for (lev = 0; lev < num_lev; ++lev) {
      Nlq = num_Q[lev][qoi];
      agg_estim_var += aggregate_variance_mean_Qsum(sum_Ql, sum_Qlm1, sum_QlQlm1, num_Q, lev, qoi)/Nlq;
    }
    check_negative(agg_estim_var);

    finalStatErrors[cntr++] = std::sqrt(agg_estim_var); // std error
    if (outputLevel >= DEBUG_OUTPUT) {
      Cout << "Estimator SE for mean = " << finalStatErrors[cntr - 1] << "\n";
    }
    // std error in variance or std deviation estimate
    lev = 0;
    agg_estim_var = 0;
    for (lev = 0; lev < num_lev; ++lev) {
      Nlq = num_Q[lev][qoi];
      agg_estim_var += aggregate_variance_variance_Qsum(sum_Ql, sum_Qlm1, sum_QlQlm1, num_Q, lev, qoi)/Nlq;
    }

    check_negative(agg_estim_var);
    if (outputLevel >= DEBUG_OUTPUT){
      Cout << "Estimator Var for variance = " << agg_estim_var << "\n";
      Cout << "Estimator SE for variance = " << sqrt(agg_estim_var) << "\n";
    }

    Real mom2 = momentStats(1, qoi);
    if (finalMomentsType == STANDARD_MOMENTS && mom2 > 0.) {
      // std error of std deviation estimator

      // An approximation for std error of a fn of another std error estimator
      // = derivative of function * std error of the other estimator -->
      // d/dtheta of sqrt( variance(theta) ) = 1/2 variance^{-1/2} = 1/(2 stdev)
      // Note: this approx. assumes normality in the estimator distribution.
      // Harding et al. 2014 assumes normality in the QoI distribution and has
      // been observed to contain bias in numerical experiments, whereas bias
      // in the derivative approx goes to zero asymptotically.
      finalStatErrors[cntr] = std::sqrt(agg_estim_var) / (2. * mom2);
      ++cntr;
      if (outputLevel >= DEBUG_OUTPUT)
        Cout << "Estimator SE for stddev = " << finalStatErrors[cntr - 1] << "\n\n";
    } else // std error of variance estimator
      finalStatErrors[cntr++] = std::sqrt(agg_estim_var);

    // level mapping errors not implemented at this time
    cntr +=
        requestedRespLevels[qoi].length() + requestedProbLevels[qoi].length() +
        requestedRelLevels[qoi].length() + requestedGenRelLevels[qoi].length();
  }
}


/*
void NonDMultilevelSampling::post_run(std::ostream& s)
{
  // Final moments are generated within core_run() by convert_moments().
  // No addtional stats are currently supported.
  //if (statsFlag) // calculate statistics on allResponses
  //  compute_statistics(allSamples, allResponses);

  // NonD::update_aleatory_final_statistics() pushes momentStats into
  // finalStatistics
  update_final_statistics();

  Analyzer::post_run(s);
}


void NonDMultilevelSampling::print_results(std::ostream& s, short results_state)
{
  if (statsFlag) {
    print_multilevel_evaluation_summary(s, NLev);
    s << "<<<<< Equivalent number of high fidelity evaluations: "
      << equivHFEvals << "\n\nStatistics based on multilevel sample set:\n";

  //print_statistics(s);
    print_moments(s, "response function",
		  iteratedModel.truth_model().response_labels());
    archive_moments();
    archive_equiv_hf_evals(equivHFEvals); 
  }
}
*/


static RealVector *static_lev_cost_vec(NULL);
static size_t *static_qoi(NULL);
static Real *static_eps_sq_div_2(NULL);
static RealVector *static_Nlq_pilot(NULL);
static size_t *static_numFunctions(NULL);
static size_t  *static_qoiAggregation(NULL);

static IntRealMatrixMap *static_sum_Ql(NULL);
static IntRealMatrixMap *static_sum_Qlm1(NULL);
static IntIntPairRealMatrixMap *static_sum_QlQlm1(NULL);
static RealMatrix *static_scalarization_response_mapping(NULL);

void NonDMultilevelSampling::assign_static_member(Real &conv_tol, size_t &qoi, size_t &qoi_aggregation, 
              size_t &num_functions, RealVector &level_cost_vec,
              IntRealMatrixMap &sum_Ql, IntRealMatrixMap &sum_Qlm1,
              IntIntPairRealMatrixMap &sum_QlQlm1,
              RealVector &pilot_samples, RealMatrix &scalarization_response_mapping) const
{
    static_lev_cost_vec= &level_cost_vec;
    static_qoi = &qoi;
    static_qoiAggregation = &qoi_aggregation;
    static_numFunctions = &num_functions;
    static_sum_Ql = &sum_Ql;
    static_sum_Qlm1 = &sum_Qlm1;
    static_sum_QlQlm1 = &sum_QlQlm1;
    static_eps_sq_div_2 = &conv_tol;
    static_Nlq_pilot = &pilot_samples;
    static_scalarization_response_mapping = &scalarization_response_mapping;
}

static Real *static_mu_four_L(NULL);
static Real *static_mu_four_H(NULL);
static Real *static_var_L(NULL);
static Real *static_var_H(NULL);
static Real *static_Ax(NULL);

void NonDMultilevelSampling::assign_static_member_problem18(Real &var_L_exact, Real &var_H_exact, 
                                                            Real &mu_four_L_exact, Real &mu_four_H_exact, 
                                                            Real &Ax, RealVector &level_cost_vec) const
{
    static_var_L = &var_L_exact;
    static_var_H = &var_H_exact;
    static_mu_four_L = &mu_four_L_exact;
    static_mu_four_H = &mu_four_H_exact;
    static_Ax = &Ax;
    static_lev_cost_vec= &level_cost_vec;
}

void NonDMultilevelSampling::target_cost_objective_eval_npsol(int &mode, int &n, double *x, double &f, double *gradf,
                                                             int &nstate) 
{
  RealVector optpp_x;
  RealVector optpp_grad_f;
  optpp_x.size(n);
  optpp_grad_f.size(n);

  f = -1; //Dummy value

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }
  target_cost_objective_eval_optpp(mode, n, optpp_x, f, optpp_grad_f, nstate);

  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }

}

void NonDMultilevelSampling::target_cost_constraint_eval_npsol(int& mode, int& m, int& n, int& ldJ, int* needc, double* x, double* g, double* grad_g, int& nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }
  target_cost_constraint_eval_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }

}

void
NonDMultilevelSampling::target_var_constraint_eval_npsol(int &mode, int &m, int &n, int &ldJ, int *needc, double *x,
                                                         double *g, double *grad_g, int &nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_var_constraint_eval_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }
}

void
NonDMultilevelSampling::target_var_objective_eval_npsol(int& mode, int& n, double* x, double& f, double* gradf, int& nstate)
{
  RealVector optpp_x;
  Real optpp_f;
  RealVector optpp_grad_f(n);
  optpp_x.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_var_objective_eval_optpp(mode, n, optpp_x, optpp_f, optpp_grad_f, nstate);

  f = optpp_f;
  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }
}

void
NonDMultilevelSampling::target_sigma_constraint_eval_npsol(int &mode, int &m, int &n, int &ldJ, int *needc, double *x,
                                                         double *g, double *grad_g, int &nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_sigma_constraint_eval_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }
}

void
NonDMultilevelSampling::target_sigma_objective_eval_npsol(int& mode, int& n, double* x, double& f, double* gradf, int& nstate)
{
  RealVector optpp_x;
  Real optpp_f;
  RealVector optpp_grad_f(n);
  optpp_x.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_sigma_objective_eval_optpp(mode, n, optpp_x, optpp_f, optpp_grad_f, nstate);

  f = optpp_f;
  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }
}

void
NonDMultilevelSampling::target_scalarization_constraint_eval_npsol(int &mode, int &m, int &n, int &ldJ, int *needc, double *x,
                                                         double *g, double *grad_g, int &nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_scalarization_constraint_eval_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }
}

void
NonDMultilevelSampling::target_scalarization_objective_eval_npsol(int& mode, int& n, double* x, double& f, double* gradf, int& nstate)
{
  RealVector optpp_x;
  Real optpp_f;
  RealVector optpp_grad_f(n);
  optpp_x.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_scalarization_objective_eval_optpp(mode, n, optpp_x, optpp_f, optpp_grad_f, nstate);

  f = optpp_f;
  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }
}

void NonDMultilevelSampling::target_var_constraint_eval_logscale_npsol(int& mode, int& m, int& n, int& ldJ, int* needc, double* x, double* g, double* grad_g, int& nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_var_constraint_eval_logscale_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }
}

void NonDMultilevelSampling::target_var_objective_eval_logscale_npsol(int& mode, int& n, double* x, double& f, double* gradf, int& nstate)
{
  RealVector optpp_x;
  Real optpp_f;
  RealVector optpp_grad_f(n);
  optpp_x.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_var_objective_eval_logscale_optpp(mode, n, optpp_x, optpp_f, optpp_grad_f, nstate);

  f = optpp_f;
  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }
}

void NonDMultilevelSampling::target_sigma_constraint_eval_logscale_npsol(int& mode, int& m, int& n, int& ldJ, int* needc, double* x, double* g, double* grad_g, int& nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_sigma_constraint_eval_logscale_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }
}

void NonDMultilevelSampling::target_sigma_objective_eval_logscale_npsol(int& mode, int& n, double* x, double& f, double* gradf, int& nstate)
{
  RealVector optpp_x;
  Real optpp_f;
  RealVector optpp_grad_f(n);
  optpp_x.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_sigma_objective_eval_logscale_optpp(mode, n, optpp_x, optpp_f, optpp_grad_f, nstate);

  f = optpp_f;
  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }
}

void NonDMultilevelSampling::target_scalarization_constraint_eval_logscale_npsol(int& mode, int& m, int& n, int& ldJ, int* needc, double* x, double* g, double* grad_g, int& nstate)
{
  RealVector optpp_x;
  RealVector optpp_g;
  RealMatrix optpp_grad_g(1, n);
  optpp_x.size(n);
  optpp_g.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_scalarization_constraint_eval_logscale_optpp(mode, n, optpp_x, optpp_g, optpp_grad_g, nstate);

  g[0] = optpp_g[0];
  for (size_t i = 0; i < n && mode; ++i) {
    grad_g[i] = optpp_grad_g[0][i];
  }
}

void NonDMultilevelSampling::target_scalarization_objective_eval_logscale_npsol(int& mode, int& n, double* x, double& f, double* gradf, int& nstate)
{
  RealVector optpp_x;
  Real optpp_f;
  RealVector optpp_grad_f(n);
  optpp_x.size(n);

  for (size_t i = 0; i < n; ++i) {
    optpp_x[i] = x[i];
  }

  target_scalarization_objective_eval_logscale_optpp(mode, n, optpp_x, optpp_f, optpp_grad_f, nstate);

  f = optpp_f;
  for (size_t i = 0; i < n && mode; ++i) {
    gradf[i] = optpp_grad_f[i];
  }
}

void NonDMultilevelSampling::target_cost_objective_eval_optpp(int mode, int n, const RealVector &x, double &f,
                                                             RealVector &grad_f, int &result_mode) 
{
  f = 0;

#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction){
      result_mode = OPTPP::NLPFunction;
#endif
  for (int i = 0; i < n; ++i) {
    f += x[i] * (*static_lev_cost_vec)[i];
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif

#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPGradient){
      result_mode = OPTPP::NLPGradient;
#endif
  for (int i = 0; i < n; ++i) {
    grad_f[i] = (*static_lev_cost_vec)[i];
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif

}

void NonDMultilevelSampling::target_cost_constraint_eval_optpp(int mode, int n, const RealVector& x, RealVector& g,
                                         RealMatrix& grad_g, int& result_mode) 
{
  g[0] = 0;

#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction){
      result_mode = OPTPP::NLPFunction;
#endif
  for (int i = 0; i < n; ++i) {
    g[0] += x[i] * (*static_lev_cost_vec)[i];
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif

#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPGradient){
      result_mode = OPTPP::NLPGradient;
#endif
  for (int i = 0; i < n; ++i) {
    grad_g[0][i] = (*static_lev_cost_vec)[i];
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif

}

void NonDMultilevelSampling::target_var_constraint_eval_logscale_optpp(int mode, int n, const RealVector& x, RealVector& g,
                                                      RealMatrix& grad_g, int& result_mode)
{
  Real agg_estim_var = 0;
  size_t num_lev = n;
  target_var_constraint_eval_optpp(mode, n, x, g, grad_g, result_mode);
  agg_estim_var = g[0];
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
#endif
  g[0] = std::log(g[0]); // - (*static_eps_sq_div_2);
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
  if(mode & OPTPP::NLPGradient) {
#endif
  for (size_t lev = 0; lev < num_lev; ++lev) {
    grad_g[0][lev] = grad_g[0][lev]/agg_estim_var;
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif
}

void NonDMultilevelSampling::target_var_objective_eval_logscale_optpp(int mode, int n, const RealVector& x, double& f,
                                        RealVector& grad_f, int& result_mode)
{
  Real agg_estim_var = 0;
  size_t num_lev = n;
  target_var_objective_eval_optpp(mode, n, x, f, grad_f, result_mode);
  agg_estim_var = f;
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
#endif
  f = std::log(f);
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
  if(mode & OPTPP::NLPGradient) {
#endif
  for (size_t lev = 0; lev < num_lev; ++lev) {
    grad_f[lev] = grad_f[lev]/agg_estim_var;
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif
}

void NonDMultilevelSampling::target_sigma_constraint_eval_logscale_optpp(int mode, int n, const RealVector& x, RealVector& g,
                                                      RealMatrix& grad_g, int& result_mode)
{
  Real agg_estim_var = 0;
  size_t num_lev = n;
  target_sigma_constraint_eval_optpp(mode, n, x, g, grad_g, result_mode);
  agg_estim_var = g[0];
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
#endif
  g[0] = std::log(g[0]); 
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
  if(mode & OPTPP::NLPGradient) {
#endif
  for (size_t lev = 0; lev < num_lev; ++lev) {
    grad_g[0][lev] = grad_g[0][lev]/agg_estim_var;
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif
}


void NonDMultilevelSampling::target_sigma_objective_eval_logscale_optpp(int mode, int n, const RealVector& x, double& f,
                                        RealVector& grad_f, int& result_mode)
{
  Real agg_estim_var = 0;
  size_t num_lev = n;
  target_sigma_objective_eval_optpp(mode, n, x, f, grad_f, result_mode);
  agg_estim_var = f;
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
#endif
  f = std::log(f); 
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
  if(mode & OPTPP::NLPGradient) {
#endif
  for (size_t lev = 0; lev < num_lev; ++lev) {
    grad_f[lev] = grad_f[lev]/agg_estim_var;
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif
}

void NonDMultilevelSampling::target_scalarization_constraint_eval_logscale_optpp(int mode, int n, const RealVector& x, RealVector& g,
                                                      RealMatrix& grad_g, int& result_mode)
{
  Real agg_estim_var = 0;
  size_t num_lev = n;
  target_scalarization_constraint_eval_optpp(mode, n, x, g, grad_g, result_mode);
  agg_estim_var = g[0];
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
#endif
  g[0] = std::log(g[0]);
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
  if(mode & OPTPP::NLPGradient) {
#endif
  for (size_t lev = 0; lev < num_lev; ++lev) {
    grad_g[0][lev] = grad_g[0][lev]/agg_estim_var;
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif
}

void NonDMultilevelSampling::target_scalarization_objective_eval_logscale_optpp(int mode, int n, const RealVector& x, double& f,
                                        RealVector& grad_f, int& result_mode)
{
  Real agg_estim_var = 0;
  size_t num_lev = n;
  target_scalarization_objective_eval_optpp(mode, n, x, f, grad_f, result_mode);
  agg_estim_var = f;
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
#endif
  f = std::log(f);
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
  if(mode & OPTPP::NLPGradient) {
#endif
  for (size_t lev = 0; lev < num_lev; ++lev) {
    grad_f[lev] = grad_f[lev]/agg_estim_var;
  }
#ifdef HAVE_NPSOL
#elif HAVE_OPTPP
  }
#endif
}

void NonDMultilevelSampling::target_var_constraint_eval_optpp(int mode, int n, const RealVector &x, RealVector &g,
                                                              RealMatrix &grad_g, int &result_mode) 
{
  bool compute_gradient = false;
#ifdef HAVE_NPSOL
  compute_gradient = mode; //if mode == 0, NPSOL ignores gradients
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
    result_mode = OPTPP::NLPFunction;
  }
  if(mode & OPTPP::NLPGradient){
    compute_gradient = true;
    result_mode = OPTPP::NLPGradient;
  }
#endif
  double g_to_f = 0;
  RealVector grad_g_to_grad_f;
  grad_g_to_grad_f.resize(n);
  target_var_objective_eval_optpp(mode, n, x, g_to_f, grad_g_to_grad_f, result_mode);
  g[0] = g_to_f;
  if(compute_gradient){
    for (size_t lev = 0; lev < n; ++lev) {
      grad_g[0][lev] = grad_g_to_grad_f[lev];
    }
  }
}

void NonDMultilevelSampling::target_var_objective_eval_optpp(int mode, int n, const RealVector& x, double& f,
                                        RealVector& grad_f, int& result_mode)
{
  bool compute_gradient = false;
#ifdef HAVE_NPSOL
  compute_gradient = mode; //if mode == 0, NPSOL ignores gradients
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
    result_mode = OPTPP::NLPFunction;
  }
  if(mode & OPTPP::NLPGradient){
    compute_gradient = true;
    result_mode = OPTPP::NLPGradient;
  }
#endif

  // std error in variance or std deviation estimate
  size_t lev = 0;
  Real Nlq = x[lev];
  size_t Nlq_pilot = (*static_Nlq_pilot)[lev];
  size_t qoi = *static_qoi;
  size_t nb_qois = *static_numFunctions;
  short  qoiAggregation = *static_qoiAggregation;
  size_t num_lev = n;
  RealVector agg_estim_var_l;
  agg_estim_var_l.size(num_lev);
  Real agg_estim_var = 0;

  if (qoiAggregation==QOI_AGGREGATION_SUM) {
    agg_estim_var_l[0] = 0;
    for(size_t qoi = 0; qoi < nb_qois; ++qoi){
      agg_estim_var_l[0] = var_of_var_ml_l0(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi,
                                       compute_gradient, grad_f[0]);
      agg_estim_var += agg_estim_var_l[0];
      for (lev = 1; lev < num_lev; ++lev) {
        Nlq = x[lev];
        Nlq_pilot = (*static_Nlq_pilot)[lev];

        agg_estim_var_l[lev] = var_of_var_ml_l(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi, lev,
                                         compute_gradient, grad_f[lev]);
        agg_estim_var += agg_estim_var_l[lev];
      }
    }
  }
  else if(qoiAggregation==QOI_AGGREGATION_MAX){
    agg_estim_var_l[0] = var_of_var_ml_l0(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi,
                                   compute_gradient, grad_f[0]);
    agg_estim_var += agg_estim_var_l[0];
    for (lev = 1; lev < num_lev; ++lev) {
      Nlq = x[lev];
      Nlq_pilot = (*static_Nlq_pilot)[lev];

      agg_estim_var_l[lev] = var_of_var_ml_l(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi, lev,
                                       compute_gradient, grad_f[lev]);
      agg_estim_var += agg_estim_var_l[lev];
    }
  }else{
    Cout << "NonDMultilevelSampling::multilevel_mc_Qsum: qoiAggregation is not known.\n";
    abort_handler(INTERFACE_ERROR);
  }

  f = agg_estim_var; 
}

void NonDMultilevelSampling::target_sigma_constraint_eval_optpp(int mode, int n, const RealVector &x, RealVector &g,
                                                              RealMatrix &grad_g, int &result_mode) 
{

  bool compute_gradient = false;
  #ifdef HAVE_NPSOL
    compute_gradient = mode; //if mode == 0, NPSOL ignores gradients
  #elif HAVE_OPTPP
    if(mode & OPTPP::NLPFunction) {
      result_mode = OPTPP::NLPFunction;
    }
    if(mode & OPTPP::NLPGradient){
      compute_gradient = true;
      result_mode = OPTPP::NLPGradient;
    }
  #endif

  double g_to_f = 0;
  RealVector grad_g_to_grad_f;
  grad_g_to_grad_f.resize(n);
  target_sigma_objective_eval_optpp(mode, n, x, g_to_f, grad_g_to_grad_f, result_mode);
  g[0] = g_to_f;
  if(compute_gradient){
    for (size_t lev = 0; lev < n; ++lev) {
      grad_g[0][lev] = grad_g_to_grad_f[lev];
    }
  }
}

void NonDMultilevelSampling::target_sigma_objective_eval_optpp(int mode, int n, const RealVector& x, double& f,
                                        RealVector& grad_f, int& result_mode)
{

  bool compute_gradient = false;
#ifdef HAVE_NPSOL
  compute_gradient = mode; //if mode == 0, NPSOL ignores gradients
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
    result_mode = OPTPP::NLPFunction;
  }
  if(mode & OPTPP::NLPGradient){
    compute_gradient = true;
    result_mode = OPTPP::NLPGradient;
  }
#endif

  // std error in variance or std deviation estimate
  size_t lev = 0;
  Real Nlq = x[lev];
  size_t Nlq_pilot = (*static_Nlq_pilot)[lev];
  size_t qoi = *static_qoi;
  size_t nb_qois = *static_numFunctions;
  short  qoiAggregation = *static_qoiAggregation;
  size_t num_lev = n;
  RealVector agg_estim_var_of_var_l, agg_estim_var_l, grad_var, grad_var_var;
  agg_estim_var_of_var_l.size(num_lev);
  agg_estim_var_l.size(num_lev);
  grad_var.size(num_lev);
  grad_var_var.size(num_lev);
  RealVector grad_test;
  grad_test.size(num_lev);
  Real agg_estim_var_of_var = 0;
  Real agg_estim_var = 0;
  Real gradient_var = 0;
  Real gradient_var_var = 0;

  IntIntPair pr11(1, 1);

  if (qoiAggregation==QOI_AGGREGATION_SUM) {
    //safe initialization
    for (lev = 0; lev < num_lev; ++lev) {
      agg_estim_var_of_var_l[lev] = 0;
      agg_estim_var_l[lev] = 0;
    }
    for(size_t qoi = 0; qoi < nb_qois; ++qoi){
      agg_estim_var_of_var_l[0] = var_of_var_ml_l0(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi,
                                       compute_gradient, gradient_var_var);
      agg_estim_var_of_var += agg_estim_var_of_var_l[0];

      agg_estim_var_l[0] = variance_Ysum_static((*static_sum_Ql)[1][0][qoi], (*static_sum_Ql)[2][0][qoi], Nlq_pilot, Nlq,
                                       compute_gradient, gradient_var); 
      agg_estim_var += agg_estim_var_l[0];

      if(compute_gradient){
      grad_f[0] += agg_estim_var_l[0] > 0 ? 1./4. * 
                    ( 
                      -1./(agg_estim_var_l[0] * agg_estim_var_l[0]) * gradient_var * agg_estim_var_of_var_l[0] 
                      +
                      1./agg_estim_var_l[0] * gradient_var_var
                    ) : 0;
      }

      for (lev = 1; lev < num_lev; ++lev) {
        Nlq = x[lev];
        Nlq_pilot = (*static_Nlq_pilot)[lev];

        agg_estim_var_of_var_l[lev] = var_of_var_ml_l(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi, lev,
                                         compute_gradient, gradient_var_var);
        agg_estim_var_of_var += agg_estim_var_of_var_l[lev];

        agg_estim_var_l[lev] = var_lev_l_static((*static_sum_Ql)[1][lev][qoi], (*static_sum_Qlm1)[1][lev][qoi], (*static_sum_Ql)[2][lev][qoi], 
                    (*static_sum_Qlm1)[2][lev][qoi], Nlq_pilot, Nlq, compute_gradient, gradient_var);
        agg_estim_var += agg_estim_var_l[lev];

        if(compute_gradient){
          grad_f[lev] += agg_estim_var_l[0] > 0 ? 1./4. * 
                        ( 
                          -1./(agg_estim_var_l[lev] * agg_estim_var_l[lev]) * gradient_var * agg_estim_var_of_var_l[lev] 
                          +
                          1./agg_estim_var_l[lev] * gradient_var_var
                        ) : 0;
        }
      }
    }
  }else if(qoiAggregation==QOI_AGGREGATION_MAX){
    agg_estim_var_of_var_l[0] = var_of_var_ml_l0(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi,
                                   compute_gradient, gradient_var_var);
    agg_estim_var_of_var += agg_estim_var_of_var_l[0];
    grad_var_var[0] = gradient_var_var;

    agg_estim_var_l[0] = variance_Ysum_static((*static_sum_Ql)[1][0][qoi], (*static_sum_Ql)[2][0][qoi], Nlq_pilot, Nlq,
                                       compute_gradient, gradient_var); 
    grad_var[0] = gradient_var;

    agg_estim_var += agg_estim_var_l[0];

    for (lev = 1; lev < num_lev; ++lev) {
      Nlq = x[lev];
      Nlq_pilot = (*static_Nlq_pilot)[lev];

      agg_estim_var_of_var_l[lev] = var_of_var_ml_l(*static_sum_Ql, *static_sum_Qlm1, *static_sum_QlQlm1, Nlq_pilot, Nlq, qoi, lev,
                                       compute_gradient, gradient_var_var);

      agg_estim_var_of_var += agg_estim_var_of_var_l[lev];

      grad_var_var[lev] = gradient_var_var;

      agg_estim_var_l[lev] = var_lev_l_static((*static_sum_Ql)[1][lev][qoi], (*static_sum_Qlm1)[1][lev][qoi], (*static_sum_Ql)[2][lev][qoi], 
                    (*static_sum_Qlm1)[2][lev][qoi], Nlq_pilot, Nlq, compute_gradient, gradient_var);
      agg_estim_var += agg_estim_var_l[lev];
      grad_var[lev] = gradient_var;
    }
    if(compute_gradient){
      for (lev = 0; lev < num_lev; ++lev) {
        grad_f[lev] = agg_estim_var > 0 ? 
                          1./4. * ( (grad_var_var[lev] * agg_estim_var - agg_estim_var_of_var * grad_var[lev])/(agg_estim_var*agg_estim_var) ) 
                          : 0;
      }
    }
  }else{
    Cout << "NonDMultilevelSampling::target_sigma_objective_eval_optpp: qoiAggregation is not known.\n";
    abort_handler(INTERFACE_ERROR);
  }

  f = agg_estim_var > 0 ? 1./4. * agg_estim_var_of_var/agg_estim_var : 0; // - (*static_eps_sq_div_2);
}

void NonDMultilevelSampling::target_scalarization_constraint_eval_optpp(int mode, int n, const RealVector &x, RealVector &g,
                                                              RealMatrix &grad_g, int &result_mode) 
{
  bool compute_gradient = false;
  #ifdef HAVE_NPSOL
    compute_gradient = mode; //if mode == 0, NPSOL ignores gradients
  #elif HAVE_OPTPP
    if(mode & OPTPP::NLPFunction) {
      result_mode = OPTPP::NLPFunction;
    }
    if(mode & OPTPP::NLPGradient){
      compute_gradient = true;
      result_mode = OPTPP::NLPGradient;
    }
  #endif

  double g_to_f = 0;
  RealVector grad_g_to_grad_f;
  grad_g_to_grad_f.resize(n);
  target_scalarization_objective_eval_optpp(mode, n, x, g_to_f, grad_g_to_grad_f, result_mode);
  g[0] = g_to_f;
  if(compute_gradient){
    for (size_t lev = 0; lev < n; ++lev) {
      grad_g[0][lev] = grad_g_to_grad_f[lev];
    }
  }
}

void NonDMultilevelSampling::target_scalarization_objective_eval_optpp(int mode, int n, const RealVector& x, double& f,
                                        RealVector& grad_f, int& result_mode)
{

  bool compute_gradient = false;
#ifdef HAVE_NPSOL
  compute_gradient = mode; //if mode == 0, NPSOL ignores gradients
#elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
    result_mode = OPTPP::NLPFunction;
  }
  if(mode & OPTPP::NLPGradient){
    compute_gradient = true;
    result_mode = OPTPP::NLPGradient;
  }
#endif

  // std error in variance or std deviation estimate
  size_t lev, Nlq_pilot;
  Real Nlq;
  size_t qoi = *static_qoi;
  size_t nb_qois = *static_numFunctions;
  short  qoiAggregation = *static_qoiAggregation;
  size_t num_lev = n;
  IntIntPair pr11(1, 1);
  
  f = 0;
  for (lev = 0; lev < num_lev; ++lev) {
    grad_f[lev] = 0;
  }
  size_t cur_qoi_offset = 0;
  for(size_t cur_qoi = 0; cur_qoi < nb_qois; ++cur_qoi){
    cur_qoi_offset = cur_qoi*2;

    //Mean
    Real f_mean = 0;
    Real f_var = 0;
    Real cur_grad_var = 0;
    RealVector grad_f_mean;
    grad_f_mean.resize(num_lev);
    if (qoiAggregation==QOI_AGGREGATION_SUM) {
      for(size_t sum_qoi = 0; sum_qoi < nb_qois; ++sum_qoi){
        for (lev = 0; lev < num_lev; ++lev) {
          Nlq = x[lev];
          Nlq_pilot = (*static_Nlq_pilot)[lev];

          f_var = (lev == 0) ? variance_Ysum_static((*static_sum_Ql)[1][0][sum_qoi], (*static_sum_Ql)[2][0][sum_qoi], Nlq_pilot, Nlq,
                                         compute_gradient, cur_grad_var) :
                      variance_Qsum_static((*static_sum_Ql)[1][lev][sum_qoi], (*static_sum_Qlm1)[1][lev][sum_qoi], (*static_sum_Ql)[2][lev][sum_qoi], 
                        (*static_sum_QlQlm1)[pr11][lev][sum_qoi], (*static_sum_Qlm1)[2][lev][sum_qoi], Nlq_pilot, Nlq, compute_gradient, cur_grad_var);
          
          f_mean += f_var/Nlq;

          if(compute_gradient){
            grad_f_mean[lev] += (Nlq * cur_grad_var - f_var)/(Nlq*Nlq);
          }
        }
      }
    }else if(qoiAggregation==QOI_AGGREGATION_MAX){
      for (lev = 0; lev < num_lev; ++lev) {
        Nlq = x[lev];
        Nlq_pilot = (*static_Nlq_pilot)[lev];
        f_var = (lev == 0) ? variance_Ysum_static((*static_sum_Ql)[1][0][cur_qoi], (*static_sum_Ql)[2][0][cur_qoi], Nlq_pilot, Nlq,
                                         compute_gradient, cur_grad_var) :
                             variance_Qsum_static((*static_sum_Ql)[1][lev][cur_qoi], (*static_sum_Qlm1)[1][lev][cur_qoi], (*static_sum_Ql)[2][lev][cur_qoi], 
                                         (*static_sum_QlQlm1)[pr11][lev][cur_qoi], (*static_sum_Qlm1)[2][lev][cur_qoi], Nlq_pilot, Nlq, compute_gradient, cur_grad_var);
        f_mean += f_var/Nlq;
        
        if(compute_gradient){
          grad_f_mean[lev] = (Nlq * cur_grad_var - f_var)/(Nlq*Nlq);
        }
      }
    }else{
      Cout << "NonDMultilevelSampling::target_scalarization_objective_eval_optpp: qoiAggregation is not known.\n";
      abort_handler(INTERFACE_ERROR);
    }

    //Sigma
    Real f_sigma = 0;
    RealVector grad_f_sigma;
    grad_f_sigma.resize(num_lev);
    for (lev = 0; lev < num_lev; ++lev) {
      grad_f_sigma[lev] = 0;
    }
    *static_qoi = cur_qoi;
    target_sigma_objective_eval_optpp(mode, n, x, f_sigma, grad_f_sigma, result_mode);
    *static_qoi = qoi; //Reset pointer

    //Cov
    Real f_upper_bound_cov = std::sqrt(f_mean * f_sigma);
    RealVector grad_f_upper_bound_cov;
    grad_f_upper_bound_cov.resize(num_lev);
    if(compute_gradient){
      for (lev = 0; lev < num_lev; ++lev) {
        grad_f_upper_bound_cov[lev] = f_upper_bound_cov > 0. ? 1./(2.*f_upper_bound_cov)*(grad_f_mean[lev]*f_sigma + f_mean*grad_f_sigma[lev]) : 0;
      }
    }

    Real f_tmp = (*static_scalarization_response_mapping)(qoi, cur_qoi_offset) * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset) * f_mean 
          + (*static_scalarization_response_mapping)(qoi, cur_qoi_offset+1) * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset+1) * f_sigma 
          + 2.0 * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset) * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset+1) * f_upper_bound_cov;
    f += f_tmp > 0 ? f_tmp : 0;
    if(compute_gradient){
      for (lev = 0; lev < num_lev; ++lev) {
        grad_f[lev] += (*static_scalarization_response_mapping)(qoi, cur_qoi_offset) * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset) * grad_f_mean[lev] 
          + (*static_scalarization_response_mapping)(qoi, cur_qoi_offset+1) * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset+1) * grad_f_sigma[lev] 
          + 2.0 * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset) * (*static_scalarization_response_mapping)(qoi, cur_qoi_offset+1) * grad_f_upper_bound_cov[lev] ;
      }
    }
  }   
}

void NonDMultilevelSampling::target_var_constraint_eval_optpp_problem18(int mode, int n, const RealVector &x, RealVector &g,
                                                              RealMatrix &grad_g, int &result_mode) 
{
  bool compute_gradient = false;

  #ifdef HAVE_NPSOL
  #elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
    result_mode = OPTPP::NLPFunction;
  }
  if(mode & OPTPP::NLPGradient){
    compute_gradient = true;
    result_mode = OPTPP::NLPGradient;
  }
  #endif

  Real Hl1_deriv, Hl2_deriv;

  Real Ax = *static_Ax;
  Real var_L = *static_var_L;
  Real var_H = *static_var_H;
  Real mu_four_L = *static_mu_four_L;
  Real mu_four_H = *static_mu_four_H;

  //Level 0
  size_t lev = 0;
  Real Nlq = x[lev];
  Real C1 = mu_four_L;
  Real C2 = var_L * var_L;
  Real Hl1 = 1./Nlq;
  Real Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Cost1L = 0;
  Real Cost2L = 0;
  Real var_of_var_l0 = Hl1 * C1 - Hl2 * C2;
  if(compute_gradient){
    Hl1_deriv = -1./(Nlq*Nlq);
    Hl2_deriv = ( (Nlq * (Nlq - 1.)) - (Nlq - 3.)*(2. * Nlq - 1) ) / ( std::pow(Nlq * (Nlq - 1.), 2) );
    grad_g[0][lev] = 1./var_of_var_l0 *
        ( Hl1_deriv * C1 - Hl2_deriv * C2 );
  }

  //Level 1
  lev = 1;
  Nlq = x[lev];
  C1 = mu_four_H + mu_four_L;
  C2 = var_H * var_H + var_L * var_L;
  Hl1 = 1./Nlq;
  Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Ax_squared = Ax * Ax;
  Real var_H_squared = var_H * var_H;
  Real Cost1H = std::pow(0.5, 12)/13. * Ax_squared - Ax_squared * var_H_squared;
  Real Cost2H = Ax_squared * var_H_squared;
  Real var_of_var_l1 = Hl1 * C1 - Hl2 * C2 - 2. * Cost1H / Nlq - 2. * Cost2H / (Nlq * (Nlq - 1.));
  if(compute_gradient){
    Hl1_deriv = -1./(Nlq*Nlq);
    Hl2_deriv = ( (Nlq * (Nlq - 1.)) - (Nlq - 3.)*(2. * Nlq - 1) ) / ( (Nlq * (Nlq - 1.))*(Nlq * (Nlq - 1.)) );
    grad_g[0][lev] = 1./var_of_var_l1 *
                     ( Hl1_deriv * C1 - Hl2_deriv * C2
                        - 2. * Cost1H * (-1/(Nlq*Nlq))
                        - 2. * Cost2H * (1 - 2*Nlq ) / (std::pow(Nlq * (Nlq - 1.), 2)) );
  }

  //Final var
  Real var_of_var_ml = var_of_var_l0 + var_of_var_l1;
  g[0] = std::log(var_of_var_ml);
}

void NonDMultilevelSampling::target_sigma_constraint_eval_optpp_problem18(int mode, int n, const RealVector &x, RealVector &g,
                                                              RealMatrix &grad_g, int &result_mode) 
{
  bool compute_gradient = false;

  #ifdef HAVE_NPSOL
  #elif HAVE_OPTPP
  if(mode & OPTPP::NLPFunction) {
    result_mode = OPTPP::NLPFunction;
  }
  if(mode & OPTPP::NLPGradient){
    compute_gradient = true;
    result_mode = OPTPP::NLPGradient;
  }
  #endif

  Real Hl1_deriv, Hl2_deriv;

  Real Ax = *static_Ax;
  Real var_L = *static_var_L;
  Real var_H = *static_var_H;
  Real mu_four_L = *static_mu_four_L;
  Real mu_four_H = *static_mu_four_H;

  //Level 0
  size_t lev = 0;
  Real Nlq = x[lev];
  Real C1 = mu_four_L;
  Real C2 = var_L * var_L;
  Real Hl1 = 1./Nlq;
  Real Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Cost1L = 0;
  Real Cost2L = 0;
  Real var_of_var_l0 = Hl1 * C1 - Hl2 * C2;
  if(compute_gradient){
    Hl1_deriv = -1./(Nlq*Nlq);
    Hl2_deriv = ( (Nlq * (Nlq - 1.)) - (Nlq - 3.)*(2. * Nlq - 1) ) / ( std::pow(Nlq * (Nlq - 1.), 2) );
    grad_g[0][lev] = 1./(4.*var_H*var_H) * ((Hl1_deriv * C1 - Hl2_deriv * C2 ) * var_H);
  }

  //Level 1
  lev = 1;
  Nlq = x[lev];
  C1 = mu_four_H + mu_four_L;
  C2 = var_H * var_H + var_L * var_L;
  Hl1 = 1./Nlq;
  Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Ax_squared = Ax * Ax;
  Real var_H_squared = var_H * var_H;
  Real Cost1H = std::pow(0.5, 12)/13. * Ax_squared - Ax_squared * var_H_squared;
  Real Cost2H = Ax_squared * var_H_squared;
  Real var_of_var_l1 = Hl1 * C1 - Hl2 * C2 - 2. * Cost1H / Nlq - 2. * Cost2H / (Nlq * (Nlq - 1.));
  if(compute_gradient){
    Hl1_deriv = -1./(Nlq*Nlq);
    Hl2_deriv = ( (Nlq * (Nlq - 1.)) - (Nlq - 3.)*(2. * Nlq - 1) ) / ( (Nlq * (Nlq - 1.))*(Nlq * (Nlq - 1.)) );
    grad_g[0][lev] = 1./(4.*var_H*var_H) * ( Hl1_deriv * C1 - Hl2_deriv * C2
                        - 2. * Cost1H * (-1/(Nlq*Nlq))
                        - 2. * Cost2H * (1 - 2*Nlq ) / (std::pow(Nlq * (Nlq - 1.), 2)) * var_H );   
  }
  //Final var
  Real var_of_var_ml = var_of_var_l0 + var_of_var_l1;
  g[0] = (1./(4. * var_H ) * var_of_var_ml);
}

double NonDMultilevelSampling::exact_var_of_var_problem18(const RealVector &Nl) {

  Real Ax = *static_Ax;
  Real var_L = *static_var_L;
  Real var_H = *static_var_H;
  Real mu_four_L = *static_mu_four_L;
  Real mu_four_H = *static_mu_four_H;

  //Level 0
  size_t lev = 0;
  Real Nlq = Nl[lev];
  Real C1 = mu_four_L;
  Real C2 = var_L * var_L;
  Real Hl1 = 1./Nlq;
  Real Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Cost1L = 0;
  Real Cost2L = 0;
  Real var_of_var_l0 = Hl1 * C1 - Hl2 * C2;

  //Level 1
  lev = 1;
  Nlq = Nl[lev];
  C1 = mu_four_H + mu_four_L;
  C2 = var_H * var_H + var_L * var_L;
  Hl1 = 1./Nlq;
  Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Ax_squared = Ax * Ax;
  Real var_H_squared = var_H * var_H;
  Real Cost1H = std::pow(0.5, 12)/13. * Ax_squared - Ax_squared * var_H_squared;
  Real Cost2H = Ax_squared * var_H_squared;
  Real var_of_var_l1 = Hl1 * C1 - Hl2 * C2 - 2. * Cost1H / Nlq - 2. * Cost2H / (Nlq * (Nlq - 1.));

  //Final var
  Real var_of_var_ml = var_of_var_l0 + var_of_var_l1;
  return var_of_var_ml;
 }

 double NonDMultilevelSampling::exact_var_of_sigma_problem18(const RealVector &Nl) {
  Real Ax = *static_Ax;
  Real var_L = *static_var_L;
  Real var_H = *static_var_H;
  Real mu_four_L = *static_mu_four_L;
  Real mu_four_H = *static_mu_four_H;

  //Level 0
  size_t lev = 0;
  Real Nlq = Nl[lev];
  Real C1 = mu_four_L;
  Real C2 = var_L * var_L;
  Real Hl1 = 1./Nlq;
  Real Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Cost1L = 0;
  Real Cost2L = 0;
  Real var_of_var_l0 = Hl1 * C1 - Hl2 * C2;

  //Level 1
  lev = 1;
  Nlq = Nl[lev];
  C1 = mu_four_H + mu_four_L;
  C2 = var_H * var_H + var_L * var_L;
  Hl1 = 1./Nlq;
  Hl2 = (Nlq - 3.)/(Nlq * (Nlq - 1.));
  Real Ax_squared = Ax * Ax;
  Real var_H_squared = var_H * var_H;
  Real Cost1H = std::pow(0.5, 12)/13. * Ax_squared - Ax_squared * var_H_squared;
  Real Cost2H = Ax_squared * var_H_squared;
  Real var_of_var_l1 = Hl1 * C1 - Hl2 * C2 - 2. * Cost1H / Nlq - 2. * Cost2H / (Nlq * (Nlq - 1.));

  //Final var
  Real var_of_var_ml = var_of_var_l0 + var_of_var_l1;
  return (1./(4. * var_H ) * var_of_var_ml);
 }

} // namespace Dakota
