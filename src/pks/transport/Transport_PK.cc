/*
  This is the Transport component of Amanzi. 

  Copyright 2010-2013 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <algorithm>
#include <vector>

#include "Epetra_Vector.h"
#include "Epetra_IntVector.h"
#include "Epetra_MultiVector.h"
#include "Epetra_Import.h"
#include "Teuchos_RCP.hpp"

#include "BCs.hh"
#include "errors.hh"
#include "Explicit_TI_RK.hh"
#include "GMVMesh.hh"
#include "LinearOperatorDefs.hh"
#include "LinearOperatorFactory.hh"
#include "Mesh.hh"
#include "OperatorDefs.hh"
#include "OperatorDiffusionFactory.hh"
#include "OperatorDiffusion.hh"
#include "OperatorAccumulation.hh"

#include "Transport_PK.hh"

namespace Amanzi {
namespace Transport {

/* ******************************************************************
* New constructor compatible with new MPC framework.
****************************************************************** */
Transport_PK::Transport_PK(Teuchos::ParameterList& pk_tree,
                           const Teuchos::RCP<Teuchos::ParameterList>& glist,
                           const Teuchos::RCP<State>& S,
                           const Teuchos::RCP<TreeVector>& soln) :
    glist_(glist),
    S_(S),
    soln_(soln)
{
  std::string pk_name = pk_tree.name();
  const char* result = pk_name.data();
  while ((result = std::strstr(result, "->")) != NULL) {
    result += 2;
    pk_name = result;   
  }

  if (glist_->isSublist("Cycle Driver")) {
    if (glist_->sublist("Cycle Driver").isParameter("component names")) {
      // grab the component names
      component_names_ = glist_->sublist("Cycle Driver")
          .get<Teuchos::Array<std::string> >("component names").toVector();
    } else {
      Errors::Message msg("Transport PK: parameter component names is missing.");
      Exceptions::amanzi_throw(msg);
    }
  } else {
    Errors::Message msg("Transport PK: sublist Cycle Driver is missing.");
    Exceptions::amanzi_throw(msg);
  }

  // Create miscaleneous lists.
  Teuchos::RCP<Teuchos::ParameterList> pk_list = Teuchos::sublist(glist, "PKs", true);
  tp_list_ = Teuchos::sublist(pk_list, pk_name, true);

  preconditioner_list_ = Teuchos::sublist(glist, "Preconditioners");
  linear_solver_list_ = Teuchos::sublist(glist, "Solvers");
  nonlinear_solver_list_ = Teuchos::sublist(glist, "Nonlinear solvers");

  subcycling_ = tp_list_->get<bool>("transport subcycling", true);
   
  vo_ = NULL;
}


/* ******************************************************************
* Old constructor for unit tests.
****************************************************************** */
Transport_PK::Transport_PK(const Teuchos::RCP<Teuchos::ParameterList>& glist,
                           Teuchos::RCP<State> S, 
                           const std::string& pk_list_name,
                           std::vector<std::string>& component_names) :
    S_(S),
    component_names_(component_names)
{
  // Create miscaleneous lists.
  Teuchos::RCP<Teuchos::ParameterList> pk_list = Teuchos::sublist(glist, "PKs", true);
  tp_list_ = Teuchos::sublist(pk_list, pk_list_name, true);

  preconditioner_list_ = Teuchos::sublist(glist, "Preconditioners");
  linear_solver_list_ = Teuchos::sublist(glist, "Solvers");
  nonlinear_solver_list_ = Teuchos::sublist(glist, "Nonlinear solvers");

  vo_ = NULL;
}


/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
Transport_PK::~Transport_PK()
{ 
  if (vo_ != NULL) {
    delete vo_;
  }
  for (int i = 0; i < bcs.size(); i++) {
    if (bcs[i] != NULL) delete bcs[i]; 
  }
  for (int i = 0; i < srcs.size(); i++) {
    if (srcs[i] != NULL) delete srcs[i]; 
  }
}


/* ******************************************************************
* Setup for Alquimia.
****************************************************************** */
#ifdef ALQUIMIA_ENABLED
void Transport_PK::SetupAlquimia(Teuchos::RCP<AmanziChemistry::Chemistry_State> chem_state,
                                 Teuchos::RCP<AmanziChemistry::ChemistryEngine> chem_engine)
{
  chem_state_ = chem_state;
  chem_engine_ = chem_engine;

  if (chem_engine_ != Teuchos::null) {
    // Retrieve the component names from the chemistry engine.
    std::vector<std::string> component_names;
    chem_engine_->GetPrimarySpeciesNames(component_names);
    component_names_ = component_names;
  }
}
#endif


/* ******************************************************************
* Define structure of this PK.
****************************************************************** */
void Transport_PK::Setup()
{
  passwd_ = "state";  // owner's password

  mesh_ = S_->GetMesh();
  dim = mesh_->space_dimension();

  // require state variables when Flow is off
  if (!S_->HasField("permeability")) {
    S_->RequireField("permeability", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, dim);
  }
  if (!S_->HasField("darcy_flux")) {
    S_->RequireField("darcy_flux", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("face", AmanziMesh::FACE, 1);
  }
  if (!S_->HasField("saturation_liquid")) {
    S_->RequireField("saturation_liquid", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S_->HasField("prev_saturation_liquid")) {
    S_->RequireField("prev_saturation_liquid", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
    S_->GetField("prev_saturation_liquid", passwd_)->set_io_vis(false);
  }

  // require state variables when Transport is on
  if (!S_->HasField("total_component_concentration")) {
    std::vector<std::vector<std::string> > subfield_names(1);
    int ncomponents = component_names_.size();
    for (int i = 0; i != ncomponents; ++i) {
      subfield_names[0].push_back(component_names_[i]);
    }

    S_->RequireField("total_component_concentration", passwd_, subfield_names)->SetMesh(mesh_)
      ->SetGhosted(true)->SetComponent("cell", AmanziMesh::CELL, ncomponents);
  }

  // testing evaluators
  if (!S_->HasField("porosity")) {
    S_->RequireField("porosity", "porosity")->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
    S_->RequireFieldEvaluator("porosity");
  }
}


/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
void Transport_PK::Initialize()
{
  // Set initial values for transport variables.
  dt_ = dt_debug_ = t_physics_ = 0.0;
  double time = S_->time();
  if (time >= 0.0) t_physics_ = time;

  dispersion_models_ = 0; 
  dispersion_preconditioner = "identity";

  internal_tests = 0;
  tests_tolerance = TRANSPORT_CONCENTRATION_OVERSHOOT;

  bc_scaling = 0.0;

  // Create verbosity object.
  Teuchos::ParameterList vlist;
  vlist.sublist("VerboseObject") = tp_list_->sublist("VerboseObject");
  vo_ = new VerboseObject("TransportPK", vlist); 

  MyPID = mesh_->get_comm()->MyPID();

  // initialize missed fields
  InitializeFields_();

  // Check input parameters. Due to limited checks, we can do it earlier.
  Policy(S_.ptr());

  ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  ncells_wghost = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::USED);

  nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);

  nnodes_wghost = mesh_->num_entities(AmanziMesh::NODE, AmanziMesh::USED);

  // extract control parameters
  ProcessParameterList();
 
  // state pre-prosessing
  Teuchos::RCP<const CompositeVector> cv;
  S_->GetFieldData("darcy_flux")->ScatterMasterToGhosted("face");
  cv = S_->GetFieldData("darcy_flux");
  darcy_flux = cv->ViewComponent("face", true);

  cv = S_->GetFieldData("saturation_liquid");
  ws = cv->ViewComponent("cell", false);

  cv = S_->GetFieldData("prev_saturation_liquid");
  ws_prev = cv->ViewComponent("cell", false);

  cv = S_->GetFieldData("porosity");
  phi = cv->ViewComponent("cell", false);

  tcc = S_->GetFieldData("total_component_concentration", passwd_);

  // memory for new components
  tcc_tmp = Teuchos::rcp(new CompositeVector(*(S_->GetFieldData("total_component_concentration"))));
  *tcc_tmp = *tcc;

  // upwind 
  const Epetra_Map& fmap_wghost = mesh_->face_map(true);
  upwind_cell_ = Teuchos::rcp(new Epetra_IntVector(fmap_wghost));
  downwind_cell_ = Teuchos::rcp(new Epetra_IntVector(fmap_wghost));

  IdentifyUpwindCells();

  // advection block initialization
  current_component_ = -1;

  const Epetra_Map& cmap_owned = mesh_->cell_map(false);
  ws_subcycle_start = Teuchos::rcp(new Epetra_Vector(cmap_owned));
  ws_subcycle_end = Teuchos::rcp(new Epetra_Vector(cmap_owned));

  // reconstruction initialization
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  lifting_ = Teuchos::rcp(new Operators::ReconstructionCell(mesh_));

  // boundary conditions initialization
  time = t_physics_;
  for (int i = 0; i < bcs.size(); i++) {
    bcs[i]->Compute(time);
  }

  VV_CheckInfluxBC();

  // source term initialization
  for (int i =0; i < srcs.size(); i++) {
    int distribution = srcs[i]->CollectActionsList();
    if (distribution & CommonDefs::DOMAIN_FUNCTION_ACTION_DISTRIBUTE_PERMEABILITY) {
      Kxy = Teuchos::rcp(new Epetra_Vector(mesh_->cell_map(false)));
      Errors::Message msg;
      msg << "Transport PK: missing capability: permeability-based source distribution.\n";
      Exceptions::amanzi_throw(msg);  
      break;
    }
  }
}


/* ******************************************************************
* Initalized fields left by State and other PKs.
****************************************************************** */
void Transport_PK::InitializeFields_()
{
  // set popular default values when flow PK is off
  if (S_->HasField("saturation_liquid")) {
    if (S_->GetField("saturation_liquid")->owner() == passwd_) {
      if (!S_->GetField("saturation_liquid", passwd_)->initialized()) {
        S_->GetFieldData("saturation_liquid", passwd_)->PutScalar(1.0);
        S_->GetField("saturation_liquid", passwd_)->set_initialized();
      }
    }
  }

  if (S_->HasField("prev_saturation_liquid")) {
    if (S_->GetField("prev_saturation_liquid")->owner() == passwd_) {
      if (!S_->GetField("prev_saturation_liquid", passwd_)->initialized()) {
        *S_->GetFieldData("prev_saturation_liquid", passwd_) = *S_->GetFieldData("saturation_liquid", passwd_);
        S_->GetField("prev_saturation_liquid", passwd_)->set_initialized();
      }
    }
  }
}


/* *******************************************************************
* Estimation of the time step based on T.Barth (Lecture Notes   
* presented at VKI Lecture Series 1994-05, Theorem 4.2.2.       
* Routine must be called every time we update a flow field.
*
* Warning: Barth calculates influx, we calculate outflux. The methods
* are equivalent for divergence-free flows and gurantee EMP. Outflux 
* takes into account sinks and sources but preserves only positivity
* of an advected mass.
* ***************************************************************** */
double Transport_PK::CalculateTransportDt()
{
  S_->GetFieldData("darcy_flux")->ScatterMasterToGhosted("face");

  IdentifyUpwindCells();

  // loop over faces and accumulate upwinding fluxes
  std::vector<double> total_outflux(ncells_wghost, 0.0);

  for (int f = 0; f < nfaces_wghost; f++) {
    int c = (*upwind_cell_)[f];
    if (c >= 0) total_outflux[c] += fabs((*darcy_flux)[0][f]);
  }

  // loop over cells and calculate minimal time step
  double vol, outflux, dt_cell;
  dt_ = dt_cell = TRANSPORT_LARGE_TIME_STEP;
  int cmin_dt = 0;
  for (int c = 0; c < ncells_owned; c++) {
    outflux = total_outflux[c];
    if (outflux) {
      vol = mesh_->cell_volume(c);
      dt_cell = vol * (*phi)[0][c] * std::min((*ws_prev)[0][c], (*ws)[0][c]) / outflux;
    }
    if (dt_cell < dt_) {
      dt_ = dt_cell;
      cmin_dt = c;
    }
  }
  if (spatial_disc_order == 2) dt_ /= 2;

  // communicate global time step
  double dt_tmp = dt_;
#ifdef HAVE_MPI
  const Epetra_Comm& comm = ws_prev->Comm();
  comm.MinAll(&dt_tmp, &dt_, 1);
#endif

  // incorporate developers and CFL constraints
  dt_ = std::min(dt_, dt_debug_);
  dt_ *= cfl_;

  // print optional diagnostics using maximum cell id as the filter
  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) {
    int cmin_dt_unique = (fabs(dt_tmp * cfl_ - dt_) < 1e-6 * dt_) ? cmin_dt : -1;
 
#ifdef HAVE_MPI
    int cmin_dt_tmp = cmin_dt_unique;
    comm.MaxAll(&cmin_dt_tmp, &cmin_dt_unique, 1);
#endif
    if (cmin_dt == cmin_dt_unique) {
      const AmanziGeometry::Point& p = mesh_->cell_centroid(cmin_dt);

      Teuchos::OSTab tab = vo_->getOSTab();
      *vo_->os() << "cell " << cmin_dt << " has smallest dt, (" << p[0] << ", " << p[1];
      if (p.dim() == 3) *vo_->os() << ", " << p[2];
      *vo_->os() << ")" << std::endl;
    }
  }
  return dt_;
}


/* ******************************************************************* 
* Estimate returns last time step unless it is zero.     
******************************************************************* */
double Transport_PK::get_dt()
{
  if (subcycling_) {
    return 1e+99;
  } else {
    CalculateTransportDt();
    return dt_;
  }
}


/* ******************************************************************* 
* MPC will call this function to advance the transport state.
* Efficient subcycling requires to calculate an intermediate state of
* saturation only once, which leads to a leap-frog-type algorithm.
******************************************************************* */
bool Transport_PK::AdvanceStep(double t_old, double t_new, bool reinit)
{ 
  bool failed = false;
  double dt_MPC = t_new - t_old;

  S_->set_intermediate_time(t_old);

  // We use original tcc and make a copy of it later if needed.
  tcc = S_->GetFieldData("total_component_concentration", passwd_);
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");

  // calculate stable time step
  double dt_shift = 0.0, dt_global = dt_MPC;
  double time = S_->intermediate_time();
  if (time >= 0.0) { 
    t_physics_ = time;
    dt_shift = S_->initial_time() - time;
    dt_global = S_->final_time() - S_->initial_time();
  }

  CalculateTransportDt();
  double dt_original = dt_;  // advance routines override dt_
  int interpolate_ws = (dt_ < dt_global) ? 1 : 0;

  // start subcycling
  double dt_sum = 0.0;
  double dt_cycle;
  if (interpolate_ws) {
    dt_cycle = dt_original;
    InterpolateCellVector(*ws_prev, *ws, dt_shift, dt_global, *ws_subcycle_start);
  } else {
    dt_cycle = dt_MPC;
    ws_start = ws_prev;
    ws_end = ws;
  }

  int ncycles = 0, swap = 1;
  while (dt_sum < dt_MPC) {
    // update boundary conditions
    time = t_physics_ + dt_cycle / 2;
    for (int i = 0; i < bcs.size(); i++) bcs[i]->Compute(time);
    
    double dt_try = dt_MPC - dt_sum;
    double tol = 1e-14 * (dt_try + dt_original); 
    bool final_cycle = false;
    if (dt_try >= 2 * dt_original) {
      dt_cycle = dt_original;
    } else if (dt_try > dt_original + tol) { 
      dt_cycle = dt_try / 2; 
    } else {
      dt_cycle = dt_try;
      final_cycle = true;
    }

    t_physics_ += dt_cycle;
    dt_sum += dt_cycle;

    if (interpolate_ws) {
      if (swap) {  // Initial water saturation is in 'start'.
        ws_start = ws_subcycle_start;
        ws_end = ws_subcycle_end;

        double dt_int = dt_sum + dt_shift;
        InterpolateCellVector(*ws_prev, *ws, dt_int, dt_global, *ws_subcycle_end);
      } else {  // Initial water saturation is in 'end'.
        ws_start = ws_subcycle_end;
        ws_end = ws_subcycle_start;

        double dt_int = dt_sum + dt_shift;
        InterpolateCellVector(*ws_prev, *ws, dt_int, dt_global, *ws_subcycle_start);
      }
      swap = 1 - swap;
    }

    if (spatial_disc_order == 1) {  // temporary solution (lipnikov@lanl.gov)
      AdvanceDonorUpwind(dt_cycle);
    } else if (spatial_disc_order == 2 && temporal_disc_order == 1) {
      AdvanceSecondOrderUpwindRK1(dt_cycle);
    } else if (spatial_disc_order == 2 && temporal_disc_order == 2) {
      AdvanceSecondOrderUpwindRK2(dt_cycle);
    }

    if (! final_cycle) {  // rotate concentrations (we need new memory for tcc)
      tcc = Teuchos::RCP<CompositeVector>(new CompositeVector(*tcc_tmp));
    }

    ncycles++;
  }

  dt_ = dt_original;  // restore the original time step (just in case)

  // We define tracer as the species #0 as calculate some statistics.
  int num_components = tcc_prev.NumVectors();
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", false);

  bool flag_dispersion = (dispersion_models_ != TRANSPORT_DISPERSIVITY_MODEL_NULL);
  bool flag_diffusion(false);
  for (int i = 0; i < 2; i++) {
    if (diffusion_phase_[i] != Teuchos::null) {
      if (diffusion_phase_[i]->values().size() != 0) flag_diffusion = true;
    }
  }
  if (flag_diffusion) {
    // no molecular diffusion if all tortuosities are zero.
    double tau(0.0);
    for (int i = 0; i < mat_properties_.size(); i++) {
      tau += mat_properties_[i]->tau[0] + mat_properties_[i]->tau[1];
    }
    if (tau == 0.0) flag_diffusion = false;
  }

  if (flag_dispersion || flag_diffusion) {
    Teuchos::ParameterList op_list;
    op_list.set<std::string>("discretization secondary", "mfd: two-point flux approximation");

    if (mesh_->mesh_type() == AmanziMesh::RECTANGULAR) {
      op_list.set<std::string>("discretization primary", "mfd: monotone for hex");
      Teuchos::Array<std::string> stensil(2);
      stensil[0] = "face";
      stensil[1] = "cell";
      op_list.set<Teuchos::Array<std::string> >("schema", stensil);
      stensil.remove(1);
      op_list.set<Teuchos::Array<std::string> >("preconditioner schema", stensil);
    } else {
      op_list.set<std::string>("discretization primary", "mfd: two-point flux approximation");
      Teuchos::Array<std::string> stensil(1);
      stensil[0] = "cell";
      op_list.set<Teuchos::Array<std::string> >("schema", stensil);
    }

    // default boundary conditions (none inside domain and Neumann on its boundary)
    std::vector<int> bc_model(nfaces_wghost, Operators::OPERATOR_BC_NONE);
    std::vector<double> bc_value(nfaces_wghost, 0.0);
    std::vector<double> bc_mixed;
    PopulateBoundaryData(bc_model, bc_value, -1);

    Teuchos::RCP<Operators::BCs> bc_dummy = 
        Teuchos::rcp(new Operators::BCs(Operators::OPERATOR_BC_TYPE_FACE, bc_model, bc_value, bc_mixed));
    AmanziGeometry::Point g;

    Operators::OperatorDiffusionFactory opfactory;
    Teuchos::RCP<Operators::OperatorDiffusion> op1 = opfactory.Create(mesh_, bc_dummy, op_list, g, 0);
    op1->SetBCs(bc_dummy, bc_dummy);
    Teuchos::RCP<Operators::Operator> op = op1->global_operator();
    Teuchos::RCP<Operators::OperatorAccumulation> op2 =
        Teuchos::rcp(new Operators::OperatorAccumulation(AmanziMesh::CELL, op));

    const CompositeVectorSpace& cvs = op1->global_operator()->DomainMap();
    CompositeVector sol(cvs), factor(cvs), factor0(cvs), source(cvs), zero(cvs);
    zero.PutScalar(0.0);
  
    // instantiale solver
    AmanziSolvers::LinearOperatorFactory<Operators::Operator, CompositeVector, CompositeVectorSpace> sfactory;
    Teuchos::RCP<AmanziSolvers::LinearOperator<Operators::Operator, CompositeVector, CompositeVectorSpace> >
        solver = sfactory.Create(dispersion_solver, *linear_solver_list_, op);

    solver->add_criteria(AmanziSolvers::LIN_SOLVER_MAKE_ONE_ITERATION);  // Make at least one iteration

    // populate the dispersion operator (if any)
    if (flag_dispersion) {
      CalculateDispersionTensor_(*darcy_flux, *phi, *ws);
    }

    int phase, num_itrs(0);
    bool flag_op1(true);
    double md_change, md_old(0.0), md_new, residual(0.0);

    // Disperse and diffuse aqueous components
    for (int i = 0; i < num_aqueous; i++) {
      FindDiffusionValue(component_names_[i], &md_new, &phase);
      md_change = md_new - md_old;
      md_old = md_new;

      if (md_change != 0.0) {
        CalculateDiffusionTensor_(md_change, phase, *phi, *ws);
        flag_op1 = true;
      }

      // set initial guess
      Epetra_MultiVector& sol_cell = *sol.ViewComponent("cell");
      for (int c = 0; c < ncells_owned; c++) {
        sol_cell[0][c] = tcc_next[i][c];
      }
      if (sol.HasComponent("face")) {
        sol.ViewComponent("face")->PutScalar(0.0);
      }

      if (flag_op1) {
        op->Init();
        Teuchos::RCP<std::vector<WhetStone::Tensor> > Dptr = Teuchos::rcpFromRef(D);
        op1->Setup(Dptr, Teuchos::null, Teuchos::null);
        op1->UpdateMatrices(Teuchos::null, Teuchos::null);

        // add accumulation term
        Epetra_MultiVector& fac = *factor.ViewComponent("cell");
        for (int c = 0; c < ncells_owned; c++) {
          fac[0][c] = (*phi)[0][c] * (*ws)[0][c];
        }
        op2->AddAccumulationTerm(sol, factor, dt_MPC, "cell");
 
        op1->ApplyBCs(true, true);
        op->SymbolicAssembleMatrix();
        op->AssembleMatrix();
        op->InitPreconditioner(dispersion_preconditioner, *preconditioner_list_);
      } else {
        Epetra_MultiVector& rhs_cell = *op->rhs()->ViewComponent("cell");
        for (int c = 0; c < ncells_owned; c++) {
          double tmp = mesh_->cell_volume(c) * (*ws)[0][c] * (*phi)[0][c] / dt_MPC;
          rhs_cell[0][c] = tcc_next[i][c] * tmp;
        }
      }
  
      CompositeVector& rhs = *op->rhs();
      int ierr = solver->ApplyInverse(rhs, sol);

      if (ierr < 0) {
        Errors::Message msg;
        msg = solver->DecodeErrorCode(ierr);
        Exceptions::amanzi_throw(msg);
      }

      residual += solver->residual();
      num_itrs += solver->num_itrs();

      for (int c = 0; c < ncells_owned; c++) {
        tcc_next[i][c] = sol_cell[0][c];
      }
    }

    // Diffuse gaseous components. We ignore dispersion 
    // tensor (D is reset). Inactive cells (s[c] = 1 and D[c] = 0) 
    // are treated with a hack of the accumulation term.
    D.clear();
    md_old = 0.0;
    for (int i = num_aqueous; i < num_components; i++) {
      FindDiffusionValue(component_names_[i], &md_new, &phase);
      md_change = md_new - md_old;
      md_old = md_new;

      if (md_change != 0.0) {
        CalculateDiffusionTensor_(md_change, phase, *phi, *ws);
      }

      // set initial guess
      Epetra_MultiVector& sol_cell = *sol.ViewComponent("cell");
      for (int c = 0; c < ncells_owned; c++) {
        sol_cell[0][c] = tcc_next[i][c];
      }
      if (sol.HasComponent("face")) {
        sol.ViewComponent("face")->PutScalar(0.0);
      }

      op->Init();
      Teuchos::RCP<std::vector<WhetStone::Tensor> > Dptr = Teuchos::rcpFromRef(D);
      op1->Setup(Dptr, Teuchos::null, Teuchos::null);
      op1->UpdateMatrices(Teuchos::null, Teuchos::null);

      // add boundary conditions and sources for gaseous components
      PopulateBoundaryData(bc_model, bc_value, i);

      Epetra_MultiVector& rhs_cell = *op->rhs()->ViewComponent("cell");
      double time = t_physics_ + dt_MPC;
      ComputeAddSourceTerms(time, 1.0, srcs, rhs_cell, i, i);
      op1->ApplyBCs(true, true);

      // add accumulation term
      Epetra_MultiVector& fac1 = *factor.ViewComponent("cell");
      Epetra_MultiVector& fac0 = *factor0.ViewComponent("cell");

      for (int c = 0; c < ncells_owned; c++) {
        fac1[0][c] = (*phi)[0][c] * (1.0 - (*ws)[0][c]);
        fac0[0][c] = (*phi)[0][c] * (1.0 - (*ws_prev)[0][c]);
        if ((*ws)[0][c] == 1.0) fac1[0][c] = 1.0;  // hack so far
      }
      op2->AddAccumulationTerm(sol, factor0, factor, dt_MPC, "cell");
 
      op->SymbolicAssembleMatrix();
      op->AssembleMatrix();
      op->InitPreconditioner(dispersion_preconditioner, *preconditioner_list_);
  
      CompositeVector& rhs = *op->rhs();
      int ierr = solver->ApplyInverse(rhs, sol);

      // if (ierr != 0) {
      //   // Errors::Message msg;
      //   // msg << "\nLinear solver returned an unrecoverable error code.\n";
      //   // Exceptions::amanzi_throw(msg);
      // }
      if (ierr < 0) {
        Errors::Message msg;
        switch(ierr){
        case  Amanzi::AmanziSolvers::LIN_SOLVER_NON_SPD_APPLY:
          msg << "Linear system is not SPD.\n";
        case  Amanzi::AmanziSolvers::LIN_SOLVER_NON_SPD_APPLY_INVERSE:
          msg << "Linear system is not SPD.\n";
        case  Amanzi::AmanziSolvers::LIN_SOLVER_MAX_ITERATIONS:
          msg << "Maximum iterations are reached in solution of linear system.\n";
        case  Amanzi::AmanziSolvers::LIN_SOLVER_RESIDUAL_OVERFLOW:
          msg << "Residual overflow in solution of linear system.\n";
        default:
          msg << "\nLinear solver returned an unrecoverable error code: "<<ierr<<".\n";
        }
        Exceptions::amanzi_throw(msg);
      }

      residual += solver->residual();
      num_itrs += solver->num_itrs();

      for (int c = 0; c < ncells_owned; c++) {
        tcc_next[i][c] = sol_cell[0][c];
      }
    }

    if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
      Teuchos::OSTab tab = vo_->getOSTab();
      *vo_->os() << "dispersion solver (" << solver->name() 
                 << ") ||r||=" << residual / num_components
                 << " itrs=" << num_itrs / num_components << std::endl;
    }
  }

  // statistics output
  nsubcycles = ncycles;
  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << ncycles << " sub-cycles, dt_stable=" << dt_original 
               << " [sec]  dt_MPC=" << dt_MPC << " [sec]" << std::endl;

    VV_PrintSoluteExtrema(tcc_next, dt_MPC);
  }

  return failed;
}


/* ******************************************************************* 
* Copy theadvected tcc to the provided state.
******************************************************************* */
void Transport_PK::CommitStep(double t_old, double t_new)
{
  Teuchos::RCP<CompositeVector> cv;
  cv = S_->GetFieldData("total_component_concentration", passwd_);
  *cv = *tcc_tmp;
}


/* ******************************************************************* 
 * A simple first-order transport method 
 ****************************************************************** */
void Transport_PK::AdvanceDonorUpwind(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // populating next state of concentrations
  tcc->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  // prepare conservative state in master and slave cells
  double vol_phi_ws, tcc_flux;

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  for (int c = 0; c < ncells_owned; c++) {
    vol_phi_ws = mesh_->cell_volume(c) * (*phi)[0][c] * (*ws_start)[0][c];

    for (int i = 0; i < num_advect; i++)
      tcc_next[i][c] = tcc_prev[i][c] * vol_phi_ws;
  }

  // advance all components at once
  for (int f = 0; f < nfaces_wghost; f++) {  // loop over master and slave faces
    int c1 = (*upwind_cell_)[f];
    int c2 = (*downwind_cell_)[f];

    double u = fabs((*darcy_flux)[0][f]);

    if (c1 >=0 && c1 < ncells_owned && c2 >= 0 && c2 < ncells_owned) {
      for (int i = 0; i < num_advect; i++) {
        tcc_flux = dt_ * u * tcc_prev[i][c1];
        tcc_next[i][c1] -= tcc_flux;
        tcc_next[i][c2] += tcc_flux;
      }

    } else if (c1 >=0 && c1 < ncells_owned && (c2 >= ncells_owned || c2 < 0)) {
      for (int i = 0; i < num_advect; i++) {
        tcc_flux = dt_ * u * tcc_prev[i][c1];
        tcc_next[i][c1] -= tcc_flux;
      }

    } else if (c1 >= ncells_owned && c2 >= 0 && c2 < ncells_owned) {
      for (int i = 0; i < num_advect; i++) {
        tcc_flux = dt_ * u * tcc_prev[i][c1];
        tcc_next[i][c2] += tcc_flux;
      }
    }
  }

  // loop over exterior boundary sets
  for (int m = 0; m < bcs.size(); m++) {
    std::vector<int>& tcc_index = bcs[m]->tcc_index();
    std::vector<int>& faces = bcs[m]->faces();
    std::vector<std::vector<double> >& values = bcs[m]->values();

    int ncomp = tcc_index.size();
    int nbfaces = faces.size();
    for (int n = 0; n < nbfaces; n++) {
      int f = faces[n];
      int c2 = (*downwind_cell_)[f];

      if (c2 >= 0) {
        double u = fabs((*darcy_flux)[0][f]);
        for (int i = 0; i < ncomp; i++) {
          int k = tcc_index[i];
          if (k < num_advect) {
            tcc_flux = dt_ * u * values[n][i];
            tcc_next[k][c2] += tcc_flux;
          }
        }
      }
    }
  }

  // process external sources
  if (srcs.size() != 0) {
    double time = t_physics_;
    ComputeAddSourceTerms(time, dt_, srcs, tcc_next, 0, num_advect - 1);
  }

  // recover concentration from new conservative state
  for (int c = 0; c < ncells_owned; c++) {
    vol_phi_ws = mesh_->cell_volume(c) * (*phi)[0][c] * (*ws_end)[0][c];
    for (int i = 0; i < num_advect; i++) tcc_next[i][c] /= vol_phi_ws;
  }

  // update mass balance
  for (int i = 0; i < mass_solutes_exact_.size(); i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_;
  }

  if (internal_tests) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
 * We have to advance each component independently due to different
 * reconstructions. We use tcc when only owned data are needed and 
 * tcc_next when owned and ghost data. This is a special routine for 
 * transient flow and uses first-order time integrator. 
 ****************************************************************** */
void Transport_PK::AdvanceSecondOrderUpwindRK1(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // work memory
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  Epetra_Vector f_component(cmap_wghost);

  // distribute vector of concentrations
  S_->GetFieldData("total_component_concentration")->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_prev(i);
    Functional(T, *component, f_component);

    double ws_ratio;
    for (int c = 0; c < ncells_owned; c++) {
      ws_ratio = (*ws_start)[0][c] / (*ws_end)[0][c];
      tcc_next[i][c] = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio;
    }
  }

  // update mass balance
  for (int i = 0; i < num_aqueous + num_gaseous; i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_;
  }

  if (internal_tests) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
 * We have to advance each component independently due to different
 * reconstructions. This is a special routine for transient flow and 
 * uses second-order predictor-corrector time integrator. 
 ****************************************************************** */
void Transport_PK::AdvanceSecondOrderUpwindRK2(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // work memory
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  Epetra_Vector f_component(cmap_wghost);

  // distribute old vector of concentrations
  S_->GetFieldData("total_component_concentration")->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  Epetra_Vector ws_ratio(Copy, *ws_start, 0);
  for (int c = 0; c < ncells_owned; c++) ws_ratio[c] /= (*ws_end)[0][c];

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  // predictor step
  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_prev(i);
    Functional(T, *component, f_component);

    for (int c = 0; c < ncells_owned; c++) {
      tcc_next[i][c] = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio[c];
    }
  }

  tcc_tmp->ScatterMasterToGhosted("cell");

  // corrector step
  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_next(i);
    Functional(T, *component, f_component);

    for (int c = 0; c < ncells_owned; c++) {
      double value = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio[c];
      tcc_next[i][c] = (tcc_next[i][c] + value) / 2;
    }
  }

  // update mass balance
  for (int i = 0; i < num_aqueous + num_gaseous; i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_ / 2;
  }

  if (internal_tests) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
 * We have to advance each component independently due to different
 * reconstructions. We use tcc when only owned data are needed and 
 * tcc_next when owned and ghost data.
 *
 * Data flow: loop over components and apply the generic RK2 method.
 * The generic means that saturation is constant during time step. 
 ****************************************************************** */
/*
void Transport_PK::AdvanceSecondOrderUpwindGeneric(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step

  Teuchos::RCP<Epetra_MultiVector> tcc = TS->total_component_concentration();
  Teuchos::RCP<Epetra_MultiVector> tcc_next = TS_nextBIG->total_component_concentration();

  // define time integration method
  Explicit_TI::RK::method_t ti_method = Explicit_TI::RK::forward_euler;
  if (temporal_disc_order == 2) {
    ti_method = Explicit_TI::RK::heun_euler;
  }
  Explicit_TI::RK TVD_RK(*this, ti_method, *component_);

  // We advect only aqueous components.
  // int ncomponents = tcc_next.NumVectors();
  int ncomponents = num_aqueous;

  for (int i = 0; i < ncomponents; i++) {
    current_component_ = i;  // it is needed in BJ called inside RK:fun

    Epetra_Vector*& tcc_component = (*tcc)(i);
    TS_nextBIG->CopyMasterCell2GhostCell(*tcc_component, *component_);

    double T = 0.0;  // requires fixes (lipnikov@lanl.gov)
    TVD_RK.step(T, dt_, *component_, *component_next_);

    for (int c = 0; c < ncells_owned; c++) (*tcc_next)[i][c] = (*component_next_)[c];
  }
}
*/


/* ******************************************************************
* Computes source and sink terms and adds them to vector tcc.
* Return mass rate for the tracer.
* The routine treats two cases of tcc with one and all components.
****************************************************************** */
void Transport_PK::ComputeAddSourceTerms(double tp, double dtp, 
                                         std::vector<TransportDomainFunction*>& srcs, 
                                         Epetra_MultiVector& tcc, int n0, int n1)
{
  int num_vectors = tcc.NumVectors();
  int nsrcs = srcs.size();

  for (int m = 0; m < nsrcs; m++) {
    int i = srcs[m]->tcc_index();
    if (i < n0 || i > n1) continue;

    int imap = i;
    if (num_vectors == 1) imap = 0;

    double t0 = tp - dtp;
    int distribution = srcs[m]->CollectActionsList();
    if (distribution & CommonDefs::DOMAIN_FUNCTION_ACTION_DISTRIBUTE_PERMEABILITY) {
      srcs[m]->ComputeDistributeMultiValue(t0, tp, Kxy->Values()); 
    } else {
      srcs[m]->ComputeDistributeMultiValue(t0, tp);
    }

    TransportDomainFunction::Iterator it;

    for (it = srcs[m]->begin(); it != srcs[m]->end(); ++it) {
      int c = it->first;
      double value = mesh_->cell_volume(c) * it->second;

      tcc[imap][c] += dtp * value;
      mass_solutes_source_[i] += value;
    }
  }
}


/* *******************************************************************
* Populates operators' boundary data for given component.
* Returns true if at least one face was populated.
******************************************************************* */
bool Transport_PK::PopulateBoundaryData(
    std::vector<int>& bc_model, std::vector<double>& bc_value, int component)
{
  bool flag = false;

  for (int i = 0; i < bc_model.size(); i++) {
    bc_model[i] = Operators::OPERATOR_BC_NONE;
    bc_value[i] = 0.0;
  }

  AmanziMesh::Entity_ID_List cells;
  for (int f = 0; f < nfaces_wghost; f++) {
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    if (cells.size() == 1) bc_model[f] = Operators::OPERATOR_BC_NEUMANN;
  }

  for (int m = 0; m < bcs.size(); m++) {
    std::vector<int>& tcc_index = bcs[m]->tcc_index();
    std::vector<int>& faces = bcs[m]->faces();
    std::vector<std::vector<double> >& values = bcs[m]->values();

    int ncomp = tcc_index.size();
    int nbfaces = faces.size();
    for (int n = 0; n < nbfaces; n++) {
      int f = faces[n];

      for (int i = 0; i < ncomp; i++) {
        int k = tcc_index[i];
        if (k == component) {
          bc_model[f] = Operators::OPERATOR_BC_DIRICHLET;
          bc_value[f] = values[n][i];
          flag = true;
        }
      }
    }
  }

  return flag;
}


/* *******************************************************************
* Identify flux direction based on orientation of the face normal 
* and sign of the  Darcy velocity.                               
******************************************************************* */
void Transport_PK::IdentifyUpwindCells()
{
  for (int f = 0; f < nfaces_wghost; f++) {
    (*upwind_cell_)[f] = -1;  // negative value indicates boundary
    (*downwind_cell_)[f] = -1;

    }
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  for (int c = 0; c < ncells_wghost; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);

    for (int i = 0; i < faces.size(); i++) {
      int f = faces[i];
      double tmp = (*darcy_flux)[0][f] * dirs[i];
      if (tmp > 0.0) {
        (*upwind_cell_)[f] = c;
      } else if (tmp < 0.0) {
        (*downwind_cell_)[f] = c;
      } else if (dirs[i] > 0) {
        (*upwind_cell_)[f] = c;
      } else {
        (*downwind_cell_)[f] = c;
      }
    }
  }
}


/* *******************************************************************
* Interpolate linearly in time between two values v0 and v1. The time 
* is measuared relative to value v0; so that v1 is at time dt. The
* interpolated data are at time dt_int.            
******************************************************************* */
void Transport_PK::InterpolateCellVector(
    const Epetra_MultiVector& v0, const Epetra_MultiVector& v1, 
    double dt_int, double dt, Epetra_MultiVector& v_int) 
{
  double a = dt_int / dt;
  double b = 1.0 - a;
  v_int.Update(b, v0, a, v1, 0.);
}

}  // namespace Transport
}  // namespace Amanzi

