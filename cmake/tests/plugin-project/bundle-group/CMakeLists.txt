# SPDX-License-Identifier: BSD-3-Clause
# ofx_plugin_bundle_group(): two groups of tiny Support plug-ins, built as combined or separate
# bundles (OFX_PLUGIN_BUNDLES). Checked by .github/scripts/cmake-plugin-project.sh, which builds
# them in both modes; the expected bundles are in the comments.

# TinyGroup: self-registering plug-ins, two of them in one directory, one elsewhere; resources (a
# file, a directory, a DESTINATION item, a file listed twice); per-plug-in compile settings; common
# objects; the plug-in's own Info.plist used or not; PROPERTIES; ALSO_SEPARATE.
#   COMBINED: TinyGroup.ofx.bundle (TinyA, TinyB, ExplicitC), plus org.example.tiny.TinyB.ofx.bundle
#             (ALSO_SEPARATE: TinyB is in both)
#   SEPARATE: org.example.tiny.TinyA, org.example.tiny.TinyB, org.example.tiny.ExplicitC
ofx_plugin_bundle_group(TinyGroup
  COMBINED_INFO_PLIST TinyGroup.plist
  COMBINED_RESOURCES combined.txt
  ID_PREFIX org.example.tiny.
  VERSION 3.2.1
  ALSO_SEPARATE TinyB
  COMMON_SOURCES common.cpp
  COMMON_LINK_LIBRARIES openfx::Support
  COMMON_COMPILE_DEFINITIONS TINY_COMMON
  COMMON_INCLUDE_DIRECTORIES .
  OUT_TARGETS _tiny_targets
  OUT_COMMON_TARGET _tiny_common
  PLUGIN TinyA
    SOURCES group/tinyA.cpp group/shared.cpp
    INFO_PLIST group/TinyA.plist # hard-codes TinyA.ofx (as the Makefiles' plists): not used, a warning
    RESOURCES group/org.example.tiny.TinyAPlugin.png group/shared-icon.txt
    COMPILE_DEFINITIONS TINY_AB -DTINY_AB_SHARED
    INCLUDE_DIRECTORIES group/inc
    PROPERTIES TINY_COMPONENTS alpha beta # (a project-prefixed custom property)
    OUT_TARGET _tinyA_target
  PLUGIN TinyB
    SOURCES group/tinyB.cpp group/shared.cpp # shared.cpp: the same settings as in TinyA, compiled once
    RESOURCES group/presets group/shared-icon.txt # a directory, with its name; the icon once
    COMPILE_DEFINITIONS TINY_AB -DTINY_AB_SHARED
    INCLUDE_DIRECTORIES group/inc
    PROPERTIES TINY_COMPONENTS beta gamma
    PROPERTIES TINY_OTHER b
    OUT_TARGET _tinyB_target
  PLUGIN ExplicitC
    ID org.example.tiny.ExplicitC
    SOURCES other/tinyC.cpp
    INFO_PLIST other/ExplicitC.plist # @PLUGIN_EXE@ and @BUNDLE_IDENTIFIER@: used
    RESOURCES other/c.txt DESTINATION shaders/c
    COMPILE_DEFINITIONS TINY_C_ONLY
    COMPILE_OPTIONS -DTINY_C_OPT=1
    INCLUDE_DIRECTORIES other/inc
    OPENMP)

# the targets, their plug-ins and properties
function(_tiny_expect what got want)
  if(NOT "${got}" STREQUAL "${want}")
    message(FATAL_ERROR "bundle-group: ${what}: '${got}', expected '${want}'")
  endif()
endfunction()
get_target_property(_p TinyGroup_common TYPE)
_tiny_expect("OUT_COMMON_TARGET" "${_tiny_common}" "TinyGroup_common")
_tiny_expect("TinyGroup_common TYPE" "${_p}" "OBJECT_LIBRARY")
# a plug-in's INCLUDE_DIRECTORIES are on the target (the union in a combined bundle)
if(OFX_PLUGIN_BUNDLES STREQUAL "SEPARATE")
  get_target_property(_p ExplicitC INCLUDE_DIRECTORIES)
  _tiny_expect("ExplicitC INCLUDE_DIRECTORIES" "${_p}" "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/other/inc")
else()
  get_target_property(_p TinyGroup INCLUDE_DIRECTORIES)
  _tiny_expect("TinyGroup INCLUDE_DIRECTORIES" "${_p}" "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/group/inc;${CMAKE_CURRENT_SOURCE_DIR}/other/inc")
endif()
# a source listed by two plug-ins gets their settings once
get_source_file_property(_p group/shared.cpp COMPILE_DEFINITIONS)
_tiny_expect("shared.cpp COMPILE_DEFINITIONS" "${_p}" "TINY_AB;TINY_AB_SHARED")
if(OFX_PLUGIN_BUNDLES STREQUAL "SEPARATE")
  _tiny_expect("OUT_TARGETS" "${_tiny_targets}" "TinyA;TinyB;ExplicitC")
  _tiny_expect("TinyA OUT_TARGET" "${_tinyA_target}" "TinyA")
  get_target_property(_p TinyA TINY_COMPONENTS)
  _tiny_expect("TinyA TINY_COMPONENTS" "${_p}" "alpha;beta")
  get_target_property(_p TinyB OFX_BUNDLE_PLUGINS)
  _tiny_expect("TinyB OFX_BUNDLE_PLUGINS" "${_p}" "TinyB")
  get_target_property(_p TinyB OUTPUT_NAME)
  _tiny_expect("TinyB OUTPUT_NAME" "${_p}" "org.example.tiny.TinyB")
else()
  _tiny_expect("OUT_TARGETS" "${_tiny_targets}" "TinyGroup;TinyB")
  _tiny_expect("TinyA OUT_TARGET" "${_tinyA_target}" "TinyGroup")
  _tiny_expect("TinyB OUT_TARGET" "${_tinyB_target}" "TinyGroup;TinyB")
  get_target_property(_p TinyGroup OFX_BUNDLE_PLUGINS)
  _tiny_expect("TinyGroup OFX_BUNDLE_PLUGINS" "${_p}" "TinyA;TinyB;ExplicitC")
  get_target_property(_p TinyGroup TINY_COMPONENTS) # the union
  _tiny_expect("TinyGroup TINY_COMPONENTS" "${_p}" "alpha;beta;gamma")
  get_target_property(_p TinyGroup TINY_OTHER)
  _tiny_expect("TinyGroup TINY_OTHER" "${_p}" "b")
  get_target_property(_p TinyB TINY_COMPONENTS)
  _tiny_expect("TinyB TINY_COMPONENTS" "${_p}" "beta;gamma")
endif()

# TinyReg: REGISTER (manual registration, a namespaced function, two functions in one plug-in),
# mixed with a self-registering plug-in; COMBINED_ONLY and SEPARATE_ONLY; NO_DEPLOY, the caller adds
# a generated resource and deploys; COMMON_RPATH and COMMON_EXPORT_SYMBOLS. Its mode is the opposite of OFX_PLUGIN_BUNDLES
# (OFX_PLUGIN_BUNDLES_TinyReg), so that each build has REGISTER in both kinds of bundles.
#   COMBINED: TinyReg.ofx.bundle (R1, R3), org.example.reg.R2.ofx.bundle (SEPARATE_ONLY)
#   SEPARATE: TinyReg.ofx.bundle (R1: COMBINED_ONLY), org.example.reg.R2, org.example.reg.R3
if(OFX_PLUGIN_BUNDLES STREQUAL "SEPARATE")
  set(OFX_PLUGIN_BUNDLES_TinyReg COMBINED)
else()
  set(OFX_PLUGIN_BUNDLES_TinyReg SEPARATE)
endif()
ofx_plugin_bundle_group(TinyReg
  ID_PREFIX org.example.reg.
  NO_DEPLOY
  COMMON_RPATH /opt/ofx-test-group-rpath
  COMMON_EXPORT_SYMBOLS OfxTestGroupSymbol
  COMMON_SOURCES common.cpp
  COMMON_LINK_LIBRARIES openfx::Support
  COMMON_COMPILE_DEFINITIONS TINY_COMMON
  COMMON_INCLUDE_DIRECTORIES .
  OUT_TARGETS _reg_targets
  PLUGIN R1 SOURCES reg/reg1.cpp REGISTER registerTinyR1 COMBINED_ONLY
  PLUGIN R2 SOURCES reg/reg2.cpp REGISTER tinyreg::registerR2 ::registerTinyR2b SEPARATE_ONLY
  PLUGIN R3 SOURCES reg/reg3.cpp)
foreach(_t IN LISTS _reg_targets)
  get_target_property(_bundle ${_t} OFX_BUNDLE_NAME)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${_t}/LICENSE-generated.txt" "${_bundle}\n")
  ofx_plugin_resources(${_t} "${CMAKE_CURRENT_BINARY_DIR}/${_t}/LICENSE-generated.txt")
  ofx_deploy_bundle(${_t})
endforeach()

# for cmake-plugin-project.sh: the bundle targets (= install components) and their bundles
set(_lines "")
foreach(_t IN LISTS _tiny_targets _reg_targets)
  get_target_property(_bundle ${_t} OFX_BUNDLE_NAME)
  get_target_property(_plugins ${_t} OFX_BUNDLE_PLUGINS)
  string(REPLACE ";" "," _plugins "${_plugins}")
  string(APPEND _lines "${_t} ${_bundle} ${_plugins}\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/ofx_bundle_groups.txt" "${_lines}")
