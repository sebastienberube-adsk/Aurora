# FixupUSDPlugInfo.cmake
# This script fixes USD plugInfo.json files to use debug library names when building Debug.
# USD's build system generates plugInfo.json with release library names even for debug builds,
# which causes plugin loading to fail at runtime.
#
# Usage: cmake -DUSD_PLUGIN_DIR=<path> -DBUILD_CONFIG=<Debug|Release|...> -P FixupUSDPlugInfo.cmake

if(NOT DEFINED USD_PLUGIN_DIR)
    message(FATAL_ERROR "USD_PLUGIN_DIR must be defined")
endif()

if(NOT DEFINED BUILD_CONFIG)
    message(FATAL_ERROR "BUILD_CONFIG must be defined")
endif()

# Only fix up for Debug builds
if(NOT BUILD_CONFIG STREQUAL "Debug")
    message(STATUS "FixupUSDPlugInfo: Skipping fixup for ${BUILD_CONFIG} build")
    return()
endif()

message(STATUS "FixupUSDPlugInfo: Fixing plugInfo.json files in ${USD_PLUGIN_DIR} for Debug build")

# Find all plugInfo.json files recursively
file(GLOB_RECURSE PLUG_INFO_FILES "${USD_PLUGIN_DIR}/*/plugInfo.json")

foreach(PLUG_INFO_FILE ${PLUG_INFO_FILES})
    # Read the file content
    file(READ "${PLUG_INFO_FILE}" CONTENT)
    
    # Check if the file contains a LibraryPath that needs fixing
    # Pattern: "LibraryPath": "...usd_xxx.dll" -> "LibraryPath": "...usd_xxxd.dll"
    # We need to add 'd' before '.dll' for USD libraries (those starting with usd_)
    string(REGEX MATCH "\"LibraryPath\"[^\"]*\"[^\"]*usd_[^\"]*\\.dll\"" HAS_USD_LIB "${CONTENT}")
    
    if(HAS_USD_LIB)
        # Check if already has debug suffix (ends with d.dll)
        string(REGEX MATCH "\"LibraryPath\"[^\"]*\"[^\"]*usd_[^\"]*d\\.dll\"" ALREADY_DEBUG "${CONTENT}")
        
        if(NOT ALREADY_DEBUG)
            # Replace .dll with d.dll for usd_ libraries
            # This regex finds usd_<name>.dll and replaces with usd_<name>d.dll
            string(REGEX REPLACE "(usd_[a-zA-Z0-9_]+)\\.dll" "\\1d.dll" NEW_CONTENT "${CONTENT}")
            
            if(NOT "${CONTENT}" STREQUAL "${NEW_CONTENT}")
                message(STATUS "  Fixing: ${PLUG_INFO_FILE}")
                file(WRITE "${PLUG_INFO_FILE}" "${NEW_CONTENT}")
            endif()
        endif()
    endif()
endforeach()

message(STATUS "FixupUSDPlugInfo: Done")
