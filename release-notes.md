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
