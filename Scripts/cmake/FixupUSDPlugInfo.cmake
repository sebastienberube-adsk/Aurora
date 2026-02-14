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

# Define the DLL name mappings (release -> debug)
# These are USD libraries that use a 'd' suffix in debug builds
set(USD_DLL_MAPPINGS
    # Core USD libraries (usd_* prefix)
    "usd_arch.dll:usd_archd.dll"
    "usd_ar.dll:usd_ard.dll"
    "usd_cameraUtil.dll:usd_cameraUtild.dll"
    "usd_garch.dll:usd_garchd.dll"
    "usd_gf.dll:usd_gfd.dll"
    "usd_glf.dll:usd_glfd.dll"
    "usd_hd.dll:usd_hdd.dll"
    "usd_hdGp.dll:usd_hdGpd.dll"
    "usd_hdsi.dll:usd_hdsid.dll"
    "usd_hdSt.dll:usd_hdStd.dll"
    "usd_hdx.dll:usd_hdxd.dll"
    "usd_hf.dll:usd_hfd.dll"
    "usd_hgi.dll:usd_hgid.dll"
    "usd_hgiGL.dll:usd_hgiGLd.dll"
    "usd_hgiInterop.dll:usd_hgiInteropd.dll"
    "usd_hgiVulkan.dll:usd_hgiVulkand.dll"
    "usd_hio.dll:usd_hiod.dll"
    "usd_js.dll:usd_jsd.dll"
    "usd_kind.dll:usd_kindd.dll"
    "usd_ndr.dll:usd_ndrd.dll"
    "usd_pcp.dll:usd_pcpd.dll"
    "usd_plug.dll:usd_plugd.dll"
    "usd_pxOsd.dll:usd_pxOsdd.dll"
    "usd_sdf.dll:usd_sdfd.dll"
    "usd_sdr.dll:usd_sdrd.dll"
    "usd_tf.dll:usd_tfd.dll"
    "usd_trace.dll:usd_traced.dll"
    "usd_usd.dll:usd_usdd.dll"
    "usd_usdAppUtils.dll:usd_usdAppUtilsd.dll"
    "usd_usdGeom.dll:usd_usdGeomd.dll"
    "usd_usdHydra.dll:usd_usdHydrad.dll"
    "usd_usdImaging.dll:usd_usdImagingd.dll"
    "usd_usdImagingGL.dll:usd_usdImagingGLd.dll"
    "usd_usdLux.dll:usd_usdLuxd.dll"
    "usd_usdMedia.dll:usd_usdMediad.dll"
    "usd_usdPhysics.dll:usd_usdPhysicsd.dll"
    "usd_usdProc.dll:usd_usdProcd.dll"
    "usd_usdProcImaging.dll:usd_usdProcImagingd.dll"
    "usd_usdRender.dll:usd_usdRenderd.dll"
    "usd_usdRi.dll:usd_usdRid.dll"
    "usd_usdRiImaging.dll:usd_usdRiImagingd.dll"
    "usd_usdShade.dll:usd_usdShaded.dll"
    "usd_usdSkel.dll:usd_usdSkeld.dll"
    "usd_usdSkelImaging.dll:usd_usdSkelImagingd.dll"
    "usd_usdUI.dll:usd_usdUId.dll"
    "usd_usdUtils.dll:usd_usdUtilsd.dll"
    "usd_usdviewq.dll:usd_usdviewqd.dll"
    "usd_usdVol.dll:usd_usdVold.dll"
    "usd_usdVolImaging.dll:usd_usdVolImagingd.dll"
    "usd_vt.dll:usd_vtd.dll"
    "usd_work.dll:usd_workd.dll"
    # Plugin libraries (no usd_ prefix)
    "hdStorm.dll:hdStormd.dll"
    "hioOiio.dll:hioOiiod.dll"
    "sdrGlslfx.dll:sdrGlslfxd.dll"
    "usdShaders.dll:usdShadersd.dll"
)

set(FILES_UPDATED 0)

foreach(PLUG_INFO_FILE ${PLUG_INFO_FILES})
    # Read the file content
    file(READ "${PLUG_INFO_FILE}" CONTENT)
    set(ORIGINAL_CONTENT "${CONTENT}")
    
    # Apply all DLL name mappings
    foreach(MAPPING ${USD_DLL_MAPPINGS})
        # Split the mapping into release and debug names
        string(REPLACE ":" ";" MAPPING_LIST "${MAPPING}")
        list(GET MAPPING_LIST 0 RELEASE_NAME)
        list(GET MAPPING_LIST 1 DEBUG_NAME)
        
        # Replace release name with debug name
        string(REPLACE "${RELEASE_NAME}" "${DEBUG_NAME}" CONTENT "${CONTENT}")
    endforeach()
    
    # Write the file if content changed
    if(NOT "${CONTENT}" STREQUAL "${ORIGINAL_CONTENT}")
        message(STATUS "  Fixing: ${PLUG_INFO_FILE}")
        file(WRITE "${PLUG_INFO_FILE}" "${CONTENT}")
        math(EXPR FILES_UPDATED "${FILES_UPDATED} + 1")
    endif()
endforeach()

message(STATUS "FixupUSDPlugInfo: Updated ${FILES_UPDATED} files")
message(STATUS "FixupUSDPlugInfo: Done")
