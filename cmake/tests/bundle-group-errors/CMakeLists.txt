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
elseif(_case STREQUAL "warnings") # succeeds, with the getPluginIDs and ID warnings
  ofx_plugin_bundle_group(G PLUGIN A ID t.NotInTheSources SOURCES warn.cpp)
else()
  message(FATAL_ERROR "unknown OFX_TEST_CASE '${_case}'")
endif()
