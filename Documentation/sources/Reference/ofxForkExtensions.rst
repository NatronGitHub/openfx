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
- keyboard modifiers in the interact actions and the selection-rectangle
  notifications (``kNatronOfxInteractPropKeyModifiers``,
  ``kNatronOfxHostPropInteractKeyModifiers``); with the Support library, call
  ``OFX::ImageEffect::getKeyModifiers()``, which tracks the modifier keys on
  hosts without the extension;
- dynamic choice parameters (``kNatronOfxParamHostPropSupportsDynamicChoices``,
  ``kOfxParamPropChoiceEnum`` on Choice parameters, cascading menus);
- markdown descriptions (``kNatronOfxPropDescriptionIsMarkdown``);
- deprecated plug-ins (``kNatronOfxImageEffectPropDeprecated``; see also the
  standard ``kOfxImageEffectPluginPropObsolete``).

The header itself has the complete description of each extension.

The parameter-based cursor extension (``kNatronOfxParamCursorName``,
``kNatronOfxImageEffectPropDefaultCursors``) is deprecated in favour of the
proposed :doc:`interact cursors <ofxInteractCursors>`.

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
