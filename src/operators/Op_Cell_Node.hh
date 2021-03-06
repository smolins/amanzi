/*
  Operators

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Ethan Coon (ecoon@lanl.gov)
*/

#ifndef AMANZI_OP_CELL_NODE_HH_
#define AMANZI_OP_CELL_NODE_HH_

#include <vector>
#include "DenseMatrix.hh"
#include "Operator.hh"
#include "Op.hh"

/*
  Op classes are small structs that play two roles:

  1. They provide a class name to the schema, enabling visitor patterns.
  2. They are a container for local matrices.
  
  This Op class is for storing local matrices of length ncells and with dofs
  on nodes, e.g. Lagrange elements.
*/

namespace Amanzi {
namespace Operators {

class Op_Cell_Node : public Op {
 public:
  Op_Cell_Node(std::string& name,
               const Teuchos::RCP<const AmanziMesh::Mesh> mesh) :
      Op(OPERATOR_SCHEMA_BASE_CELL |
         OPERATOR_SCHEMA_DOFS_NODE, name, mesh) {
    WhetStone::DenseMatrix null_matrix;
    matrices.resize(mesh->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED), null_matrix);
    matrices_shadow = matrices;
  }

  virtual void ApplyMatrixFreeOp(const Operator* assembler,
          const CompositeVector& X, CompositeVector& Y) const {
    assembler->ApplyMatrixFreeOp(*this, X, Y);
  }

  virtual void SymbolicAssembleMatrixOp(const Operator* assembler,
          const SuperMap& map, GraphFE& graph,
          int my_block_row, int my_block_col) const {
    assembler->SymbolicAssembleMatrixOp(*this, map, graph, my_block_row, my_block_col);
  }

  virtual void AssembleMatrixOp(const Operator* assembler,
          const SuperMap& map, MatrixFE& mat,
          int my_block_row, int my_block_col) const {
    assembler->AssembleMatrixOp(*this, map, mat, my_block_row, my_block_col);
  }
  
  virtual bool ApplyBC(BCs& bc,
                       const Teuchos::Ptr<CompositeVector>& rhs,                       
                       bool bc_previously_applied) {
    ASSERT(0);
    return false;
  }

  // rescaling columns of local matrices
  virtual void Rescale(const CompositeVector& scaling) {
    if (scaling.HasComponent("node")) {
      const Epetra_MultiVector& s_n = *scaling.ViewComponent("node", true);
      AmanziMesh::Entity_ID_List nodes;

      for (int c = 0; c != matrices.size(); ++c) {
        WhetStone::DenseMatrix& Acell = matrices[c];
        mesh_->cell_get_nodes(c, &nodes);

        for (int n = 0; n != nodes.size(); ++n) {
          for (int m = 0; m != nodes.size(); ++m) {
            Acell(n, m) *= s_n[0][nodes[n]];
          }
        }
      }
    }
  }
};

}  // namespace Operators
}  // namespace Amanzi


#endif


