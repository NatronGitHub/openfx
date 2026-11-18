# SPDX-License-Identifier: BSD-3-Clause
#
# Building OFX plug-in bundles with CMake: add_ofx_plugin(), ofx_plugin_resources(),
# ofx_target_openmp(), ofx_deploy_bundle(). The reference for the plug-in repositories
# (openfx-misc, openfx-io, ...): see Documentation/proposals/cmake-plugins.md.
# Ported from AcademySoftwareFoundation/openfx; adapted for the NatronGitHub
# fork: the bundle is also assembled in the build tree (usable directly with
# OFX_PLUGIN_PATH), Info.plist is generated at build time instead of being
# written into PLUGIN_INSTALLDIR at configure time, and the architecture folder
# follows the OFX packaging spec.

# install(DESTINATION) paths are normalized (CMake 3.31): for the functions below, also when this
# module is used through find_package(openfx)
if(POLICY CMP0177)
  cmake_policy(SET CMP0177 NEW)
endif()

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
# Linux and FreeBSD: link the plug-ins with -z defs (no undefined symbols); OFF for sanitizer builds
option(OFX_PLUGIN_NO_UNDEFINED "Linux/FreeBSD: link the plug-ins with -z defs (no undefined symbols)" ON)
set(OFX_PLUGIN_RPATH "" CACHE STRING "Extra install rpath directories of the plug-ins (;-separated)")

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

# Location of the Info.plist template and osxDeploy.sh. In the OpenFX source tree they are in
# Examples/ and Support/include/; in an installed package they sit next to this module.
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
#   Unlike Makefile.master's list, it has no system directory (/usr/lib64, ...): CMake leaves
#   out its implicit link directories, which the loader searches anyway. (CMake puts the
#   directories of the linked libraries before the others in the installed binary.)
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
    # of the plug-in's load. Turn it off for sanitizer builds, whose runtime the host provides.
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
