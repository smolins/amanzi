/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
#include "species.hh"

#include <cmath>

#include <sstream>
#include <iostream>
#include <iomanip>

#include "chemistry_exception.hh"

#include "VerboseObject.hh"
#include "exceptions.hh"

namespace Amanzi {
namespace AmanziChemistry {

extern VerboseObject* chem_out;

Species::Species()
    : molality_(1.e-9),
      activity_(1.0),
      act_coef_(1.0),
      ln_molality_(0.0),
      ln_activity_(0.0),
      ln_act_coef_(0.0),
      identifier_(0),
      charge_(0),
      gram_molecular_weight_(0.0),
      ion_size_parameter_(0.0),
      name_("") {
  //  ActivityCoefficient* activityCoefficient;
}  // end Species constructor


Species::Species(SpeciesId id, SpeciesName name, double charge, double mol_wt,
                 double size)
    : molality_(1.e-9),
      activity_(1.0),
      act_coef_(1.0),
      ln_molality_(0.0),
      ln_activity_(0.0),
      ln_act_coef_(0.0),
      identifier_(id),
      charge_(charge),
      gram_molecular_weight_(mol_wt),
      ion_size_parameter_(size),
      name_(name) {
  //  ActivityCoefficient* activityCoefficient;
  if (identifier() < 0) {
    std::ostringstream error_stream;
    error_stream << "Species::Species(): \n"
                 << "invalid identifier (id < 0), id = " << identifier() << std::endl;
    Exceptions::amanzi_throw(ChemistryInvalidInput(error_stream.str()));
  }
  if (gram_molecular_weight() < 0.0) {
    std::ostringstream error_stream;
    error_stream << "Species::Species(): \n"
                 << "invalid gram molecular weight "
                 << "(gmw < 0.0), gmw = " << gram_molecular_weight() << std::endl;
    Exceptions::amanzi_throw(ChemistryInvalidInput(error_stream.str()));
  }
  if (ion_size_parameter() < 0.0) {
    std::ostringstream error_stream;
    error_stream << "Species::Species(): \n"
                 << "invalid ion size parameter "
                 << "(size < 0.0), size = " << ion_size_parameter() << std::endl;
    Exceptions::amanzi_throw(ChemistryInvalidInput(error_stream.str()));
  }
}  // end Species constructor

Species::~Species() {
}  // end Species destructor

void Species::update(const double molality) {
  molality_ = molality;
  // note that activity coefficient not updated
  // act_coef_ = 1.;
  activity_ = act_coef_ * molality_;
  ln_molality_ = std::log(molality_);
  ln_act_coef_ = std::log(act_coef_);
  ln_activity_ = ln_molality_ + ln_act_coef_;
}  // end update()

void Species::update(void) {
  activity_ = act_coef_ * molality_;
  ln_molality_ = std::log(molality_);
  ln_act_coef_ = std::log(act_coef_);
  ln_activity_ = ln_molality_ + ln_act_coef_;
}  // end update()

void Species::display(void) const {
  std::cout << "    " << name();
  std::cout << std::endl;
  std::cout << "        charge = " << charge() << std::endl;
  std::cout << "        mol wt = " << gram_molecular_weight() << std::endl;
}  // end display()

void Species::Display(void) const {
  std::stringstream message;
  message << std::setw(15) << name() << std::fixed
          << std::setprecision(2) << std::setw(10) << charge()
          << std::setprecision(5) << std::setw(10) << gram_molecular_weight()
          << std::setprecision(2) << std::setw(10) << ion_size_parameter()
          << std::endl;
  chem_out->Write(Teuchos::VERB_HIGH, message);
}  // end Display()

void Species::DisplayResultsHeader(void) const {
  std::stringstream message;
  message << std::setw(15) << "Name"
          << std::setw(15) << "Molality"
          << std::setw(15) << "Activity Coeff"
          << std::setw(15) << "Activity"
          << std::endl;
  chem_out->Write(Teuchos::VERB_HIGH, message);
}  // end DisplayResultsHeader()

void Species::DisplayResults(void) const {
  std::stringstream message;
  message << std::setw(15) << name()
          << std::scientific << std::setprecision(5)
          << std::setw(15) << molality()
          << std::setw(15) << act_coef()
          << std::setw(15) << activity()
          << std::endl;
  chem_out->Write(Teuchos::VERB_HIGH, message);
}  // end DisplayResults()

}  // namespace AmanziChemistry
}  // namespace Amanzi
