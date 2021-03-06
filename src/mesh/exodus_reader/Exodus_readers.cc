#include <algorithm>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lambda/lambda.hpp>
namespace bl = boost::lambda;

#include "Exodus_readers.hh"
#include "Exodus_error.hh"

#include "Element_types.hh"

#include <exodusII.h>

// MAX_STR_LENGTH AND MAX_LINE_LENGTH are defined in exodusII.h

namespace 
{

template <typename S>
Amanzi::AmanziMesh::Data::Coordinates<double>* 
read_coordinates_impl_ (Amanzi::Exodus::Exodus_file file, 
                        int num_nodes, int dimensions)
{

    std::vector<std::vector<S> > data (dimensions, std::vector<S>(num_nodes));

    int ret_val = ex_get_coord (file.id, &data [0] [0], &data [1] [0], &data [2] [0]);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read coordinates (%d)") %
                   file.filename % ret_val);
      Exceptions::amanzi_throw( Amanzi::Exodus::ExodusError (msg.c_str()) );
    }

    return Amanzi::AmanziMesh::Data::Coordinates<double>::build_from (data);

}
}


namespace Amanzi {
namespace Exodus {

AmanziMesh::Data::Parameters* read_parameters (Exodus_file file)
{

    char title_data [MAX_LINE_LENGTH];

    // Global mesh information
    // -----------------------
    int dimensionality;
    int num_nodes;
    int num_elements;
    int num_element_blocks;
    int num_node_sets;
    int num_side_sets;
    int ret_val = ex_get_init (file.id, title_data, 
                               &dimensionality, &num_nodes, &num_elements, 
                               &num_element_blocks, &num_node_sets, &num_side_sets);

    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read global parameters (%d)") %
                   file.filename % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }
    std::string title(title_data);

    std::vector<int> element_block_ids;
    std::vector<int> node_set_ids;
    std::vector<int> side_set_ids;

    // Element blocks
    // --------------
    if (num_element_blocks > 0)
    {
        element_block_ids.resize (num_element_blocks);
        ret_val = ex_get_elem_blk_ids (file.id, &element_block_ids [0]);
        if (ret_val < 0) {
          std::string msg = 
            boost::str(boost::format("%s: error: cannot read element block ids (%d)") %
                       file.filename % ret_val);
          Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
        }
    }


    // Node sets
    // ---------
    if (num_node_sets > 0)
    {
        node_set_ids.resize (num_node_sets);
        ret_val = ex_get_node_set_ids (file.id, &node_set_ids [0]);
        if (ret_val < 0) {
          std::string msg = 
            boost::str(boost::format("%s: error: cannot read node block ids (%d)") %
                       file.filename % ret_val);
          Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
        }
    }


    // Side sets
    // ---------
    if (num_side_sets > 0)
    {
        side_set_ids.resize (num_side_sets);
        ret_val = ex_get_side_set_ids (file.id, &side_set_ids [0]);
        if (ret_val < 0) {
          std::string msg = 
            boost::str(boost::format("%s: error: cannot read side block ids (%d)") %
                       file.filename % ret_val);
          Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
        }
    }



    
    AmanziMesh::Data::Parameters *params = new AmanziMesh::Data::Parameters (title, dimensionality, num_nodes, num_elements,
                                                               num_element_blocks, num_node_sets, num_side_sets,
                                                               element_block_ids, node_set_ids, side_set_ids);

    return params;

}


AmanziMesh::Data::Coordinates<double>* read_coordinates (Exodus_file file, int num_nodes, int dimensions)
{

    if (file.exodus_word_size == sizeof (float))
        return read_coordinates_impl_<float> (file, num_nodes, dimensions);

    if (file.exodus_word_size == sizeof (double))
        return read_coordinates_impl_<double> (file, num_nodes, dimensions);

    return NULL;
}


AmanziMesh::Data::Node_set* read_node_set (Exodus_file file, int set_id)
{

    int num_nodes;
    int num_dist_factors;
    int ret_val = ex_get_node_set_param (file.id, set_id, &num_nodes, &num_dist_factors);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read node set %d parameters (%d)") %
                   file.filename % set_id % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }

    std::vector<int> node_list(num_nodes);
    std::vector<double> node_dist_factors(num_dist_factors);

    ret_val = ex_get_node_set (file.id, set_id, &node_list [0]);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read node set %d (%d)") %
                   file.filename % set_id % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }

    if (num_dist_factors > 0)
    {
        ret_val = ex_get_node_set_dist_fact (file.id, set_id, &node_dist_factors [0]);
        if (ret_val < 0) {
          std::string msg = 
            boost::str(boost::format("%s: error: cannot read node set distribution factors %d (%d)") %
                       file.filename % set_id % ret_val);
          Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
        }
    }

    // This call is corrupting memory in optimized versions on some platforms
    // when no node set name is stored with the node set
    // Since amanzi is not using names to identify node sets, it is disabled
    
    char name_data [2*MAX_STR_LENGTH] = "";
    //    ret_val = ex_get_name (file.id, EX_NODE_SET, set_id, name_data);
    //    if (ret_val < 0) {
    //       std::string msg =
    //       boost::str(boost::format("%s: error: cannot read node set name %d (%d)") %
    //                  file.filename % set_id % ret_val);
    //       Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    //    }

    std::string name (name_data);

    // make node indexes 0-based
    std::for_each(node_list.begin(), node_list.end(), bl::_1 -= 1); 

    return AmanziMesh::Data::Node_set::build_from (set_id, node_list, node_dist_factors, name);

}


AmanziMesh::Data::Side_set* read_side_set (Exodus_file file, int set_id)
{

    int num_sides;
    int num_dist_factors;

    int ret_val = ex_get_side_set_param (file.id, set_id, &num_sides, &num_dist_factors);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read side set %d parameters (%d)") %
                   file.filename % set_id % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }

    std::vector<int> element_list(num_sides);
    std::vector<int> side_list(num_sides);

    ret_val = ex_get_side_set (file.id, set_id, &element_list [0], &side_list [0]);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read side set %d (%d)") %
                   file.filename % set_id % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }

    // This call is corrupting memory in optimized versions on some platforms
    // when no side set name is stored for the side set
    // Since amanzi is not using names to identify side sets, it is disabled
    
    char name_data [2*MAX_STR_LENGTH] = "";
    //    ret_val = ex_get_name (file.id, EX_SIDE_SET, set_id, name_data);
    //    if (ret_val < 0) {
    //        std::string msg =
    //        boost::str(boost::format("%s: error: cannot read side set name %d (%d)") %
    //                  file.filename % set_id % ret_val);
    //       Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    //    }
    
    

    std::string name (name_data);

    // make element indexes 0-based
    std::for_each(element_list.begin(), element_list.end(), bl::_1 -= 1); 

    // make side indexes 0-based
    std::for_each(side_list.begin(), side_list.end(), bl::_1 -= 1); 

    return AmanziMesh::Data::Side_set::build_from (set_id, 
                                            element_list, 
                                            side_list,
                                            name
                                            );
    
}


AmanziMesh::Data::Element_block* read_element_block(Exodus_file file, int block_id)
{


    // Block size and type data
    int num_elements;
    int num_nodes_per_element;
    int num_attributes;
    char element_name_data [2*MAX_STR_LENGTH];
    int ret_val = ex_get_elem_block (file.id, block_id, element_name_data,
                                     &num_elements, &num_nodes_per_element, &num_attributes);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read element block %d parameters (%d)") %
                   file.filename % block_id % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }


    // Connectivity Map
    std::vector<int> connectivity_map(num_elements * num_nodes_per_element);
    ret_val = ex_get_elem_conn (file.id, block_id, &connectivity_map [0]);
    if (ret_val < 0) {
      std::string msg = 
        boost::str(boost::format("%s: error: cannot read element block %d connectivity (%d)") %
                   file.filename % block_id % ret_val);
      Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
    }

    // Attribute Map
    std::vector<double> attribute_map;
    if (num_attributes > 0)
    {
        attribute_map.resize (num_elements * num_attributes);
        ret_val = ex_get_elem_attr (file.id, block_id, &attribute_map [0]);
        if (ret_val < 0) {
          std::string msg = 
            boost::str(boost::format("%s: error: cannot read element block %d attributes (%d)") %
                       file.filename % block_id % ret_val);
          Exceptions::amanzi_throw( ExodusError (msg.c_str()) );
        }
    }

    AmanziMesh::Cell_type element_type = read_element_type(element_name_data);

    char element_block_name [2*MAX_STR_LENGTH];
    ret_val = ex_get_name (file.id, EX_ELEM_BLOCK, block_id, element_block_name);

    // make node indexes 0-based
    std::for_each(connectivity_map.begin(), connectivity_map.end(), bl::_1 -= 1); 
    
    return  AmanziMesh::Data::Element_block::build_from (block_id,
                                                  std::string (element_block_name),
                                                  num_elements,
                                                  element_type, 
                                                  connectivity_map,
                                                  attribute_map);

}

AmanziMesh::Data::Data* read_exodus_file (const Exodus_file& file)
{
    
    AmanziMesh::Data::Parameters* parameters = read_parameters (file);

    const int num_nodes = parameters->num_nodes_;
    const int dimensions = parameters->dimensions_;

    AmanziMesh::Data::Coordinates<double>* coords = read_coordinates (file, num_nodes, dimensions);

    // Element blocks:
    std::vector<AmanziMesh::Data::Element_block*> element_blocks (parameters->num_element_blocks_);
    for (int i = 0; i < parameters->num_element_blocks_; ++i)
        element_blocks [i] = read_element_block (file, parameters->element_block_ids_ [i]);

    // Side sets:
    std::vector<AmanziMesh::Data::Side_set*> side_sets (parameters->num_side_sets_);
    for (int i = 0; i < parameters->num_side_sets_; ++i)
        side_sets [i] = read_side_set (file, parameters->side_set_ids_ [i]);

    // Node sets:
    std::vector<AmanziMesh::Data::Node_set*> node_sets (parameters->num_node_sets_);
    for (int i = 0; i < parameters->num_node_sets_; ++i)
        node_sets [i] = read_node_set (file, parameters->node_set_ids_ [i]);

    
    AmanziMesh::Data::Data* mesh = AmanziMesh::Data::Data::build_from (parameters, coords, element_blocks, side_sets, node_sets);

    return mesh;
        

}

AmanziMesh::Cell_type read_element_type (const char * name)
{

    std::string id (name, name+3);
    boost::to_upper(id);

    if (id == "TRI")
        return AmanziMesh::TRI;
    if (id == "QUA")
        return AmanziMesh::QUAD;
    if (id == "TET")
        return AmanziMesh::TET;
    if (id == "PYR")
        return AmanziMesh::PYRAMID;
    if (id == "WED")
        return AmanziMesh::PRISM;
    if (id == "HEX")
        return AmanziMesh::HEX;

    return AmanziMesh::CELLTYPE_UNKNOWN;

}

AmanziMesh::Data::Data* read_exodus_file (const char * filename)
{
  Exodus_file file (filename);
  return read_exodus_file(file);
}

} // namespace Exodus
} // namespace Amanzi

