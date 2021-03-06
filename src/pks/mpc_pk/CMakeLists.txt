# -*- mode: cmake -*-

#
#  Amanzi
#   Energy process kernel
#

# Amanzi module, include files found in AMANZI_MODULE_PATH
include(PrintVariable)
include(TestManager)

#
# Define a project name
# After this command the following varaibles are defined
#   MPC_TREE_SOURCE_DIR
#   MPC_TREE_BINARY_DIR
# Other projects (subdirectories) can reference this directory
# through these variables.
project(MPC_TREE)

# Get the includes directory for chemistry (needed for ReactiveTransport).
get_property(CHEM_INCLUDES_DIR GLOBAL PROPERTY CHEM_INCLUDES_DIR)

# Amanzi include directories
include_directories(${ATK_SOURCE_DIR})
include_directories(${DBC_SOURCE_DIR})
include_directories(${DBG_SOURCE_DIR})
include_directories(${CHEM_INCLUDES_DIR})
include_directories(${CHEMPK_SOURCE_DIR})
include_directories(${MESH_SOURCE_DIR})
include_directories(${DATA_STRUCTURES_SOURCE_DIR})
include_directories(${OPERATORS_SOURCE_DIR})
include_directories(${PKS_SOURCE_DIR})
include_directories(${STATE_SOURCE_DIR})
include_directories(${SOLVERS_SOURCE_DIR})
include_directories(${WHETSTONE_SOURCE_DIR})
include_directories(${TIME_INTEGRATION_SOURCE_DIR})
include_directories(${TRANSPORT_SOURCE_DIR})

# External (TPL) include directories
include_directories(${Teuchos_INCLUDE_DIRS})
include_directories(${Epetra_INCLUDE_DIRS})
include_directories(${ASCEMIO_INCLUDE_DIR})

#
# MPC_PKS registrations
#
register_evaluator_with_factory(
  HEADERFILE FlowReactiveTransport_PK_reg.hh 
  LISTNAME   MPC_PKS_REGISTRATIONS
  )

register_evaluator_with_factory(
  HEADERFILE FlowEnergy_PK_reg.hh 
  LISTNAME   MPC_PKS_REGISTRATIONS
  INSTALL    True
  )

register_evaluator_with_factory(
  HEADERFILE ReactiveTransport_PK_reg.hh
  LISTNAME   MPC_PKS_REGISTRATIONS
  )

generate_evaluators_registration_header( 
  HEADERFILE mpc_pks_registration.hh 
  LISTNAME   MPC_PKS_REGISTRATIONS
  INSTALL    True
  )


#
# Library: mpc_tree
#
set(mpc_tree_src_files 
    PK_MPCWeak.cc 
    PK_MPCSubcycled.cc 
    ReactiveTransport_PK.cc
    FlowEnergy_PK.cc
    FlowReactiveTransport_PK.cc)

set(mpc_tree_tpl_libs ${Teuchos_LIBRARIES} ${Epetra_LIBRARIES})
add_amanzi_library(mpc_tree SOURCE ${mpc_tree_src_files}
                   LINK_LIBS ${mpc_tree_tpl_libs} time_integration data_structures state)

#
# Install Targets
#
set(mpc_tree_inc_files
    PK_MPCStrong.hh
    PK_MPCWeak.hh
    PK_MPCSubcycled.hh
    PK_MPC.hh
    PK_MPCAdditive.hh
    ReactiveTransport_PK.hh
    FlowEnergy_PK.hh
    FlowReactiveTransport_PK.hh)

add_install_include_file(${mpc_tree_inc_files} mpc_pks_registration.hh)

if (BUILD_TESTS) 
    # Add UnitTest include directoy
    include_directories(${UnitTest_INCLUDE_DIRS})

    # Copy test directory files if an out of source build
    if (NOT (${MPC_TREE_SOURCE_DIR} EQUAL ${MPC_TREE_BINARY_DIR}) )
        execute_process(COMMAND ${CMAKE_COMMAND} -E 
          copy_directory ${MPC_TREE_SOURCE_DIR}/test ${MPC_TREE_BINARY_DIR}/test) 
    endif()

    # Add the flow directory to the include paths
    include_directories(${MPC_TREE_SOURCE_DIR})

    set(amanzi_libs geometry mesh mesh_audit mesh_factory state energy operators whetstone data_structures)

    # Test: 1
    #add_amanzi_test(energy_test energy_test
    #                KIND int
    #                SOURCE test/Main.cc test/energy_test.cc
    #                LINK_LIBS ${amanzi_libs} ${UnitTest_LIBRARIES})
endif()

