/*
  Operators 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <vector>

#include "MFD3DFactory.hh"

#include "OperatorDefs.hh"
#include "Operator_Schema.hh"
#include "Op_Cell_Schema.hh"
#include "Op_Face_Schema.hh"
#include "PDE_AdvectionRiemann.hh"

namespace Amanzi {
namespace Operators {

/* ******************************************************************
* Initialize operator from parameter list.
****************************************************************** */
void PDE_AdvectionRiemann::InitAdvection_(Teuchos::ParameterList& plist)
{
  Teuchos::ParameterList& range = plist.sublist("schema range");
  Teuchos::ParameterList& domain = plist.sublist("schema domain");

  if (global_op_ == Teuchos::null) {
    // constructor was given a mesh
    // -- range schema and cvs
    local_schema_row_.Init(range, mesh_);
    global_schema_row_ = local_schema_row_;

    Teuchos::RCP<CompositeVectorSpace> cvs_row = Teuchos::rcp(new CompositeVectorSpace());
    cvs_row->SetMesh(mesh_)->SetGhosted(true);

    for (auto it = global_schema_row_.begin(); it != global_schema_row_.end(); ++it) {
      std::string name(local_schema_row_.KindToString(it->kind));
      cvs_row->AddComponent(name, it->kind, it->num);
    }

    // -- domain schema and cvs
    local_schema_col_.Init(domain, mesh_);
    global_schema_col_ = local_schema_col_;

    Teuchos::RCP<CompositeVectorSpace> cvs_col = Teuchos::rcp(new CompositeVectorSpace());
    cvs_col->SetMesh(mesh_)->SetGhosted(true);

    for (auto it = global_schema_col_.begin(); it != global_schema_col_.end(); ++it) {
      std::string name(local_schema_col_.KindToString(it->kind));
      cvs_col->AddComponent(name, it->kind, it->num);
    }

    global_op_ = Teuchos::rcp(new Operator_Schema(cvs_row, cvs_col, plist, global_schema_row_, global_schema_col_));
    if (local_schema_col_.base() == AmanziMesh::CELL) {
      local_op_ = Teuchos::rcp(new Op_Cell_Schema(global_schema_row_, global_schema_col_, mesh_));
    } else if (local_schema_col_.base() == AmanziMesh::FACE) {
      local_op_ = Teuchos::rcp(new Op_Face_Schema(global_schema_row_, global_schema_col_, mesh_));
    }

  } else {
    // constructor was given an Operator
    global_schema_row_ = global_op_->schema_row();
    global_schema_col_ = global_op_->schema_col();

    mesh_ = global_op_->DomainMap().Mesh();
    local_schema_row_.Init(range, mesh_);
    local_schema_col_.Init(domain, mesh_);

    if (local_schema_col_.base() == AmanziMesh::CELL) {
      local_op_ = Teuchos::rcp(new Op_Cell_Schema(global_schema_row_, global_schema_col_, mesh_));
    } else if (local_schema_col_.base() == AmanziMesh::FACE) {
      local_op_ = Teuchos::rcp(new Op_Face_Schema(global_schema_row_, global_schema_col_, mesh_));
    }
  }

  // register the advection Op
  global_op_->OpPushBack(local_op_);

  // parse discretization parameters
  // -- discretization method
  WhetStone::MFD3DFactory factory;
  auto mfd = factory.Create(mesh_, plist);

  // -- matrices to build
  matrix_ = plist.get<std::string>("matrix type");

  if (factory.method() == "dg modal") {
    space_col_ = DG;
    dg_ = Teuchos::rcp_dynamic_cast<WhetStone::DG_Modal>(mfd);
  } else {
    Errors::Message msg;
    msg << "Advection operator: method \"" << method_ << "\" is invalid.";
    Exceptions::amanzi_throw(msg);
  }

  if (local_schema_row_ == local_schema_col_) { 
    space_row_ = space_col_;
  } else {
    space_row_ = P0;  // FIXME
  }

  // -- fluxes
  flux_ = plist.get<std::string>("flux formula", "Rusavov");
  jump_on_test_ = plist.get<bool>("jump operator on test function", true);
}


/* ******************************************************************
* A simple first-order transport method of the form div (u C), where 
* u is the given velocity field and C is the advected field.
****************************************************************** */
void PDE_AdvectionRiemann::UpdateMatrices(
    const Teuchos::Ptr<const std::vector<WhetStone::Polynomial> >& u)
{
  std::vector<WhetStone::DenseMatrix>& matrix = local_op_->matrices;
  std::vector<WhetStone::DenseMatrix>& matrix_shadow = local_op_->matrices_shadow;

  WhetStone::DenseMatrix Aface;

  if (matrix_ == "flux" && flux_ == "upwind") {
    for (int f = 0; f < nfaces_owned; ++f) {
      dg_->FluxMatrix(f, (*u)[f], Aface, true, jump_on_test_);
      matrix[f] = Aface;
    }
  } else if (matrix_ == "flux" && flux_ == "downwind") {
    for (int f = 0; f < nfaces_owned; ++f) {
      dg_->FluxMatrix(f, (*u)[f], Aface, false, jump_on_test_);
      matrix[f] = Aface;
    }
  } else if (matrix_ == "flux" && flux_ == "Rusanov") {
    AmanziMesh::Entity_ID_List cells;
    for (int f = 0; f < nfaces_owned; ++f) {
      mesh_->face_get_cells(f, AmanziMesh::Parallel_type::ALL, &cells);
      int c1 = cells[0];
      int c2 = (cells.size() == 2) ? cells[1] : c1;
      dg_->FluxMatrixRusanov(f, (*Kc_)[c1], (*Kc_)[c2], (*Kf_)[f], Aface);
      matrix[f] = Aface;
    }
  }
}


/* *******************************************************************
* Apply boundary condition to the local matrices
******************************************************************* */
void PDE_AdvectionRiemann::ApplyBCs(const Teuchos::RCP<BCs>& bc, bool primary)
{
  const std::vector<int>& bc_model = bc->bc_model();
  const std::vector<std::vector<double> >& bc_value = bc->bc_value_vector();
  int nk = bc_value[0].size();

  Epetra_MultiVector& rhs_c = *global_op_->rhs()->ViewComponent("cell", true);

  AmanziMesh::Entity_ID_List cells;

  int dir, d = mesh_->space_dimension();
  std::vector<AmanziGeometry::Point> tau(d - 1);

  // create integration object for all mesh cells
  WhetStone::NumericalIntegration numi(mesh_, false);

  for (int f = 0; f != nfaces_owned; ++f) {
    if (bc_model[f] == OPERATOR_BC_DIRICHLET) {
      mesh_->face_get_cells(f, AmanziMesh::Parallel_type::ALL, &cells);
      int c = cells[0];

      const AmanziGeometry::Point& xf = mesh_->face_centroid(f);
      const AmanziGeometry::Point& normal = mesh_->face_normal(f, false, c, &dir);

      // set polynomial with Dirichlet data
      WhetStone::DenseVector coef(nk);
      for (int i = 0; i < nk; ++i) {
        coef(i) = bc_value[f][i];
      }

      WhetStone::Polynomial pf(d, dg_->order()); 
      pf.SetPolynomialCoefficients(coef);
      pf.set_origin(xf);

      // convert boundary polynomial to space polynomial
      pf.ChangeOrigin(mesh_->cell_centroid(c));
      numi.ChangeBasisRegularToNatural(c, pf);

      // extract coefficients and update right-hand side 
      WhetStone::DenseMatrix& Aface = local_op_->matrices[f];
      int nrows = Aface.NumRows();
      int ncols = Aface.NumCols();

      WhetStone::DenseVector v(nrows), av(ncols);
      pf.GetPolynomialCoefficients(v);

      Aface.Multiply(v, av, false);

      for (int i = 0; i < ncols; ++i) {
        rhs_c[i][c] -= av(i);
      }

      // elliminate matrices from system
      local_op_->matrices_shadow[f] = Aface;
      Aface.PutScalar(0.0);
    }
  } 
}


/* *******************************************************************
* Identify the advected flux of u
******************************************************************* */
void PDE_AdvectionRiemann::UpdateFlux(
    const Teuchos::Ptr<const CompositeVector>& h,
    const Teuchos::Ptr<const CompositeVector>& u,
    const Teuchos::RCP<BCs>& bc, const Teuchos::Ptr<CompositeVector>& flux)
{
  h->ScatterMasterToGhosted("cell");
  
  const Epetra_MultiVector& h_f = *h->ViewComponent("face", true);
  const Epetra_MultiVector& u_f = *u->ViewComponent("face", false);
  Epetra_MultiVector& flux_f = *flux->ViewComponent("face", false);

  flux->PutScalar(0.0);
  for (int f = 0; f < nfaces_owned; ++f) {
    flux_f[0][f] = u_f[0][f] * h_f[0][f];
  }  
}

}  // namespace Operators
}  // namespace Amanzi