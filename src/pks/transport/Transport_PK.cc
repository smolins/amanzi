/*
  Transport PK 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)

  Major transport algorithms.
*/

#include <algorithm>
#include <vector>

#include "Epetra_Vector.h"
#include "Epetra_IntVector.h"
#include "Epetra_MultiVector.h"
#include "Epetra_Import.h"
#include "Teuchos_RCP.hpp"

// Amanzi
#include "BCs.hh"
#include "errors.hh"
#include "Explicit_TI_RK.hh"
#include "FieldEvaluator.hh"
#include "GMVMesh.hh"
#include "LinearOperatorDefs.hh"
#include "LinearOperatorFactory.hh"
#include "Mesh.hh"
#include "OperatorDefs.hh"
#include "PDE_Accumulation.hh"
#include "PDE_Diffusion.hh"
#include "PDE_DiffusionFactory.hh"
#include "PK_DomainFunctionFactory.hh"
#include "PK_Utils.hh"

// amanzi::Transport
#include "MultiscaleTransportPorosityFactory.hh"
#include "Transport_PK.hh"
#include "TransportBoundaryFunction_Alquimia.hh"
#include "TransportDomainFunction.hh"
#include "TransportSourceFunction_Alquimia.hh"

namespace Amanzi {
namespace Transport {

/* ******************************************************************
* New constructor compatible with new MPC framework.
****************************************************************** */
Transport_PK::Transport_PK(Teuchos::ParameterList& pk_tree,
                           const Teuchos::RCP<Teuchos::ParameterList>& glist,
                           const Teuchos::RCP<State>& S,
                           const Teuchos::RCP<TreeVector>& soln) :
  S_(S),
  soln_(soln)
{
  std::string pk_name = pk_tree.name();
  auto found = pk_name.rfind("->");
  if (found != std::string::npos) pk_name.erase(0, found + 2);

  if (glist->isSublist("cycle driver")) {
    if (glist->sublist("cycle driver").isParameter("component names")) {
      // grab the component names
      component_names_ = glist->sublist("cycle driver")
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

  preconditioner_list_ = Teuchos::sublist(glist, "preconditioners");
  linear_solver_list_ = Teuchos::sublist(glist, "solvers");
  nonlinear_solver_list_ = Teuchos::sublist(glist, "nonlinear solvers");

  subcycling_ = tp_list_->get<bool>("transport subcycling", true);
   
  // domain name
  domain_ = tp_list_->template get<std::string>("domain name", "domain");

  // initialize io
  Teuchos::RCP<Teuchos::ParameterList> units_list = Teuchos::sublist(glist, "units");
  units_.Init(*units_list);

  vo_ = Teuchos::null;
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

  preconditioner_list_ = Teuchos::sublist(glist, "preconditioners");
  linear_solver_list_ = Teuchos::sublist(glist, "solvers");
  nonlinear_solver_list_ = Teuchos::sublist(glist, "nonlinear solvers");

  // domain name
  domain_ = tp_list_->template get<std::string>("domain name", "domain");

  // initialize io
  Teuchos::RCP<Teuchos::ParameterList> units_list = Teuchos::sublist(glist, "units");
  units_.Init(*units_list);

  vo_ = Teuchos::null;

}


/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
Transport_PK::~Transport_PK()
{ 
  if (vo_ != Teuchos::null) vo_ = Teuchos::null;
}


/* ******************************************************************
* Setup for Alquimia.
****************************************************************** */
#ifdef ALQUIMIA_ENABLED
void Transport_PK::SetupAlquimia(Teuchos::RCP<AmanziChemistry::Alquimia_PK> chem_pk,
                                 Teuchos::RCP<AmanziChemistry::ChemistryEngine> chem_engine)
{
  chem_pk_ = chem_pk;
  chem_engine_ = chem_engine;

  if (chem_engine_ != Teuchos::null) {
    // Retrieve the component names (primary and secondary) from the chemistry 
    // engine.
    std::vector<std::string> component_names;
    chem_engine_->GetPrimarySpeciesNames(component_names);
    component_names_ = component_names;
    for (int i = 0; i < chem_engine_->NumAqueousComplexes(); ++i) {
      char secondary_name[128];
      snprintf(secondary_name, 127, "secondary_%d", i);
      component_names_.push_back(secondary_name);
    }
  }
}
#endif


/* ******************************************************************
* Define structure of this PK.
****************************************************************** */
void Transport_PK::Setup(const Teuchos::Ptr<State>& S)
{
  passwd_ = "state";  // owner's password

  mesh_ = S->GetMesh(domain_);
  dim = mesh_->space_dimension();

  // generate keys here to be available for setup of the base class
  tcc_key_ = Keys::getKey(domain_, "total_component_concentration"); 

  permeability_key_ = Keys::getKey(domain_, "permeability"); 
  porosity_key_ = Keys::getKey(domain_, "porosity"); 
  transport_porosity_key_ = Keys::getKey(domain_, "transport_porosity"); 

  darcy_flux_key_ = Keys::getKey(domain_, "darcy_flux"); 
  darcy_flux_fracture_key_ = Keys::getKey(domain_, "darcy_flux_fracture");

  saturation_liquid_key_ = Keys::getKey(domain_, "saturation_liquid"); 
  prev_saturation_liquid_key_ = Keys::getKey(domain_, "prev_saturation_liquid"); 

  water_content_key_ = Keys::getKey(domain_, "water_content"); 
  prev_water_content_key_ = Keys::getKey(domain_, "prev_water_content"); 


  // cross-coupling of PKs
  Teuchos::RCP<Teuchos::ParameterList> physical_models =
      Teuchos::sublist(tp_list_, "physical models and assumptions");
  bool abs_perm = physical_models->get<bool>("permeability field is required", false);
  std::string multiscale_model = physical_models->get<std::string>("multiscale model", "single continuum");
  use_transport_porosity_ = physical_models->get<bool>("effective transport porosity", false);

  // require state fields when Flow PK is off
  if (!S->HasField(permeability_key_) && abs_perm) {
    S->RequireField(permeability_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, dim);
  }
  if (!S->HasField(darcy_flux_key_)) {
    S->RequireField(darcy_flux_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("face", AmanziMesh::FACE, 1);
  }
  if (!S->HasField(saturation_liquid_key_)) {
    S->RequireField(saturation_liquid_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S->HasField(prev_saturation_liquid_key_)) {
    S->RequireField(prev_saturation_liquid_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
    S->GetField(prev_saturation_liquid_key_, passwd_)->set_io_vis(false);
  }

  // require state fields when Transport PK is on
  if (component_names_.size() == 0) {
    Errors::Message msg;
    msg << "Transport PK: list of solutes is empty.\n";
    Exceptions::amanzi_throw(msg);  
  }

  int ncomponents = component_names_.size();
  if (!S->HasField(tcc_key_)) {
    std::vector<std::vector<std::string> > subfield_names(1);
    subfield_names[0] = component_names_;

    S->RequireField(tcc_key_, passwd_, subfield_names)
      ->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, ncomponents);
  }

  // porosity evaluators
  if (!S->HasField(porosity_key_)) {
    S->RequireField(porosity_key_, porosity_key_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
    S->RequireFieldEvaluator(porosity_key_);
  }

  if (use_transport_porosity_) {
    if (!S->HasField(transport_porosity_key_)) {
      S->RequireField(transport_porosity_key_, transport_porosity_key_)->SetMesh(mesh_)
        ->SetGhosted(true)->SetComponent("cell", AmanziMesh::CELL, 1);
      S->RequireFieldEvaluator(transport_porosity_key_);
    }
  }

  // require multiscale fields
  multiscale_porosity_ = false;
  if (multiscale_model == "dual continuum discontinuous matrix") {
    multiscale_porosity_ = true;
    msp_ = CreateMultiscaleTransportPorosityPartition(mesh_, tp_list_);

    if (!S->HasField("total_component_concentration_matrix")) {
      std::vector<std::vector<std::string> > subfield_names(1);
      subfield_names[0] = component_names_;

      S->RequireField("total_component_concentration_matrix", passwd_, subfield_names)
        ->SetMesh(mesh_)->SetGhosted(false)
        ->SetComponent("cell", AmanziMesh::CELL, ncomponents);

      // Secondary matrix nodes are collected here. We assume same order of species.
      int nnodes, nnodes_tmp = NumberMatrixNodes(msp_);
      mesh_->get_comm()->MaxAll(&nnodes_tmp, &nnodes, 1);
      if (nnodes > 1) {
        S->RequireField("total_component_concentration_matrix_aux", passwd_)
          ->SetMesh(mesh_)->SetGhosted(false)
          ->SetComponent("cell", AmanziMesh::CELL, ncomponents * (nnodes - 1));
        S->GetField("total_component_concentration_matrix_aux", passwd_)->set_io_vis(false);
      }
    }

    // -- water content in fractures
    if (!S->HasField(water_content_key_)) {
      S->RequireField(water_content_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
    }
    if (!S->HasField(prev_water_content_key_)) {
      S->RequireField(prev_water_content_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
      S->GetField(prev_water_content_key_, passwd_)->set_io_vis(false);
    }

    // -- water content in matrix
    if (!S->HasField("water_content_matrix")) {
      S->RequireField("water_content_matrix", passwd_)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
    }
    if (!S->HasField("prev_water_content_matrix")) {
      S->RequireField("prev_water_content_matrix", passwd_)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
      S->GetField("prev_water_content_matrix", passwd_)->set_io_vis(false);
    }

    // -- porosity of matrix
    if (!S->HasField("porosity_matrix")) {
      S->RequireField("porosity_matrix", passwd_)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
    }
  }

  // require fracture fields
  if (mesh_->space_dimension() != mesh_->manifold_dimension()) {
    if (!S->HasField(darcy_flux_fracture_key_)) {
      S->RequireField(darcy_flux_fracture_key_, passwd_)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, mesh_->cell_get_max_faces());
      S->GetField(darcy_flux_fracture_key_, passwd_)->set_io_vis(false);
    }
  }
}


/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
void Transport_PK::Initialize(const Teuchos::Ptr<State>& S)
{
  // Set initial values for transport variables.
  dt_ = dt_debug_ = t_physics_ = 0.0;
  double time = S->time();
  if (time >= 0.0) t_physics_ = time;

  dispersion_preconditioner = "identity";

  internal_tests_ = 0;
  internal_tests_tol_ = TRANSPORT_CONCENTRATION_OVERSHOOT;

  // Create verbosity object.
  Teuchos::ParameterList vlist;
  vlist.sublist("verbose object") = tp_list_->sublist("verbose object");
  vo_ =  Teuchos::rcp(new VerboseObject("Transport-" + domain_, vlist)); 

  MyPID = mesh_->get_comm()->MyPID();

  // initialize missed fields
  InitializeFields_();

  // Check input parameters. Due to limited amount of checks, we can do it earlier.
  Policy(S.ptr());

  ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::Parallel_type::OWNED);
  ncells_wghost = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::Parallel_type::ALL);

  nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::Parallel_type::OWNED);
  nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::Parallel_type::ALL);

  nnodes_wghost = mesh_->num_entities(AmanziMesh::NODE, AmanziMesh::Parallel_type::ALL);

  // extract control parameters
  InitializeAll_();
 
  // pointers to state variables (move in subroutines for consistency)
  S->GetFieldData(darcy_flux_key_)->ScatterMasterToGhosted("face");

  darcy_flux = S->GetFieldData(darcy_flux_key_)->ViewComponent("face", true);

  ws = S->GetFieldData(saturation_liquid_key_)->ViewComponent("cell", false);
  ws_prev = S->GetFieldData(prev_saturation_liquid_key_)->ViewComponent("cell", false);
  phi = S->GetFieldData(porosity_key_)->ViewComponent("cell", false);

  if (use_transport_porosity_) {
    transport_phi = S->GetFieldData(transport_porosity_key_)->ViewComponent("cell", false);
  } else {
    transport_phi = phi;
  }

  flux_map_ = S_->GetFieldData(darcy_flux_key_)->Map().Map("face", true);

  
  tcc = S->GetFieldData(tcc_key_, passwd_);

  // memory for new components
  tcc_tmp = Teuchos::rcp(new CompositeVector(*(S->GetFieldData(tcc_key_))));
  *tcc_tmp = *tcc;

  // upwind structures
  IdentifyUpwindCells();

  // advection block initialization
  current_component_ = -1;

  const Epetra_Map& cmap_owned = mesh_->cell_map(false);
  ws_subcycle_start = Teuchos::rcp(new Epetra_Vector(cmap_owned));
  ws_subcycle_end = Teuchos::rcp(new Epetra_Vector(cmap_owned));

  // reconstruction initialization
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  lifting_ = Teuchos::rcp(new Operators::ReconstructionCell(mesh_));
  limiter_ = Teuchos::rcp(new Operators::LimiterCell(mesh_));

  // mechanical dispersion
  flag_dispersion_ = false;
  if (tp_list_->isSublist("material properties")) {
    Teuchos::RCP<Teuchos::ParameterList>
        mdm_list = Teuchos::sublist(tp_list_, "material properties");
    mdm_ = CreateMDMPartition(mesh_, mdm_list, flag_dispersion_);
    if (flag_dispersion_) CalculateAxiSymmetryDirection();
  }

  // create boundary conditions
  if (tp_list_->isSublist("boundary conditions")) {
    // -- try simple Dirichlet conditions for species
    PK_DomainFunctionFactory<TransportDomainFunction> factory(mesh_);
    Teuchos::ParameterList& clist = tp_list_->sublist("boundary conditions").sublist("concentration");

    for (auto it = clist.begin(); it != clist.end(); ++it) {
      std::string name = it->first;
      if (name == "coupling") {
        Teuchos::ParameterList& bc_list = clist.sublist(name);        
        Teuchos::ParameterList::ConstIterator it1 = bc_list.begin();
        std::string specname = it1->first;
        Teuchos::ParameterList& spec = bc_list.sublist(specname);
        Teuchos::RCP<TransportDomainFunction> 
          bc = factory.Create(spec, "boundary concentration", AmanziMesh::FACE, Kxy);
        
        for (int i = 0; i < component_names_.size(); i++) {
          bc->tcc_names().push_back(component_names_[i]);
          bc->tcc_index().push_back(i);
        }
        bc->set_state(S_);
        bcs_.push_back(bc);
      } else {        
        if (clist.isSublist(name)) {
          Teuchos::ParameterList& bc_list = clist.sublist(name);
          for (auto it1 = bc_list.begin(); it1 != bc_list.end(); ++it1) {
            std::string specname = it1->first;
            Teuchos::ParameterList& spec = bc_list.sublist(specname);
            Teuchos::RCP<TransportDomainFunction> 
              bc = factory.Create(spec, "boundary concentration", AmanziMesh::FACE, Kxy);

            bc->tcc_names().push_back(name);
            bc->tcc_index().push_back(FindComponentNumber(name));
            bc->set_state(S_);
            bcs_.push_back(bc);
          }
        }
      }
    }
#ifdef ALQUIMIA_ENABLED
    // -- try geochemical conditions
    Teuchos::ParameterList& glist = tp_list_->sublist("boundary conditions").sublist("geochemical");

    for (auto it = glist.begin(); it != glist.end(); ++it) {
      std::string specname = it->first;
      Teuchos::ParameterList& spec = glist.sublist(specname);

      Teuchos::RCP<TransportBoundaryFunction_Alquimia> 
          bc = Teuchos::rcp(new TransportBoundaryFunction_Alquimia(spec, mesh_, chem_pk_, chem_engine_));

      std::vector<int>& tcc_index = bc->tcc_index();
      std::vector<std::string>& tcc_names = bc->tcc_names();

      for (int i = 0; i < tcc_names.size(); i++) {
        tcc_index.push_back(FindComponentNumber(tcc_names[i]));
      }

      bcs_.push_back(bc);
    }
#endif
  } else {
    if (vo_->getVerbLevel() > Teuchos::VERB_NONE) {
      Teuchos::OSTab tab = vo_->getOSTab();
      *vo_->os() << vo_->color("yellow") << "No BCs were specified." << vo_->reset() << std::endl;
    }
  }

  // -- initialization
  time = t_physics_;
  for (int i = 0; i < bcs_.size(); i++) {
    bcs_[i]->Compute(time, time);
  }

  VV_CheckInfluxBC();

  // source term initialization: so far only "concentration" is available.
  if (tp_list_->isSublist("source terms")) {
    PK_DomainFunctionFactory<TransportDomainFunction> factory(mesh_);
    PKUtils_CalculatePermeabilityFactorInWell(S_.ptr(), Kxy);

    Teuchos::ParameterList& clist = tp_list_->sublist("source terms").sublist("concentration");
    for (auto it = clist.begin(); it != clist.end(); ++it) {
      std::string name = it->first;
      if (name == "coupling") {
        Teuchos::ParameterList& src_list = clist.sublist(name);
        Teuchos::ParameterList::ConstIterator it1 = src_list.begin();
        std::string specname = it1->first;
        Teuchos::ParameterList& spec = src_list.sublist(specname);
        Teuchos::RCP<TransportDomainFunction> src = factory.Create(spec, "sink", AmanziMesh::CELL, Kxy);
        
        for (int i = 0; i < component_names_.size(); i++) {
          src->tcc_names().push_back(component_names_[i]);
          src->tcc_index().push_back(i);
        }
          
        src->set_state(S_);
        srcs_.push_back(src);

      } else {
        if (clist.isSublist(name)) {
          Teuchos::ParameterList& src_list = clist.sublist(name);
          for (auto it1 = src_list.begin(); it1 != src_list.end(); ++it1) {
            std::string specname = it1->first;
            Teuchos::ParameterList& spec = src_list.sublist(specname);
            Teuchos::RCP<TransportDomainFunction> src = factory.Create(spec, AmanziMesh::CELL, Kxy);

            src->tcc_names().push_back(name);
            src->tcc_index().push_back(FindComponentNumber(name));

            srcs_.push_back(src);
          }
        }
      }
    }
#ifdef ALQUIMIA_ENABLED
    // -- try geochemical conditions
    Teuchos::ParameterList& glist = tp_list_->sublist("source terms").sublist("geochemical");

    for (auto it = glist.begin(); it != glist.end(); ++it) {
      std::string specname = it->first;
      Teuchos::ParameterList& spec = glist.sublist(specname);

      Teuchos::RCP<TransportSourceFunction_Alquimia> 
          src = Teuchos::rcp(new TransportSourceFunction_Alquimia(spec, mesh_, chem_pk_, chem_engine_));

      std::vector<int>& tcc_index = src->tcc_index();
      std::vector<std::string>& tcc_names = src->tcc_names();

      for (int i = 0; i < tcc_names.size(); i++) {
        tcc_index.push_back(FindComponentNumber(tcc_names[i]));
      }

      srcs_.push_back(src);
    }
#endif
  }

  // Temporarily Transport PK hosts the Henry law.
  PrepareAirWaterPartitioning_();

  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << "Number of components: " << tcc->size() << std::endl
               << "cfl=" << cfl_ << " spatial/temporal discretization: " 
               << spatial_disc_order << " " << temporal_disc_order << std::endl
               << "using transport porosity: " << use_transport_porosity_ << std::endl;
    *vo_->os() << vo_->color("green") << "Initalization of PK is complete." 
               << vo_->reset() << std::endl << std::endl;
  }
}


/* ******************************************************************
* Initalized fields not touched by State and other PKs.
****************************************************************** */
void Transport_PK::InitializeFields_()
{
  Teuchos::OSTab tab = vo_->getOSTab();

  // set popular default values when flow PK is off
  InitializeField(S_.ptr(), passwd_, saturation_liquid_key_, 1.0);
  InitializeField(S_.ptr(), passwd_, darcy_flux_fracture_key_, 0.0);

  InitializeFieldFromField_(water_content_key_, porosity_key_, false);
  InitializeFieldFromField_(prev_water_content_key_, water_content_key_, false);

  InitializeFieldFromField_("water_content_matrix", "porosity_matrix", false);
  InitializeFieldFromField_("prev_water_content_matrix", "water_content_matrix", false);

  InitializeFieldFromField_(prev_saturation_liquid_key_, saturation_liquid_key_, false);
  InitializeFieldFromField_("total_component_concentration_matrix", tcc_key_, false);

  InitializeField(S_.ptr(), passwd_, "total_component_concentration_matrix_aux", 0.0);
}


/* ****************************************************************
* Auxiliary initialization technique.
**************************************************************** */
void Transport_PK::InitializeFieldFromField_(
    const std::string& field0, const std::string& field1, bool call_evaluator)
{
  if (S_->HasField(field0)) {
    if (S_->GetField(field0)->owner() == passwd_) {
      if (!S_->GetField(field0, passwd_)->initialized()) {
        if (call_evaluator)
            S_->GetFieldEvaluator(field1)->HasFieldChanged(S_.ptr(), passwd_);

        const CompositeVector& f1 = *S_->GetFieldData(field1);
        CompositeVector& f0 = *S_->GetFieldData(field0, passwd_);
        f0 = f1;

        S_->GetField(field0, passwd_)->set_initialized();

        if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM)
            *vo_->os() << "initialized " << field0 << " to " << field1 << std::endl;
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
* are equivalent for divergence-free flows and guarantee the extrema
* diminishing principle. Outflux takes into account sinks and 
* sources but preserves only positivity of an advected mass.
* ***************************************************************** */
double Transport_PK::StableTimeStep()
{
  IdentifyUpwindCells();

  // Accumulate upwinding fluxes.
  std::vector<double> total_outflux(ncells_wghost, 0.0);

  for (int f = 0; f < nfaces_wghost; f++) {
    for (int k = 0; k < upwind_cells_[f].size(); k++) {
      int c = upwind_cells_[f][k];
      if (c >= 0) {
        total_outflux[c] += fabs(upwind_flux_[f][k]);
      }
    }
  }

  // Account for extraction of solute in production wells.
  // We assume one well per cell (FIXME).
  double t_old = S_->intermediate_time();
  for (int m = 0; m < srcs_.size(); m++) {
    if (srcs_[m]->keyword() == "producer") {
      srcs_[m]->Compute(t_old, t_old); 

      for (auto it = srcs_[m]->begin(); it != srcs_[m]->end(); ++it) {
        int c = it->first;
        std::vector<double>& values = it->second; 

        for (int i = 0; i < values.size(); ++i) {
          double value = fabs(values[i]) * mesh_->cell_volume(c);
          total_outflux[c] = std::max(total_outflux[c], value);
        }
      }
    }
    else if (srcs_[m]->keyword() == "sink") {  // FIXME
      srcs_[m]->Compute(t_old, t_old); 

      const auto& to_matrix = srcs_[m]->linear_term();
      for (auto it = to_matrix.begin(); it != to_matrix.end(); ++it) {
        int c = it->first;
        total_outflux[c] += std::fabs(it->second);
      }
    }
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

  // correct time step for high-order schemes
  if (spatial_disc_order == 2) dt_ /= 2;

  // no CFL update forsinks, since their are flow dependent.

  // communicate global time step
  double dt_tmp = dt_;
  ws_prev->Comm().MinAll(&dt_tmp, &dt_, 1);

  // incorporate developers and CFL constraints
  dt_ = std::min(dt_, dt_debug_);
  dt_ *= cfl_;

  // print optional diagnostics using maximum cell id as the filter
  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) {
    int cmin_dt_unique = (fabs(dt_tmp * cfl_ - dt_) < 1e-6 * dt_) ? cmin_dt : -1;
 
    int cmin_dt_tmp = cmin_dt_unique;
    ws_prev->Comm().MaxAll(&cmin_dt_tmp, &cmin_dt_unique, 1);
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
    StableTimeStep();
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

  // We use original tcc and make a copy of it later if needed.
  tcc = S_->GetFieldData(tcc_key_, passwd_);
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");

  // calculate stable time step
  double dt_shift = 0.0, dt_global = dt_MPC;
  double time = S_->intermediate_time();
  if (time >= 0.0) { 
    t_physics_ = time;
    dt_shift = time - S_->initial_time();
    dt_global = S_->final_time() - S_->initial_time();
  }

  StableTimeStep();
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
    for (int i = 0; i < bcs_.size(); i++) {
      bcs_[i]->Compute(time, time);
    }
    
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

    if (mesh_->space_dimension() == mesh_->manifold_dimension()) {
      if (spatial_disc_order == 1) {
        AdvanceDonorUpwind(dt_cycle);
      } else if (spatial_disc_order == 2 && genericRK_) {
        AdvanceSecondOrderUpwindRKn(dt_cycle);
      /* DEPRECATED 
      } else if (spatial_disc_order == 2 && temporal_disc_order == 1) {
        AdvanceSecondOrderUpwindRK1(dt_cycle);
      */
      } else if (spatial_disc_order == 2 && temporal_disc_order == 2) {
        AdvanceSecondOrderUpwindRK2(dt_cycle);
      }
    } else {  // transport on intersecting manifolds
      if (spatial_disc_order == 1) {
        AdvanceDonorUpwindNonManifold(dt_cycle);
      } else {
        AdvanceSecondOrderUpwindRKn(dt_cycle);
      }
    }

    // add implicit multiscale model
    if (multiscale_porosity_) {
      double t_int1 = t_old + dt_sum - dt_cycle;
      double t_int2 = t_old + dt_sum;
      AddMultiscalePorosity_(t_old, t_new, t_int1, t_int2);
    }

    if (! final_cycle) {  // rotate concentrations (we need new memory for tcc)
      tcc = Teuchos::RCP<CompositeVector>(new CompositeVector(*tcc_tmp));
    }

    ncycles++;
  }

  // output of selected statistics
  VV_PrintLimiterStatistics();

  dt_ = dt_original;  // restore the original time step (just in case)

  // We define tracer as the species #0 as calculate some statistics.
  int num_components = tcc_prev.NumVectors();
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", false);

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

  if (flag_dispersion_ || flag_diffusion) {
    Teuchos::ParameterList& op_list = 
        tp_list_->sublist("operators").sublist("diffusion operator").sublist("matrix");

    // default boundary conditions (none inside domain and Neumann on its boundary)
    Teuchos::RCP<Operators::BCs> bc_dummy = 
        Teuchos::rcp(new Operators::BCs(mesh_, AmanziMesh::FACE, Operators::DOF_Type::SCALAR));

    std::vector<int>& bc_model = bc_dummy->bc_model();
    std::vector<double>& bc_value = bc_dummy->bc_value();
    ComputeBCs_(bc_model, bc_value, -1);

    Operators::PDE_DiffusionFactory opfactory;
    Teuchos::RCP<Operators::PDE_Diffusion> op1 = opfactory.Create(op_list, mesh_, bc_dummy);
    op1->SetBCs(bc_dummy, bc_dummy);
    Teuchos::RCP<Operators::Operator> op = op1->global_operator();
    Teuchos::RCP<Operators::PDE_Accumulation> op2 =
        Teuchos::rcp(new Operators::PDE_Accumulation(AmanziMesh::CELL, op));

    const CompositeVectorSpace& cvs = op1->global_operator()->DomainMap();
    CompositeVector sol(cvs), factor(cvs), factor0(cvs), source(cvs), zero(cvs);
    zero.PutScalar(0.0);
  
    // instantiale solver
    AmanziSolvers::LinearOperatorFactory<Operators::Operator, CompositeVector, CompositeVectorSpace> sfactory;
    Teuchos::RCP<AmanziSolvers::LinearOperator<Operators::Operator, CompositeVector, CompositeVectorSpace> >
        solver = sfactory.Create(dispersion_solver, *linear_solver_list_, op);

    solver->add_criteria(AmanziSolvers::LIN_SOLVER_MAKE_ONE_ITERATION);  // Make at least one iteration

    // populate the dispersion operator (if any)
    if (flag_dispersion_) {
      CalculateDispersionTensor_(*darcy_flux, *transport_phi, *ws);
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
        CalculateDiffusionTensor_(md_change, phase, *transport_phi, *ws);
        flag_op1 = true;
      }

      // set the initial guess
      Epetra_MultiVector& sol_cell = *sol.ViewComponent("cell");
      for (int c = 0; c < ncells_owned; c++) {
        sol_cell[0][c] = tcc_next[i][c];
      }
      if (sol.HasComponent("face")) {
        sol.ViewComponent("face")->PutScalar(0.0);
      }

      if (flag_op1) {
        op->Init();
        Teuchos::RCP<std::vector<WhetStone::Tensor> > Dptr = Teuchos::rcpFromRef(D_);
        op1->Setup(Dptr, Teuchos::null, Teuchos::null);
        op1->UpdateMatrices(Teuchos::null, Teuchos::null);

        // add accumulation term
        Epetra_MultiVector& fac = *factor.ViewComponent("cell");
        for (int c = 0; c < ncells_owned; c++) {
          fac[0][c] = (*phi)[0][c] * (*ws)[0][c];
        }
        op2->AddAccumulationDelta(sol, factor, factor, dt_MPC, "cell");
 
        op1->ApplyBCs(true, true, true);
        op->SymbolicAssembleMatrix();
        op->AssembleMatrix();

        Teuchos::ParameterList pc_list = preconditioner_list_->sublist(dispersion_preconditioner);
        op->InitializePreconditioner(pc_list);
        op->UpdatePreconditioner();
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
    // tensor (D is reset). Inactive cells (s[c] = 1 and D_[c] = 0) 
    // are treated with a hack of the accumulation term.
    D_.clear();
    md_old = 0.0;
    for (int i = num_aqueous; i < num_components; i++) {
      FindDiffusionValue(component_names_[i], &md_new, &phase);
      md_change = md_new - md_old;
      md_old = md_new;

      if (md_change != 0.0 || i == num_aqueous) {
        CalculateDiffusionTensor_(md_change, phase, *transport_phi, *ws);
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
      Teuchos::RCP<std::vector<WhetStone::Tensor> > Dptr = Teuchos::rcpFromRef(D_);
      op1->Setup(Dptr, Teuchos::null, Teuchos::null);
      op1->UpdateMatrices(Teuchos::null, Teuchos::null);

      // add boundary conditions and sources for gaseous components
      ComputeBCs_(bc_model, bc_value, i);

      Epetra_MultiVector& rhs_cell = *op->rhs()->ViewComponent("cell");
      ComputeSources_(t_new, 1.0, rhs_cell, tcc_prev, i, i);
      op1->ApplyBCs(true, true, true);

      // add accumulation term
      Epetra_MultiVector& fac1 = *factor.ViewComponent("cell");
      Epetra_MultiVector& fac0 = *factor0.ViewComponent("cell");

      for (int c = 0; c < ncells_owned; c++) {
        fac1[0][c] = (*phi)[0][c] * (1.0 - (*ws)[0][c]);
        fac0[0][c] = (*phi)[0][c] * (1.0 - (*ws_prev)[0][c]);
        if ((*ws)[0][c] == 1.0) fac1[0][c] = 1.0;  // hack so far
      }
      op2->AddAccumulationDelta(sol, factor0, factor, dt_MPC, "cell");
 
      op->SymbolicAssembleMatrix();
      op->AssembleMatrix();

      Teuchos::ParameterList pc_list = preconditioner_list_->sublist(dispersion_preconditioner);
      op->InitializePreconditioner(pc_list);
      op->UpdatePreconditioner();
  
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

    if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
      Teuchos::OSTab tab = vo_->getOSTab();
      *vo_->os() << "dispersion solver (" << solver->name() 
                 << ") ||r||=" << residual / num_components
                 << " itrs=" << num_itrs / num_components << std::endl;
    }
  }

  // optional Henry Law for the case of gas diffusion
  if (henry_law_) {
    MakeAirWaterPartitioning_();
  }

  // statistics output
  nsubcycles = ncycles;
  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << ncycles << " sub-cycles, dt_stable=" << units_.OutputTime(dt_original) 
               << ", dt_MPC=" << units_.OutputTime(dt_MPC) << std::endl;

    VV_PrintSoluteExtrema(tcc_next, dt_MPC);
  }

  return failed;
}


/* ******************************************************************* 
* Add implict time discretization of the multiscale porosity model on 
* time sub-interval [t_int1, t_int2]:
*   d(VWC_f)/dt -= G_s, d(VWC_m) = G_s 
*   G_s = G_w C^* + omega_s (C_f - C_m).
******************************************************************* */
void Transport_PK::AddMultiscalePorosity_(
    double t_old, double t_new, double t_int1, double t_int2)
{
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell");
  Epetra_MultiVector& tcc_matrix = 
     *S_->GetFieldData("total_component_concentration_matrix", passwd_)->ViewComponent("cell");

  const Epetra_MultiVector& wcf_prev = *S_->GetFieldData(prev_water_content_key_)->ViewComponent("cell");
  const Epetra_MultiVector& wcf = *S_->GetFieldData(water_content_key_)->ViewComponent("cell");

  const Epetra_MultiVector& wcm_prev = *S_->GetFieldData("prev_water_content_matrix")->ViewComponent("cell");
  const Epetra_MultiVector& wcm = *S_->GetFieldData("water_content_matrix")->ViewComponent("cell");

  // multi-node matrix requires more input data
  const Epetra_MultiVector& phi_matrix = *S_->GetFieldData("porosity_matrix")->ViewComponent("cell");

  int nnodes(1);
  Teuchos::RCP<Epetra_MultiVector> tcc_matrix_aux;
  if (S_->HasField("total_component_concentration_matrix_aux")) {
    tcc_matrix_aux = S_->GetFieldData("total_component_concentration_matrix_aux", passwd_)->ViewComponent("cell");
    nnodes = tcc_matrix_aux->NumVectors() + 1; 
  }
  WhetStone::DenseVector tcc_m(nnodes);

  double flux_liquid, flux_solute, wcm0, wcm1, wcf0, wcf1;
  double dtg, dts, t1, t2, tmp0, tmp1, tfp0, tfp1, a, b;
  std::vector<AmanziMesh::Entity_ID> block;

  dtg = t_new - t_old;
  dts = t_int2 - t_int1;
  t1 = t_int1 - t_old;
  t2 = t_int2 - t_old;

  for (int c = 0; c < ncells_owned; ++c) {
    wcm0 = wcm_prev[0][c];
    wcm1 = wcm[0][c];
    flux_liquid = (wcm1 - wcm0) / dtg;
  
    wcf0 = wcf_prev[0][c];
    wcf1 = wcf[0][c];
  
    a = t1 / dtg;
    b = t2 / dtg;

    tfp0 = a * wcf1 + (1.0 - a) * wcf0;
    tfp1 = b * wcf1 + (1.0 - b) * wcf0;

    tmp0 = a * wcm1 + (1.0 - a) * wcm0;
    tmp1 = b * wcm1 + (1.0 - b) * wcm0;

    double phim = phi_matrix[0][c];

    for (int i = 0; i < num_aqueous; ++i) {
      tcc_m(0) = tcc_matrix[i][c];
      if (tcc_matrix_aux != Teuchos::null) {
        for (int n = 0; n < nnodes - 1; ++n) 
          tcc_m(n + 1) = (*tcc_matrix_aux)[n][c];
      }

      flux_solute = msp_->second[(*msp_->first)[c]]->ComputeSoluteFlux(
          flux_liquid, tcc_next[i][c], tcc_m, 
          i, dts, tfp0, tfp1, tmp0, tmp1, phim);

      tcc_matrix[i][c] = tcc_m(0);
      if (tcc_matrix_aux != Teuchos::null) {
        for (int n = 0; n < nnodes - 1; ++n) 
          (*tcc_matrix_aux)[n][c] = tcc_m(n + 1);
      }
    }
  }
}


/* ******************************************************************* 
* Copy the advected tcc field to the state.
******************************************************************* */
void Transport_PK::CommitStep(double t_old, double t_new, const Teuchos::RCP<State>& S)
{
  Teuchos::RCP<CompositeVector> tcc;
  tcc = S->GetFieldData(tcc_key_, passwd_);
  *tcc = *tcc_tmp;
}


/* ******************************************************************* 
* A simple first-order "donor" upwind method.
******************************************************************* */
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

  auto flux_map = S_->GetFieldData(darcy_flux_key_)->Map().Map("face", true);
 
  // advance all components at once
  for (int f = 0; f < nfaces_wghost; f++) {  // loop over master and slave faces
    int g = flux_map->FirstPointInElement(f);

    for ( int j = 0; j < upwind_cells_[f].size(); j++) {
      int c1 = upwind_cells_[f][j];
      int c2 = downwind_cells_[f][j];
                
      double u = fabs((*darcy_flux)[0][g + j]);

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
  }

  // loop over exterior boundary sets
  int flag(0);
  tcc_tmp->PutScalarGhosted(0.0);

  for (int m = 0; m < bcs_.size(); m++) {
    std::vector<int>& tcc_index = bcs_[m]->tcc_index();
    int ncomp = tcc_index.size();

    for (auto it = bcs_[m]->begin(); it != bcs_[m]->end(); ++it) {
      int f = it->first;
      if (f >= nfaces_owned) continue;

      std::vector<double>& values = it->second;       
      if (downwind_cells_[f].size() > 0) {
        for (int j = 0; j < downwind_cells_[f].size(); j++) {
          int c2 = downwind_cells_[f][j];
          if (c2 < 0) continue;
          if (c2 >= ncells_owned) flag = 1;

          double u = fabs(downwind_flux_[f][j]);
          for (int i = 0; i < ncomp; i++) {
            int k = tcc_index[i];
            if (k < num_advect) {
              tcc_flux = dt_ * u * values[i];
              tcc_next[k][c2] += tcc_flux;
            }
          }
        }
      }
    }    
  }

  int flag_tmp(flag);
  mesh_->get_comm()->MaxAll(&flag_tmp, &flag, 1);
  if (flag == 1) tcc_tmp->GatherGhostedToMaster();

  // process external sources
  if (srcs_.size() != 0) {
    double time = t_physics_;
    ComputeSources_(time, dt_, tcc_next, tcc_prev, 0, num_advect - 1);
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

  if (internal_tests_) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
* A simple first-order upwind method on non-manifolds.
******************************************************************* */
void Transport_PK::AdvanceDonorUpwindNonManifold(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // populating next state of concentrations
  tcc->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  // prepare conservative state in master and slave cells
  double u, vol_phi_ws, tcc_flux;

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  for (int c = 0; c < ncells_owned; c++) {
    vol_phi_ws = mesh_->cell_volume(c) * (*phi)[0][c] * (*ws_start)[0][c];

    for (int i = 0; i < num_advect; i++)
      tcc_next[i][c] = tcc_prev[i][c] * vol_phi_ws;
  }

  // advance all components at once
  for (int f = 0; f < nfaces_wghost; f++) {
    // calculate in and out fluxes and solutes at given face
    double flux_in(0.0);
    std::vector<double> tcc_out(num_advect, 0.0);

    for (int n = 0; n < upwind_cells_[f].size(); ++n) {
      int c = upwind_cells_[f][n];
      u = upwind_flux_[f][n];

      for (int i = 0; i < num_advect; i++) {
        tcc_out[i] += u * tcc_prev[i][c];
      }
    }

    for (int n = 0; n < downwind_cells_[f].size(); ++n) {
      flux_in -= downwind_flux_[f][n];
    }
    if (flux_in == 0.0) flux_in = 1e-12;

    // update solutes
    for (int n = 0; n < upwind_cells_[f].size(); ++n) {
      int c = upwind_cells_[f][n];
      u = upwind_flux_[f][n];

      if (c < ncells_owned) {
        for (int i = 0; i < num_advect; i++) {
          tcc_next[i][c] -= dt_ * u * tcc_prev[i][c];
        }
      }
    }

    for (int n = 0; n < downwind_cells_[f].size(); ++n) {
      int c = downwind_cells_[f][n];
      u = downwind_flux_[f][n];

      if (c < ncells_owned) {
        double tmp = u / flux_in;
        for (int i = 0; i < num_advect; i++) {
          tcc_next[i][c] -= dt_ * tmp * tcc_out[i];
        }
      }
    }
  }

  // loop over exterior boundary sets
  for (int m = 0; m < bcs_.size(); m++) {
    std::vector<int>& tcc_index = bcs_[m]->tcc_index();
    int ncomp = tcc_index.size();

    for (auto it = bcs_[m]->begin(); it != bcs_[m]->end(); ++it) {
      int f = it->first;
      std::vector<double>& values = it->second; 

      if (downwind_cells_[f].size() > 0) {
        int c = downwind_cells_[f][0];
        double u = downwind_flux_[f][0];

        for (int i = 0; i < ncomp; i++) {
          int k = tcc_index[i];
          if (k < num_advect) {
            tcc_flux = dt_ * u * values[i];
            tcc_next[k][c] -= tcc_flux;
          }
        }
      }
    }
  }

  // process external sources
  if (srcs_.size() != 0) {
    double time = t_physics_;
    ComputeSources_(time, dt_, tcc_next, tcc_prev, 0, num_advect - 1);
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
}


/* ******************************************************************* 
*                         DEPRECATED
* Advance each component independently due to different field 
* reconstruction. This routine uses first-order time integrator. 
******************************************************************* */
/*
void Transport_PK::AdvanceSecondOrderUpwindRK1(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // work memory
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  Epetra_Vector f_component(cmap_wghost);

  // distribute vector of concentrations
  S_->GetFieldData(tcc_key_)->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_prev(i);
    DudtOld(T, *component, f_component);

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

  if (internal_tests_) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}
*/


/* ******************************************************************* 
* Advance each component independently due to different field
* reconstructions. This routine uses custom implementation of the 
* second-order predictor-corrector time integration scheme. 
******************************************************************* */
void Transport_PK::AdvanceSecondOrderUpwindRK2(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // work memory
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  Epetra_Vector f_component(cmap_wghost);

  // distribute old vector of concentrations
  S_->GetFieldData(tcc_key_)->ScatterMasterToGhosted("cell");
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
    DudtOld(T, *component, f_component);

    for (int c = 0; c < ncells_owned; c++) {
      tcc_next[i][c] = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio[c];
    }
  }

  tcc_tmp->ScatterMasterToGhosted("cell");

  // corrector step
  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed in BJ for BCs

    double T = t_physics_;
    Epetra_Vector*& component = tcc_next(i);
    DudtOld(T, *component, f_component);

    for (int c = 0; c < ncells_owned; c++) {
      double value = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio[c];
      tcc_next[i][c] = (tcc_next[i][c] + value) / 2;
    }
  }

  // update mass balance
  for (int i = 0; i < num_aqueous + num_gaseous; i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_ / 2;
  }

  if (internal_tests_) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
* Advance each component independently due to different field
* reconstructions. This routine uses generic explicit time integrator. 
******************************************************************* */
void Transport_PK::AdvanceSecondOrderUpwindRKn(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step

  S_->GetFieldData(tcc_key_)->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  // define time integration method
  auto ti_method = Explicit_TI::forward_euler;
  if (temporal_disc_order == 2) {
    ti_method = Explicit_TI::heun_euler;
  } else if (temporal_disc_order == 3) {
    ti_method = Explicit_TI::tvd_3rd_order;
  } else if (temporal_disc_order == 4) {
    ti_method = Explicit_TI::runge_kutta_4th_order;
  }

  // We interpolate ws using dt which becomes local time.
  double T = 0.0; 
  // We advect only aqueous components.
  int ncomponents = num_aqueous;

  for (int i = 0; i < ncomponents; i++) {
    current_component_ = i;  // it is needed in BJ called inside RK:fun

    Epetra_Vector*& component_prev = tcc_prev(i);
    Epetra_Vector*& component_next = tcc_next(i);

    Explicit_TI::RK<Epetra_Vector> TVD_RK(*this, ti_method, *component_prev);
    TVD_RK.TimeStep(T, dt_, *component_prev, *component_next);
  }
}


/* ******************************************************************
* Adss source terms to conservative quantity tcc [mol]. Producers
* use the initial concentration vector tcc_prev. 
* The routine treats two cases of tcc with one and all components.
****************************************************************** */
void Transport_PK::ComputeSources_(
    double tp, double dtp, Epetra_MultiVector& tcc,
    const Epetra_MultiVector& tcc_prev, int n0, int n1)
{
  int num_vectors = tcc.NumVectors();
  int nsrcs = srcs_.size();

  for (int m = 0; m < nsrcs; m++) {
    double t0 = tp - dtp;
    srcs_[m]->Compute(t0, tp); 

    std::vector<int> index = srcs_[m]->tcc_index();
    for (auto it = srcs_[m]->begin(); it != srcs_[m]->end(); ++it) {
      int c = it->first;
      std::vector<double>& values = it->second; 

      for (int k = 0; k < index.size(); ++k) {
        int i = index[k];
        if (i < n0 || i > n1) continue;

        int imap = i;
        if (num_vectors == 1) imap = 0;

        double value = mesh_->cell_volume(c) * values[k];
        if (srcs_[m]->keyword() == "producer") {
          // correction for an extraction well
          value *= tcc_prev[imap][c];
        } else if (srcs_[m]->name() == "domain coupling") {
          value = values[k];         
        } else {
          // correction for non-SI concentration units
          if (srcs_[m]->name() == "volume" || srcs_[m]->name() == "weight")
              value /= units_.concentration_factor();
        }

        tcc[imap][c] += dtp * value;
        mass_solutes_source_[i] += value;
      }
    }
  }
}


/* *******************************************************************
* Populates operators' boundary data for given component.
* Returns true if at least one face was populated.
******************************************************************* */
bool Transport_PK::ComputeBCs_(
    std::vector<int>& bc_model, std::vector<double>& bc_value, int component)
{
  bool flag = false;

  for (int i = 0; i < bc_model.size(); i++) {
    bc_model[i] = Operators::OPERATOR_BC_NONE;
    bc_value[i] = 0.0;
  }

  AmanziMesh::Entity_ID_List cells;
  for (int f = 0; f < nfaces_wghost; f++) {
    mesh_->face_get_cells(f, AmanziMesh::Parallel_type::ALL, &cells);
    if (cells.size() == 1) bc_model[f] = Operators::OPERATOR_BC_NEUMANN;
  }

  for (int m = 0; m < bcs_.size(); m++) {
    std::vector<int>& tcc_index = bcs_[m]->tcc_index();
    int ncomp = tcc_index.size();

    for (auto it = bcs_[m]->begin(); it != bcs_[m]->end(); ++it) {
      int f = it->first;

      std::vector<double>& values = it->second; 

      for (int i = 0; i < ncomp; i++) {
        int k = tcc_index[i];
        if (k == component) {
          bc_model[f] = Operators::OPERATOR_BC_DIRICHLET;
          bc_value[f] = values[i];
          flag = true;
        }
      }
    }
  }

  return flag;
}


/* *******************************************************************
* Identify flux direction based on orientation of the face normal 
* and sign of the Darcy velocity.                               
******************************************************************* */
void Transport_PK::IdentifyUpwindCells()
{
  S_->GetFieldData(darcy_flux_key_)->ScatterMasterToGhosted("face");

  upwind_cells_.clear();
  downwind_cells_.clear();

  upwind_cells_.resize(nfaces_wghost);
  downwind_cells_.resize(nfaces_wghost);

  upwind_flux_.clear();
  downwind_flux_.clear();

  upwind_flux_.resize(nfaces_wghost);
  downwind_flux_.resize(nfaces_wghost);

  AmanziMesh::Entity_ID_List faces, cells;
  std::vector<int> dirs;

  if (mesh_->space_dimension() == mesh_->manifold_dimension()) {
    const Epetra_Map& cmap = mesh_->cell_map(true);

    for (int f = 0; f < nfaces_wghost; f++) {
      int ndofs = flux_map_->ElementSize(f);
      upwind_cells_[f].assign(ndofs, -1);
      downwind_cells_[f].assign(ndofs, -1);
      upwind_flux_[f].assign(ndofs, 0.0);
      downwind_flux_[f].assign(ndofs, 0.0);
    }
    
    for (int c = 0; c < ncells_wghost; c++) {
      mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
      
      for (int i = 0; i < faces.size(); i++) {
        int f = faces[i];
        mesh_->face_get_cells(f, AmanziMesh::Parallel_type::ALL, &cells);

        int g = flux_map_->FirstPointInElement(f);
        int ndofs = flux_map_->ElementSize(f);

        // We assume that two DOFs are placed only on internal faces
        if (ndofs == 2) {
          // define local position of DOF that the current cell controls
          int pos(0);
          int gid = cmap.GID(c);
          int gid_min = std::min(cmap.GID(cells[0]), cmap.GID(cells[1]));
          if (gid > gid_min) pos = 1;

          // define only upwind cell
          double tmp = (*darcy_flux)[0][g + pos] * dirs[i];

          if (tmp >= 0.0) {
            upwind_cells_[f][pos] = c;
            upwind_flux_[f][pos] = (*darcy_flux)[0][g + pos];
          } else {
            downwind_cells_[f][pos] = c;
            downwind_flux_[f][pos] = (*darcy_flux)[0][g + pos];
          }
        }
        else {
          double tmp = (*darcy_flux)[0][g] * dirs[i];
          if (tmp >= 0.0) {
            upwind_cells_[f][0] = c;
            upwind_flux_[f][0] = (*darcy_flux)[0][g];
          } else if (tmp < 0.0) {
            downwind_cells_[f][0] = c;
            downwind_flux_[f][0] = (*darcy_flux)[0][g];
          } else if (dirs[i] > 0) {
            upwind_cells_[f][0] = c;
            upwind_flux_[f][0] = (*darcy_flux)[0][g];            
          } else {
            downwind_cells_[f][0] = c;
            downwind_flux_[f][0] = (*darcy_flux)[0][g];
          }
        }
      }
    }

  } else {
    const Epetra_MultiVector& flux = *S_->GetFieldData(darcy_flux_fracture_key_)->ViewComponent("cell", true);
    S_->GetFieldData(darcy_flux_fracture_key_, passwd_)->ScatterMasterToGhosted();

    for (int c = 0; c < ncells_wghost; c++) {
      mesh_->cell_get_faces(c, &faces);

      for (int i = 0; i < faces.size(); i++) {
        int f = faces[i];
        double u = flux[i][c];
        if (u >= 0.0) {
          upwind_cells_[f].push_back(c);
          upwind_flux_[f].push_back(u);
        } else {
          downwind_cells_[f].push_back(c);
          downwind_flux_[f].push_back(u);
        }
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
  v_int.Update(b, v0, a, v1, 0.0);
}

}  // namespace Transport
}  // namespace Amanzi

