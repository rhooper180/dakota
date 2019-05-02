/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:        EvaluationStore
//- Description:  Interface to evaluation storage database
//- Owner:        J. Adam Stephens
//- Version: $Id:$

#ifndef EVALUATION_STORE_H
#define EVALUATION_STORE_H

#include <memory>
#include <set>
#include "DakotaActiveSet.hpp"
#include "dakota_data_types.hpp"

namespace Dakota {

class Variables;
class Response;
class Variables;
class Response;
#ifdef DAKOTA_HAVE_HDF5
class HDF5IOHelper;
#endif
// Hold the default/maximal ActiveSet for a model or interface+model
struct DefaultSet {
    ActiveSet set;
    /// number of functions in the active set
    size_t numFunctions;
    /// number of gradients in the active set
    size_t numGradients;
    /// number of hessians in the active set
    size_t numHessians;
    DefaultSet(const ActiveSet &in_set);
    DefaultSet() {};
};


class EvaluationStore {
  public:
#ifdef DAKOTA_HAVE_HDF5
    /// Set the HDF5IOHelper to use
    void set_database(std::shared_ptr<HDF5IOHelper> db_ptr);
#endif

    /// Database is open for writing
    bool active();
    
    /// Provide model selection
    void model_selection(const unsigned short &selection);

    /// Provide interface selection
    void interface_selection(const unsigned short &selection);
    /// Declare a source for the mdoel or iterator. 
    void declare_source(const String &owner_id, const String &owner_type,
                        const String &source_id, const String &source_type);
    /// Allocate storage for iterators. (The only things stored for iterators are
    /// softlinks to the sources)
    EvaluationsDBState iterator_allocate(const String &iterator_id, const bool &top_level);
    /// Allocate storage for model evaluations
    EvaluationsDBState model_allocate(const String &model_id, const String &model_type, 
                        const Variables &variables, const Response &response,
                        const ActiveSet &set);
    /// Allocate storage for evalulations of interface+model pairs
    EvaluationsDBState interface_allocate(const String &model_id,const String &interface_id, 
                        const String &interface_type, const Variables &variables, 
                        const Response &response, const ActiveSet &set, 
                        const String2DArray &an_comp);
    /// Store variables for a model evaluation
    void store_model_variables(const String &model_id, const String &model_type, 
                                const int &eval_id, const ActiveSet &set, const Variables &variables);
    
    /// Store response for a model evaluation
    void store_model_response(const String &model_id, const String &model_type, 
                                const int &eval_id, const Response &response);

    /// Store variables for an evaluation of a interface+model pair.
    void store_interface_variables(const String &model_id, const String &interface_id, 
                                const int &eval_id, const ActiveSet &set, const Variables &variables);

    /// Store response for an interface+model evaluation
    void store_interface_response(const String &model_id, const String &interface_id, 
                                const int &eval_id, const Response &response);

  private:
    /// Create the root scale group
    String create_scale_root(const String &root_group);

    /// Create the root model group
    String create_model_root(const String &model_id, const String &model_type);

    /// Create the root interface group
    String create_interface_root(const String &model_id, const String &interface_id);

    /// Allocate storage for variables
    void allocate_variables(const String &root_group, const Variables &variables);
    
    /// Allocate storage for responses
    void allocate_response(const String &root_group, const Response &response, 
        const DefaultSet &set_s);
    
    /// Allocate storage for metadata
    void allocate_metadata(const String &root_group, const Variables &variables,
        const Response &response, const DefaultSet &set_s, 
        const String2DArray &an_comps = String2DArray());

    /// Store variables
    void store_variables(const String &root_group, const Variables &variables);

    /// Store response
    void store_response(const String &root_group, const int &resp_idx, 
        const Response &response, const DefaultSet &default_set_s);

    /// Store metadata
    void store_metadata(const String &root_group, const ActiveSet &set, 
        const DefaultSet &default_set_s);

    /// Return true if the model is active
    bool model_active(const String &model_id);

    /// Return true if the interface is active
    bool interface_active(const String &model_id);

    /// Choice of interfaces to store
    unsigned short interfaceSelection;
    /// Choice of models to store
    unsigned short modelSelection;
   
    /// ID of top-level method
    String topLevelMethodId;

#ifdef DAKOTA_HAVE_HDF5
    /// Pointer to HDF5IOHelper instance
    std::shared_ptr<HDF5IOHelper> hdf5Stream;
#endif
    /// Models that have been allocated
    std::set<String> allocatedModels;
    /// Interface+model pairs that have been allocated
    std::set<std::pair<String,String> > allocatedInterfaces;
    /// Default ActiveSets and whether they have gradients and hessians for models
    std::map<String, DefaultSet > modelDefaultSets;
    /// Default ActiveSets and whether they have gradients and hessians for interfaces
    std::map<std::pair<String, String>, DefaultSet > interfaceDefaultSets;

    /// Cache index of "row" in dataset for this model+evalID tuple. 
    std::map< std::tuple<String,int>, int > modelResponseIndexCache;
    /// Cache index of "row" in dataset for this interface+model+evalID tuple. 
    std::map< std::tuple<String,String,int>, int > interfaceResponseIndexCache;

    /// Models that have been declared as sources to iterators. Only populated when
    /// TOP_METHOD or ALL_METHODS model evals are stored.
    std::set<String> sourceModels;

}; // class EvaluationStore

} // Dakota namespace

#endif  // EVALUATION_STORE_H
