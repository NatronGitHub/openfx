// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _ofxSonyVegas_h_
#define _ofxSonyVegas_h_

/** @file ofxSonyVegas.h
    @brief Extensions of the VEGAS Pro hosts (formerly Sony Vegas, now MAGIX/VEGAS Creative
    Software) and of the Vegas Movie Studio hosts.

    The names and values are those VEGAS Pro hosts use, for interoperability.

    The Support library uses them when it is built with OFX_EXTENSIONS_VEGAS, and HostSupport
    implements some of them.
*/

#include "ofxImageEffect.h" /* OfxStatus, OfxImageClipHandle, OfxParamHandle, OfxInteractHandle, ... */


/* ------------------------------------------------------------------------------------------ */
/* Host names (the kOfxPropName of the host) */

/** @brief Host name of Sony Vegas Pro and of VEGAS Pro up to version 15 at least (versions 12 to 15 seen) */
#define kOfxSonyVegasHostName                    "com.sonycreativesoftware.vegas"

/** @brief Host name of VEGAS Pro 19 (at least) */
#define kOfxVegasProHostName                     "com.vegascreativesoftware.vegas"

/** @brief Host name of Sony Vegas Movie Studio HD */
#define kOfxSonyVegasMovieStudioHostName         "com.sonycreativesoftware.vegas.moviestudio.hd"

/** @brief Host name of Sony Vegas Movie Studio Platinum Edition */
#define kOfxSonyVegasMovieStudioPlatinumHostName "com.sonycreativesoftware.vegas.moviestudio.pe"


/** @brief Directory where the host wants plug-ins to keep their own files, such as logs and caches.
    It is set by the host on its host descriptor.

    @propdef
    type: string
    dimension: 1
    extension: vegas
*/
#define kOfxPropVegasHostAppDataDirectory "OfxPropVegasHostAppDataDirectory"


/* ------------------------------------------------------------------------------------------ */
/* Parameters */

/** @brief kOfxParamPropDoubleType value of a 2D double parameter edited as an angle and a radius */
#define kOfxParamDoubleTypePolar "OfxParamDoubleTypePolar"

/** @brief kOfxParamPropDoubleType value of a 2D double parameter edited as a point on a color wheel */
#define kOfxParamDoubleTypeChrominance "OfxParamDoubleTypeChrominance"

/** @brief Color wheel level of a 2D double parameter of type kOfxParamDoubleTypeChrominance.
    The Support library sets it on the parameter descriptor
    (OFX::Double2DParamDescriptor::setColorWheelLevel).

    @propdef
    type: double
    dimension: 1
    extension: vegas
*/
#define kOfxParamPropColorWheelLevel "OfxParamPropColorWheelLevel"

/** @brief Color model the host uses by default to edit an RGB or RGBA color parameter.
    It is set on the parameter descriptor.

    @propdef
    type: enum
    dimension: 1
    values:
      - OfxParamColorColorspaceRGB
      - OfxParamColorColorspaceHSL
      - OfxParamColorColorspaceHSV
      - OfxParamColorColorspaceLab
    extension: vegas
*/
#define kOfxParamColorDefaultColorspace "OfxParamColorDefaultColorspace"

/** @brief Values of kOfxParamColorDefaultColorspace */
#define kOfxParamColorColorspaceRGB "OfxParamColorColorspaceRGB"
#define kOfxParamColorColorspaceHSL "OfxParamColorColorspaceHSL"
#define kOfxParamColorColorspaceHSV "OfxParamColorColorspaceHSV"
#define kOfxParamColorColorspaceLab "OfxParamColorColorspaceLab"

/** @brief Whether the host shows the parameter unfolded (1) or folded (0) when it first appears.
    It applies to the 2D and 3D double and integer parameters and to the RGB and RGBA parameters,
    and is set on the parameter descriptor.

    @propdef
    type: int
    dimension: 1
    extension: vegas
*/
#define kOfxParamPropParameterExpanded "OfxParamPropParameterExpanded"


/* ------------------------------------------------------------------------------------------ */
/* Pixel formats */

/** @brief kOfxImageEffectPropSupportedPixelDepths and clip pixel depth values of the Vegas pixel
    formats whose components are stored in reverse (B, G, R, A) order: 8-bit unsigned integers,
    16-bit unsigned integers and 32-bit floats.
*/
#define kOfxBitDepthByteBGR  "OfxBitDepthByteBGR"
#define kOfxBitDepthShortBGR "OfxBitDepthShortBGR"
#define kOfxBitDepthFloatBGR "OfxBitDepthFloatBGR"

/** @brief Values of kOfxImagePropPixelOrder */
#define kOfxImagePixelOrderRGBA "OfxImagePixelOrderRGBA"
#define kOfxImagePixelOrderBGRA "OfxImagePixelOrderBGRA"

/** @brief Order of the components of the pixels of a clip's images, R, G, B, A or B, G, R, A.
    The host sets it on the clip instance (read only); the Support library reads RGBA when it is
    missing.

    @propdef
    type: enum
    dimension: 1
    values:
      - OfxImagePixelOrderRGBA
      - OfxImagePixelOrderBGRA
    extension: vegas
*/
#define kOfxImagePropPixelOrder "OfxImageEffectPropPixelOrder"


/* ------------------------------------------------------------------------------------------ */
/* Plug-in descriptor: help, about and preset thumbnails */

/** @brief Path of the plug-in's help file, which the host opens when the plug-in doesn't handle
    kOfxImageEffectActionInvokeHelp. Without it, the host looks for a help file named after the
    plug-in in the plug-in's resource directory.

    @propdef
    type: string
    dimension: 1
    extension: vegas
*/
#define kOfxImageEffectPropHelpFile "OfxImageEffectPropHelpFile"

/** @brief Topic of the help file (kOfxImageEffectPropHelpFile) that the host opens for this plug-in

    @propdef
    type: int
    dimension: 1
    extension: vegas
*/
#define kOfxImageEffectPropHelpContextID "OfxImageEffectPropHelpContextID"

/** @brief Action the host calls when the user asks for the plug-in's help.
    The handle is the image effect instance; inArgs and outArgs are NULL.
    The plug-in returns kOfxStatOK if it showed its help itself, and kOfxStatReplyDefault to
    let the host open kOfxImageEffectPropHelpFile.
*/
#define kOfxImageEffectActionInvokeHelp "OfxImageEffectActionInvokeHelp"

/** @brief Action the host calls when the user asks for the plug-in's "about" information.
    The handle is the image effect instance; inArgs and outArgs are NULL.
    The plug-in returns kOfxStatOK if it showed it itself, and kOfxStatReplyDefault to let the
    host show its own.
*/
#define kOfxImageEffectActionInvokeAbout "OfxImageEffectActionInvokeAbout"

/** @brief How the host draws the thumbnails of the plug-in's presets.
    It is set on the plug-in descriptor.

    @propdef
    type: enum
    dimension: 1
    values:
      - OfxProbPluginVegasPresetThumbnailDefault
      - OfxProbPluginVegasPresetThumbnailSolidImage
      - OfxProbPluginVegasPresetThumbnailImageWithAlpha
    extension: vegas
*/
#define kOfxProbPluginVegasPresetThumbnail "OfxProbPluginVegasPresetThumbnail"

/** @brief Values of kOfxProbPluginVegasPresetThumbnail: the host's default, an opaque image,
    or an image with an alpha channel
*/
#define kOfxProbPluginVegasPresetThumbnailDefault        "OfxProbPluginVegasPresetThumbnailDefault"
#define kOfxProbPluginVegasPresetThumbnailSolidImage     "OfxProbPluginVegasPresetThumbnailSolidImage"
#define kOfxProbPluginVegasPresetThumbnailImageWithAlpha "OfxProbPluginVegasPresetThumbnailImageWithAlpha"


/* ------------------------------------------------------------------------------------------ */
/* Upgrading the projects of older (non-OFX) Vegas plug-ins: "uplift" */

/** @brief GUIDs of the older plug-ins whose project data this plug-in can take over.
    It is set on the plug-in descriptor, one GUID per element.

    @propdef
    type: string
    dimension: N
    extension: vegas
*/
#define kOfxImageEffectPropVegasUpliftGUID "OfxImageEffectPropVegasUpliftGUID"

/** @brief Action the host calls to give an instance the data of an older plug-in it replaces.
    The handle is the image effect instance and outArgs is NULL. inArgs has
    kOfxImageEffectPropVegasUpliftGUID (the older plug-in) and one element per keyframe in
    kOfxPropVegasUpliftKeyframeData, kOfxPropVegasUpliftKeyframeDataLength,
    kOfxPropVegasUpliftKeyframeTime and kOfxPropVegasUpliftKeyframeInterpolation. It may also
    have kOfxPropVegasUpliftData and kOfxPropVegasUpliftDataLength.
*/
#define kOfxImageEffectActionVegasKeyframeUplift "OfxImageEffectActionVegasKeyframeUplift"

/** @brief The older plug-in's data (in the inArgs of kOfxImageEffectActionVegasKeyframeUplift)

    @propdef
    type: pointer
    dimension: 1
    extension: vegas
*/
#define kOfxPropVegasUpliftData "OfxPropVegasUpliftData"

/** @brief Size in bytes of kOfxPropVegasUpliftData

    @propdef
    type: int
    dimension: 1
    extension: vegas
*/
#define kOfxPropVegasUpliftDataLength "OfxPropVegasUpliftDataLength"

/** @brief The older plug-in's data at each keyframe (in the inArgs of
    kOfxImageEffectActionVegasKeyframeUplift)

    @propdef
    type: pointer
    dimension: N
    extension: vegas
*/
#define kOfxPropVegasUpliftKeyframeData "OfxPropVegasUpliftKeyframeData"

/** @brief Size in bytes of each element of kOfxPropVegasUpliftKeyframeData

    @propdef
    type: int
    dimension: N
    extension: vegas
*/
#define kOfxPropVegasUpliftKeyframeDataLength "OfxPropVegasUpliftKeyframeDataLength"

/** @brief Time of each keyframe of kOfxPropVegasUpliftKeyframeData

    @propdef
    type: double
    dimension: N
    extension: vegas
*/
#define kOfxPropVegasUpliftKeyframeTime "OfxPropVegasUpliftKeyframeTime"

/** @brief Interpolation of each keyframe of kOfxPropVegasUpliftKeyframeData
    (one of the kOfxVegasKeyframeInterpolation values)

    @propdef
    type: enum
    dimension: N
    values:
      - OfxVegasKeyframeInterpolationUnknown
      - OfxVegasKeyframeInterpolationLinear
      - OfxVegasKeyframeInterpolationFast
      - OfxVegasKeyframeInterpolationSlow
      - OfxVegasKeyframeInterpolationSmooth
      - OfxVegasKeyframeInterpolationSharp
      - OfxVegasKeyframeInterpolationHold
      - OfxVegasKeyframeInterpolationManual
      - OfxVegasKeyframeInterpolationSplit
    extension: vegas
*/
#define kOfxPropVegasUpliftKeyframeInterpolation "OfxPropVegasUpliftKeyframeInterpolation"


/* ------------------------------------------------------------------------------------------ */
/* Render arguments and instance context */

/** @brief Number of views the host renders: 1 for a single view, 2 for stereo.
    It is in the inArgs of the render action (read only).

    @propdef
    type: int
    dimension: 1
    extension: vegas
*/
#define kOfxImageEffectPropViewsToRender "OfxImageEffectPropViewsToRender"

/** @brief View the render action renders: 0 for a single view or the left eye, 1 for the right
    eye. It is in the inArgs of the render action (read only).

    @propdef
    type: int
    dimension: 1
    extension: vegas
*/
#define kOfxImageEffectPropRenderView "OfxImageEffectPropRenderView"

/** @brief Quality the host wants for this render, from the fastest (draft) to the best.
    It is in the inArgs of the render action (read only).

    @propdef
    type: enum
    dimension: 1
    values:
      - OfxImageEffectPropRenderQualityDraft
      - OfxImageEffectPropRenderQualityPreview
      - OfxImageEffectPropRenderQualityGood
      - OfxImageEffectPropRenderQualityBest
    extension: vegas
*/
#define kOfxImageEffectPropRenderQuality "OfxImageEffectPropRenderQuality"

/** @brief Values of kOfxImageEffectPropRenderQuality. The string of
    kOfxImageEffectPropRenderQualityDraft is also the name of the OFX 1.4 int property
    kOfxImageEffectPropRenderQualityDraft of ofxImageEffect.h (the draft render flag): the same
    macro, two different uses.
*/
#define kOfxImageEffectPropRenderQualityDraft   "OfxImageEffectPropRenderQualityDraft"
#define kOfxImageEffectPropRenderQualityPreview "OfxImageEffectPropRenderQualityPreview"
#define kOfxImageEffectPropRenderQualityGood    "OfxImageEffectPropRenderQualityGood"
#define kOfxImageEffectPropRenderQualityBest    "OfxImageEffectPropRenderQualityBest"

/** @brief Where the effect instance is applied in the host's project: to a media file, a track,
    an event (or its fade in or fade out), the whole project, or as a generator.
    The host sets it on the effect instance (read only).

    @propdef
    type: enum
    dimension: 1
    values:
      - OfxImageEffectPropVegasContextUnknown
      - OfxImageEffectPropVegasContextMedia
      - OfxImageEffectPropVegasContextTrack
      - OfxImageEffectPropVegasContextEvent
      - OfxImageEffectPropVegasContextEventFadeIn
      - OfxImageEffectPropVegasContextEventFadeOut
      - OfxImageEffectPropVegasContextProject
      - OfxImageEffectPropVegasContextGenerator
    extension: vegas
*/
#define kOfxImageEffectPropVegasContext "OfxImageEffectPropVegasContext"

/** @brief Values of kOfxImageEffectPropVegasContext */
#define kOfxImageEffectPropVegasContextUnknown      "OfxImageEffectPropVegasContextUnknown"
#define kOfxImageEffectPropVegasContextMedia        "OfxImageEffectPropVegasContextMedia"
#define kOfxImageEffectPropVegasContextTrack        "OfxImageEffectPropVegasContextTrack"
#define kOfxImageEffectPropVegasContextEvent        "OfxImageEffectPropVegasContextEvent"
#define kOfxImageEffectPropVegasContextEventFadeIn  "OfxImageEffectPropVegasContextEventFadeIn"
#define kOfxImageEffectPropVegasContextEventFadeOut "OfxImageEffectPropVegasContextEventFadeOut"
#define kOfxImageEffectPropVegasContextProject      "OfxImageEffectPropVegasContextProject"
#define kOfxImageEffectPropVegasContextGenerator    "OfxImageEffectPropVegasContextGenerator"


/* ------------------------------------------------------------------------------------------ */
/* Progress suite */

/** @brief Suite name of OfxVegasProgressSuiteV1 (version 1) or
    OfxVegasProgressSuiteV2 (version 2). It is the progress suite of ofxProgress.h with a
    message identifier, which the host can use to translate the message.
*/
#define kOfxVegasProgressSuite "OfxVegasProgressSuite"

/** @brief Version 1 of kOfxVegasProgressSuite */
typedef struct OfxVegasProgressSuiteV1 {
  /** @brief Starts a progress bar showing message; messageid identifies the message */
  OfxStatus (*progressStart)(void *effectInstance,
                             const char *message,
                             const char *messageid);
  /** @brief Sets the progress, from 0 to 1 */
  OfxStatus (*progressUpdate)(void *effectInstance, double progress);
  /** @brief Ends the progress started by progressStart */
  OfxStatus (*progressEnd)(void *effectInstance);
} OfxVegasProgressSuiteV1;

/** @brief Version 2 of kOfxVegasProgressSuite: progressStart takes an extra int flag,
    showTimeWindows. The Support library only uses version 1.
*/
typedef struct OfxVegasProgressSuiteV2 {
  OfxStatus (*progressStart)(void *effectInstance,
                             const char *message,
                             const char *messageid,
                             int showTimeWindows);
  OfxStatus (*progressUpdate)(void *effectInstance, double progress);
  OfxStatus (*progressEnd)(void *effectInstance);
} OfxVegasProgressSuiteV2;


/* ------------------------------------------------------------------------------------------ */
/* Stereoscopic image suite */

/** @brief Suite name of OfxVegasStereoscopicImageSuiteV1 (version 1), with which a plug-in can
    fetch a clip's image for a given view
*/
#define kOfxVegasStereoscopicImageEffectSuite "OfxVegasStereoscopicImageEffectSuite"

/** @brief Version 1 of kOfxVegasStereoscopicImageEffectSuite. The struct tag and the typedef
    name differ: both are kept for compatibility.
*/
typedef struct OfxVegasStereoscopicImageEffectSuiteV1 {
  /** @brief clipGetImage of the OfxImageEffectSuiteV1 for view iview (0: left, 1: right) */
  OfxStatus (*clipGetStereoscopicImage)(OfxImageClipHandle clip,
                                        OfxTime time,
                                        int iview,
                                        OfxRectD *region,
                                        OfxPropertySetHandle *imageHandle);
} OfxVegasStereoscopicImageSuiteV1;


/* ------------------------------------------------------------------------------------------ */
/* Keyframe suite */

/** @brief Suite name of OfxVegasKeyframeSuiteV1 (version 1), which reads and
    sets the interpolation and the slopes of a parameter's keyframes
*/
#define kOfxVegasKeyframeSuite "OfxVegasKeyframeSuite"

/** @brief Keyframe interpolation types: values of kOfxPropVegasUpliftKeyframeInterpolation and
    of the interpolationType of the OfxVegasKeyframeSuiteV1 functions
*/
#define kOfxVegasKeyframeInterpolationUnknown "OfxVegasKeyframeInterpolationUnknown"
#define kOfxVegasKeyframeInterpolationLinear  "OfxVegasKeyframeInterpolationLinear"
#define kOfxVegasKeyframeInterpolationFast    "OfxVegasKeyframeInterpolationFast"
#define kOfxVegasKeyframeInterpolationSlow    "OfxVegasKeyframeInterpolationSlow"
#define kOfxVegasKeyframeInterpolationSmooth  "OfxVegasKeyframeInterpolationSmooth"
#define kOfxVegasKeyframeInterpolationSharp   "OfxVegasKeyframeInterpolationSharp"
#define kOfxVegasKeyframeInterpolationHold    "OfxVegasKeyframeInterpolationHold"
#define kOfxVegasKeyframeInterpolationManual  "OfxVegasKeyframeInterpolationManual"
#define kOfxVegasKeyframeInterpolationSplit   "OfxVegasKeyframeInterpolationSplit"

/** @brief Version 1 of kOfxVegasKeyframeSuite.

    paramGetKeySlopes and paramSetKeySlopes take the slopes of the keyframe at time as variadic
    double arguments (double * for paramGetKeySlopes, double for paramSetKeySlopes), dimension by
    dimension (x, then y, then z for a 1D, 2D or 3D double parameter):
    - kOfxVegasKeyframeInterpolationManual: one slope per dimension;
    - kOfxVegasKeyframeInterpolationSplit: two slopes per dimension, the incoming one first, then
      the outgoing one (x in, x out, y in, y out, ...).
*/
typedef struct OfxVegasKeyframeSuiteV1 {
  /** @brief Gets the interpolation type of the keyframe at time */
  OfxStatus (*paramGetKeyInterpolation)(OfxParamHandle paramHandle,
                                        OfxTime time,
                                        char **interpolationType);
  /** @brief Gets the slopes of the keyframe at time (see above) */
  OfxStatus (*paramGetKeySlopes)(OfxParamHandle paramHandle,
                                 OfxTime time,
                                 ...);
  /** @brief Sets the interpolation type of the keyframe at time */
  OfxStatus (*paramSetKeyInterpolation)(OfxParamHandle paramHandle,
                                        OfxTime time,
                                        const char *interpolationType);
  /** @brief Sets the slopes of the keyframe at time (see above) */
  OfxStatus (*paramSetKeySlopes)(OfxParamHandle paramHandle,
                                 OfxTime time,
                                 ...);
} OfxVegasKeyframeSuiteV1;


/* ------------------------------------------------------------------------------------------ */
/* Windows custom interfaces (HWND interacts) */

/** @brief Suite name of OfxHWNDInteractSuiteV1 (version 1). On Windows, it lets
    a plug-in draw its custom interface in a child window of the host's window instead of an
    OpenGL overlay.
*/
#define kOfxHWndInteractSuite "OfxHWndInteractSuite"

/** @brief Entry point (OfxPluginEntryPoint) of the plug-in's HWND interact, which receives the
    kOfxHWndInteract actions. It is set on the plug-in descriptor.

    @propdef
    type: pointer
    dimension: 1
    extension: vegas
*/
#define kOfxImageEffectPluginPropHWndInteractV1 "OfxImageEffectPluginPropHWndInteractV1"

/** @brief The host's window (HWND) in which the interact creates its window
    (in the inArgs of kOfxHWndInteractActionCreateWindow)

    @propdef
    type: pointer
    dimension: 1
    extension: vegas
*/
#define kOfxHWndInteractPropParent "OfxHWndInteractPropParent"

/** @brief Smallest size (width, height) of the interact's window
    (in the outArgs of kOfxHWndInteractActionCreateWindow)

    @propdef
    type: int
    dimension: 2
    extension: vegas
*/
#define kOfxHWndInteractPropMinSize "OfxHWndInteractPropMinSize"

/** @brief Size (width, height) the interact's window would like to have
    (in the outArgs of kOfxHWndInteractActionCreateWindow).
    The string value is spelled "Preffered": hosts use it as it is.

    @propdef
    type: int
    dimension: 2
    extension: vegas
*/
#define kOfxHWndInteractPropPreferredSize "OfxHWndInteractPropPrefferedSize"

/** @brief Position and size (left, top, width, height) of the interact's window
    (in the inArgs of kOfxHWndInteractActionMoveWindow)

    @propdef
    type: int
    dimension: 4
    extension: vegas
*/
#define kOfxHWndInteractPropLocation "OfxHWndInteractPropLocation"

/** @brief HWND interact action: create the interact's window as a child of
    kOfxHWndInteractPropParent (inArgs), and return kOfxHWndInteractPropMinSize and
    kOfxHWndInteractPropPreferredSize (outArgs)
*/
#define kOfxHWndInteractActionCreateWindow "OfxHWndInteractActionCreateWindow"

/** @brief HWND interact action: move and resize the interact's window to
    kOfxHWndInteractPropLocation (inArgs); outArgs is NULL
*/
#define kOfxHWndInteractActionMoveWindow "OfxHWndInteractActionMoveWindow"

/** @brief HWND interact action: show the interact's window; inArgs and outArgs are NULL */
#define kOfxHWndInteractActionShowWindow "OfxHWndInteractActionShowWindow"

/** @brief HWND interact action: destroy the interact's window; inArgs and outArgs are NULL */
#define kOfxHWndInteractActionDisposeWindow "OfxHWndInteractActionDisposeWindow"

/** @brief Version 1 of kOfxHWndInteractSuite */
typedef struct OfxHWNDInteractSuiteV1 {
  /** @brief Returns, in property, the properties of an HWND interact instance */
  OfxStatus (*interactGetPropertySet)(OfxInteractHandle interactInstance,
                                      OfxPropertySetHandle *property);
  /** @brief Tells the host that the interact needs updating (OFX::HWNDInteract::triggerUpdate) */
  OfxStatus (*interactUpdate)(OfxInteractHandle interactInstance);
} OfxHWNDInteractSuiteV1;

#endif /* _ofxSonyVegas_h_ */
