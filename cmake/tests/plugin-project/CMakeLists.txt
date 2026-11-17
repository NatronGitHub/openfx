# SPDX-License-Identifier: BSD-3-Clause
# A plug-in project built against openfx the two ways plug-in repositories use it: an installed
# package (find_package(openfx)), or the source tree (add_subdirectory, OFX_VIA_SUBDIRECTORY=ON). It exercises the cmake/OpenFX.cmake API; checked by
# .github/scripts/cmake-plugin-project.sh.
cmake_minimum_required(VERSION 3.16)
project(OfxPluginProjectTest CXX)

option(OFX_VIA_SUBDIRECTORY "use openfx with add_subdirectory() instead of find_package()" OFF)
if(OFX_VIA_SUBDIRECTORY)
  # (without EXCLUDE_FROM_ALL, so that the install rules of openfx run: they must not install
  # openfx itself, its example hosts or the bundled expat headers)
  add_subdirectory("${OFX_SOURCE_DIR}" openfx)
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
# Apple clang: a dependency that links -lomp by name (as a static ImageMagick's .pc does) finds
# libomp through the link directory that ofx_target_openmp adds
if(OFX_TEST_REQUIRE_OPENMP AND APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  add_library(TestOmpByName INTERFACE)
  target_link_libraries(TestOmpByName INTERFACE omp)
  target_link_libraries(TestInvert PRIVATE TestOmpByName)
endif()
ofx_deploy_bundle(TestInvert)

# the plug-in's own Info.plist, used as-is
add_ofx_plugin(TestPlist
  INFO_PLIST Info.plist
  RPATH /opt/ofx-test-rpath # an extra install rpath directory (not deployed: it stays)
  SOURCES "${OFX_SOURCE_DIR}/Support/Plugins/Basic/basic.cpp")
# an imported dependency listing missing include directories, used through an alias, with an
# inner imported target reached only through $<LINK_ONLY:> (as OpenImageIO's config on Ubuntu
# 24.04, with /usr/include/opencv4)
add_library(TestMissingIncInner INTERFACE IMPORTED)
set_target_properties(TestMissingIncInner PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_BINARY_DIR}/no-such-dir")
add_library(TestMissingInc INTERFACE IMPORTED GLOBAL) # (GLOBAL: an ALIAS needs it with CMake < 3.18)
set_target_properties(TestMissingInc PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "/no-such-system-dir"
  INTERFACE_INCLUDE_DIRECTORIES "/no-such-dir-either"
  INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:TestMissingIncInner>")
add_library(Test::MissingInc ALIAS TestMissingInc)
ofx_target_filter_missing_includes(Test::MissingInc)
get_target_property(_inc TestMissingIncInner INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_inc2 TestMissingInc INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_inc3 TestMissingInc INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
if(NOT _inc STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}" OR _inc2 OR _inc3)
  message(FATAL_ERROR "ofx_target_filter_missing_includes: '${_inc}' '${_inc2}' '${_inc3}'")
endif()
# same-named imported targets in two sibling directories, each filtered, each linked
add_subdirectory(filter-a)
add_subdirectory(filter-b)
target_link_libraries(TestPlist openfx::Support OpenGL::GL Test::MissingInc) # the plain form works too
ofx_plugin_resources(TestPlist icon.txt) # not deployed: the installed bundle's signature must still be valid

# not deployed, with OpenMP: on macOS its install rpath has no libomp (MacPorts) directory, but
# keeps that of a library with an @rpath install name in an SDK prefix (OFX_TEST_SDK_LIBRARY,
# built by cmake-plugin-project.sh)
add_ofx_plugin(TestOmpNoDeploy SOURCES "${OFX_SOURCE_DIR}/Support/Plugins/Basic/basic.cpp")
target_link_libraries(TestOmpNoDeploy PRIVATE openfx::Support OpenGL::GL)
ofx_target_openmp(TestOmpNoDeploy)
if(OFX_TEST_SDK_LIBRARY)
  add_library(TestSdkDep SHARED IMPORTED)
  set_target_properties(TestSdkDep PROPERTIES IMPORTED_LOCATION "${OFX_TEST_SDK_LIBRARY}")
  target_link_libraries(TestOmpNoDeploy PRIVATE TestSdkDep)
endif()
