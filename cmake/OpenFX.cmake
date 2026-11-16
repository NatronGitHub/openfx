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
#                  [EXPORT_SYMBOLS <symbol>...] [SOURCES <file>...])
#
# Builds <BUNDLE_NAME>.ofx (default: <target>) into
# ${OFX_PLUGIN_BUILD_DIR}/<BUNDLE_NAME>.ofx.bundle/Contents/<ARCHDIR>/, with the Info.plist in
# Contents/, and installs the bundle into PLUGIN_INSTALLDIR.
# - INFO_PLIST: the plug-in's own Info.plist, run through configure_file(@ONLY) (a file without
#   @VAR@ is copied unchanged). Default: the generic template, with IDENTIFIER (default
#   org.openeffects.plugins.<BUNDLE_NAME>) and VERSION (default 1.0.0).
# - Only OfxGetPlugin, OfxGetNumberOfPlugins and the EXPORT_SYMBOLS (e.g. OfxSetHost) are
#   exported: hidden visibility, plus an export list on macOS and a version script on Linux and
#   FreeBSD, as Support/Plugins/Makefile.master does (hidden visibility alone isn't enough for
#   plug-ins that link static libraries, e.g. OSMesa or LLVM).
# - Install rpath: @loader_path/../Frameworks and @loader_path/../Libraries on macOS,
#   $ORIGIN/../../Libraries on Linux and FreeBSD (libraries bundled by the installers).
# - DEBUG is defined in Debug builds; CXX_STANDARD is ${OFX_CXX_STANDARD}, without GNU
#   extensions; /bigobj (MSVC) or -Wa,-mbig-obj (MinGW gcc) for large translation units (not in
#   Makefile.master). The MinGW runtime DLLs are bundled by the installers, as with the Makefiles.
# - macOS: the installed binary is signed ad hoc again, since install_name_tool (which adds the
#   install rpath) invalidates the signature that Xcode builds have.
# Link the plug-in with openfx::Support (C++ Support library) or openfx::Api (C API only).
function(add_ofx_plugin TARGET)
  cmake_parse_arguments(PARSE_ARGV 1 OFX "" "DIR;BUNDLE_NAME;INFO_PLIST;IDENTIFIER;VERSION"
                        "EXPORT_SYMBOLS;SOURCES")
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
  if(TARGET openfx::Api)
    target_link_libraries(${TARGET} PRIVATE openfx::Api)
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
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_info}" "${_bundle}/Contents/Info.plist"
    VERBATIM)
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
  endif()

  # Install rpath, for the libraries that the installers put in the bundle
  if(APPLE)
    set_target_properties(${TARGET} PROPERTIES
      INSTALL_RPATH "@loader_path/../Frameworks;@loader_path/../Libraries")
  elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    set_target_properties(${TARGET} PROPERTIES INSTALL_RPATH "$ORIGIN/../../Libraries")
  endif()

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
  install(
    TARGETS ${TARGET}
    LIBRARY DESTINATION "${_dest}/${ARCHDIR}"
    RUNTIME DESTINATION "${_dest}/${ARCHDIR}"
    ARCHIVE DESTINATION "${_dest}/${ARCHDIR}" EXCLUDE_FROM_ALL)
  install(FILES "${_info}" DESTINATION "${_dest}" RENAME Info.plist)
  if(APPLE)
    if(IS_ABSOLUTE "${_dest}")
      set(_bin "${_dest}/${ARCHDIR}/${OFX_BUNDLE_NAME}.ofx")
    else()
      set(_bin "\${CMAKE_INSTALL_PREFIX}/${_dest}/${ARCHDIR}/${OFX_BUNDLE_NAME}.ofx")
    endif()
    install(CODE "execute_process(COMMAND codesign -f -s - \"\$ENV{DESTDIR}${_bin}\" OUTPUT_QUIET ERROR_QUIET)")
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
    install(FILES ${_files} DESTINATION "${_dest}")
  endif()
  foreach(_d IN LISTS _dirs)
    get_filename_component(_dname "${_d}" NAME)
    list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_d}" "${_bundle}/Contents/${_sub}/${_dname}")
    install(DIRECTORY "${_d}" DESTINATION "${_dest}")
  endforeach()
  add_custom_target(${_rt} ALL ${_cmds} VERBATIM)
  add_dependencies(${TARGET} ${_rt})
endfunction()

# Compile and link a target with OpenMP, if OFX_USE_OPENMP is ON:
#
#   ofx_target_openmp(<target> [REQUIRED])
#
# With Apple clang, which has no OpenMP runtime, it uses libomp from MacPorts
# (/opt/local/include/libomp, /opt/local/lib/libomp), or from OFX_LIBOMP_ROOT.
function(ofx_target_openmp TARGET)
  cmake_parse_arguments(PARSE_ARGV 1 OFX "REQUIRED" "" "")
  if(NOT OFX_USE_OPENMP)
    return()
  endif()
  if(TARGET OpenMP::OpenMP_CXX) # found by an earlier call
    target_link_libraries(${TARGET} PRIVATE OpenMP::OpenMP_CXX)
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
  elseif(OFX_REQUIRED)
    message(FATAL_ERROR "ofx_target_openmp(${TARGET}): OpenMP not found (MacPorts: port install libomp)")
  else()
    message(WARNING "ofx_target_openmp(${TARGET}): OpenMP not found, building without it (MacPorts: port install libomp; or set OFX_LIBOMP_ROOT, or OFX_USE_OPENMP=OFF)")
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
# Call it after the last ofx_plugin_resources() of the target: files installed into the bundle
# after it would break the bundle's signature. The install path must not contain spaces
# (osxDeploy.sh). Windows (bundling the DLLs, with an embedded manifest) and Linux are not
# implemented yet.
function(ofx_deploy_bundle TARGET)
  if(NOT OFX_DEPLOY_BUNDLES)
    return()
  endif()
  get_target_property(_name ${TARGET} OFX_BUNDLE_NAME)
  if(NOT _name)
    message(FATAL_ERROR "ofx_deploy_bundle: ${TARGET} was not created by add_ofx_plugin")
  endif()
  set_target_properties(${TARGET} PROPERTIES OFX_DEPLOYED TRUE)
  if(APPLE)
    if(PLUGIN_INSTALLDIR MATCHES " " OR CMAKE_INSTALL_PREFIX MATCHES " ")
      message(FATAL_ERROR "ofx_deploy_bundle(${TARGET}): the install path (PLUGIN_INSTALLDIR, CMAKE_INSTALL_PREFIX) must not contain spaces (osxDeploy.sh)")
    endif()
    _ofx_find_file(_deploy osxDeploy.sh)
    if(IS_ABSOLUTE "${PLUGIN_INSTALLDIR}")
      set(_bundle "${PLUGIN_INSTALLDIR}/${_name}.ofx.bundle")
    else()
      set(_bundle "\${CMAKE_INSTALL_PREFIX}/${PLUGIN_INSTALLDIR}/${_name}.ofx.bundle")
    endif()
    # release builds only, as in Makefile.master (or an empty build type with a single-config
    # generator)
    install(CODE "
     if(NOT CMAKE_INSTALL_CONFIG_NAME OR CMAKE_INSTALL_CONFIG_NAME MATCHES \"^(Release|RelWithDebInfo|MinSizeRel)\$\")
      set(_b \"\$ENV{DESTDIR}${_bundle}\")
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
      endforeach()
      execute_process(COMMAND codesign -f -s - \"\${_b}\" RESULT_VARIABLE _r)
      if(_r)
        message(FATAL_ERROR \"codesign failed on \${_b}\")
      endif()
      execute_process(COMMAND codesign --verify --strict \"\${_b}\" RESULT_VARIABLE _r)
      if(_r)
        message(FATAL_ERROR \"codesign --verify --strict failed on \${_b}\")
      endif()
     endif()
    ")
  endif()
endfunction()
