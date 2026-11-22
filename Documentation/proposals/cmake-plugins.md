# CMake for the plug-in repositories (Natron#1181)

Status: plan v1, implemented: PR A is NatronGitHub/openfx#125 (with the follow-ups #127, #129), PR B is #139. Since then: `-z defs` and `OFX_PLUGIN_NO_UNDEFINED` (#157, #161), the Linux bundle check `linuxCheckBundle.sh` and `OFX_LINUX_CHECK_BUNDLE` (#159, #163), and the macOS 14.0 deployment target (#160). `ofx_deploy_bundle` is macOS only, by design (decided 2026-11-18): on Linux and Windows, plug-ins use system libraries, or header-only ones such as pocketfft, and installers such as Natron's deploy anything else (on Linux, into `<bundle>/Libraries`, checked by `linuxCheckBundle.sh`). Agreed with the openfx-misc, openfx-io, openfx-arena, openfx-gmic, openfx-supportext and Natron sessions on 2026-11-16. The openfx repository is the reference for the CMake build of plug-ins. Each repository keeps its Makefiles until Natron's `tools/jenkins/build-plugins.sh` switches to CMake.

## Order

1. **openfx PR A:** the `cmake/OpenFX.cmake` API below. The Support plug-ins and the examples are built with it, and openfx's per-PR Linux and macOS CI moves to CMake (Windows already uses it).
2. **openfx PR B:** HostSupport usable by Natron's CMake build. `openfx::HostSupport` (target `OfxHost`) is a static PIC library, with its include directories and definitions as PUBLIC usage requirements and expat linked PUBLIC (ofxhPluginCache.h includes expat.h). A host uses it with `add_subdirectory(openfx EXCLUDE_FROM_ALL)` (so that only what it links is built, not OfxSupport), setting the options first (CMP0077 is NEW, since CMake 3.13), and may add `add_library(HostSupport ALIAS OfxHost)` for its existing target names:
   - OFX_EXTENSIONS_NUKE/TUTTLE/VEGAS/NATRON/RESOLVE, and OFX_SUPPORTS_OPENGLRENDER/MULTITHREAD/DIALOG/PARAMETRIC. OFX_SUPPORTS_PARAMETRIC_V2 needs a V2 implementation of the parametric suite, which Natron's isn't. The options are also OfxApi INTERFACE definitions;
   - expat is the system one (CMake's FindEXPAT, `EXPAT::EXPAT`, or conan's `expat::expat`): openfx no longer bundles it;
   - with OFX_SUPPORTS_PARAMETRIC, the host provides `ofxhParametricParam.h` and its implementation (Natron: `libs/OpenFX_extensions`) and adds them with absolute paths: `target_sources(OfxHost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/<dir>/ofxhParametricParam.cpp)`, `target_include_directories(OfxHost PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/<dir>>)` (the generator expression keeps `install(EXPORT)` working);
   - nothing is installed and nothing is added to the parent's global include directories or compile options (OFX_INSTALL is OFF under add_subdirectory).
   Tested by `cmake/tests/host-project` (`.github/scripts/cmake-host-project.sh`, on Linux, macOS and Windows/MSYS2), which also builds the parametric recipe with a stub `ofxhParametricParam.cpp`.
3. **openfx-supportext** (openfx-supportext#45): `supportext_add_sources(<target> [ALL] [OPENGL] [OSMESA] [EGL] [COMPONENTS ...])` in `SupportExt/cmake/SupportExt.cmake`. The components are THREADS, FILEOPEN, LUT, MULTIPLANE, SHUTTER, TRANSFORM3X3, TRANSFORM_INTERACT, RECTANGLE_INTERACT, GENERATOR, TEXT, RAMP, TRACKING, OGL_FUNCTIONS and EGL, with their dependencies pulled in; OSMESA links `OSMesa::OSMesa`, which the consumer provides.
   - the sources are added PRIVATE to the plug-in target, so they compile with the plug-in's exact defines. OFX_EXTENSIONS_* and OFXS_HAVE_DRAWSUITE change class layouts, so a separately compiled library would break the one-definition rule;
   - it also adds the include directories, and links openfx::Support if that target exists.
   openfx-io provides the same pattern for IOSupport: `ioSupport_add_sources()` (GenericReader, GenericWriter, GenericOCIO, SequenceParsing), also used by openfx-arena.
4. **Each repository:**
   - a draft parity PR on a thin local wrapper that uses the names below;
   - when A merges, a rebase that removes the wrapper and switches per-PR CI to CMake, in the same PR;
   - a weekly Make build until build-plugins.sh switches.

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

`ofx_plugin_bundle_group(<group> ... PLUGIN <name> ... [PLUGIN <name> ...])`: combined or separate bundles, the CMake equivalent of `make nomulti`, with other bundle names (see **Names**). Generic: any repository, several groups per project, plug-ins in any directory, any number of plug-ins per bundle, and per-bundle Info.plist and resources. Combining bundles and `REGISTER` are for plug-ins built on the Support library: raw C-API plug-ins each define `OfxGetPlugin` and `OfxGetNumberOfPlugins`, so two of them can't share a binary.

```cmake
ofx_plugin_bundle_group(<group>
  [COMBINED_NAME <bundle>]            # default <group>; also the combined target and install component
  [COMBINED_ID <identifier>]          # CFBundleIdentifier of a generated combined Info.plist (default org.openeffects.plugins.<bundle>)
  [COMBINED_INFO_PLIST <file>]        # default: generated
  [COMBINED_RESOURCES <item>...]      # only in the combined bundle
  [COMBINED_SOURCES <file>...]        # only in the combined bundle
  [ID_PREFIX <prefix>]                # a plug-in's identifier, without ID: <prefix><name>
  [VERSION <x.y.z>]                   # generated Info.plists (default PROJECT_VERSION, else 1.0.0)
  [ALSO_SEPARATE <plugin>...]         # COMBINED mode: these also get their own bundle
  [NO_DEPLOY]                         # don't call ofx_deploy_bundle()
  [COMMON_SOURCES <file>...]          # helpers only (no self-registering factory): one OBJECT library, PIC, in every bundle
  [COMMON_LINK_LIBRARIES <lib>...] [COMMON_COMPILE_DEFINITIONS <def>...]
  [COMMON_COMPILE_OPTIONS <opt>...] [COMMON_INCLUDE_DIRECTORIES <dir>...]
  [COMMON_RPATH <dir>...]             # add_ofx_plugin(RPATH) of every bundle
  [COMMON_EXPORT_SYMBOLS <symbol>...] # add_ofx_plugin(EXPORT_SYMBOLS) of every bundle
  [OUT_TARGETS <var>]                 # all the bundle targets
  [OUT_COMMON_TARGET <var>]           # the OBJECT library of COMMON_SOURCES (<group>_common), if any
  PLUGIN <name>
    SOURCES <file>...
    [ID <identifier>]                 # names its separate bundle (default <ID_PREFIX><name>)
    [REGISTER <function>...]          # `void f(OFX::PluginFactoryArray&)`, for manual registration
    [RESOURCES <item>...] [INFO_PLIST <file>]
    [LINK_LIBRARIES <lib>...] [COMPILE_DEFINITIONS <def>...] [COMPILE_OPTIONS <opt>...] [INCLUDE_DIRECTORIES <dir>...]
    [OPENMP]
    [COMBINED_ONLY | SEPARATE_ONLY]
    [NO_ID_CHECK]                     # no warning about the ID (see Registration)
    [PROPERTIES <property> <value>...]...  # custom properties, appended to the targets that contain the plug-in
    [OUT_TARGET <var>]                # the targets that contain the plug-in
  [PLUGIN <name> ...]...)
```

- **Mode:** the cache variable `OFX_PLUGIN_BUNDLES`, `COMBINED` (default) or `SEPARATE`; the variable `OFX_PLUGIN_BUNDLES_<group>` overrides it for one group.
  - `COMBINED`: one `add_ofx_plugin(<COMBINED_NAME>)` with every plug-in except the `SEPARATE_ONLY` ones, plus `COMBINED_SOURCES` and `COMBINED_RESOURCES`. The `ALSO_SEPARATE` and `SEPARATE_ONLY` plug-ins also get their own bundle.
  - `SEPARATE`: one `add_ofx_plugin(<name>)` per plug-in, except the `COMBINED_ONLY` ones (e.g. a debug-only test plug-in), which still go into a combined bundle (with `COMBINED_SOURCES` and `COMBINED_RESOURCES`), so that no plug-in is lost.
  - `ALSO_SEPARATE` plug-ins are in both bundles. This is for separate downloads: openfx-misc ships Misc.ofx.bundle and a standalone DenoiseSharpen, and a user installs one or the other. The plug-in's OFX identifier is then in two bundles, and a host must not load both. A host that finds both keeps one: HostSupport keeps the higher version, and for the same version the bundle in the earlier `OFX_PLUGIN_PATH` entry; within one folder, the choice is unspecified. The helper gives no error or warning.
- **Names:**
  - a separate bundle is named after its plug-in's identifier, `ID` or `<ID_PREFIX><name>`: `net.sf.openfx.Merge.ofx.bundle`, with the binary `net.sf.openfx.Merge.ofx`. Two packages then can't overwrite each other's bundles in a shared folder such as `/usr/OFX/Plugins`;
  - these names differ from `make nomulti`'s, which are `<Dir>.ofx.bundle` (`Merge.ofx.bundle`). The standalone bundle of an `ALSO_SEPARATE` plug-in is ID-named too (`net.sf.openfx.DenoiseSharpen.ofx.bundle`). A packager or user who still has an old Makefile bundle installed (`DenoiseSharpen.ofx.bundle`) gets the same plug-in twice: remove the old one;
  - an entry that registers several factories is named after that entry's ID, not after one of its factories: openfx-misc's Merge registers 33;
  - `ID` or `ID_PREFIX` is required, except for `COMBINED_ONLY` plug-ins. A missing one is a configure error in both modes, so that the SEPARATE configuration always works;
  - names and identifiers must match `[A-Za-z0-9_.+-]+`.
- **Targets and install components:**
  - a separate bundle's target and install component are the short PLUGIN `<name>` (e.g. `cmake --install build --component DenoiseSharpen`), the combined bundle's are `<COMBINED_NAME>`;
  - the dotted bundle name is the target's `OUTPUT_NAME` and `OFX_BUNDLE_NAME`;
  - a group with one plug-in may use the plug-in's name as the group name (e.g. `Shadertoy`): the target is then `Shadertoy` in both modes. A `COMBINED_NAME` that is also the target of a plug-in's separate bundle in either mode (the plug-in is `ALSO_SEPARATE` or `SEPARATE_ONLY`, or the group has `COMBINED_ONLY` plug-ins) is an error in both modes;
  - also errors: a target name that already exists, a bundle name made by another group (compared without case), two PLUGINs with the same name or identifier, a PLUGIN without SOURCES, and an unknown `ALSO_SEPARATE` name.
- **Registration:**
  - Support plug-ins register their factories with `mRegisterPluginFactoryInstance` (a static `OFX::PluginFactories` object), and the Support library's own `OFX::Plugin::getPluginIDs` does nothing. Linking a plug-in's sources into a bundle is then enough, in both modes, with no generated file. Every plug-in of openfx-misc, openfx-io and openfx-arena works this way.
  - `REGISTER <function>...` is for plug-ins that register by hand: functions `void f(OFX::PluginFactoryArray &ids)` that `push_back` their factories (namespaces allowed: `MyNS::getMyPluginIDs`). For each bundle that has such plug-ins, a generated source calls their functions in declaration order, at static initialization, into the Support library's automatic registration (`OFX::PluginFactories::plugIDs()`, the list that `mRegisterPluginFactoryInstance` fills).
  - The generated source doesn't define `OFX::Plugin::getPluginIDs`, which the Support library already defines (the CMake build never uses `OFXS_MANUAL_REGISTRATION`). So it needs `openfx::Support`, and works with the installed library as it is. `REGISTER OFX::Plugin::getPluginIDs` is an error.
  - REGISTER and self-registering plug-ins may be mixed in one bundle. The order between the two kinds is the static initialization order, which is unspecified; hosts don't depend on it.
  - A plug-in source that defines `getPluginIDs` (the old manual registration) gives a configure warning, since it would be a duplicate symbol with the Support library's: rename the function and pass it to `REGISTER`. There is no warning when the plug-in has a `REGISTER` function of that name in another namespace (`MyNS::getPluginIDs`).
  - An explicit `ID` with which no string literal of the plug-in's sources, or of the headers in their directories (e.g. openfx-misc's `CImg/Erode/CImgErodeDilate.h`), starts gives a warning. `NO_ID_CHECK` skips it for that plug-in. `"net.sf.openfx.MergePlugin"` matches the ID `net.sf.openfx.Merge`. Both checks are best-effort text searches.
- **Sources:**
  - a plug-in's sources are compiled into each bundle target that contains it, never through a static archive, whose unreferenced objects (and so the static registrations) the linker would drop;
  - a source listed by several plug-ins is added once to a combined bundle, e.g. openfx-misc's `CImg/CImgFilter.cpp`, listed by every CImg entry;
  - `COMMON_SOURCES` go into one OBJECT library, `<group>_common`, linked into every bundle of the group. It is PIC, has the compile settings of `add_ofx_plugin`, and links `COMMON_LINK_LIBRARIES` PUBLIC.
- **COMMON_SOURCES pitfalls:**
  - they must not register factories, since they are in every bundle;
  - they are compiled without OpenMP. If they need it, call `ofx_target_openmp()` on `OUT_COMMON_TARGET`; every bundle then needs OpenMP too;
  - never call `supportext_add_sources()` (or `ioSupport_add_sources()`) on the common target: each bundle adds its own SupportExt sources, so they would be linked twice (duplicate symbols);
  - they get no per-plug-in flags. A helper that must be compiled with a plug-in's definitions (e.g. `OFX_EXTENSIONS_*`, or a feature macro that changes a class layout) would break the one-definition rule there: list it in that plug-in's `SOURCES` instead (it is then compiled once per bundle).
- **Compile and link settings:**
  - `COMMON_*` apply to every bundle and to the common objects. `COMMON_RPATH` and `COMMON_EXPORT_SYMBOLS` are `add_ofx_plugin`'s `RPATH` and `EXPORT_SYMBOLS` for every bundle, e.g. openfx-io's in-tree SeExpr directory;
  - a plug-in's `LINK_LIBRARIES` and `INCLUDE_DIRECTORIES` apply to the bundles that contain it (a combined bundle gets the union, each once). So a source shared by plug-ins, such as `CImgFilter.cpp` or `randomGenerator.cpp`, sees all of them. It also means that a combined bundle searches every plug-in's include directories: header names must be unique across them, or the combined build silently uses the first plug-in's header (e.g. two `Common.h`, one in each plug-in's directory). The separate builds, which see one plug-in's directories each, won't show it;
  - a plug-in's `COMPILE_DEFINITIONS` and `COMPILE_OPTIONS` apply to its own `SOURCES` only, in both modes, as source file properties:
    - other targets of the same directory that compile these files see them too;
    - sources added to the target later (e.g. by `supportext_add_sources`) don't;
    - a source listed by several plug-ins gets them once, and these plug-ins must give it the same ones (an error otherwise);
    - a generator-expression source gets none (a warning);
    - with the Xcode generator, per-source settings are per file of the whole project, and per-source options that Xcode can't express per file may be ignored. Prefer `COMMON_COMPILE_DEFINITIONS` there;
  - `OPENMP`: `ofx_target_openmp()` on the bundles that contain the plug-in (a combined bundle, if any of its plug-ins has it);
  - the helper links through the `LINK_LIBRARIES` property, so the caller may use either form of `target_link_libraries` on the targets.
- **Resources:**
  - `RESOURCES` and `COMBINED_RESOURCES` items are:
    - files;
    - directories, copied with their contents under their own name: Shadertoy's `presets` gives `Contents/Resources/presets/default/*.frag.glsl`;
    - `<file-or-directory> DESTINATION <subdir>`, under `Contents/Resources/<subdir>`;
  - nothing goes outside `Contents/Resources`: an absolute `DESTINATION`, or one with `..`, is an error. There is no localisation;
  - relative paths are relative to the calling directory. The items must exist at configure time: configure a generated file first;
  - they are installed with `ofx_plugin_resources()`;
  - a combined bundle gets the union of its plug-ins' resources and `COMBINED_RESOURCES`. An item listed by several plug-ins is copied once. Two different files with the same destination are an error, and destinations are compared without case (case-insensitive file systems on macOS and Windows).
- **Info.plist:**
  - a separate bundle's Info.plist is generated, with `CFBundleExecutable` `<ID>.ofx`, `CFBundleIdentifier` `<ID>` and `CFBundleVersion` `VERSION` (default `PROJECT_VERSION`, else 1.0.0);
  - a plug-in's `INFO_PLIST` is used only if, after `configure_file(@ONLY)`, its `CFBundleExecutable` and `CFBundleIdentifier` are these: use `@PLUGIN_EXE@` and `@BUNDLE_IDENTIFIER@`;
  - otherwise, e.g. with the Makefiles' per-directory plists that hard-code `<Dir>.ofx`, a warning says so and the generated one is used. The check runs in both modes, so a COMBINED build warns too;
  - a combined bundle's Info.plist is `COMBINED_INFO_PLIST`, or generated with `COMBINED_ID`. `COMBINED_INFO_PLIST`'s `CFBundleExecutable` must be `<COMBINED_NAME>.ofx` after `configure_file(@ONLY)`, e.g. openfx-misc's `Misc/Info.plist`; it's an error otherwise.
- **Target properties:**
  - every bundle target gets `OFX_BUNDLE_PLUGINS` (the PLUGIN names in that bundle, in declaration order) and `OFX_BUNDLE_GROUP`;
  - each `PROPERTIES <property> <value>...` of a plug-in is appended to the targets that contain it, with duplicates removed. A combined bundle gets the union, so the caller can derive per-bundle settings, e.g. the SupportExt components to add;
  - only custom properties with a project prefix are allowed, e.g. `OFXMISC_SUPPORTEXT`. These names are errors: a keyword of the function, an `OFX_*` name, a property that the target already has after `add_ofx_plugin` (`CXX_STANDARD`, `OUTPUT_NAME`...), and `SUPPORTEXT_COMPONENTS` and `IOSUPPORT_COMPONENTS`;
  - never reuse a helper's own bookkeeping property. `supportext_add_sources()` records the components it has added in `SUPPORTEXT_COMPONENTS`, and `ioSupport_add_sources()` in `IOSUPPORT_COMPONENTS`, and both skip the components already listed there. Setting such a property beforehand silently drops every source of these components.
- **Deployment and signing order:** `ofx_deploy_bundle()` is called on each bundle after its resources (macOS signs the installed bundle). With `NO_DEPLOY`, the caller adds its own resources first (e.g. generated license files, with `ofx_plugin_resources()`), then calls `ofx_deploy_bundle()` on each target of `OUT_TARGETS`.
- **Install layout:** the same as `add_ofx_plugin`.
- **OUT_TARGET** is a list: the combined target, plus the separate one for an `ALSO_SEPARATE` plug-in (one target otherwise). Use `foreach()` on it, or on `OUT_TARGETS`, to add per-bundle sources.
- **Parsing:** the arguments are split at each `PLUGIN` keyword, a property value can't be one of the PLUGIN keywords, and empty arguments are dropped.

Migration example (openfx-misc, abridged; see its `CMakeLists.txt` for the full lists):

```cmake
# Misc.ofx.bundle (Misc/Makefile). randomGenerator.cpp is used by several plug-ins: it is a
# COMMON_SOURCE (no factory). TestRender is only in the combined bundle (and compiled out unless
# DEBUG).
ofx_plugin_bundle_group(Misc
  COMBINED_INFO_PLIST Misc/Info.plist           # CFBundleExecutable Misc.ofx
  COMBINED_SOURCES Test/TestRender.cpp
  ID_PREFIX net.sf.openfx.
  ALSO_SEPARATE DenoiseSharpen                  # Misc.ofx.bundle, and net.sf.openfx.DenoiseSharpen.ofx.bundle
  COMMON_SOURCES Misc/randomGenerator.cpp
  COMMON_LINK_LIBRARIES openfx::Support
  COMMON_INCLUDE_DIRECTORIES Misc ${OFXMISC_GLEXT_INCLUDE_DIR}
  OUT_TARGETS _misc_targets
  PLUGIN Merge                                  # 33 factories: net.sf.openfx.Merge.ofx.bundle
    SOURCES Merge/Merge.cpp
    RESOURCES Merge/net.sf.openfx.MergePlugin.png Merge/net.sf.openfx.MergePlugin.svg
              Merge/net.sf.openfx.MergePlus.png # ... one icon per operator
    INCLUDE_DIRECTORIES Merge
    PROPERTIES OFXMISC_SUPPORTEXT MULTIPLANE    # not SUPPORTEXT_COMPONENTS: see Target properties
  PLUGIN DenoiseSharpen
    SOURCES DenoiseSharpen/DenoiseSharpen.cpp
    INCLUDE_DIRECTORIES DenoiseSharpen
    OPENMP
    PROPERTIES OFXMISC_SUPPORTEXT MULTIPLANE THREADS
  # PLUGIN ... (every directory of Misc/Makefile)
  )
foreach(_t IN LISTS _misc_targets)
  get_target_property(_components ${_t} OFXMISC_SUPPORTEXT)
  supportext_add_sources(${_t} COMPONENTS ${_components})
endforeach()

# CImg.ofx.bundle (CImg/Makefile): CImgFilter.cpp is listed by each plug-in (added once to the
# combined bundle); OpenMP as CImg/Makefile's OPENMP=1, with cimg_use_openmp only if it is found
if(BUILD_CIMG_PLUGINS)
  set(_cimg_omp "")
  if(NOT MSVC) # MSVC has OpenMP 2.0 only
    set(_cimg_omp OPENMP)
  endif()
  ofx_plugin_bundle_group(CImg
    COMBINED_INFO_PLIST CImg/Info.plist
    ID_PREFIX net.sf.cimg.CImg                  # PLUGIN Blur: net.sf.cimg.CImgBlur.ofx.bundle
    COMMON_LINK_LIBRARIES openfx::Support
    COMMON_INCLUDE_DIRECTORIES CImg
    COMMON_COMPILE_DEFINITIONS "$<$<AND:$<BOOL:${OFX_USE_OPENMP}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<TARGET_EXISTS:OpenMP::OpenMP_CXX>>:cimg_use_openmp>"
    OUT_TARGETS _cimg_targets
    PLUGIN Blur                                 # 14 factories (CImgBlur, CImgLaplacian, CImgSharpen...)
      SOURCES CImg/Blur/CImgBlur.cpp CImg/CImgFilter.cpp
      RESOURCES CImg/Blur/net.sf.cimg.CImgBlur.png CImg/Blur/net.sf.cimg.CImgBlur.svg # ...
      ${_cimg_omp}
    PLUGIN Erode                                # CImgErode and CImgDilate
      SOURCES CImg/Erode/CImgErode.cpp CImg/Erode/CImgDilate.cpp CImg/CImgFilter.cpp
      RESOURCES CImg/Erode/net.sf.cimg.CImgErode.png CImg/Erode/net.sf.cimg.CImgErode.svg
                CImg/Erode/net.sf.cimg.CImgDilate.png CImg/Erode/net.sf.cimg.CImgDilate.svg
      ${_cimg_omp}
    # PLUGIN ... (every directory of CImg/Makefile)
    )
  foreach(_t IN LISTS _cimg_targets)
    supportext_add_sources(${_t} COMPONENTS THREADS LUT)
  endforeach()
endif()

# Shadertoy.ofx.bundle (Shadertoy/Makefile): a group of one plug-in, so the target is Shadertoy
# in both modes (net.sf.openfx.Shadertoy.ofx.bundle in SEPARATE mode). NO_DEPLOY: OSMesa is set
# up on the target first.
if(BUILD_SHADERTOY)
  ofx_plugin_bundle_group(Shadertoy
    COMBINED_INFO_PLIST Shadertoy/Info.plist
    NO_DEPLOY
    COMMON_LINK_LIBRARIES openfx::Support OpenGL::GL ${CMAKE_DL_LIBS}
    COMMON_INCLUDE_DIRECTORIES ${OFXMISC_GLEXT_INCLUDE_DIR}
    PLUGIN Shadertoy
      ID net.sf.openfx.Shadertoy
      SOURCES Shadertoy/Shadertoy.cpp Shadertoy/ShadertoyParse.cpp
              Shadertoy/ShadertoyRenderGL.cpp Shadertoy/ShadertoyRenderMesa.cpp
      RESOURCES Shadertoy/net.sf.openfx.Shadertoy.png Shadertoy/net.sf.openfx.Shadertoy.svg
                Shadertoy/presets               # Contents/Resources/presets/default/*.frag.glsl
      OUT_TARGET _shadertoy)
  supportext_add_sources(${_shadertoy} COMPONENTS THREADS)
  if(USE_OSMESA)
    target_compile_definitions(${_shadertoy} PRIVATE HAVE_OSMESA)
    set_property(SOURCE Shadertoy/ShadertoyRenderMesa.cpp APPEND PROPERTY INCLUDE_DIRECTORIES ${_osmesa_includes})
    target_link_libraries(${_shadertoy} PRIVATE OSMesa::OSMesa)
  endif()
  ofx_deploy_bundle(${_shadertoy})
endif()

# Test.ofx.bundle (Test/Makefile, debug builds): one bundle that is never split, so it stays an
# add_ofx_plugin(Test INFO_PLIST Test/Info.plist SOURCES Test/TestRender.cpp Test/TestPosition.cpp ...)
```

`cmake -DOFX_PLUGIN_BUNDLES=SEPARATE` then gives `net.sf.openfx.Merge.ofx.bundle`, `net.sf.openfx.DenoiseSharpen.ofx.bundle`, `net.sf.cimg.CImgBlur.ofx.bundle`, `net.sf.cimg.CImgErode.ofx.bundle`, `net.sf.openfx.Shadertoy.ofx.bundle`... (no `Misc.ofx.bundle`, since Misc has no `COMBINED_ONLY` plug-in; `COMBINED_SOURCES` such as TestRender are then not built). `cmake --install build --component DenoiseSharpen` installs one bundle; in COMBINED mode that is the standalone `net.sf.openfx.DenoiseSharpen.ofx.bundle`, next to `Misc.ofx.bundle` (component `Misc`).

**macOS deployment target**
- Every plug-in bundle targets macOS 14.0 (VFX Reference Platform CY2027). CMake reads `CMAKE_OSX_DEPLOYMENT_TARGET` at the first `project()`, so the plug-in project sets it before that: `set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version")`, or `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0`, or the `MACOSX_DEPLOYMENT_TARGET` environment variable. `cmake/OpenFX.cmake` warns if it is empty or newer than 14.0. The plug-in Makefiles (`Makefile.master`) default to 14.0.

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
- **Linux and FreeBSD:** no deployment step. The install rpath is `$ORIGIN/../../Libraries`, i.e. `<bundle>.ofx.bundle/Libraries`; copying third-party libraries there is left to the installers (Natron's installer puts symlinks to its OIIO, OCIO, FFmpeg... there). Release bundles never carry the system runtimes (glibc, libstdc++, libgcc_s, libgomp, libatomic, GLVND libGL, libdrm, Mesa's libgbm and libglapi, libudev, libselinux, and the GPU drivers' libcuda, libnvidia-* and OpenCL vendor ICDs; the OpenCL ICD loader libOpenCL.so.1 is not one of them), and every binary's symbol versions must exist on the baseline system, EL9 (Rocky Linux 9, VFX CY2027); a plug-in that needs newer symbol versions must stop using them, or link that code statically.
- **Linux bundle check:** `linuxCheckBundle.sh` (`Support/include/`, installed next to `OpenFX.cmake`; its path is `OFX_LINUX_CHECK_BUNDLE`) checks installed Linux bundles: `"${OFX_LINUX_CHECK_BUNDLE}" [-v] [--no-needed] <bundle.ofx.bundle | file | directory>...`. It needs bash, readelf (binutils) and GNU coreutils; its header has the details. Run it in the plug-in repositories' CI and in the installers, after the libraries are bundled. Run it on the tree that is shipped (the `cmake --install` output, or the uploaded archive), never on CMake's build tree, and upload or package only the installed tree: the build tree is not the bundle that users get. Without `BUILD_WITH_INSTALL_RPATH`, CMake gives a build-tree ELF binary its build rpath, padded with `:` to the length of the install rpath so that `cmake --install` can rewrite it in place, and those empty RUNPATH entries mean the current directory to the loader; a build tree may also lack files that only the install puts in the bundle (openfx-gmic's build-tree Linux artifact had no Info.plist, openfx-gmic#119). `add_ofx_plugin` avoids the padding (`OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH`), but the rule stays.
  - it checks every ELF file of the bundle (`Contents/Linux-*/`, which must have one, `Libraries/`, `Contents/Libraries/`, subdirectories included), and the ELF files that symlinks in the bundle point to;
  - system libraries: a file with such a name or soname anywhere in the bundle is an error, versioned or not (e.g. `libstdc++.so`): glibc's (libc, libm, libpthread, libdl, librt, libutil, libresolv, libmvec, libanl, libthread_db, ld-linux), libcrypt, libstdc++, libgcc_s, libgomp, libatomic, libGL, libGLX, libEGL, libGLdispatch, libOpenGL, libdrm and libdrm_*, libgbm, libglapi, libudev, libselinux, libcuda, libnvidia-* and libnvcuvid, and the OpenCL vendor ICDs (libnvidia-opencl, libamdocl*, libigdrcl, libintelocl, libMesaOpenCL, libRusticlOpenCL, libpocl), plus the sonames in `OFX_ABI_SYSTEM_LIBS`. The OpenCL ICD loader, `libOpenCL.so.1` (ocl-icd or the Khronos OpenCL-ICD-Loader), is not a system library: no default EL9 or Ubuntu install has it, and it loads the vendor ICDs itself (`/etc/OpenCL/vendors/*.icd`), so the installers bundle it, or list it in `OFX_ABI_OPTIONAL_LIBS`. libcrypt is EL9's libxcrypt (`libcrypt.so.2`, in the minimal install), tied to the system's password hashing configuration: it is a system library, and a need on `libcrypt.so.1` (Debian, Ubuntu) is a warning, since EL9 only has it in libxcrypt-compat;
  - optional libraries may be bundled or come from the system: `OFX_ABI_OPTIONAL_LIBS`, default `libz.so.1`;
  - every other `DT_NEEDED` library must be bundled: it must exist (symlinks are followed) in one of the file's `DT_RUNPATH` (or `DT_RPATH`) directories inside the bundle, `$ORIGIN` being the file's own directory. So `$ORIGIN/../../Libraries` from `Contents/Linux-*/` finds `<bundle>/Libraries`, `$ORIGIN` finds a library next to the plug-in (openfx-io's test bundles), and a bundled library that needs another one must have its own rpath. `--no-needed` (or `OFX_ABI_CHECK_NEEDED=0`) skips this rule, for CI builds against the distribution's libraries;
  - `DT_RUNPATH` and `DT_RPATH` entries must start with `$ORIGIN` (`DT_RPATH` is ignored when `DT_RUNPATH` exists, as the loader does), without `$LIB` or `$PLATFORM`: an absolute entry (a build-machine path such as `/opt/rh/gcc-toolset-14/...` or `/__w/...`, an SDK prefix added by CMake's `INSTALL_RPATH_USE_LINK_PATH`, an `add_ofx_plugin(RPATH)` or `OFX_PLUGIN_RPATH` entry, or a Makefile's `-Wl,-rpath,<libdir>`), a relative one or an empty entry in a list is an error, unless listed in `OFX_ABI_RPATH_ALLOW`. An empty value, which the loader ignores, and an `$ORIGIN` entry that leads outside the bundle are warnings;
  - symbol versions (`readelf -V`, the versions each file needs) must be at most the baseline's. The defaults are EL9's, measured on rockylinux:9 (glibc 2.34-290.el9_9, GCC 11.5.0): `OFX_ABI_GLIBC` 2.34, `OFX_ABI_GLIBCXX` 3.4.29, `OFX_ABI_CXXABI` 1.3.13, `OFX_ABI_GCC` 7.0.0 for x86_64 files and 11.0 for aarch64 files, `OFX_ABI_GOMP` 5.0.1 (GOMP_ and OMP_), `OFX_ABI_LIBATOMIC` 1.2, `OFX_ABI_ZLIB` 1.2.9. 99 disables a limit. `GLIBC_PRIVATE` and `GLIBC_ABI_*` (e.g. `GLIBC_ABI_DT_RELR`, from `-z pack-relative-relocs`) are errors, except the names in `OFX_ABI_GLIBC_ABI_ALLOW`;
  - a version need of a checked library without a number (e.g. `GLIBC_FOO`, a bare `ZLIB`) is an error, except libstdc++'s `CXXABI_FLOAT128`;
  - GLIBC_2.35: an error by default. EL9 backported a few GLIBC_2.35 symbols, but other glibc 2.34 systems don't have them. `OFX_ABI_GLIBC_EXTRA` lists the symbols allowed at exactly GLIBC_2.35; EL9's are `__memcmpeq epoll_pwait2 __rseq_flags __rseq_offset __rseq_size _dl_find_object posix_spawn_file_actions_addtcsetpgrp_np`;
  - needing `libOpenGL.so.0` is a warning: EL9 has it in libglvnd-opengl, which isn't always installed. Link `libGL.so.1` instead, as the Makefiles do (`-lGL`). CMake's `OpenGL::GL` is `libOpenGL.so.0` (GLVND, the default since CMP0072), unless `OpenGL_GL_PREFERENCE` is `LEGACY`: openfx's own CMake-built examples get the warning today;
  - errors: a directory argument with no bundle, a bundle with no ELF file in `Contents/Linux-*/`, and a truncated or unreadable ELF file. A file argument inside a bundle is checked with that bundle as its context;
  - exit status: 0 if there is no problem, 1 if there are problems (each is printed, then their number), 2 on a usage error or a missing tool.
- **Install components:** each plug-in's install rules (binary, Info.plist, resources, deployment) are in a component named after its target, so `cmake --install <build> --component <target>` installs one bundle.
- **macOS, not implemented yet:** checking that every `@rpath/X` is in `Contents/Libraries`, and resolving the `@rpath` dependencies of copied dylibs against their original LC_RPATHs (MacPorts libraries use absolute install names, so this matters little today).
- **Windows** (not implemented yet; Natron's Windows installer does this itself, so Natron keeps `OFX_DEPLOY_BUNDLES` OFF there):
  - copying the DLLs next to the `.ofx` is not enough. Hosts call `LoadLibraryW(fullpath)` (`ofxhBinary.cpp`, and Natron), which doesn't search the plug-in's directory;
  - instead, embed an assembly manifest (resource ID 2) listing the bundled DLLs as `<file>` entries of a private assembly. That's Natron's `build-Windows-installer.sh` approach, with the Windows SDK's `mt.exe`;
  - strip the `.ofx` before running `mt.exe`;
  - the closure comes from `file(GET_RUNTIME_DEPENDENCIES)`, with PRE_EXCLUDE_REGEXES `api-ms-.*` and `ext-ms-.*` and POST_EXCLUDE_REGEXES `.*[Ss]ystem32.*`. DLL names are case-insensitive, and an unresolved name is an error. Don't use `ntldd -R`: it recurses into System32.
  - Load test: a host linked statically, run with only `C:\Windows\System32` and `C:\Windows` on PATH.
  - Known risk: a MinGW host may already have loaded its own libstdc++, libgomp or winpthread, which would be used instead of the bundled ones.
- **Linux:** the `$ORIGIN/../../Libraries` rpath; copying libraries is left to the Natron installer, and `linuxCheckBundle.sh` checks the result.

**openfx::Api** passes on these usage requirements, to Support and raw-C-API plug-ins alike:
- the OFX_EXTENSIONS_* and OFX_SUPPORTS_* defines (they change class layouts);
- MinGW: `_USE_MATH_DEFINES` and `__USE_MINGW_ANSI_STDIO=1`;
- MSVC: `/Zc:__cplusplus`.

**openfx::Support:** static, PIC, built with `OFX_CXX_STANDARD`, with `Support/Plugins/include` on its include path (as in the Makefiles), and openfx::Api PUBLIC. It links `CMAKE_DL_LIBS`, not OpenGL: the OpenGL backend of `DrawArgs::getDrawContext()` (`ofxsDrawContextOpenGL.cpp`, #165, #171) loads OpenGL at run time, and only the plug-ins that call `getDrawContext()` or `isOpenGLContextCurrent()` (e.g. through SupportExt's `OverlayDraw`) pull that object from the static library. It needs no OpenGL header (it draws with `include/ofxDrawSuiteOpenGL.h`, which declares the OpenGL types and constants it uses), so openfx::Support doesn't look for OpenGL; `OFX_SUPPORT_DRAWCONTEXT_OPENGL=OFF` compiles it with `OFXS_DRAWCONTEXT_NO_OPENGL`. Hidden visibility comes with openfx#124 (upstream #278); the export lists already restrict the exports. G'MIC uses the raw C API, so it links openfx::Api only.

## How openfx tests it

- `.github/scripts/cmake-build-test.sh` (the per-PR Linux and macOS jobs): builds and installs openfx with CMake, then runs the hostDemo checks (`hostdemo-checks.sh`, shared with the Makefile build) on the CMake-built plug-ins.
- It then runs `cmake-plugin-project.sh`, which builds `cmake/tests/plugin-project` against the installed package (`find_package(openfx)`), as a plug-in repository would. It checks:
  - the bundle name, identifier, version and executable in the Info.plist;
  - an Info.plist used as-is;
  - Resources, with a subdirectory;
  - the exported symbols (exactly OfxGetPlugin and OfxGetNumberOfPlugins) and the rpath;
  - on macOS, deployment: libomp bundled, `codesign --verify --strict`, and nothing referenced outside the bundle.
- `cmake/tests/plugin-project/bundle-group` has two groups of tiny Support plug-ins for `ofx_plugin_bundle_group()`: self-registering ones (two in one directory, one in another, with an explicit dotted ID), REGISTER ones (a namespaced function, two functions in one plug-in) mixed with a self-registering one, a shared source and a shared resource, a resources directory, a `DESTINATION` item, `COMMON_SOURCES`, per-plug-in compile settings checked with `#error`, `PROPERTIES`, `ALSO_SEPARATE`, `COMBINED_ONLY`, `SEPARATE_ONLY`, `NO_DEPLOY`, `COMMON_RPATH` and `COMMON_EXPORT_SYMBOLS`. `cmake-plugin-project.sh` builds them in COMBINED mode (with the rest of the project) and in SEPARATE mode (a second build of the group targets only, each installed through its install component), and checks the bundles, their resources and Info.plist, an install component, and, on Linux and macOS, that cacheDemo loads every plug-in from the expected bundle. `cmake/tests/bundle-group-errors` checks the configure errors and warnings (configure only).
- It runs the Support self-tests `drawsuite-opengl-test` (the OpenGL DrawSuite core with a fake OpenGL table, also on Windows) and `drawcontext-opengl-test` (macOS in an offscreen CGL context, Linux without a context, to test the run-time loading of OpenGL).
- On Linux, `cmake-plugin-project.sh` checks that `OFX_LINUX_CHECK_BUNDLE` points to the installed or the source-tree script, and that `linuxCheckBundle.sh` reports the test plug-ins' deliberate absolute rpath entries, then accepts them once they are in `OFX_ABI_RPATH_ALLOW`. On macOS it builds with `CMAKE_OSX_DEPLOYMENT_TARGET=14.0`.
- On Linux, it runs `linuxCheckBundle.sh` on the installed examples: no system library bundled and no other library needed (they need only system libraries), with the symbol-version limits disabled (`OFX_ABI_*=99`: the Ubuntu builder is newer than EL9; the baseline is checked where the releases are built). Finding no bundle fails.
- The Makefile builds run weekly (extended.yml).

## Patterns (documented, no helper)

- OpenGL: `target_link_libraries(t PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})`. On Linux that links `libOpenGL.so.0` (GLVND) unless `OpenGL_GL_PREFERENCE` is `LEGACY` (`libGL.so.1`, as the Makefiles): see the Linux bundle check.
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
  - `OFX_PLUGIN_NO_UNDEFINED` (ON): Linux and FreeBSD plug-ins are linked with `-z defs` (a plug-in takes no symbol from the host, so an undefined symbol is a missing library, or a symbol that the oldest target lacks). Turn it off for clang sanitizer builds, or use `-shared-libsan`; the Makefiles use `NO_UNDEFINED_FLAG=`;
  - `OFX_OPTFLAG` (empty): optimization flags of the release configurations of the plug-ins and of openfx::Support, after CMake's (so `-O2` overrides Release's `-O3`). `ofx_target_optflag(<target>)` applies it to a repository's own libraries. An installed openfx::Support keeps the flags it was built with. Ignored with MSVC. Decided after the per-repository -O2/-O3 study: plug-in release builds use CMake's Release default, -O3 (it changed no output in openfx-misc, io, arena or gmic on macOS or Linux, except at most one 16-bit step in 3 plug-ins on aarch64; size +2.5-4% with clang, +13-23% with GCC), and OFX_OPTFLAG stays an empty override. Don't use -Ofast: it disables NaN checks;
  - `OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH` (ON): Linux and FreeBSD plug-ins are linked in the build tree with their install rpath (`BUILD_WITH_INSTALL_RPATH`), so the build-tree binaries have the same RUNPATH as the installed ones instead of CMake's build rpath padded with `:` (empty entries: the current directory), and the install rewrites nothing. While it is ON, `BUILD_RPATH`, `CMAKE_BUILD_RPATH` and `CMAKE_SKIP_BUILD_RPATH` don't affect the plug-ins. `INSTALL_RPATH_USE_LINK_PATH` skips the link directories inside the source and binary trees, so a library from an in-tree prefix (e.g. a dependency built in the checkout, such as openfx-io CI's `SeExprBuild`) must be passed with `add_ofx_plugin(RPATH)` or `OFX_PLUGIN_RPATH` (openfx-io does this for SeExpr), or the option turned off. Turn it off as well for a project whose plug-ins, run from the build tree, need a shared library that the project builds;
  - `OFX_PLUGIN_RPATH` (empty): extra install rpath directories of every plug-in (ignored on Windows, like `RPATH`);
  - `OFX_LICENSE=GPL|COMMERCIAL`, only where it changes what's built (openfx-arena: no poppler when COMMERCIAL);
  - `BUILD_TESTING`.
- **Paths for build-plugins.sh** (CMake equivalents of the Makefile variables):
  - OSMESA_PATH and LLVM_PATH (or LLVM_CONFIG), and the choice of mangled OSMesa or GLU (openfx-misc);
  - OIIO_HOME and SEEXPR_HOME (openfx-io); FFmpeg and LibRaw through CMAKE_PREFIX_PATH or their pkg-config paths;
  - OPTFLAG becomes `OFX_OPTFLAG`. CMake's Release flags are `-O3 -DNDEBUG`, Makefile.master's are `-O2 -DNDEBUG`: pass `-DOFX_OPTFLAG=-O2` if a bundle diff needs identical code;
  - OFX_USE_OPENMP defaults to ON, which also enables OpenMP with Apple clang (the Makefiles don't): Natron passes it explicitly.
- **Repository options** keep their Makefile variable names (USE_OSMESA, IM, AUDIO, RICHTEXT, LEGACYIM, MAGICK_PKG_CONFIG, FFMPEG_PKG_CONFIG_PATH, SEEXPR_HOME...), so build-plugins.sh maps them one to one.
- **CMake minimum:** 3.21 everywhere: `cmake_minimum_required(VERSION 3.21...3.31)`, which also makes CMP0177 (normalized install paths, CMake 3.31) NEW with CMake 3.31 and newer. Every target platform ships 3.21 or newer: Rocky Linux 8.8+ and 9 (3.26), Ubuntu 22.04 (3.22) and 24.04 (3.28), MacPorts, MSYS2 and the GitHub runners; Ubuntu 20.04 (3.16) is not supported. `cmake/OpenFX.cmake` and `cmake/OpenFXExpat.cmake` stop with an error under an older CMake, and set the policies of CMake 3.21...3.31 for their own code and functions (and for the modules these include, e.g. FindOpenMP), whatever the project's minimum; a project's own code keeps the policies of its own `cmake_minimum_required`.

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
