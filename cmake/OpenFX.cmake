# SPDX-License-Identifier: BSD-3-Clause
#
# add_ofx_plugin(): build an OFX plug-in bundle.
# Ported from AcademySoftwareFoundation/openfx; adapted for the NatronGitHub
# fork: the bundle is also assembled in the build tree (usable directly with
# OFX_PLUGIN_PATH), Info.plist is generated at build time instead of being
# written into PLUGIN_INSTALLDIR at configure time, and the Linux arch folder
# follows the build architecture.

if(NOT PLUGIN_INSTALLDIR)
  if(APPLE)
    set(PLUGIN_INSTALLDIR "/Library/OFX/Plugins/OpenFX Examples")
  elseif(WIN32)
    set(PLUGIN_INSTALLDIR "C:/Program Files/Common Files/OFX/Plugins/OpenFX Examples")
  elseif(UNIX)
    set(PLUGIN_INSTALLDIR "/usr/OFX/Plugins/OpenFX Examples")
  else()
    set(PLUGIN_INSTALLDIR "/unknown-os")
  endif()
endif()

# Bundle architecture folder (OFX packaging spec, ofxPackaging.rst). The names must match what
# HostSupport looks for (HostSupport/src/ofxhPluginCache.cpp, ofxhArchDirs.h) and the Makefiles
# (Support/Plugins/Makefile.master).
if(APPLE)
  set(ARCHDIR "MacOS") # universal or arch-specific: no arm64 folder
elseif(WIN32)
  # MSVC: the target architecture (ARM64EC, ARM64, x64, X86); other compilers: pointer size
  string(TOUPPER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID};${CMAKE_GENERATOR_PLATFORM}" _OFX_WINARCH)
  if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
    # MinGW (MSYS2 CLANGARM64, llvm-mingw) on CMake < 4.1: the architecture ID is only set for MSVC, and
    # CMAKE_SYSTEM_PROCESSOR is the build machine's unless a toolchain file sets it: ask the compiler for
    # its target (CMAKE_CXX_COMPILER_TARGET, or `-dumpmachine`), e.g. aarch64-w64-mingw32 or
    # arm64ec-w64-windows-gnu (llvm-mingw Arm64EC)
    set(_OFX_TARGET "${CMAKE_CXX_COMPILER_TARGET}")
    if(NOT _OFX_TARGET)
      execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_ARG1} -dumpmachine
                      OUTPUT_VARIABLE _OFX_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    string(TOLOWER "${_OFX_TARGET}" _OFX_TARGET)
    if(_OFX_TARGET MATCHES "^arm64ec")
      set(_OFX_WINARCH "ARM64EC")
    elseif(_OFX_TARGET MATCHES "^(aarch64|arm64)")
      set(_OFX_WINARCH "ARM64")
    endif()
  endif()
  if(_OFX_WINARCH MATCHES "ARM64EC" OR CMAKE_CXX_FLAGS MATCHES "/arm64EC")
    set(ARCHDIR "Win-arm64ec") # normative (upstream main, 2026): Arm64EC (and Arm64X) plug-ins
  elseif(_OFX_WINARCH MATCHES "ARM64|AARCH64")
    # CMake >= 4.1 reports "aarch64" as the MinGW architecture ID
    set(ARCHDIR "Win-arm64")   # reserved, not yet normative: native Arm64 hosts
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCHDIR "Win64")
  else()
    set(ARCHDIR "Win32")
  endif()
elseif(UNIX)
  if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_OFX_OS "FreeBSD")
  else()
    set(_OFX_OS "Linux")
  endif()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _OFX_CPU)
  if(_OFX_CPU MATCHES "^(x86_64|amd64|i[3-6]86|x86)$")
    # x86: use the pointer size, the userland may be 32-bit on a 64-bit kernel
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(ARCHDIR "${_OFX_OS}-x86-64")
    else()
      set(ARCHDIR "${_OFX_OS}-x86")
    endif()
  elseif(_OFX_CPU MATCHES "^armv[2-6]" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    # pre-ARMv7 (HostSupport: __ARM_ARCH < 7)
    if(_OFX_OS STREQUAL "FreeBSD")
      set(ARCHDIR "FreeBSD-armv6")
    else()
      set(ARCHDIR "Linux-armv6l")
    endif()
  elseif(_OFX_CPU MATCHES "^(aarch64|arm64|armv8l|arm.*)$")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(ARCHDIR "${_OFX_OS}-aarch64")
    elseif(_OFX_OS STREQUAL "FreeBSD")
      set(ARCHDIR "FreeBSD-armv7")
    else()
      set(ARCHDIR "Linux-armv7l")
    endif()
  elseif(_OFX_OS STREQUAL "FreeBSD" AND _OFX_CPU MATCHES "^riscv")
    set(ARCHDIR "FreeBSD-riscv64")
  elseif(_OFX_OS STREQUAL "FreeBSD" AND _OFX_CPU MATCHES "^powerpc")
    # uname -p: powerpc64, powerpc64le (uname -m only says powerpc)
    if(NOT DEFINED CMAKE_CXX_BYTE_ORDER) # CMake < 3.20
      include(TestBigEndian)
      test_big_endian(_OFX_BIG_ENDIAN)
      if(NOT _OFX_BIG_ENDIAN)
        set(CMAKE_CXX_BYTE_ORDER "LITTLE_ENDIAN")
      endif()
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_CXX_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
      set(ARCHDIR "FreeBSD-powerpc64le")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(ARCHDIR "FreeBSD-powerpc64")
    else()
      set(ARCHDIR "FreeBSD-powerpc")
    endif()
  else()
    set(ARCHDIR "${_OFX_OS}-${CMAKE_SYSTEM_PROCESSOR}") # Linux: uname -m (riscv64, ppc64le, s390x, ...)
  endif()
else()
  set(ARCHDIR "unknown-arch")
endif()

# Where bundles are assembled in the build tree
if(NOT OFX_PLUGIN_BUILD_DIR)
  set(OFX_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/Plugins")
endif()

# Location of the Info.plist template. In the OpenFX source tree it is in
# Examples/; in an installed package it sits next to this module.
set(_OFX_CMAKE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Add a new OFX plugin "target".
#
# Arguments: TARGET
# Optional argument: DIR, defaults to same as TARGET (use when renaming TARGET)
function(add_ofx_plugin TARGET)
  if(${ARGC} GREATER 1)
    set(DIR ${ARGN})
  else()
    set(DIR ${TARGET})
  endif()
  if(APPLE)
    add_library(${TARGET} MODULE) # build as an OSX bundle
  else()
    add_library(${TARGET} SHARED) # build as shared lib/DLL
  endif()
  set_target_properties(${TARGET} PROPERTIES SUFFIX ".ofx" PREFIX "")

  set(PLUGIN_NAME ${TARGET})
  set(PLUGIN_EXE ${TARGET}.ofx)
  set(BUNDLE_IDENTIFIER org.openeffects.plugins.${TARGET})
  set(BUNDLE_SIGNATURE "????")  # any value is OK here, ignored by modern MacOS
  set(PLUGIN_VERSION "1.0.0")

  set(_ofx_info_plist "")
  foreach(_dir "${_OFX_CMAKE_MODULE_DIR}"
               "${_OFX_CMAKE_MODULE_DIR}/../Examples"
               "${CMAKE_SOURCE_DIR}/Examples")
    if(EXISTS "${_dir}/Info.plist.in")
      set(_ofx_info_plist "${_dir}/Info.plist.in")
      break()
    endif()
  endforeach()
  if(NOT _ofx_info_plist)
    message(FATAL_ERROR "add_ofx_plugin: could not find Info.plist.in")
  endif()

  # Assemble the bundle in the build tree: <TARGET>.ofx.bundle/Contents/{Info.plist,<ARCHDIR>/<TARGET>.ofx}
  set(_bundle "${OFX_PLUGIN_BUILD_DIR}/${TARGET}.ofx.bundle")
  configure_file("${_ofx_info_plist}" "${_bundle}/Contents/Info.plist")
  set_target_properties(${TARGET} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_bundle}/Contents/${ARCHDIR}"
    RUNTIME_OUTPUT_DIRECTORY "${_bundle}/Contents/${ARCHDIR}")
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_cfg}" _CFG)
    set_target_properties(${TARGET} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_bundle}/Contents/${ARCHDIR}"
      RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_bundle}/Contents/${ARCHDIR}")
  endforeach()

  # Set symbol visibility hidden. Individual symbols are exposed via
  # __declspec(dllexport) or __attribute__((visibility("default"))) (OfxExport)
  set_target_properties(${TARGET} PROPERTIES C_VISIBILITY_PRESET hidden
                                             CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON)

  # To install plugins: cmake --install Build
  install(
    TARGETS ${TARGET}
    LIBRARY DESTINATION "${PLUGIN_INSTALLDIR}/${TARGET}.ofx.bundle/Contents/${ARCHDIR}"
    RUNTIME DESTINATION "${PLUGIN_INSTALLDIR}/${TARGET}.ofx.bundle/Contents/${ARCHDIR}")
  install(FILES "${_bundle}/Contents/Info.plist"
          DESTINATION "${PLUGIN_INSTALLDIR}/${TARGET}.ofx.bundle/Contents")
endfunction()
