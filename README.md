NatronGitHub/openfx: the OpenFX image effect API, headers and C++ libraries
===========================================================================

[![Build](https://github.com/NatronGitHub/openfx/actions/workflows/build.yml/badge.svg)](https://github.com/NatronGitHub/openfx/actions/workflows/build.yml)

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
| `HostSupport/` | The C++ host support library (plug-in cache, property suite, param/clip/effect instances) used by Natron. It bundles expat 2.10.0. |
| `openfx-cpp/` | Upstream's newer C++ layer (generated property accessors). It is additive: `Support/` remains the main plug-in library. |
| `Examples/`, `Support/Plugins/`, `HostSupport/examples/` | Example plug-ins and the `hostDemo`/`cacheDemo` example hosts. |
| `Documentation/` | The OFX 1.5.1 documentation (Sphinx and doxygen), with pages on the fork's extensions. |

## OpenFX 1.5.1 support

Both libraries implement the 1.5 / 1.5.1 features. Each one is opt-in on both sides, so existing plug-ins and hosts
work unchanged:

- string-valued choice params (StrChoice), ChoiceOrder, Bytes params and InterpType;
- the effect properties Obsolete, CPURenderSupported, ThumbnailRender and DisplayTimeOffset;
- CUDA (and CUDA streams), Metal (buffers and textures) and OpenCL (buffers, images and the program suite) render properties;
- DrawSuite and OverlayInteractV2;
- colour management (styles, clip colourspaces, GetOutputColourspace).

Known gaps are tracked in [#76](https://github.com/NatronGitHub/openfx/issues/76) (read-only properties),
[#77](https://github.com/NatronGitHub/openfx/issues/77) (Win-arm64ec/Linux arch bundle folders),
[#78](https://github.com/NatronGitHub/openfx/issues/78) (NoSpatialAwareness render argument) and
[#79](https://github.com/NatronGitHub/openfx/issues/79) (property-set audit). The host side of these features (for
example in Natron) must be implemented by each host; see the HostSupport header comments.

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

With Make (what the consumer repositories use):
```sh
make -C Support/Library        # libOfxSupport.a
make -C HostSupport            # libofxHost.a (builds the bundled expat)
make -C Support/Plugins        # sample plug-ins; CONFIG=debug|release|relwithdebinfo
make -C HostSupport/examples   # hostDemo, cacheDemo
```

With CMake:
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # options: OFX_EXTENSIONS_*, OFX_USE_SYSTEM_EXPAT, BUILD_EXAMPLE_HOSTS
cmake --build build
cmake --install build --prefix /path/to/prefix   # provides find_package(openfx): openfx::Api, openfx::Support, openfx::HostSupport
```
A conan recipe (`conanfile.py`, with `test_package/`) is also provided.

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
