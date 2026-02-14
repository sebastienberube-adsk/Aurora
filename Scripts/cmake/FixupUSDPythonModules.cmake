# FixupUSDPythonModules.cmake
# This script fixes USD Python extension modules (.pyd files) for Debug builds on Windows.
# USD's debug builds name Python modules with a 'd' suffix (e.g., _tfd.pyd), but Python's
# import mechanism expects them without the suffix (e.g., _tf.pyd).
# This script creates copies of the debug .pyd files without the 'd' suffix.
#
# Usage: cmake -DUSD_PYTHON_DIR=<path> -DBUILD_CONFIG=<Debug|Release|...> -P FixupUSDPythonModules.cmake

if(NOT DEFINED USD_PYTHON_DIR)
    message(FATAL_ERROR "USD_PYTHON_DIR must be defined")
endif()

if(NOT DEFINED BUILD_CONFIG)
    message(FATAL_ERROR "BUILD_CONFIG must be defined")
endif()

# Only fix up for Debug builds
if(NOT BUILD_CONFIG STREQUAL "Debug")
    message(STATUS "FixupUSDPythonModules: Skipping fixup for ${BUILD_CONFIG} build")
    return()
endif()

message(STATUS "FixupUSDPythonModules: Fixing Python modules in ${USD_PYTHON_DIR} for Debug build")

# List of USD Python module names (without underscore prefix or .pyd extension)
# These are the known USD modules that need debug->release name mapping
# Debug naming: _<name>d.pyd, Release naming: _<name>.pyd
set(USD_PYTHON_MODULES
    ar
    cameraUtil
    garch
    gf
    glf
    kind
    ndr
    pcp
    plug
    pxOsd
    sdf
    sdr
    tf
    trace
    usd
    usdAppUtils
    usdGeom
    usdHydra
    usdImagingGL
    usdLux
    usdMedia
    usdPhysics
    usdProc
    usdRender
    usdRi
    usdShade
    usdSkel
    usdUI
    usdUtils
    usdVol
    usdviewq
    vt
    work
)

set(FILES_COPIED 0)

foreach(MODULE_NAME ${USD_PYTHON_MODULES})
    # Construct the debug and release filenames
    set(DEBUG_FILENAME "_${MODULE_NAME}d.pyd")
    set(RELEASE_FILENAME "_${MODULE_NAME}.pyd")
    
    # Search for the debug file in the Python directory
    file(GLOB_RECURSE DEBUG_PYD_FILES "${USD_PYTHON_DIR}/${DEBUG_FILENAME}")
    
    foreach(DEBUG_PYD_FILE ${DEBUG_PYD_FILES})
        get_filename_component(FILE_DIR "${DEBUG_PYD_FILE}" DIRECTORY)
        set(RELEASE_PYD_FILE "${FILE_DIR}/${RELEASE_FILENAME}")
        
        # Copy the debug file to the release-named file
        message(STATUS "  Copying: ${DEBUG_FILENAME} -> ${RELEASE_FILENAME}")
        file(COPY_FILE "${DEBUG_PYD_FILE}" "${RELEASE_PYD_FILE}")
        math(EXPR FILES_COPIED "${FILES_COPIED} + 1")
    endforeach()
endforeach()

message(STATUS "FixupUSDPythonModules: Copied ${FILES_COPIED} files")
message(STATUS "FixupUSDPythonModules: Done")
