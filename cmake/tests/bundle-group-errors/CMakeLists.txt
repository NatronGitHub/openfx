# SPDX-License-Identifier: BSD-3-Clause
# The configure-time errors and warnings of ofx_plugin_bundle_group(): one case per configure
# (OFX_TEST_CASE), checked by .github/scripts/cmake-plugin-project.sh. Nothing is built.
cmake_minimum_required(VERSION 3.16)
project(OfxBundleGroupErrors CXX)
find_package(openfx REQUIRED)

set(_case "${OFX_TEST_CASE}")
if(_case STREQUAL "duplicate-plugin") # "PLUGIN A is declared twice"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp PLUGIN A SOURCES warn.cpp)
elseif(_case STREQUAL "no-sources") # "PLUGIN A has no SOURCES"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A RESOURCES a/x.txt)
elseif(_case STREQUAL "no-id") # "PLUGIN A has no ID"
  ofx_plugin_bundle_group(G PLUGIN A SOURCES warn.cpp)
elseif(_case STREQUAL "resource-clash") # "two resources go to Contents/Resources/x.txt"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp RESOURCES a/x.txt PLUGIN B SOURCES warn2.cpp RESOURCES b/x.txt)
elseif(_case STREQUAL "destination-outside") # "must be a relative subdirectory"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp RESOURCES a/x.txt DESTINATION ../x)
elseif(_case STREQUAL "target-exists") # "the target A already exists"
  add_custom_target(A)
  set(OFX_PLUGIN_BUNDLES SEPARATE)
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp)
elseif(_case STREQUAL "settings-conflict") # "with different COMPILE_DEFINITIONS"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp COMPILE_DEFINITIONS X PLUGIN B SOURCES warn.cpp)
elseif(_case STREQUAL "also-separate-unknown") # "ALSO_SEPARATE C: no such PLUGIN"
  ofx_plugin_bundle_group(G ID_PREFIX t. ALSO_SEPARATE C PLUGIN A SOURCES warn.cpp)
elseif(_case STREQUAL "combined-plist") # "CFBundleExecutable is 'Other.ofx', not G.ofx"
  file(WRITE "${CMAKE_BINARY_DIR}/G.plist" "<plist><dict><key>CFBundleExecutable</key><string>Other.ofx</string></dict></plist>\n")
  ofx_plugin_bundle_group(G ID_PREFIX t. COMBINED_INFO_PLIST "${CMAKE_BINARY_DIR}/G.plist" PLUGIN A SOURCES warn.cpp)
elseif(_case STREQUAL "resource-clash-case") # "two resources go to Contents/Resources/"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp RESOURCES a/x.txt PLUGIN B SOURCES warn2.cpp RESOURCES c/X.txt)
elseif(_case STREQUAL "destination-missing") # "DESTINATION without a subdirectory"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp RESOURCES a/x.txt DESTINATION)
elseif(_case STREQUAL "combined-and-separate-only") # "COMBINED_ONLY and SEPARATE_ONLY"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp COMBINED_ONLY SEPARATE_ONLY)
elseif(_case STREQUAL "also-separate-combined-only") # "the plug-in is COMBINED_ONLY"
  ofx_plugin_bundle_group(G ID_PREFIX t. ALSO_SEPARATE A PLUGIN A SOURCES warn.cpp COMBINED_ONLY)
elseif(_case STREQUAL "register-invalid") # "is not a C++ function name"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp REGISTER 1bad)
elseif(_case STREQUAL "register-getpluginids") # "the Support library defines OFX::Plugin::getPluginIDs"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp REGISTER OFX::Plugin::getPluginIDs)
elseif(_case STREQUAL "property-reserved") # "PROPERTIES SUPPORTEXT_COMPONENTS: a keyword"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp PROPERTIES SUPPORTEXT_COMPONENTS THREADS)
elseif(_case STREQUAL "property-set") # "already has this property"
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN A SOURCES warn.cpp PROPERTIES CXX_STANDARD 20)
elseif(_case STREQUAL "combined-name-clash") # "is also the target of PLUGIN G's separate bundle"
  # (an error in the default COMBINED mode too: in SEPARATE mode, both targets would be made)
  ofx_plugin_bundle_group(G ID_PREFIX t. PLUGIN G SOURCES warn.cpp PLUGIN T SOURCES warn2.cpp COMBINED_ONLY)
elseif(_case STREQUAL "ok-same-name") # succeeds: a group of one plug-in of the same name, both modes
  set(OFX_PLUGIN_BUNDLES_S SEPARATE)
  ofx_plugin_bundle_group(S PLUGIN S ID t.S SOURCES warn.cpp)
  set(OFX_PLUGIN_BUNDLES_C COMBINED)
  ofx_plugin_bundle_group(C PLUGIN C ID t.C SOURCES warn2.cpp)
elseif(_case STREQUAL "bundle-name-case") # "the bundle t.a.ofx.bundle is already made by another group"
  set(OFX_PLUGIN_BUNDLES SEPARATE)
  ofx_plugin_bundle_group(G1 PLUGIN A ID t.A SOURCES warn.cpp)
  ofx_plugin_bundle_group(G2 PLUGIN B ID t.a SOURCES warn2.cpp)
elseif(_case STREQUAL "warnings") # succeeds, with warnings (the INFO_PLIST one in COMBINED mode)
  file(WRITE "${CMAKE_BINARY_DIR}/A.plist" "<plist><dict><key>CFBundleExecutable</key><string>A.ofx</string></dict></plist>\n")
  ofx_plugin_bundle_group(G
    PLUGIN A ID t.NotInTheSources SOURCES warn.cpp $<$<CONFIG:Debug>:warn2.cpp> COMPILE_DEFINITIONS X
      INFO_PLIST "${CMAKE_BINARY_DIR}/A.plist"
    PLUGIN B ID t.B SOURCES warn3.cpp REGISTER ns::getPluginIDs
    PLUGIN C ID t.NoCheck SOURCES warn2.cpp NO_ID_CHECK # no ID warning
    PLUGIN D ID t.InHeader SOURCES hdr/id.cpp)           # the ID is in hdr/id.h: no ID warning
else()
  message(FATAL_ERROR "unknown OFX_TEST_CASE '${_case}'")
endif()
