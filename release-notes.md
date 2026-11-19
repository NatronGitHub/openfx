# NatronGitHub fork

This is [NatronGitHub/openfx](https://github.com/NatronGitHub/openfx), the fork of
[AcademySoftwareFoundation/openfx](https://github.com/AcademySoftwareFoundation/openfx)
used by Natron and the openfx-misc, openfx-io, openfx-arena, openfx-gmic and
openfx-supportext plug-ins. It implements OpenFX 1.5.1, plus the post-1.5.1
additions listed in `release-notes-next.md`, and keeps the Natron, Nuke, Vegas,
Resolve and TuttleOFX extension headers. See the documentation page
*Extensions in the NatronGitHub fork* (`Documentation/sources/Reference/ofxForkExtensions.rst`).

Fixes in HostSupport and Support made before the 1.5 port, since the last upstream
merge, are listed in the pull requests of NatronGitHub/openfx (#23–#58), e.g. expat
2.10.0, property dimension checks, exception safety at the C API boundary, and plugin
cache robustness.

The OFX 1.5 port, all additive and source-compatible with existing plug-ins and hosts:

- #57: OFX 1.5 headers (`ofxGPURender.h`, `ofxColour.h`, `ofxDrawSuite.h`, new definitions; `ofxOpenGLRender.h` kept as a shim)
- #59, #62: Support API (ChoiceOrder, OpenCL buffers/images, upstream spelling aliases, Obsolete, CPU render, thumbnail render, display time offset, param colour management, interpolation type)
- #60, #61: HostSupport declares the OFX 1.5 effect, clip, image, action and param properties with their spec defaults
- #55: StrChoice parameters (standard since 1.5, no longer behind `OFX_EXTENSIONS_RESOLVE`)
- #70: HostSupport API for thumbnail renders
- #71: Metal texture render (Support)
- #72: Bytes parameters
- #73: OpenCL program suite
- #74: DrawSuite and OverlayInteractV2
- #75: colour management (`ofxColour.h`)
- #84: `@propdef`/`@propset`/`@actiondef` metadata in the headers, `scripts/check-props.py`
- #89: documentation port: OpenFX 1.5.1 reference, generated property reference, fork extensions

Since the 1.5 port (post-1.5.1 API additions, such as the keyboard modifiers of
#134/#140 and the removal of `ImageEffectDescriptor::getSelectionRectangle()` in #141,
are in `release-notes-next.md`):

- #85: upstream's header-only C++17 `openfx-cpp` layer, with its generator (`scripts/gen-props.py`) and examples
- #86: upstream's CMake/conan build, adapted to the fork (the Makefiles are unchanged); #125, #127, #129: `cmake/OpenFX.cmake` helpers for the plug-in repositories (`add_ofx_plugin` options, `ofx_plugin_resources`, `ofx_target_openmp`, `ofx_deploy_bundle`); #139: `OFX_SUPPORTS_PARAMETRIC` options, so hosts can use the `openfx::HostSupport` target with `add_subdirectory`
- #121: `@propdef` for every property, including the extension properties; `check-props.py` classifies every string `#define` of `include/`; generated Python `ofxhost.constants` and `ofxhost.props`
- #63: proposed interact-cursor extension (`kOfxInteractPropCursor` and related properties in `ofxInteract.h`, under `OFX_EXTENSIONS_NATRON`)
- #87: Support feature macro `OFXS_SUPPORT_HAS_DRAWCONTEXT`
- #92: `kOfxImageEffectPropNoSpatialAwareness` declared in every build, passed to Render and BeginSequenceRender (`ImageEffect::Instance::isNoSpatialAwarenessRender()`, `RenderArguments::isNoSpatialAwareness`)
- #101: the OFX 1.5 GPU render properties (OpenCL, CUDA, Metal) compiled without `OFX_EXTENSIONS_RESOLVE` in HostSupport and Support; #95: the MultiplyGPU sample builds again, and by default
- #93: `propsetDump` and `scripts/audit-propsets.py` audit HostSupport's property sets against the metadata; fixes found (e.g. `kOfxImageEffectPropOpenGLRenderSupported` always declared, `kOfxImageEffectHostPropNativeOrigin` dimension 1)
- #90, #103, #105, #106, #112, #113: bundle architecture folders (see below); one naming rule for HostSupport, the Makefiles and CMake
- #96, #99, #111, #117–#119, #122, #123, #126, #128, #132: `hostDemo` as a plug-in test host (renders any filter/general plug-in, parameters, animation keys, PFM/PPM I/O, JSON report), the `ofxhost` Python wrapper and graph executor; #120: template pytest suites for the plug-in repositories, and hostDemo's Double parameter derivative and integral (they were stubs)
- #124: ports of upstream fixes (upstream #278, #280, #281, #291), listed under Fixes in `release-notes-next.md`
- Portability: #94 (C++23 builds of Support: the protected copy and assignment declarations and the `ValueParamDescriptor`/`ParametricParamDescriptor` default constructors of `ofxsParam.h` are declared, not defined; `ofxsHWNDInteract.cpp` compiled only with `OFX_EXTENSIONS_VEGAS`; `std::numeric_limits` instead of `DBL_MAX` in `ofxhParam.cpp`; missing `<cstring>` includes), #98, #108 (MSYS2/MinGW Makefiles), #110 (`_WIN32` tests, so strict `-std` MinGW plug-ins export their entry points), #114 (`_MSVC_LANG` in the C++11 tests: MSVC `/std:c++17` failed on `std::auto_ptr`), #116 (`NOMINMAX` before `windows.h`)
- Support fixes: #133 (messages passed as a `"%s"` argument of the message suite, so a `%` in a message is no longer interpreted by the host), #137 (data race at plug-in unload), #142 (`OFX::Log` thread-safe with C++11: per-thread indent; the line lock only in DEBUG builds), #144 (per-thread property logging switch, `OFXS_THREAD_LOCAL`)
- #130: `ofxNatron.h` extension 7: flags of the selection-rectangle state parameter (documentation only)

Merged on 2026-11-18 (#145–#156):

- Build:
  - #149, #155: the Makefiles (`Support/Plugins/Makefile.master`, `Examples/Makefile.master`) pass `MACOSX_DEPLOYMENT_TARGET` as `-mmacosx-version-min` when it is set (from the environment or as a make variable), instead of always the SDK's version; a whitespace-only value is ignored (#155). Since #160 the default is 14.0 instead of the SDK's version.
  - #150: `OFX_EXTENSIONS_NATRON` without `OFX_EXTENSIONS_NUKE` never built: the Natron code in Support and HostSupport uses the Nuke extensions. It now stops early, with an `#error` in `ofxsCore.h` and `ofxhHost.h`, and a FATAL_ERROR at CMake configure.
  - #154: the CMake error for a missing expat says that building without HostSupport (`-DOFX_BUILD_HOSTSUPPORT=OFF`) also needs `-DBUILD_EXAMPLE_HOSTS=OFF`.
  - #155: each plug-in's install rules in `cmake/OpenFX.cmake` (binary, Info.plist, resources, signing, macOS deployment) are in a component named after its target, so `cmake --install <build> --component <target>` installs one bundle. A plain `cmake --install` installs them all, as before.
- HostSupport:
  - #151: `OFX::IsNaN()` and `OFX::IsInfinite()` (`ofxhUtilities.h`) work with `-ffast-math`, and hostDemo no longer uses NaN as an "unset" value; listed under Fixes in `release-notes-next.md`. hostDemo's curves also refuse non-finite key times and values, and non-finite integration bounds, with `kOfxStatErrValue`.
  - #153: parameter descriptors declare the OFX 1.2 properties `kOfxParamPropHasHostOverlayHandle` (read-only for plug-ins) and `kOfxParamPropUseHostOverlayHandle` (writable), both int, default 0, so plug-ins that set `UseHostOverlayHandle` no longer get `kOfxStatErrUnknown`.
- Support:
  - #152: `OFX::IsNaN()` and `OFX::IsInfinite()` (`ofxsCore.h`) work with `-ffast-math`, with a new inline helper `OFX::DoubleWords()`, and a few NaN-producing operations are guarded (the Natron `getTransform()` fallback, the Gamma, Dot, Invert and PropTester samples); listed under Fixes in `release-notes-next.md`. In debug builds with `-ffast-math`, the `assert(!OFX::IsNaN(...))` of the consumers now really fire on NaN input.
  - #156: the internal plug-in UID is `<id>@<major>.<minor>`. It was `id + major + minor`, so ("foo", 1, 23) and ("foo", 12, 3) collided, and the second factory replaced the first. A binary that declares the same identifier and version twice gets an error log (and an assert in debug builds). The UID is never sent to the host.
- Docs and CI:
  - #145: the generated property reference pages are regenerated (15 missing properties, among them the key modifier and selection rectangle properties of `ofxNatron.h`), and the Linux CI job fails if `scripts/gen-props-doc.py` changes them.
  - #146: status refresh of the README, the interact cursor pages, `hostdemo-testing.md` and `ofxForkExtensions.rst`.
  - #147: these release notes for the PRs since #84.
  - #148: the README build badge shows master's status.

Merged on 2026-11-18 and 2026-11-19 (#157–#173):

- Build:
  - #157: Linux and FreeBSD plug-ins are linked with `-z defs`, so an undefined symbol fails the link instead of the load in the host. CMake: `add_ofx_plugin` adds it, and the option `OFX_PLUGIN_NO_UNDEFINED` (ON) turns it off, e.g. for clang sanitizer builds. Makefiles: `NO_UNDEFINED_FLAG` (default `-Wl,-z,defs`; `NO_UNDEFINED_FLAG=` turns it off). The plug-in Makefiles also link `-pthread -ldl` on Linux and `-pthread` on FreeBSD, since `pthread_create` and `dlopen` are not in libc before glibc 2.34 (Rocky 8).
  - #161: the Makefiles link the system libraries last, in `SYSLIBS` (`-lGL`, `-pthread`, `-ldl`, MinGW's `-lopengl32`), after the static libraries that consumers add to `LINKFLAGS` or `LDFLAGS_ADD`.
  - #160: macOS bundles target macOS 14.0 (VFX Reference Platform CY2027) by default: `CMAKE_OSX_DEPLOYMENT_TARGET` when openfx is the top-level project, the conan recipe (unless the profile sets `os.version`), and the plug-in Makefiles (`MACOSX_DEPLOYMENT_TARGET` still overrides). `cmake/OpenFX.cmake` warns once if a plug-in project leaves it empty or sets it above 14.0.
  - #159, #163: `Support/include/linuxCheckBundle.sh` checks installed Linux bundles against the release baseline, EL9 (Rocky Linux 9, CY2027): no system library is bundled, every other needed library is bundled and found through the file's rpath, and symbol versions are at most EL9's. Since #163, every `DT_RUNPATH`/`DT_RPATH` entry must start with `$ORIGIN`, unless it is listed in `OFX_ABI_RPATH_ALLOW`. CMake gives its path in `OFX_LINUX_CHECK_BUNDLE`, and installs it next to `OpenFX.cmake`. The policy and the options are in `Documentation/proposals/cmake-plugins.md`.
- HostSupport:
  - #158: HostSupport calls the optional OFX 1.5 entry point `OfxSetHost`: before `OfxGetNumberOfPlugins` when it scans a binary, and once before `OfxGetPlugin` when it loads a binary that was only known from the plug-in cache, or was unloaded since the scan. A binary whose `OfxSetHost` returns `kOfxStatFailed` is skipped silently. New: `PluginCache::setScanHost()`, the virtual `Host::loadingBinary()`, called before each `OfxSetHost`, and `Base::isObsolete()` (`kOfxImageEffectPluginPropObsolete`).
  - #164: the `kOfxPropType` of an effect instance is `kOfxTypeImageEffectInstance` again, as the spec says. Since 2016 it was copied from the descriptor (`kOfxTypeImageEffect`).
  - #170: `Host::supportsOfxSetHost()` (default true) and `Property::Set::removeProperty()`. `kOfxHostSupportHostPointer` moves to `ofxhHost.h`. `paramDefine` of a StrChoice or Bytes param returns `kOfxStatErrUnknown` when the host has no support property for that type (still `kOfxStatErrUnsupported` when the property is 0).
  - #174: `kOfxActionLoad` and `kOfxActionDescribe` are sent once per load of a binary (the scan used to describe every plug-in twice). By default, a scanned binary gets Load, Describe and Unload for each plug-in and is then unloaded; the first use of a plug-in loads it again (`OfxSetHost`, Load, Describe), as for a plug-in known from the cache. `Host::keepPluginsLoadedAfterScan()` (default false) keeps the scanned plug-ins loaded until `PluginCache::clearPluginCache()` instead; Natron uses it. After an Unload, the base descriptor's pointer properties (overlay entry points) are reset to NULL. hostDemo: `OFX_HOSTDEMO_KEEP_LOADED=1` selects the keep mode, and `hostdemo-checks.sh` checks the Load, Describe and Unload sequence in both modes (with the PropTester trace, `OFX_PROPTESTER_TRACE=1`).
  - #178: `clipGetHandle` and `imageMemoryAlloc` return `kOfxStatErrBadHandle` for a descriptor handle ("Not a valid call in any of the describe actions") instead of using it as an instance, and `clipGetHandle` rejects a NULL name and sets its outputs to NULL on failure.
  - #179: the host properties (`kOfxPropAPIVersion`, `kOfxPropType`, `kOfxPropName`, `kOfxPropLabel`, `kOfxPropVersion`, `kOfxPropVersionLabel`) are read-only for plug-ins, as the spec says, and plug-ins can never overwrite HostSupport's private host pointer, whatever the read-only policy.
  - #180: `paramEditBegin` and `paramEditEnd` return `kOfxStatErrBadHandle` for a param set that is not an instance's, and `paramEditBegin` accepts a NULL name (an empty undo label).
  - #181: `Host::preferOverlayInteractV1()` (default false): a DrawSuite host that draws overlays with OpenGL calls a plug-in's V1 (OpenGL) entry when the plug-in sets both V1 and V2; V2-only plug-ins still get V2. Such a host must make an OpenGL context current around V1 draws. Natron will use it.
- Support:
  - #165: `DrawArgs::getDrawContext()`, so that a plug-in writes one overlay `draw()` for all hosts. Register the overlay with `setOverlayInteractDescriptorV2()` and draw only through `getDrawContext()`: it draws with the host's DrawSuite through the V2 entry, and with an OpenGL emulation of the DrawSuite (`Support/Library/ofxsDrawContextOpenGL.cpp`) through a V1 entry. `DrawArgs::drawContext` is unchanged. Also new: `DrawContext::usesDrawSuite()`, `usesOpenGL()` and `canDraw()`, the feature macro `OFXS_SUPPORT_HAS_DRAWCONTEXT_OPENGL`, and the CMake option `OFX_SUPPORT_DRAWCONTEXT_OPENGL` (ON; OFF, or no OpenGL headers, compiles the backend with `OFXS_DRAWCONTEXT_NO_OPENGL`). The Basic sample draws its overlay that way. `Support/tests/DrawContextOpenGLTest.cpp` (CMake target `drawcontext-opengl-test`) tests the backend.
  - #171: a separate OpenGL `kOfxImageEffectPluginPropOverlayInteractV1` entry for each descriptor class registered with `setOverlayInteractDescriptorV2()` (since #182, registered on DrawSuite hosts only with `hasOpenGLDraw`). V2 stays the main registration. New: `DrawArgs::calledViaOverlayInteractV2()` (`OFXS_SUPPORT_HAS_OVERLAY_INTERACT_V1_ENTRY`), and `DrawArgs::isOpenGLContextCurrent()` (`OFXS_SUPPORT_HAS_OPENGL_CONTEXT_CHECK`), for plug-ins with their own drawing code. The OpenGL backend loads the OpenGL 1.1 functions it uses at run time (dlopen/dlsym, or `GetProcAddress` on Windows), and only uses OpenGL if a context is current. So the Support library no longer needs OpenGL at link time.
  - #182: strict overlay rules, as in the spec (`kOfxInteractActionDraw`: "If this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite. If this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use OpenGL."): V1 draws use only OpenGL, V2 draws only the DrawSuite. `setOverlayInteractDescriptorV2(desc, hasOpenGLDraw = false)`: on a DrawSuite host, the V1 entry of #171 is registered next to V2 only if `hasOpenGLDraw` is true (the plug-in's `draw()` has its own OpenGL code); else V2 only. On a host without the DrawSuite, V1 as before: the only compatibility layer, where `getDrawContext()` converts the DrawSuite calls to OpenGL. In every V1 draw (also parameter interacts and overlays registered with `setOverlayInteractDescriptor()`), `DrawArgs::drawContext` is NULL, and `getDrawContext()` uses OpenGL if a context is current, else draws nothing, never the host's DrawSuite. On a DrawSuite host, `setOverlayInteractDescriptor()` registers the V1 sibling entry of the descriptor class (if it has one), so a class registered both as V1 and as V2 keeps OpenGL in V1 draws and the DrawSuite in V2 draws. `setOverlayInteractDescriptorV2()` returns false when nothing is registered (no overlay support, or no main entry). Not supported: setting `kOfxImageEffectPluginPropOverlayInteractV2` directly, without `setOverlayInteractDescriptorV2()`: the draws of that entry are treated as V1 (`drawContext` NULL, OpenGL), even when the host calls it as V2. One exception remains: `DrawArgs` built by the plug-in from the inArgs (not by the dispatcher) don't know V1 from V2, and use the DrawSuite if the inArgs have a draw context. `DrawArgs::getHostDrawContext()` (#171, unreleased) is removed. Feature macro `OFXS_SUPPORT_HAS_OVERLAY_HAS_OPENGL_DRAW`.
  - #166, #172: fallbacks for OFX 1.4 hosts. Without the plane suite V2, `getViewCount()` and `getViewName()` give one view, "Main"; `Clip::fetchImagePlane(t, view, plane[, bounds])` uses the V1 plane suite, or `fetchImage()` for the colour plane. Since #172, a view other than 0 comes from the Vegas stereoscopic suite if the host has it, and fails with `HostMissingSuite` otherwise (it silently returned view 0). With `OFX_EXTENSIONS_NUKE`, `RenderArguments::planes` is the colour plane when the host sets no `kOfxImageEffectPropRenderPlanes`. A few other calls no longer throw or dereference NULL on 1.4 hosts: `ChoiceParamDescriptor::appendOption` (the hint), the pen viewport position, the OpenGL bit-depth descriptors, `fetchStereoscopicImage(t, 0)`, the parametric param methods (`HostMissingSuite`), the Natron in-viewport setters and `getRegionOfDefinition(t, view)`.
  - #167: inline aliases, so that upstream-style plug-in code compiles: `ImageProcessor::setRenderWindow(const OfxRectI&)` (render scale 1), the `processImagesCuda()` hook, `PropertySet::propGetNString()` and `Double2DParamDescriptor::setUseHostOverlayHandle()`. `Makefile.master` gets a `%.mm` (Objective-C++) rule.
  - #174: the host description (`getImageEffectHostDescription()`) is reset at the last `kOfxActionUnload` of the binary's plug-ins and is NULL until the next Load, and the load count is undone when a plug-in's `load()` throws.
  - #175: the debug validation no longer checks the Min/Max defaults of RGB and RGBA colour parameters, which hosts set differently.
  - #177: on a Vegas host without the plane suite V2, the output clip's colour plane (`fetchImagePlane(t, view[, bounds])`, `fetchStereoscopicImage()`) comes from `fetchImage()`, the image being rendered, as before #172; the Vegas stereoscopic suite is only used for source clips.
  - #173: debug builds no longer log an ERROR for a property that a host may lack. An optional read or write (`throwOnFailure=false`) that gets `kOfxStatErrUnknown` or `kOfxStatErrUnsupported` is not logged. The debug validation skips the host-optional properties, and the ones introduced in an OFX version newer than the host's `kOfxPropAPIVersion`, when the host doesn't have them. Some wrong default checks are fixed, and a `StrChoiceParam` instance is validated as a StrChoice param.
- Examples and samples:
  - #162: `Examples/DrawSuite`, `Examples/ColourSpace` and `Examples/ChoiceParams`, ported from upstream, without upstream's spdlog and CImg dependencies. DrawSuite registers both overlay entries: V1 draws with OpenGL, V2 with the DrawSuite, and calls every DrawSuite function. The three work on OFX 1.4 hosts.
  - #169: the raw C-API `Examples/Cuda` and `Examples/OpenCL` follow `ofxGPURender.h`, and there is a new `Examples/Metal` (macOS). They render on the host's device memory, on its stream or queue, when the host enables it, and on the CPU otherwise (also on 1.4 hosts). They return `kOfxStatGPURenderFailed` when they can't render on the GPU. Makefiles: `HAVE_METAL` (on by default on macOS), `HAVE_OPENCL=1`, `HAVE_CUDA=1` (Linux). `OFX_EXTENSIONS_RESOLVE` is no longer needed, and CMake turns CUDA off on macOS.
  - #167: `Support/Plugins/GPUGain` (CPU, Metal, CUDA, and OpenCL buffers and images; CMake option `OFX_EXAMPLE_GPUGAIN_METAL`) and `Support/Plugins/ChoiceParams`, ported from upstream with fixes.
  - #168: the MultiplyGPU sample links the Wayback Machine snapshot of opticalenquiry.com.
- Test host and CI:
  - #170: `hostDemo --ofx14` emulates a host that implements only OFX 1.4: only the 1.4 host properties and suites, no `OfxSetHost`, no StrChoice or Bytes params, one view. Plug-in repositories use it from Python with `extra_args=["--ofx14"]` (see `Support/Plugins/tests/test_ofx14.py`). It is the one exception to the hostDemo freeze (`Documentation/proposals/hostdemo-testing.md`).
  - CI: the Linux job runs `linuxCheckBundle.sh` on the installed examples (#159), and the plug-in project test checks that its deliberate absolute rpath entries are reported (#163). The macOS plug-in project test uses 14.0 (#160). The macOS jobs build the OpenCL examples (#169). `cmake-build-test.sh` runs `drawcontext-opengl-test` on Linux and macOS (#165, #171). `hostdemo-checks.sh` renders every sample plug-in in the default and the OFX 1.4 host and compares the outputs (#170). New Python tests: `test_gpugain.py`, `test_choiceparams.py` (#167) and `test_ofx14.py` (#170).

Behaviour changes in the fork since the 1.5 port:

- **Read-only properties are enforced** (#91): the plug-in-facing property suite (`propSet*`, `propSetN*`, `propReset`) now returns `kOfxStatErrValue` when a plug-in writes a property that is read-only for plug-ins; Support turns that into an exception unless the call is non-throwing. Host-side C++ setters are not restricted. Hosts can relax this with `OFX::Host::Property::setPluginReadOnlyPolicy(ePluginReadOnlyWarn)` (accept and log once per property) or `ePluginReadOnlyIgnore` (old behaviour). The properties copied from the clip descriptor stay read-only on clip instances, as documented (except the Natron label, secret and hint, below); `kOfxImageClipPropPreferredColourspaces` stays writable on clip instances. Some Natron descriptor properties and the IsIdentity outArgs `kFnOfxImageEffectPropView` and `kOfxImageEffectPropIdentityPlane` became writable.
- **Clip depths in clip preferences** (#102): the `OfxImageClipPropDepth_<clip>` outArgs are writable if and only if both the host and the plug-in support multiple clip depths, whatever the context. Support's `ClipPreferencesSetter::setClipBitDepth` no longer fails the action when the host refuses the write: it logs a warning and leaves the depth to the host. The warning goes through `OFX::Log::warning`, which only writes in DEBUG builds, so the refusal is silent in release builds. Plug-ins that set clip or output depths must declare `kOfxImageEffectPropSupportsMultipleClipDepths`, or the host keeps its own depths. Hosts that build their own clip-preferences outArgs (Natron does) should apply the same rule: writable if and only if host and plug-in both support multiple clip depths.
- **Clip instance labels (Natron extension)** (#104, #106): an exception to #91: with `OFX_EXTENSIONS_NATRON`, the clip instance `kOfxPropLabel`, `kOfxPropShortLabel`, `kOfxPropLongLabel`, `kOfxParamPropSecret` and `kOfxParamPropHint` stay writable by plug-ins; other clip instance properties are read-only.
- **Plug-in binary folders** (#90, #103, #105, #106, #112): HostSupport searches the OFX 1.5 folders, and uses the first folder whose binary matches the host's architecture (read from its ELF, Mach-O or PE header), skipping binaries that don't match (#112).
  - Linux: `Linux-$(uname -m)` on non-x86 hosts, with a fallback to the legacy `Linux-x86-64`/`Linux-x86` (#90, #103, #106).
  - macOS: `MacOS` first; x86-64 hosts then search the deprecated `MacOS-x86-64` as a fallback (#106).
  - Windows: `Win32`/`Win64` as the spec spells them, then the lowercase `win32`/`win64` fallbacks (#112); Arm64EC and emulated x64 hosts search `Win-arm64ec` first; native Arm64 hosts use the reserved `Win-arm64`/`Win-arm64x` (#90).
  - FreeBSD (not in the spec): names follow `uname -p`, e.g. `FreeBSD-aarch64` and `FreeBSD-armv7`, with the old `FreeBSD-arm64`/`FreeBSD-arm` kept as fallbacks (#105, #106). CMake no longer adds a `Linux-` prefix on FreeBSD.
  - For packagers: Makefile builds on Linux aarch64 now install to `Linux-aarch64` (without `-m64`), CMake Arm64EC builds to `Win-arm64ec`, and native Arm64 builds to `Win-arm64`. HostSupport still falls back to the legacy folders for old bundles.
- **Keyframe defaults** (#128): HostSupport's default keyframe functions (used by hosts that don't override the `KeyframeParam` methods, e.g. for push buttons in Natron) return the statuses `ofxParam.h` lists, `kOfxStatOK` with 0 keys, `kOfxStatErrBadIndex` or `kOfxStatFailed`, instead of `kOfxStatErrMissingHostFeature`. Also, CMake installs the bundled expat headers and the example hosts only with `OFX_INSTALL`, which is OFF under `add_subdirectory`.
- **NoSpatialAwareness renders** (#92): hosts get a new virtual, `ImageEffect::Instance::isNoSpatialAwarenessRender()` (default false). A host overrides it to send `"true"`; until then plug-ins see false.
- **Support messages** (#133): `sendMessage` and `setPersistentMessage` pass the message as a `"%s"` argument, so a plug-in that escaped `%` as `%%` now shows `%%`: stop escaping. `OFX::Log::print`, `warning` and `error` now have printf format attributes, so a plug-in that passes a non-literal format gets `-Wformat-security`, an error under `-Werror=format-security`: pass `"%s", msg`.
- **Descriptor setters** (#92, #101): `setNoSpatialAwareness()` and the GPU `setSupports*Render()` / `setSupportsCudaStream()` no longer throw on hosts that don't know the property.
- **Natron extensions need the Nuke ones** (#150): `OFX_EXTENSIONS_NATRON` without `OFX_EXTENSIONS_NUKE` is an error. All the consumers define both.
- **Host overlay handles** (#153): hosts that derive from HostSupport get `kOfxParamPropHasHostOverlayHandle` and `kOfxParamPropUseHostOverlayHandle` on every parameter but groups and pages. A host that declares them itself with `addProperties()` (Natron) is unaffected: that is a no-op for an existing property.
- **Undefined symbols fail the link** (#157, #161): a Linux or FreeBSD plug-in repository that is under-linked today now fails to link. Typical causes: a static library linked without its own dependencies (OSMesa, LLVM with an old `llvm-config`, a static FFTW, OCIO through `pkg-config` without `--static`), or `-lstdc++fs` with GCC 8. Add the missing libraries. Consumers no longer need to add `-ldl` or `-lGL` after their static libraries. A Makefile that filters `-lGL` out of `LINKFLAGS` must also filter `SYSLIBS` (`SYSLIBS := $(filter-out -lGL,$(SYSLIBS))`).
- **macOS 14.0** (#160): plug-in repositories set `CMAKE_OSX_DEPLOYMENT_TARGET` to 14.0 before `project()`, or on the command line. A build that exports `MACOSX_DEPLOYMENT_TARGET` overrides the Makefiles' default, so it must export 14.0 (Natron's `tools/jenkins/common.sh` exported the build machine's version).
- **Linux bundle check** (#159, #163): run `linuxCheckBundle.sh` (`OFX_LINUX_CHECK_BUNDLE` in CMake) on the installed bundles, in CI and in the installers after the libraries are bundled. Absolute rpath entries are reported: from CMake's `INSTALL_RPATH_USE_LINK_PATH` (set by `add_ofx_plugin`) when building against an SDK prefix, from `add_ofx_plugin(RPATH)` or `OFX_PLUGIN_RPATH`, and from Makefile `-Wl,-rpath,<libdir>` flags. List them in `OFX_ABI_RPATH_ALLOW` for CI, or rewrite them at install time (e.g. `patchelf --set-rpath`). CI builds against the distribution's libraries pass `--no-needed`. CMake's `OpenGL::GL` links `libOpenGL.so.0`, which gives a warning; the Makefiles link `libGL.so.1`.
- **Clean rebuild** (#165, #167, #182): `Makefile.master` has no header dependency tracking, and #165 changed the layouts of `DrawContext` and `DrawArgs` (private members only), so consumers must rebuild from clean after the bump. #167 also changes Support headers, and #182 changes the mangled name of `setOverlayInteractDescriptorV2()` (new argument) and `ofxsSupportPrivate.h`. No source change is needed.
- **Overlays: V1 draws use only OpenGL, V2 draws only the DrawSuite** (#165, #171, #182): `drawContext` is NULL in every V1 draw, even when the host passes a draw context (HostSupport does), also for the overlays registered with `setOverlayInteractDescriptor()` and for parameter interacts: a V1 `draw()` that used the host's `drawContext` must draw with OpenGL. A plug-in with its own OpenGL drawing code registers with `setOverlayInteractDescriptorV2(desc, true)` and tests `DrawArgs::calledViaOverlayInteractV2()` (and `isOpenGLContextCurrent()`), not `drawContext == NULL`. Without `hasOpenGLDraw`, a DrawSuite host only gets the V2 entry. HostSupport-based hosts call the V2 entry when they have the DrawSuite, and Natron (no DrawSuite) only sees V1. A host that has the DrawSuite but calls only V1 entries no longer shows the overlays registered with `setOverlayInteractDescriptorV2()` without `hasOpenGLDraw` (the spec expects it to call V2). A descriptor class with a custom `getMainEntry()` (no V1 sibling entry) must not be registered both as V1 and as V2; other classes may be, since `setOverlayInteractDescriptor()` then registers their V1 sibling entry.
- **OpenGL at link time** (#165, #171): the plug-ins built with `Support/Plugins/Makefile.master` link `ofxsDrawContextOpenGL.o`, which needs the OpenGL headers at compile time, but not OpenGL at link time: that Makefile links `-lGL` with `--as-needed` on Linux and FreeBSD, and weak-links the OpenGL framework on macOS. `openfx::Support` links `CMAKE_DL_LIBS`, not OpenGL. Without the OpenGL headers, CMake compiles that file with `OFXS_DRAWCONTEXT_NO_OPENGL` (also with `OFX_SUPPORT_DRAWCONTEXT_OPENGL=OFF`); Makefile builds pass `-DOFXS_DRAWCONTEXT_NO_OPENGL`. `getDrawContext()` then draws nothing on V1 overlays. Repositories that list the Support sources need `ofxsDrawContextOpenGL.cpp` only if they call `getDrawContext()` or `isOpenGLContextCurrent()` (e.g. through SupportExt's `OverlayDraw`).
- **OFX 1.4 hosts** (#166, #172): plug-ins that worked around the missing plane suite V2 or `kOfxImageEffectPropRenderPlanes` (openfx-io's GenericWriter and WriteOIIO, openfx-misc's Premult) now work on 1.4 hosts without the workarounds. On a host without the plane suite V2 and without the Vegas stereoscopic suite, fetching a view other than 0 now throws `HostMissingSuite` instead of returning view 0 (e.g. openfx-misc's SideBySide with a view other than 0).
- **Effect instance type** (#164): `kOfxPropType` on an effect instance is `kOfxTypeImageEffectInstance`, as on other hosts. A plug-in that compared it with `kOfxTypeImageEffect` would already fail elsewhere.
- **OfxSetHost** (#158): a binary that refuses with `kOfxStatFailed` is cached as having no plug-ins until it changes, so hosts should put their name and version in `setCacheVersion()`. `PluginCache` gained a member: hosts rebuild HostSupport.
- **Quieter debug plug-ins** (#170, #173): the debug property validation no longer requires the OFX 1.5 `kOfxParamHostPropSupportsStrChoiceAnimation`, and no longer reports the properties that a host may lack. Missing required properties, and wrong types, dimensions or values, are still ERRORs.
- **Load and Describe once per load; unload after the scan** (#174): hosts that derive from HostSupport must be rebuilt (the `Host` vtable and the `ImageEffectPlugin` and `PluginBinary` layouts changed); the plug-in cache format is unchanged. By default, a scanned binary is unloaded (`dlclose`/`FreeLibrary`) after the scan and loaded again at first use, so plug-ins must re-initialise their global state across Unload and Load, and their static destructors may run at unload (on Linux and Windows; macOS doesn't unmap images with thread-local variables, which Support plug-ins have). Scan runs pay the static initialisation and Load twice. Hosts that don't want this override `keepPluginsLoadedAfterScan()` to return true. Hosts must serialize the call that loads a plug-in (the first `getPluginHandle()`) and the unloads with every other call, across all the plug-ins of a binary.
- **Host properties are read-only** (#179): a plug-in not built on Support that wrote a host property now gets `kOfxStatErrValue` (under the default Enforce policy); none was found in the consumer repositories.
- **Bad handles** (#178, #180): a plug-in that calls `clipGetHandle`, `imageMemoryAlloc`, `paramEditBegin` or `paramEditEnd` with a describe-time handle gets `kOfxStatErrBadHandle` instead of crashing the host.
- **Host rebuild** (#181): `Host::preferOverlayInteractV1()` is a new virtual, so hosts rebuild HostSupport again.

# OpenFX image processing plug-in API

This is the release 1.5.1 of OFX, the Open Effects image-processing plug-in standard API.

Documentation and more info can be found at:

* [The OpenFX website](https://openeffects.org)
* [OpenFX Documentation](https://openfx.readthedocs.io/en/latest) - start here
* [OpenFX Documentation: Reference](https://openfx.readthedocs.io/en/latest/Reference)
* [OFX Programming Guide By Example](https://openfx.readthedocs.io/en/latest/Guide)
* [OpenFX Wiki](https://lf-aswf.atlassian.net/wiki/spaces/OFX/overview)

# Release Notes - 1.5.1

This is version 1.5.1 of the OpenFX API, a minor release with bug fixes and improvements.

## Key Features of OpenFX Version 1.5.1:

- **CPU Render Support**: Added `kOfxImageEffectPropCPURenderSupported` property to allow plugins to declare CPU rendering capability (or rather lack thereof).
- **Thumbnail Rendering**: Added `kOfxImageEffectPropThumbnailRender` property to support efficient thumbnail generation.
- **Spatial Awareness**: Added `kOfxImageEffectPropNoSpatialAwareness`. Allows the host and plugin to coordinate a render that ensures no spatial changes to the image.


## Fixes in OpenFX Version 1.5.1:

- Fix LICENSE.md to conform to Github template for BSD 3-Clause (issue #194).
- Use predefined `_WIN32` macro everywhere instead of defining `WINDOWS`, improving Windows portability.
- Fix CI build problems with conan version, expat dependency, and compiler compatibility.
- Restore older CentOS 7 builds for vfx-2021 & 2022 compatibility.
- Clarify that Image Effect Hosts property `kOfxPropType` must have the value
  `kOfxTypeImageEffectHost`, and fix host support lib to implement that.

## Documentation Improvements

- Significantly cleaned up documentation for Interacts, including DrawSuite and Overlay Interacts interaction.
- Fixed documentation for `clipGetRegionOfDefinition`.
- Updated colourspace example for more complete coverage.
- Fixed numerous typos throughout Documentation, include, Support, and HostSupport directories (via codespell).
- Corrected copyright notice with explanation in CONTRIBUTING.md (addresses issues #110, #206).
- Updated documentation footer and build script to support `uv` package manager.
- Removed stray references to old OFX Association.

## Deprecations

None

## Detailed List of Changes

- Add `kOfxImageEffectPropCPURenderSupported` (issue #212) - Property to indicate CPU rendering support.
- Add `kOfxImageEffectPropThumbnailRender` (issue #193) - Property for thumbnail rendering mode.

# Release Notes - 1.5

This is version 1.5 of the OFX API.

## Key Features of OpenFX Version 1.5:
- Color Management: Enhanced color management APIs to ensure accurate and consistent color rendering across different source media, devices and platforms.
- DrawSuite: On-screen drawing capabilities to allow hosts and plugins to work together without requiring OpenGL.
- GPU Support: Comprehensive GPU support between hosts and plugins, including OpenCL, CUDA, and Metal, for accelerated performance and smoother rendering.
- Choice Params: Improved parameter handling with the addition of choice parameters, allowing for more intuitive and organized user interfaces.
- Binary Data: Support for binary data to facilitate the handling of plugin-specific data types and improve processing efficiency.
- Windows ARM64: Version 1.6 enables future compatibility with Windows ARM64, expanding the range of supported devices and platforms.
- Houdini Support: SideFX has announced support for OpenFX in their upcoming version 20.5 of Houdini, in the new Copernicus image-processing node.

## Deprecations

None

## Detailed List of Changes

- Add `OfxSetHost` call
- Add `OfxDrawSuite` for drawing image overlays without use of OpenGL
- Add OpenCL, CUDA and Metal support for direct GPU rendering
- Add :c:macro:`kOfxParamPropChoiceOrder` for reordering Choice Params
- Colour management is in [[ofxColour.h][include/ofxColour.h]]

# Release Notes - 1.4

This is version 1.4 of the OFX API.  Significant additions include a
Dialog Suite for plugins to request the host to allow them to put up a
modal dialog, a NativeOrigin host property, draft render quality
support, half-float format tag available for CPU rendering (not just
OpenGL), and a new internationalizable version of the progress suite.

A number of ambiguities in the spec have also been clarified in this
version, including allowing OpenGL processing and tiled rendering to
be enabled/disabled in Instance Changed events, and clarifying the
semantics of dialogs and the progress suite.  The old Analysis pass
action has also been removed.

## Deprecations

In this release, some old properties and tags which were deprecated in
previous releases have now been moved to a new header file, `include/oldOfx.h`.
To continue to support those old tags, whether as a plugin or a host,
you'll need to include oldOfx.h.

## Detailed List of Changes

* _removed_ = final stage of deprecation: moved to oldOfx.h
* _added_ = new property or tag
* _refined/modified_ = documentation of precise expectations (without touching definitions)
also:   some prototypes add a const

ofxImageEffects.h :

* removed -  kOfxImageEffectPropInAnalysis
* removed -  kOfxImageComponentYUVA
* added   -  kOfxImageEffectPropRenderQualityDraft
* refined -  kOfxImageEffectPropSupportsTiles
* added   -  kOfxImageEffectHostPropNativeOrigin
* refined -  kOfxImageEffectPropRenderScale (Interact action was missing in supported actions)
* refined -  prop kOfxBitDepthHalf (kOfxImageEffectPropPixelDepth)
* refined -  prop kOfxBitDepthHalf (kOfxImageClipPropUnmappedPixelDepth)
* refined -  prop kOfxBitDepthHalf (kOfxImageEffectPropSupportedPixelDepths)

ofxParam.h:

* removed - kOfxParamPropPluginMayWrite
* removed - kOfxParamDoubleTypeNormalisedX
* removed - kOfxParamDoubleTypeNormalisedY
* removed - kOfxParamDoubleTypeNormalisedXAbsolute
* removed - kOfxParamDoubleTypeNormalisedYAbsolute
* removed - kOfxParamDoubleTypeNormalisedXY
* removed - kOfxParamDoubleTypeNormalisedXYAbsolute
* modified - kOfxParamPropInteractMinimumSize is now a double (matches kOfxParamPropInteractSize)
* modified - changed comments with regards to OfxParameterSuiteV1 (i.e. no Analysis action anymore)
* refined - paramGetValue, paramSetValue: precision about when it's safe to use - i.e. Instance Changed domain only

ofxPixels.h:

* removed - OfxYUVAColourB;OfxYUVAColourS;OfxYUVAColourF // leftover from 1.2 deprecation
* modified - OfxRGBColourD should not have an "a" component

ofxProgress.h:

* refined - OfxProgressSuiteV1
* added - OfxProgressSuiteV2

ofxDialog.h: NEW

* added - OfxDialogSuiteV1

ofxOpenGLRender.h:

* refined - kOfxImageEffectPropOpenGLRenderSupported

ofxCore.h:

* modified - kOfxFlagInfiniteMax (INT_MAX)  (and Min)
* added - kOfxBitDepthHalf

ofxInteract.h:

* removed - kOfxInteractPropViewportSize

ofxOld.h:

* New file added to capture removals ("removed" above)

/Support folder:
* C++ wrapper adds support for v1.3 and v1.4
