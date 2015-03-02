/*
  This is the flow component of the Amanzi code.
 
  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include "darcy_velocity_evaluator.hh"
#include "Flow_PK.hh"

namespace Amanzi {
namespace Flow {

/* ****************************************************************
* Calculates hydraulic head using h = z + (p - p0) / (rho * g)
**************************************************************** */
void aux_compute_hydraulic_head(
    Epetra_MultiVector& hydraulic_head, double p_atm, const Epetra_MultiVector& pressure, 
    double rho, AmanziGeometry::Point gravity, Epetra_Vector* centroids) 
{
  int dim = gravity.dim();

  hydraulic_head.PutScalar(-p_atm);
  hydraulic_head.Update(1.0, pressure, 1.0);
  
  double g = fabs(gravity[dim - 1]);
  
  hydraulic_head.Scale(1.0 / (g * rho));
  hydraulic_head.Update(1.0, *centroids, 1.0);
}


/* ****************************************************************
* Hydraulic head support for Darcy PK.
**************************************************************** */
void Flow_PK::UpdateAuxilliaryData() 
{
  Teuchos::OSTab tab = vo_->getOSTab();
  *vo_->os() << "Secondary fields: hydraulic head, etc..." << std::endl;  

  Epetra_MultiVector& hydraulic_head = *(S_->GetFieldData("hydraulic_head", passwd_)->ViewComponent("cell"));
  const Epetra_MultiVector& pressure = *(S_->GetFieldData("pressure")->ViewComponent("cell"));
  double rho = *(S_->GetScalarData("fluid_density"));

  const Epetra_BlockMap map = mesh_->cell_map(false);
  Epetra_Vector z_centroid(map);
  
  for (int c = 0; c != ncells_owned; ++c) {
    const AmanziGeometry::Point& xp = mesh_->cell_centroid(c); 
    z_centroid[c] = xp[dim-1]; 
  }

  aux_compute_hydraulic_head(
      hydraulic_head, atm_pressure_, pressure, rho, gravity_, &z_centroid);

  darcy_flux_eval_->SetFieldAsChanged(S_.ptr());
  S_->GetFieldEvaluator("darcy_velocity")->HasFieldChanged(S_.ptr(), "darcy_velocity");
}

}  // namespace Flow
}  // namespace Amanzi
