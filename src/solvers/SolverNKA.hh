/*
  This is the Nonlinear Solver component of the Amanzi code.

  Interface for using NKA as a solver.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#ifndef AMANZI_NKA_SOLVER_
#define AMANZI_NKA_SOLVER_

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"

#include "VerboseObject.hh"

#include "Solver.hh"
#include "SolverFnBase.hh"
#include "SolverDefs.hh"
#include "NKA_Base.hh"

namespace Amanzi {
namespace AmanziSolvers {

template<class Vector, class VectorSpace>
class SolverNKA : public Solver<Vector> {
 public:
  SolverNKA(Teuchos::ParameterList& plist,
            const Teuchos::RCP<SolverFnBase<Vector> >& fn,
            const VectorSpace& map);

  virtual int Solve(const Teuchos::RCP<Vector>& u);

  // access
  int num_itrs() { return num_itrs_; }
  int pc_calls() { return pc_calls_; }

 protected:
  void Init_();

 protected:
  Teuchos::ParameterList plist_;
  Teuchos::RCP<SolverFnBase<Vector> > fn_;
  Teuchos::RCP<NKA_Base<Vector, VectorSpace> > nka_;

  Teuchos::RCP<VerboseObject> vo_;

  double nka_tol_;
  int nka_dim_;

 private:
  double tol_, overflow_tol_;

  int max_itrs_, num_itrs_;
  int fun_calls_, pc_calls_;
  int pc_lag_, update_pc_calls_;
  int nka_lag_space_, nka_lag_iterations_;
  int max_error_growth_factor_, max_du_growth_factor_;
  int max_divergence_count_;
  int monitor_;
};


/* ******************************************************************
* Constructor.
****************************************************************** */
template<class Vector, class VectorSpace>
SolverNKA<Vector, VectorSpace>::SolverNKA(Teuchos::ParameterList& plist,
                                          const Teuchos::RCP<SolverFnBase<Vector> >& fn,
                                          const VectorSpace& map) :
    plist_(plist), fn_(fn)
{
  Init_();

  // create the NKA space
  nka_ = Teuchos::rcp(new NKA_Base<Vector, VectorSpace>(nka_dim_, nka_tol_, map));
  nka_->Init(plist);
}


/* ******************************************************************
* Initialization of the NKA solver
****************************************************************** */
template<class Vector, class VectorSpace>
void SolverNKA<Vector, VectorSpace>::Init_()
{
  tol_ = plist_.get<double>("nonlinear tolerance", 1.e-6);
  overflow_tol_ = plist_.get<double>("diverged tolerance", 1.0e10);
  max_itrs_ = plist_.get<int>("limit iterations", 50);
  max_du_growth_factor_ = plist_.get<double>("max du growth factor", 1.0e5);
  max_error_growth_factor_ = plist_.get<double>("max error growth factor", 1.0e5);
  max_divergence_count_ = plist_.get<int>("max divergent iterations", 3);
  monitor_ = SOLVER_MONITOR_UPDATE;

  nka_dim_ = plist_.get<int>("max nka vectors", 10);
  nka_dim_ = std::min<int>(nka_dim_, max_itrs_ - 1);
  nka_tol_ = plist_.get<double>("nka vector tolerance", 0.05);

  fun_calls_ = 0;
  pc_calls_ = 0;
  update_pc_calls_ = 0;

  // update the verbose options
  vo_ = Teuchos::rcp(new VerboseObject("AmanziSolver::NKA", plist_));
}


/* ******************************************************************
* The body of NKA solver
****************************************************************** */
template<class Vector, class VectorSpace>
int SolverNKA<Vector, VectorSpace>::Solve(const Teuchos::RCP<Vector>& u) {
  Teuchos::OSTab tab = vo_->getOSTab();

  // restart the nonlinear solver (flush its history)
  nka_->Restart();

  // initialize the iteration counter
  num_itrs_ = 0;

  // create storage
  Teuchos::RCP<Vector> r = Teuchos::rcp(new Vector(*u));
  Teuchos::RCP<Vector> du = Teuchos::rcp(new Vector(*u));
  Teuchos::RCP<Vector> du_tmp = Teuchos::rcp(new Vector(*u));

  // variables to monitor the progress of the nonlinear solver
  double error(0.0), previous_error(0.0), l2_error(0.0);
  double l2_error_initial(0.0);
  double du_norm(0.0), previous_du_norm(0.0);
  int divergence_count(0);

  do {
    // Check for too many nonlinear iterations.
    if (num_itrs_ > max_itrs_) {
      if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH)
        *vo_->os() << "Solve reached maximum of iteration " << num_itrs_ 
                   << "  error=" << error << std::endl;
      return SOLVER_MAX_ITERATIONS;
    }

    // Update the preconditioner if necessary.
    if (num_itrs_ % (pc_lag_ + 1) == 0) {
      update_pc_calls_++;
      fn_->UpdatePreconditioner(u);
    }

    // Increment iteration counter.
    num_itrs_++;

    // Evaluate the nonlinear function.
    fun_calls_++;
    fn_->Residual(u, r);

    // If monitoring the residual, check for convergence.
    if (monitor_ == SOLVER_MONITOR_RESIDUAL) {
      previous_error = error;
      error = fn_->ErrorNorm(u, r);
      r->Norm2(&l2_error);

      if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
        *vo_->os() << num_itrs_ << ": error=" << error << "  L2-error=" << l2_error << endl;

      // We attempt to catch non-convergence early.
      if (num_itrs_ == 1) {
        l2_error_initial = l2_error;
      } else if (num_itrs_ > 8) {
        if (l2_error > l2_error_initial) {
          if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
            *vo_->os() << "Solver stagnating, L2-error=" << l2_error
                       << " > " << l2_error_initial << " (initial L2-error)" << endl;
          return SOLVER_STAGNATING;
        }
      }

      if (error < tol_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
          *vo_->os() << "Solver converged: " << num_itrs_ << " itrs, error=" << error << endl;
        return SOLVER_CONVERGED;
      } else if (error > overflow_tol_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_LOW) 
          *vo_->os() << "Solve failed, error " << error << " > "
                     << overflow_tol_ << " (overflow)" << endl;
        return SOLVER_OVERFLOW;
      } else if ((num_itrs_ > 1) && (error > max_error_growth_factor_ * previous_error)) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_LOW) 
          *vo_->os() << "Solver threatens to overflow, error " << error << " > "
                     << previous_error << " (previous error)" << endl;
        return SOLVER_OVERFLOW;
      }
    }

    // Apply the preconditioner to the nonlinear residual.
    pc_calls_++;
    fn_->ApplyPreconditioner(r, du_tmp);

    // Calculate the accelerated correction.
    if (num_itrs_ <= nka_lag_space_) {
      // Lag the NKA space, just use the PC'd update.
      *du = *du_tmp;
    } else {
      if (num_itrs_ <= nka_lag_iterations_) {
        // Lag NKA's iteration, but update the space with this Jacobian info.
        nka_->Correction(*du_tmp, *du, du);
        *du = *du_tmp;
      } else {
        // Take the standard NKA correction.
        nka_->Correction(*du_tmp, *du, du);
      }
    }

    // Hack the correction
    bool hacked = fn_->ModifyCorrection(r, u, du);
    if (hacked) {
      // If we had to hack things, it't not unlikely that the Jacobian
      // information is crap. Take the hacked correction, and restart
      // NKA to start building a new Jacobian space.
      nka_->Restart();
    }

    // Make sure that we do not diverge and cause numerical overflow.
    previous_du_norm = du_norm;
    du->NormInf(&du_norm);

    if ((num_itrs_ > 1) && (du_norm > max_du_growth_factor_ * previous_du_norm)) {
      // try to recover by restarting NKA
      nka_->Restart();

      // ... this is the first invocation of nka_correction with an empty
      // nka space, so we call it withoug du_last, since there isn't one
      nka_->Correction(*du_tmp, *du);

      // re-check du
      du->NormInf(&du_norm);
      if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
        *vo_->os() << "Solver threatens to overflow, trying to restart NKA." << endl
                   << "  ||du||=" << du_norm << ", ||du_prev||=" << previous_du_norm << endl;

      // If it fails again, give up.
      if ((num_itrs_ > 1) && (du_norm > max_du_growth_factor_ * previous_du_norm)) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) 
           *vo_->os() << "Solver threatens to overflow: FAIL." << endl
                      << "  ||du||=" << du_norm << ", ||du_prev||=" << previous_du_norm << endl;
        return SOLVER_OVERFLOW;
      }
    }

    // Keep track of diverging iterations
    if (num_itrs_ > 1 && du_norm >= previous_du_norm) {
      divergence_count++;

      // If it does not recover quickly, abort.
      if (divergence_count == max_divergence_count_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_LOW)
          *vo_->os() << "Solver is diverging repeatedly, FAIL." << endl;
        return SOLVER_DIVERGING;
      }
    } else {
      divergence_count = 0;
    }

    // Next solution iterate and error estimate: u  = u - du
    u->Update(-1.0, *du, 1.0);
    fn_->ChangedSolution();

    // Monitor the PC'd residual.
    if (monitor_ == SOLVER_MONITOR_PCED_RESIDUAL) {
      previous_error = error;
      error = fn_->ErrorNorm(u, du_tmp);
      du_tmp->Norm2(&l2_error);

      if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
        *vo_->os() << num_itrs_ << ": error (prec)=" << error
                   << "  L2-error (prec)=" << l2_error << endl;

      // Check for convergence.
      if (error < tol_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
          *vo_->os() << "Solver converged: " << num_itrs_ << " itrs, error=" << error << endl;
        return SOLVER_CONVERGED;
      } else if (error > overflow_tol_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_LOW) 
          *vo_->os() << "Solve failed, error " << error << " > "
                     << overflow_tol_ << " (ovreflow)" << std::endl;
        return SOLVER_OVERFLOW;
      } else if ((num_itrs_ > 1) && (error > max_error_growth_factor_ * previous_error)) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_LOW) 
          *vo_->os() << "Solver threatens to overflow: " << error << " > " 
                     << previous_error << " (previous error)" << endl;
        return SOLVER_OVERFLOW;
      }
    }

    // Monitor the NKA'd PC'd residual.
    if (monitor_ == SOLVER_MONITOR_UPDATE) {
      previous_error = error;
      error = fn_->ErrorNorm(u, du);
      du->Norm2(&l2_error);

      if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
        *vo_->os() << num_itrs_ << ": error=" << error << ",  L2-error=" << l2_error << endl;

      // Check for convergence.
      if (error < tol_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) 
          *vo_->os() << "Solver converged: " << num_itrs_ << " itrs, error=" << error << endl;
        return SOLVER_CONVERGED;
      } else if (error > overflow_tol_) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) 
          *vo_->os() << "Solver failed, error " << error << " > "
                     << overflow_tol_ << " (overflow)" << endl;
        return SOLVER_OVERFLOW;
      } else if ((num_itrs_ > 1) && (error > max_error_growth_factor_ * previous_error)) {
        if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) 
          *vo_->os() << "Solver threatens to overflow, FAIL." << endl
                     << " ||error||=" << error << ", ||error_prev||=" << previous_error << endl;
        return SOLVER_OVERFLOW;
      }
    }
  } while (true);
}

}  // namespace AmanziSolvers
}  // namespace Amanzi

#endif