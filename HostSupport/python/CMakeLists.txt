# Python bindings of HostSupport (nanobind): the ofxh module (-DOFX_PYTHON_BINDINGS=ON).
# Needs Python >= 3.10 (Interpreter and Development.Module) and nanobind, found from the Python
# interpreter (pip install nanobind; or set nanobind_DIR). The module links OfxHost, so it is
# compiled with the same OFX_* definitions (PUBLIC usage requirements of OfxHost).
# (a subdirectory: no cmake_minimum_required, which would reset the policies of the top level;
# the top level's minimum version is enough for nanobind)

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
  src/ofxhPythonModule.cpp
  src/ofxhPythonParams.cpp)
target_link_libraries(ofxh PRIVATE OfxHost)
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
