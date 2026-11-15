# SPDX-License-Identifier: BSD-3-Clause
#
# add_ofx_plugin(): build an OFX plug-in bundle.
# Ported from AcademySoftwareFoundation/openfx; adapted for the NatronGitHub
# fork: the bundle is also assembled in the build tree (usable directly with
# OFX_PLUGIN_PATH), Info.plist is generated at build time instead of being
# written into PLUGIN_INSTALLDIR at configure time, and the Linux arch folder
# follows the build architecture.

if(NOT PLUGIN_INSTALLDIR)
  if(APPLE)
    set(PLUGIN_INSTALLDIR "/Library/OFX/Plugins/OpenFX Examples")
  elseif(WIN32)
    set(PLUGIN_INSTALLDIR "C:/Program Files/Common Files/OFX/Plugins/OpenFX Examples")
  elseif(UNIX)
    set(PLUGIN_INSTALLDIR "/usr/OFX/Plugins/OpenFX Examples")
  else()
    set(PLUGIN_INSTALLDIR "/unknown-os")
  endif()
endif()

# Bundle architecture folder (OFX packaging spec, ofxPackaging.rst)
if(APPLE)
  set(ARCHDIR "MacOS") # universal or arch-specific: no arm64 folder
elseif(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCHDIR "Win64")
  else()
    set(ARCHDIR "Win32")
  endif()
elseif(UNIX)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(ARCHDIR "Linux-x86-64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "86")
    set(ARCHDIR "Linux-x86")
  else()
    set(ARCHDIR "Linux-${CMAKE_SYSTEM_PROCESSOR}")
  endif()
else()
  set(ARCHDIR "unknown-arch")
endif()

# Where bundles are assembled in the build tree
if(NOT OFX_PLUGIN_BUILD_DIR)
  set(OFX_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/Plugins")
endif()

# Location of the Info.plist template. In the OpenFX source tree it is in
# Examples/; in an installed package it sits next to this module.
set(_OFX_CMAKE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Add a new OFX plugin "target".
#
# Arguments: TARGET
# Optional argument: DIR, defaults to same as TARGET (use when renaming TARGET)
function(add_ofx_plugin TARGET)
  if(${ARGC} GREATER 1)
    set(DIR ${ARGN})
  else()
    set(DIR ${TARGET})
  endif()
  if(APPLE)
    add_library(${TARGET} MODULE) # build as an OSX bundle
  else()
    add_library(${TARGET} SHARED) # build as shared lib/DLL
  endif()
  set_target_properties(${TARGET} PROPERTIES SUFFIX ".ofx" PREFIX "")

  set(PLUGIN_NAME ${TARGET})
  set(PLUGIN_EXE ${TARGET}.ofx)
  set(BUNDLE_IDENTIFIER org.openeffects.plugins.${TARGET})
  set(BUNDLE_SIGNATURE "????")  # any value is OK here, ignored by modern MacOS
  set(PLUGIN_VERSION "1.0.0")

  set(_ofx_info_plist "")
  foreach(_dir "${_OFX_CMAKE_MODULE_DIR}"
               "${_OFX_CMAKE_MODULE_DIR}/../Examples"
               "${CMAKE_SOURCE_DIR}/Examples")
    if(EXISTS "${_dir}/Info.plist.in")
      set(_ofx_info_plist "${_dir}/Info.plist.in")
      break()
    endif()
  endforeach()
  if(NOT _ofx_info_plist)
    message(FATAL_ERROR "add_ofx_plugin: could not find Info.plist.in")
  endif()

  # Assemble the bundle in the build tree: <TARGET>.ofx.bundle/Contents/{Info.plist,<ARCHDIR>/<TARGET>.ofx}
  set(_bundle "${OFX_PLUGIN_BUILD_DIR}/${TARGET}.ofx.bundle")
  configure_file("${_ofx_info_plist}" "${_bundle}/Contents/Info.plist")
  set_target_properties(${TARGET} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_bundle}/Contents/${ARCHDIR}"
    RUNTIME_OUTPUT_DIRECTORY "${_bundle}/Contents/${ARCHDIR}")
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_cfg}" _CFG)
    set_target_properties(${TARGET} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_bundle}/Contents/${ARCHDIR}"
      RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_bundle}/Contents/${ARCHDIR}")
  endforeach()

  # Set symbol visibility hidden. Individual symbols are exposed via
  # __declspec(dllexport) or __attribute__((visibility("default"))) (OfxExport)
  set_target_properties(${TARGET} PROPERTIES C_VISIBILITY_PRESET hidden
                                             CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON)

  # To install plugins: cmake --install Build
  install(
    TARGETS ${TARGET}
    LIBRARY DESTINATION "${PLUGIN_INSTALLDIR}/${TARGET}.ofx.bundle/Contents/${ARCHDIR}"
    RUNTIME DESTINATION "${PLUGIN_INSTALLDIR}/${TARGET}.ofx.bundle/Contents/${ARCHDIR}")
  install(FILES "${_bundle}/Contents/Info.plist"
          DESTINATION "${PLUGIN_INSTALLDIR}/${TARGET}.ofx.bundle/Contents")
endfunction()
