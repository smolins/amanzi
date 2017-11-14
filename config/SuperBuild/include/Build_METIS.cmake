#  -*- mode: cmake -*-

#
# Build TPL:  METIS
#
# --- Define all the directories and common external project flags
define_external_project_args(METIS
                             TARGET metis)


# add version version to the autogenerated tpl_versions.h file
amanzi_tpl_version_write(FILENAME ${TPL_VERSIONS_INCLUDE_FILE}
  PREFIX METIS
  VERSION ${METIS_VERSION_MAJOR} ${METIS_VERSION_MINOR} ${METIS_VERSION_PATCH})

set(METIS_DIR ${TPL_INSTALL_PREFIX})
set(METIS_GKLIB_DIR ${METIS_source_dir}/GKlib)
# --- Define the CMake configure parameters
# Note:
#      CMAKE_CACHE_ARGS requires -DVAR:<TYPE>=VALUE syntax
#      CMAKE_ARGS -DVAR=VALUE OK
# NO WHITESPACE between -D and VAR. Parser blows up otherwise.
set(METIS_CMAKE_CACHE_ARGS
      -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
      -DCMAKE_INSTALL_PREFIX:FILEPATH=${TPL_INSTALL_PREFIX}
      -DBUILD_SHARED_LIBS:BOOL=${BUILD_SHARED_LIBS}
      -DSHARED:BOOL=${BUILD_SHARED_LIBS}
      -DGKLIB_PATH:PATH=${METIS_GKLIB_DIR})

# --- Add external project build and tie to the METIS build target
ExternalProject_Add(${METIS_BUILD_TARGET}
                    DEPENDS   ${METIS_PACKAGE_DEPENDS}            # Package dependency target
                    TMP_DIR   ${METIS_tmp_dir}                    # Temporary files directory
                    STAMP_DIR ${METIS_stamp_dir}                  # Timestamp and log directory
                    # -- Download and URL definitions
                    DOWNLOAD_DIR ${TPL_DOWNLOAD_DIR}              # Download directory
                    URL          ${METIS_URL}                     # URL may be a web site OR a local file
                    URL_MD5      ${METIS_MD5_SUM}                 # md5sum of the archive file
                    # -- Configure
                    SOURCE_DIR   ${METIS_source_dir}              # Source directory
                    CMAKE_ARGS   ${AMANZI_CMAKE_CACHE_ARGS}       # Global definitions from root CMakeList
		                 ${METIS_CMAKE_CACHE_ARGS}  
                                 ${Amanzi_CMAKE_C_COMPILER_ARGS}  # Ensure uniform build
                                 -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
                    # -- Build
                    BINARY_DIR       ${METIS_build_dir}           # Build directory
                    BUILD_COMMAND    $(MAKE)                      # $(MAKE) enables parallel builds through make
                    BUILD_IN_SOURCE  ${METIS_BUILD_IN_SOURCE}     # Flag for in source builds
                    # -- Install
                    INSTALL_DIR      ${TPL_INSTALL_PREFIX}        # Install directory
                    INSTALL_COMMAND  $(MAKE) install
                    # -- Output control
                    ${METIS_logging_args})

# --- Build variables needed outside this file
include(BuildLibraryName)
build_library_name(metis METIS_LIBRARIES APPEND_PATH ${TPL_INSTALL_PREFIX}/lib)
set(METIS_INCLUDE_DIRS ${TPL_INSTALL_PREFIX}/include)

