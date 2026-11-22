# SPDX-License-Identifier: BSD-3-Clause
# The link-time check of the configuration macros (NatronGitHub/openfx#226): a plug-in on
# openfx::Support and a host on openfx::HostSupport, built with add_subdirectory(openfx) and its
# default options, link (targets plugin and host); compiled with one of the covered macros
# flipped (-D if the library lacks it, -U if it has it), they must fail to link, naming the
# configuration object (the mismatch-* targets, excluded from ALL, listed with the symbol each
# must name in mismatch-targets.txt). The mismatch targets are built with link-time optimization
# (when supported) and with unused-section removal, so that the check survives them.
# Checked by .github/scripts/cmake-config-check.sh.
cmake_minimum_required(VERSION 3.21...3.31)
project(OfxConfigCheckTest C CXX)
if(NOT OFX_SOURCE_DIR)
  message(FATAL_ERROR "set OFX_SOURCE_DIR to the openfx source tree")
endif()
set(OFX_BUILD_HOSTSUPPORT ON)
add_subdirectory("${OFX_SOURCE_DIR}" openfx EXCLUDE_FROM_ALL)

add_library(plugin MODULE plugin.cpp)
target_link_libraries(plugin PRIVATE openfx::Support)
set_target_properties(plugin PROPERTIES PREFIX "" SUFFIX ".ofx"
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
add_executable(host host.cpp)
target_link_libraries(host PRIVATE openfx::HostSupport)

include(CheckIPOSupported)
check_ipo_supported(RESULT _ofx_ipo OUTPUT _ofx_ipo_output LANGUAGES CXX)
get_target_property(_support_defs OfxSupport INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_host_defs OfxHost INTERFACE_COMPILE_DEFINITIONS)
set(_list "")
# mismatch(<kind> <macro>): <kind> is plugin (openfx::Support) or host (openfx::HostSupport)
function(mismatch kind macro)
  if(kind STREQUAL "plugin")
    set(_defs ${_support_defs})
    set(_symbol supportConfiguration_)
  else()
    set(_defs ${_host_defs})
    set(_symbol hostSupportConfiguration_)
  endif()
  set(_target "mismatch-${kind}-${macro}")
  if(macro IN_LIST _defs)
    set(_flags "-U${macro}")
    # the Natron extensions need the Nuke ones: undefine both
    if(macro STREQUAL "OFX_EXTENSIONS_NUKE" AND "OFX_EXTENSIONS_NATRON" IN_LIST _defs)
      list(APPEND _flags "-UOFX_EXTENSIONS_NATRON")
    endif()
  else()
    set(_flags "-D${macro}")
    if(macro STREQUAL "OFX_EXTENSIONS_NATRON" AND NOT "OFX_EXTENSIONS_NUKE" IN_LIST _defs)
      list(APPEND _flags "-DOFX_EXTENSIONS_NUKE")
    endif()
  endif()
  if(kind STREQUAL "plugin")
    add_library(${_target} MODULE EXCLUDE_FROM_ALL plugin.cpp)
    target_link_libraries(${_target} PRIVATE openfx::Support)
    set_target_properties(${_target} PROPERTIES PREFIX "" SUFFIX ".ofx"
      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  else()
    add_executable(${_target} EXCLUDE_FROM_ALL host.cpp)
    target_link_libraries(${_target} PRIVATE openfx::HostSupport)
  endif()
  # (compile options come after the definitions on the command line: -U wins)
  target_compile_options(${_target} PRIVATE ${_flags})
  if(_ofx_ipo)
    set_target_properties(${_target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
  if(APPLE)
    target_link_options(${_target} PRIVATE -Wl,-dead_strip)
  elseif(NOT MSVC) # (MSVC: /OPT:REF is the default of release links)
    target_compile_options(${_target} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(${_target} PRIVATE -Wl,--gc-sections)
  endif()
  set(_list "${_list}${_target} ${_symbol}\n" PARENT_SCOPE)
endfunction()

foreach(_m OFX_EXTENSIONS_NATRON OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_VEGAS OFX_EXTENSIONS_TUTTLE
           OFX_SUPPORTS_OPENGLRENDER OFX_SUPPORTS_DIALOG)
  mismatch(plugin ${_m})
endforeach()
foreach(_m OFX_EXTENSIONS_NATRON OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_VEGAS
           OFX_SUPPORTS_OPENGLRENDER OFX_SUPPORTS_MULTITHREAD OFX_SUPPORTS_DIALOG OFX_USE_STATIC_PLUGINS)
  mismatch(host ${_m})
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/mismatch-targets.txt" "${_list}")
