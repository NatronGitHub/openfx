// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Ofx Example plugin that shows how an overlay is drawn and interacted with,
  using the OfxDrawSuiteV1 (OFX 1.5) and/or OpenGL.

  It is meant to illustrate certain features of the API, as opposed to being a perfectly
  crafted piece of image processing software.

  Note, that the default bitdepths and components are specified for the source (RGBA + A, 8 + 16 + 32) and that
  this plugin does absolutely no image processing, it assumes the isIdentity action will be called before a render
  and so do nothing.

  This plug-in sets both overlay entry points, so that a host test can tell which one the
  host uses:

  - kOfxImageEffectPluginPropOverlayInteractV1 (overlayMainV1): draws with OpenGL, in
    ORANGE (1, 0.5, 0), with the label "V1 OpenGL" above-right of the crosshair.
  - kOfxImageEffectPluginPropOverlayInteractV2 (overlayMainV2): draws with OfxDrawSuiteV1,
    in MAGENTA (1, 0, 1), with the label "V2 DrawSuite" right of the crosshair, and below it
    a test card that calls every OfxDrawSuiteV1 function: getColour (every
    OfxStandardColour), setColour (opaque and translucent), setLineWidth, setLineStipple
    (every OfxDrawLineStipplePattern), draw (every OfxDrawPrimitive) and drawText (several
    OfxDrawTextAlignment values). The last line of the card says whether all the return
    statuses were as specified; failures are also printed on stderr.

  What the spec says about the two entry points:

  include/ofxImageEffect.h, kOfxImageEffectPluginPropOverlayInteractV2:
    "Sets the entry for an effect's overlay interaction. Unlike
     kOfxImageEffectPluginPropOverlayInteractV1, the overlay interact in the plug-in is expected
     to implement the kOfxInteractActionDraw using the OfxDrawSuiteV1."

  include/ofxInteract.h, kOfxInteractActionDraw:
    "If this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite.
     If this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use OpenGL.
     Some existing plugins may use DrawSuite via kOfxImageEffectPluginPropOverlayInteractV1 if
     it's supported by the host, but this is discouraged."

  Documentation/sources/Reference/ofxInteracts.rst (upstream main):
    "So as of OFX 1.5, effects may use the OfxDrawSuiteV1 instead of OpenGL, if the host
     supports it, by requesting kOfxImageEffectPluginPropOverlayInteractV2 instead of
     kOfxImageEffectPluginPropOverlayInteractV1."
    "If the plugin requests OverlayInteractV2 and the host supports it, then the plugin *must*
     do all drawing with the DrawSuite, not OpenGL."

  The spec does not say explicitly which entry point a host must use when a plug-in sets
  both. The wording above ("instead of", "if the host supports it") implies that a host
  that implements the DrawSuite uses V2, and that a host without it uses V1 (HostSupport
  does this: ImageEffect::Base::getOverlayInteractMainEntry). A host should use only one of
  them for a given overlay, so only one of the two labels should be visible.

  The V2 entry point is only set if the host gives the OfxDrawSuiteV1 (fetchSuite), since it
  could not draw anything otherwise.
 */

#if defined(_WIN32) || defined(WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <new>
#include <string>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxDrawSuite.h"

#include "../include/ofxUtilities.H" // example support utils
#include "../include/ofxuFont5x7.H" // bitmap font, for the V1 (OpenGL) label

#define kPointParam "point"
#define kInvalidArgsParam "testInvalidArgs"

// pointers to various bits of the host
OfxHost               *gHost;
OfxImageEffectSuiteV1 *gEffectHost = 0;
OfxPropertySuiteV1    *gPropHost = 0;
OfxParameterSuiteV1   *gParamHost = 0;
OfxMemorySuiteV1      *gMemoryHost = 0;
OfxMultiThreadSuiteV1 *gThreadHost = 0;
OfxMessageSuiteV1     *gMessageSuite = 0;
OfxInteractSuiteV1    *gInteractHost = 0;
OfxDrawSuiteV1        *gDrawSuite = 0;

// the colours that tell which entry point the host called
static const OfxRGBAColourF kV1Colour = {1.f, 0.5f, 0.f, 1.f}; // orange
static const OfxRGBAColourF kV2Colour = {1.f, 0.f, 1.f, 1.f};  // magenta

// we are always identity as we are just a hack example plugin
static OfxStatus
isIdentity(OfxImageEffectHandle  /*pluginInstance*/,
	   OfxPropertySetHandle /*inArgs*/,
	   OfxPropertySetHandle outArgs)
{
  // set the property in the out args indicating which is the identity clip
  gPropHost->propSetString(outArgs, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
  return kOfxStatOK;
}

// the process code  that the host sees
static OfxStatus render(OfxImageEffectHandle  /*instance*/,
			OfxPropertySetHandle /*inArgs*/,
			OfxPropertySetHandle /*outArgs*/)
{
  // do nothing as this should never be called as isIdentity should always be trapped
  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// the interaction routines, shared by the V1 and V2 entry points
struct MyInteractData {
  bool selected;
  OfxParamHandle pointParam;
  OfxParamHandle invalidArgsParam;
  std::string lastReport; // the last DrawSuite check report printed on stderr

  MyInteractData(OfxParamHandle pParam, OfxParamHandle iParam)
    : selected(false)
    , pointParam(pParam)
    , invalidArgsParam(iParam)
  {
  }
};

// get the interact data from an interact instance
static MyInteractData *
getInteractData(OfxInteractHandle interactInstance)
{
  void *dataV = ofxuGetInteractInstanceData(interactInstance);
  return (MyInteractData *) dataV;
}

// creation of an interact instance
static OfxStatus
interactDescribe(OfxInteractHandle /*interactDescriptor*/)
{

  // and we are good
  return kOfxStatOK;
}

// creation of an interact instance
static OfxStatus
interactCreateInstance(OfxImageEffectHandle pluginInstance,
		       OfxInteractHandle interactInstance)
{
  // get the parameter set for this effect
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(pluginInstance, &paramSet);

  // fetch a handle to the params from the parameter set
  OfxParamHandle pointParam = 0;
  gParamHost->paramGetHandle(paramSet, kPointParam, &pointParam, 0);
  OfxParamHandle invalidArgsParam = 0;
  gParamHost->paramGetHandle(paramSet, kInvalidArgsParam, &invalidArgsParam, 0);

  // make my interact's instance data
  MyInteractData *data = new MyInteractData(pointParam, invalidArgsParam);

  // and set the interact's data pointer
  ofxuSetInteractInstanceData(interactInstance, (void *) data);

  OfxPropertySetHandle interactProps;
  gInteractHost->interactGetPropertySet(interactInstance, &interactProps);

  // slave this interact to the params so redraws are triggered cleanly
  gPropHost->propSetString(interactProps, kOfxInteractPropSlaveToParam, 0, kPointParam);
  gPropHost->propSetString(interactProps, kOfxInteractPropSlaveToParam, 1, kInvalidArgsParam);

  return kOfxStatOK;
}

// destruction of an interact instance
static OfxStatus
interactDestroyInstance(OfxImageEffectHandle  /*pluginInstance*/,
			OfxInteractHandle interactInstance)
{
  MyInteractData *data = getInteractData(interactInstance);
  delete data;
  return kOfxStatOK;
}

// size of the cross hair in screen pixels
#define kXHairSize 10

////////////////////////////////////////////////////////////////////////////////
// V1: draw with OpenGL

// draw text with the bitmap font, one GL quad per font pixel, with its baseline-left
// corner at (x, y) in canonical coordinates, each font pixel being size screen pixels
static void
glDrawBitmapText(const char *text, double x, double y, const double pixelScale[2], int size)
{
  const double w = size * pixelScale[0];
  const double h = size * pixelScale[1];
  glBegin(GL_QUADS);
  for (int i = 0; text[i] != '\0'; ++i) {
    const double cx = x + i * ofxu::kFontCellWidth * w;
    for (int col = 0; col < ofxu::kFontGlyphWidth; ++col) {
      for (int row = 0; row < ofxu::kFontGlyphHeight; ++row) {
        if (ofxu::fontPixel(text[i], col, row)) {
          // row 0 is the top row: the bottom row (6) sits on the baseline
          const double px = cx + col * w;
          const double py = y + (ofxu::kFontGlyphHeight - 1 - row) * h;
          glVertex2d(px, py);
          glVertex2d(px + w, py);
          glVertex2d(px + w, py + h);
          glVertex2d(px, py + h);
        }
      }
    }
  }
  glEnd();
}

static OfxStatus
interactDrawV1(OfxImageEffectHandle  /*pluginInstance*/,
	       OfxInteractHandle interactInstance,
	       OfxPropertySetHandle drawArgs)
{
  // get my private interact data
  MyInteractData *data = getInteractData(interactInstance);

  // get the size of a pixel in the current projection
  double pixelScale[2];
  ofxuGetInteractPixelScale(drawArgs, pixelScale);

  // get my param's value
  double x, y;
  gParamHost->paramGetValue(data->pointParam, &x, &y);

  // make the xhair a constant size on screen by scaling by the pixel scale
  const double dx = kXHairSize * pixelScale[0];
  const double dy = kXHairSize * pixelScale[1];

  // if the we have selected the Xhair, draw it highlit
  if(data->selected)
    glColor3f(1, 1, 1);
  else
    glColor4f(kV1Colour.r, kV1Colour.g, kV1Colour.b, kV1Colour.a);

  // Draw a cross hair, the current coordinate system aligns with the image plane.
  glBegin(GL_LINES);
  glVertex2d(x - dx, y);
  glVertex2d(x + dx, y);
  glVertex2d(x, y - dy);
  glVertex2d(x, y + dy);
  glEnd();

  // the label, above the V2 one so that both can be seen if a host (wrongly) calls both
  glColor4f(kV1Colour.r, kV1Colour.g, kV1Colour.b, kV1Colour.a);
  glDrawBitmapText("V1 OpenGL", x + 14 * pixelScale[0], y + 24 * pixelScale[1], pixelScale, 2);

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// V2: draw with the OfxDrawSuiteV1

// checks the statuses returned by the DrawSuite, and makes a report of the failures
class DrawChecker {
public:
  DrawChecker() : _nChecks(0), _nFailures(0) {}

  // returns true if got is the expected status
  bool check(const char *what, OfxStatus got, OfxStatus expected = kOfxStatOK)
  {
    ++_nChecks;
    if (got == expected) {
      return true;
    }
    ++_nFailures;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "  %s returned %d, expected %d\n", what, (int)got, (int)expected);
    _report += buf;
    return false;
  }

  int nChecks() const { return _nChecks; }
  int nFailures() const { return _nFailures; }
  const std::string &report() const { return _report; }

private:
  int _nChecks;
  int _nFailures;
  std::string _report;
};

static const char *const kStandardColourNames[] = {
  "kOfxStandardColourOverlayBackground",
  "kOfxStandardColourOverlayActive",
  "kOfxStandardColourOverlaySelected",
  "kOfxStandardColourOverlayDeselected",
  "kOfxStandardColourOverlayMarqueeFG",
  "kOfxStandardColourOverlayMarqueeBG",
  "kOfxStandardColourOverlayText",
};
static const OfxStandardColour kStandardColours[] = {
  kOfxStandardColourOverlayBackground,
  kOfxStandardColourOverlayActive,
  kOfxStandardColourOverlaySelected,
  kOfxStandardColourOverlayDeselected,
  kOfxStandardColourOverlayMarqueeFG,
  kOfxStandardColourOverlayMarqueeBG,
  kOfxStandardColourOverlayText,
};
static const int kNStandardColours = (int)(sizeof(kStandardColours) / sizeof(kStandardColours[0]));

static const char *const kStippleNames[] = {"Solid", "Dot", "Dash", "AltDash", "DotDash"};
static const OfxDrawLineStipplePattern kStipples[] = {
  kOfxDrawLineStipplePatternSolid,
  kOfxDrawLineStipplePatternDot,
  kOfxDrawLineStipplePatternDash,
  kOfxDrawLineStipplePatternAltDash,
  kOfxDrawLineStipplePatternDotDash,
};
static const int kNStipples = (int)(sizeof(kStipples) / sizeof(kStipples[0]));

static const float kLineWidths[] = {0.f, 1.f, 2.f, 4.f, 8.f};
static const char *const kLineWidthNames[] = {"0", "1", "2", "4", "8"};
static const int kNLineWidths = (int)(sizeof(kLineWidths) / sizeof(kLineWidths[0]));

static const char *const kPrimitiveNames[] = {"Lines", "LineStrip", "LineLoop", "Rectangle", "Polygon", "Ellipse"};
static const OfxDrawPrimitive kPrimitives[] = {
  kOfxDrawPrimitiveLines,
  kOfxDrawPrimitiveLineStrip,
  kOfxDrawPrimitiveLineLoop,
  kOfxDrawPrimitiveRectangle,
  kOfxDrawPrimitivePolygon,
  kOfxDrawPrimitiveEllipse,
};
static const int kNPrimitives = (int)(sizeof(kPrimitives) / sizeof(kPrimitives[0]));

static const char *const kAlignmentNames[] = {
  "Left|Baseline", "Right|Baseline", "CenterH|Baseline", "Left|Top", "Left|Bottom", "Left|CenterV", "CenterH|CenterV",
};
static const int kAlignments[] = {
  kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline,
  kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentBaseline,
  kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentBaseline,
  kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop,
  kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom,
  kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentCenterV,
  kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV,
};
static const int kNAlignments = (int)(sizeof(kAlignments) / sizeof(kAlignments[0]));

// Everything in the V2 overlay is laid out in screen pixels (u to the right, v up) from
// the point param, converted to canonical coordinates with the pixel scale, so that the
// test card keeps the same size on screen whatever the zoom.
struct Layout {
  double x, y;     // the origin (the point param), in canonical coordinates
  double sx, sy;   // the pixel scale (canonical units per screen pixel)

  OfxPointD at(double u, double v) const
  {
    OfxPointD p = {x + u * sx, y + v * sy};
    return p;
  }
};

static OfxStatus
interactDrawV2(OfxImageEffectHandle  /*pluginInstance*/,
	       OfxInteractHandle interactInstance,
	       OfxPropertySetHandle drawArgs)
{
  if (!gDrawSuite) {
    // can't happen: overlayMainV2 is only set if the suite was fetched
    return kOfxStatFailed;
  }

  // get my private interact data
  MyInteractData *data = getInteractData(interactInstance);

  // get the size of a pixel in the current projection
  double pixelScale[2];
  ofxuGetInteractPixelScale(drawArgs, pixelScale);

  // get my params' values
  double x, y;
  gParamHost->paramGetValue(data->pointParam, &x, &y);
  int testInvalidArgs = 0;
  if (data->invalidArgsParam)
    gParamHost->paramGetValue(data->invalidArgsParam, &testInvalidArgs);

  OfxDrawContextHandle context = 0;
  gPropHost->propGetPointer(drawArgs, kOfxInteractPropDrawContext, 0, (void**)&context);
  if (!context) {
    std::fprintf(stderr, "DrawSuite example: no kOfxInteractPropDrawContext in the kOfxInteractActionDraw inArgs\n");
    return kOfxStatFailed;
  }

  const Layout L = {x, y, pixelScale[0], pixelScale[1]};
  DrawChecker chk;
  char what[128];

  const OfxRGBAColourF white = {1.f, 1.f, 1.f, 1.f};
  const OfxRGBAColourF translucentWhite = {1.f, 1.f, 1.f, 0.5f};

  // the colour for the titles: the host's text colour, else magenta
  OfxRGBAColourF textColour = kV2Colour;
  if (!chk.check("getColour(kOfxStandardColourOverlayText)",
                 gDrawSuite->getColour(context, kOfxStandardColourOverlayText, &textColour))) {
    textColour = kV2Colour;
  }

  chk.check("setLineWidth(1)", gDrawSuite->setLineWidth(context, 1.f));
  chk.check("setLineStipple(Solid)", gDrawSuite->setLineStipple(context, kOfxDrawLineStipplePatternSolid));

  // the crosshair (white if selected, as in the OpenGL example), and a dotted circle
  // in the host's "active" colour around it when selected
  if (data->selected) {
    OfxRGBAColourF col = white;
    if (!chk.check("getColour(kOfxStandardColourOverlayActive)",
                   gDrawSuite->getColour(context, kOfxStandardColourOverlayActive, &col))) {
      col = white;
    }
    chk.check("setColour(active)", gDrawSuite->setColour(context, &col));
    chk.check("setLineStipple(Dot)", gDrawSuite->setLineStipple(context, kOfxDrawLineStipplePatternDot));
    const OfxPointD circle[2] = {L.at(-2 * kXHairSize, -2 * kXHairSize), L.at(2 * kXHairSize, 2 * kXHairSize)};
    chk.check("draw(Ellipse) selection", gDrawSuite->draw(context, kOfxDrawPrimitiveEllipse, circle, 2));
    chk.check("setLineStipple(Solid)", gDrawSuite->setLineStipple(context, kOfxDrawLineStipplePatternSolid));
    chk.check("setColour(white)", gDrawSuite->setColour(context, &white));
  } else {
    chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
  }
  const OfxPointD xhair[4] = {
    L.at(-kXHairSize, 0), L.at(kXHairSize, 0),
    L.at(0, -kXHairSize), L.at(0, kXHairSize),
  };
  chk.check("draw(Lines) crosshair", gDrawSuite->draw(context, kOfxDrawPrimitiveLines, xhair, 4));

  // the label that says which entry point the host called
  chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
  {
    const OfxPointD pos = L.at(14, 6);
    chk.check("drawText(label)", gDrawSuite->drawText(context, "V2 DrawSuite", &pos,
                                                      kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
  }

  // A: getColour for every OfxStandardColour, each drawn as a filled swatch (or an
  //    outline if the host has no such colour); then setColour with a translucent colour
  //    over an opaque one ("The host should use "over" compositing when using a
  //    non-opaque colour.")
  {
    const OfxPointD title = L.at(0, -36);
    chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
    chk.check("drawText(title A)", gDrawSuite->drawText(context, "getColour (0-6), setColour (alpha 1, 0.5):", &title,
                                                        kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
    for (int i = 0; i < kNStandardColours; ++i) {
      OfxRGBAColourF col = {0.f, 0.f, 0.f, 0.f};
      std::snprintf(what, sizeof(what), "getColour(%s)", kStandardColourNames[i]);
      const bool ok = chk.check(what, gDrawSuite->getColour(context, kStandardColours[i], &col));
      const OfxPointD rect[2] = {L.at(i * 30, -64), L.at(i * 30 + 24, -44)};
      if (ok) {
        chk.check("setColour(standard colour)", gDrawSuite->setColour(context, &col));
        chk.check("draw(Rectangle) swatch", gDrawSuite->draw(context, kOfxDrawPrimitiveRectangle, rect, 2));
      } else {
        // an outline with a cross, in magenta: "no such colour"
        chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
        const OfxPointD outline[4] = {rect[0], L.at(i * 30 + 24, -64), rect[1], L.at(i * 30, -44)};
        chk.check("draw(LineLoop) missing swatch", gDrawSuite->draw(context, kOfxDrawPrimitiveLineLoop, outline, 4));
        const OfxPointD cross[4] = {rect[0], rect[1], outline[1], outline[3]};
        chk.check("draw(Lines) missing swatch", gDrawSuite->draw(context, kOfxDrawPrimitiveLines, cross, 4));
      }
      chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
      const char digit[2] = {(char)('0' + i), '\0'};
      const OfxPointD pos = L.at(i * 30 + 12, -68);
      chk.check("drawText(swatch index)", gDrawSuite->drawText(context, digit, &pos,
                                                               kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentTop));
    }
    chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
    const OfxPointD opaque[2] = {L.at(230, -64), L.at(270, -44)};
    chk.check("draw(Rectangle) opaque", gDrawSuite->draw(context, kOfxDrawPrimitiveRectangle, opaque, 2));
    chk.check("setColour(alpha 0.5)", gDrawSuite->setColour(context, &translucentWhite));
    const OfxPointD translucent[2] = {L.at(250, -60), L.at(290, -48)};
    chk.check("draw(Rectangle) translucent", gDrawSuite->draw(context, kOfxDrawPrimitiveRectangle, translucent, 2));
  }

  // B: setLineStipple, with every OfxDrawLineStipplePattern
  {
    const OfxPointD title = L.at(0, -96);
    chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
    chk.check("drawText(title B)", gDrawSuite->drawText(context, "setLineStipple:", &title,
                                                        kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
    for (int i = 0; i < kNStipples; ++i) {
      const double v = -110 - i * 16;
      chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
      std::snprintf(what, sizeof(what), "setLineStipple(%s)", kStippleNames[i]);
      chk.check(what, gDrawSuite->setLineStipple(context, kStipples[i]));
      const OfxPointD line[2] = {L.at(0, v), L.at(160, v)};
      chk.check("draw(Lines) stipple", gDrawSuite->draw(context, kOfxDrawPrimitiveLines, line, 2));
      chk.check("setLineStipple(Solid)", gDrawSuite->setLineStipple(context, kOfxDrawLineStipplePatternSolid));
      chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
      const OfxPointD pos = L.at(170, v);
      chk.check("drawText(stipple name)", gDrawSuite->drawText(context, kStippleNames[i], &pos,
                                                               kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentCenterV));
    }
  }

  // C: setLineWidth ("Use width 0 for a single pixel line or non-zero for a smooth line of
  //    the desired width")
  {
    const OfxPointD title = L.at(0, -200);
    chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
    chk.check("drawText(title C)", gDrawSuite->drawText(context, "setLineWidth:", &title,
                                                        kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
    for (int i = 0; i < kNLineWidths; ++i) {
      const double u = 10 + i * 30;
      chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
      std::snprintf(what, sizeof(what), "setLineWidth(%s)", kLineWidthNames[i]);
      chk.check(what, gDrawSuite->setLineWidth(context, kLineWidths[i]));
      const OfxPointD line[2] = {L.at(u, -210), L.at(u, -240)};
      chk.check("draw(Lines) width", gDrawSuite->draw(context, kOfxDrawPrimitiveLines, line, 2));
      chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
      const OfxPointD pos = L.at(u, -244);
      chk.check("drawText(width)", gDrawSuite->drawText(context, kLineWidthNames[i], &pos,
                                                        kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentTop));
    }
    chk.check("setLineWidth(1)", gDrawSuite->setLineWidth(context, 1.f));
  }

  // D: draw, with every OfxDrawPrimitive, each in a 40x40 cell
  {
    const OfxPointD title = L.at(0, -276);
    chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
    chk.check("drawText(title D)", gDrawSuite->drawText(context, "draw:", &title,
                                                        kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
    const double kPi = 3.14159265358979323846;
    for (int i = 0; i < kNPrimitives; ++i) {
      const double u0 = i * 80, u1 = u0 + 40, v0 = -330, v1 = -290;
      const double um = (u0 + u1) / 2, vm = (v0 + v1) / 2;
      OfxPointD pts[6];
      int n = 0;
      switch (kPrimitives[i]) {
      case kOfxDrawPrimitiveLines: // 2 separated lines: an X
        pts[0] = L.at(u0, v0); pts[1] = L.at(u1, v1); pts[2] = L.at(u0, v1); pts[3] = L.at(u1, v0);
        n = 4;
        break;
      case kOfxDrawPrimitiveLineStrip: // an open zigzag
        pts[0] = L.at(u0, v0); pts[1] = L.at(u0 + 13, v1); pts[2] = L.at(u0 + 27, v0); pts[3] = L.at(u1, v1);
        n = 4;
        break;
      case kOfxDrawPrimitiveLineLoop: // a closed triangle outline
        pts[0] = L.at(u0, v0); pts[1] = L.at(u1, v0); pts[2] = L.at(um, v1);
        n = 3;
        break;
      case kOfxDrawPrimitiveRectangle: // a filled rectangle, from 2 opposite corners
        pts[0] = L.at(u0, v0); pts[1] = L.at(u1, v1);
        n = 2;
        break;
      case kOfxDrawPrimitivePolygon: // a filled hexagon
        for (int k = 0; k < 6; ++k) {
          const double a = k * kPi / 3;
          pts[k] = L.at(um + 20 * std::cos(a), vm + 20 * std::sin(a));
        }
        n = 6;
        break;
      case kOfxDrawPrimitiveEllipse: // an ellipse outline, in the rectangle given by 2 opposite corners
        pts[0] = L.at(u0, v0 + 8); pts[1] = L.at(u1, v1 - 8);
        n = 2;
        break;
      }
      chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
      std::snprintf(what, sizeof(what), "draw(%s, %d points)", kPrimitiveNames[i], n);
      chk.check(what, gDrawSuite->draw(context, kPrimitives[i], pts, n));
      chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
      const OfxPointD pos = L.at(um, v0 - 4);
      chk.check("drawText(primitive name)", gDrawSuite->drawText(context, kPrimitiveNames[i], &pos,
                                                                 kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentTop));
    }
  }

  // E: drawText, with several OfxDrawTextAlignment values. The white cross is the
  //    position given to drawText.
  {
    const OfxPointD title = L.at(0, -366);
    chk.check("setColour(text)", gDrawSuite->setColour(context, &textColour));
    chk.check("drawText(title E)", gDrawSuite->drawText(context, "drawText (+ = pos):", &title,
                                                        kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
    for (int i = 0; i < kNAlignments; ++i) {
      const double u = 200, v = -390 - i * 24;
      chk.check("setColour(white)", gDrawSuite->setColour(context, &white));
      const OfxPointD cross[4] = {L.at(u - 4, v), L.at(u + 4, v), L.at(u, v - 4), L.at(u, v + 4)};
      chk.check("draw(Lines) text anchor", gDrawSuite->draw(context, kOfxDrawPrimitiveLines, cross, 4));
      chk.check("setColour(V2 magenta)", gDrawSuite->setColour(context, &kV2Colour));
      const OfxPointD pos = L.at(u, v);
      std::snprintf(what, sizeof(what), "drawText(%s)", kAlignmentNames[i]);
      chk.check(what, gDrawSuite->drawText(context, kAlignmentNames[i], &pos, kAlignments[i]));
    }
  }

  // F: invalid arguments, only if the "Test invalid arguments" param is set, since a host
  //    that doesn't check them may misbehave. The out-of-range values are 7, which is
  //    representable by each of these enums (their values are 0 to 4, 5 or 6).
  if (testInvalidArgs) {
    OfxRGBAColourF col = {0.f, 0.f, 0.f, 0.f};
    chk.check("getColour(invalid std_colour 7)",
              gDrawSuite->getColour(context, (OfxStandardColour)7, &col), kOfxStatErrValue);
    chk.check("setLineStipple(invalid pattern 7)",
              gDrawSuite->setLineStipple(context, (OfxDrawLineStipplePattern)7), kOfxStatErrValue);
    chk.check("setLineStipple(Solid)", gDrawSuite->setLineStipple(context, kOfxDrawLineStipplePatternSolid));
    const OfxPointD pts[2] = {L.at(0, -600), L.at(10, -590)};
    chk.check("draw(invalid primitive 7)",
              gDrawSuite->draw(context, (OfxDrawPrimitive)7, pts, 2), kOfxStatErrValue);
    chk.check("draw(Rectangle, 1 point)",
              gDrawSuite->draw(context, kOfxDrawPrimitiveRectangle, pts, 1), kOfxStatErrValue);
    chk.check("draw(Ellipse, 1 point)",
              gDrawSuite->draw(context, kOfxDrawPrimitiveEllipse, pts, 1), kOfxStatErrValue);
    chk.check("drawText(NULL text)",
              gDrawSuite->drawText(context, NULL, &pts[0], kOfxDrawTextAlignmentLeft), kOfxStatErrValue);
    chk.check("drawText(NULL pos)",
              gDrawSuite->drawText(context, "x", NULL, kOfxDrawTextAlignmentLeft), kOfxStatErrValue);
  }

  // G: the result of the checks
  {
    char status[128];
    if (chk.nFailures() == 0) {
      std::snprintf(status, sizeof(status), "DrawSuite checks: %d OK%s", chk.nChecks(),
                    testInvalidArgs ? " (with invalid args)" : "");
    } else {
      std::snprintf(status, sizeof(status), "DrawSuite checks: %d of %d FAILED (see stderr)", chk.nFailures(), chk.nChecks());
    }
    gDrawSuite->setColour(context, chk.nFailures() == 0 ? &textColour : &kV2Colour);
    const OfxPointD pos = L.at(0, -570);
    gDrawSuite->drawText(context, status, &pos, kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline);

    // print the failures once (not at every redraw)
    if (chk.report() != data->lastReport) {
      if (chk.nFailures() > 0) {
        std::fprintf(stderr, "DrawSuite example: %d of %d DrawSuite checks failed:\n%s",
                     chk.nFailures(), chk.nChecks(), chk.report().c_str());
      }
      data->lastReport = chk.report();
    }
  }

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// pen interaction, shared by V1 and V2

// function reacting to pen motion
static OfxStatus
interactPenMotion(OfxImageEffectHandle  /*pluginInstance*/,
		  OfxInteractHandle interactInstance,
		  OfxPropertySetHandle inArgs)
{
  // get my data handle
  MyInteractData *data = getInteractData(interactInstance);

  // Have we grabbed on a pen down already?
  if(data->selected) {
    // get the pen position
    OfxPointD penPos;
    gPropHost->propGetDoubleN(inArgs, kOfxInteractPropPenPosition, 2, &penPos.x);

    // set the value of the 'point' param
    gParamHost->paramSetValue(data->pointParam, penPos.x, penPos.y);
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

static OfxStatus
interactPenDown(OfxImageEffectHandle  /*pluginInstance*/,
		OfxInteractHandle interactInstance,
		OfxPropertySetHandle inArgs)
{
  // get my data handle
  MyInteractData *data = getInteractData(interactInstance);

  // get the point param's value
  double x, y;
  gParamHost->paramGetValue(data->pointParam, &x, &y);

  // get the size of a pixel on screen
  double pixelScale[2];
  ofxuGetInteractPixelScale(inArgs, pixelScale);

  // see if the pen is within 5 screen pixels of the point, in which case, select it
  double penPos[2];
  gPropHost->propGetDoubleN(inArgs, kOfxInteractPropPenPosition, 2, penPos);
  if(std::fabs(x - penPos[0]) < 5 * pixelScale[0] && std::fabs(y - penPos[1]) < 5 * pixelScale[1]) {
    data->selected = true;
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

static OfxStatus
interactPenUp(OfxImageEffectHandle  /*pluginInstance*/,
		OfxInteractHandle interactInstance,
		OfxPropertySetHandle /*inArgs*/)
{
  // get my data handle
  MyInteractData *data = getInteractData(interactInstance);

  if(data->selected) {
    // pen's gone up, deselect
    data->selected = false;
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

////////////////////////////////////////////////////////////////////////////////
// the entry points for the overlay
typedef OfxStatus (*InteractDrawFunc)(OfxImageEffectHandle, OfxInteractHandle, OfxPropertySetHandle);

static OfxStatus
overlayMain(InteractDrawFunc drawFunc, const char *action, const void *handle, OfxPropertySetHandle inArgs)
{
  OfxInteractHandle interact = (OfxInteractHandle ) handle;

  OfxPropertySetHandle props;
  gInteractHost->interactGetPropertySet(interact, &props);

  if(strcmp(action, kOfxActionDescribe) == 0) {
    return interactDescribe(interact);
  }
  else {
    // fetch the effect instance from the interact
    OfxImageEffectHandle pluginInstance;
    gPropHost->propGetPointer(props, kOfxPropEffectInstance, 0, (void **) &pluginInstance);

    if(strcmp(action, kOfxActionCreateInstance) == 0) {
      return interactCreateInstance(pluginInstance, interact);
    }
    else if(strcmp(action, kOfxActionDestroyInstance) == 0) {
      return interactDestroyInstance(pluginInstance, interact);
    }
    else if(strcmp(action, kOfxInteractActionDraw) == 0) {
      return drawFunc(pluginInstance, interact, inArgs);
    }
    else if(strcmp(action, kOfxInteractActionPenMotion) == 0) {
      return interactPenMotion(pluginInstance, interact, inArgs);
    }
    else if(strcmp(action, kOfxInteractActionPenDown) == 0) {
      return interactPenDown(pluginInstance, interact, inArgs);
    }
    else if(strcmp(action, kOfxInteractActionPenUp) == 0) {
      return interactPenUp(pluginInstance, interact, inArgs);
    }
    return kOfxStatReplyDefault;
  }
}

// kOfxImageEffectPluginPropOverlayInteractV1: draws with OpenGL
static OfxStatus
overlayMainV1(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/)
{
  try {
    return overlayMain(interactDrawV1, action, handle, inArgs);
  } catch (...) {
    return kOfxStatFailed;
  }
}

// kOfxImageEffectPluginPropOverlayInteractV2: draws with the OfxDrawSuiteV1
static OfxStatus
overlayMainV2(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/)
{
  try {
    return overlayMain(interactDrawV2, action, handle, inArgs);
  } catch (...) {
    return kOfxStatFailed;
  }
}

////////////////////////////////////////////////////////////////////////////////
// the plugin's description routine
static OfxStatus
describe(OfxImageEffectHandle effect)
{
  // fetch the host APIs
  OfxStatus stat;
  if((stat = ofxuFetchHostSuites()) != kOfxStatOK)
    return stat;

  // NULL if the host doesn't implement the DrawSuite
  gDrawSuite =
    (OfxDrawSuiteV1*)gHost->fetchSuite(gHost->host, kOfxDrawSuite, 1);

  // see if the host supports overlays: if not, the plug-in is still usable (it's an
  // identity effect), but without an overlay
  int supportsOverlays = 0;
  gPropHost->propGetInt(gHost->host, kOfxImageEffectPropSupportsOverlays, 0, &supportsOverlays);

  // get the property handle for the plugin
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // set the bit depths the plugin can handle
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);

  // define the plugin to the host
  gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "OFX DrawSuite Example");
  gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "OFX Example");

  // define the contexts we can be used in
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);

  if (supportsOverlays) {
    // set the properties that are the overlay's main entry points for the plugin: V1
    // (OpenGL) for all hosts, and V2 (DrawSuite) if the host has the DrawSuite. See the
    // comment at the top of this file for the spec text about how they coexist.
    gPropHost->propSetPointer(effectProps, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void *) overlayMainV1);
    if (gDrawSuite) {
      gPropHost->propSetPointer(effectProps, kOfxImageEffectPluginPropOverlayInteractV2, 0, (void *) overlayMainV2);
    }
  }

  return kOfxStatOK;
}

static OfxStatus
describeInContext(OfxImageEffectHandle  effect, OfxPropertySetHandle /*inArgs*/)
{
  // define the single source clip
  OfxPropertySetHandle props;
  gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);

  // define the output clip
  gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);

  // fetch the parameter set from the effect
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  // define the 2D point we are going to draw an overlay for
  gParamHost->paramDefine(paramSet, kOfxParamTypeDouble2D, kPointParam, &props);
  gPropHost->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
  gPropHost->propSetString(props, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesNormalised);
  gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 0.5);
  gPropHost->propSetDouble(props, kOfxParamPropDefault, 1, 0.5);
  gPropHost->propSetString(props, kOfxParamPropHint, 0, "Point attached to overlay crosshair");
  gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kPointParam);
  gPropHost->propSetString(props, kOfxPropLabel, 0, "Point");

  // whether the V2 overlay also calls the DrawSuite with invalid arguments, and checks
  // that it returns kOfxStatErrValue
  gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, kInvalidArgsParam, &props);
  gPropHost->propSetInt(props, kOfxParamPropDefault, 0, 0);
  gPropHost->propSetInt(props, kOfxParamPropAnimates, 0, 0);
  gPropHost->propSetString(props, kOfxParamPropHint, 0,
                           "In the DrawSuite (V2) overlay, also call each DrawSuite function with invalid arguments, "
                           "and check that it returns kOfxStatErrValue. Off by default, since a host that doesn't "
                           "check its arguments may misbehave.");
  gPropHost->propSetString(props, kOfxParamPropScriptName, 0, kInvalidArgsParam);
  gPropHost->propSetString(props, kOfxPropLabel, 0, "Test invalid arguments");

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// The main function
static OfxStatus
pluginMain(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
  try {
  // cast to appropriate type
  OfxImageEffectHandle effect = (OfxImageEffectHandle ) handle;

  if(strcmp(action, kOfxActionDescribe) == 0) {
    return describe(effect);
  }
  else if(strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
    return describeInContext(effect, inArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
    return isIdentity(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionRender) == 0) {
    return render(effect, inArgs, outArgs);
  }
  } catch (const std::bad_alloc&) {
    // catch memory
    //std::cout << "OFX Plugin Memory error." << std::endl;
    return kOfxStatErrMemory;
  } catch ( const std::exception& ) {
    // standard exceptions
    //std::cout << "OFX Plugin error: " << e.what() << std::endl;
    return kOfxStatErrUnknown;
  } catch (int err) {
    // ho hum, gone wrong somehow
    return err;
  } catch ( ... ) {
    // everything else
    //std::cout << "OFX Plugin error" << std::endl;
    return kOfxStatErrUnknown;
  }

  // other actions to take the default value
  return kOfxStatReplyDefault;
}

// function to set the host structure
static void
setHostFunc(OfxHost *hostStruct)
{
  gHost         = hostStruct;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin struct
static OfxPlugin basicPlugin =
{
  kOfxImageEffectPluginApi,
  1,
  "uk.co.thefoundry.BasicDrawSuitePlugin",
  1,
  0,
  setHostFunc,
  pluginMain
};

// the two mandated functions
OfxPlugin *
OfxGetPlugin(int nth)
{
  if(nth == 0)
    return &basicPlugin;
  return 0;
}

int
OfxGetNumberOfPlugins(void)
{
  return 1;
}
