# SPDX-License-Identifier: BSD-3-Clause
#
# Building OFX plug-in bundles with CMake: add_ofx_plugin(), ofx_plugin_resources(),
# ofx_target_openmp(), ofx_deploy_bundle(), ofx_plugin_bundle_group(). The reference for the plug-in repositories
# (openfx-misc, openfx-io, ...): see Documentation/proposals/cmake-plugins.md.
# Ported from AcademySoftwareFoundation/openfx; adapted for the NatronGitHub
# fork: the bundle is also assembled in the build tree (usable directly with
# OFX_PLUGIN_PATH), Info.plist is generated at build time instead of being
# written into PLUGIN_INSTALLDIR at configure time, and the architecture folder
# follows the OFX packaging spec.

if(CMAKE_VERSION VERSION_LESS 3.21)
  message(FATAL_ERROR "openfx's OpenFX.cmake needs CMake 3.21 or newer (found ${CMAKE_VERSION})")
endif()
# The policies of this module and of its functions, whatever the project's cmake_minimum_required:
# a function runs with the policies recorded when it was defined (cmake_policy() documentation),
# and so do the modules it includes, e.g. FindOpenMP in ofx_target_openmp(). The policies up to
# CMake 3.31 are NEW when this CMake knows them (later policies stay OLD: raise the upper bound after testing a newer CMake): CMP0126 (set(CACHE) doesn't remove a normal
# variable of the same name, 3.21), CMP0177 (install(DESTINATION) paths are normalized, 3.31).
cmake_policy(PUSH)
cmake_policy(VERSION 3.21...3.31)

# Where the bundles are installed. A relative path is relative to CMAKE_INSTALL_PREFIX: the
# default for the plug-in repositories is OFX/Plugins (e.g. /usr/OFX/Plugins with the prefix
# /usr); openfx's own examples default to the system's OFX plug-in folder.
if(NOT PLUGIN_INSTALLDIR AND NOT (PROJECT_NAME STREQUAL "openfx" AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR))
  set(PLUGIN_INSTALLDIR "OFX/Plugins")
elseif(NOT PLUGIN_INSTALLDIR)
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
  # The plug-ins must run on macOS 14.0 (VFX Reference Platform CY2027). CMake reads the deployment
  # target at the first project(), so a plug-in project sets it before that: in its CMakeLists.txt,
  # set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version"), or on the command
  # line, -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 (or the MACOSX_DEPLOYMENT_TARGET environment variable).
  # Warn if it is empty (clang then uses the SDK's version) or newer than 14.0 (CMake sets it to the
  # build machine's macOS when that is older than the SDK).
  get_property(_ofx_osx_warned GLOBAL PROPERTY _OFX_OSX_TARGET_WARNED)
  if((NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER 14.0)
     AND NOT _ofx_osx_warned AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_property(GLOBAL PROPERTY _OFX_OSX_TARGET_WARNED ON)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      set(_ofx_osx_now "is ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
      set(_ofx_osx_now "is not set (the SDK's version is used)")
    endif()
    message(WARNING "CMAKE_OSX_DEPLOYMENT_TARGET ${_ofx_osx_now}: the plug-ins won't load on macOS 14.0 "
      "(VFX Reference Platform CY2027). Set it to 14.0 before project(), or pass "
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0.")
  endif()
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
      # CMAKE_CXX_COMPILER_ARG1 is one string (e.g. " --target=aarch64-w64-mingw32"): split it
      # (UNIX_COMMAND works with any CMake version, for projects that copy this block)
      separate_arguments(_OFX_CXX_ARGS UNIX_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
      execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${_OFX_CXX_ARGS} -dumpmachine
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
    message(WARNING "Native Arm64 plug-in: installed to Win-arm64, a reserved, not yet normative folder. "
                    "Current Windows-on-Arm hosts (Resolve, Vegas) are Arm64EC: build for Arm64EC for them.")
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
  elseif(_OFX_OS STREQUAL "Linux")
    # the spec's name, literally: Linux-${uname -m} (CMAKE_SYSTEM_PROCESSOR is uname -m for native builds;
    # cross toolchain files set it). HostSupport also searches the compiled architecture's name.
    set(ARCHDIR "Linux-${CMAKE_SYSTEM_PROCESSOR}")
  elseif(_OFX_CPU MATCHES "^armv[2-6]" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCHDIR "FreeBSD-armv6")
  elseif(_OFX_CPU MATCHES "^(aarch64|arm64|armv8l|arm.*)$")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(ARCHDIR "FreeBSD-aarch64")
    else()
      set(ARCHDIR "FreeBSD-armv7")
    endif()
  elseif(_OFX_OS STREQUAL "FreeBSD" AND _OFX_CPU MATCHES "^riscv")
    set(ARCHDIR "FreeBSD-riscv64")
  elseif(_OFX_OS STREQUAL "FreeBSD" AND _OFX_CPU MATCHES "^powerpc")
    # uname -p: powerpc64, powerpc64le (uname -m only says powerpc)
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

# Shared cache variables (Documentation/proposals/cmake-plugins.md)
# OFX_CXX_STANDARD defaults to CMAKE_CXX_STANDARD if it is set, else 17
if(CMAKE_CXX_STANDARD)
  set(_ofx_std ${CMAKE_CXX_STANDARD})
else()
  set(_ofx_std 17)
endif()
set(OFX_CXX_STANDARD ${_ofx_std} CACHE STRING "C++ standard of the plug-ins built with add_ofx_plugin, and of openfx::Support")
option(OFX_USE_OPENMP "ofx_target_openmp(): use OpenMP" ON)
option(OFX_DEPLOY_BUNDLES "ofx_deploy_bundle(): make the installed bundles self-contained" OFF)
option(OFX_DEPLOY_STRICT_MINOS "ofx_deploy_bundle(): fail, instead of warning, if a bundled library requires a newer macOS than CMAKE_OSX_DEPLOYMENT_TARGET" OFF)
# Optimization flags of the release configurations (Release, RelWithDebInfo, MinSizeRel) of the
# plug-ins and of openfx::Support, after CMake's own (so they win with gcc and clang), e.g. -O2.
# Empty: CMake's defaults (-O3 for Release). An installed openfx::Support keeps the flags it was
# built with.
set(OFX_OPTFLAG "" CACHE STRING "Extra optimization flags of the plug-ins' release configurations (empty: CMake's defaults)")
# Extra install rpath directories of every plug-in (e.g. dependency prefixes inside the source
# tree, which INSTALL_RPATH_USE_LINK_PATH skips), after the bundle's own Libraries directory
set(OFX_PLUGIN_RPATH "" CACHE STRING "Extra install rpath directories of the plug-ins (;-separated)")
# Linux and FreeBSD: link the plug-ins in the build tree with their install rpath
# (BUILD_WITH_INSTALL_RPATH). Otherwise CMake gives the build-tree binary its build rpath, padded
# with ':' to the length of the install rpath so that `cmake --install` can rewrite it in place:
# the empty entries mean the current directory to the loader. ON, the build-tree and installed
# binaries have the same RUNPATH, and nothing is rewritten at install. While it is ON, BUILD_RPATH,
# CMAKE_BUILD_RPATH and CMAKE_SKIP_BUILD_RPATH don't affect the plug-ins. INSTALL_RPATH_USE_LINK_PATH
# skips the link directories inside the source and binary trees: a library in an in-tree prefix
# (e.g. a dependency built in the checkout) needs add_ofx_plugin(RPATH) or OFX_PLUGIN_RPATH, as for
# the installed bundle. Turn it OFF for a project whose plug-ins, run from the build tree, need a
# shared library that the project builds, or one in such a prefix that is not passed that way
option(OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH "Linux/FreeBSD: build the plug-ins with their install rpath (no padded build rpath)" ON)
# Linux and FreeBSD: link the plug-ins with -z defs (no undefined symbols). OFF for sanitizer builds
# with clang, which links its sanitizer runtime into executables only (or use -shared-libsan); gcc links
# its shared sanitizer runtime by default, so it works with -z defs
option(OFX_PLUGIN_NO_UNDEFINED "Linux/FreeBSD: link the plug-ins with -z defs (no undefined symbols)" ON)
# ofx_plugin_bundle_group(): COMBINED (one bundle per group, the default) or SEPARATE (one bundle
# per plug-in, the CMake equivalent of the Makefiles' `make nomulti`). The variable
# OFX_PLUGIN_BUNDLES_<group> overrides it for one group.
set(OFX_PLUGIN_BUNDLES "COMBINED" CACHE STRING "ofx_plugin_bundle_group(): COMBINED (one bundle per group) or SEPARATE (one bundle per plug-in)")
set_property(CACHE OFX_PLUGIN_BUNDLES PROPERTY STRINGS COMBINED SEPARATE)

# Add OFX_OPTFLAG to the compile options of a target's release configurations (add_ofx_plugin
# and openfx::Support do it; also for a repository's own static libraries linked into plug-ins):
#
#   ofx_target_optflag(<target>)
function(ofx_target_optflag TARGET)
  if(OFX_OPTFLAG AND NOT MSVC)
    separate_arguments(_optflags UNIX_COMMAND "${OFX_OPTFLAG}")
    target_compile_options(${TARGET} PRIVATE
      "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:${_optflags}>")
  endif()
endfunction()

# Where bundles are assembled in the build tree
if(NOT OFX_PLUGIN_BUILD_DIR)
  set(OFX_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/Plugins" CACHE PATH "Where the OFX bundles are assembled in the build tree")
endif()
# Cache variables, so that the functions below work in any directory of a project that uses openfx
# with add_subdirectory() (these are set in openfx's directory scope)
set(ARCHDIR "${ARCHDIR}" CACHE INTERNAL "OFX bundle architecture folder (OFX packaging spec)")
set(PLUGIN_INSTALLDIR "${PLUGIN_INSTALLDIR}" CACHE STRING "Where the OFX bundles are installed (relative: to CMAKE_INSTALL_PREFIX)" FORCE)

# Location of the Info.plist template, osxDeploy.sh and linuxCheckBundle.sh. In the OpenFX source
# tree they are in Examples/ and Support/include/; in an installed package they sit next to this
# module.
set(_OFX_CMAKE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

function(_ofx_find_file VAR NAME)
  foreach(_dir "${_OFX_CMAKE_MODULE_DIR}"
               "${_OFX_CMAKE_MODULE_DIR}/../Examples"
               "${_OFX_CMAKE_MODULE_DIR}/../Support/include")
    if(EXISTS "${_dir}/${NAME}")
      set(${VAR} "${_dir}/${NAME}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "OpenFX.cmake: could not find ${NAME}")
endfunction()

# OFX_LINUX_CHECK_BUNDLE: the path of linuxCheckBundle.sh (Support/include/ in the source tree,
# next to this module in an installed package). It checks installed Linux bundles against the
# baseline system (EL9 by default), in a plug-in repository's CI or in an installer, e.g.
#   "${OFX_LINUX_CHECK_BUNDLE}" [--no-needed] <prefix>/OFX/Plugins
# The script's header describes the rules, the limits and their OFX_ABI_* overrides.
_ofx_find_file(_ofx_check_bundle linuxCheckBundle.sh)
get_filename_component(_ofx_check_bundle "${_ofx_check_bundle}" ABSOLUTE)
set(OFX_LINUX_CHECK_BUNDLE "${_ofx_check_bundle}" CACHE FILEPATH
    "linuxCheckBundle.sh, which checks installed Linux OFX bundles against the baseline system" FORCE)
unset(_ofx_check_bundle)

# Add an OFX plug-in bundle.
#
#   add_ofx_plugin(<target> [<dir>]
#                  [DIR <dir>] [BUNDLE_NAME <name>] [INFO_PLIST <file>]
#                  [IDENTIFIER <id>] [VERSION <x.y.z>]
#                  [EXPORT_SYMBOLS <symbol>...] [RPATH <dir>...] [SOURCES <file>...])
#
# Builds <BUNDLE_NAME>.ofx (default: <target>) into
# ${OFX_PLUGIN_BUILD_DIR}/<BUNDLE_NAME>.ofx.bundle/Contents/<ARCHDIR>/, with the Info.plist in
# Contents/, and installs the bundle into PLUGIN_INSTALLDIR.
# Each plug-in's install rules (binary, Info.plist, resources, deployment) are in an install
# component named after the target: `cmake --install <build> --component <target>` installs that
# bundle only (a plain `cmake --install` installs them all). A project's own install() of a file
# into a bundle (e.g. a DLL next to the plug-in) must pass `COMPONENT <target>` too.
# - INFO_PLIST: the plug-in's own Info.plist, run through configure_file(@ONLY) (a file without
#   @VAR@ is copied unchanged). Default: the generic template, with IDENTIFIER (default
#   org.openeffects.plugins.<BUNDLE_NAME>) and VERSION (default 1.0.0).
# - Only OfxGetPlugin, OfxGetNumberOfPlugins and the EXPORT_SYMBOLS (e.g. OfxSetHost) are
#   exported: hidden visibility, plus an export list on macOS and a version script on Linux and
#   FreeBSD, as Support/Plugins/Makefile.master does (hidden visibility alone isn't enough for
#   plug-ins that link static libraries, e.g. OSMesa or LLVM).
# - Install rpath: @loader_path/../Frameworks and @loader_path/../Libraries on macOS,
#   $ORIGIN/../../Libraries on Linux and FreeBSD (libraries bundled by the installers), then the
#   RPATH and OFX_PLUGIN_RPATH directories, then the directories of the linked libraries outside
#   the project (INSTALL_RPATH_USE_LINK_PATH), except, on macOS, those in /opt/local, /usr/local
#   and /opt/homebrew (their libraries have absolute install names; Makefile.master's bundles
#   only have the @loader_path rpaths), as Makefile.master's -Wl,-rpath: installers
#   (osxDeploy.sh, Natron's Linux installer) find the libraries there, then rewrite the rpath.
#   On Linux, linuxCheckBundle.sh (OFX_LINUX_CHECK_BUNDLE) reports every non-$ORIGIN entry left in
#   an installed bundle: rewrite them at install, or list them in OFX_ABI_RPATH_ALLOW for a CI check.
#   Unlike Makefile.master's list, it has no system directory (/usr/lib64, ...): CMake leaves
#   out its implicit link directories, which the loader searches anyway. (CMake puts the
#   directories of the linked libraries before the others in the installed binary.)
#   On Linux and FreeBSD, the build-tree binary has the install rpath too
#   (OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH, default ON), not CMake's build rpath padded with empty
#   entries. Still, ship and check the installed bundle, never the build tree.
# - The installed binary is mode 755 (Debian's CMake strips the execute bit of modules).
# - OFX_OPTFLAG is added to the compile options of the release configurations (not with MSVC).
# - RPATH and OFX_PLUGIN_RPATH are ignored on Windows.
# - DEBUG is defined in Debug builds; CXX_STANDARD is ${OFX_CXX_STANDARD}, without GNU
#   extensions; /bigobj (MSVC) or -Wa,-mbig-obj (MinGW gcc) for large translation units (not in
#   Makefile.master). The MinGW runtime DLLs are bundled by the installers, as with the Makefiles.
# - macOS: the installed binary is signed ad hoc again, since install_name_tool (which adds the
#   install rpath) invalidates the signature that Xcode builds have.
# Link the plug-in with openfx::Support (C++ Support library) or openfx::Api (C API only).
function(add_ofx_plugin TARGET)
  cmake_parse_arguments(PARSE_ARGV 1 OFX "" "DIR;BUNDLE_NAME;INFO_PLIST;IDENTIFIER;VERSION"
                        "EXPORT_SYMBOLS;RPATH;SOURCES")
  if(OFX_UNPARSED_ARGUMENTS) # the old form: add_ofx_plugin(TARGET DIR)
    list(GET OFX_UNPARSED_ARGUMENTS 0 OFX_DIR)
  endif()
  if(NOT OFX_BUNDLE_NAME)
    set(OFX_BUNDLE_NAME ${TARGET})
  endif()
  if(NOT OFX_IDENTIFIER)
    set(OFX_IDENTIFIER org.openeffects.plugins.${OFX_BUNDLE_NAME})
  endif()
  if(NOT OFX_VERSION)
    set(OFX_VERSION "1.0.0")
  endif()

  # a loadable module everywhere: an OSX bundle, a DLL without import library, a .so without soname
  add_library(${TARGET} MODULE)
  set_target_properties(${TARGET} PROPERTIES SUFFIX ".ofx" PREFIX "" OUTPUT_NAME "${OFX_BUNDLE_NAME}")
  if(OFX_SOURCES)
    target_sources(${TARGET} PRIVATE ${OFX_SOURCES})
  endif()
  # the C API headers and the usage requirements (definitions); openfx::Support for C++ plug-ins
  # (the OFX_EXTENSIONS_*/OFX_SUPPORTS_* definitions change class layouts: never add one to a
  # plug-in target or file, set openfx's options instead, see README.md and #226)
  # (through LINK_LIBRARIES, so that the consumer may use either target_link_libraries form)
  if(TARGET openfx::Api)
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_LIBRARIES openfx::Api)
  endif()
  if(APPLE) # plug-ins may use Apple's deprecated OpenGL headers
    target_compile_definitions(${TARGET} PRIVATE GL_SILENCE_DEPRECATION)
  endif()

  # Info.plist, assembled into the bundle in the build tree:
  # <BUNDLE_NAME>.ofx.bundle/Contents/{Info.plist,<ARCHDIR>/<BUNDLE_NAME>.ofx}
  set(_bundle "${OFX_PLUGIN_BUILD_DIR}/${OFX_BUNDLE_NAME}.ofx.bundle")
  set(PLUGIN_NAME ${OFX_BUNDLE_NAME})
  set(PLUGIN_EXE ${OFX_BUNDLE_NAME}.ofx)
  set(BUNDLE_IDENTIFIER ${OFX_IDENTIFIER})
  set(BUNDLE_SIGNATURE "????")  # any value is OK here, ignored by modern MacOS
  set(PLUGIN_VERSION ${OFX_VERSION})
  # configured into the binary directory, copied into the bundle at each build (so that the
  # bundle can be rebuilt from scratch), and installed from the binary directory
  set(_info "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.Info.plist")
  if(OFX_INFO_PLIST)
    get_filename_component(_plist "${OFX_INFO_PLIST}" ABSOLUTE)
    configure_file("${_plist}" "${_info}" @ONLY)
  else()
    _ofx_find_file(_plist Info.plist.in)
    configure_file("${_plist}" "${_info}")
  endif()
  # the output folder is created before linking too (universal macOS links fail if it was deleted)
  add_custom_command(TARGET ${TARGET} PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle}/Contents/${ARCHDIR}"
    VERBATIM)
  add_custom_target(${TARGET}_info_plist ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle}/Contents"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_info}" "${_bundle}/Contents/Info.plist"
    VERBATIM)
  add_dependencies(${TARGET} ${TARGET}_info_plist)
  set_target_properties(${TARGET} PROPERTIES
    OFX_BUNDLE_NAME "${OFX_BUNDLE_NAME}"
    OFX_BUNDLE_DIR "${_bundle}"
    LIBRARY_OUTPUT_DIRECTORY "${_bundle}/Contents/${ARCHDIR}"
    RUNTIME_OUTPUT_DIRECTORY "${_bundle}/Contents/${ARCHDIR}")
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_cfg}" _CFG)
    set_target_properties(${TARGET} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_bundle}/Contents/${ARCHDIR}"
      RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_bundle}/Contents/${ARCHDIR}")
  endforeach()

  # Exported symbols: hidden visibility; the OFX entry points are declared with OfxExport
  # (ofxCore.h), which marks them visible. The export list also hides the symbols of linked
  # static libraries.
  set_target_properties(${TARGET} PROPERTIES C_VISIBILITY_PRESET hidden
                                             CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON)
  set(_symbols OfxGetPlugin OfxGetNumberOfPlugins ${OFX_EXPORT_SYMBOLS})
  set(_exports "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.exports")
  if(APPLE)
    list(TRANSFORM _symbols PREPEND "_")
    string(JOIN "\n" _content ${_symbols})
    file(WRITE "${_exports}.new" "${_content}\n")
    configure_file("${_exports}.new" "${_exports}" COPYONLY)
    target_link_options(${TARGET} PRIVATE "LINKER:-exported_symbols_list,${_exports}")
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${_exports}")
  elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    string(JOIN ";\n    " _content ${_symbols})
    file(WRITE "${_exports}.new" "OFX_1.0 {\n  global:\n    ${_content};\n  local:\n    *;\n};\n")
    configure_file("${_exports}.new" "${_exports}" COPYONLY)
    target_link_options(${TARGET} PRIVATE "LINKER:--version-script=${_exports}")
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${_exports}")
    # no undefined symbols: a plug-in takes no symbol from the host, so an unresolved one (e.g. a
    # missing library, or a symbol that the build machine's runtime lacks) fails the link instead
    # of the plug-in's load. Turn it off for clang sanitizer builds (clang links the sanitizer
    # runtime into executables only), or use -shared-libsan.
    if(OFX_PLUGIN_NO_UNDEFINED)
      target_link_options(${TARGET} PRIVATE "LINKER:-z,defs")
    endif()
  endif()

  # Install rpath: the libraries that the installers put in the bundle and the explicit extra
  # directories, plus (CMake, first) the directories of the linked libraries outside the project
  if(APPLE)
    set(_rpath "@loader_path/../Frameworks;@loader_path/../Libraries")
  elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    set(_rpath "$ORIGIN/../../Libraries")
  else()
    set(_rpath "")
  endif()
  if(NOT WIN32)
    list(APPEND _rpath ${OFX_RPATH} ${OFX_PLUGIN_RPATH})
    # (macOS: the rpaths into /opt/local, /usr/local and /opt/homebrew are deleted at install, see
    # below; the others stay, e.g. for libraries with @rpath install names in an SDK prefix)
    set_target_properties(${TARGET} PROPERTIES INSTALL_RPATH "${_rpath}" INSTALL_RPATH_USE_LINK_PATH ON)
  endif()
  # Linux and FreeBSD: the build-tree binary gets the install rpath too, not CMake's build rpath
  # padded with ':' (empty entries: the current directory), see OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH
  if(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD" AND OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH)
    set_target_properties(${TARGET} PROPERTIES BUILD_WITH_INSTALL_RPATH ON)
  endif()
  ofx_target_optflag(${TARGET})

  target_compile_definitions(${TARGET} PRIVATE $<$<CONFIG:Debug>:DEBUG>)
  set_target_properties(${TARGET} PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD}
                                             CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${TARGET} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
  elseif(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${TARGET} PRIVATE -Wa,-mbig-obj) # (clang's assembler has no -mbig-obj)
  endif()

  # To install plugins: cmake --install Build
  set(_dest "${PLUGIN_INSTALLDIR}/${OFX_BUNDLE_NAME}.ofx.bundle/Contents")
  # (on Windows, the import library of the DLL is not installed)
  set(_perms PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)
  if(APPLE)
    if(IS_ABSOLUTE "${_dest}")
      set(_bin "${_dest}/${ARCHDIR}/${OFX_BUNDLE_NAME}.ofx")
    else()
      set(_bin "\${CMAKE_INSTALL_PREFIX}/${_dest}/${ARCHDIR}/${OFX_BUNDLE_NAME}.ofx")
    endif()
    string(REPLACE "/./" "/" _bin "${_bin}") # (PLUGIN_INSTALLDIR=.: for the messages)
    # macOS: a new copy at each install. The installed binary is signed again after install (see
    # _ofx_install_sign); if that happens within the second of the link, CMake later finds it
    # "Up-to-date", doesn't copy it, but still adds the install rpaths again (install_name_tool:
    # "would duplicate path")
    install(CODE "file(REMOVE \"\$ENV{DESTDIR}${_bin}\")" COMPONENT ${TARGET})
  endif()
  install(
    TARGETS ${TARGET}
    LIBRARY DESTINATION "${_dest}/${ARCHDIR}" ${_perms} COMPONENT ${TARGET}
    RUNTIME DESTINATION "${_dest}/${ARCHDIR}" ${_perms} COMPONENT ${TARGET}
    ARCHIVE DESTINATION "${_dest}/${ARCHDIR}" COMPONENT ${TARGET} EXCLUDE_FROM_ALL)
  install(FILES "${_info}" DESTINATION "${_dest}" RENAME Info.plist COMPONENT ${TARGET})
  if(APPLE)
    set_target_properties(${TARGET} PROPERTIES OFX_INSTALLED_BINARY "${_bin}")
    # the LC_RPATHs into MacPorts, /usr/local and Homebrew are deleted, as in Makefile.master (its
    # bundles only have the @loader_path ones): their libraries have absolute install names.
    # Kept: the RPATH and OFX_PLUGIN_RPATH entries, and a directory where an @rpath dependency of
    # the plug-in is (e.g. a library installed into /usr/local with CMake's defaults)
    set(_keep ${OFX_RPATH} ${OFX_PLUGIN_RPATH})
    install(CODE "
      set(_keep \"${_keep}\")
      execute_process(COMMAND otool -l \"\$ENV{DESTDIR}${_bin}\" OUTPUT_VARIABLE _cmds ERROR_QUIET)
      execute_process(COMMAND otool -L \"\$ENV{DESTDIR}${_bin}\" OUTPUT_VARIABLE _deps ERROR_QUIET)
      string(REGEX MATCHALL \"@rpath/[^ \\t\\n]+\" _rdeps \"\${_deps}\")
      string(REGEX MATCHALL \" path (/opt/local|/usr/local|/opt/homebrew)(/[^\\n]*)? \\\\(offset\" _bad \"\${_cmds}\")
      foreach(_p \${_bad})
        string(REGEX REPLACE \"^ path (.*) \\\\(offset$\" \"\\\\1\" _p \"\${_p}\")
        list(FIND _keep \"\${_p}\" _i) # (no IN_LIST: the install script has CMake's old policies)
        if(NOT _i EQUAL -1)
          continue()
        endif()
        set(_used FALSE)
        foreach(_d \${_rdeps})
          string(REPLACE \"@rpath/\" \"\${_p}/\" _f \"\${_d}\")
          if(EXISTS \"\${_f}\")
            set(_used TRUE)
          endif()
        endforeach()
        if(NOT _used)
          execute_process(COMMAND install_name_tool -delete_rpath \"\${_p}\" \"\$ENV{DESTDIR}${_bin}\" OUTPUT_QUIET ERROR_QUIET)
        endif()
      endforeach()" COMPONENT ${TARGET})
    _ofx_install_sign(${TARGET})
  endif()
endfunction()

# macOS: sign the installed plug-in ad hoc (install_name_tool invalidated its signature). This
# seals the bundle with what is installed so far, so ofx_plugin_resources() calls it again after
# its files.
function(_ofx_install_sign TARGET)
  get_target_property(_bin ${TARGET} OFX_INSTALLED_BINARY)
  if(APPLE AND _bin)
    string(REPLACE "/./" "/" _bin "${_bin}") # (PLUGIN_INSTALLDIR=.: for the messages)
    install(CODE "
      execute_process(COMMAND codesign -f -s - \"\$ENV{DESTDIR}${_bin}\" RESULT_VARIABLE _r OUTPUT_QUIET ERROR_VARIABLE _err)
      if(_r)
        message(FATAL_ERROR \"codesign failed on \$ENV{DESTDIR}${_bin}: \${_err}\")
      endif()" COMPONENT ${TARGET})
  endif()
endfunction()

# Add files to the Resources of a plug-in bundle (icons, presets...):
#
#   ofx_plugin_resources(<target> [DESTINATION <subdir>] <file>...)
#
# The files are copied to Contents/Resources[/<subdir>] in the build tree (after each build)
# and installed. A directory is copied with its contents, as `cp -r` in Makefile.master. Relative paths are relative to the current source directory. Call it as often
# as needed, inside if() for conditional resources; for a configured file, run configure_file()
# first and pass its output.
function(ofx_plugin_resources TARGET)
  cmake_parse_arguments(PARSE_ARGV 1 OFX "" "DESTINATION" "")
  get_target_property(_bundle ${TARGET} OFX_BUNDLE_DIR)
  get_target_property(_name ${TARGET} OFX_BUNDLE_NAME)
  if(NOT _bundle)
    message(FATAL_ERROR "ofx_plugin_resources: ${TARGET} was not created by add_ofx_plugin")
  endif()
  get_target_property(_deployed ${TARGET} OFX_DEPLOYED)
  if(_deployed)
    message(FATAL_ERROR "ofx_plugin_resources(${TARGET}): call it before ofx_deploy_bundle(${TARGET}), which signs the installed bundle")
  endif()
  # the copies are made by a custom target that runs at each build, so that edited resources are
  # updated in the build tree too (copy_if_different)
  get_target_property(_n ${TARGET} OFX_RESOURCE_TARGETS)
  if(NOT _n)
    set(_n 0)
  endif()
  math(EXPR _n "${_n} + 1")
  set_target_properties(${TARGET} PROPERTIES OFX_RESOURCE_TARGETS ${_n})
  set(_rt ${TARGET}_resources_${_n})
  set(_sub "Resources")
  if(OFX_DESTINATION)
    set(_sub "Resources/${OFX_DESTINATION}")
  endif()
  set(_files "")
  set(_dirs "")
  foreach(_f IN LISTS OFX_UNPARSED_ARGUMENTS)
    get_filename_component(_abs "${_f}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(IS_DIRECTORY "${_abs}")
      list(APPEND _dirs "${_abs}")
    else()
      list(APPEND _files "${_abs}")
    endif()
  endforeach()
  set(_dest "${PLUGIN_INSTALLDIR}/${_name}.ofx.bundle/Contents/${_sub}")
  set(_cmds COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle}/Contents/${_sub}")
  if(_files)
    list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_files} "${_bundle}/Contents/${_sub}")
    install(FILES ${_files} DESTINATION "${_dest}" COMPONENT ${TARGET})
  endif()
  foreach(_d IN LISTS _dirs)
    get_filename_component(_dname "${_d}" NAME)
    list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_d}" "${_bundle}/Contents/${_sub}/${_dname}")
    install(DIRECTORY "${_d}" DESTINATION "${_dest}" COMPONENT ${TARGET})
  endforeach()
  _ofx_install_sign(${TARGET})
  add_custom_target(${_rt} ALL ${_cmds} VERBATIM)
  add_dependencies(${TARGET} ${_rt})
endfunction()

# Apple clang: the directory of libomp (OFX_LIBOMP_LIBRARY) on the link path of the target, so that
# its other dependencies built with OpenMP find it too, e.g. a static ImageMagick whose .pc has
# -lomp (MacPorts' lib/libomp isn't searched by default)
function(_ofx_libomp_link_directory TARGET)
  if(NOT (APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang") OR NOT TARGET OpenMP::OpenMP_CXX)
    return()
  endif()
  # the libomp that OpenMP::OpenMP_CXX links (not necessarily OFX_LIBOMP_LIBRARY: OpenMP may have
  # been configured through its own cache variables)
  get_target_property(_libs OpenMP::OpenMP_CXX INTERFACE_LINK_LIBRARIES)
  foreach(_l IN LISTS _libs)
    if(_l MATCHES "/libomp[^/]*\\.(dylib|a)$" AND EXISTS "${_l}")
      get_filename_component(_dir "${_l}" DIRECTORY)
      # (PUBLIC for a static library: the plug-in that links it gets the directory too)
      get_target_property(_type ${TARGET} TYPE)
      if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
        target_link_directories(${TARGET} PUBLIC "${_dir}")
      else()
        target_link_directories(${TARGET} PRIVATE "${_dir}")
      endif()
      break()
    endif()
  endforeach()
endfunction()

# Compile and link a target with OpenMP, if OFX_USE_OPENMP is ON:
#
#   ofx_target_openmp(<target> [REQUIRED])
#
# With Apple clang, which has no OpenMP runtime, it uses libomp from MacPorts
# (/opt/local/include/libomp, /opt/local/lib/libomp), or from OFX_LIBOMP_ROOT.
# With Apple clang, it sets OpenMP_CXX_FLAGS, OpenMP_CXX_LIB_NAMES and OpenMP_omp_LIBRARY as
# normal variables before find_package(OpenMP), so they hide the cache entries of the same
# names: -DOpenMP_CXX_FLAGS=... is ignored there. Use OFX_LIBOMP_ROOT to choose another libomp.
# The directory of libomp is also added to the target's link directories (Apple clang), for its
# dependencies that link -lomp.
# MSYS2: OpenMP needs the mingw-w64-ucrt-x86_64-libgomp package, and the plug-in then depends on
# libgomp-1.dll (also with -static-libgcc -static-libstdc++).
function(ofx_target_openmp TARGET)
  cmake_parse_arguments(PARSE_ARGV 1 OFX "REQUIRED" "" "")
  if(NOT OFX_USE_OPENMP)
    return()
  endif()
  if(TARGET OpenMP::OpenMP_CXX) # found by an earlier call
    target_link_libraries(${TARGET} PRIVATE OpenMP::OpenMP_CXX)
    _ofx_libomp_link_directory(${TARGET})
    return()
  endif()
  if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(NOT "${OFX_LIBOMP_ROOT}" STREQUAL "${_OFX_LIBOMP_ROOT_SEARCHED}")
      # OFX_LIBOMP_ROOT changed: search again
      unset(OFX_LIBOMP_INCLUDE_DIR CACHE)
      unset(OFX_LIBOMP_LIBRARY CACHE)
      set(_OFX_LIBOMP_ROOT_SEARCHED "${OFX_LIBOMP_ROOT}" CACHE INTERNAL "")
    endif()
    find_path(OFX_LIBOMP_INCLUDE_DIR omp.h
      HINTS ${OFX_LIBOMP_ROOT} ${OFX_LIBOMP_ROOT}/include ${OFX_LIBOMP_ROOT}/include/libomp
            /opt/local/include/libomp
      NO_DEFAULT_PATH)
    find_library(OFX_LIBOMP_LIBRARY omp
      HINTS ${OFX_LIBOMP_ROOT} ${OFX_LIBOMP_ROOT}/lib ${OFX_LIBOMP_ROOT}/lib/libomp
            /opt/local/lib/libomp
      NO_DEFAULT_PATH)
    if(OFX_LIBOMP_INCLUDE_DIR AND OFX_LIBOMP_LIBRARY)
      set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${OFX_LIBOMP_INCLUDE_DIR}")
      set(OpenMP_CXX_LIB_NAMES omp)
      set(OpenMP_omp_LIBRARY "${OFX_LIBOMP_LIBRARY}")
    endif()
  endif()
  find_package(OpenMP COMPONENTS CXX)
  if(TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(${TARGET} PRIVATE OpenMP::OpenMP_CXX)
    _ofx_libomp_link_directory(${TARGET})
  elseif(OFX_REQUIRED)
    message(FATAL_ERROR "ofx_target_openmp(${TARGET}): OpenMP not found (MacPorts: port install libomp; MSYS2: pacboy libgomp:p)")
  else()
    get_property(_warned GLOBAL PROPERTY _OFX_OPENMP_WARNED)
    if(NOT _warned)
      set_property(GLOBAL PROPERTY _OFX_OPENMP_WARNED TRUE)
      message(WARNING "ofx_target_openmp: OpenMP not found, building ${TARGET} (and the next targets) without it (MacPorts: port install libomp; MSYS2: pacboy libgomp:p; or set OFX_LIBOMP_ROOT, or OFX_USE_OPENMP=OFF)")
    endif()
  endif()
endfunction()

# Drop the include directories that don't exist from an imported target, and from the imported
# targets it links, recursively:
#
#   ofx_target_filter_missing_includes(<target>...)
#
# Generation fails if an imported target lists a missing include directory; e.g. Ubuntu 24.04's
# OpenImageIO config lists /usr/include/opencv4 even without libopencv-dev. Call it after the
# find_package(), before the targets are used. Entries with generator expressions are kept.
# Non-imported targets are not changed (their link libraries are still visited). Link entries are
# followed when they are target names or $<LINK_ONLY:...>, not inside other generator expressions.
function(ofx_target_filter_missing_includes)
  # targets visited by this call (non-global imported targets are directory-scoped: another
  # directory may have a different target of the same name)
  set_property(GLOBAL PROPERTY _OFX_FILTERED_TARGETS "")
  foreach(_t IN LISTS ARGN)
    _ofx_filter_missing_includes("${_t}")
  endforeach()
endfunction()

function(_ofx_filter_missing_includes _t)
  string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _t "${_t}")
  if(NOT TARGET "${_t}")
    return()
  endif()
  get_target_property(_aliased "${_t}" ALIASED_TARGET)
  if(_aliased)
    set(_t "${_aliased}")
  endif()
  get_property(_seen GLOBAL PROPERTY _OFX_FILTERED_TARGETS)
  if("${_t}" IN_LIST _seen)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY _OFX_FILTERED_TARGETS "${_t}")
  get_target_property(_imported "${_t}" IMPORTED)
  if(_imported)
    foreach(_prop INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
      get_target_property(_dirs "${_t}" ${_prop})
      if(_dirs)
        set(_keep "")
        foreach(_d IN LISTS _dirs)
          if(_d MATCHES "\\$<" OR EXISTS "${_d}")
            list(APPEND _keep "${_d}")
          else()
            # (once per directory: several targets may list it)
            get_property(_reported GLOBAL PROPERTY _OFX_REPORTED_MISSING_INCLUDES)
            if(NOT _d IN_LIST _reported)
              set_property(GLOBAL APPEND PROPERTY _OFX_REPORTED_MISSING_INCLUDES "${_d}")
              message(STATUS "${_t}: ignoring the missing include directory ${_d}")
            endif()
          endif()
        endforeach()
        set_target_properties("${_t}" PROPERTIES ${_prop} "${_keep}")
      endif()
    endforeach()
  endif()
  get_target_property(_libs "${_t}" INTERFACE_LINK_LIBRARIES)
  if(_libs)
    foreach(_l IN LISTS _libs)
      _ofx_filter_missing_includes("${_l}")
    endforeach()
  endif()
endfunction()

# Make the installed bundle of a plug-in self-contained, if OFX_DEPLOY_BUNDLES is ON:
#
#   ofx_deploy_bundle(<target>)
#
# macOS, at install time of a release configuration (Release, RelWithDebInfo, MinSizeRel), as
# in Makefile.master: Support/include/osxDeploy.sh copies the non-system libraries into
# Contents/Libraries (with @rpath install names), then the libraries and the bundle are signed
# ad hoc (required on arm64), and the install fails if a Mach-O file still references
# /opt/local, /usr/local or /opt/homebrew.
# If CMAKE_OSX_DEPLOYMENT_TARGET is set, a bundled file with an architecture that requires a newer
# macOS (LC_BUILD_VERSION minos, or LC_VERSION_MIN_MACOSX; at least 11.0 for arm64) gives a
# warning, or an error with OFX_DEPLOY_STRICT_MINOS.
# Call it after the last ofx_plugin_resources() of the target: files installed into the bundle
# after it would break the bundle's signature. macOS only, by design: on Linux and Windows, plug-ins
# use system libraries (or header-only ones), and installers (e.g. Natron's) deploy anything else.
function(ofx_deploy_bundle TARGET)
  get_target_property(_name ${TARGET} OFX_BUNDLE_NAME)
  if(NOT _name)
    message(FATAL_ERROR "ofx_deploy_bundle: ${TARGET} was not created by add_ofx_plugin")
  endif()
  # (also without OFX_DEPLOY_BUNDLES, so that a wrong order of calls shows on every build)
  set_target_properties(${TARGET} PROPERTIES OFX_DEPLOYED TRUE)
  if(NOT OFX_DEPLOY_BUNDLES)
    return()
  endif()
  if(APPLE)
    _ofx_find_file(_deploy osxDeploy.sh)
    if(OFX_DEPLOY_STRICT_MINOS)
      set(_minos_level FATAL_ERROR)
    else()
      set(_minos_level WARNING)
    endif()
    if(IS_ABSOLUTE "${PLUGIN_INSTALLDIR}")
      set(_bundle "${PLUGIN_INSTALLDIR}/${_name}.ofx.bundle")
    else()
      set(_bundle "\${CMAKE_INSTALL_PREFIX}/${PLUGIN_INSTALLDIR}/${_name}.ofx.bundle")
    endif()
    string(REPLACE "/./" "/" _bundle "${_bundle}") # (PLUGIN_INSTALLDIR=.: for the messages)
    # release builds only, as in Makefile.master (or an empty build type with a single-config
    # generator)
    install(CODE "
     string(TOUPPER \"\${CMAKE_INSTALL_CONFIG_NAME}\" _cfg)
     if(_cfg STREQUAL \"\" OR _cfg MATCHES \"^(RELEASE|RELWITHDEBINFO|MINSIZEREL)\$\")
      set(_b \"\$ENV{DESTDIR}${_bundle}\")
      set(_target \"${CMAKE_OSX_DEPLOYMENT_TARGET}\")
      set(_minos_level ${_minos_level})
      set(_minos_msg \"\")
      execute_process(COMMAND bash \"${_deploy}\" \"\${_b}\" \"${_name}.ofx\" RESULT_VARIABLE _r)
      if(_r)
        message(FATAL_ERROR \"osxDeploy.sh failed on \${_b}\")
      endif()
      # every file in Libraries and Frameworks, subfolders included (e.g. ImageMagick modules):
      # signed ad hoc (non-Mach-O files are skipped by codesign), and checked
      file(GLOB_RECURSE _libs LIST_DIRECTORIES false \"\${_b}/Contents/Libraries/*\" \"\${_b}/Contents/Frameworks/*\")
      foreach(_f \${_libs} \"\${_b}/Contents/MacOS/${_name}.ofx\")
        # LC_RPATHs into the build machine's package managers (e.g. MacPorts' libomp has
        # /opt/local/lib/libomp) are deleted: the bundled libraries are found with @rpath
        execute_process(COMMAND otool -l \"\${_f}\" OUTPUT_VARIABLE _cmds ERROR_QUIET)
        string(REGEX MATCHALL \" path (/opt/local|/usr/local|/opt/homebrew)[^ ]*\" _bad \"\${_cmds}\")
        foreach(_p \${_bad})
          string(REGEX REPLACE \"^ path \" \"\" _p \"\${_p}\")
          execute_process(COMMAND install_name_tool -delete_rpath \"\${_p}\" \"\${_f}\" OUTPUT_QUIET ERROR_QUIET)
        endforeach()
        execute_process(COMMAND file -b \"\${_f}\" OUTPUT_VARIABLE _type)
        if(NOT _type MATCHES \"Mach-O\")
          continue()
        endif()
        execute_process(COMMAND codesign -f -s - \"\${_f}\" RESULT_VARIABLE _r OUTPUT_QUIET ERROR_VARIABLE _err)
        if(_r)
          message(FATAL_ERROR \"codesign failed on \${_f}: \${_err}\")
        endif()
        execute_process(COMMAND otool -L \"\${_f}\" OUTPUT_VARIABLE _deps ERROR_QUIET)
        execute_process(COMMAND otool -l \"\${_f}\" OUTPUT_VARIABLE _cmds ERROR_QUIET)
        if(_deps MATCHES \"\\t(/opt/local|/usr/local|/opt/homebrew)/\")
          message(FATAL_ERROR \"\${_f} still references \${CMAKE_MATCH_1} (osxDeploy.sh copies MacPorts libraries, not Homebrew ones):\\n\${_deps}\")
        endif()
        if(_cmds MATCHES \" path (/opt/local|/usr/local|/opt/homebrew)/\")
          message(FATAL_ERROR \"\${_f} has an LC_RPATH in \${CMAKE_MATCH_1}\")
        endif()
        # the minimum macOS of each architecture of the file: MacPorts builds libraries for the
        # build machine's macOS, whatever the plug-in's deployment target. arm64 slices are never
        # below 11.0 (the linker raises a lower target to 11.0).
        if(NOT \"\${_target}\" STREQUAL \"\")
          execute_process(COMMAND lipo -archs \"\${_f}\" OUTPUT_VARIABLE _archs ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
          separate_arguments(_archs UNIX_COMMAND \"\${_archs}\")
          foreach(_a \${_archs})
            execute_process(COMMAND otool -arch \${_a} -l \"\${_f}\" OUTPUT_VARIABLE _acmds ERROR_QUIET)
            set(_minos \"\")
            string(REGEX MATCH \" minos [0-9.]+|LC_VERSION_MIN_MACOSX\\n[^\\n]*\\n *version [0-9.]+\" _v \"\${_acmds}\")
            if(_v)
              string(REGEX REPLACE \".* \" \"\" _minos \"\${_v}\")
            endif()
            set(_atarget \"\${_target}\")
            if(_a MATCHES \"^arm64\" AND _atarget VERSION_LESS 11.0)
              set(_atarget 11.0)
            endif()
            if(NOT _minos STREQUAL \"\" AND _minos VERSION_GREATER _atarget)
              string(APPEND _minos_msg \"\\n  \${_f} (\${_a}) requires macOS \${_minos}\")
            endif()
          endforeach()
        endif()
      endforeach()
      if(_minos_msg)
        message(\${_minos_level} \"\${_b}: files newer than CMAKE_OSX_DEPLOYMENT_TARGET \${_target}, so the plug-in won't load on older macOS versions:\${_minos_msg}\\nBuild the dependencies for \${_target} (MacPorts: macosx_deployment_target in macports.conf)\")
      endif()
      execute_process(COMMAND codesign -f -s - \"\${_b}\" RESULT_VARIABLE _r)
      if(_r)
        message(FATAL_ERROR \"codesign failed on \${_b}\")
      endif()
      execute_process(COMMAND codesign --verify --strict \"\${_b}\" RESULT_VARIABLE _r)
      if(_r)
        message(FATAL_ERROR \"codesign --verify --strict failed on \${_b}\")
      endif()
     endif()
    " COMPONENT ${TARGET})
  endif()
endfunction()

# A group of OFX plug-ins built as one combined bundle, or as one bundle per plug-in (the CMake
# equivalent of the Makefiles' `make nomulti`, with other bundle names: see below):
#
#   ofx_plugin_bundle_group(<group>
#     [COMBINED_NAME <bundle>] [COMBINED_ID <identifier>] [COMBINED_INFO_PLIST <file>]
#     [COMBINED_RESOURCES <item>...] [COMBINED_SOURCES <file>...]
#     [ID_PREFIX <prefix>] [VERSION <x.y.z>] [ALSO_SEPARATE <plugin>...] [NO_DEPLOY]
#     [COMMON_SOURCES <file>...] [COMMON_LINK_LIBRARIES <lib>...]
#     [COMMON_COMPILE_DEFINITIONS <def>...] [COMMON_COMPILE_OPTIONS <opt>...]
#     [COMMON_INCLUDE_DIRECTORIES <dir>...] [COMMON_RPATH <dir>...] [COMMON_EXPORT_SYMBOLS <symbol>...]
#     [OUT_TARGETS <var>] [OUT_COMMON_TARGET <var>]
#     PLUGIN <name> SOURCES <file>... [ID <identifier>] [REGISTER <function>...]
#            [RESOURCES <item>...] [INFO_PLIST <file>]
#            [LINK_LIBRARIES <lib>...] [COMPILE_DEFINITIONS <def>...] [COMPILE_OPTIONS <opt>...]
#            [INCLUDE_DIRECTORIES <dir>...] [OPENMP] [COMBINED_ONLY | SEPARATE_ONLY] [NO_ID_CHECK]
#            [PROPERTIES <property> <value>...]... [OUT_TARGET <var>]
#     [PLUGIN <name> ...]...)
#
# The mode is OFX_PLUGIN_BUNDLES (COMBINED or SEPARATE), or the variable OFX_PLUGIN_BUNDLES_<group>.
# - COMBINED: one add_ofx_plugin(<COMBINED_NAME>) (default <group>; install component
#   <COMBINED_NAME>) with every plug-in except the SEPARATE_ONLY ones, plus COMBINED_SOURCES and
#   COMBINED_RESOURCES. The ALSO_SEPARATE and SEPARATE_ONLY plug-ins get their own bundle too
#   (ALSO_SEPARATE ones are in both, for separate downloads: their identifiers are then in two
#   bundles, and a host must not load both).
# - SEPARATE: one add_ofx_plugin(<name>) per plug-in except the COMBINED_ONLY ones, which still go
#   into a combined bundle (with COMBINED_SOURCES and COMBINED_RESOURCES).
# - A separate bundle is named after the plug-in's identifier, ID or <ID_PREFIX><name> (e.g.
#   net.sf.openfx.Merge.ofx.bundle with the binary net.sf.openfx.Merge.ofx), so that the bundles of
#   different packages can't collide in a shared plug-in folder (`make nomulti` names them
#   <Dir>.ofx.bundle); its target and its install component are the short <name>. An ID or ID_PREFIX
#   is required, except for COMBINED_ONLY plug-ins. A COMBINED_NAME that is also the target of a
#   plug-in's separate bundle, in either mode, is an error.
# - Registration: Support plug-ins register their factories with mRegisterPluginFactoryInstance,
#   so linking their sources is enough. REGISTER names functions `void f(OFX::PluginFactoryArray&)`
#   (namespaces allowed, e.g. MyNS::getMyPluginIDs): a generated source of each bundle calls them,
#   in declaration order, at static initialization, into the Support library's automatic
#   registration (it needs openfx::Support, and doesn't define OFX::Plugin::getPluginIDs). REGISTER
#   and self-registering plug-ins may be mixed in a bundle. Combining and REGISTER are for
#   Support-based plug-ins: raw C-API plug-ins each define OfxGetPlugin.
# - Sources are compiled into each bundle target (never through a static archive, whose unreferenced
#   objects, and so the static registrations, would be dropped). COMMON_SOURCES (helpers without
#   self-registering factories) are compiled once into an OBJECT library, `<group>_common`, which
#   links COMMON_LINK_LIBRARIES PUBLIC and is linked into every bundle of the group.
# - COMMON_LINK_LIBRARIES, _COMPILE_DEFINITIONS, _COMPILE_OPTIONS and _INCLUDE_DIRECTORIES apply to
#   every bundle, COMMON_RPATH and COMMON_EXPORT_SYMBOLS are add_ofx_plugin's RPATH and
#   EXPORT_SYMBOLS of every bundle; a plug-in's LINK_LIBRARIES and INCLUDE_DIRECTORIES apply to the
#   bundles that contain it (a combined bundle gets the union, so a combined build searches every
#   plug-in's include directories: header names must be unique across them, or it silently uses
#   the first plug-in's header); its COMPILE_DEFINITIONS and
#   COMPILE_OPTIONS to its own SOURCES only (source file properties, also seen by other targets of
#   the directory that compile these files; not for generator-expression sources). A source listed
#   by several plug-ins is compiled once per bundle, and these plug-ins must give it the same
#   COMPILE_DEFINITIONS and COMPILE_OPTIONS.
# - RESOURCES items: a file, a directory (copied with its contents, under its own name), or
#   `<file-or-directory> DESTINATION <subdir>` (under Contents/Resources/<subdir>). A combined bundle
#   gets the union of its plug-ins' resources (an item listed twice is copied once) and
#   COMBINED_RESOURCES; two different files with the same destination (compared without case) are
#   an error.
# - Info.plist: a separate bundle's is generated (identifier <ID>, version VERSION, else
#   PROJECT_VERSION, else 1.0.0), unless INFO_PLIST gives one whose CFBundleExecutable and
#   CFBundleIdentifier, after configure_file(@ONLY), are <ID>.ofx and <ID> (use @PLUGIN_EXE@ and
#   @BUNDLE_IDENTIFIER@); another INFO_PLIST gives a warning (in both modes) and is not used. A combined bundle's is
#   COMBINED_INFO_PLIST, whose CFBundleExecutable must be <COMBINED_NAME>.ofx, or generated
#   (identifier COMBINED_ID, default org.openeffects.plugins.<COMBINED_NAME>).
# - OPENMP: ofx_target_openmp() on the bundles that contain the plug-in.
# - Each bundle target gets the properties OFX_BUNDLE_PLUGINS (its plug-ins' names) and
#   OFX_BUNDLE_GROUP, and the union of its plug-ins' PROPERTIES (appended, duplicates removed).
#   PROPERTIES are custom, project-prefixed properties only: a keyword, a property that the target
#   already has, an OFX_* name, or the bookkeeping property of a helper (SUPPORTEXT_COMPONENTS,
#   IOSUPPORT_COMPONENTS: supportext_add_sources would then skip these components) is an error.
# - ofx_deploy_bundle() is called on every bundle, after its resources, unless NO_DEPLOY: the caller
#   then adds its own resources (ofx_plugin_resources) and calls ofx_deploy_bundle() itself.
# - OUT_TARGETS: all the bundle targets; OUT_TARGET: the targets that contain the plug-in (a list:
#   two with ALSO_SEPARATE); OUT_COMMON_TARGET: the OBJECT library of COMMON_SOURCES, if any.
# - Best-effort configure-time checks: a warning if a plug-in source defines OFX::Plugin::getPluginIDs
#   (it would clash with the Support library's), or if no string literal of the plug-in's sources
#   (or of the headers in their directories) starts with its explicit ID; NO_ID_CHECK skips that.
# Documentation/proposals/cmake-plugins.md has an example.
function(ofx_plugin_bundle_group GROUP)
  set(_ctx "ofx_plugin_bundle_group(${GROUP})")
  set(_name_re "^[A-Za-z0-9_.+-]+$")
  if(NOT GROUP MATCHES "${_name_re}")
    message(FATAL_ERROR "${_ctx}: the group name must match ${_name_re}")
  endif()
  # split the arguments at PLUGIN
  set(_head "")
  set(_np 0)
  set(_cur _head)
  foreach(_a IN LISTS ARGN)
    if(_a STREQUAL "PLUGIN")
      math(EXPR _np "${_np} + 1")
      set(_cur _seg${_np})
      set(${_cur} "")
    else()
      list(APPEND ${_cur} "${_a}")
    endif()
  endforeach()
  if(_np EQUAL 0)
    message(FATAL_ERROR "${_ctx}: no PLUGIN")
  endif()
  cmake_parse_arguments(G "NO_DEPLOY"
    "COMBINED_NAME;COMBINED_ID;COMBINED_INFO_PLIST;ID_PREFIX;VERSION;OUT_TARGETS;OUT_COMMON_TARGET"
    "COMBINED_RESOURCES;COMBINED_SOURCES;ALSO_SEPARATE;COMMON_SOURCES;COMMON_LINK_LIBRARIES;COMMON_COMPILE_DEFINITIONS;COMMON_COMPILE_OPTIONS;COMMON_INCLUDE_DIRECTORIES;COMMON_RPATH;COMMON_EXPORT_SYMBOLS"
    ${_head})
  if(G_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "${_ctx}: unknown arguments: ${G_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT G_COMBINED_NAME)
    set(G_COMBINED_NAME "${GROUP}")
  endif()
  if(NOT G_COMBINED_NAME MATCHES "${_name_re}")
    message(FATAL_ERROR "${_ctx}: COMBINED_NAME ${G_COMBINED_NAME} must match ${_name_re}")
  endif()
  if(NOT G_COMBINED_ID)
    set(G_COMBINED_ID "org.openeffects.plugins.${G_COMBINED_NAME}")
  endif()
  if(NOT G_VERSION)
    if(PROJECT_VERSION)
      set(G_VERSION "${PROJECT_VERSION}")
    else()
      set(G_VERSION "1.0.0")
    endif()
  endif()
  if(DEFINED OFX_PLUGIN_BUNDLES_${GROUP})
    set(_mode "${OFX_PLUGIN_BUNDLES_${GROUP}}")
  else()
    set(_mode "${OFX_PLUGIN_BUNDLES}")
  endif()
  if(_mode STREQUAL "")
    set(_mode COMBINED)
  endif()
  string(TOUPPER "${_mode}" _mode)
  if(NOT _mode MATCHES "^(COMBINED|SEPARATE)$")
    message(FATAL_ERROR "${_ctx}: OFX_PLUGIN_BUNDLES (or OFX_PLUGIN_BUNDLES_${GROUP}) is '${_mode}', not COMBINED or SEPARATE")
  endif()

  _ofx_group_abs_paths(G_COMBINED_SOURCES ${G_COMBINED_SOURCES})
  _ofx_group_abs_paths(G_COMMON_SOURCES ${G_COMMON_SOURCES})
  _ofx_group_abs_paths(G_COMMON_INCLUDE_DIRECTORIES ${G_COMMON_INCLUDE_DIRECTORIES})
  _ofx_group_definitions(G_COMMON_COMPILE_DEFINITIONS ${G_COMMON_COMPILE_DEFINITIONS})
  _ofx_group_parse_resources(_cr_src _cr_sub "${_ctx}: COMBINED_RESOURCES" ${G_COMBINED_RESOURCES})
  if(G_COMBINED_INFO_PLIST)
    get_filename_component(G_COMBINED_INFO_PLIST "${G_COMBINED_INFO_PLIST}" ABSOLUTE)
  endif()

  # the plug-ins: _p<i>_<field>
  set(_kws SOURCES ID REGISTER RESOURCES INFO_PLIST LINK_LIBRARIES COMPILE_DEFINITIONS COMPILE_OPTIONS
           INCLUDE_DIRECTORIES OPENMP COMBINED_ONLY SEPARATE_ONLY NO_ID_CHECK PROPERTIES OUT_TARGET)
  # PROPERTIES may not use these names: the keywords, and the bookkeeping properties of the
  # helpers that add sources to a target (supportext_add_sources, ioSupport_add_sources), which
  # skip the components listed there
  set(_reserved_props ${_kws} PLUGIN DESTINATION COMBINED_NAME COMBINED_ID COMBINED_INFO_PLIST
      COMBINED_RESOURCES COMBINED_SOURCES ID_PREFIX VERSION ALSO_SEPARATE NO_DEPLOY COMMON_SOURCES
      COMMON_LINK_LIBRARIES COMMON_COMPILE_DEFINITIONS COMMON_COMPILE_OPTIONS COMMON_INCLUDE_DIRECTORIES
      COMMON_RPATH COMMON_EXPORT_SYMBOLS OUT_TARGETS OUT_COMMON_TARGET
      SUPPORTEXT_COMPONENTS IOSUPPORT_COMPONENTS)
  set(_names "")
  set(_ids "")
  foreach(_i RANGE 1 ${_np})
    set(_seg "${_seg${_i}}")
    list(LENGTH _seg _n)
    if(_n EQUAL 0)
      message(FATAL_ERROR "${_ctx}: PLUGIN without a name")
    endif()
    list(GET _seg 0 _name)
    list(REMOVE_AT _seg 0)
    if(NOT _name MATCHES "${_name_re}" OR _name IN_LIST _kws)
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: the name must match ${_name_re}")
    endif()
    if(_name IN_LIST _names)
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name} is declared twice")
    endif()
    list(APPEND _names ${_name})
    set(_p${_i}_name ${_name})
    # PROPERTIES <property> <value>... (may be repeated)
    set(_rest "")
    set(_state "")
    set(_p${_i}_props "")
    set(_k 0)
    foreach(_a IN LISTS _seg)
      if(_a STREQUAL "PROPERTIES")
        set(_state name)
      elseif(_state STREQUAL "name")
        if(_a IN_LIST _reserved_props OR _a MATCHES "^OFX_")
          message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: PROPERTIES ${_a}: a keyword, or a property of OpenFX.cmake or of a helper (e.g. supportext_add_sources' own SUPPORTEXT_COMPONENTS). Use a custom, project-prefixed name, e.g. MYPROJECT_SUPPORTEXT")
        endif()
        math(EXPR _k "${_k} + 1")
        list(APPEND _p${_i}_props "${_a}")
        set(_p${_i}_pv${_k} "")
        set(_state values)
      elseif(_state STREQUAL "values" AND NOT _a IN_LIST _kws)
        list(APPEND _p${_i}_pv${_k} "${_a}")
      else()
        set(_state "")
        list(APPEND _rest "${_a}")
      endif()
    endforeach()
    if(_state STREQUAL "name")
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: PROPERTIES without a property name")
    endif()
    cmake_parse_arguments(P "OPENMP;COMBINED_ONLY;SEPARATE_ONLY;NO_ID_CHECK" "ID;INFO_PLIST;OUT_TARGET"
      "SOURCES;REGISTER;RESOURCES;LINK_LIBRARIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS;INCLUDE_DIRECTORIES"
      ${_rest})
    if(P_UNPARSED_ARGUMENTS)
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: unknown arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT P_SOURCES)
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name} has no SOURCES")
    endif()
    if(P_COMBINED_ONLY AND P_SEPARATE_ONLY)
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: COMBINED_ONLY and SEPARATE_ONLY")
    endif()
    if(P_ID)
      set(_id "${P_ID}")
    elseif(DEFINED G_ID_PREFIX)
      set(_id "${G_ID_PREFIX}${_name}")
    elseif(P_COMBINED_ONLY)
      set(_id "")
    else()
      message(FATAL_ERROR "${_ctx}: PLUGIN ${_name} has no ID, and the group has no ID_PREFIX (a separate bundle is named after its plug-in's identifier)")
    endif()
    if(_id)
      if(NOT _id MATCHES "${_name_re}")
        message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: the identifier '${_id}' must match ${_name_re}")
      endif()
      if(_id IN_LIST _ids)
        message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: the identifier ${_id} is used twice")
      endif()
      list(APPEND _ids "${_id}")
    endif()
    foreach(_f IN LISTS P_REGISTER)
      if(NOT _f MATCHES "^(::)?[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*$")
        message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: REGISTER ${_f} is not a C++ function name")
      endif()
      if(_f MATCHES "^(::)?OFX::Plugin::getPluginIDs$")
        message(FATAL_ERROR "${_ctx}: PLUGIN ${_name}: REGISTER ${_f}: the Support library defines OFX::Plugin::getPluginIDs (a duplicate symbol): rename the plug-in's function")
      endif()
    endforeach()
    set(_p${_i}_id "${_id}")
    set(_p${_i}_explicit_id "")
    if(NOT P_NO_ID_CHECK)
      set(_p${_i}_explicit_id "${P_ID}") # (only for the ID check)
    endif()
    _ofx_group_abs_paths(_p${_i}_sources ${P_SOURCES})
    list(REMOVE_DUPLICATES _p${_i}_sources)
    _ofx_group_abs_paths(_p${_i}_incs ${P_INCLUDE_DIRECTORIES})
    _ofx_group_definitions(_p${_i}_defs ${P_COMPILE_DEFINITIONS})
    if(_p${_i}_incs)
      list(REMOVE_DUPLICATES _p${_i}_incs)
    endif()
    if(_p${_i}_defs)
      list(REMOVE_DUPLICATES _p${_i}_defs)
    endif()
    set(_p${_i}_opts "${P_COMPILE_OPTIONS}")
    set(_p${_i}_libs "${P_LINK_LIBRARIES}")
    set(_p${_i}_register "${P_REGISTER}")
    set(_p${_i}_openmp ${P_OPENMP})
    set(_p${_i}_combined_only ${P_COMBINED_ONLY})
    set(_p${_i}_separate_only ${P_SEPARATE_ONLY})
    set(_p${_i}_out "${P_OUT_TARGET}")
    set(_p${_i}_plist "")
    if(P_INFO_PLIST)
      # checked in both modes, so that a COMBINED build warns too
      get_filename_component(_plist "${P_INFO_PLIST}" ABSOLUTE)
      if(_id AND NOT P_COMBINED_ONLY)
        _ofx_group_check_plist(_ok ${_name} ${_id} "${_plist}")
        if(_ok)
          set(_p${_i}_plist "${_plist}")
        else()
          message(WARNING "${_ctx}: PLUGIN ${_name}: INFO_PLIST ${_plist} is not used: ${_ok_msg} (use @PLUGIN_EXE@ and @BUNDLE_IDENTIFIER@). A separate bundle gets a generated Info.plist.")
        endif()
      else()
        message(WARNING "${_ctx}: PLUGIN ${_name}: INFO_PLIST is not used (a COMBINED_ONLY plug-in has no separate bundle)")
      endif()
    endif()
    _ofx_group_parse_resources(_p${_i}_rsrc _p${_i}_rsub "${_ctx}: PLUGIN ${_name}: RESOURCES" ${P_RESOURCES})
    _ofx_group_source_checks(${_i})
  endforeach()
  foreach(_a IN LISTS G_ALSO_SEPARATE)
    list(FIND _names "${_a}" _j)
    if(_j EQUAL -1)
      message(FATAL_ERROR "${_ctx}: ALSO_SEPARATE ${_a}: no such PLUGIN")
    endif()
    math(EXPR _j "${_j} + 1")
    if(_p${_j}_combined_only)
      message(FATAL_ERROR "${_ctx}: ALSO_SEPARATE ${_a}: the plug-in is COMBINED_ONLY")
    endif()
  endforeach()

  # a combined target named like a plug-in's separate target, in either mode: an error in both
  set(_has_combined_only FALSE)
  foreach(_i RANGE 1 ${_np})
    if(_p${_i}_combined_only)
      set(_has_combined_only TRUE)
    endif()
  endforeach()
  list(FIND _names "${G_COMBINED_NAME}" _j)
  if(NOT _j EQUAL -1)
    math(EXPR _j "${_j} + 1")
    if(_p${_j}_separate_only OR G_COMBINED_NAME IN_LIST G_ALSO_SEPARATE
       OR (_has_combined_only AND NOT _p${_j}_combined_only))
      message(FATAL_ERROR "${_ctx}: the combined bundle's target ${G_COMBINED_NAME} (COMBINED_NAME, default <group>) is also the target of PLUGIN ${G_COMBINED_NAME}'s separate bundle (in COMBINED or SEPARATE mode): rename one of them")
    endif()
  endif()

  # what goes where
  set(_combined "")
  set(_separate "")
  foreach(_i RANGE 1 ${_np})
    if(_mode STREQUAL "COMBINED")
      if(NOT _p${_i}_separate_only)
        list(APPEND _combined ${_i})
      endif()
      if(_p${_i}_separate_only OR _p${_i}_name IN_LIST G_ALSO_SEPARATE)
        list(APPEND _separate ${_i})
      endif()
    elseif(_p${_i}_combined_only)
      list(APPEND _combined ${_i})
    else()
      list(APPEND _separate ${_i})
    endif()
  endforeach()

  # the common objects
  set(_common_obj "")
  if(G_COMMON_SOURCES)
    set(_common_obj "${GROUP}_common")
    if(TARGET ${_common_obj})
      message(FATAL_ERROR "${_ctx}: the target ${_common_obj} (COMMON_SOURCES) already exists")
    endif()
    add_library(${_common_obj} OBJECT ${G_COMMON_SOURCES})
    _ofx_group_object_settings(${_common_obj})
    if(G_COMMON_LINK_LIBRARIES)
      target_link_libraries(${_common_obj} PUBLIC ${G_COMMON_LINK_LIBRARIES})
    endif()
    _ofx_group_compile_settings(${_common_obj})
  endif()

  set(_targets "")
  foreach(_i RANGE 1 ${_np})
    set(_p${_i}_targets "")
  endforeach()
  if(_combined)
    _ofx_group_bundle(${G_COMBINED_NAME} ${G_COMBINED_NAME} "" COMBINED ${_combined})
    list(APPEND _targets ${G_COMBINED_NAME})
    foreach(_i IN LISTS _combined)
      list(APPEND _p${_i}_targets ${G_COMBINED_NAME})
    endforeach()
  endif()
  foreach(_i IN LISTS _separate)
    _ofx_group_bundle(${_p${_i}_name} ${_p${_i}_id} "${_p${_i}_plist}" SEPARATE ${_i})
    list(APPEND _targets ${_p${_i}_name})
    list(APPEND _p${_i}_targets ${_p${_i}_name})
  endforeach()

  foreach(_i RANGE 1 ${_np})
    if(_p${_i}_out)
      set(${_p${_i}_out} "${_p${_i}_targets}" PARENT_SCOPE)
    endif()
  endforeach()
  if(G_OUT_TARGETS)
    set(${G_OUT_TARGETS} "${_targets}" PARENT_SCOPE)
  endif()
  if(G_OUT_COMMON_TARGET)
    set(${G_OUT_COMMON_TARGET} "${_common_obj}" PARENT_SCOPE)
  endif()
endfunction()

# _ofx_group_abs_paths(<var> <path>...): absolute paths (relative to the current source
# directory), generator expressions kept as they are
function(_ofx_group_abs_paths VAR)
  set(_out "")
  foreach(_p IN LISTS ARGN)
    if(NOT _p MATCHES "^\\$<")
      get_filename_component(_p "${_p}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    list(APPEND _out "${_p}")
  endforeach()
  set(${VAR} "${_out}" PARENT_SCOPE)
endfunction()

# _ofx_group_definitions(<var> <def>...): without a leading -D, as target_compile_definitions
function(_ofx_group_definitions VAR)
  set(_out "")
  foreach(_d IN LISTS ARGN)
    string(REGEX REPLACE "^-D" "" _d "${_d}")
    list(APPEND _out "${_d}")
  endforeach()
  set(${VAR} "${_out}" PARENT_SCOPE)
endfunction()

# _ofx_group_parse_resources(<srcs var> <subdirs var> <context> <item>...): the absolute source
# of each item, and its subdirectory of Contents/Resources ("." for none)
function(_ofx_group_parse_resources SRCS SUBS CTX)
  set(_srcs "")
  set(_subs "")
  set(_dest FALSE)
  foreach(_t IN LISTS ARGN)
    if(_dest)
      string(REGEX REPLACE "^(\\./)+" "" _t "${_t}")
      string(REGEX REPLACE "/+$" "" _t "${_t}")
      if(_t STREQUAL "" OR _t STREQUAL "." OR IS_ABSOLUTE "${_t}" OR _t MATCHES "^[A-Za-z]:|^/|\\\\"
         OR _t MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "${CTX}: DESTINATION '${_t}' must be a relative subdirectory of Contents/Resources (no '..')")
      endif()
      list(REMOVE_AT _subs -1)
      list(APPEND _subs "${_t}")
      set(_dest FALSE)
    elseif(_t STREQUAL "DESTINATION")
      list(LENGTH _subs _n)
      set(_last ".")
      if(_n GREATER 0)
        list(GET _subs -1 _last)
      endif()
      if(_n EQUAL 0 OR NOT _last STREQUAL ".")
        message(FATAL_ERROR "${CTX}: DESTINATION must follow a file or a directory")
      endif()
      set(_dest TRUE)
    else()
      get_filename_component(_abs "${_t}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
      if(NOT EXISTS "${_abs}")
        message(FATAL_ERROR "${CTX}: ${_abs} doesn't exist")
      endif()
      list(APPEND _srcs "${_abs}")
      list(APPEND _subs ".")
    endif()
  endforeach()
  if(_dest)
    message(FATAL_ERROR "${CTX}: DESTINATION without a subdirectory")
  endif()
  set(${SRCS} "${_srcs}" PARENT_SCOPE)
  set(${SUBS} "${_subs}" PARENT_SCOPE)
endfunction()

# _ofx_group_source_checks(<i>) (in ofx_plugin_bundle_group's scope): the per-plug-in
# COMPILE_DEFINITIONS and COMPILE_OPTIONS of plug-in <i>, as source file properties (the same for
# every plug-in that lists a source), and the best-effort checks of its sources
function(_ofx_group_source_checks I)
  set(_name ${_p${I}_name})
  set(_sig "D:${_p${I}_defs}|O:${_p${I}_opts}")
  set(_id_found FALSE)
  # REGISTER functions named getPluginIDs in another namespace: no getPluginIDs warning
  set(_own_getpluginids FALSE)
  foreach(_f IN LISTS _p${I}_register)
    if(_f MATCHES "(^|::)getPluginIDs$")
      set(_own_getpluginids TRUE)
    endif()
  endforeach()
  foreach(_s IN LISTS _p${I}_sources)
    if(_s MATCHES "^\\$<")
      if(_p${I}_defs OR _p${I}_opts)
        message(WARNING "${_ctx}: PLUGIN ${_name}: the source ${_s} is a generator expression: it doesn't get the plug-in's COMPILE_DEFINITIONS and COMPILE_OPTIONS")
      endif()
      continue()
    endif()
    string(MD5 _key "${_s}")
    get_property(_known DIRECTORY PROPERTY _OFX_GROUP_SRC_${_key} SET)
    if(_known)
      get_property(_osig DIRECTORY PROPERTY _OFX_GROUP_SRC_${_key})
      get_property(_owner DIRECTORY PROPERTY _OFX_GROUP_SRC_OWNER_${_key})
      if(NOT _osig STREQUAL _sig)
        message(FATAL_ERROR "${_ctx}: ${_s} is listed by the plug-ins ${_owner} and ${_name}, with different COMPILE_DEFINITIONS or COMPILE_OPTIONS (they are properties of the source file)")
      endif()
    else()
      set_property(DIRECTORY PROPERTY _OFX_GROUP_SRC_${_key} "${_sig}")
      set_property(DIRECTORY PROPERTY _OFX_GROUP_SRC_OWNER_${_key} "${_name}")
      if(_p${I}_defs)
        set_property(SOURCE "${_s}" APPEND PROPERTY COMPILE_DEFINITIONS ${_p${I}_defs})
      endif()
      if(_p${I}_opts)
        set_property(SOURCE "${_s}" APPEND PROPERTY COMPILE_OPTIONS ${_p${I}_opts})
      endif()
    endif()
    # best effort: a definition of OFX::Plugin::getPluginIDs (once per source file), and a string
    # literal that starts with the explicit ID (e.g. "net.sf.openfx.MergePlugin" for the ID
    # net.sf.openfx.Merge of an entry that registers several factories)
    get_property(_read DIRECTORY PROPERTY _OFX_GROUP_READ_${_key} SET)
    if((_read AND NOT _p${I}_explicit_id) OR NOT EXISTS "${_s}" OR IS_DIRECTORY "${_s}")
      continue()
    endif()
    file(READ "${_s}" _content)
    if(NOT _read)
      set_property(DIRECTORY PROPERTY _OFX_GROUP_READ_${_key} TRUE)
      if(NOT _own_getpluginids AND _content MATCHES "getPluginIDs[ \t\r\n]*\\([^)]*\\)[ \t\r\n]*{")
        message(WARNING "${_ctx}: PLUGIN ${_name}: ${_s} seems to define OFX::Plugin::getPluginIDs. "
          "The Support library registers the factories automatically (mRegisterPluginFactoryInstance) and "
          "defines getPluginIDs itself (a duplicate symbol at link time): use mRegisterPluginFactoryInstance, "
          "or rename the function and pass it to REGISTER.")
      endif()
    endif()
    if(_p${I}_explicit_id)
      string(FIND "${_content}" "\"${_p${I}_explicit_id}" _pos)
      if(NOT _pos EQUAL -1)
        set(_id_found TRUE)
      endif()
    endif()
  endforeach()
  # also the headers next to the sources (e.g. a kPluginIdentifier in CImg/Erode/CImgErodeDilate.h)
  if(_p${I}_explicit_id AND NOT _id_found)
    set(_dirs "")
    foreach(_s IN LISTS _p${I}_sources)
      if(NOT _s MATCHES "^\\$<")
        get_filename_component(_d "${_s}" DIRECTORY)
        list(APPEND _dirs "${_d}")
      endif()
    endforeach()
    if(_dirs)
      list(REMOVE_DUPLICATES _dirs)
    endif()
    foreach(_d IN LISTS _dirs)
      file(GLOB _hdrs LIST_DIRECTORIES false "${_d}/*.h" "${_d}/*.hpp" "${_d}/*.hxx" "${_d}/*.H")
      foreach(_h IN LISTS _hdrs)
        file(READ "${_h}" _content)
        string(FIND "${_content}" "\"${_p${I}_explicit_id}" _pos)
        if(NOT _pos EQUAL -1)
          set(_id_found TRUE)
          break()
        endif()
      endforeach()
      if(_id_found)
        break()
      endif()
    endforeach()
  endif()
  if(_p${I}_explicit_id AND NOT _id_found)
    message(WARNING "${_ctx}: PLUGIN ${_name}: no string literal of its SOURCES, or of the headers in their directories, starts with the ID \"${_p${I}_explicit_id}\" (it should name the plug-in, e.g. the start of its kPluginIdentifier; NO_ID_CHECK skips this check)")
  endif()
endfunction()

# _ofx_group_check_plist(<ok var> <target> <bundle> <file>) (in ofx_plugin_bundle_group's scope):
# whether a plug-in's INFO_PLIST, configured as add_ofx_plugin does, has CFBundleExecutable
# <bundle>.ofx and CFBundleIdentifier <bundle>; the reason in _ok_msg otherwise
function(_ofx_group_check_plist OK TGT BUNDLE PLIST)
  set(PLUGIN_NAME ${BUNDLE})
  set(PLUGIN_EXE ${BUNDLE}.ofx)
  set(BUNDLE_IDENTIFIER ${BUNDLE})
  set(BUNDLE_SIGNATURE "????")
  set(PLUGIN_VERSION ${G_VERSION})
  configure_file("${PLIST}" "${CMAKE_CURRENT_BINARY_DIR}/${TGT}.plugin.Info.plist.check" @ONLY)
  _ofx_group_plist_values(_exe _pid "${CMAKE_CURRENT_BINARY_DIR}/${TGT}.plugin.Info.plist.check")
  if(_exe STREQUAL "${BUNDLE}.ofx" AND _pid STREQUAL "${BUNDLE}")
    set(${OK} TRUE PARENT_SCOPE)
  else()
    set(${OK} FALSE PARENT_SCOPE)
    set(_ok_msg "its CFBundleExecutable is '${_exe}' and its CFBundleIdentifier '${_pid}', not ${BUNDLE}.ofx and ${BUNDLE}" PARENT_SCOPE)
  endif()
endfunction()

# the COMMON_COMPILE_* settings of a group (in ofx_plugin_bundle_group's scope)
function(_ofx_group_compile_settings TARGET)
  if(G_COMMON_COMPILE_DEFINITIONS)
    target_compile_definitions(${TARGET} PRIVATE ${G_COMMON_COMPILE_DEFINITIONS})
  endif()
  if(G_COMMON_COMPILE_OPTIONS)
    target_compile_options(${TARGET} PRIVATE ${G_COMMON_COMPILE_OPTIONS})
  endif()
  if(G_COMMON_INCLUDE_DIRECTORIES)
    target_include_directories(${TARGET} PRIVATE ${G_COMMON_INCLUDE_DIRECTORIES})
  endif()
endfunction()

# the compile settings that add_ofx_plugin gives a plug-in, for the OBJECT library of COMMON_SOURCES
function(_ofx_group_object_settings TARGET)
  set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON
                                             C_VISIBILITY_PRESET hidden
                                             CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON
                                             CXX_STANDARD ${OFX_CXX_STANDARD}
                                             CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF)
  if(TARGET openfx::Api)
    target_link_libraries(${TARGET} PUBLIC openfx::Api)
  endif()
  if(APPLE)
    target_compile_definitions(${TARGET} PRIVATE GL_SILENCE_DEPRECATION)
  endif()
  target_compile_definitions(${TARGET} PRIVATE $<$<CONFIG:Debug>:DEBUG>)
  ofx_target_optflag(${TARGET})
  if(MSVC)
    target_compile_options(${TARGET} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
  elseif(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${TARGET} PRIVATE -Wa,-mbig-obj)
  endif()
endfunction()

# _ofx_group_plist_values(<exe var> <identifier var> <file>): CFBundleExecutable and
# CFBundleIdentifier of a configured Info.plist
function(_ofx_group_plist_values EXE ID FILE)
  file(READ "${FILE}" _c)
  set(_exe "")
  set(_id "")
  if(_c MATCHES "<key>CFBundleExecutable</key>[ \t\r\n]*<string>([^<]*)</string>")
    set(_exe "${CMAKE_MATCH_1}")
  endif()
  if(_c MATCHES "<key>CFBundleIdentifier</key>[ \t\r\n]*<string>([^<]*)</string>")
    set(_id "${CMAKE_MATCH_1}")
  endif()
  set(${EXE} "${_exe}" PARENT_SCOPE)
  set(${ID} "${_id}" PARENT_SCOPE)
endfunction()

# _ofx_group_bundle(<target> <bundle name> <Info.plist or ""> COMBINED|SEPARATE <plug-in index>...)
# (in ofx_plugin_bundle_group's scope): one bundle
function(_ofx_group_bundle TGT BUNDLE PLIST KIND)
  set(_members ${ARGN})
  if(TARGET ${TGT})
    message(FATAL_ERROR "${_ctx}: the target ${TGT} already exists")
  endif()
  # (case-insensitive: the file systems of macOS and Windows usually are)
  get_property(_bundles GLOBAL PROPERTY _OFX_GROUP_BUNDLES)
  string(TOLOWER "${BUNDLE}" _lbundle)
  if(_lbundle IN_LIST _bundles)
    message(FATAL_ERROR "${_ctx}: the bundle ${BUNDLE}.ofx.bundle is already made by another group")
  endif()
  set_property(GLOBAL APPEND PROPERTY _OFX_GROUP_BUNDLES ${_lbundle})

  # Info.plist: checked with the values add_ofx_plugin configures it with
  if(KIND STREQUAL "COMBINED")
    set(_ident "${G_COMBINED_ID}")
    set(PLIST "${G_COMBINED_INFO_PLIST}")
  else()
    set(_ident "${BUNDLE}")
  endif()
  set(_plist_args IDENTIFIER "${_ident}")
  if(PLIST AND KIND STREQUAL "SEPARATE")
    list(APPEND _plist_args INFO_PLIST "${PLIST}") # (checked by _ofx_group_check_plist)
  elseif(PLIST)
    set(PLUGIN_NAME ${BUNDLE})
    set(PLUGIN_EXE ${BUNDLE}.ofx)
    set(BUNDLE_IDENTIFIER ${_ident})
    set(BUNDLE_SIGNATURE "????")
    set(PLUGIN_VERSION ${G_VERSION})
    configure_file("${PLIST}" "${CMAKE_CURRENT_BINARY_DIR}/${TGT}.Info.plist.check" @ONLY)
    _ofx_group_plist_values(_exe _pid "${CMAKE_CURRENT_BINARY_DIR}/${TGT}.Info.plist.check")
    if(KIND STREQUAL "COMBINED")
      if(NOT _exe STREQUAL "${BUNDLE}.ofx")
        message(FATAL_ERROR "${_ctx}: COMBINED_INFO_PLIST ${PLIST}: CFBundleExecutable is '${_exe}', not ${BUNDLE}.ofx (or @PLUGIN_EXE@)")
      endif()
      list(APPEND _plist_args INFO_PLIST "${PLIST}")
    endif()
  endif()

  set(_sources "")
  set(_libs "")
  set(_incs "")
  set(_register "")
  set(_openmp FALSE)
  set(_rsrc "")
  set(_rsub "")
  set(_plugins "")
  foreach(_i IN LISTS _members)
    list(APPEND _plugins ${_p${_i}_name})
    list(APPEND _sources ${_p${_i}_sources})
    list(APPEND _libs ${_p${_i}_libs})
    list(APPEND _incs ${_p${_i}_incs})
    list(APPEND _register ${_p${_i}_register})
    list(APPEND _rsrc ${_p${_i}_rsrc})
    list(APPEND _rsub ${_p${_i}_rsub})
    if(_p${_i}_openmp)
      set(_openmp TRUE)
    endif()
  endforeach()
  if(KIND STREQUAL "COMBINED")
    list(APPEND _sources ${G_COMBINED_SOURCES})
    list(APPEND _rsrc ${_cr_src})
    list(APPEND _rsub ${_cr_sub})
  endif()
  list(REMOVE_DUPLICATES _sources)

  set(_extra_args "")
  if(G_COMMON_RPATH)
    list(APPEND _extra_args RPATH ${G_COMMON_RPATH})
  endif()
  if(G_COMMON_EXPORT_SYMBOLS)
    list(APPEND _extra_args EXPORT_SYMBOLS ${G_COMMON_EXPORT_SYMBOLS})
  endif()
  add_ofx_plugin(${TGT} BUNDLE_NAME ${BUNDLE} VERSION ${G_VERSION} ${_plist_args} ${_extra_args} SOURCES ${_sources})
  # PROPERTIES: custom properties only, not one that add_ofx_plugin set
  foreach(_i IN LISTS _members)
    foreach(_prop IN LISTS _p${_i}_props)
      get_property(_set TARGET ${TGT} PROPERTY ${_prop} SET)
      if(_set)
        message(FATAL_ERROR "${_ctx}: PLUGIN ${_p${_i}_name}: PROPERTIES ${_prop}: the target ${TGT} already has this property (only custom properties are allowed)")
      endif()
    endforeach()
  endforeach()
  # (the LINK_LIBRARIES property: the caller may use either form of target_link_libraries)
  if(_common_obj)
    set_property(TARGET ${TGT} APPEND PROPERTY LINK_LIBRARIES ${_common_obj})
  else()
    set_property(TARGET ${TGT} APPEND PROPERTY LINK_LIBRARIES ${G_COMMON_LINK_LIBRARIES})
  endif()
  set_property(TARGET ${TGT} APPEND PROPERTY LINK_LIBRARIES ${_libs})
  _ofx_group_compile_settings(${TGT})
  # a plug-in's INCLUDE_DIRECTORIES: on the target (a combined bundle gets the union), so that a
  # source shared by plug-ins (e.g. CImgFilter.cpp) sees them all
  if(_incs)
    list(REMOVE_DUPLICATES _incs)
    target_include_directories(${TGT} PRIVATE ${_incs})
  endif()

  if(_register)
    list(REMOVE_DUPLICATES _register)
    set(_decls "")
    set(_calls "")
    foreach(_f IN LISTS _register)
      string(REGEX REPLACE "^::" "" _f "${_f}")
      string(REPLACE "::" ";" _parts "${_f}")
      list(LENGTH _parts _n)
      math(EXPR _last "${_n} - 1")
      list(GET _parts ${_last} _base)
      list(REMOVE_AT _parts ${_last})
      set(_open "")
      set(_close "")
      foreach(_ns IN LISTS _parts)
        string(APPEND _open "namespace ${_ns} { ")
        string(APPEND _close " }")
      endforeach()
      string(APPEND _decls "${_open}void ${_base}(OFX::PluginFactoryArray &ids);${_close}\n")
      string(APPEND _calls "    ::${_f}(ids);\n")
    endforeach()
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${TGT}.register.cpp")
    file(WRITE "${_gen}.new"
"// Generated by ${_ctx} for ${BUNDLE}.ofx: the REGISTER functions of its plug-ins, called in
// declaration order at static initialization, add their factories to the Support library's
// automatic registration (as mRegisterPluginFactoryInstance does).
#include \"ofxsImageEffect.h\"

${_decls}
namespace {
struct OfxBundleGroupRegistration {
  OfxBundleGroupRegistration() {
    OFX::PluginFactoryArray &ids = OFX::PluginFactories::plugIDs();
${_calls}  }
};
OfxBundleGroupRegistration ofxBundleGroupRegistration;
}
")
    configure_file("${_gen}.new" "${_gen}" COPYONLY)
    target_sources(${TGT} PRIVATE "${_gen}")
  endif()
  if(_openmp)
    ofx_target_openmp(${TGT})
  endif()
  set_target_properties(${TGT} PROPERTIES OFX_BUNDLE_PLUGINS "${_plugins}" OFX_BUNDLE_GROUP "${GROUP}")
  foreach(_i IN LISTS _members)
    set(_k 0)
    foreach(_prop IN LISTS _p${_i}_props)
      math(EXPR _k "${_k} + 1")
      get_property(_v TARGET ${TGT} PROPERTY ${_prop})
      list(APPEND _v ${_p${_i}_pv${_k}})
      if(_v)
        list(REMOVE_DUPLICATES _v)
      endif()
      set_property(TARGET ${TGT} PROPERTY ${_prop} "${_v}")
    endforeach()
  endforeach()
  _ofx_group_resources(${TGT} _rsrc _rsub)
  if(NOT G_NO_DEPLOY)
    ofx_deploy_bundle(${TGT})
  endif()
endfunction()

# _ofx_group_resources(<target> <srcs var> <subdirs var>): ofx_plugin_resources() for the items,
# each once, with an error if two different files have the same destination
function(_ofx_group_resources TGT SRCS SUBS)
  set(_srcs "${${SRCS}}")
  set(_subs "${${SUBS}}")
  list(LENGTH _srcs _n)
  if(_n EQUAL 0)
    return()
  endif()
  math(EXPR _n "${_n} - 1")
  set(_done "")
  set(_order "")
  foreach(_j RANGE ${_n})
    list(GET _srcs ${_j} _s)
    list(GET _subs ${_j} _sub)
    set(_item "${_s}|${_sub}")
    if(_item IN_LIST _done)
      continue()
    endif()
    list(APPEND _done "${_item}")
    if(_sub STREQUAL ".")
      set(_prefix "")
    else()
      set(_prefix "${_sub}/")
    endif()
    get_filename_component(_fname "${_s}" NAME)
    set(_dests "")
    set(_froms "")
    if(IS_DIRECTORY "${_s}")
      file(GLOB_RECURSE _rel LIST_DIRECTORIES false RELATIVE "${_s}" "${_s}/*")
      foreach(_r IN LISTS _rel)
        list(APPEND _dests "${_prefix}${_fname}/${_r}")
        list(APPEND _froms "${_s}/${_r}")
      endforeach()
    else()
      set(_dests "${_prefix}${_fname}")
      set(_froms "${_s}")
    endif()
    list(LENGTH _dests _m)
    if(_m GREATER 0)
      math(EXPR _m "${_m} - 1")
      foreach(_k RANGE ${_m})
        list(GET _dests ${_k} _d)
        list(GET _froms ${_k} _f)
        # (case-insensitive: the file systems of macOS and Windows usually are)
        string(TOLOWER "${_d}" _key)
        string(MD5 _key "${_key}")
        if(DEFINED _dest_${_key} AND NOT _dest_${_key} STREQUAL _f)
          message(FATAL_ERROR "${_ctx}: ${TGT}: two resources go to Contents/Resources/${_d}: ${_dest_${_key}} and ${_f}")
        endif()
        set(_dest_${_key} "${_f}")
      endforeach()
    endif()
    string(MD5 _skey "${_sub}")
    if(NOT DEFINED _items_${_skey})
      set(_items_${_skey} "")
      list(APPEND _order "${_sub}")
    endif()
    list(APPEND _items_${_skey} "${_s}")
  endforeach()
  foreach(_sub IN LISTS _order)
    string(MD5 _skey "${_sub}")
    if(_sub STREQUAL ".")
      ofx_plugin_resources(${TGT} ${_items_${_skey}})
    else()
      ofx_plugin_resources(${TGT} DESTINATION "${_sub}" ${_items_${_skey}})
    endif()
  endforeach()
endfunction()

cmake_policy(POP)
