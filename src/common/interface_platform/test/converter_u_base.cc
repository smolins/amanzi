/*
  This is the input component of the Amanzi code. 

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

// TPLs
#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_XMLParameterListHelpers.hpp"
#include "UnitTest++.h"
#include "XMLParameterListWriter.hh"

// Amanzi
#include "InputConverterU.hh"


/* **************************************************************** */
TEST(CONVERTER_BASE) {
  using namespace Teuchos;
  using namespace Amanzi;

  Epetra_MpiComm comm(MPI_COMM_WORLD);
  int MyPID = comm.MyPID();
  if (MyPID == 0) std::cout << "Test: convert transport test" << std::endl;

  // read parameter list
  std::string xmlFileName = "test/converter_u_base.xml";
  // std::string xmlFileName = "test/porflow.xml";

  Amanzi::AmanziInput::InputConverterU converter;
  converter.Init(xmlFileName);
  Teuchos::ParameterList new_xml = converter.Translate();
  // std::cout << new_xml << std::endl;

  Teuchos::Amanzi_XMLParameterListWriter XMLWriter;
  Teuchos::XMLObject XMLobj = XMLWriter.toXML(new_xml);

  std::ofstream xmlfile;
  xmlfile.open("native_v6.xml");
  xmlfile << XMLobj;
}
	