#  -*- mode: cmake -*-

#
# Amanzi SuperBuild
#       Amanzi TPL Build Targets
cmake_minimum_required(VERSION 3.2)

# cmake_policy(SET CMP0017 NEW)
# cmake_policy(SET CMP0022 NEW)
# cmake_policy(SET CMP0037 OLD)
# cmake_policy(SET CMP0048 OLD)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ############################################################################ #
# Compiler Definition Warnings
# ############################################################################ #
if ( NOT CMAKE_C_COMPILER )
  message(WARNING "C compiler not specified. CMake will guess!")
endif()

if ( NOT CMAKE_CXX_COMPILER )
  message(WARNING "C++ compiler not specified. CMake will guess!")
endif()

if ( NOT CMAKE_Fortran_COMPILER )
  message(WARNING "Fortran compiler not specified. CMake will guess!")
endif()

# ############################################################################ #
# Define project name (SuperBuild_SOURCE_DIR and SuperBuild_BINARY_DIR)
# ############################################################################ #
project(SuperBuild C CXX Fortran)

# ############################################################################ #
# Define Amanzi directories 
# ############################################################################ #
file(TO_CMAKE_PATH ${SuperBuild_SOURCE_DIR}/../.. Amanzi_SOURCE_DIR)

# ############################################################################ #
# Update the CMake module path
# ############################################################################ #
set(Amanzi_MODULE_PATH
           ${Amanzi_SOURCE_DIR}/tools/cmake
           ${Amanzi_SOURCE_DIR}/tools/cmake/Utils
           ${Amanzi_SOURCE_DIR}/tools/cmake/Modules)

set(CMAKE_MODULE_PATH 
         ${CMAKE_MODULE_PATH} 
	     ${SuperBuild_SOURCE_DIR}/cmake
	     ${Amanzi_MODULE_PATH})

# ############################################################################ #
# Location of the Build_* files and template files
# ############################################################################ #
set(SuperBuild_BUILD_FILES_DIR    "${SuperBuild_SOURCE_DIR}/include")
set(SuperBuild_TEMPLATE_FILES_DIR "${SuperBuild_SOURCE_DIR}/templates")

# ############################################################################ #
# Compiler checks
# ############################################################################ #
include(FortranCInterface)
FortranCInterface_VERIFY()

include(DefineCompilerVersion)
define_compiler_version()

include(CheckMPISourceCompiles)
check_mpi_source_compiles(MPI_WRAPPERS_IN_USE)
if (NOT MPI_WRAPPERS_IN_USE)
  # MPI_WRAPPERS_IN_USE is FALSE if any of the compilers fail to compile an MPI app
  #message(FATAL_ERROR "Missing a compiler type for MPI applications -- exiting.
  message(WARING "Missing a compiler type for MPI applications.
  See previous error messages to determine missing compiler.")
endif()


include(${SuperBuild_SOURCE_DIR}/TPLVersions.cmake)
# prepare the version numbers file
set(TPL_VERSIONS_INCLUDE_FILE ${CMAKE_BINARY_DIR}/tpl_versions.h)
file(WRITE ${TPL_VERSIONS_INCLUDE_FILE} "")
install(FILES ${CMAKE_BINARY_DIR}/tpl_versions.h DESTINATION include)
amanzi_tpl_version_write(FILENAME ${TPL_VERSIONS_INCLUDE_FILE}
  PREFIX AMANZI
  VERSION ${AMANZI_TPLS_VERSION_MAJOR} ${AMANZI_TPLS_VERSION_MINOR} ${AMANZI_TPLS_VERSION_PATCH})


# ############################################################################ #
# Build options
# ############################################################################ #

message(STATUS "Setting build type")
set(SuperBuild_BUILD_TYPE_DFLT "Release")
if ( NOT CMAKE_BUILD_TYPE ) 
  set(CMAKE_BUILD_TYPE ${SuperBuild_BUILD_TYPE_DFLT})
endif()
message(STATUS "Setting build type -- ${CMAKE_BUILD_TYPE}")

# Download directory TPL_DOWNLOAD_DIR
set(TPL_DOWNLOAD_DIR_DFLT ${SuperBuild_BINARY_DIR}/Downloads)
if (NOT TPL_DOWNLOAD_DIR)
  set(TPL_DOWNLOAD_DIR ${TPL_DOWNLOAD_DIR_DFLT})
endif()
message(STATUS "Download TPL files in ${TPL_DOWNLOAD_DIR}") 

# Disable external web downloads
option(DISABLE_EXTERNAL_DOWNLOAD "Flag to disable external web downloads" FALSE)
if (DISABLE_EXTERNAL_DOWNLOAD)
  message(STATUS "Will not download files from the web, instead will search "
                 "for archive files in ${TPL_DOWNLOAD_DIR}")
else()
  include(CheckDownload)
  check_download(TEST_FILE MD5SUMS TEST_URL http://software.lanl.gov/ascem/tpls)
endif()

# TPL install location
set(TPL_INSTALL_PREFIX_DFLT "${SuperBuild_BINARY_DIR}/external-projects")
if ( NOT TPL_INSTALL_PREFIX )
  set(TPL_INSTALL_PREFIX ${TPL_INSTALL_PREFIX_DFLT})
endif()
message(STATUS "Install TPLs in ${TPL_INSTALL_PREFIX}")

# also set the CMAKE_INSTALL_PREFIX accordingly
set(CMAKE_INSTALL_PREFIX ${TPL_INSTALL_PREFIX})

# Amanzi structured AMR mesh API and tools
option(ENABLE_Structured "Enable Amanzi structured mesh capability" TRUE)
option(ENABLE_CCSE_TOOLS "Enable building CCSE post processing tools" FALSE)

# Amanzi unstructured mesh API and frameworks
option(ENABLE_Unstructured "Enable Amanzi unstructured mesh capability" TRUE)
option(ENABLE_STK_Mesh "Enable the Unstructured STK Mesh Toolkit" TRUE)
option(ENABLE_MSTK_Mesh "Enable the Unstructured MSTK Mesh Toolkit" TRUE)
option(ENABLE_MOAB_Mesh "Enable the Unstructured MOAB Mesh Toolkit" FALSE)

# Amanzi output formats
option(ENABLE_Silo "Enable the Silo output specification for visualization" FALSE)

# OpenMP Search - ENABLED ONLY WHEN STRUCTURED IS ON BECAUSE
# WE SUSPECT THAT IT MAY NOT BE WORKING WELL WITH HYPRE ON
# THE UNSTRUCTURED SIDE
if ( ENABLE_Structured )
#option(ENABLE_OpenMP "Add OpenMP directives to the build" TRUE)
set (ENABLE_OpenMP FALSE)
if ( ENABLE_OpenMP )
  find_package(OpenMP)
  find_package(OpenMP_Fortran)
  if ( OPENMP_FOUND )
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} ${OpenMP_Fortran_FLAGS}")
    set(CMAKE_LINK_FLAGS "${CMAKE_LINK_FLAGS} ${OpenMP_LINK_FLAGS}")
  else()  
    message(SEND_ERROR "Failed to determine the OpenMP compiler flags. Will deactivate.")
    set(ENABLE_OpenMP FALSE CACHE BOOL "Enable OpenMP compile directives" FORCE)
  endif()
endif()
endif()

# If unstructured is not enabled, there is no reason to have these mesh frameworks
if ( NOT ENABLE_Unstructured )
  set (ENABLE_STK_Mesh FALSE)
  set (ENABLE_MSTK_Mesh FALSE)
  set (ENABLE_MOAB_Mesh FALSE)
endif()

# Amanzi Test Suite
option(ENABLE_TESTS "Build the Amanzi Test Suite" TRUE)


# ############################################################################ #
# MPI Configuration 
# ############################################################################ #

# --- The FindMPI package for CMake for versions 2.8.6> use compiler wrappers
#     to determine include paths, link flags and libraries for each language.
if ( MPI_WRAPPERS_IN_USE )

  set(MPI_C_COMPILER ${CMAKE_C_COMPILER})
  set(MPI_CXX_COMPILER ${CMAKE_CXX_COMPILER})
  set(MPI_Fortran_COMPILER ${CMAKE_Fortran_COMPILER})

else()

  message(STATUS "Searching for MPI compiler wrappers to define"
                 " compile and link flags")

  find_program(MPI_C_COMPILER
               NAME mpicc
               HINTS ${MPI_INSTALL_PREFIX} ENV MPIROOT ENV MPI_ROOT ENV MPI_HOME ENV MPIHOME ENV MPICH_ROOT 
               PATH_SUFFIXES bin Bin
               )

  find_program(MPI_CXX_COMPILER
               NAMES mpicxx mpic++ mpiCC
               HINTS ${MPI_INSTALL_PREFIX} ENV MPIROOT ENV MPI_ROOT ENV MPI_HOME ENV MPIHOME ENV MPICH_ROOT 
               PATH_SUFFIXES bin Bin
               )

  find_program(MPI_Fortran_COMPILER
               NAMES mpif90
               HINTS ${MPI_INSTALL_PREFIX} ENV MPIROOT ENV MPI_ROOT ENV MPI_HOME ENV MPIHOME ENV MPICH_ROOT 
               PATH_SUFFIXES bin Bin
               )

endif()

# --- This package find call will set
#     MPI_<lang>_FOUND
#     MPI_<lang>_COMPILE_FLAGS
#     MPI_<lang>_INCLUDE_PATH
#     MPI_<lang>_LINK_FLAGS
#     MPI_<lang>_LIBRARIES
#
#     MPIEXEC
#     MPIEXEC_NUMPROC_FLAG
#     MPIEXEC_PREFLAGS
#     MPIEXEC_POSTFLAGS
find_package(MPI)

# The FindMPI package uses FindPackageHandleStandardArgs
# to MPI_<lang>_FOUND. For consistency, I set MPI_FORTRAN_FOUND
# to MPI_Fortran_FOUND
set(MPI_Fortran_FOUND ${MPI_FORTRAN_FOUND})

# Warn the user if MPI information is not found
if ( NOT MPI_C_FOUND )
  message(WARNING "Failed to locate MPI C include and library files")
endif()

if ( NOT MPI_CXX_FOUND )
  message(WARNING "Failed to locate MPI C++ include and library files")
endif()

if ( NOT MPI_Fortran_FOUND )
  message(WARNING "Failed to locate MPI Fortran include and library files")
endif()

# Add MPI to the build if any MPI_*_FOUND == FALSE
if ( MPI_C_FOUND AND MPI_CXX_FOUND AND MPI_Fortran_FOUND )
  message(STATUS "Found MPI")
  set(BUILD_MPI FALSE)
  set(mpi_var_list COMPILER COMPILE_FLAGS INCLUDE_PATH LINK_FLAGS LIBRARIES)
  foreach ( lang C CXX Fortran )
    foreach ( var ${mpi_var_list} )
      set(var MPI_${lang}_${var})
      if ("${var}")
	message(STATUS "      ${var}: ${${var}}")
      endif()
    endforeach()
  endforeach()  
else()
  message(STATUS "Will build MPI with the other TPLs")
  set(BUILD_MPI TRUE)
  set(MPI_C_COMPILER       ${TPL_INSTALL_PREFIX}/bin/mpicc)
  set(MPI_CXX_COMPILER     ${TPL_INSTALL_PREFIX}/bin/mpicxx)
  set(MPI_Fortran_COMPILER ${TPL_INSTALL_PREFIX}/bin/mpif90)
  set(MPIEXEC              ${TPL_INSTALL_PREFIX}/bin/mpirun)
endif()

# --- Add the include paths, link flags and libraries if using a 
#     user requested MPI but not the wrappers from this MPI installation.
if ( NOT MPI_WRAPPERS_IN_USE AND NOT BUILD_MPI )
  
  foreach ( lang C CXX Fortran )
    # Build MPI compile flags
    set(add_mpi_flags_list)
    if (MPI_${lang}_COMPILE_FLAGS)
      foreach (flag ${MPI_${lang}_COMPILE_FLAGS} )
	list(APPEND add_mpi_flags_list ${flag})
      endforeach()
    endif()
    if (MPI_${lang}_INCLUDE_PATH)
      foreach (dir ${MPI_${lang}_INCLUDE_PATH} )
	list(APPEND add_mpi_flags_list "-I${dir}")
      endforeach()
    endif() 
    if (MPI_${lang}_LINK_FLAGS)
      foreach (flag ${MPI_${lang}_LINK_FLAGS} )
	list(APPEND add_mpi_flags_list ${flag})
      endforeach()
    endif()
    list(REMOVE_DUPLICATES add_mpi_flags_list)
    list(LENGTH add_mpi_flags_list num_flags)
    if ( "${num_flags}" GREATER "0" )
      include(BuildWhitespaceString)
      if ( CMAKE_${lang}_FLAGS )
	build_whitespace_string(new_compile_flags ${CMAKE_${lang}_FLAGS} ${add_mpi_flags_list})
      else()	
	build_whitespace_string(new_compile_flags ${add_mpi_flags_list})
      endif()
      set(CMAKE_${lang}_FLAGS "${new_compile_flags}")
      message(STATUS "Update CMAKE_${lang}_FLAGS: ${CMAKE_${lang}_FLAGS}")
    endif()  


  endforeach()

  
endif()

# --- Define which compilers to use in MPI dependent projects
#     If building MPI as an external project use MPI_* compiler
#     otherwise use the CMAKE_* compilers which are either
#     MPI compiler wrappers OR have the neccessary flags to build.
if ( BUILD_MPI )
  set(CMAKE_C_COMPILER_USE       ${MPI_C_COMPILER})
  set(CMAKE_CXX_COMPILER_USE     ${MPI_CXX_COMPILER})
  set(CMAKE_Fortran_COMPILER_USE ${MPI_Fortran_COMPILER})
else()  
  set(CMAKE_C_COMPILER_USE       ${CMAKE_C_COMPILER})
  set(CMAKE_CXX_COMPILER_USE     ${CMAKE_CXX_COMPILER})
  set(CMAKE_Fortran_COMPILER_USE ${CMAKE_Fortran_COMPILER})
endif()  

# --- Amanzi uses MPI_EXEC* not MPIEXEC* variables. This allows the user to 
#     override the find package results.

# - MPI execute binary
if (NOT MPI_EXEC)

  if ( MPIEXEC )
    set(MPI_EXEC "${MPIEXEC}" CACHE STRING "Select MPI executable from FindMPI results")
  else()
    find_program(MPI_EXEC
                 NAMES mpirun mpiexec aprun openmpirun
                 HINTS ENV MPI_ROOT ENV MPIROOT ENV MPI_PREFIX ENV MPI_HOME ENV MPIHOME
                )
  endif()

endif()

# - Number of MPI ranks flag
set(MPI_EXEC_NUMPROCS_FLAG_DFLT -n)
if(NOT MPI_EXEC_NUMPROCS_FLAG )

  if (MPIEXEC_NUMPROC_FLAG)
    set(MPI_EXEC_NUMPROCS_FLAG "${MPIEXEC_NUMPROC_FLAG}" CACHE STRING "Set MPI number of procs flag from FindMPI")
  else()
    set(MPI_EXEC_NUMPROCS_FLAG ${MPI_EXEC_NUMPROCS_FLAG_DFLT})
  endif()

endif()

# - Maximum number of processors. This is a limit for the test suite
#   Some tests require too many processors and it increases the execution time
#   considerably. 
set(MPI_EXEC_MAX_NUMPROCS_DFLT 8)
if ( NOT MPI_EXEC_MAX_NUMPROCS )
  include(ProcessorCount)
  ProcessorCount(proc_count)
  if ( NOT proc_count EQUAL 0 )
    math(EXPR MPI_EXEC_MAX_NUMPROCS "${proc_count} * 2") 
    message(STATUS "Detected ${proc_count} processors and will set maximum to ${MPI_EXEC_MAX_NUMPROCS}")
  else()
    set(MPI_EXEC_MAX_NUMPROCS ${MPI_EXEC_MAX_NUMPROCS_DFLT})
  endif()
endif()  

# - Set the pre and post flags
#   Usage:
#   ${MPI_EXEC} ${MPI_EXEC_NUMPROCS_FLAG} PROCS ${MPI_EXEC_PREFLAGS} EXECUTABLE ${MPI_EXEC_POSTFLAGS}
if ( NOT MPI_EXEC_PREFLAGS )
  if ( MPIEXEC_PREFLAGS )
    set(MPI_EXEC_PREFLAGS "${MPIEXEC_PRFLAGS}" CACHE STRING "Set MPI execute pre flags")
  endif()
endif()

if ( NOT MPI_EXEC_POSTFLAGS )
  if ( MPIEXEC_POSTFLAGS )
    set(MPI_EXEC_POSTFLAGS "${MPIEXEC_PRFLAGS}" CACHE STRING "Set MPI execute post flags")
  endif()
endif()

# ############################################################################ #
# LAPACK/BLAS Configuration 
# ############################################################################ #

# Search for BLAS (Basic Linear Algebra Subprograms) http://www.netlib.org/blas
find_package(BLAS REQUIRED)

# Search for LAPACK (Linear Algebra PACKage) http://www/netlib.org/lapack
find_package(LAPACK REQUIRED)

# ############################################################################ #
# Set common build compiler flags, build types and directories
# ############################################################################ #

# CMake compiler settings for any package built with CMake

# C language flags
set(Amanzi_CMAKE_C_COMPILER_ARGS
     -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}
     -DCMAKE_C_FLAGS_DEBUG:STRING=${CMAKE_C_FLAGS_DEBUG}
     -DCMAKE_C_FLAGS_MINSIZEREL:STRING=${CMAKE_C_FLAGS_MINSIZEREL}
     -DCMAKE_C_FLAGS_RELEASE:STRING=${CMAKE_C_FLAGS_RELEASE}
     -DCMAKE_C_FLAGS_RELWITHDEBINFO:STRING=${CMAKE_C_FLAGS_RELWITHDEBINFO})

# C++ language flags 
set(Amanzi_CMAKE_CXX_COMPILER_ARGS
     -DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}
     -DCMAKE_CXX_FLAGS_DEBUG:STRING=${CMAKE_CXX_FLAGS_DEBUG}
     -DCMAKE_CXX_FLAGS_MINSIZEREL:STRING=${CMAKE_CXX_FLAGS_MINSIZEREL}
     -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
     -DCMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=${CMAKE_CXX_FLAGS_RELWITHDEBINFO})

# Fortran language flags    
set(Amanzi_CMAKE_Fortran_COMPILER_ARGS
     -DCMAKE_Fortran_FLAGS:STRING=${CMAKE_Fortran_FLAGS}
     -DCMAKE_Fortran_FLAGS_DEBUG:STRING=${CMAKE_Fortran_FLAGS_DEBUG}
     -DCMAKE_Fortran_FLAGS_MINSIZEREL:STRING=${CMAKE_Fortran_FLAGS_MINSIZEREL}
     -DCMAKE_Fortran_FLAGS_RELEASE:STRING=${CMAKE_Fortran_FLAGS_RELEASE}
     -DCMAKE_Fortran_FLAGS_RELWITHDEBINFO:STRING=${CMAKE_Fortran_FLAGS_RELWITHDEBINFO})

# Link flags
set(Amanzi_CMAKE_LINKER_ARGS
     -DCMAKE_EXE_LINKER_FLAGS:STRING=${CMAKE_EXE_LINKER_FLAGS}
     -DCMAKE_SHARED_LINKER_FLAGS:STRING=${CMAKE_SHARED_LINKER_FLAGS})

message("AMANZI: SHARED LINKER FLAGS = ${Amanzi_CMAKE_LINKER_ARGS}")

# GNU configure static/shared
if(BUILD_SHARED_LIBS)
  set(Amanzi_SHARED_SWITCH "--enable-shared")
else()
  set(Amanzi_SHARED_SWITCH "--disable-shared")
endif()


# Common compiler flags to ensure a uniform build in projects that do not use CMake
include(BuildWhitespaceString)
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_uc)
set(build_c_flags ${CMAKE_C_FLAGS_${build_type_uc}})
set(build_cxx_flags ${CMAKE_CXX_FLAGS_${build_type_uc}})
set(build_fortran_flags ${CMAKE_Fortran_FLAGS_${build_type_uc}})
message(STATUS "Common compile flags for build type ${CMAKE_BUILD_TYPE}")
build_whitespace_string(Amanzi_COMMON_CFLAGS ${CMAKE_C_FLAGS} ${build_c_flags}) 
build_whitespace_string(Amanzi_COMMON_CXXFLAGS ${CMAKE_CXX_FLAGS} ${build_cxx_flags}) 
build_whitespace_string(Amanzi_COMMON_FCFLAGS ${CMAKE_Fortran_FLAGS} ${build_fortran_flags}) 
message(STATUS "\tC flags\t\t${Amanzi_COMMON_CFLAGS}")
message(STATUS "\tC++ flags\t${Amanzi_COMMON_CXXFLAGS}")
message(STATUS "\tFortran flags\t${Amanzi_COMMON_FCFLAGS}")

# Common link flags for projects that do not use CMake
set(Amanzi_COMMON_LDFLAGS)
if (CMAKE_EXE_LINK_FLAGS)
  build_whitespace_string(Amanzi_COMMON_LDFLAGS ${CMAKE_EXE_LINK_FLAGS})
endif()


# ############################################################################ #
# Begin TPL builds 
# ############################################################################ #

# --- Add .NOTPARALLEL target in order to force the top level make to 
#     execute serially

add_custom_target(.NOTPARALLEL)

# --- Initalize and set the TPL build files

# Create configure, build, install and test targets for each TPL
set_property(DIRECTORY PROPERTY
             EP_STEP_TARGETS download patch configure build install test)

# Include the TPL version information
include(${SuperBuild_SOURCE_DIR}/TPLVersions.cmake)

# Include the CMake module ExternalProject
include(ExternalProject)

# Include the macro that defines uniform build, install and Add_ExternalProject args
include(DefineExternalProjectArgs)

# Set of CMake configure arguments to pass to Amanzi once the TPLs are built
set(Amanzi_TPL_CMAKE_ARGS)

# --- Begin the build definitions

# MPI
if ( BUILD_MPI )
  include(${SuperBuild_BUILD_FILES_DIR}/Build_OpenMPI.cmake)
  set(MPI_PROJECT OpenMPI)
endif()

# ZLIB
include(${SuperBuild_BUILD_FILES_DIR}/Build_zlib.cmake) 

# CURL
include(${SuperBuild_BUILD_FILES_DIR}/Build_CURL.cmake)

# UnitTest
append_set(Amanzi_TPL_CMAKE_ARGS
           -DENABLE_TEST:BOOL=${ENABLE_TESTS})
if (ENABLE_TESTS)
  include(${SuperBuild_BUILD_FILES_DIR}/Build_UnitTest.cmake)
  append_set(Amanzi_TPL_CMAKE_ARGS 
             -DUnitTest_DIR:FILEPATH=${TPL_INSTALL_PREFIX})
endif()

# CCSE
set(CCSE_BL_SPACEDIM_DFLT 2)
if(NOT CCSE_BL_SPACEDIM )
  set(CCSE_BL_SPACEDIM ${CCSE_BL_SPACEDIM_DFLT})
endif()
if ( ENABLE_Structured )
  include(${SuperBuild_BUILD_FILES_DIR}/Build_CCSE.cmake) 
  append_set(Amanzi_TPL_CMAKE_ARGS
            -DCCSE_DIR:FILEPATH=${TPL_INSTALL_PREFIX}
            -DENABLE_MPI:BOOL=TRUE
            -DENABLE_OpenMP:BOOL=${ENABLE_OpenMP}
            -DAMANZI_PRECISION:STRING=DOUBLE
            -DAMANZI_SPACEDIM:INT=${CCSE_BL_SPACEDIM})
endif()

# Boost
include(${SuperBuild_BUILD_FILES_DIR}/Build_Boost.cmake) 
#TESTINGinclude(${SuperBuild_BUILD_FILES_DIR}/Build_BoostCmake.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS
           -DBOOST_ROOT:FILEPATH=${TPL_INSTALL_PREFIX}
           -DBoost_USE_STATIC_LIBS:BOOL=TRUE
           -DBoost_NO_SYSTEM_PATHS:BOOL=TRUE)

# HDF5  
include(${SuperBuild_BUILD_FILES_DIR}/Build_HDF5.cmake) 
#BROKENinclude(${SuperBuild_BUILD_FILES_DIR}/Build_HDF5Cmake.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS
           -DHDF5_ROOT:FILEPATH=${TPL_INSTALL_PREFIX})

# ASCEM-IO  
include(${SuperBuild_BUILD_FILES_DIR}/Build_ASCEMIO.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS
           -DASCEMIO_DIR:FILEPATH=${TPL_INSTALL_PREFIX})

# XERCES
include(${SuperBuild_BUILD_FILES_DIR}/Build_XERCES.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS
           -DXERCES_DIR:FILEPATH=${TPL_INSTALL_PREFIX})

# NetCDF
include(${SuperBuild_BUILD_FILES_DIR}/Build_NetCDF.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS
           -DNetCDF_DIR:FILEPATH=${TPL_INSTALL_PREFIX})

# NetCDF Fortran
include(${SuperBuild_BUILD_FILES_DIR}/Build_NetCDF_Fortran.cmake)
append_set(Amanzi_TPL_CMAKE_ARGS
           -DNetCDF_FORTRAN_DIR:FILEPATH=${TPL_INSTALL_PREFIX})

# ExodusII
include(${SuperBuild_BUILD_FILES_DIR}/Build_ExodusII.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS
           -DExodusII_DIR:FILEPATH=${TPL_INSTALL_PREFIX})

# MOAB
if ( ENABLE_MOAB_Mesh )
  include(${SuperBuild_BUILD_FILES_DIR}/Build_MOAB.cmake)
  append_set(Amanzi_TPL_CMAKE_ARGS
             -DENABLE_MOAB_Mesh:BOOL=${ENABLE_MOAB_Mesh}
	     -DMOAB_DIR:PATH=${TPL_INSTALL_PREFIX})
endif()

# HYPRE
option(ENABLE_HYPRE "Build the HYPRE solver package" TRUE)
if ( ENABLE_HYPRE )
  include(${SuperBuild_BUILD_FILES_DIR}/Build_HYPRE.cmake)
endif()

# METIS -- needs to go before PETSc because PETSc needs it.
include(${SuperBuild_BUILD_FILES_DIR}/Build_METIS.cmake) 
append_set(Amanzi_TPL_CMAKE_ARGS 
           -DMETIS_DIR:FILEPATH=${METIS_DIR})

# ParMetis -- needs to go before PETSc because PETSc needs it.
include(${SuperBuild_BUILD_FILES_DIR}/Build_ParMetis.cmake)
if (ENABLE_PETSC OR ENABLE_Structured)
  append_set(Amanzi_TPL_CMAKE_ARGS
             -DParMetis_DIR:FILEPATH=${ParMetis_DIR})
endif ()

# PETSc
option(ENABLE_PETSC "Build the PETSc package" FALSE)
if (ENABLE_PETSC OR ENABLE_Structured)
  set(ENABLE_PETSC TRUE)
  include(${SuperBuild_BUILD_FILES_DIR}/Build_PETSc.cmake)
endif()

# Trilinos  
include(${SuperBuild_BUILD_FILES_DIR}/Build_Trilinos.cmake)
append_set(Amanzi_TPL_CMAKE_ARGS
           -DENABLE_STK_Mesh:BOOL=${ENABLE_STK_Mesh}
           -DTrilinos_INSTALL_PREFIX:PATH=${Trilinos_INSTALL_PREFIX})

# SEACAS
include(${SuperBuild_BUILD_FILES_DIR}/Build_SEACAS.cmake)

# MSTK
if ( ENABLE_MSTK_Mesh )
  include(${SuperBuild_BUILD_FILES_DIR}/Build_MSTK.cmake) 
  append_set(Amanzi_TPL_CMAKE_ARGS
            -DENABLE_MSTK_Mesh:BOOL=${ENABLE_MSTK_Mesh}
            -DMSTK_INCLUDE_DIR:PATH=${MSTK_INCLUDE_DIR}
            -DMSTK_LIBRARY_DIR:PATH=${MSTK_LIBRARY_DIR})
endif()

# Silo
if (ENABLE_Silo)
  include(${SuperBuild_BUILD_FILES_DIR}/Build_Silo.cmake)
  append_set(Amanzi_TPL_CMAKE_ARGS
    -DENABLE_Silo:BOOL=${ENABLE_Silo}
    -DSilo_INCLUDE_DIR:PATH=${Silo_INCLUDE_DIR}
    -DSilo_LIBRARY_DIR:PATH=${Silo_LIBRARY_DIR})
endif()

# Alquimia and PFlotran
option(ENABLE_ALQUIMIA "Build the Alquimia chemistry interface" FALSE)
option(ENABLE_PFLOTRAN "Build the PFlotran chemistry package" FALSE)
if (ENABLE_ALQUIMIA)
  set(ENABLE_PFLOTRAN TRUE)
endif()
if (ENABLE_PFLOTRAN)
  if (NOT ENABLE_PETSC)
    set(ENABLE_PETSC TRUE)
    include(${SuperBuild_BUILD_FILES_DIR}/Build_PETSc.cmake)
  endif()
  include(${SuperBuild_BUILD_FILES_DIR}/Build_pflotran.cmake)
endif()
if (ENABLE_ALQUIMIA)
  include(${SuperBuild_BUILD_FILES_DIR}/Build_alquimia.cmake)
endif()

# ############################################################################ #
#  Final steps
# ############################################################################ #

#  --- Create a CMake cache scipt
set(SuperBuild_Amanzi_CACHE_SCRIPT amanzi-tpl-config.cmake)
configure_file(${SuperBuild_TEMPLATE_FILES_DIR}/${SuperBuild_Amanzi_CACHE_SCRIPT}.in
               ${SuperBuild_BINARY_DIR}/${SuperBuild_Amanzi_CACHE_SCRIPT}
               @ONLY)
install( FILES "${SuperBuild_BINARY_DIR}/${SuperBuild_Amanzi_CACHE_SCRIPT}"             
         DESTINATION ${TPL_INSTALL_PREFIX}/share/cmake)

# --- Create a Exodus split mesh script and add to the install target     
set(SuperBuild_EXOSPLIT_SCRIPT splitexo)
configure_file(${SuperBuild_TEMPLATE_FILES_DIR}/${SuperBuild_EXOSPLIT_SCRIPT}.sh.in
               ${SuperBuild_BINARY_DIR}/${SuperBuild_EXOSPLIT_SCRIPT}
               @ONLY)
install(PROGRAMS "${SuperBuild_BINARY_DIR}/${SuperBuild_EXOSPLIT_SCRIPT}"             
         DESTINATION ${TPL_INSTALL_PREFIX}/bin)

#  --- Build the docs
option(BUILD_DOCS "Build the SuperBuild documentation" FALSE)
if ( BUILD_DOCS )
  add_subdirectory(doc)
endif()

# Build the Amanzi build script
option(BUILD_Amanzi "Add Amanzi to the build stack" FALSE)
if (Amanzi_INSTALL_PREFIX)
  set(BUILD_Amanzi TRUE)
endif()

if ( BUILD_Amanzi )

  set(Amanzi_CMAKE_ARGS ${Amanzi_CMAKE_COMPILER_ARGS})

  # Enable the config report
  append_set(Amanzi_CMAKE_ARGS -DENABLE_Config_Report:BOOL=TRUE)

  # Install path
  if (Amanzi_INSTALL_PREFIX)
    append_set(Amanzi_CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:FILEPATH=${Amanzi_INSTALL_PREFIX})
  endif()

  # Build type
  if(CMAKE_BUILD_TYPE)
    append_set(Amanzi_CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
  endif() 

  # Disable shared libraries and executable unless user has requested shared libs
  set(Amanzi_SHARED_CMAKE_ARGS)
  if(BUILD_SHARED_LIBS)
    append_set(Amanzi_SHARED_CMAKE_ARGS -DBUILD_SHARED_LIBS:BOOL=TRUE)
  else()  
    append_set(Amanzi_SHARED_CMAKE_ARGS 
               -DBUILD_SHARED_LIBS:BOOL=FALSE
            -DPREFER_STATIC_LIBRARIES:BOOL=TRUE)
    #        -DBUILD_STATIC_EXECUTABLES:BOOL=TRUE)
  endif()      

  append_set(Amanzi_CMAKE_ARGS ${Amanzi_SHARED_CMAKE_ARGS})

  # MPI
  set(Amanzi_MPI_CMAKE_ARGS)
  if ( MPI_EXEC )
    append_set(Amanzi_MPI_CMAKE_ARGS -DMPI_EXEC:FILEPATH=${MPI_EXEC})
  else()  
    message(FATAL_ERROR "Enable Amanzi build requires MPI parallel run launch command\n"
                        "Please define the this binary with:\n"
                        "-DMPI_EXEC:FILEPATH=<binary>\n"
                        "and re-run cmake")
  endif()        

  if ( MPI_EXEC_MAX_NUMPROCS )
    append_set(Amanzi_MPI_CMAKE_ARGS -DMPI_EXEC_MAX_NUMPROCS:STRING=${MPI_EXEC_NUMPROCS})
  else()  
    append_set(Amanzi_MPI_CMAKE_ARGS -DMPI_EXEC_MAX_NUMPROCS:STRING=4)
  endif()        

  if ( MPI_EXEC_NUMPROCS_FLAG )
    append_set(Amanzi_MPI_CMAKE_ARGS -DMPI_EXEC_NUMPROCS_FLAG:STRING=${MPI_EXEC_NUMPROCS_FLAG})
  else()  
    append_set(Amanzi_MPI_CMAKE_ARGS -DMPI_EXEC_NUMPROCS_FLAG:STRING=-n)
  endif()        

  append_set(Amanzi_CMAKE_ARGS ${Amanzi_MPI_CMAKE_ARGS})
  
  append_set(Amanzi_CMAKE_ARGS ${Amanzi_TPL_CMAKE_ARGS})


  append_set(Amanzi_CMAKE_ARGS
             -DENABLE_Unstructured:BOOL=${ENABLE_Unstructured}
             -DENABLE_Structured:BOOL=${ENABLE_Structured})

  #print_variable(Amanzi_CMAKE_ARGS) 

  if(NOT Amanzi_SOURCE_DIR)
    message(FATAL_ERROR "Enable Amanzi build requires an Amanzi source directory."
                        "Define this dorectory with:"
                        "-DAmanzi_SOURCE_DIR:FILEPATH=<Amanzi source location>"
                        "and re-run cmake")
  endif()
  message(STATUS "Will build Amanzi source located in ${Amanzi_SOURCE_DIR}")

  # Now add amanzi to the build stack
  include(ExternalProject)

  set(Amanzi_DEPENDS 
    ${ExodusII_BUILD_TARGET}
	${HDF5_BUILD_TARGET}
	${ASCEMIO_BUILD_TARGET}
	${XERCES_BUILD_TARGET}
	${UnitTest_BUILD_TARGET}
	${Boost_BUILD_TARGET}
	${Trilinos_BUILD_TARGET})

  if ( ENABLE_Unstructured )
    list(APPEND Amanzi_DEPENDS ${MSTK_BUILD_TARGET} ${MOAB_BUILD_TARGET})
  endif()
  
  if ( ENABLE_Structured )
    list(APPEND Amanzi_DEPENDS ${CCSE_BUILD_TARGET})
  endif()  

  if ( ENABLE_Silo )
    list(APPEND Amanzi_DEPENDS ${Silo_BUILD_TARGET})
  endif()

  ExternalProject_Add(amanzi
    DEPENDS ${Amanzi_DEPENDS}
	PREFIX ${CMAKE_BINARY_DIR}/Amanzi
    SOURCE_DIR ${Amanzi_SOURCE_DIR}
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS	${Amanzi_CMAKE_ARGS}
                ${Amanzi_CMAKE_C_COMPILER_ARGS}
                -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER_USE}
			    ${Amanzi_CMAKE_CXX_COMPILER_ARGS}
                -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER_USE}
			    ${Amanzi_CMAKE_Fortran_COMPILER_ARGS}
                -DCMAKE_Fortran_COMPILER:FILEPATH=${CMAKE_Fortran_COMPILER_USE}
    INSTALL_DIR ${Amanzi_INSTALL_PREFIX} )
endif()        
               
