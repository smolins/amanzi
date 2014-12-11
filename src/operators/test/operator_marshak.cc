/*
  This is the operators component of the Amanzi code. 

  Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "UnitTest++.h"

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_ParameterXMLFileReader.hpp"

#include "MeshFactory.hh"
#include "Mesh_MSTK.hh"
#include "GMVMesh.hh"

#include "tensor.hh"
#include "mfd3d_diffusion.hh"

#include "LinearOperatorFactory.hh"
#include "OperatorDefs.hh"
#include "Operator.hh"
#include "OperatorDiffusionSurface.hh"
#include "OperatorSource.hh"
#include "UpwindStandard.hh"

const double TemperatureSource = 100.0; 
const double TemperatureFloor = 0.02; 

namespace Amanzi{

class HeatConduction {
 public:
  HeatConduction(Teuchos::RCP<const AmanziMesh::Mesh> mesh) : mesh_(mesh) { 
    CompositeVectorSpace cvs;
    cvs.SetMesh(mesh_);
    cvs.SetGhosted(true);
    cvs.SetComponent("cell", AmanziMesh::CELL, 1);
    cvs.SetOwned(false);
    cvs.AddComponent("face", AmanziMesh::FACE, 1);

    values_ = Teuchos::RCP<CompositeVector>(new CompositeVector(cvs, true));
    derivatives_ = Teuchos::RCP<CompositeVector>(new CompositeVector(cvs, true));
  }
  ~HeatConduction() {};

  // main members
  void UpdateValues(const CompositeVector& u) { 
    const Epetra_MultiVector& uc = *u.ViewComponent("cell", true); 
    const Epetra_MultiVector& values_c = *values_->ViewComponent("cell", true); 

    int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::USED);
    for (int c = 0; c < ncells; c++) {
      values_c[0][c] = std::pow(uc[0][c], 3.0);
    }

    derivatives_->PutScalar(1.0);
  }

  double Conduction(int c, double T) const {
    ASSERT(T > 0.0);
    return T * T * T;
  }

  Teuchos::RCP<CompositeVector> values() { return values_; }
  Teuchos::RCP<CompositeVector> derivatives() { return derivatives_; }
   

 private:
  Teuchos::RCP<const AmanziMesh::Mesh> mesh_;
  Teuchos::RCP<CompositeVector> values_, derivatives_;
};

typedef double(HeatConduction::*ModelUpwindFn)(int c, double T) const; 

}  // namespace Amanzi


double exact(double t, const Amanzi::AmanziGeometry::Point& p) {
  double x = p[0], c = 0.4;
  return std::pow(3 * c * (c * t - x), 1.0 / 3);
}


/* *****************************************************************
* This test replaves tensor and boundary conditions by continuous
* functions. This is a prototype forheat conduction solvers.
* **************************************************************** */
TEST(MASHAK_NONLINEAR_WAVE) {
  using namespace Teuchos;
  using namespace Amanzi;
  using namespace Amanzi::AmanziMesh;
  using namespace Amanzi::AmanziGeometry;
  using namespace Amanzi::Operators;

  Epetra_MpiComm comm(MPI_COMM_WORLD);
  int MyPID = comm.MyPID();

  if (MyPID == 0) std::cout << "\nTest: Simulating nonlinear Marshak wave" << std::endl;

  // read parameter list
  std::string xmlFileName = "test/operator_marshak.xml";
  ParameterXMLFileReader xmlreader(xmlFileName);
  ParameterList plist = xmlreader.getParameters();

  // create an MSTK mesh framework
  ParameterList region_list = plist.get<Teuchos::ParameterList>("Regions");
  GeometricModelPtr gm = new GeometricModel(2, region_list, &comm);

  FrameworkPreference pref;
  pref.clear();
  pref.push_back(MSTK);
  pref.push_back(STKMESH);

  MeshFactory meshfactory(&comm);
  meshfactory.preference(pref);
  // RCP<const Mesh> mesh = meshfactory(0.0, 0.0, 3.0, 1.0, 200, 10, gm);
  RCP<const Mesh> mesh = meshfactory("test/marshak.exo", gm);

  /* modify diffusion coefficient */
  std::vector<WhetStone::Tensor> K;
  int ncells_owned = mesh->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nfaces_wghost = mesh->num_entities(AmanziMesh::FACE, AmanziMesh::USED);

  for (int c = 0; c < ncells_owned; c++) {
    WhetStone::Tensor Kc(2, 1);
    Kc(0, 0) = 1.0;
    K.push_back(Kc);
  }
  double rho(1.0), mu(1.0);

  // create boundary data (no mixed bc)
  std::vector<int> bc_model(nfaces_wghost, OPERATOR_BC_NONE);
  std::vector<double> bc_value(nfaces_wghost);
  std::vector<double> bc_mixed;

  for (int f = 0; f < nfaces_wghost; f++) {
    const Point& xf = mesh->face_centroid(f);

    if (fabs(xf[1]) < 1e-6 || fabs(xf[1] - 1.0) < 1e-6) {
      bc_model[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_value[f] = 0.0;
    } else if(fabs(xf[0]) < 1e-6) {
      bc_model[f] = Operators::OPERATOR_BC_DIRICHLET;
      bc_value[f] = TemperatureSource;
    } else if(fabs(xf[0] - 1.0) < 1e-6) {
      bc_model[f] = Operators::OPERATOR_BC_DIRICHLET;
      bc_value[f] = TemperatureFloor;
    }
  }
  Teuchos::RCP<BCs> bc = Teuchos::rcp(new BCs(OPERATOR_BC_TYPE_FACE, bc_model, bc_value, bc_mixed));

  // create solution map.
  Teuchos::RCP<CompositeVectorSpace> cvs = Teuchos::rcp(new CompositeVectorSpace());
  cvs->SetMesh(mesh);
  cvs->SetGhosted(true);
  cvs->SetComponent("cell", AmanziMesh::CELL, 1);
  cvs->SetOwned(false);
  cvs->AddComponent("face", AmanziMesh::FACE, 1);

  // create and initialize state variables.
  Teuchos::RCP<CompositeVector> flux = Teuchos::rcp(new CompositeVector(*cvs));
  Epetra_MultiVector& flx = *flux->ViewComponent("face", true);

  Point velocity(0.0, 0.0);
  for (int f = 0; f < nfaces_wghost; f++) {
    const Point& normal = mesh->face_normal(f);
    flx[0][f] = velocity * normal;
  }

  CompositeVector solution(*cvs);
  solution.PutScalar(TemperatureFloor);  // solution at time T=0

  CompositeVector heat_capacity(*cvs);
  heat_capacity.PutScalar(1.0);

  // Create nonlinear coefficient.
  Teuchos::RCP<HeatConduction> knc = Teuchos::rcp(new HeatConduction(mesh));

  // Create upwind model
  ParameterList& ulist = plist.sublist("PK operator").sublist("upwind");
  UpwindStandard<HeatConduction> upwind(mesh, knc);
  upwind.Init(ulist);

  // MAIN LOOP
  int step(0);
  double T(0.0), dT(1e-4);
  while (T < 1.0) {
    solution.ScatterMasterToGhosted();

    // update bc
    for (int f = 0; f < nfaces_wghost; f++) {
      const Point& xf = mesh->face_centroid(f);
      if(fabs(xf[0]) < 1e-6) bc_value[f] = exact(T + dT, xf);
    }

    // upwind heat conduction coefficient
    knc->UpdateValues(solution);
    ModelUpwindFn func = &HeatConduction::Conduction;
    upwind.Compute(*flux, bc_model, bc_value, *knc->values(), *knc->values(), func);

    // add diffusion operator
    Teuchos::ParameterList olist = plist.sublist("PK operator").sublist("diffusion operator");
    Teuchos::RCP<OperatorDiffusion> op = Teuchos::rcp(new OperatorDiffusion(cvs, olist, bc));
    op->Init();

    int schema_dofs = op->schema_dofs();
    int schema_prec_dofs = op->schema_prec_dofs();

    op->InitOperator(K, knc->values(), knc->derivatives(), rho, mu);
    op->UpdateMatrices(flux, Teuchos::null);
    op->ApplyBCs();
    op->SymbolicAssembleMatrix(schema_prec_dofs);
    op->AssembleMatrix(schema_prec_dofs);

    // add accumulation terms
    op->AddAccumulationTerm(solution, heat_capacity, dT, "cell");

    // create preconditoner
    ParameterList slist = plist.get<Teuchos::ParameterList>("Preconditioners");
    op->InitPreconditioner("Hypre AMG", slist);

    // solve the problem
    ParameterList lop_list = plist.get<Teuchos::ParameterList>("Solvers");
    AmanziSolvers::LinearOperatorFactory<OperatorDiffusion, CompositeVector, CompositeVectorSpace> factory;
    Teuchos::RCP<AmanziSolvers::LinearOperator<OperatorDiffusion, CompositeVector, CompositeVectorSpace> >
       solver = factory.Create("Amanzi GMRES", lop_list, op);

    Epetra_MultiVector& sol_new = *solution.ViewComponent("cell");
    Epetra_MultiVector sol_old(sol_new);

    CompositeVector rhs = *op->rhs();
    solver->add_criteria(AmanziSolvers::LIN_SOLVER_MAKE_ONE_ITERATION);
    int ierr = solver->ApplyInverse(rhs, solution);

    step++;
    T += dT;

    double snorm;
    solution.ViewComponent("cell")->Norm2(&snorm);

    if (MyPID == 0) {
      printf("%3d  ||r||=%11.6g  itr=%2d  ||sol||=%11.6g  T=%7.4f  dT=%7.4f\n",
          step, solver->residual(), solver->num_itrs(), snorm, T, dT);
    }

    // change time step
    Epetra_MultiVector sol_diff(sol_old);
    sol_diff.Update(1.0, sol_new, -1.0);

    double ds_max, ds_rel(0.0);
    for (int c = 0; c < ncells_owned; c++) {
      ds_rel = std::max(ds_rel, sol_diff[0][c] / (1e-3 + sol_old[0][c] + sol_new[0][c]));
    }
    if (ds_rel < 0.05) {
      dT *= 1.2;
    } else if (ds_rel > 0.10) {
      dT *= 0.8;
    }
  }

  if (MyPID == 0) {
    // visualization
    const Epetra_MultiVector& p = *solution.ViewComponent("cell");
    GMV::open_data_file(*mesh, (std::string)"operators.gmv");
    GMV::start_data();
    GMV::write_cell_data(p, 0, "solution");
    GMV::close_data_file();
  }
}

