NatronGitHub/openfx: the OpenFX image effect API, headers and C++ libraries
===========================================================================

[![Build](https://github.com/NatronGitHub/openfx/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/NatronGitHub/openfx/actions/workflows/build.yml)

This is [NatronGitHub/openfx](https://github.com/NatronGitHub/openfx), the fork of the official OpenFX repository
[AcademySoftwareFoundation/openfx](https://github.com/AcademySoftwareFoundation/openfx) used by
[Natron](https://github.com/NatronGitHub/Natron) and by the
[openfx-misc](https://github.com/NatronGitHub/openfx-misc), [openfx-io](https://github.com/NatronGitHub/openfx-io),
[openfx-arena](https://github.com/NatronGitHub/openfx-arena), [openfx-gmic](https://github.com/NatronGitHub/openfx-gmic)
and [openfx-supportext](https://github.com/NatronGitHub/openfx-supportext) plug-ins. Those repositories include it as a
git submodule.

It follows **OpenFX 1.5.1**, including the additions after 1.5.1 listed in `release-notes-next.md`. It adds bug fixes
and the Natron, Nuke, Vegas, Resolve and TuttleOFX extensions. See `release-notes.md` for the changes.

## Contents

| Directory | What |
|---|---|
| `include/` | The OpenFX C API headers. They include the OFX 1.5 headers `ofxGPURender.h`, `ofxColour.h` (with the native OCIO/ACES config) and `ofxDrawSuite.h`. `ofxOpenGLRender.h` is kept as a compatibility header that includes `ofxGPURender.h`. Every property definition carries `@propdef` metadata (`scripts/check-props.py`). Extension headers: `ofxNatron.h`, `nuke/`, `ofxSonyVegas.h`, `tuttle/`. |
| `Support/` | The C++ plug-in support library (`OFX::ImageEffect`, params, clips, interacts), compiled into each plug-in. |
| `HostSupport/` | The C++ host support library (plug-in cache, property suite, param/clip/effect instances) used by Natron. It needs the system expat library (e.g. `libexpat1-dev`, MacPorts `expat`, MSYS2 `mingw-w64-ucrt-x86_64-expat`, vcpkg or conan `expat`). |
| `openfx-cpp/` | Upstream's newer C++ layer (generated property accessors). It is additive: `Support/` remains the main plug-in library. |
| `Examples/`, `Support/Plugins/`, `HostSupport/examples/` | Example plug-ins and the `hostDemo`/`cacheDemo` example hosts. |
| `Documentation/` | The OFX 1.5.1 documentation (Sphinx and doxygen), with pages on the fork's extensions. |

## OpenFX 1.5.1 support

Both libraries implement the 1.5 / 1.5.1 features. Each one is opt-in on both sides, so existing plug-ins and hosts
work unchanged:

- string-valued choice params (StrChoice), ChoiceOrder, Bytes params and InterpType;
- the effect properties Obsolete, CPURenderSupported, ThumbnailRender and DisplayTimeOffset;
- CUDA (and CUDA streams), Metal (buffers and textures) and OpenCL (buffers, images and the program suite) render properties;
- DrawSuite and OverlayInteractV2. The Support library applies the spec strictly: V1 overlay draws use only OpenGL,
  V2 draws only the DrawSuite. With `setOverlayInteractDescriptorV2()` and `DrawArgs::getDrawContext()`, one DrawSuite
  overlay `draw()` also works on hosts without the DrawSuite (it is converted to OpenGL there), and
  `setOverlayInteractDescriptorV2(desc, true)` also registers a V1 entry for plug-ins that have their own OpenGL code;
- colour management (styles, clip colourspaces, GetOutputColourspace);
- the optional `OfxSetHost` entry point, which HostSupport calls before `OfxGetNumberOfPlugins`.

Plug-ins must keep working in hosts that implement only OFX 1.4. The Support library falls back when the host
lacks an OFX 1.5 or extension suite (views, image planes, render planes), and `hostDemo --ofx14` emulates such a
host for the tests.

The gaps found when these features were merged (read-only properties, the Win-arm64ec and Linux arch bundle
folders, the NoSpatialAwareness render argument, and a property-set audit) are fixed ([#76](https://github.com/NatronGitHub/openfx/issues/76)–[#79](https://github.com/NatronGitHub/openfx/issues/79), closed). The host
side of these features (for example in Natron) must be implemented by each host; see the HostSupport header
comments.

## Extensions

The fork keeps these extension sets:
- **Natron** (`ofxNatron.h`): extra params, multi-plane images, in-viewer params, and others. It also has the proposed
  **interact cursor** extension ([#4](https://github.com/NatronGitHub/openfx/issues/4)), which lets overlays set the
  mouse cursor (`kOfxInteractPropCursor`). It's enabled with `OFX_EXTENSIONS_NATRON` and described in
  `Documentation/proposals/interact-cursor.md`.
- **Nuke** (`nuke/`): camera, multi-plane and transform extensions.
- **Vegas** (`ofxSonyVegas.h`).
- **TuttleOFX** (`tuttle/`).
- **Resolve**: GPU render properties, now part of OFX 1.5.

Each set is enabled with the matching `OFX_EXTENSIONS_*` define. The documentation has a page on them.

## Building

The libraries are usually compiled as part of a host or plug-in build. The repository can also be built on its own.

With Make (still used by the consumers' release builds, e.g. Natron's `build-plugins.sh`; their CI builds with CMake):
```sh
make -C Support/Library        # libOfxSupport.a
make -C HostSupport            # libofxHost.a (needs expat: pkg-config, or EXPAT_INCLUDE=<dir>)
make -C Support/Plugins        # sample plug-ins; CONFIG=debug|release|relwithdebinfo
make -C HostSupport/examples   # hostDemo, cacheDemo
make -C Examples               # raw C-API examples; HAVE_METAL=0|1 (macOS, default 1), HAVE_OPENCL=1, HAVE_CUDA=1 (Linux)
```
On Linux and FreeBSD, the plug-in Makefiles link with `-Wl,-z,defs` (`NO_UNDEFINED_FLAG`) and put the system
libraries (`SYSLIBS`) last. On macOS they target 14.0 unless `MACOSX_DEPLOYMENT_TARGET` is set.
The Makefiles define no `OFX_EXTENSIONS_*`/`OFX_SUPPORTS_*` (all off, unlike the CMake defaults): pass them with `CXXFLAGS_EXTRA`
to `Support/Plugins` and `Examples` (in `Support/Plugins` it also covers the Support objects), and with `OPTIMISE` to `HostSupport` and
`HostSupport/examples`. `libOfxSupport.a` from `make -C Support/Library` is compiled without any, so link it only into a
plug-in compiled without any (see the rule on these definitions below).

With CMake (3.21 or newer):
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/prefix   # provides find_package(openfx): openfx::Api, openfx::Support, openfx::HostSupport
```
Main options (see below for the rule on the `OFX_EXTENSIONS_*` and `OFX_SUPPORTS_*` ones):
- `OFX_EXTENSIONS_NATRON`, `_NUKE`, `_VEGAS`, `_TUTTLE` (ON) and `_RESOLVE` (OFF): enable the extensions in the Support and HostSupport code. They are PUBLIC compile definitions that change class layouts, so they must match between the libraries and the plug-in or host built with them (the extension headers are always installed). `_NATRON` needs `_NUKE`;
- `OFX_SUPPORTS_OPENGLRENDER` (ON), `_OPENCLRENDER` and `_CUDARENDER` (OFF): GPU rendering in the libraries and the examples (OpenCL needs the OpenCLHeaders and OpenCLICDLoader packages, except on macOS; CUDA is off on macOS);
- `OFX_SUPPORTS_MULTITHREAD`, `_DIALOG`, `_PARAMETRIC` and `_PARAMETRIC_V2` (OFF): HostSupport features, only for a host that implements them (they add pure virtual functions, and PARAMETRIC needs the host's `ofxhParametricParam.h`);
- `OFX_BUILD_HOSTSUPPORT`: build HostSupport, on by default at top level;
- `OFX_HOSTSUPPORT_OPENGL` (OFF): openfx::HostSupportOpenGL, offscreen OpenGL contexts (EGL on Linux and BSD, loaded at run time; CGL on macOS; WGL on Windows), textures and renders for hosts (HostSupport/include/ofxhOpenGL*.h); needs `OFX_SUPPORTS_OPENGLRENDER`;
- `BUILD_EXAMPLE_PLUGINS` and `BUILD_EXAMPLE_HOSTS`: the example plug-ins and hosts;
- `OFX_CXX_STANDARD`: the C++ standard of the plug-ins and of openfx::Support (default: `CMAKE_CXX_STANDARD`, else 17);
- `OFX_PLUGIN_NO_UNDEFINED` (ON): link the plug-ins with `-z defs` on Linux and FreeBSD, so an undefined symbol fails the link; turn it off for clang sanitizer builds, or use clang's `-shared-libsan` (gcc links its shared sanitizer runtime by default, so it works with `-z defs`). The Makefiles do the same, unless `NO_UNDEFINED_FLAG=` is passed;
- `OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH` (ON): on Linux and FreeBSD, link the build-tree plug-ins with their install rpath, so their RUNPATH has no empty entry (CMake's build rpath is padded with `:` for the install-time rewrite, and an empty entry means the current directory). While it is ON, `BUILD_RPATH`, `CMAKE_BUILD_RPATH` and `CMAKE_SKIP_BUILD_RPATH` don't affect the plug-ins. `INSTALL_RPATH_USE_LINK_PATH` skips the link directories inside the source and binary trees, so a library from an in-tree prefix (e.g. a dependency built in the checkout) must be passed with `add_ofx_plugin(RPATH)` or `OFX_PLUGIN_RPATH`; otherwise, or if the plug-ins, run from the build tree, need a shared library built by the project, turn the option off. Ship and check (`linuxCheckBundle.sh`) the installed bundles, never the build tree;
- `OFX_OPTFLAG`: extra optimization flags of the release configurations, after CMake's (empty: CMake's defaults; ignored with MSVC);
- `CMAKE_OSX_DEPLOYMENT_TARGET`: 14.0 by default at top level (VFX Reference Platform CY2027), unless the `MACOSX_DEPLOYMENT_TARGET` environment variable is set; the plug-in Makefiles (`Makefile.master`) use the same default (the HostSupport and hostDemo Makefiles don't set one). A toolchain file (e.g. conan's `os.version`) sets its own. Plug-in projects set it before `project()`, and `cmake/OpenFX.cmake` warns if it is empty or newer than 14.0 when `OFX_DEPLOY_BUNDLES` is ON (the shipped bundles; CI and development builds may target the build machine's macOS);
- `OFX_SUPPORT_DRAWCONTEXT_OPENGL` (ON): compile the OpenGL backend of `DrawArgs::getDrawContext()` (`Support/Library/ofxsDrawContextOpenGL.cpp`). Neither openfx::Support nor the plug-ins link OpenGL: the backend loads it at run time. It needs no OpenGL header: it draws with the OpenGL DrawSuite core, `include/ofxDrawSuiteOpenGL.h`, which declares the OpenGL types and constants it uses (hosts can use that header too). OFF compiles it with `OFXS_DRAWCONTEXT_NO_OPENGL` (no OpenGL at all: `getDrawContext()` draws nothing on V1 overlays); Makefile builds can pass `-DOFXS_DRAWCONTEXT_NO_OPENGL`;
- `BUILD_EXAMPLE_MULTIPLYGPU` (ON) and `OFX_EXAMPLE_GPUGAIN_METAL` (ON on macOS): the GPU samples of `Support/Plugins`;
- `OFX_DEPLOY_BUNDLES` (for plug-in projects, macOS only, by design): make the installed bundles self-contained with `ofx_deploy_bundle()`. Build every macOS bundle you ship with it ON: it also checks the deployment target (14.0). On Linux and Windows, the installers bundle the third-party libraries (on Linux, in `<bundle>/Libraries`, found through the `$ORIGIN/../../Libraries` rpath), and the system libraries are never bundled.

The `OFX_EXTENSIONS_*` and `OFX_SUPPORTS_*` definitions change class layouts, virtual signatures and enum values in Support and HostSupport. Every source file of one plug-in or host binary, including the Support or HostSupport sources compiled into it or into the library it links, must be compiled with the same set. Different binaries may use different sets: in particular, a host built with HostSupport and the plug-ins it loads, built with Support, are separate binaries and need not support the same extensions, so the HostSupport and Support configurations need not match (the extensions are negotiated at run time through properties and suites). A mismatch is a silent ODR violation, not detected at build time (NatronGitHub/openfx#226).
- With CMake, choose them through these options (and `OFX_EXTRA_DEFINITIONS`), which `openfx::Api`, `openfx::Support` and `openfx::HostSupport` pass on to the targets that link them. Don't add or remove one on such a target or on a single file.
- A build that compiles the Support or HostSupport sources itself (a Makefile, qmake, or a CMake target that lists them) chooses its own set for each binary, as long as all the sources of that binary get it.
- The same holds for the consumers' own layout macros, e.g. SupportExt's `OFXS_HAVE_DRAWSUITE`.

`OFX_LINUX_CHECK_BUNDLE` (set by `cmake/OpenFX.cmake`) is the path of `linuxCheckBundle.sh`, which checks installed Linux
bundles against the EL9 baseline (no bundled system library, `$ORIGIN` rpaths only unless listed in `OFX_ABI_RPATH_ALLOW`,
symbol versions at most EL9's): see `Documentation/proposals/cmake-plugins.md`.

Plug-in projects use `add_ofx_plugin()` and the other helpers of `cmake/OpenFX.cmake`, with openfx as a subdirectory or as an installed package: see [Documentation/proposals/cmake-plugins.md](Documentation/proposals/cmake-plugins.md).
Each plug-in's install rules are in a component named after its target: `cmake --install <build> --component <target>` installs one bundle.
A conan recipe (`conanfile.py`, with `test_package/`) is also provided.

### Examples

`Examples/` has the raw C-API examples, including the OFX 1.5 DrawSuite, ColourSpace and ChoiceParams examples and the
Cuda, OpenCL and Metal GPU examples (see `Examples/README.txt`). `Support/Plugins/` has the Support library samples,
with their pytest tests in `Support/Plugins/tests`. GPU paths are built only where the toolkit is available.

### CI

`.github/workflows/build.yml` runs one build per OS for each pull request (Linux, Windows, then macOS, which isn't
required for merging); `extended.yml` builds the other variants weekly (Saturday 03:17 UTC) and on demand. Docs-only changes run no CI. See the release
notes for the checks.

Documentation: `cd Documentation && ./build.sh` (see `Documentation/README.md` for the prerequisites).

## Contributing

Open pull requests against `master` of NatronGitHub/openfx. Changes to the headers, `Support/` or `HostSupport/` affect
every consumer: keep them source-compatible, and build at least one consumer against them. Changes to the OpenFX
standard itself go to [upstream](https://github.com/AcademySoftwareFoundation/openfx), following its
`STANDARD_PROCESS.md` and `AI_POLICY.md`.

## Historical notes (OpenFX 1.4)

These notes were written for OpenFX 1.4. Many of them are answered by OpenFX 1.5.

### Things that needed to be clarified in the OpenFX 1.4 spec

#### Default value for output components?

When a plugin accepts different components for input and output, e.g.:
```
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentAlpha);

    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
```
Does the host have to set the dstClip components by default to the components of the srcClip, or does the plug-in have to explicitely set these, using for example:
```
void
MyPlugin::getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences)
{
    clipPreferences.setClipComponents(*_dstClip, _srcClip->getPixelComponents());
}
```

#### OpenGL render support missing documentation

- when entering the render action, is the output framebuffer bound so that drawing is done on output image ?
- may OpenGL rendering also support tiles ? In this case what coordinate system should be used to draw - relative to bottomleft of renderWindow, or canonical ?

### Things that needed to be added to OpenFX

#### Plug-in and host license properties

The plugin and the host license may be incompatible, e.g. if one is GPL and the other is not GPL-compatible:
- http://www.gnu.org/licenses/gpl-faq.en.html#GPLAndPlugins
- http://www.gnu.org/licenses/gpl-faq.en.html#GPLPluginsInNF

We should have a host and instance descriptor property (kOfxPropLicense ?) that holds a string containing a standardized acronym of the license, e.g. "GPL-2.0", "BSD-3-Clause". For example, all plugins in the official OpenFX examples are "BSD-3-Clause", which is GPL-compatible. The default value should be "Commercial".

For a list of license acronyms, see http://opensource.org/licenses/alphabetical

#### Passing host data as an opaque pointer to all suite functions

All suite functions should take a void* as first argument, so that the suite function, as implemented in the host, may figure out in which context the action was called (time, view, plane, etc.). This pointer should be passed in the inArgs for all actions. This requires upgrading all suites.

### Possible OpenFX extensions

The following OpenFX extensions are not mentioned in the draft proposals on http://openeffects.org

#### Ability to fetch images at a given render scale

#### Sound effects suite
