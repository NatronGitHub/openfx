.. SPDX-License-Identifier: CC-BY-4.0

Extensions in the NatronGitHub fork
===================================

`NatronGitHub/openfx <https://github.com/NatronGitHub/openfx>`_ follows the
OpenFX standard (this reference describes OFX 1.5.1) and also ships the
headers of several non-standard extensions, used by Natron and by the
openfx-misc, openfx-io, openfx-arena and openfx-gmic plug-ins. None of them is
part of the OpenFX standard: a plug-in must check that the host supports an
extension (usually through a host property, or by the status returned when
setting a property) before relying on it.

The properties of these extensions carry ``@propdef`` metadata with an
``extension:`` key (``natron``, ``nuke``, ``vegas``, ``resolve``, ``tuttle``),
so they appear in the generated :doc:`property reference
<ofxPropertiesReferenceGenerated>` (for the extensions defined in
``include/ofx*.h``). They are not listed in the standard property sets.
``scripts/check-props.py`` validates the metadata of the standard and
extension headers.

Natron (``ofxNatron.h``, ``OFX_EXTENSIONS_NATRON``)
---------------------------------------------------

Natron-specific extensions, among them:

- multi-plane images and layer selection
  (``kNatronOfxImageEffectPropChannelSelector``, ``kOfxImageEffectPropRenderPlanes``);
- distortion and transform concatenation (``kOfxImageEffectPropCanDistort``,
  ``kOfxPropInverseDistortionFunction``, ``kOfxPropMatrix3x3``);
- host masking and mixing (``kNatronOfxImageEffectPropHostMasking``,
  ``kNatronOfxImageEffectPropHostMixing``);
- in-viewer interface: in-viewport parameters and toolbars, shortcuts, the
  host selection rectangle, right-click menus, undo/redo
  (``kNatronOfxImageEffectPropInViewerContextParamsOrder`` and related
  properties);
- keyboard modifiers in the interact actions, of overlays and parameter
  interacts (``kNatronOfxInteractPropKeyModifiers``,
  ``kNatronOfxInteractHostPropSupportsKeyModifiers``), a mirror of the proposed
  OFX 1.6 ``kOfxInteractPropKeyModifiers``; with the Support library, call
  ``OFX::ImageEffect::getKeyModifiers()``, which tracks the modifier keys on
  hosts without the extension;
- drop shadows on DrawSuite elements (``kNatronOfxInteractPropDrawSuiteShadows``,
  in the inArgs of the draw action, 0 if absent): drawn by the host's
  DrawSuite implementation, and by the Support library's DrawSuite-to-OpenGL
  emulation in V1 draws (``OFXS_SUPPORT_HAS_DRAWSUITE_SHADOWS``), for the
  lines, ellipse outlines and text, not the filled rectangles and polygons;
  plug-ins never draw shadows with DrawSuite calls (nor their own shadow loop
  around ``getDrawContext()`` calls), and OpenGL drawing code keeps its own;
- dynamic choice parameters (``kNatronOfxParamHostPropSupportsDynamicChoices``,
  ``kOfxParamPropChoiceEnum`` on Choice parameters, cascading menus);
- markdown descriptions (``kNatronOfxPropDescriptionIsMarkdown``).

The header itself has the complete description of each extension.

Plug-ins that are superseded (by a newer version, by another plug-in, or by
a host-internal node) set the standard ``kOfxImageEffectPluginPropObsolete``;
the former Natron ``NatronOfxImageEffectPropDeprecated`` extension was removed.

The parameter-based cursor extension (``kNatronOfxParamCursorName``,
``kNatronOfxImageEffectPropDefaultCursors``) is deprecated in favour of the
proposed :doc:`interact cursors <ofxInteractCursors>`. Natron 2.6 (the RB-2.6
branch, unreleased) no longer honours it (Natron#1108): plug-ins that still set the cursor parameter get the
default cursor there.

Nuke (``include/nuke/``, ``OFX_EXTENSIONS_NUKE``)
-------------------------------------------------

The Foundry's public extensions, as implemented by Nuke: multi-plane images
(``kFnOfxImageEffectPlaneSuite``, ``kFnOfxImageEffectPropComponentsPresent``),
multi-view (``kFnOfxImageEffectPropView``, ``kFnOfxImageEffectPropViewAware``),
pass-through clips, 2D transform matrices (``kFnOfxPropMatrix2D``), camera
inputs (``camera.h``), layout hints and host version properties
(``fnPublicOfxExtensions.h``).

Sony Vegas (``ofxSonyVegas.h``, ``OFX_EXTENSIONS_VEGAS``)
---------------------------------------------------------

Vegas Pro extensions: render quality, stereoscopic image suite, progress and
keyframe suites, the Vegas contexts, and the data used to upgrade ("uplift")
older Vegas plug-ins.

DaVinci Resolve (``OFX_EXTENSIONS_RESOLVE``)
--------------------------------------------

Resolve introduced several properties before they were standardized:

- CUDA, CUDA stream, Metal and OpenCL rendering, now standard in
  ``ofxGPURender.h`` (OFX 1.5); the fork keeps the original
  ``OFX_EXTENSIONS_RESOLVE`` block of ``ofxImageEffect.h``, whose definitions
  are token-identical to ``ofxGPURender.h``;
- the string-valued choice parameter (``kOfxParamTypeStrChoice``,
  ``kOfxParamPropChoiceEnum``), standard since OFX 1.5 and no longer behind
  ``OFX_EXTENSIONS_RESOLVE`` in this fork;
- ``kOfxImageEffectPropNoSpatialAwareness``, standard since OFX 1.5.1;
- Resolve-only properties such as ``kOfxImageEffectPropResolvePage`` and
  ``kOfxImageClipPropThumbnail``.

TuttleOFX (``include/tuttle/``, ``OFX_EXTENSIONS_TUTTLE``)
----------------------------------------------------------

Reader/writer properties of the TuttleOFX host
(``kTuttleOfxImageEffectPropSupportedExtensions``,
``kTuttleOfxImageEffectPropEvaluation``).

C++ Support library additions
-----------------------------

The fork's Support library (``Support/``) has a few additions that are not in
upstream's. Each one has a feature macro in ``ofxsInteract.h``, so that code
shared with other Support versions can test it:

- ``OFXS_SUPPORT_HAS_DRAWCONTEXT``: the C++ API of the OFX 1.5 DrawSuite
  (``OFX::DrawContext``, ``DrawArgs::drawContext`` and
  ``ImageEffectDescriptor::setOverlayInteractDescriptorV2()``). Test this
  macro, not ``kOfxDrawSuite``, which only says that ``ofxDrawSuite.h`` exists.
- ``OFXS_SUPPORT_HAS_DRAWCONTEXT_OPENGL``: ``DrawArgs::getDrawContext()``, and
  ``DrawContext::usesDrawSuite()``, ``usesOpenGL()`` and ``canDraw()``. A
  plug-in registers its overlay with ``setOverlayInteractDescriptorV2()`` and
  draws only through ``getDrawContext()``. In a V2 draw, it draws with the
  host's DrawSuite; in a V1 draw, with an OpenGL emulation of the DrawSuite
  (``Support/Library/ofxsDrawContextOpenGL.cpp``). So the same ``draw()``
  works on DrawSuite hosts and on OFX 1.4 hosts.
- ``OFXS_SUPPORT_HAS_OVERLAY_INTERACT_V1_ENTRY``: a descriptor class has a
  separate ``kOfxImageEffectPluginPropOverlayInteractV1`` entry. On a
  DrawSuite host, ``setOverlayInteractDescriptorV2()`` registers it next to
  V2 only with ``hasOpenGLDraw`` (see below), and
  ``setOverlayInteractDescriptor()`` registers it instead of the main entry.
  ``DrawArgs::calledViaOverlayInteractV2()`` says which entry the host called.
  The spec doesn't say which entry a host uses when both are set; HostSupport
  uses V2 when it has the DrawSuite, unless the host prefers V1
  (``Host::preferOverlayInteractV1()``, e.g. Natron).

  The fork follows the per-call reading of ``ofxInteract.h``
  (``kOfxInteractActionDraw``): "If this is called via
  kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite. If
  this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing
  SHOULD use OpenGL." The rule applies to each draw call, according to the
  entry the host called. So a plug-in may register both entries, and the
  host chooses one (``Host::preferOverlayInteractV1()``). The reference
  guide (``ofxInteracts.rst``) says that effects use the DrawSuite "by
  requesting kOfxImageEffectPluginPropOverlayInteractV2 instead of
  kOfxImageEffectPluginPropOverlayInteractV1", and that then "the plugin
  *must* do all drawing with the DrawSuite". The fork deliberately reads
  this as describing how a V2 draw draws, not as forbidding both
  registrations. The maintainer chose this reading.
- ``OFXS_SUPPORT_HAS_OPENGL_CONTEXT_CHECK``: ``getDrawContext()`` uses OpenGL
  only if an OpenGL context is current, and
  ``DrawArgs::isOpenGLContextCurrent()`` makes the same check.
- ``OFXS_SUPPORT_HAS_OVERLAY_HAS_OPENGL_DRAW``: the strict overlay rules, and
  the ``hasOpenGLDraw`` argument of ``setOverlayInteractDescriptorV2()``. The
  spec says (``kOfxInteractActionDraw``): "If this is called via
  kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite. If
  this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing
  SHOULD use OpenGL." The Support library makes both exclusive: a V2 draw
  uses only the DrawSuite, a V1 draw only OpenGL. In every V1 draw,
  ``DrawArgs::drawContext`` is NULL, even if the host passed a draw context,
  and ``getDrawContext()`` draws nothing if no OpenGL context is current. The
  only compatibility layer is for a plug-in that draws only with the
  DrawSuite, on a host without the DrawSuite: there the overlay is registered
  as V1, and ``getDrawContext()`` converts its DrawSuite calls to OpenGL. On a
  DrawSuite host, ``setOverlayInteractDescriptorV2(desc)`` registers V2 only;
  ``setOverlayInteractDescriptorV2(desc, true)`` also registers the V1 entry,
  for a plug-in whose ``draw()`` has its own OpenGL code: it tests
  ``calledViaOverlayInteractV2()`` (and ``isOpenGLContextCurrent()``), not
  ``drawContext == NULL``.

The OpenGL backend loads the OpenGL functions it uses at run time, so neither
the Support library nor the plug-ins need OpenGL at link time (the
Makefiles weak-link it on macOS). It needs no OpenGL header either: its
drawing code is the OpenGL DrawSuite core (see below), which declares the
OpenGL types and constants it uses. ``OFX_SUPPORT_DRAWCONTEXT_OPENGL=OFF``
compiles ``ofxsDrawContextOpenGL.cpp`` with ``OFXS_DRAWCONTEXT_NO_OPENGL``
(Makefile builds can pass ``-DOFXS_DRAWCONTEXT_NO_OPENGL``):
``getDrawContext()`` then draws nothing on V1 overlays. The comments of
``ofxsInteract.h`` and ``ofxsImageEffect.h`` are the reference.

The OpenGL DrawSuite core
^^^^^^^^^^^^^^^^^^^^^^^^^

``include/ofxDrawSuiteOpenGL.h`` (namespace ``OFX::DrawSuiteOpenGL``) is a
header-only C++98 implementation of the OFX 1.5 DrawSuite with OpenGL 1.1
compatibility-profile calls. It is not part of the standard. The Support
library's OpenGL backend draws with it, and hosts that implement the
DrawSuite with OpenGL can use it too, so that a DrawSuite overlay looks the
same in a DrawSuite host and, through Support's emulation, in an OpenGL-only
host. It includes no OpenGL header: the caller fills a table of the 13
OpenGL 1.1 functions it calls (``GLFunctions``, with ``loadGLFunctions()``
and the caller's symbol lookup, e.g. dlsym, GetProcAddress or glad). It
provides the DrawSuite state (``State``, ``resetState()``), the DrawSuite
functions (``standardColour()``, ``setColour()``, ``setLineWidth()``,
``setLineStipple()``, ``drawPrimitive()``, ``drawText()`` with the built-in
5x7 font of ``include/ofxFont5x7.h``), and their building blocks
(``ScopedGLState``, ``applyLineState()``, ``stipplePattern()``,
``ellipseSegmentCount()``, ``builtinTextMetrics()``, ``drawBuiltinText()``,
``alignText()``), so that a host can draw the text with its own font and
keep the rest. Its header comment documents the behaviour: line widths in
screen pixels scaled by the screen pixel ratio, width 0 an aliased line,
the stipple patterns, ellipses within 1/4 screen pixel, the text
alignment, and the optional drop shadows (``State::shadows``), which
plug-ins never draw with DrawSuite calls. The self-test
``Support/tests/DrawSuiteOpenGLTest.cpp`` (CMake target
``drawsuite-opengl-test``) checks its OpenGL calls with a fake OpenGL
table.

For HostSupport hosts, ``HostSupport/include/ofxhDrawSuiteOpenGL.h``
(optional, header-only) has ``OFX::Host::Interact::OpenGLDrawSuite``: the
DrawSuite state of one draw action and the OpenGL table, as a member of the
host's interact instance, which forwards its DrawSuite hooks to it and calls
``beginDraw(pixelScale, screenPixelRatio, shadows)`` at the start of each
draw action. A host derives from it to use its own standard colours
(``getColour()``), its own font (``measureText()`` and ``renderText()``,
which get the left end of the baseline computed by ``alignText()``) or its
own primitive renderer (``renderPrimitive()``); it draws the shadow pass of
the lines, ellipse outlines and text, in the host's background colour
(``shadowColour()`` and ``shadowOffset()`` can be overridden). A
``renderText()`` that returns ``kOfxStatReplyDefault`` (a view that can't
render text) makes it fall back to the built-in font. ``Interact::Instance::drawSuiteShadows()`` (false by default)
sets ``kNatronOfxInteractPropDrawSuiteShadows`` in the draw action's inArgs.

The Support library also falls back on hosts that implement only OFX 1.4: one
view, named "Main", without the plane suite V2; ``Clip::fetchImagePlane()``
through the V1 plane suite or ``fetchImage()``; the colour plane as the render
plane when the host sets no ``kOfxImageEffectPropRenderPlanes`` (with
``OFX_EXTENSIONS_NUKE``). ``hostDemo --ofx14`` emulates such a host for the
tests. In debug builds, the property validation doesn't report the properties
that a host may lack: the host-optional ones, and the ones newer than the
host's ``kOfxPropAPIVersion``.
