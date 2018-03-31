/*
  Operators

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Ethan Coon
*/

#ifndef AMANZI_OPERATORS_BC_FACTORY_HH_
#define AMANZI_OPERATORS_BC_FACTORY_HH_

#include "BCs.hh"

namespace Amanzi {
namespace Operators {

class BCs_Factory {
 public:
  BCs_Factory() {};

  void set_mesh(const Teuchos::RCP<const AmanziMesh::Mesh>& mesh) { mesh_ = mesh; }
  void set_kind(AmanziMesh::Entity_kind kind) { kind_ = kind; }
  void set_type(int type) { type_ = type; }

  Teuchos::RCP<BCs> Create() const {
    return Teuchos::rcp(new BCs(mesh_, kind_, type_));
  }
  
 private:
  Teuchos::RCP<const AmanziMesh::Mesh> mesh_;
  AmanziMesh::Entity_kind kind_;
  int type_;
};

}  // namespace Operators
}  // namespace Amanzi


#endif