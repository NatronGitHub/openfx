# CMake build of the OpenFX headers, the HostSupport and Support libraries,
# and (optionally) the example plug-ins and hosts.
#
# Ported from AcademySoftwareFoundation/openfx and adapted to the
# NatronGitHub fork: the fork's extension headers and defines, the system
# expat, and the fork's examples. It is also the reference for the CMake build of the plug-in
# repositories (openfx-misc, openfx-io, ...: cmake/OpenFX.cmake and
# Documentation/proposals/cmake-plugins.md); the Makefiles are kept until Natron's build switches.
# The policies up to CMake 3.31 are NEW when this CMake knows them (later policies stay OLD: raise the upper bound after testing a newer CMake), e.g. CMP0177 (install(DESTINATION)
# paths are normalized; e.g. PLUGIN_INSTALLDIR=.). cmake/OpenFX.cmake sets the same policies for its
# functions.
cmake_minimum_required(VERSION 3.21...3.31)

# macOS: the CMake-built plug-ins, libraries and hosts need macOS 14.0 (VFX Reference Platform
# CY2027) unless CMAKE_OSX_DEPLOYMENT_TARGET, the MACOSX_DEPLOYMENT_TARGET environment variable or a
# toolchain file (e.g. conan's, from os.version) says otherwise. CMake reads it at the first
# project(), so this only applies when openfx is the top-level project: plug-in projects set their
# own (see cmake/OpenFX.cmake). Not for cross builds to other systems (CMAKE_SYSTEM_NAME, e.g. iOS).
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
   AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version")
endif()
option(OFX_SUPPORTS_CUDARENDER "Build examples with support for CUDA GPU rendering" OFF)
# nvcc doesn't run on macOS hosts (no CUDA since macOS 10.13)
if(CMAKE_HOST_APPLE AND OFX_SUPPORTS_CUDARENDER)
  message(STATUS "OFX_SUPPORTS_CUDARENDER is turned off: CUDA is not available on macOS")
  set(OFX_SUPPORTS_CUDARENDER OFF CACHE BOOL "" FORCE)
endif()
if(OFX_SUPPORTS_CUDARENDER)
  project(openfx VERSION 1.5.1 LANGUAGES C CXX CUDA)
else()
  project(openfx VERSION 1.5.1 LANGUAGES C CXX)
endif()

# A plug-in repository that uses openfx with add_subdirectory() usually needs only the headers and
# the Support library: HostSupport (and expat) and openfx's install rules are then off by default.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(_ofx_top_level ON)
else()
  set(_ofx_top_level OFF)
endif()
option(OFX_BUILD_HOSTSUPPORT "Build the HostSupport library (needs expat)" ${_ofx_top_level})
option(OFX_INSTALL "Install the headers, libraries and CMake package of openfx" ${_ofx_top_level})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES AND _ofx_top_level)
  # universal binaries, like the Makefiles; override with -DCMAKE_OSX_ARCHITECTURES=arm64
  set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
endif()

# ---------------------------------------------------------------------------
# Options

# Rendering extensions
option(OFX_SUPPORTS_OPENGLRENDER "OpenGL render support (OFX_SUPPORTS_OPENGLRENDER)" ON)
# The OpenGL backend of OFX::DrawArgs::getDrawContext() (Support/Library/ofxsDrawContextOpenGL.cpp):
# OFF compiles it with OFXS_DRAWCONTEXT_NO_OPENGL (getDrawContext() then draws nothing on V1
# overlays). It needs no OpenGL header (include/ofxDrawSuiteOpenGL.h), and neither openfx::Support
# nor the plug-ins that call getDrawContext() need to link OpenGL: the backend loads it at run time,
# in the draw actions that may use OpenGL (V1 entries, parameter interacts).
option(OFX_SUPPORT_DRAWCONTEXT_OPENGL "Support: OpenGL backend of OFX::DrawArgs::getDrawContext()" ON)
option(OFX_SUPPORTS_OPENCLRENDER "Build examples with support for OpenCL GPU rendering" OFF)

# Host and plug-in API extensions of this fork. The defaults match what the
# Natron plug-ins (openfx-misc, openfx-io, openfx-arena) are built with.
option(OFX_EXTENSIONS_NATRON "Natron extensions (ofxNatron.h)" ON)
option(OFX_EXTENSIONS_NUKE "Nuke extensions (nuke/)" ON)
option(OFX_EXTENSIONS_VEGAS "Sony Vegas extensions (ofxSonyVegas.h)" ON)
option(OFX_EXTENSIONS_TUTTLE "TuttleOFX extensions (tuttle/)" ON)
option(OFX_EXTENSIONS_RESOLVE "DaVinci Resolve extensions" OFF)
if(OFX_EXTENSIONS_NATRON AND NOT OFX_EXTENSIONS_NUKE)
  message(FATAL_ERROR "OFX_EXTENSIONS_NATRON needs OFX_EXTENSIONS_NUKE (the Natron extensions use the Nuke ones)")
endif()
# Host-side features: they add pure virtual functions to OFX::Host::ImageEffect::Host,
# so only enable them for a host that implements them (Natron enables both).
option(OFX_SUPPORTS_MULTITHREAD "HostSupport: the host implements OfxMultiThreadSuiteV1" OFF)
option(OFX_SUPPORTS_DIALOG "HostSupport: the host implements the dialog suite" OFF)
# Parametric parameters (ofxParametricParam.h) in HostSupport: the host provides
# ofxhParametricParam.h and its implementation (e.g. Natron's libs/OpenFX_extensions), and adds
# them to the HostSupport target after add_subdirectory(openfx EXCLUDE_FROM_ALL), with absolute paths:
#   target_sources(OfxHost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/<dir>/ofxhParametricParam.cpp)
#   target_include_directories(OfxHost PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/<dir>>)
# OFX_SUPPORTS_PARAMETRIC_V2 needs an implementation of the V2 suite (Natron's doesn't build with it).
option(OFX_SUPPORTS_PARAMETRIC "HostSupport: the host provides parametric parameters (ofxhParametricParam.h)" OFF)
option(OFX_SUPPORTS_PARAMETRIC_V2 "HostSupport: parametric parameters with interpolations (needs OFX_SUPPORTS_PARAMETRIC and a V2 implementation)" OFF)
if(OFX_SUPPORTS_PARAMETRIC_V2 AND NOT OFX_SUPPORTS_PARAMETRIC)
  message(WARNING "OFX_SUPPORTS_PARAMETRIC_V2 has no effect on HostSupport without OFX_SUPPORTS_PARAMETRIC")
endif()
set(OFX_EXTRA_DEFINITIONS "" CACHE STRING
    "Additional compile definitions for the libraries and examples (;-separated)")


option(BUILD_EXAMPLE_PLUGINS "Build the example plug-ins (Examples/, Support/Plugins/, Support/PropTester/)" OFF)
option(BUILD_EXAMPLE_HOSTS "Build the example hosts (HostSupport/examples: hostDemo, cacheDemo, propsetDump, suiteHandleChecks)" OFF)
option(OFX_PYTHON_BINDINGS "HostSupport Python bindings (nanobind)" OFF)
# openfx::HostSupportOpenGL (HostSupport/include/ofxhOpenGL*.h): offscreen OpenGL contexts, textures
# and renders for hosts that implement the OpenGL render suite. Needs OFX_SUPPORTS_OPENGLRENDER.
option(OFX_HOSTSUPPORT_OPENGL "HostSupport: offscreen OpenGL rendering (openfx::HostSupportOpenGL)" OFF)
# the example hosts and the Python bindings need HostSupport, also when openfx is used with
# add_subdirectory()
if((BUILD_EXAMPLE_HOSTS OR OFX_PYTHON_BINDINGS OR OFX_HOSTSUPPORT_OPENGL) AND NOT OFX_BUILD_HOSTSUPPORT)
  set(OFX_BUILD_HOSTSUPPORT ON CACHE BOOL "Build the HostSupport library (needs expat)" FORCE)
endif()
set(PLUGIN_INSTALLDIR "" CACHE STRING "Where the OFX bundles are installed (relative: to CMAKE_INSTALL_PREFIX; default depends on OS)")

# ---------------------------------------------------------------------------
# Compile definitions shared by the libraries and anything linking them.
# They change class layouts in Support and HostSupport, so they are PUBLIC
# usage requirements of the library targets (see OFX_DEFINITIONS below).
set(OFX_DEFINITIONS "")
foreach(_def
    OFX_SUPPORTS_OPENGLRENDER OFX_SUPPORTS_OPENCLRENDER OFX_SUPPORTS_CUDARENDER
    OFX_EXTENSIONS_NATRON OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_VEGAS
    OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_RESOLVE
    OFX_SUPPORTS_MULTITHREAD OFX_SUPPORTS_DIALOG
    OFX_SUPPORTS_PARAMETRIC OFX_SUPPORTS_PARAMETRIC_V2)
  if(${_def})
    list(APPEND OFX_DEFINITIONS ${_def})
  endif()
endforeach()
list(APPEND OFX_DEFINITIONS ${OFX_EXTRA_DEFINITIONS})
# GL_SILENCE_DEPRECATION (Apple's OpenGL headers) is a plug-in-side definition: it is set by
# add_ofx_plugin and openfx::Support, not by openfx::Api or openfx::HostSupport (hosts don't need it)
if(OFX_SUPPORTS_OPENCLRENDER AND NOT APPLE)
  find_package(OpenCLHeaders REQUIRED)
  find_package(OpenCLICDLoader REQUIRED)
endif()
if(OFX_SUPPORTS_CUDARENDER)
  set(CMAKE_CUDA_ARCHITECTURES "all-major")
endif()

# We use #if DEBUG in the examples
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")

if(MSVC)
  add_compile_definitions(_HAS_AUTO_PTR_ETC NOMINMAX)
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations")
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# expat, for the HostSupport plug-in cache
if(OFX_BUILD_HOSTSUPPORT)
  include(OpenFXExpat)
  ofx_find_expat()
  if(NOT TARGET expat::expat)
    message(FATAL_ERROR "HostSupport needs the expat library and headers, which were not found. Install "
      "them (e.g. libexpat1-dev, MacPorts expat, MSYS2 mingw-w64-ucrt-x86_64-expat, vcpkg or conan "
      "expat), or set EXPAT_INCLUDE_DIR and EXPAT_LIBRARY, or build without HostSupport "
      "(-DOFX_BUILD_HOSTSUPPORT=OFF, which also needs -DBUILD_EXAMPLE_HOSTS=OFF: the example hosts need "
      "HostSupport).")
  endif()
  # the minimum expat version for the installed package: ofxhPluginCache.cpp uses the 64-bit line
  # and column functions with expat >= 2.9 headers
  if(EXPAT_VERSION)
    set(_ofx_expat_version "${EXPAT_VERSION}")
  else()
    set(_ofx_expat_version "${EXPAT_VERSION_STRING}")
  endif()
  # (older headers use functions that every expat 2 has: no minimum)
  if(_ofx_expat_version VERSION_GREATER_EQUAL 2.9)
    set(OFX_EXPAT_VERSION 2.9)
  else()
    set(OFX_EXPAT_VERSION "")
  endif()
endif()

# Optional dependencies of the example plug-ins only
if(BUILD_EXAMPLE_PLUGINS)
  find_package(OpenGL REQUIRED)
endif()

# Macros (add_ofx_plugin)
include(OpenFX)
set_property(GLOBAL PROPERTY OFX_PROJECT_SOURCE_DIR "${PROJECT_SOURCE_DIR}")

# C API headers, including the fork's extension headers (ofxNatron.h,
# ofxSonyVegas.h, nuke/, tuttle/)
set(OFX_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
file(GLOB_RECURSE OFX_HEADER_FILES "${OFX_HEADER_DIR}/*.h")
add_library(OfxApi INTERFACE)
target_include_directories(OfxApi INTERFACE
  $<BUILD_INTERFACE:${OFX_HEADER_DIR}>
  $<INSTALL_INTERFACE:include>)
set_target_properties(OfxApi PROPERTIES EXPORT_NAME Api)
add_library(openfx::Api ALIAS OfxApi)
# Usage requirements of every plug-in, built on Support or on the C API alone
# (Documentation/proposals/cmake-plugins.md): the OFX_EXTENSIONS_* and OFX_SUPPORTS_* definitions
# (they change class layouts), the MinGW definitions of Support/Plugins/Makefile.master, and
# MSVC's real __cplusplus value.
# (decided at configure time: $<CXX_COMPILER_FRONTEND_VARIANT> needs CMake 3.30, and the
# expression would be exported to the consumers of the package)
target_compile_definitions(OfxApi INTERFACE ${OFX_DEFINITIONS})
if(WIN32 AND NOT MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_definitions(OfxApi INTERFACE _USE_MATH_DEFINES __USE_MINGW_ANSI_STDIO=1)
endif()
target_compile_options(OfxApi INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:__cplusplus>")

# ---------------------------------------------------------------------------
if(OFX_BUILD_HOSTSUPPORT)
  add_subdirectory(HostSupport)
endif()
if(OFX_PYTHON_BINDINGS)
  # the ofxh module (HostSupport/python/src/ofxhPython.h; HostSupport/examples/python/minimal_host.py)
  add_subdirectory(HostSupport/python)
endif()
add_subdirectory(Support)
if(BUILD_EXAMPLE_PLUGINS)
  add_subdirectory(Examples)
endif()

# ---------------------------------------------------------------------------
# Install: headers, libraries, and a CMake package (find_package(openfx))
if(OFX_INSTALL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/ DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "DocSrc" EXCLUDE)
if(OFX_HOSTSUPPORT_OPENGL)
  install(DIRECTORY HostSupport/include/ DESTINATION include/HostSupport FILES_MATCHING PATTERN "*.h")
else()
  # without openfx::HostSupportOpenGL, not its headers
  install(DIRECTORY HostSupport/include/ DESTINATION include/HostSupport FILES_MATCHING PATTERN "*.h"
          PATTERN "ofxhOpenGLContext.h" EXCLUDE PATTERN "ofxhOpenGLTexture.h" EXCLUDE PATTERN "ofxhOpenGLRender.h" EXCLUDE)
endif()
install(DIRECTORY Support/include/ DESTINATION include/Support FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Support/Plugins/include/ DESTINATION include/Support/Plugins
        FILES_MATCHING PATTERN "*.h" PATTERN "*.H")
install(FILES cmake/OpenFX.cmake cmake/OpenFXExpat.cmake Examples/Info.plist.in DESTINATION lib/cmake/openfx)
install(PROGRAMS Support/include/osxDeploy.sh Support/include/linuxCheckBundle.sh DESTINATION lib/cmake/openfx)
set(_ofx_export_targets OfxApi OfxSupport)
if(OFX_BUILD_HOSTSUPPORT)
  list(APPEND _ofx_export_targets OfxHost)
  if(OFX_HOSTSUPPORT_OPENGL)
    list(APPEND _ofx_export_targets OfxHostOpenGL)
  endif()
endif()
install(TARGETS ${_ofx_export_targets} EXPORT openfxTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT openfxTargets NAMESPACE openfx:: DESTINATION lib/cmake/openfx)
configure_package_config_file(cmake/openfxConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/openfxConfig.cmake"
  INSTALL_DESTINATION lib/cmake/openfx)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/openfxConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openfxConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/openfxConfigVersion.cmake"
        DESTINATION lib/cmake/openfx)
endif()
