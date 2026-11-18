/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: off; c-basic-offset: 2 -*- */
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause


/*
  Plugin example demonstrating colourspace handling

  Ported from AcademySoftwareFoundation/openfx (Examples/ColourSpace). Unlike upstream,
  this version has no third-party dependency: it logs with printf-style messages on
  stderr instead of spdlog, and writes its text into the output image with a small
  built-in bitmap font (../include/ofxuFont5x7.H) instead of CImg.
 */
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <new>
#include <string>
#include <cstring>
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxColour.h"
#include "ofx-native-v1.5_aces-v1.3_ocio-v2.3.h"

#include "../include/ofxUtilities.H" // example support utils
#include "../include/ofxuFont5x7.H" // bitmap font, to write text in the output image

// Log a printf-style message on stderr (upstream uses spdlog).
#if defined(__GNUC__) || defined(__clang__)
static void logMessage(const char *level, const char *format, ...) __attribute__((format(printf, 2, 3)));
#endif
static void
logMessage(const char *level, const char *format, ...)
{
  std::fprintf(stderr, "[colourspace] [%s] ", level);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
}
#define LOG_INFO(...) logMessage("info", __VA_ARGS__)
#define LOG_WARN(...) logMessage("warning", __VA_ARGS__)
#define LOG_ERROR(...) logMessage("error", __VA_ARGS__)

// printf-style formatting into a std::string
#if defined(__GNUC__) || defined(__clang__)
static std::string stringPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
static std::string
stringPrintf(const char *format, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  return std::string(buf);
}

enum class ColourManagementStyle
{
  None, Basic, Core, Full, OCIO
};

struct space_info
{
  const char * ui_label;
  const char * ident;
  ColourManagementStyle style;
};

static
ColourManagementStyle ofxstring_to_style(const std::string & style)
{
  if(style == kOfxImageEffectColourManagementNone)
    return ColourManagementStyle::None;
  else if(style == kOfxImageEffectColourManagementBasic)
    return ColourManagementStyle::Basic;
  else if(style == kOfxImageEffectColourManagementCore)
    return ColourManagementStyle::Core;
  else if(style == kOfxImageEffectColourManagementFull)
    return ColourManagementStyle::Full;
  else if(style == kOfxImageEffectColourManagementOCIO)
    return ColourManagementStyle::OCIO;
  else {
    LOG_INFO("unknown colour management style '%s'", style.c_str());
    return ColourManagementStyle::None;
  }
}

// Use macros to automatically fetch the IsBasic and IsCore properties
#define SPACE(ident) { ident##Label, ident, ident##IsBasic ? ColourManagementStyle::Basic : (ident##IsCore ? ColourManagementStyle::Core : ColourManagementStyle::Full) }
#define ROLE(ident, label) { label, ident, ident##IsBasic ? ColourManagementStyle::Basic : (ident##IsCore ? ColourManagementStyle::Core : ColourManagementStyle::Full) }

static space_info input_space_choices[]
{
  { "[Unspecified, accepts anything]", NULL, ColourManagementStyle::Basic },

  // For this example plugin we'll offer every possible space defined in the header file
  // Real plugins do not need this flexibility!
  
  ROLE(kOfxColourspaceRoleAcesInterchange, "ACES interchange role (ACES2065-1)"),
  ROLE(kOfxColourspaceRoleCieXyzD65Interchange, "CIE XYZ D65 interchange role (CIE XYZ D65)"),
  ROLE(kOfxColourspaceRoleColorPicking, "Colour Picking role (Arbitrary display space)"),
  ROLE(kOfxColourspaceRoleColorTiming, "Color Timing role (Arbitrary scene log)"),
  ROLE(kOfxColourspaceRoleCompositingLog, "Compositing Log role (Arbitrary scene log)"),
  ROLE(kOfxColourspaceRoleData, "Data role (Non-colour image)"),
  ROLE(kOfxColourspaceRoleMattePaint, "Matte Paint role"),
  ROLE(kOfxColourspaceRoleSceneLinear, "Scene Linear role (Arbitrary scene linear)"),
  ROLE(kOfxColourspaceRoleTexturePaint, "Texture Paint role (Typically sRGB)"),
  
  SPACE(kOfxColourspaceOfxDisplayHdr),
  SPACE(kOfxColourspaceOfxDisplaySdr),
  SPACE(kOfxColourspaceOfxRaw),
  SPACE(kOfxColourspaceOfxSceneLinear),
  SPACE(kOfxColourspaceOfxSceneLog),
  SPACE(kOfxColourspaceSrgbDisplay),
  SPACE(kOfxColourspaceDisplayp3Display),
  SPACE(kOfxColourspaceRec1886Rec709Display),
  SPACE(kOfxColourspaceRec1886Rec2020Display),
  SPACE(kOfxColourspaceRec2100HlgDisplay),
  SPACE(kOfxColourspaceRec2100PqDisplay),
  SPACE(kOfxColourspaceSt2084P3d65Display),
  SPACE(kOfxColourspaceP3d65Display),
  SPACE(kOfxColourspaceACES20651),
  SPACE(kOfxColourspaceACEScc),
  SPACE(kOfxColourspaceACEScct),
  SPACE(kOfxColourspaceACEScg),
  SPACE(kOfxColourspaceLinP3d65),
  SPACE(kOfxColourspaceLinRec2020),
  SPACE(kOfxColourspaceLinRec709Srgb),
  SPACE(kOfxColourspaceG18Rec709Tx),
  SPACE(kOfxColourspaceG22Ap1Tx),
  SPACE(kOfxColourspaceG22Rec709Tx),
  SPACE(kOfxColourspaceG24Rec709Tx),
  SPACE(kOfxColourspaceSrgbEncodedAp1Tx),
  SPACE(kOfxColourspaceSrgbEncodedP3d65Tx), 
  SPACE(kOfxColourspaceSrgbTx),
  SPACE(kOfxColourspaceRaw),
  SPACE(kOfxColourspaceCIEXYZD65),
  SPACE(kOfxColourspaceP3d60Display),
  SPACE(kOfxColourspaceP3DciDisplay),
  SPACE(kOfxColourspaceADX10),
  SPACE(kOfxColourspaceADX16),
  SPACE(kOfxColourspaceLinArriWideGamut3),
  SPACE(kOfxColourspaceArriLogc3Ei800),
  SPACE(kOfxColourspaceLinArriWideGamut4),
  SPACE(kOfxColourspaceArriLogc4),
  SPACE(kOfxColourspaceBmdfilmWidegamutGen5),
  SPACE(kOfxColourspaceDavinciIntermediateWidegamut),
  SPACE(kOfxColourspaceLinBmdWidegamutGen5),
  SPACE(kOfxColourspaceLinDavinciWidegamut),
  SPACE(kOfxColourspaceCanonlog2CinemagamutD55),
  SPACE(kOfxColourspaceCanonlog3CinemagamutD55),
  SPACE(kOfxColourspaceLinCinemagamutD55),
  SPACE(kOfxColourspaceLinVgamut),
  SPACE(kOfxColourspaceVlogVgamut),
  SPACE(kOfxColourspaceLinRedwidegamutrgb),
  SPACE(kOfxColourspaceLog3g10Redwidegamutrgb),
  SPACE(kOfxColourspaceLinSgamut3),
  SPACE(kOfxColourspaceLinSgamut3cine),
  SPACE(kOfxColourspaceLinVeniceSgamut3),
  SPACE(kOfxColourspaceLinVeniceSgamut3cine),
  SPACE(kOfxColourspaceSlog3Sgamut3),
  SPACE(kOfxColourspaceSlog3Sgamut3cine),
  SPACE(kOfxColourspaceSlog3VeniceSgamut3),
  SPACE(kOfxColourspaceSlog3VeniceSgamut3cine),
  SPACE(kOfxColourspaceCameraRec709)
};

static space_info output_space_choices[]
{
  { "[Same as input]", "OfxColourspace_Source", ColourManagementStyle::Basic },

  // For this example plugin we'll offer every possible space defined in the header file
  // Real plugins do not need this flexibility!
  
  ROLE(kOfxColourspaceRoleAcesInterchange, "ACES interchange role (ACES2065-1)"),
  ROLE(kOfxColourspaceRoleCieXyzD65Interchange, "CIE XYZ D65 interchange role (CIE XYZ D65)"),
  ROLE(kOfxColourspaceRoleColorPicking, "Colour Picking role (Arbitrary display space)"),
  ROLE(kOfxColourspaceRoleColorTiming, "Color Timing role (Arbitrary scene log)"),
  ROLE(kOfxColourspaceRoleCompositingLog, "Compositing Log role (Arbitrary scene log)"),
  ROLE(kOfxColourspaceRoleData, "Data role (Non-colour image)"),
  ROLE(kOfxColourspaceRoleMattePaint, "Matte Paint role"),
  ROLE(kOfxColourspaceRoleSceneLinear, "Scene Linear role (Arbitrary scene linear)"),
  ROLE(kOfxColourspaceRoleTexturePaint, "Texture Paint role (Typically sRGB)"),
  
  SPACE(kOfxColourspaceOfxDisplayHdr),
  SPACE(kOfxColourspaceOfxDisplaySdr),
  SPACE(kOfxColourspaceOfxRaw),
  SPACE(kOfxColourspaceOfxSceneLinear),
  SPACE(kOfxColourspaceOfxSceneLog),
  SPACE(kOfxColourspaceSrgbDisplay),
  SPACE(kOfxColourspaceDisplayp3Display),
  SPACE(kOfxColourspaceRec1886Rec709Display),
  SPACE(kOfxColourspaceRec1886Rec2020Display),
  SPACE(kOfxColourspaceRec2100HlgDisplay),
  SPACE(kOfxColourspaceRec2100PqDisplay),
  SPACE(kOfxColourspaceSt2084P3d65Display),
  SPACE(kOfxColourspaceP3d65Display),
  SPACE(kOfxColourspaceACES20651),
  SPACE(kOfxColourspaceACEScc),
  SPACE(kOfxColourspaceACEScct),
  SPACE(kOfxColourspaceACEScg),
  SPACE(kOfxColourspaceLinP3d65),
  SPACE(kOfxColourspaceLinRec2020),
  SPACE(kOfxColourspaceLinRec709Srgb),
  SPACE(kOfxColourspaceG18Rec709Tx),
  SPACE(kOfxColourspaceG22Ap1Tx),
  SPACE(kOfxColourspaceG22Rec709Tx),
  SPACE(kOfxColourspaceG24Rec709Tx),
  SPACE(kOfxColourspaceSrgbEncodedAp1Tx),
  SPACE(kOfxColourspaceSrgbTx),
  SPACE(kOfxColourspaceRaw),
  SPACE(kOfxColourspaceCIEXYZD65),
  SPACE(kOfxColourspaceP3d60Display),
  SPACE(kOfxColourspaceP3DciDisplay),
  SPACE(kOfxColourspaceADX10),
  SPACE(kOfxColourspaceADX16),
  SPACE(kOfxColourspaceLinArriWideGamut3),
  SPACE(kOfxColourspaceArriLogc3Ei800),
  SPACE(kOfxColourspaceLinArriWideGamut4),
  SPACE(kOfxColourspaceArriLogc4),
  SPACE(kOfxColourspaceBmdfilmWidegamutGen5),
  SPACE(kOfxColourspaceDavinciIntermediateWidegamut),
  SPACE(kOfxColourspaceLinBmdWidegamutGen5),
  SPACE(kOfxColourspaceLinDavinciWidegamut),
  SPACE(kOfxColourspaceCanonlog2CinemagamutD55),
  SPACE(kOfxColourspaceCanonlog3CinemagamutD55),
  SPACE(kOfxColourspaceLinCinemagamutD55),
  SPACE(kOfxColourspaceLinVgamut),
  SPACE(kOfxColourspaceVlogVgamut),
  SPACE(kOfxColourspaceLinRedwidegamutrgb),
  SPACE(kOfxColourspaceLog3g10Redwidegamutrgb),
  SPACE(kOfxColourspaceLinSgamut3),
  SPACE(kOfxColourspaceLinSgamut3cine),
  SPACE(kOfxColourspaceLinVeniceSgamut3),
  SPACE(kOfxColourspaceLinVeniceSgamut3cine),
  SPACE(kOfxColourspaceSlog3Sgamut3),
  SPACE(kOfxColourspaceSlog3Sgamut3cine),
  SPACE(kOfxColourspaceSlog3VeniceSgamut3),
  SPACE(kOfxColourspaceSlog3VeniceSgamut3cine),
  SPACE(kOfxColourspaceCameraRec709),
  SPACE(kOfxColourspaceSrgbEncodedP3d65Tx)
};

static const char *errMsg(int err) {
  switch (err) {
  case kOfxStatOK: return "OK";
  case kOfxStatFailed : return "Failed";
  case kOfxStatErrFatal: return "ErrFatal";
  case kOfxStatErrUnknown: return "ErrUnknown";
  case kOfxStatErrMissingHostFeature: return "ErrMissingHostFeature";
  case kOfxStatErrUnsupported: return "ErrUnsupported";
  case kOfxStatErrExists : return "ErrExists ";
  case kOfxStatErrFormat: return "ErrFormat";
  case kOfxStatErrMemory : return "ErrMemory";
  case kOfxStatErrBadHandle: return "ErrBadHandle";
  case kOfxStatErrBadIndex: return "ErrBadIndex";
  case kOfxStatErrValue: return "ErrValue";
  case kOfxStatReplyYes: return "ReplyYes";
  case kOfxStatReplyNo: return "ReplyNo";
  case kOfxStatReplyDefault: return "ReplyDefault";
  default: return "Unknown!?";
  }
}

// pointers to various bits of the host
OfxHost                 *gHost;
OfxImageEffectSuiteV1 *gEffectHost = 0;
OfxPropertySuiteV1    *gPropHost = 0;
OfxParameterSuiteV1   *gParamHost = 0;
OfxMemorySuiteV1      *gMemoryHost = 0;
OfxMultiThreadSuiteV1 *gThreadHost = 0;
OfxMessageSuiteV1     *gMessageSuite = 0;
OfxInteractSuiteV1    *gInteractHost = 0;

// some flags about the host's behaviour
int gHostSupportsMultipleBitDepths = false;
ColourManagementStyle gHostColourManagementStyle;

// private instance data type
struct MyInstanceData {
  bool isGeneralEffect;

  // handles to the clips we deal with
  OfxImageClipHandle sourceClip;
  OfxImageClipHandle outputClip;

  // handles to our parameters
  OfxParamHandle inputSpaceParam;
  OfxParamHandle fallbackInputSpaceParam;
  OfxParamHandle outputSpaceParam;
};

// The white level and the pixel type for each bit depth
template <class T> struct PixelTraits;
template <> struct PixelTraits<unsigned char> { static float white() { return 255.f; } };
template <> struct PixelTraits<unsigned short> { static float white() { return 65535.f; } };
template <> struct PixelTraits<float> { static float white() { return 1.f; } };

template <class T> static inline T
toPixel(float v)
{
  // round and clamp for integer types (float: no-op)
  const float w = PixelTraits<T>::white();
  if (w == 1.f) {
    return (T)v;
  }
  return (T)std::max(0.f, std::min(w, std::floor(v + 0.5f)));
}

// the address of pixel (x, y) in an RGBA image, or NULL if it is outside the image bounds
template <class T> static inline T *
pixelAddress(void *img, const OfxRectI &bounds, int rowBytes, int x, int y)
{
  if (!img || x < bounds.x1 || x >= bounds.x2 || y < bounds.y1 || y >= bounds.y2)
    return NULL;
  return (T *)((char *)img + (std::ptrdiff_t)(y - bounds.y1) * rowBytes) + 4 * (x - bounds.x1);
}

// copy the source to the output, within the render window (black and transparent
// outside the source image)
template <class T> static void
copyImage(void *src, const OfxRectI &srcRect, int srcRowBytes,
          void *dst, const OfxRectI &dstRect, int dstRowBytes,
          const OfxRectI &window)
{
  for (int y = window.y1; y < window.y2; ++y) {
    for (int x = window.x1; x < window.x2; ++x) {
      T *d = pixelAddress<T>(dst, dstRect, dstRowBytes, x, y);
      if (!d)
        continue;
      const T *s = pixelAddress<T>(src, srcRect, srcRowBytes, x, y);
      for (int c = 0; c < 4; ++c)
        d[c] = s ? s[c] : T(0);
    }
  }
}

/**
 * Draw a line of text with the bitmap font in an RGBA image, within the render window.
 * (x, y) is the bottom-left corner of the text, in pixel coordinates, and each font pixel
 * is sx by sy image pixels. As upstream: the text box background is darkened by 20%,
 * the text is white, and the alpha is at least the text coverage.
 */
template <class T> static void
drawTextT(const std::string &message, int x, int y, int sx, int sy,
          void *image, const OfxRectI &bounds, int rowBytes, const OfxRectI &window)
{
  const float white = PixelTraits<T>::white();
  const float bg_opacity = 0.2f;
  const int cellW = ofxu::kFontCellWidth * sx;
  const int cellH = ofxu::kFontCellHeight * sy;
  const int x2 = x + (int)message.size() * cellW;
  const int y2 = y + cellH;
  for (int iy = std::max(y, window.y1); iy < std::min(y2, window.y2); ++iy) {
    // font row 0 is the top row; the bottom font row (kFontCellHeight - 1) is blank
    const int row = ofxu::kFontCellHeight - 1 - (iy - y) / sy;
    for (int ix = std::max(x, window.x1); ix < std::min(x2, window.x2); ++ix) {
      T *pix = pixelAddress<T>(image, bounds, rowBytes, ix, iy);
      if (!pix)
        continue;
      const int i = (ix - x) / cellW;
      const int col = ((ix - x) % cellW) / sx;
      const bool on = ofxu::fontPixel(message[i], col, row);
      for (int c = 0; c < 3; ++c)
        pix[c] = on ? toPixel<T>(white) : toPixel<T>(pix[c] * (1.f - bg_opacity));
      if (on)
        pix[3] = toPixel<T>(white);
    }
  }
}

static void
drawText(const std::string &message, int x, int y, int sx, int sy,
         void *image, const OfxRectI &bounds, int rowBytes, int bits, const OfxRectI &window)
{
  switch (bits) {
  case 8:
    drawTextT<unsigned char>(message, x, y, sx, sy, image, bounds, rowBytes, window);
    break;
  case 16:
    drawTextT<unsigned short>(message, x, y, sx, sy, image, bounds, rowBytes, window);
    break;
  case 32:
    drawTextT<float>(message, x, y, sx, sy, image, bounds, rowBytes, window);
    break;
  }
}



/* mandatory function to set up the host structures */


// Convenience wrapper to get private data 
static MyInstanceData *
getMyInstanceData( OfxImageEffectHandle effect)
{
  // get the property handle for the plugin
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // get my data pointer out of that
  MyInstanceData *myData = 0;
  gPropHost->propGetPointer(effectProps,  kOfxPropInstanceData, 0, 
			    (void **) &myData);
  return myData;
}

/** @brief Called at load */
static OfxStatus
onLoad(void)
{
  return kOfxStatOK;
}

/** @brief Called before unload */
static OfxStatus
onUnLoad(void)
{
  return kOfxStatOK;
}

//  instance construction
static OfxStatus
createInstance( OfxImageEffectHandle effect)
{
  // get a pointer to the effect properties
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // get a pointer to the effect's parameter set
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  // make my private instance data
  MyInstanceData *myData = new MyInstanceData;
  const char *context = 0;

  // is this instance a general effect ?
  gPropHost->propGetString(effectProps, kOfxImageEffectPropContext, 0,  &context);
  myData->isGeneralEffect = context && (strcmp(context, kOfxImageEffectContextGeneral) == 0);

  // cache param handles
  gParamHost->paramGetHandle(paramSet, "input_colourspace", &myData->inputSpaceParam, 0);
  gParamHost->paramGetHandle(paramSet, "fallback_colourspace", &myData->fallbackInputSpaceParam, 0);
  gParamHost->paramGetHandle(paramSet, "output_colourspace", &myData->outputSpaceParam, 0);

  // cache clip handles
  gEffectHost->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &myData->sourceClip, 0);
  gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &myData->outputClip, 0);
  
  // set my private instance data
  gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) myData);

  return kOfxStatOK;
}

// instance destruction
static OfxStatus
destroyInstance( OfxImageEffectHandle  effect)
{
  // get my instance data
  MyInstanceData *myData = getMyInstanceData(effect);

  // and delete it
  if(myData)
    delete myData;
  return kOfxStatOK;
}

// tells the host what region we are capable of filling
static OfxStatus
getSpatialRoD( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);

  OfxTime time;
  gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  
  // my RoD is the same as my input's
  OfxRectD rod;
  gEffectHost->clipGetRegionOfDefinition(myData->sourceClip, time, &rod);

  // set the rod in the out args
  gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, &rod.x1);

  return kOfxStatOK;
}

// tells the host how much of the input we need to fill the given window
static OfxStatus
getSpatialRoI( OfxImageEffectHandle  /*effect*/,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // get the RoI the effect is interested in from inArgs
  OfxRectD roi;
  gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRegionOfInterest, 4, &roi.x1);

  // the input needed is the same as the output, so set that on the source clip
  gPropHost->propSetDoubleN(outArgs, "OfxImageClipPropRoI_Source", 4, &roi.x1);

  return kOfxStatOK;
}

// Tells the host how many frames we can fill. Only called in the general context.
// This shows the default behaviour; shown for illustrative purposes.
static OfxStatus
getTemporalDomain( OfxImageEffectHandle  effect,  OfxPropertySetHandle /*inArgs*/,  OfxPropertySetHandle outArgs)
{
  MyInstanceData *myData = getMyInstanceData(effect);

  double sourceRange[2];
  
  // get the frame range of the source clip
  OfxPropertySetHandle props; gEffectHost->clipGetPropertySet(myData->sourceClip, &props);
  gPropHost->propGetDoubleN(props, kOfxImageEffectPropFrameRange, 2, sourceRange);

  // set it on the out args
  gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropFrameRange, 2, sourceRange);
  
  return kOfxStatOK;
}


// Set our clip preferences 
static OfxStatus 
getClipPreferences( OfxImageEffectHandle effect, OfxPropertySetHandle /*inArgs*/, OfxPropertySetHandle outArgs)
{
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);

  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // retrieve the colour management style that the host has decided we shall use
  ColourManagementStyle active_style = ColourManagementStyle::None;
  const char * style = NULL;
  if(gPropHost->propGetString(effectProps, kOfxImageEffectPropColourManagementStyle, 0, &style) == kOfxStatOK && style) {
    active_style = ofxstring_to_style(style);
  }

  // get the component type and bit depth of our main input
  int  bitDepth;
  bool isRGBA;
  ofxuClipGetFormat(myData->sourceClip, bitDepth, isRGBA, true); // get the unmapped clip component

  // get the strings used to label the various bit depths
  const char *bitDepthStr = bitDepth == 8 ? kOfxBitDepthByte : (bitDepth == 16 ? kOfxBitDepthShort : kOfxBitDepthFloat);
  const char *componentStr = isRGBA ? kOfxImageComponentRGBA : kOfxImageComponentAlpha;

  const char * preferredInputSpace = NULL;
  gParamHost->paramGetValue(myData->inputSpaceParam, &preferredInputSpace);
  if(preferredInputSpace && preferredInputSpace[0] == '\0')
    preferredInputSpace = NULL;

  const char * fallbackInputSpace = NULL;
  gParamHost->paramGetValue(myData->fallbackInputSpaceParam, &fallbackInputSpace);
  if(fallbackInputSpace && fallbackInputSpace[0] == '\0')
    fallbackInputSpace = NULL;
  
  // set our output component and bitdepth to be the same same as the input
  gPropHost->propSetString(outArgs, "OfxImageClipPropComponents_Output", 0, componentStr);
  if(gHostSupportsMultipleBitDepths)
    gPropHost->propSetString(outArgs, "OfxImageClipPropDepth_Output", 0, bitDepthStr);

  // Colour management -- preferred colourspaces, in order (most preferred first)
  if (active_style != ColourManagementStyle::None) {
    LOG_INFO("Specifying preferred input colourspaces");
    if(preferredInputSpace == NULL) {
      LOG_INFO("Not specifying any preference");
    } else {
      const char* colourSpaces[] = {
	preferredInputSpace, // this is the user's choice
	fallbackInputSpace ? fallbackInputSpace : kOfxColourspaceRoleSceneLinear,
	kOfxColourspaceRoleSceneLinear};
      for (int i = 0; i < (int)(sizeof(colourSpaces) / sizeof(colourSpaces[0])); i++) {
	LOG_INFO("Specifying preferred input colourspace #%d = %s", i+1, colourSpaces[i]);
	gPropHost->propSetString(outArgs, kOfxImageClipPropPreferredColourspaces "_Source",
				 i, colourSpaces[i]);
      }
    }
  } else {
    LOG_INFO("Host does not support colour management (this example won't be very interesting)");
  }

  return kOfxStatOK;
}

static OfxStatus 
getOutputColourspace( OfxImageEffectHandle  effect,  OfxPropertySetHandle /*inArgs*/,  OfxPropertySetHandle outArgs)
{
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);
  
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // retrieve the colour management style that the host has decided we shall use
  ColourManagementStyle active_style = ColourManagementStyle::None;
  const char * style = NULL;
  if(gPropHost->propGetString(effectProps, kOfxImageEffectPropColourManagementStyle, 0, &style) == kOfxStatOK && style) {
    active_style = ofxstring_to_style(style);
  }

  OfxStatus status = kOfxStatReplyDefault;
  
  if (active_style != ColourManagementStyle::None) {
  
    // We could access the colourspace set on our input clip(s) here,
    // if it affects the output space logic.

    // We could also check kOfxImageClipPropPreferredColourspaces from
    // inArgs here, as the host may have set that to indicate its
    // preferred output spaces
    
    // Get the colourspace from the parameter
    const char * outputSpace = NULL;
    gParamHost->paramGetValue(myData->outputSpaceParam, &outputSpace);
  
    if (outputSpace == NULL || outputSpace[0] == '\0') {
      LOG_INFO("Not specifying an output colourspace");
    } else {
      LOG_INFO("Specifying output colourspaces = %s", outputSpace);
      // Set the selected colourspace in outArgs
      gPropHost->propSetString(outArgs, kOfxImageClipPropColourspace, 0, outputSpace);
      status = kOfxStatOK;
    }
  } else {
    LOG_INFO("Host does not support colour management (this example won't be very interesting)");
    status = kOfxStatFailed;
  }
   
  return status;
}

// are the settings of the effect performing an identity operation
static OfxStatus
isIdentity( OfxImageEffectHandle  /*effect*/,
	    OfxPropertySetHandle /*inArgs*/,
	    OfxPropertySetHandle /*outArgs*/)
{
  return kOfxStatReplyDefault;  // this example is never identity; always render
}

////////////////////////////////////////////////////////////////////////////////
// function called when the instance has been changed by anything
static OfxStatus
instanceChanged( OfxImageEffectHandle  /*effect*/,
		 OfxPropertySetHandle /*inArgs*/,
		 OfxPropertySetHandle /*outArgs*/)
{
  // nothing to do: the colourspace params are clip preferences slave params
  // (kOfxImageEffectPropClipPreferencesSlaveParam), so the host calls the clip
  // preferences action when they change
  return kOfxStatReplyDefault;
}


static std::string getClipColourspace(const OfxImageClipHandle clip) {
    OfxPropertySetHandle clipProps;
    gEffectHost->clipGetPropertySet(clip, &clipProps);
    const char *tmpStr = NULL;
    OfxStatus status = gPropHost->propGetString(clipProps, kOfxImageClipPropColourspace, 0, &tmpStr);
    if (status == kOfxStatOK && tmpStr) {
      return std::string(tmpStr);
    } else {
      LOG_INFO("Can't get clip's colourspace; propGetString returned %s", errMsg(status));
      return std::string("unspecified");
    }

}

static OfxStatus render( OfxImageEffectHandle  instance,
                         OfxPropertySetHandle inArgs,
                         OfxPropertySetHandle /*outArgs*/)
{
  // get the render window and the time from the inArgs
  OfxTime time;
  OfxRectI renderWindow;
  OfxStatus status = kOfxStatOK;

  gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

  double renderScale[2];
  OfxStatus st = gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2,
                                          renderScale);
  if (st != kOfxStatOK) {
    LOG_WARN("Can't get render scale! %s", errMsg(st));
    renderScale[0] = renderScale[1] = 1.0;
  }
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(instance);

  // property handles and members of each image
  // in reality, we would put this in a struct as the C++ support layer does
  OfxPropertySetHandle sourceImg = NULL, outputImg = NULL;
  int srcRowBytes, srcBitDepth, dstRowBytes, dstBitDepth;
  bool srcIsAlpha, dstIsAlpha;
  OfxRectI dstRect, srcRect;
  void *src, *dst;

  std::string inputColourspace = getClipColourspace(myData->sourceClip);
  LOG_INFO("source clip colourspace = %s", inputColourspace.c_str());
  std::string outputColourspace = getClipColourspace(myData->outputClip);
  LOG_INFO("output clip colourspace = %s", outputColourspace.c_str());

  // In a real plugin, the render behaviour will change based on the
  // input and output colourspaces set in the clips.
  //
  // Note that a clip's input colourspace can be *any* valid
  // colourspace identifier. It is *not* guaranteed to be:
  //
  // - a space listed in the plugin's preferences for the clip
  // - a valid space for the colour management style that the plugin supports
  //
  // The output colourspace can always be expected to be the space
  // that was set in the call to getOutputColourspace() - unless that
  // was "OfxColourspace_<input_clip>" in which case the clip will have
  // the actual colourspace identifier set.

  try {
    // get the source image
    sourceImg = ofxuGetImage(myData->sourceClip, time, srcRowBytes, srcBitDepth, srcIsAlpha, srcRect, src);
    if(sourceImg == NULL) throw OfxuNoImageException();

    // get the output image
    outputImg = ofxuGetImage(myData->outputClip, time, dstRowBytes, dstBitDepth, dstIsAlpha, dstRect, dst);
    if(outputImg == NULL) throw OfxuNoImageException();

    // see if they have the same depths and components
    if(srcBitDepth != dstBitDepth || srcIsAlpha != dstIsAlpha || dstIsAlpha) {
      throw OfxuStatusException(kOfxStatErrImageFormat);
    }

    // Lay out the text from the top-left corner of the source RoD (in canonical
    // coordinates), converted to pixel coordinates at this render scale, so that it
    // doesn't depend on the render window (tiles).
    OfxRectD rod;
    gEffectHost->clipGetRegionOfDefinition(myData->sourceClip, time, &rod);
    OfxPropertySetHandle clipProps;
    gEffectHost->clipGetPropertySet(myData->sourceClip, &clipProps);
    double par = 1.0;
    gPropHost->propGetDouble(clipProps, kOfxImagePropPixelAspectRatio, 0, &par);
    if (!(par > 0.0))
      par = 1.0;
    const double xScale = renderScale[0] / par; // pixels per canonical unit, horizontally
    const double yScale = renderScale[1];
    // font pixel size: lines about 50 pixels high at render scale 1, as upstream
    const int sx = std::max(1, (int)std::floor(6 * xScale + 0.5));
    const int sy = std::max(1, (int)std::floor(6 * yScale + 0.5));
    const int lineHeight = ofxu::kFontCellHeight * sy;
    const int xstart = (int)std::floor((rod.x1 + 100) * xScale);
    int ystart = (int)std::floor((rod.y2 - 100) * yScale) - lineHeight;

    LOG_INFO("Rendering window (%d,%d)-(%d,%d) of %dx%d image @%d,%d, depth=%d",
             renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2,
             srcRect.x2 - srcRect.x1, srcRect.y2 - srcRect.y1, srcRect.x1, srcRect.y1, dstBitDepth);

    // Just copy from source to dest, and draw some text
    switch (dstBitDepth) {
    case 8:
      copyImage<unsigned char>(src, srcRect, srcRowBytes, dst, dstRect, dstRowBytes, renderWindow);
      break;
    case 16:
      copyImage<unsigned short>(src, srcRect, srcRowBytes, dst, dstRect, dstRowBytes, renderWindow);
      break;
    case 32:
      copyImage<float>(src, srcRect, srcRowBytes, dst, dstRect, dstRowBytes, renderWindow);
      break;
    default:
      throw OfxuStatusException(kOfxStatErrImageFormat);
    }
    const std::string lines[] = {
      stringPrintf("Image: %dx%d, depth=%d, scale=%.2fx%.2f",
                   (int)std::floor((rod.x2 - rod.x1) * xScale + 0.5), (int)std::floor((rod.y2 - rod.y1) * yScale + 0.5),
                   dstBitDepth, renderScale[0], renderScale[1]),
      "input colourspace: " + inputColourspace,
      "output colourspace: " + outputColourspace,
    };
    for (const std::string &line : lines) {
      drawText(line, xstart, ystart, sx, sy, dst, dstRect, dstRowBytes, dstBitDepth, renderWindow);
      ystart -= lineHeight;
    }
  }
  catch(OfxuNoImageException &) {
    // if we were interrupted, the failed fetch is fine, just return kOfxStatOK
    // otherwise, something weird happened
    if(!gEffectHost->abort(instance)) {
      status = kOfxStatFailed;
    }
  }
  catch(OfxuStatusException &ex) {
    status = ex.status();
  }

  // release the data pointers
  if(sourceImg)
    gEffectHost->clipReleaseImage(sourceImg);
  if(outputImg)
    gEffectHost->clipReleaseImage(outputImg);

  return status;
}

//  describe the plugin in context
template<ColourManagementStyle STYLE>
static OfxStatus
describeInContext( OfxImageEffectHandle  effect,  OfxPropertySetHandle /*inArgs*/)
{
  // The host will decide which colour management style it will use
  // when using this plugin, and set its decision into the effect
  // instance properties.
  //
  // We cannot access that here as we don't have an effect instance,
  // so we **assume** that the host has chosen the most complex
  // mutually-supported style.
  
  ColourManagementStyle active_style = std::min(STYLE, gHostColourManagementStyle);
  
  OfxPropertySetHandle clipProps;
  // define the single output clip in both contexts
  gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &clipProps);

  // set the component types we can handle on out output
  gPropHost->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

  // define the single source clip in both contexts
  gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clipProps);

  // set the component types we can handle on our main input
  gPropHost->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

  ////////////////////////////////////////////////////////////////////////////////
  // define the parameters for this context
  OfxPropertySetHandle props;
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  gParamHost->paramDefine(paramSet, kOfxParamTypeStrChoice, "input_colourspace", &props);
  gPropHost->propSetString(props, kOfxPropLabel, 0, "First-choice Input Colourspace");
  int i = 0;
  for(const auto & space : input_space_choices) {
    if(space.style > active_style) {
      continue; // this space isn't available in the negotiated colour style
    }
    if(i == 0) {
      // set first available space as the default
      gPropHost->propSetString(props, kOfxParamPropDefault, 0, space.ident);
    }
    gPropHost->propSetString(props, kOfxParamPropChoiceEnum, i, space.ident);
    gPropHost->propSetString(props, kOfxParamPropChoiceOption, i, space.ui_label);
    gPropHost->propSetInt(props, kOfxParamPropChoiceOrder, i, i);
    i++;
  }

  gParamHost->paramDefine(paramSet, kOfxParamTypeStrChoice, "fallback_colourspace", &props);
  gPropHost->propSetString(props, kOfxPropLabel, 0, "Second-choice Input Colourspace");
  i = 0;
  for(const auto & space : input_space_choices) {
    if(space.style > active_style) {
      continue; // this space isn't available in the negotiated colour style
    }
    if(i == 0) {
      // set first available space as the default
      gPropHost->propSetString(props, kOfxParamPropDefault, 0, space.ident);
    }
    gPropHost->propSetString(props, kOfxParamPropChoiceEnum, i, space.ident);
    gPropHost->propSetString(props, kOfxParamPropChoiceOption, i, space.ui_label);
    gPropHost->propSetInt(props, kOfxParamPropChoiceOrder, i, i);
    i++;
  }
 
  gParamHost->paramDefine(paramSet, kOfxParamTypeStrChoice, "output_colourspace", &props);
  gPropHost->propSetString(props, kOfxPropLabel, 0, "Output Colourspace");
  i = 0;
  for(const auto & space : output_space_choices) {
    if(space.style > active_style) {
      continue; // this space isn't available in the negotiated colour style
    }
    if(i == 0) {
      // set first available space as the default
      gPropHost->propSetString(props, kOfxParamPropDefault, 0, space.ident);
    }
    gPropHost->propSetString(props, kOfxParamPropChoiceEnum, i, space.ident);
    gPropHost->propSetString(props, kOfxParamPropChoiceOption, i, space.ui_label);
    gPropHost->propSetInt(props, kOfxParamPropChoiceOrder, i, i);
    i++;
  }
  
  // These params affect clip preferences
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropClipPreferencesSlaveParam, 0, "input_colourspace");
  gPropHost->propSetString(effectProps, kOfxImageEffectPropClipPreferencesSlaveParam, 1, "output_colourspace");

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin's description routine
template<ColourManagementStyle STYLE>
static OfxStatus
describe(OfxImageEffectHandle  effect)
{
  // first fetch the host APIs, this cannot be done before this call
  OfxStatus stat;
  if((stat = ofxuFetchHostSuites()) != kOfxStatOK)
    return stat;

  // record a few host features
  gPropHost->propGetInt(gHost->host, kOfxImageEffectPropSupportsMultipleClipDepths, 0, &gHostSupportsMultipleBitDepths);
  const char *tmpStr = NULL;
  stat = gPropHost->propGetString(gHost->host, kOfxImageEffectPropColourManagementStyle, 0, &tmpStr);
  if (stat == kOfxStatOK && tmpStr) {
    gHostColourManagementStyle = ofxstring_to_style(tmpStr);
  } else {
    LOG_INFO("describe: host does not support colour management (err=%s)", errMsg(stat));
    gHostColourManagementStyle = ColourManagementStyle::None;
  }

  // get the property handle for the plugin
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // We can render both fields in a fielded images in one hit if there is no animation
  // So set the flag that allows us to do this
  gPropHost->propSetInt(effectProps, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);

  // say we can support multiple pixel depths and let the clip preferences action deal with it all.
  gPropHost->propSetInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);
  
  // set the bit depths the plugin can handle
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);

  // set some labels and the group it belongs to
  switch(STYLE)
  {
  case ColourManagementStyle::Basic:
    gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "OFX Colourspace Basic Example");
    break;
  case ColourManagementStyle::Core:
    gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "OFX Colourspace Core Example");
    break;
  case ColourManagementStyle::Full:
    gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "OFX Colourspace Full Example");
    break;
  default: // None and OCIO: not instantiated
    break;
  }
  gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "OFX Example");

  // define the contexts we can be used in
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);

  if (gHostColourManagementStyle != ColourManagementStyle::None) {
    // host supports colour management, so tell it what level the plugin supports
    switch(STYLE)
    {
    case ColourManagementStyle::Basic:
      stat = gPropHost->propSetString(effectProps,
				      kOfxImageEffectPropColourManagementStyle, 0,
				      kOfxImageEffectColourManagementBasic);
      break;
    case ColourManagementStyle::Core:
      stat = gPropHost->propSetString(effectProps,
				      kOfxImageEffectPropColourManagementStyle, 0,
				      kOfxImageEffectColourManagementCore);
      break;
    case ColourManagementStyle::Full:
      stat = gPropHost->propSetString(effectProps,
				      kOfxImageEffectPropColourManagementStyle, 0,
				      kOfxImageEffectColourManagementFull);
      break;
    default: // None and OCIO: not instantiated
      break;
    }
    if (stat != kOfxStatOK) {
      LOG_ERROR("setting kOfxImageEffectPropColourManagementStyle prop: stat=%s", errMsg(stat));
    }
  }

  return kOfxStatOK;
}

// Make these actions "silent", i.e. not logged. They can be super verbose.
static const std::set<std::string> silentActions = {"uk.co.thefoundry.FnOfxImageEffectActionGetTransform"};

////////////////////////////////////////////////////////////////////////////////
// The main function

template<ColourManagementStyle STYLE>
static OfxStatus
pluginMain(const char *action, const void *handle, OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  OfxStatus stat = kOfxStatOK;

  if (silentActions.find(action) == silentActions.end())
    LOG_INFO(">>> pluginMain(%s)", action);
  try {
  // cast to appropriate type
  OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

  if(strcmp(action, kOfxActionDescribe) == 0) {
    stat = describe<STYLE>(effect);
  }
  else if(strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
    stat = describeInContext<STYLE>(effect, inArgs);
  }
  else if(strcmp(action, kOfxActionLoad) == 0) {
    stat = onLoad();
  }
  else if(strcmp(action, kOfxActionUnload) == 0) {
    stat = onUnLoad();
  }
  else if(strcmp(action, kOfxActionCreateInstance) == 0) {
    stat = createInstance(effect);
  } 
  else if(strcmp(action, kOfxActionDestroyInstance) == 0) {
    stat = destroyInstance(effect);
  } 
  else if(strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
    stat = isIdentity(effect, inArgs, outArgs);
  }    
  else if(strcmp(action, kOfxImageEffectActionRender) == 0) {
    stat = render(effect, inArgs, outArgs);
  }    
  else if(strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
    stat = getSpatialRoD(effect, inArgs, outArgs);
  }  
  else if(strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0) {
    stat = getSpatialRoI(effect, inArgs, outArgs);
  }  
  else if(strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
    stat = getClipPreferences(effect, inArgs, outArgs);
  }  
  else if(strcmp(action, kOfxActionInstanceChanged) == 0) {
    stat = instanceChanged(effect, inArgs, outArgs);
  }  
  else if(strcmp(action, kOfxImageEffectActionGetTimeDomain) == 0) {
    stat = getTemporalDomain(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetOutputColourspace) == 0) {
    stat = getOutputColourspace(effect, inArgs, outArgs);
  }
  } catch (const std::bad_alloc&) {
    // catch memory
    LOG_ERROR("Caught OFX Plugin Memory error");
    stat = kOfxStatErrMemory;
  } catch ( const std::exception& e ) {
    // standard exceptions
    LOG_ERROR("Caught OFX Plugin error %s", e.what());
    stat = kOfxStatErrUnknown;
  } catch (int err) {
    LOG_ERROR("Caught misc error %d", err);
    stat = err;
  } catch ( ... ) {
    // everything else
    LOG_ERROR("Caught unknown OFX plugin error");
    stat = kOfxStatErrUnknown;
  }
  // other actions to take the default value


  if (silentActions.find(action) == silentActions.end())
    LOG_INFO("<<< pluginMain(%s) = %s", action, errMsg(stat));
  return stat;
}

// function to set the host structure
static void
setHostFunc(OfxHost *hostStruct)
{
  gHost         = hostStruct;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin struct 
static OfxPlugin colourspacePlugin[] =
{
  {       
    kOfxImageEffectPluginApi,
    1,
    "io.aswf.openfx.example.ColourspacePluginBasic",
    1,
    0,
    setHostFunc,
    pluginMain<ColourManagementStyle::Basic>
  },
  {       
    kOfxImageEffectPluginApi,
    1,
    "io.aswf.openfx.example.ColourspacePluginCore",
    1,
    0,
    setHostFunc,
    pluginMain<ColourManagementStyle::Core>
  },
  {       
    kOfxImageEffectPluginApi,
    1,
    "io.aswf.openfx.example.ColourspacePluginFull",
    1,
    0,
    setHostFunc,
    pluginMain<ColourManagementStyle::Full>
  }
  // (can't add ColourManagementStyle::OCIO without adding an unwanted OCIO dependency to this example)
};
   
// the two mandated functions
OfxPlugin *
OfxGetPlugin(int nth)
{
  if(nth < 3)
    return &colourspacePlugin[nth];
  return 0;
}
 
int
OfxGetNumberOfPlugins(void)
{       
  return 3;
}
