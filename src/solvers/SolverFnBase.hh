/*
  This is the Nonlinear Solver component of the Amanzi code.

  Interface for an evaluator required for a nonlinear solver.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/


#ifndef AMANZI_SOLVER_FN_BASE_
#define AMANZI_SOLVER_FN_BASE_

#include "Teuchos_RCP.hpp"

namespace Amanzi {
namespace AmanziSolvers {

template<class Vector>
class SolverFnBase {
 public:
  // computes the non-linear functional r = F(u)
  virtual void Residual(const Teuchos::RCP<Vector>& u, 
                        const Teuchos::RCP<Vector>& r) = 0;

  // preconditioner toolkit
  virtual void ApplyPreconditioner(const Teuchos::RCP<const Vector>& r,
                                   const Teuchos::RCP<Vector>& Pr) = 0;
  virtual void UpdatePreconditioner(const Teuchos::RCP<const Vector>& u) = 0;

  // error analysis
  virtual double ErrorNorm(const Teuchos::RCP<const Vector>& u, 
                           const Teuchos::RCP<const Vector>& du) = 0;

  // allow PK to modify a correction
  virtual bool ModifyCorrection(const Teuchos::RCP<const Vector>& r, 
                                const Teuchos::RCP<const Vector>& u, 
                                const Teuchos::RCP<Vector>& du) { return false; }

  // bookkeeping for state
  virtual void ChangedSolution() = 0;
};

}  // namespace AmanziSolvers
}  // namespace Amanzi

#endif