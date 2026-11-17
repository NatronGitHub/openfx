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

`add_ofx_plugin(<target> [DIR <d>] [BUNDLE_NAME <name>] [INFO_PLIST <file>] [IDENTIFIER <id>] [VERSION <x.y.z>] [EXPORT_SYMBOLS <symbol>...] [RPATH <dir>...] [SOURCES ...])`
- The old form, `add_ofx_plugin(TARGET [DIR])`, keeps working.
- `INFO_PLIST`: the repository's own file. It's run through `configure_file(@ONLY)`, so a file without `@VAR@` is copied unchanged. Without it, the generic template is used with `IDENTIFIER` and `VERSION`.
- Always applied, as in `Support/Plugins/Makefile.master`:
  - hidden visibility, and an export list generated per target (macOS `-exported_symbols_list`, a Linux and FreeBSD version script), so that only OfxGetPlugin, OfxGetNumberOfPlugins and the `EXPORT_SYMBOLS` are exported. Hidden visibility alone isn't enough for bundles that statically link OSMesa or LLVM. OfxSetHost isn't in the default list, because a macOS export list that names a missing symbol is a link error: add `EXPORT_SYMBOLS OfxSetHost` if the plug-in defines it;
  - install rpath: `@loader_path/../Frameworks` and `@loader_path/../Libraries` on macOS, `$ORIGIN/../../Libraries` on Linux; then the `RPATH` directories and `OFX_PLUGIN_RPATH`; then the directories of the linked libraries outside the project (`INSTALL_RPATH_USE_LINK_PATH`), as Makefile.master's `-Wl,-rpath`. On macOS, those into `/opt/local`, `/usr/local` and `/opt/homebrew` are deleted at install: their libraries have absolute install names, and Makefile.master's bundles only have the two `@loader_path` rpaths. The others stay, e.g. for libraries with `@rpath` install names in an SDK prefix, and so do the `RPATH` and `OFX_PLUGIN_RPATH` entries and a directory where an `@rpath` dependency of the plug-in is (e.g. a library installed into `/usr/local` with CMake's defaults). CMake puts the directories of the linked libraries first. A deployed bundle (`ofx_deploy_bundle`) keeps none of the `/opt/local`, `/usr/local` and `/opt/homebrew` rpaths, explicit ones included, and `osxDeploy.sh` doesn't bundle `@rpath` dependencies: give them absolute install names (e.g. `CMAKE_INSTALL_NAME_DIR`) for a self-contained bundle. The installers (osxDeploy.sh, Natron's Linux installer) find the libraries there, then rewrite the rpath. Unlike Makefile.master's list, it has no system directory (`/usr/lib64`, `/usr/lib/<triplet>`): CMake leaves out its implicit link directories, which the loader searches anyway, so a bundle diff against a Makefile build shows a different RUNPATH (openfx-io);
  - the installed binary has mode 755 (Debian's CMake would strip the execute bit of a module);
  - `OFX_OPTFLAG` is appended to the compile options of the release configurations;
  - `DEBUG` in Debug builds, `NDEBUG` otherwise (CMake's default);
  - `/bigobj` (MSVC) and `-Wa,-mbig-obj` (MinGW);
  - MinGW: `-static-libgcc -static-libstdc++`;
  - `CXX_STANDARD ${OFX_CXX_STANDARD}`.
- The bundle `<BUNDLE_NAME>.ofx.bundle/Contents/{Info.plist,Resources/,<ARCHDIR>/<BUNDLE_NAME>.ofx}` is assembled in `${OFX_PLUGIN_BUILD_DIR}`, so it's usable with `OFX_PLUGIN_PATH`, and is installed to `PLUGIN_INSTALLDIR`. A project may define several bundles.

`ofx_plugin_resources(<target> [DESTINATION <subdir>] <files...>)`
- Copies the files to `Contents/Resources[/<subdir>]`, at build and install time. A directory is copied with its contents, as `cp -r` in Makefile.master (Shadertoy presets).
- It may be called many times, inside `if()` for conditional resources. For a configured resource, run `configure_file()` first and pass its output.

`ofx_target_openmp(<target> [REQUIRED])`
- Links OpenMP::OpenMP_CXX.
- With Apple clang it uses `-Xpreprocessor -fopenmp` and libomp from MacPorts (`/opt/local/include/libomp`, `/opt/local/lib/libomp`), or from `OFX_LIBOMP_ROOT`.
- Does nothing if `OFX_USE_OPENMP` is OFF.
- MSYS2 (UCRT64): OpenMP needs the `mingw-w64-ucrt-x86_64-libgomp` package (`pacboy libgomp:p`). The plug-in then needs `libgomp-1.dll` at run time, even with `-static-libgcc -static-libstdc++`: the installer bundles it with the other MinGW DLLs.
- With Apple clang it sets `OpenMP_CXX_FLAGS`, `OpenMP_CXX_LIB_NAMES` and `OpenMP_omp_LIBRARY` as normal variables, which hide the cache entries of the same names: `-DOpenMP_CXX_FLAGS=...` has no effect there. `OFX_LIBOMP_ROOT` is the override.
- With Apple clang it also adds the directory of the libomp that OpenMP::OpenMP_CXX links to the target's link directories (PUBLIC for a static library), so that other dependencies built with OpenMP, e.g. a static ImageMagick whose `.pc` has `-lomp`, find it (arena).

`ofx_target_filter_missing_includes(<target>...)`
- Drops the include directories that don't exist from imported targets, and from the imported targets they link, recursively. Generation fails otherwise: e.g. Ubuntu 24.04's OpenImageIO config lists `/usr/include/opencv4` without libopencv-dev (openfx-io).
- Call it after `find_package()`. Entries with generator expressions are kept, and each dropped directory is reported once.

`ofx_deploy_bundle(<target>)`
- An install-time step, enabled by `OFX_DEPLOY_BUNDLES`, that makes the bundle self-contained.
- **macOS** (implemented):
  - `osxDeploy.sh`: non-system dylibs go to `Contents/Libraries`, with `@rpath` ids;
  - LC_RPATHs into `/opt/local`, `/usr/local` or `/opt/homebrew` are deleted from the bundled files (MacPorts' libomp has `/opt/local/lib/libomp`);
  - every file in `Libraries` and `Frameworks`, subfolders included, is re-signed ad hoc, then the bundle, and `codesign --verify --strict` must pass (arm64 requires a valid signature);
  - the install fails if a Mach-O file still references `/opt/local`, `/usr/local` or `/opt/homebrew` (`otool -L`) or has an LC_RPATH there (`otool -l`);
  - if `CMAKE_OSX_DEPLOYMENT_TARGET` is set: one warning per bundle lists the files with an architecture that requires a newer macOS (the minos of `LC_BUILD_VERSION` or `LC_VERSION_MIN_MACOSX`; arm64 is never below 11.0). `OFX_DEPLOY_STRICT_MINOS` makes it an error. MacPorts builds libraries for the build machine's macOS unless `macosx_deployment_target` is set in macports.conf;
  - CMake's own install-time rpath edit prints `install_name_tool: warning: changes being made to the file will invalidate the code signature`: harmless, the files are signed again afterwards;
  - the build-tree binary has the build rpath (the linked libraries' directories), not `@loader_path/../Libraries`: only the installed bundle is deployed;
  - call `ofx_deploy_bundle` after the last `ofx_plugin_resources` of the target (it's an error otherwise): files installed into the bundle after it would break the signature;
  - the install path may contain spaces, and `osxDeploy.sh` fails if a copy or an `install_name_tool` call fails;
  - the bundle gets an ad hoc signature (`Contents/_CodeSignature`, which the Makefile bundles don't have): an installer that later changes files in the bundle must sign it again (`codesign -f -s -`). Without it, `codesign --verify --strict` fails on the deployed bundle.
- **macOS, not implemented yet:** checking that every `@rpath/X` is in `Contents/Libraries`, and resolving the `@rpath` dependencies of copied dylibs against their original LC_RPATHs (MacPorts libraries use absolute install names, so this matters little today).
- **Windows** (not implemented yet; Natron's Windows installer does this itself, so Natron keeps `OFX_DEPLOY_BUNDLES` OFF there):
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

**openfx::Support:** static, PIC, built with `OFX_CXX_STANDARD`, with `Support/Plugins/include` on its include path (as in the Makefiles), and openfx::Api PUBLIC. Hidden visibility comes with openfx#124 (upstream #278); the export lists already restrict the exports. G'MIC uses the raw C API, so it links openfx::Api only.

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
- CI package lists: a `--static` pkg-config link can pull in more than the direct dependencies. E.g. libzip brings `-lssl -lcrypto`, so a bare Ubuntu 24.04 image needs `libssl-dev` (openfx-arena).

## Conventions

- **Shared cache variables:**
  - `OFX_CXX_STANDARD` (default 17; Natron uses 17 with Qt5 and 20 with Qt6, Natron#1148). It's applied with CXX_STANDARD to the plug-ins and to openfx::Support, so a `-std=` inside `CXX` would be overridden: pass the standard with this variable instead;
  - `OFX_USE_OPENMP` (ON);
  - `OFX_DEPLOY_BUNDLES` (OFF). Natron's builds set it ON on macOS only, where it replaces Makefile.master's osxDeploy step. On Windows it stays OFF: build-Windows-installer.sh strips the `.ofx`, copies the DLLs and embeds its own manifest;
  - `PLUGIN_INSTALLDIR` (default `OFX/Plugins`, relative to `CMAKE_INSTALL_PREFIX`: set the prefix to `/usr` or `/Library` for the standard OFX folders);
  - `OFX_OPTFLAG` (empty): optimization flags of the release configurations of the plug-ins and of openfx::Support, after CMake's (so `-O2` overrides Release's `-O3`). `ofx_target_optflag(<target>)` applies it to a repository's own libraries. An installed openfx::Support keeps the flags it was built with. Ignored with MSVC. The level is decided after the per-repository -O2/-O3 study;
  - `OFX_PLUGIN_RPATH` (empty): extra install rpath directories of every plug-in (ignored on Windows, like `RPATH`);
  - `OFX_LICENSE=GPL|COMMERCIAL`, only where it changes what's built (openfx-arena: no poppler when COMMERCIAL);
  - `BUILD_TESTING`.
- **Paths for build-plugins.sh** (CMake equivalents of the Makefile variables):
  - OSMESA_PATH and LLVM_PATH (or LLVM_CONFIG), and the choice of mangled OSMesa or GLU (openfx-misc);
  - OIIO_HOME and SEEXPR_HOME (openfx-io); FFmpeg and LibRaw through CMAKE_PREFIX_PATH or their pkg-config paths;
  - OPTFLAG becomes `OFX_OPTFLAG`. CMake's Release flags are `-O3 -DNDEBUG`, Makefile.master's are `-O2 -DNDEBUG`: pass `-DOFX_OPTFLAG=-O2` if a bundle diff needs identical code;
  - OFX_USE_OPENMP defaults to ON, which also enables OpenMP with Apple clang (the Makefiles don't): Natron passes it explicitly.
- **Repository options** keep their Makefile variable names (USE_OSMESA, IM, AUDIO, RICHTEXT, LEGACYIM, MAGICK_PKG_CONFIG, FFMPEG_PKG_CONFIG_PATH, SEEXPR_HOME...), so build-plugins.sh maps them one to one.
- **CMake minimum:** 3.16 everywhere. A project sets policy CMP0177 to NEW (normalized install paths, CMake 3.31) with `if(POLICY CMP0177)`, before `include(OpenFX)` or `find_package(openfx)`; OpenFX.cmake sets it too.

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
