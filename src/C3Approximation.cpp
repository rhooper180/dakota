/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

#include "ProblemDescDB.hpp"
#include "C3Approximation.hpp"

#include "SharedC3ApproxData.hpp"
//#include "NonDIntegration.hpp"

//#define DEBUG

namespace Dakota {


void C3FnTrainPtrs::ft_derived_functions_init_null()
{
  ft_derived_fns.set = 0;
        
  ft_derived_fns.ft_squared = NULL;
  ft_derived_fns.ft_cubed   = NULL;
  ft_derived_fns.ft_constant_at_mean = NULL;
  ft_derived_fns.ft_diff_from_mean   = NULL;
  ft_derived_fns.ft_diff_from_mean_squared = NULL;
  ft_derived_fns.ft_diff_from_mean_cubed   = NULL;        

  ft_derived_fns.ft_diff_from_mean_tesseracted = NULL;
  ft_derived_fns.ft_diff_from_mean_normalized  = NULL;

  ft_derived_fns.ft_diff_from_mean_normalized_squared = NULL;
  ft_derived_fns.ft_diff_from_mean_normalized_cubed   = NULL;
}


// RENAME TO INDICATE THAT THIS IS REALLY ALLOCATING/FILLING STATS MEMORY DOWNSTREAM
void C3FnTrainPtrs::ft_derived_functions_create(struct MultiApproxOpts * opts)
{
  ft_derived_fns.ft_squared = function_train_product(ft,ft);

  ft_derived_fns.ft_cubed
    = function_train_product(ft_derived_fns.ft_squared,ft);
  //ft_derived_fns.ft_tesseracted
  //  = function_train_product(ft_derived_fns.ft_squared,
  //                           ft_derived_fns.ft_squared);

  ft_derived_fns.first_moment   = function_train_integrate_weighted(ft);
  ft_derived_fns.ft_constant_at_mean
    = function_train_constant(-ft_derived_fns.first_moment,opts);
  ft_derived_fns.ft_diff_from_mean
    = function_train_sum(ft,ft_derived_fns.ft_constant_at_mean);
  ft_derived_fns.ft_diff_from_mean_squared =
    function_train_product(ft_derived_fns.ft_diff_from_mean,
			   ft_derived_fns.ft_diff_from_mean);
  ft_derived_fns.ft_diff_from_mean_cubed =
    function_train_product(ft_derived_fns.ft_diff_from_mean_squared,
			   ft_derived_fns.ft_diff_from_mean);        
  ft_derived_fns.ft_diff_from_mean_tesseracted =
    function_train_product(ft_derived_fns.ft_diff_from_mean_squared,
			   ft_derived_fns.ft_diff_from_mean_squared);

  ft_derived_fns.second_central_moment
    = function_train_integrate_weighted(
      ft_derived_fns.ft_diff_from_mean_squared); // var
        
  ft_derived_fns.third_central_moment
    = function_train_integrate_weighted(ft_derived_fns.ft_diff_from_mean_cubed);
        
  ft_derived_fns.fourth_central_moment
    = function_train_integrate_weighted(
      ft_derived_fns.ft_diff_from_mean_tesseracted);

  ft_derived_fns.second_moment
    = function_train_integrate_weighted(ft_derived_fns.ft_squared);
  ft_derived_fns.third_moment
    = function_train_integrate_weighted(ft_derived_fns.ft_cubed);

  ft_derived_fns.std_dev
    = sqrt(ft_derived_fns.second_central_moment);

  ft_derived_fns.ft_diff_from_mean_normalized
    = function_train_copy(ft_derived_fns.ft_diff_from_mean);
  function_train_scale(ft_derived_fns.ft_diff_from_mean_normalized,
		       1.0/ft_derived_fns.std_dev);

  ft_derived_fns.ft_diff_from_mean_normalized_squared =
    function_train_product(ft_derived_fns.ft_diff_from_mean_normalized,
			   ft_derived_fns.ft_diff_from_mean_normalized);

  ft_derived_fns.ft_diff_from_mean_normalized_cubed =
    function_train_product(ft_derived_fns.ft_diff_from_mean_normalized_squared,
			   ft_derived_fns.ft_diff_from_mean_normalized);

  ft_derived_fns.skewness
    = function_train_integrate_weighted(
      ft_derived_fns.ft_diff_from_mean_normalized_cubed);
  ft_derived_fns.kurtosis = ft_derived_fns.fourth_central_moment
    / ft_derived_fns.second_central_moment
    / ft_derived_fns.second_central_moment;

  ft_derived_fns.set = 1;
}


void C3FnTrainPtrs::ft_derived_functions_free()
{
  function_train_free(ft_derived_fns.ft_squared);
  ft_derived_fns.ft_squared          = NULL;
  function_train_free(ft_derived_fns.ft_cubed);
  ft_derived_fns.ft_cubed            = NULL;
  function_train_free(ft_derived_fns.ft_constant_at_mean);
  ft_derived_fns.ft_constant_at_mean = NULL;
  function_train_free(ft_derived_fns.ft_diff_from_mean);
  ft_derived_fns.ft_diff_from_mean   = NULL;
  function_train_free(ft_derived_fns.ft_diff_from_mean_squared);
  ft_derived_fns.ft_diff_from_mean_squared = NULL;

  function_train_free(ft_derived_fns.ft_diff_from_mean_cubed);
  ft_derived_fns.ft_diff_from_mean_cubed = NULL;        

  function_train_free(ft_derived_fns.ft_diff_from_mean_tesseracted);
  ft_derived_fns.ft_diff_from_mean_tesseracted = NULL;
  function_train_free(ft_derived_fns.ft_diff_from_mean_normalized);
  ft_derived_fns.ft_diff_from_mean_normalized  = NULL;

  function_train_free(ft_derived_fns.
		      ft_diff_from_mean_normalized_squared);
  ft_derived_fns.ft_diff_from_mean_normalized_squared = NULL;

  function_train_free(ft_derived_fns.ft_diff_from_mean_normalized_cubed);
  ft_derived_fns.ft_diff_from_mean_normalized_cubed = NULL;

  ft_derived_fns.set = 0;
}


C3Approximation::
C3Approximation(ProblemDescDB& problem_db,
		const SharedApproxData& shared_data,
		const String& approx_label):
  Approximation(BaseConstructor(), problem_db, shared_data, approx_label)
  //sharedC3DataRep((SharedC3ApproxData*)sharedDataRep)
{ base_init(); }


C3Approximation::C3Approximation(const SharedApproxData& shared_data):
  Approximation(NoDBBaseConstructor(), shared_data)
  //sharedC3DataRep((SharedC3ApproxData*)sharedDataRep)
{ base_init(); }


C3Approximation::~C3Approximation()
{
  // memory deallocations now managed by C3FnTrainPtrs dtor
}


void C3Approximation::base_init()
{
  // NULL ptr initializations now managed by C3FnTrainPtrs ctor

  // Initialize memory for the rest of private data
  expansionMoments.size(4);
}


void C3Approximation::build()
{
  if (sharedDataRep->outputLevel >= DEBUG_OUTPUT)
    Cout << "Calling C3Approximation::build()\n";

  // base class implementation checks data set against min required
  Approximation::build();

  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  if (data_rep->adaptConstruct) {
    Cerr << "Error: Adaptive construction not yet implemented in "
	 << "C3Approximation." << std::endl;
    abort_handler(APPROX_ERROR);
  }
  else {
    size_t i, j, num_v = sharedDataRep->numVars;
    SizetVector start_ranks(num_v+1);  
    start_ranks(0) = 1;  start_ranks(num_v) = 1;
    for (i=1; i<num_v; ++i)
      start_ranks(i) = data_rep->startRank;

    struct FTRegress * ftr
      = ft_regress_alloc(num_v, data_rep->approxOpts, start_ranks.values());
	    
    if (data_rep->regressType == FT_RLS2) {
      ft_regress_set_alg_and_obj(ftr,AIO,FTLS_SPARSEL2);
      // reg param is required (no reasonable default due to scaling)
      ft_regress_set_regularization_weight(ftr, data_rep->regressRegParam);
    }
    else // default
      ft_regress_set_alg_and_obj(ftr,AIO,FTLS);

    size_t r_adapt = data_rep->adaptRank ? 1 : 0;
    ft_regress_set_adapt(   ftr,r_adapt);
    ft_regress_set_maxrank( ftr,data_rep->maxRank);
    ft_regress_set_kickrank(ftr,data_rep->kickRank);
    ft_regress_set_roundtol(ftr,data_rep->roundingTol);
    ft_regress_set_verbose( ftr,data_rep->c3Verbosity);

    double absxtol = 1e-10;
    struct c3Opt* optimizer = c3opt_create(BFGS);
    c3opt_set_maxiter(optimizer,data_rep->maxSolverIterations);
    c3opt_set_gtol   (optimizer,data_rep->solverTol);
    c3opt_set_relftol(optimizer,data_rep->solverTol);
    c3opt_set_absxtol(optimizer,absxtol);
    c3opt_set_verbose(optimizer,data_rep->c3Verbosity);

    // free if previously built
    levApproxIter->second.free_ft();

    if (data_rep->crossVal) // future capability for poly orders
      Cerr << "Warning: CV is not yet implemented in C3Approximation.  "
	   << "Ignoring CV request.\n";

    const Pecos::SurrogateData& approx_data = surrogate_data();
    const Pecos::SDVArray& sdv_array = approx_data.variables_data();
    const Pecos::SDRArray& sdr_array = approx_data.response_data();
    size_t ndata = approx_data.points();

    // JUST 1 QOI
    // Transfer the training data to the Teuchos arrays used by the GP
    // input variables (reformats surrData for C3)
    double* xtrain = (double*)calloc(num_v*ndata,sizeof(double));
    // QoI observations (reformats surrData for C3)
    double* ytrain = (double*)calloc(ndata,sizeof(double));

    // process currentPoints
    for (i=0; i<ndata; ++i) {
      const RealVector& c_vars = sdv_array[i].continuous_variables();
      for (j=0; j<num_v; j++)
	xtrain[j + i*num_v] = c_vars[j];
      ytrain[i] = sdr_array[i].response_function();
    }

    // Build FT model
    levApproxIter->second.ft
      = ft_regress_run(ftr,optimizer,ndata,xtrain,ytrain);
    // Avoid these calculations if not needed
    // > clarify expansions of derivatives w.r.t. non-random variables
    //   vs. random deriv-enhanced builds vs. eval of derivs of expansions:
    //   --> These derivatives are functions that can then be evaluated after
    //       the build at particular points.
    // > then add flags / final_asv control of these
    //   --> the logic on whether or not to precompute these needs to be based
    //       on whether we expect emulator grad/Hessian evals downstream.
    if (false) { // *** TO DO
      levApproxIter->second.ft_gradient
	= function_train_gradient(levApproxIter->second.ft);
      levApproxIter->second.ft_hessian
	= ft1d_array_jacobian(levApproxIter->second.ft_gradient);
    }

    // free approximation stuff
    free(xtrain);          xtrain    = NULL;
    free(ytrain);          ytrain    = NULL;
    ft_regress_free(ftr);  ftr       = NULL;
    c3opt_free(optimizer); optimizer = NULL;
  }
}


void C3Approximation::rebuild()
{
  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  active_model_key(data_rep->activeKey); // redundant but needed for
                                                // prevExp prior to compute
  // for use in pop_coefficients()
  prevC3FTPtrs.copy(levApproxIter->second); // deep copy

  build(); // from scratch
}


void C3Approximation::pop_coefficients(bool save_data)
{
  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  const UShortArray& key = data_rep->activeKey;

  // likely overkill, but multilevel roll up after increment modifies and
  // then restores active key
  active_model_key(key);

  C3FnTrainPtrs& c3ft_ptrs = levApproxIter->second;

  // store the incremented coeff state for possible push
  //if (save_data)
  //  poppedC3FTPtrs[key].push_back(c3ft_ptrs);

  // reset expansion
  c3ft_ptrs.copy(prevC3FTPtrs);

  //clear_computed_bits();
}


void C3Approximation::push_coefficients()
{
  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  const UShortArray& key = data_rep->activeKey;

  // synchronize expansionCoeff{s,Grads} and approxData
  active_model_key(key);

  // SharedPolyApproxData::candidate_index() currently returns 0 for
  // all cases other than generalized sparse grids
  //size_t p_index = data_rep->push_index(key); // *** TO DO

  // store current state for use in pop_coefficients()
  prevC3FTPtrs.copy(levApproxIter->second); // deep copy

  // retrieve a previously popped state
  //std::map<UShortArray, C3FnTrainPtrs>::iterator prv_it
  //  = poppedC3FTPtrs.find(key); // *** TO DO
  //C3FnTrainPtrsDeque::iterator rv_it;
  //if (prv_it != poppedC3FTPtrs.end()) {
  //  rv_it = prv_it->second.begin();      std::advance(rv_it, p_index);
  //  levApproxIter->second.copy(*rv_it);  prv_it->second.erase(rv_it);
  //}

  //clear_computed_bits();
}


void C3Approximation::combine_coefficients()
{
  // Option 1: adds x to y and overwrites y (I allocate x and y)
  combinedC3FTPtrs.free_ft();
  std::map<UShortArray, C3FnTrainPtrs>::iterator it = levelApprox.begin();
  combinedC3FTPtrs.ft = function_train_copy(it->second.ft); ++it;
  for (; it!= levelApprox.end(); ++it)
    c3axpy(1., it->second.ft, &combinedC3FTPtrs.ft, 1.e-8);

  // Option 2: function_train_sum (I allocate a and b and C3 allocates c)
  // > remember to deallocate c when done
  //struct FunctionTrain * c = function_train_sum(a, b);
}


void C3Approximation::combined_to_active_coefficients(bool clear_combined)
{
  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  active_model_key(data_rep->activeKey);

  levApproxIter->second.copy(combinedC3FTPtrs);
  if (clear_combined)
    combinedC3FTPtrs.free_all();

  //allocate_component_sobol();  // size sobolIndices from shared sobolIndexMap

  // If outgoing stats type is active (e.g., as in Dakota::NonDExpansion::
  // multifidelity_expansion()), then previous active stats are invalidated.
  // But if outgoing stats type is combined, then can avoid recomputation
  // and carry over current moment stats from combined to active. 
  // Note: this reuse optimization introduces an order dependency --> updating
  //       stats type from COMBINED to ACTIVE must occur after this function
  //if (data_rep->expConfigOptions.refineStatsType == ACTIVE_EXPANSION_STATS)
  //  clear_computed_bits();
}


void C3Approximation::clear_inactive_coefficients()
{
  std::map<UShortArray, C3FnTrainPtrs>::iterator it = levelApprox.begin();
  while (it != levelApprox.end())
    if (it == levApproxIter) // preserve active
      ++it;
    else // clear inactive: postfix increments manage iterator invalidations
      levelApprox.erase(it++);
}


void C3Approximation::compute_all_sobol_indices(size_t interaction_order)
{
  C3SobolSensitivity* fts = levApproxIter->second.ft_sobol;
  if (fts) c3_sobol_sensitivity_free(fts);
  fts = c3_sobol_sensitivity_calculate(levApproxIter->second.ft,
				       interaction_order);
}


void C3Approximation::compute_derived_statistics(bool overwrite)
{
  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  if (levApproxIter->second.ft_derived_fns.set == 0)
    levApproxIter->second.ft_derived_functions_create(data_rep->approxOpts);
  else if (overwrite == true) {
    levApproxIter->second.ft_derived_functions_free();
    levApproxIter->second.ft_derived_functions_create(data_rep->approxOpts);
  }
}


void C3Approximation::compute_moments(bool full_stats, bool combined_stats)
{
  expansionMoments[0] = mean();          expansionMoments[1] = variance();
  expansionMoments[2] = third_central(); expansionMoments[3] = fourth_central();
}


void C3Approximation::
compute_moments(const RealVector& x, bool full_stats, bool combined_stats)
{ expansionMoments[0] = mean(x);         expansionMoments[1] = variance(x); }


Real C3Approximation::mean()
{
  compute_derived_statistics(false);
  return levApproxIter->second.ft_derived_fns.first_moment;
}


Real C3Approximation::mean(const RealVector &x)
{
  // compute_derived_statistics(false);

  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  const SizetVector& rand_indices = data_rep->randomIndices;
  struct FunctionTrain * ftnonrand =
    function_train_integrate_weighted_subset(levApproxIter->second.ft,
      rand_indices.length(),rand_indices.values());

  double out = function_train_eval(ftnonrand,x.values());
  function_train_free(ftnonrand); //ftnonrand = NULL;
    
  return out;
}


const RealVector& C3Approximation::mean_gradient()
{
  Cerr << "Error: mean_gradient() in C3Approximation is not implemented "
       << "because\n       Alex is not sure how what it means" << std::endl;
  abort_handler(APPROX_ERROR);
}


const RealVector& C3Approximation::
mean_gradient(const RealVector &x,const SizetArray & dvv)
{
  Cerr << "Error: mean_gradient(x,dvv) in C3Approximation is not implemented "
       << "because\n       Alex is not sure how what it means" << std::endl;
  abort_handler(APPROX_ERROR);        
}


Real C3Approximation::variance()
{
  compute_derived_statistics(false);
  return levApproxIter->second.ft_derived_fns.second_central_moment;
}


Real C3Approximation::variance(const RealVector &x)
{
  compute_derived_statistics(false);

  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  const SizetVector& rand_indices = data_rep->randomIndices;
  struct FunctionTrain * ftnonrand =
    function_train_integrate_weighted_subset(
      levApproxIter->second.ft_derived_fns.ft_squared,
      rand_indices.length(),rand_indices.values());

  //size_t num_det = sharedDataRep->numVars - num_rand;
  //for (size_t ii = 0; ii < num_det;ii++)
  //  pt_det[ii] = x(ii);
  //double var = function_train_eval(ftnonrand,pt_det) - mean * mean;

  Real mu = mean(),
      var = function_train_eval(ftnonrand,x.values()) - mu * mu;
    
  function_train_free(ftnonrand); //ftnonrand = NULL;
    
  return var;
}


const RealVector& C3Approximation::variance_gradient()
{
  // Gradient of the Variance with respect to design variables *NOT VARIANCE OF GRADIENT*
  // I need to manage low-rank representations of the gradient, then evaluate the variance of each
  // of the partial derivatives
  Cerr << "Error: mean_gradient() in C3Approximation is not implemented "
       << "because\n       Alex is not sure how what it means" << std::endl;
  abort_handler(APPROX_ERROR);
}


const RealVector & C3Approximation::
variance_gradient(const RealVector &x,const SizetArray & dvv)
{
  // size of output is size of dvv, only take gradients with respect to dvv fix design and epistemic at x
  Cerr << "Error: mean_gradient(x,dvv) in C3Approximation is not implemented "
       << "because\n       Alex is not sure how what it means" << std::endl;
  abort_handler(APPROX_ERROR);        
}


struct FunctionTrain * C3Approximation::subtract_const(Real val)
{
  SharedC3ApproxData* data_rep = (SharedC3ApproxData*)sharedDataRep;
  struct FunctionTrain * ftconst
    = function_train_constant(val,data_rep->approxOpts);
  struct FunctionTrain * updated
    = function_train_sum(levApproxIter->second.ft,ftconst);

  function_train_free(ftconst); ftconst = NULL;
  return updated;
}


Real C3Approximation::covariance(Approximation& approx_2)
{
  C3Approximation* ft2 = (C3Approximation*)approx_2.approx_rep();
  Real mean1 = mean(), mean2 = ft2->mean();

  struct FunctionTrain * fttemp =       subtract_const(mean1);
  struct FunctionTrain * fttemp2 = ft2->subtract_const(mean2);

  double retval = function_train_inner_weighted(fttemp,fttemp2);

  function_train_free(fttemp);  fttemp = NULL;
  function_train_free(fttemp2); fttemp2 = NULL;

  return retval;
}


Real C3Approximation::covariance(const RealVector& x, Approximation& approx_2)
{
  C3Approximation* ft2 = (C3Approximation*)approx_2.approx_rep();

  Cerr << "Error: covariance(x,ft2) in C3Approximation is not implemented "
       << "because\n       Alex is not sure how what it means" << std::endl;
  abort_handler(APPROX_ERROR);
}


int C3Approximation::min_coefficients() const
{
  // min number of samples required to build the network is equal to
  // the number of design variables + 1

  // Note: Often this is too few samples.  It is better to have about
  // O(n^2) samples, where 'n' is the number of variables.

  return sharedDataRep->numVars + 1;
}

    
// ignore discrete variables for now
Real C3Approximation::value(const Variables& vars)
{
  return function_train_eval(levApproxIter->second.ft,
			     vars.continuous_variables().values());
}


// ignore discrete variables for now
const RealVector& C3Approximation::gradient(const Variables& vars)
{
  size_t i, num_v = sharedDataRep->numVars;
  if (approxGradient.empty())
    approxGradient.sizeUninitialized(num_v);
  const Real* c_vars = vars.continuous_variables().values();
  for (i = 0; i < num_v; ++i)
    approxGradient(i)
      = function_train_eval(levApproxIter->second.ft_gradient->ft[i], c_vars);
  return approxGradient;
}


// ignore discrete variables for now
const RealSymMatrix& C3Approximation::hessian(const Variables& vars)
{
  size_t i, j, num_v = sharedDataRep->numVars;
  if (approxHessian.empty())
    approxHessian.shapeUninitialized(num_v);
  const Real* c_vars = vars.continuous_variables().values();
  for (i = 0; i < num_v; ++i)
    for (j = 0; j <= i; ++j)
      approxHessian(i,j)
	= function_train_eval(levApproxIter->second.ft_hessian->ft[i+j*num_v],
			      c_vars);
  return approxHessian;
}

} // namespace Dakota
