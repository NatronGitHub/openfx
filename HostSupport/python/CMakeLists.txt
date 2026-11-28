# Python bindings of HostSupport (nanobind): the ofxh module.
# Two ways to build it:
# - in the openfx tree, with -DOFX_PYTHON_BINDINGS=ON (the top-level CMakeLists.txt adds this
#   directory): the module is <build>/HostSupport/python/ofxh.*;
# - as its own project, which adds the openfx tree (../..) with the options it needs: this is
#   what pyproject.toml does (scikit-build-core: pip install ./HostSupport/python), and the
#   module is installed at the top of the wheel. The other openfx options (the extensions...)
#   keep their defaults; set them with -C cmake.define.OFX_EXTENSIONS_VEGAS=OFF, for example.
# Needs Python >= 3.10 (Interpreter and Development.Module) and nanobind, found from the Python
# interpreter (pip install nanobind; or set nanobind_DIR). The module links OfxHost, so it is
# compiled with the same OFX_* definitions (PUBLIC usage requirements of OfxHost).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # (the policies and the minimum of the openfx tree)
  cmake_minimum_required(VERSION 3.21...3.31)
  # macOS 14.0 (VFX Reference Platform CY2027) as the openfx tree's rule, which must come before the
  # first project(): after it, CMake's Darwin initialisation has defined CMAKE_OSX_DEPLOYMENT_TARGET
  # (empty), and the tree's rule no longer applies. MACOSX_DEPLOYMENT_TARGET (which pip or
  # scikit-build-core may set) wins.
  if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
     AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version")
  endif()
  project(ofxh LANGUAGES C CXX)
  set(_ofxh_standalone ON)
  # HostSupport only: no examples, no install of openfx, and not this directory again
  set(OFX_BUILD_HOSTSUPPORT ON CACHE BOOL "" FORCE)
  set(OFX_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
  set(OFX_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLE_HOSTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." openfx EXCLUDE_FROM_ALL)
else()
  # (a subdirectory of the openfx tree: no cmake_minimum_required, which would reset the policies
  # of the top level; the top level's minimum version is enough for nanobind)
  set(_ofxh_standalone OFF)
endif()

# the host functions that these options make pure virtual are not implemented
if(OFX_SUPPORTS_MULTITHREAD OR OFX_SUPPORTS_DIALOG)
  message(FATAL_ERROR "OFX_PYTHON_BINDINGS: the ofxh module doesn't implement the host functions of "
    "OFX_SUPPORTS_MULTITHREAD and OFX_SUPPORTS_DIALOG: turn them off")
endif()

find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
if(NOT nanobind_DIR)
  execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
                  OUTPUT_VARIABLE _ofx_nanobind_dir OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE _ofx_nanobind_result)
  if(_ofx_nanobind_result EQUAL 0)
    set(nanobind_DIR "${_ofx_nanobind_dir}" CACHE PATH "nanobind's CMake directory")
  endif()
endif()
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(ofxh NB_STATIC
  src/ofxhPython.h
  src/ofxhPythonAdapters.cpp
  src/ofxhPythonDescribe.cpp
  src/ofxhPythonModule.cpp
  src/ofxhPythonParams.cpp)
target_link_libraries(ofxh PRIVATE OfxHost)
if(TARGET OfxHostOpenGL)
  # OpenGL renders (ofxh.init(opengl=True)): openfx::HostSupportOpenGL (OFX_HOSTSUPPORT_OPENGL)
  target_link_libraries(ofxh PRIVATE OfxHostOpenGL)
  target_compile_definitions(ofxh PRIVATE OFXH_OPENGL)
endif()
if(TARGET OfxHostOpenCL)
  # OpenCL renders (ofxh.init(opencl=True)): openfx::HostSupportOpenCL (OFX_HOSTSUPPORT_OPENCL)
  target_link_libraries(ofxh PRIVATE OfxHostOpenCL)
  target_compile_definitions(ofxh PRIVATE OFXH_OPENCL)
endif()
if(TARGET OfxHostMetal)
  # Metal renders (ofxh.init(metal=True)): openfx::HostSupportMetal (OFX_HOSTSUPPORT_METAL, macOS)
  target_link_libraries(ofxh PRIVATE OfxHostMetal)
  target_compile_definitions(ofxh PRIVATE OFXH_METAL)
endif()
target_compile_features(ofxh PRIVATE cxx_std_17)
set_target_properties(ofxh PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
# export only the module's init function, not the symbols of the static libraries (HostSupport,
# expat, nanobind)
if(APPLE)
  target_link_options(ofxh PRIVATE "LINKER:-exported_symbol,_PyInit_ofxh")
elseif(NOT WIN32)
  target_link_options(ofxh PRIVATE "LINKER:--exclude-libs,ALL")
endif()

if(_ofxh_standalone)
  # the wheel (scikit-build-core): the module at its top
  install(TARGETS ofxh LIBRARY DESTINATION .)
endif()
