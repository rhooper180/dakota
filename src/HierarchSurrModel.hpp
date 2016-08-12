/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:       HierarchSurrModel
//- Description: A model which manages a surrogate relationship between a low
//-              fidelity approximate model and a high fidelity truth model.
//- Owner:       Mike Eldred
//- Checked by:
//- Version: $Id: HierarchSurrModel.hpp 7024 2010-10-16 01:24:42Z mseldre $

#ifndef HIERARCH_SURR_MODEL_H
#define HIERARCH_SURR_MODEL_H

#include "SurrogateModel.hpp"
#include "ParallelLibrary.hpp"
#include "DataModel.hpp"


namespace Dakota
{

// define special values for componentParallelMode
#define LF_MODEL 1
#define HF_MODEL 2


/// Derived model class within the surrogate model branch for managing
/// hierarchical surrogates (models of varying fidelity).

/** The HierarchSurrModel class manages hierarchical models of varying
    fidelity.  The class contains an ordered array of model forms
    (fidelity ordered from low to high), where each model form may
    also contain a set of solution levels (space/time discretization,
    convergence tolerances, etc.).  At run time, one of these
    combinations is activated as the low fidelity model and used to
    perform approximate function evaluations, while another of these
    combinations is activated as the high fidelity model and used to
    provide truth evaluations for computing corrections to the low
    fidelity results. */

class HierarchSurrModel: public SurrogateModel
{
public:

  //
  //- Heading: Constructors and destructor
  //

  HierarchSurrModel(ProblemDescDB& problem_db); ///< constructor
  ~HierarchSurrModel();                         ///< destructor

  //
  //- Heading: Member functions
  //

  /// return the active DiscrepancyCorrection instance
  DiscrepancyCorrection& discrepancy_correction();

protected:

  //
  //- Heading: Virtual function redefinitions
  //

  /// portion of evaluate() specific to HierarchSurrModel
  void derived_evaluate(const ActiveSet& set);
  /// portion of evaluate_nowait() specific to HierarchSurrModel
  void derived_evaluate_nowait(const ActiveSet& set);
  /// portion of synchronize() specific to HierarchSurrModel
  const IntResponseMap& derived_synchronize();
  /// portion of synchronize_nowait() specific to HierarchSurrModel
  const IntResponseMap& derived_synchronize_nowait();

  /// return the active low fidelity model
  Model& surrogate_model();
  /// set the indices identifying the active low fidelity model
  void surrogate_model_indices(size_t lf_model_index,
                               size_t lf_soln_lev_index = _NPOS);
  /// set the index pair identifying the active low fidelity model
  void surrogate_model_indices(const SizetSizetPair& lf_form_level);
  /// return the indices identifying the active low fidelity model
  const SizetSizetPair& surrogate_model_indices() const;

  /// return pair of active low fidelity and high fidelity model indices
  SizetSizet2DPair get_indices();

  /// return the active high fidelity model
  Model& truth_model();
  /// set the indices identifying the active high fidelity model
  void truth_model_indices(size_t hf_model_index,
                           size_t hf_soln_lev_index = _NPOS);
  /// set the index pair identifying the active high fidelity model
  void truth_model_indices(const SizetSizetPair& hf_form_level);
  /// return the indices identifying the active high fidelity model
  const SizetSizetPair& truth_model_indices() const;

  /// return orderedModels and, optionally, their sub-model recursions
  void derived_subordinate_models(ModelList& ml, bool recurse_flag);

  /// set the relative weightings for multiple objective functions or least
  /// squares terms and optionally recurses into LF/HF models
  void primary_response_fn_weights(const RealVector& wts,
                                   bool recurse_flag = true);

  /// set responseMode and pass any bypass request on to the high
  /// fidelity model for any lower-level surrogate recursions
  void surrogate_response_mode(short mode);

  /// (re)set the surrogate index set in SurrogateModel::surrogateFnIndices
  void surrogate_function_indices(const IntSet& surr_fn_indices);

  /// use the high fidelity model to compute the truth values needed for
  /// correction of the low fidelity model results
  void build_approximation();
  // Uses the c_vars/response anchor point to define highFidResponse
  //bool build_approximation(const RealVector& c_vars,const Response& response);

  /// update component parallel mode for supporting parallelism in
  /// the low ad high fidelity models
  void component_parallel_mode(short mode);

  IntIntPair estimate_partition_bounds(int max_eval_concurrency);

  /// set up parallel operations for the array of ordered model fidelities
  void derived_init_communicators(ParLevLIter pl_iter, int max_eval_concurrency,
                                  bool recurse_flag = true);
  /// set up serial operations for the array of ordered model fidelities
  void derived_init_serial();
  /// set active parallel configuration within the current low and
  /// high fidelity models identified by {low,high}FidelityIndices
  void derived_set_communicators(ParLevLIter pl_iter, int max_eval_concurrency,
                                 bool recurse_flag = true);
  /// deallocate communicator partitions for the HierarchSurrModel
  /// (request forwarded to the the array of ordered model fidelities)
  void derived_free_communicators(ParLevLIter pl_iter, int max_eval_concurrency,
                                  bool recurse_flag = true);

  /// Service the low and high fidelity model job requests received from the
  /// master; completes when termination message received from stop_servers().
  void serve_run(ParLevLIter pl_iter, int max_eval_concurrency);
  /// Executed by the master to terminate the low and high fidelity model
  /// server operations when iteration on the HierarchSurrModel is complete
  void stop_servers();

  /// update the Model's inactive view based on higher level (nested)
  /// context and optionally recurse into
  void inactive_view(short view, bool recurse_flag = true);

  /// set the evaluation counter reference points for the HierarchSurrModel
  /// (request forwarded to the low and high fidelity models)
  void set_evaluation_reference();
  /// request fine-grained evaluation reporting within the low and high
  /// fidelity models
  void fine_grained_evaluation_counters();
  /// print the evaluation summary for the HierarchSurrModel
  /// (request forwarded to the low and high fidelity models)
  void print_evaluation_summary(std::ostream& s, bool minimal_header = false,
                                bool relative_count = true) const;

private:

  //
  //- Heading: Convenience functions
  //

  /// update sameInterfaceInstance based on interface ids for models
  /// identified by current {low,high}FidelityIndices
  void check_interface_instance();

  /// update the passed model (low or high fidelity) with current variable
  /// values/bounds/labels
  void update_model(Model& model);

  /// called from derived_synchronize() and derived_synchronize_nowait() to
  /// extract and rekey response maps using blocking or nonblocking
  /// synchronization on the LF and HF models
  void derived_synchronize_sequential(IntResponseMap& hf_resp_map_rekey,
                                      IntResponseMap& lf_resp_map_rekey,
                                      bool block);
  /// called from derived_synchronize() for case of distinct models/interfaces
  /// with competing LF/HF job queues
  void derived_synchronize_competing();
  /// combine the HF and LF response maps into a combined response map
  void derived_synchronize_combine(const IntResponseMap& hf_resp_map,
                                   IntResponseMap& lf_resp_map,
                                   IntResponseMap& combined_resp_map);
  /// combine the available components from HF and LF response maps
  /// into a combined response map
  void derived_synchronize_combine_nowait(const IntResponseMap& hf_resp_map,
                                          IntResponseMap& lf_resp_map,
                                          IntResponseMap& combined_resp_map);

  /// resize currentResponse based on responseMode
  void resize_response();

  /// helper function used in the AUTO_CORRECTED_SURROGATE responseMode
  /// for computing a correction and applying it to lf_resp_map
  void compute_apply_delta(IntResponseMap& lf_resp_map);

  //
  //- Heading: Data members
  //

  /// manages construction and application of correction functions that
  /// are applied to a surrogate model (DataFitSurr or HierarchSurr) in
  /// order to reproduce high fidelity data.
  DiscrepCorrMap deltaCorr;
  /// order of correction: 0, 1, or 2
  short corrOrder;
  /// type of correction: additive, multiplicative, or combined
  short corrType;

  /// vector to specify a sequence of discrepancy corrections to apply in
  /// AUTO_CORRECTED_SURROGATE mode
  std::vector<SizetSizet2DPair> corrSequence;

  /// Ordered sequence (low to high) of model fidelities.  Models are of
  /// arbitrary type and supports recursions.
  ModelArray orderedModels;
  /// index of the low fidelity model that is currently active within
  /// orderedModels; provides approximate low fidelity function evaluations.
  SizetSizetPair lowFidelityIndices;
  /// index of the high fidelity model that is currently active within
  /// orderedModels; provides truth evaluations for computing corrections
  /// to the low fidelity results.
  SizetSizetPair highFidelityIndices;
  /// flag indicating that the {low,high}FidelityIndices correspond to the
  /// same model instance, requiring modifications to updating and evaluation
  /// scheduling processes
  bool sameModelInstance;
  /// flag indicating that the models identified by {low,high}FidelityIndices
  /// employ the same interface instance, requiring modifications to evaluation
  /// scheduling processes
  bool sameInterfaceInstance;

  /// map of reference truth (high fidelity) responses computed in
  /// build_approximation() and used for calculating corrections
  std::map<SizetSizetPair,Response> truthResponseRef;
  /// map of truth (high-fidelity) responses retrieved in
  /// derived_synchronize_nowait() that could not be returned since
  /// corresponding low-fidelity response portions were still pending
  IntResponseMap cachedTruthRespMap;
};


inline HierarchSurrModel::~HierarchSurrModel()
{ } // Virtual destructor handles referenceCount at Strategy level.


inline void HierarchSurrModel::check_interface_instance()
{
  if (sameModelInstance) sameInterfaceInstance = true;
  else
    sameInterfaceInstance
      = (orderedModels[lowFidelityIndices.first].interface_id() ==
         orderedModels[highFidelityIndices.first].interface_id());
}


inline Model& HierarchSurrModel::surrogate_model()
{ return orderedModels[lowFidelityIndices.first]; }


inline DiscrepancyCorrection& HierarchSurrModel::discrepancy_correction()
{ return deltaCorr[std::make_pair(lowFidelityIndices,highFidelityIndices)]; }


inline void HierarchSurrModel::
surrogate_model_indices(size_t lf_model_index, size_t lf_soln_lev_index)
{
  lowFidelityIndices.first  = lf_model_index;
  lowFidelityIndices.second = lf_soln_lev_index; // including _NPOS default
  sameModelInstance = (lf_model_index == highFidelityIndices.first);
  check_interface_instance();

  if (lf_soln_lev_index != _NPOS)
    orderedModels[lf_model_index].solution_level_index(lf_soln_lev_index);

  DiscrepancyCorrection& delta_corr
    = deltaCorr[std::make_pair(lowFidelityIndices,highFidelityIndices)];
  if (!delta_corr.initialized())
    delta_corr.initialize(orderedModels[lowFidelityIndices.first],
			  surrogateFnIndices, corrType, corrOrder);

  // TO DO:
  //deltaCorr.surrogate_model(orderedModels[lf_model_index]);
  //deltaCorr.clear();
}


inline void HierarchSurrModel::
surrogate_model_indices(const SizetSizetPair& lf_form_level)
{
  lowFidelityIndices = lf_form_level;
  size_t lf_model_index = lf_form_level.first,
         lf_soln_lev_index = lf_form_level.second;
  sameModelInstance = (lf_model_index == highFidelityIndices.first);
  check_interface_instance();

  if (lf_soln_lev_index != _NPOS)
    orderedModels[lf_model_index].solution_level_index(lf_soln_lev_index);

  DiscrepancyCorrection& delta_corr
    = deltaCorr[std::make_pair(lowFidelityIndices,highFidelityIndices)];
  if (!delta_corr.initialized())
    delta_corr.initialize(orderedModels[lowFidelityIndices.first],
			  surrogateFnIndices, corrType, corrOrder);

  // TO DO:
  //deltaCorr.surrogate_model(orderedModels[lf_model_index]);
  //deltaCorr.clear();
}


inline const SizetSizetPair& HierarchSurrModel::surrogate_model_indices() const
{ return lowFidelityIndices; }


inline Model& HierarchSurrModel::truth_model()
{ return orderedModels[highFidelityIndices.first]; }

inline SizetSizet2DPair HierarchSurrModel::get_indices()
{ return std::make_pair(lowFidelityIndices,highFidelityIndices); }


inline void HierarchSurrModel::
truth_model_indices(size_t hf_model_index, size_t hf_soln_lev_index)
{
  highFidelityIndices.first  = hf_model_index;
  highFidelityIndices.second = hf_soln_lev_index; // including _NPOS default
  sameModelInstance = (hf_model_index == lowFidelityIndices.first);
  check_interface_instance();

  if (hf_soln_lev_index != _NPOS)
    orderedModels[hf_model_index].solution_level_index(hf_soln_lev_index);

  DiscrepancyCorrection& delta_corr
    = deltaCorr[get_indices()];
  if (!delta_corr.initialized())
    delta_corr.initialize(surrogate_model(),
			  surrogateFnIndices, corrType, corrOrder);
}


inline void HierarchSurrModel::
truth_model_indices(const SizetSizetPair& hf_form_level)
{
  highFidelityIndices = hf_form_level;
  size_t hf_model_index = hf_form_level.first,
         hf_soln_lev_index = hf_form_level.second;
  sameModelInstance = (hf_model_index == lowFidelityIndices.first);
  check_interface_instance();

  if (hf_soln_lev_index != _NPOS)
    orderedModels[hf_model_index].solution_level_index(hf_soln_lev_index);

  DiscrepancyCorrection& delta_corr
    = deltaCorr[get_indices()];
  if (!delta_corr.initialized())
    delta_corr.initialize(surrogate_model(),
			  surrogateFnIndices, corrType, corrOrder);
}


inline const SizetSizetPair& HierarchSurrModel::truth_model_indices() const
{ return highFidelityIndices; }


inline void HierarchSurrModel::
derived_subordinate_models(ModelList& ml, bool recurse_flag)
{
  size_t i, num_models = orderedModels.size();
  for (i=0; i<num_models; ++i) {
    ml.push_back(orderedModels[i]);
    if (recurse_flag)
      orderedModels[i].derived_subordinate_models(ml, true);
  }
}


inline void HierarchSurrModel::
primary_response_fn_weights(const RealVector& wts, bool recurse_flag)
{
  primaryRespFnWts = wts;
  if (recurse_flag) {
    size_t i, num_models = orderedModels.size();
    for (i=0; i<num_models; ++i)
      orderedModels[i].primary_response_fn_weights(wts, recurse_flag);
  }
}


inline void HierarchSurrModel::surrogate_response_mode(short mode)
{
  responseMode = mode;
  // if necessary, resize the response for entering/exiting an aggregated mode.
  // Since parallel job scheduling only involves either the LF or HF model at
  // any given time, this call does not need to be matched on serve_run() procs.
  resize_response();

  // don't pass to low fidelity model (in case it includes surrogates) since
  // point of a surrogate bypass is to get a surrogate-free truth evaluation
  if (mode == BYPASS_SURROGATE) // recurse in this case
    orderedModels[highFidelityIndices.first].surrogate_response_mode(mode);
}


inline void HierarchSurrModel::
surrogate_function_indices(const IntSet& surr_fn_indices)
{
  surrogateFnIndices = surr_fn_indices;
}


inline IntIntPair HierarchSurrModel::
estimate_partition_bounds(int max_eval_concurrency)
{
  // responseMode is a run-time setting, so we are conservative on usage of
  // max_eval_concurrency as in derived_init_communicators()

  size_t i, num_models = orderedModels.size();
  IntIntPair min_max(INT_MAX, INT_MIN), min_max_i;
  for (i=0; i<num_models; ++i) {
    Model& model_i = orderedModels[i];
    probDescDB.set_db_model_nodes(model_i.model_id());
    min_max_i = model_i.estimate_partition_bounds(max_eval_concurrency);
    if (min_max_i.first  < min_max.first)  min_max.first  = min_max_i.first;
    if (min_max_i.second > min_max.second) min_max.second = min_max_i.second;
  }
  return min_max;

  // list nodes are reset at the calling level after completion of recursion
}


inline void HierarchSurrModel::derived_init_serial()
{
  size_t i, num_models = orderedModels.size();
  for (i=0; i<num_models; ++i)
    orderedModels[i].init_serial();
}


inline void HierarchSurrModel::stop_servers()
{
  component_parallel_mode(0);
}


inline void HierarchSurrModel::inactive_view(short view, bool recurse_flag)
{
  currentVariables.inactive_view(view);
  userDefinedConstraints.inactive_view(view);
  if (recurse_flag) {
    size_t i, num_models = orderedModels.size();
    for (i=0; i<num_models; ++i)
      orderedModels[i].inactive_view(view, recurse_flag);
  }
}


inline void HierarchSurrModel::set_evaluation_reference()
{
  //orderedModels[lowFidelityIndices.first].set_evaluation_reference();

  // don't recurse this, since the eval reference is for the top level iteration
  //if (responseMode == BYPASS_SURROGATE)
  //  orderedModels[highFidelityIndices.first].set_evaluation_reference();

  // may want to add this in time
  //surrModelEvalRef = surrModelEvalCntr;
}


inline void HierarchSurrModel::fine_grained_evaluation_counters()
{
  size_t i, num_models = orderedModels.size();
  for (i=0; i<num_models; ++i)
    orderedModels[i].fine_grained_evaluation_counters();
}


inline void HierarchSurrModel::
print_evaluation_summary(std::ostream& s, bool minimal_header,
                         bool relative_count) const
{
  size_t i, num_models = orderedModels.size();
  for (i=0; i<num_models; ++i)
    orderedModels[i].print_evaluation_summary(s, minimal_header,
        relative_count);
}

} // namespace Dakota

#endif
