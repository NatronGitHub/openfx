# DRAFT: CMake for the plug-in repositories (Natron#1181)

Status: plan v1, implemented by openfx PR A (except the Windows part of `ofx_deploy_bundle`, which needs Windows CI and comes next). Agreed with the openfx-misc, openfx-io, openfx-arena, openfx-gmic, openfx-supportext and Natron sessions on 2026-11-16. The openfx repository is the reference for the CMake build of plug-ins. Each repository keeps its Makefiles until Natron's `tools/jenkins/build-plugins.sh` switches to CMake.

## Order

1. **openfx PR A:** the `cmake/OpenFX.cmake` API below. The Support plug-ins and the examples are built with it, and openfx's per-PR Linux and macOS CI moves to CMake (Windows already uses it).
2. **openfx PR B:** HostSupport usable by Natron's CMake build:
   - the defines as options, set before the sources compile: OFX_EXTENSIONS_NUKE/TUTTLE/VEGAS/NATRON/RESOLVE and OFX_SUPPORTS_PARAMETRIC/OPENGLRENDER/MULTITHREAD/DIALOG;
   - a static library with PIC;
   - expat bundled or from the system;
   - no `include_directories` leaking to the consumer.
3. **openfx-supportext** (openfx-supportext#45): `supportext_add_sources(<target> [ALL] [OPENGL] [OSMESA] [EGL] [COMPONENTS ...])` in `SupportExt/cmake/SupportExt.cmake`. The components are THREADS, FILEOPEN, LUT, MULTIPLANE, SHUTTER, TRANSFORM3X3, TRANSFORM_INTERACT, RECTANGLE_INTERACT, GENERATOR, TEXT, RAMP, TRACKING, OGL_FUNCTIONS and EGL, with their dependencies pulled in; OSMESA links `OSMesa::OSMesa`, which the consumer provides.
   - the sources are added PRIVATE to the plug-in target, so they compile with the plug-in's exact defines. OFX_EXTENSIONS_* and OFXS_HAVE_DRAWSUITE change class layouts, so a separately compiled library would break the one-definition rule;
   - it also adds the include directories, and links openfx::Support if that target exists.
   openfx-io provides the same pattern for IOSupport: `ioSupport_add_sources()` (GenericReader, GenericWriter, GenericOCIO, SequenceParsing), also used by openfx-arena.
4. **Each repository:**
   - a draft parity PR on a thin local wrapper that uses the names below;
   - when A merges, a rebase that removes the wrapper and switches per-PR CI to CMake, in the same PR;
   - a nightly Make build until build-plugins.sh switches.

**Done** for a repository means:
- its CMake install tree matches its Makefile bundles: bundle names, file list, Info.plist, Resources and exported symbols, checked by a bundle-diff script;
- its CI builds with CMake.

## API (cmake/OpenFX.cmake)

`add_ofx_plugin(<target> [DIR <d>] [BUNDLE_NAME <name>] [INFO_PLIST <file>] [IDENTIFIER <id>] [VERSION <x.y.z>] [EXPORT_SYMBOLS <symbol>...] [SOURCES ...])`
- The old form, `add_ofx_plugin(TARGET [DIR])`, keeps working.
- `INFO_PLIST`: the repository's own file. It's run through `configure_file(@ONLY)`, so a file without `@VAR@` is copied unchanged. Without it, the generic template is used with `IDENTIFIER` and `VERSION`.
- Always applied, as in `Support/Plugins/Makefile.master`:
  - hidden visibility, and an export list generated per target (macOS `-exported_symbols_list`, a Linux and FreeBSD version script), so that only OfxGetPlugin, OfxGetNumberOfPlugins and the `EXPORT_SYMBOLS` are exported. Hidden visibility alone isn't enough for bundles that statically link OSMesa or LLVM. OfxSetHost isn't in the default list, because a macOS export list that names a missing symbol is a link error: add `EXPORT_SYMBOLS OfxSetHost` if the plug-in defines it;
  - rpath: `@loader_path/../Frameworks` and `@loader_path/../Libraries` on macOS, `$ORIGIN/../../Libraries` on Linux;
  - `DEBUG` in Debug builds, `NDEBUG` otherwise (CMake's default);
  - `/bigobj` (MSVC) and `-Wa,-mbig-obj` (MinGW);
  - MinGW: `-static-libgcc -static-libstdc++`;
  - `CXX_STANDARD ${OFX_CXX_STANDARD}`.
- The bundle `<BUNDLE_NAME>.ofx.bundle/Contents/{Info.plist,Resources/,<ARCHDIR>/<BUNDLE_NAME>.ofx}` is assembled in `${OFX_PLUGIN_BUILD_DIR}`, so it's usable with `OFX_PLUGIN_PATH`, and is installed to `PLUGIN_INSTALLDIR`. A project may define several bundles.

`ofx_plugin_resources(<target> [DESTINATION <subdir>] <files...>)`
- Copies the files to `Contents/Resources[/<subdir>]`, at build and install time.
- It may be called many times, inside `if()` for conditional resources. For a configured resource, run `configure_file()` first and pass its output.

`ofx_target_openmp(<target> [REQUIRED])`
- Links OpenMP::OpenMP_CXX.
- With Apple clang it uses `-Xpreprocessor -fopenmp` and libomp from MacPorts (`/opt/local/include/libomp`, `/opt/local/lib/libomp`), or from `OFX_LIBOMP_ROOT`.
- Does nothing if `OFX_USE_OPENMP` is OFF.

`ofx_deploy_bundle(<target>)`
- An install-time step, enabled by `OFX_DEPLOY_BUNDLES`, that makes the bundle self-contained.
- **macOS:**
  - `osxDeploy.sh`: non-system dylibs go to `Contents/Libraries`, with `@rpath` ids;
  - then re-sign ad hoc, dylibs first, then the bundle (`codesign -f -s -`). Otherwise `codesign --verify --strict` fails, and arm64 requires the signature;
  - check that no Mach-O references `/opt/local`, Homebrew or `/usr/local` (`otool -L`, `otool -l`), and that every `@rpath/X` is in `Contents/Libraries`;
  - resolve the `@rpath` dependencies of the copied dylibs against their original LC_RPATHs before deleting those.
- **Windows:**
  - copying the DLLs next to the `.ofx` is not enough. Hosts call `LoadLibraryW(fullpath)` (`ofxhBinary.cpp`, and Natron), which doesn't search the plug-in's directory;
  - instead, embed an assembly manifest (resource ID 2) listing the bundled DLLs as `<file>` entries of a private assembly. That's Natron's `build-Windows-installer.sh` approach, with the Windows SDK's `mt.exe`;
  - strip the `.ofx` before running `mt.exe`;
  - the closure comes from `file(GET_RUNTIME_DEPENDENCIES)`, with PRE_EXCLUDE_REGEXES `api-ms-.*` and `ext-ms-.*` and POST_EXCLUDE_REGEXES `.*[Ss]ystem32.*`. DLL names are case-insensitive, and an unresolved name is an error. Don't use `ntldd -R`: it recurses into System32.
  - Load test: a host linked statically, run with only `C:\Windows\System32` and `C:\Windows` on PATH.
  - Known risk: a MinGW host may already have loaded its own libstdc++, libgomp or winpthread, which would be used instead of the bundled ones.
- **Linux:** the `$ORIGIN/../../Libraries` rpath; copying libraries is left to the Natron installer.

**openfx::Api** passes on these usage requirements, to Support and raw-C-API plug-ins alike:
- the OFX_EXTENSIONS_* and OFX_SUPPORTS_* defines (they change class layouts);
- MinGW: `_USE_MATH_DEFINES` and `__USE_MINGW_ANSI_STDIO=1`;
- MSVC: `/Zc:__cplusplus`.

**openfx::Support:** static, PIC, hidden visibility (upstream #278), with openfx::Api PUBLIC. G'MIC uses the raw C API, so it links openfx::Api only.

## How openfx tests it

- `.github/scripts/cmake-build-test.sh` (the per-PR Linux and macOS jobs): builds and installs openfx with CMake, then runs the hostDemo checks (`hostdemo-checks.sh`, shared with the Makefile build) on the CMake-built plug-ins.
- It then runs `cmake-plugin-project.sh`, which builds `cmake/tests/plugin-project` against the installed package (`find_package(openfx)`), as a plug-in repository would. It checks:
  - the bundle name, identifier, version and executable in the Info.plist;
  - an Info.plist used as-is;
  - Resources, with a subdirectory;
  - the exported symbols (exactly OfxGetPlugin and OfxGetNumberOfPlugins) and the rpath;
  - on macOS, deployment: libomp bundled, `codesign --verify --strict`, and nothing referenced outside the bundle.
- The Makefile builds run nightly (extended.yml).

## Patterns (documented, no helper)

- OpenGL: `target_link_libraries(t PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})`.
- OSMesa and LLVM: a repository option, `USE_OSMESA` (openfx-misc), with the OSMesa target that SupportExt's OSMESA component expects.
- pkg-config libraries: `pkg_check_modules(<P> REQUIRED IMPORTED_TARGET ...)`. An injected `-fopenmp`, as ImageMagick's .pc adds, is filtered out of the imported target's compile options.
- A per-file C++ standard (arena's ReadPDF needs C++20): a separate OBJECT library with its own `CXX_STANDARD`, linked into the plug-in.
- Download plus verify (G'MIC's sources): `file(DOWNLOAD ... EXPECTED_HASH SHA256=...)`, or FetchContent, then `execute_process` for the patches.
- Debug-only plug-ins: `if(CMAKE_BUILD_TYPE STREQUAL "Debug")`.
- Tests: `add_test()`, run when `BUILD_TESTING` is on.

## Conventions

- **Shared cache variables:**
  - `OFX_CXX_STANDARD` (default 17; Natron with Qt6 uses 20);
  - `OFX_USE_OPENMP` (ON);
  - `OFX_DEPLOY_BUNDLES` (OFF; Natron's builds set it ON);
  - `PLUGIN_INSTALLDIR`;
  - `OFX_LICENSE=GPL|COMMERCIAL`, only where it changes what's built (openfx-arena: no poppler when COMMERCIAL);
  - `BUILD_TESTING`.
- **Repository options** keep their Makefile variable names (USE_OSMESA, IM, AUDIO, RICHTEXT, LEGACYIM, MAGICK_PKG_CONFIG, FFMPEG_PKG_CONFIG_PATH, SEEXPR_HOME...), so build-plugins.sh maps them one to one.
- **CMake minimum:** 3.16 everywhere.

## Per-repository notes

- **openfx-misc:**
  - several bundles (Misc, CImg, Shadertoy, with optional OSMesa and LLVM);
  - debug-only Test and DebugProxy;
  - its own Info.plist per bundle;
  - resources from about 170 plug-in directories, plus the Shadertoy presets and shaders.
- **openfx-io:**
  - one IO bundle with optional dependency groups: OCIO, OIIO, OpenEXR/Imath, FFmpeg (with a selectable pkg-config path), LibRaw (needs OpenMP), SeExpr 2.11 (SeExpr 1 API, via SEEXPR_HOME), webp, tiff, openjp2, png;
  - a macOS check that OIIO and the plug-ins don't use two different libavcodec files with the same name;
  - tests through CTest (link tbbmalloc with oneTBB);
  - no LICENSE switch of its own.
- **openfx-arena:**
  - one Arena bundle of about 26 plug-ins, plus optional OCL and HaldCLUT bundles;
  - SupportExt and IOSupport sources;
  - about 50 resources, some conditional, plus a configured fonts.conf;
  - pkg-config dependencies: Magick++ variants, cairo, pango, librsvg, poppler (GPL only), sox and others;
  - static MinGW builds for non-Natron hosts;
  - the Natron DMG symlinks `Contents/Libraries`.
- **openfx-gmic:**
  - raw C API, one binary with 1205 effects;
  - its own Info.plist and GMIC.png;
  - the G'MIC source download, verified and patched;
  - a large translation unit (`/bigobj`), OpenMP, C++17;
  - a self-contained deployment (see `ofx_deploy_bundle`).
- **openfx-supportext:** link dependencies the helper resolves:
  - Generator needs RectangleInteract;
  - Transform3x3 needs Shutter;
  - Ramp and Tracking need OGLTextRenderer, which needs OGLFontData;
  - OGLUtilities and OGLFunctions_gl need glad.
