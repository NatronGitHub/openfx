# SPDX-License-Identifier: BSD-3-Clause
# A plug-in project built against openfx the two ways plug-in repositories use it: an installed
# package (find_package(openfx)), or the source tree (add_subdirectory, OFX_VIA_SUBDIRECTORY=ON). It exercises the cmake/OpenFX.cmake API; checked by
# .github/scripts/cmake-plugin-project.sh.
cmake_minimum_required(VERSION 3.16)
project(OfxPluginProjectTest CXX)

option(OFX_VIA_SUBDIRECTORY "use openfx with add_subdirectory() instead of find_package()" OFF)
if(OFX_VIA_SUBDIRECTORY)
  add_subdirectory("${OFX_SOURCE_DIR}" openfx EXCLUDE_FROM_ALL)
else()
  find_package(openfx REQUIRED)
endif()
find_package(OpenGL REQUIRED)
if(NOT OFX_SOURCE_DIR)
  message(FATAL_ERROR "set OFX_SOURCE_DIR to the openfx source tree (for the sample plug-in source)")
endif()

# a bundle name, identifier and version of our own, resources, OpenMP, deployment
add_ofx_plugin(TestInvert
  BUNDLE_NAME InvertBundle
  IDENTIFIER org.example.InvertBundle
  VERSION 2.3.4
  SOURCES "${OFX_SOURCE_DIR}/Support/Plugins/Invert/invert.cpp" includes.cpp)
target_link_libraries(TestInvert PRIVATE openfx::Support)
ofx_plugin_resources(TestInvert icon.txt)
ofx_plugin_resources(TestInvert DESTINATION presets preset.txt)
ofx_plugin_resources(TestInvert shaders) # a directory, copied with its contents
# REQUIRED on CI (OFX_TEST_REQUIRE_OPENMP), so that the bundled libomp is really checked
if(OFX_TEST_REQUIRE_OPENMP)
  ofx_target_openmp(TestInvert REQUIRED)
else()
  ofx_target_openmp(TestInvert)
endif()
ofx_deploy_bundle(TestInvert)

# the plug-in's own Info.plist, used as-is
add_ofx_plugin(TestPlist
  INFO_PLIST Info.plist
  RPATH /opt/ofx-test-rpath # an extra install rpath directory (not deployed: it stays)
  SOURCES "${OFX_SOURCE_DIR}/Support/Plugins/Basic/basic.cpp")
# an imported dependency listing a missing include directory, through an alias and another
# imported target (as OpenImageIO's config on Ubuntu 24.04, with /usr/include/opencv4)
add_library(TestMissingIncInner INTERFACE IMPORTED)
set_target_properties(TestMissingIncInner PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_BINARY_DIR}/no-such-dir")
add_library(TestMissingInc INTERFACE IMPORTED)
set_target_properties(TestMissingInc PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "/no-such-system-dir"
  INTERFACE_INCLUDE_DIRECTORIES "/no-such-dir-either"
  INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:TestMissingIncInner>;TestMissingIncInner")
ofx_target_filter_missing_includes(TestMissingInc)
get_target_property(_inc TestMissingIncInner INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_inc2 TestMissingInc INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _inc STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}" OR _inc2)
  message(FATAL_ERROR "ofx_target_filter_missing_includes: '${_inc}' '${_inc2}'")
endif()
target_link_libraries(TestPlist openfx::Support OpenGL::GL TestMissingInc) # the plain form works too
ofx_plugin_resources(TestPlist icon.txt) # not deployed: the installed bundle's signature must still be valid
