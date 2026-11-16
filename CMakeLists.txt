# CMake build of the OpenFX headers, the HostSupport and Support libraries,
# and (optionally) the example plug-ins and hosts.
#
# Ported from AcademySoftwareFoundation/openfx and adapted to the
# NatronGitHub fork: the fork's extension headers and defines, the bundled
# expat, and the fork's examples. It is also the reference for the CMake build of the plug-in
# repositories (openfx-misc, openfx-io, ...: cmake/OpenFX.cmake and
# Documentation/proposals/cmake-plugins.md); the Makefiles are kept until Natron's build switches.
cmake_minimum_required(VERSION 3.16.0)

option(OFX_SUPPORTS_CUDARENDER "Build examples with support for CUDA GPU rendering" OFF)
if(APPLE)
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
option(OFX_BUILD_HOSTSUPPORT "Build the HostSupport library (and the bundled expat)" ${_ofx_top_level})
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
option(OFX_SUPPORTS_OPENCLRENDER "Build examples with support for OpenCL GPU rendering" OFF)

# Host and plug-in API extensions of this fork. The defaults match what the
# Natron plug-ins (openfx-misc, openfx-io, openfx-arena) are built with.
option(OFX_EXTENSIONS_NATRON "Natron extensions (ofxNatron.h)" ON)
option(OFX_EXTENSIONS_NUKE "Nuke extensions (nuke/)" ON)
option(OFX_EXTENSIONS_VEGAS "Sony Vegas extensions (ofxSonyVegas.h)" ON)
option(OFX_EXTENSIONS_TUTTLE "TuttleOFX extensions (tuttle/)" ON)
option(OFX_EXTENSIONS_RESOLVE "DaVinci Resolve extensions" OFF)
# Host-side features: they add pure virtual functions to OFX::Host::ImageEffect::Host,
# so only enable them for a host that implements them (Natron enables both).
option(OFX_SUPPORTS_MULTITHREAD "HostSupport: the host implements OfxMultiThreadSuiteV1" OFF)
option(OFX_SUPPORTS_DIALOG "HostSupport: the host implements the dialog suite" OFF)
set(OFX_EXTRA_DEFINITIONS "" CACHE STRING
    "Additional compile definitions for the libraries and examples (;-separated)")

option(OFX_USE_SYSTEM_EXPAT "HostSupport: use the system expat (find_package(EXPAT)) instead of the bundled HostSupport/expat-2.10.0" OFF)

option(BUILD_EXAMPLE_PLUGINS "Build the example plug-ins (Examples/, Support/Plugins/, Support/PropTester/)" OFF)
option(BUILD_EXAMPLE_HOSTS "Build the example hosts (HostSupport/examples: hostDemo, cacheDemo, propsetDump)" OFF)
# the example hosts need HostSupport, also when openfx is used with add_subdirectory()
if(BUILD_EXAMPLE_HOSTS AND NOT OFX_BUILD_HOSTSUPPORT)
  set(OFX_BUILD_HOSTSUPPORT ON CACHE BOOL "Build the HostSupport library (and the bundled expat)" FORCE)
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
    OFX_SUPPORTS_MULTITHREAD OFX_SUPPORTS_DIALOG)
  if(${_def})
    list(APPEND OFX_DEFINITIONS ${_def})
  endif()
endforeach()
list(APPEND OFX_DEFINITIONS ${OFX_EXTRA_DEFINITIONS})
if(APPLE)
  list(APPEND OFX_DEFINITIONS GL_SILENCE_DEPRECATION)
endif()
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
if(NOT OFX_BUILD_HOSTSUPPORT)
  # no HostSupport, no expat
elseif(OFX_USE_SYSTEM_EXPAT)
  find_package(EXPAT REQUIRED)
  if(NOT TARGET expat::expat)
    # CMake's FindEXPAT provides EXPAT::EXPAT; conan's CMakeDeps provides expat::expat
    add_library(expat::expat INTERFACE IMPORTED)
    target_link_libraries(expat::expat INTERFACE EXPAT::EXPAT)
  endif()
else()
  set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
  set(EXPAT_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
  set(EXPAT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory(HostSupport/expat-2.10.0 EXCLUDE_FROM_ALL)
  set_target_properties(expat PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
target_compile_definitions(OfxApi INTERFACE ${OFX_DEFINITIONS}
  "$<$<AND:$<PLATFORM_ID:Windows>,$<NOT:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:_USE_MATH_DEFINES;__USE_MINGW_ANSI_STDIO=1>")
target_compile_options(OfxApi INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:__cplusplus>")

# ---------------------------------------------------------------------------
if(OFX_BUILD_HOSTSUPPORT)
  add_subdirectory(HostSupport)
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
install(DIRECTORY HostSupport/include/ DESTINATION include/HostSupport FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Support/include/ DESTINATION include/Support FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Support/Plugins/include/ DESTINATION include/Support/Plugins
        FILES_MATCHING PATTERN "*.h" PATTERN "*.H")
install(FILES cmake/OpenFX.cmake Examples/Info.plist.in DESTINATION lib/cmake/openfx)
install(PROGRAMS Support/include/osxDeploy.sh DESTINATION lib/cmake/openfx)
set(_ofx_export_targets OfxApi OfxSupport)
if(OFX_BUILD_HOSTSUPPORT)
  list(APPEND _ofx_export_targets OfxHost)
endif()
if(OFX_BUILD_HOSTSUPPORT AND NOT OFX_USE_SYSTEM_EXPAT)
  # static bundled expat must be exported with OfxHost
  list(APPEND _ofx_export_targets expat)
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
