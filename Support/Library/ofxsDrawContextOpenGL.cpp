// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxsDrawContextOpenGL.cpp
  OFX::DrawArgs::getDrawContext(), and the OpenGL backend of OFX::DrawContext: an emulation of the
  OFX 1.5 DrawSuite (include/ofxDrawSuite.h, OfxDrawSuiteV1) with OpenGL 1.x compatibility-profile
  calls, for the overlays registered as kOfxImageEffectPluginPropOverlayInteractV1. See the
  documentation of OFX::DrawContext in ofxsInteract.h.

  This is the only file of the Support library that calls OpenGL. ofxsInteract.cpp only calls this
  backend through virtual functions, so a plug-in that never calls getDrawContext() doesn't link
  this file (from a static library) or OpenGL.

  Define OFXS_DRAWCONTEXT_NO_OPENGL to build without OpenGL (no OpenGL header, no OpenGL call):
  getDrawContext() then returns a context that draws nothing for the V1 overlays.

  Spec references (upstream include/ofxDrawSuite.h, include/ofxInteract.h and
  Documentation/sources/Reference/ofxInteracts.rst, OFX 1.5):
  - coordinates: "The GL_PROJECTION matrix will be set up so that it maps openGL coordinates to
    canonical image coordinates. The GL_MODELVIEW matrix will be the identity matrix."
    (ofxInteracts.rst, OpenGL overlays). The DrawSuite doesn't say it, but its points are in the
    same canonical coordinates (the upstream DrawSuite example draws its cross hair at the point
    parameter's canonical position, sized with kOfxInteractPropPixelScale). This backend uses the
    current OpenGL matrices, so the points are canonical coordinates.
  - kOfxInteractPropPixelScale: "The size of a real screen pixel under the interact's canonical
    projection." It is used for the sizes that are in screen pixels (text, ellipse tessellation).
  - setColour: "Sets the colour for future drawing operations (lines, filled shapes and text)
    [...] The host should use "over" compositing when using a non-opaque colour."
  - setLineWidth: "Use width 0 for a single pixel line or non-zero for a smooth line of the
    desired width. The host should adjust for screen density."
  - draw: "kOfxDrawPrimitiveLines - like GL_LINES, n points draws n/2 separated lines;
    kOfxDrawPrimitiveLineStrip - like GL_LINE_STRIP, n points draws n-1 connected lines;
    kOfxDrawPrimitiveLineLoop - like GL_LINE_LOOP, n points draws n connected lines;
    kOfxDrawPrimitiveRectangle - draws an axis-aligned filled rectangle defined by 2 opposite
    corner points; kOfxDrawPrimitivePolygon - like GL_POLYGON, draws a filled n-sided polygon;
    kOfxDrawPrimitiveEllipse - draws a axis-aligned elliptical line (not filled) within the
    rectangle defined by 2 opposite corner points". Returns "kOfxStatErrValue - invalid
    primitive, or point_count not valid for primitive".
  - drawText: "The text font face and size are determined by the host." Returns
    "kOfxStatErrValue - text or pos were not defined".
  - OfxDrawTextAlignment: Left 0x1, Right 0x2, Top 0x4, Bottom 0x8, Baseline 0x10,
    CenterH = (Left | Right), CenterV = (Top | Baseline). CenterV is Top|Baseline, not
    Top|Bottom: this backend centres the text vertically when Top is set with Bottom or Baseline.
  - getColour: "Retrieves the host's desired draw colour for" an OfxStandardColour; the spec
    gives no values. This backend returns the fixed colours of kStandardColours below.
 */

#include "ofxsSupportPrivate.h"

#ifndef OFXS_DRAWCONTEXT_NO_OPENGL

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // for WINGDIAPI and APIENTRY, used by GL/gl.h
#endif

#ifdef __APPLE__
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION // OpenGL 1.x compatibility profile, as every OpenGL overlay
#endif
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "ofxsFont5x7.h"

#endif // !OFXS_DRAWCONTEXT_NO_OPENGL

namespace OFX {

  namespace Private {

#ifndef OFXS_DRAWCONTEXT_NO_OPENGL

    namespace {

      // The colours returned by getColour(), indexed by OfxStandardColour. The spec gives no
      // values: these are close to the colours of the Natron and SupportExt OpenGL overlays.
      const OfxRGBAColourF kStandardColours[] = {
        { 0.f, 0.f, 0.f, 1.f },     // kOfxStandardColourOverlayBackground: black (the overlays' shadow)
        { 0.f, 1.f, 0.f, 1.f },     // kOfxStandardColourOverlayActive: green (hovered, being dragged)
        { 1.f, 0.f, 0.f, 1.f },     // kOfxStandardColourOverlaySelected: red
        { 0.8f, 0.8f, 0.8f, 1.f },  // kOfxStandardColourOverlayDeselected: light grey (the default)
        { 0.5f, 0.8f, 1.f, 1.f },   // kOfxStandardColourOverlayMarqueeFG: light blue (Natron's selection rectangle)
        { 0.5f, 0.8f, 1.f, 0.4f },  // kOfxStandardColourOverlayMarqueeBG: translucent light blue
        { 1.f, 1.f, 1.f, 1.f },     // kOfxStandardColourOverlayText: white
      };
      const int kStandardColourCount = (int)(sizeof(kStandardColours) / sizeof(kStandardColours[0]));

      // GL_LINE_STIPPLE patterns, in screen pixels (bit 0 is the first pixel), indexed by
      // OfxDrawLineStipplePattern (solid: no stipple)
      const GLushort kStipplePatterns[] = {
        0xFFFF, // kOfxDrawLineStipplePatternSolid   -----
        0x3333, // kOfxDrawLineStipplePatternDot     .....  2 on, 2 off
        0x00FF, // kOfxDrawLineStipplePatternDash    - - -  8 on, 8 off
        0xFF00, // kOfxDrawLineStipplePatternAltDash  - - - 8 off, 8 on
        0x1FE3, // kOfxDrawLineStipplePatternDotDash .-.-.- 2 on, 3 off, 8 on, 3 off
      };
      const int kStipplePatternCount = (int)(sizeof(kStipplePatterns) / sizeof(kStipplePatterns[0]));

      // text metrics, in screen pixels (one font pixel per screen pixel)
      const int kTextAscent = kFontGlyphHeight; // the glyphs sit on the baseline
      const int kTextDescent = kFontCellHeight - kFontGlyphHeight;

      inline bool isFinitePositive(double v)
      {
        return v > 0. && v < 1e300; // false for NaN and infinities
      }

      // Saves the OpenGL state that the backend changes, and sets the state common to all the
      // primitives: no texture, lighting, depth test or face culling, filled polygons, and
      // "over" compositing ("The host should use "over" compositing when using a non-opaque
      // colour"). The destructor restores it.
      class GLStateSaver {
      public:
        explicit GLStateSaver(const OfxRGBAColourF &colour)
        {
          glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_LINE_BIT |
                       GL_POLYGON_BIT | GL_HINT_BIT);
          glDisable(GL_TEXTURE_1D);
          glDisable(GL_TEXTURE_2D);
          glDisable(GL_LIGHTING);
          glDisable(GL_DEPTH_TEST);
          glDisable(GL_CULL_FACE);
          glDisable(GL_ALPHA_TEST);
          glDisable(GL_POLYGON_SMOOTH);
          glDisable(GL_POLYGON_STIPPLE);
          glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
          glEnable(GL_BLEND);
          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          glColor4f(colour.r, colour.g, colour.b, colour.a);
        }
        ~GLStateSaver()
        {
          glPopAttrib();
        }
      private:
        GLStateSaver(const GLStateSaver &);
        GLStateSaver &operator=(const GLStateSaver &);
      };

      class OpenGLBackend : public DrawContextBackend {
      public:
        virtual OfxStatus getColour(const DrawContext & /*ctx*/, OfxStandardColour stdColour, OfxRGBAColourF *colour) const
        {
          if (!colour || (int)stdColour < 0 || (int)stdColour >= kStandardColourCount) {
            return kOfxStatErrValue; // "std_colour was invalid"
          }
          *colour = kStandardColours[(int)stdColour];
          return kOfxStatOK;
        }

        virtual OfxStatus setColour(const DrawContext &ctx, const OfxRGBAColourF *c) const
        {
          if (!c) {
            return kOfxStatFailed;
          }
          colour(ctx) = *c;
          return kOfxStatOK;
        }

        virtual OfxStatus setLineWidth(const DrawContext &ctx, float width) const
        {
          if ( !(width >= 0.f && width < 1e30f) ) { // negative, NaN or infinite
            return kOfxStatFailed; // (the spec lists no kOfxStatErrValue for setLineWidth)
          }
          lineWidth(ctx) = width;
          return kOfxStatOK;
        }

        virtual OfxStatus setLineStipple(const DrawContext &ctx, OfxDrawLineStipplePattern pattern) const
        {
          if ( (int)pattern < 0 || (int)pattern >= kStipplePatternCount ) {
            return kOfxStatErrValue; // "pattern was not valid"
          }
          lineStipple(ctx) = pattern;
          return kOfxStatOK;
        }

        virtual OfxStatus draw(const DrawContext &ctx, OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount) const
        {
          int minCount, maxCount;
          switch (primitive) {
          case kOfxDrawPrimitiveLines:
          case kOfxDrawPrimitiveLineStrip:
          case kOfxDrawPrimitiveLineLoop:
            minCount = 2; maxCount = -1; break;
          case kOfxDrawPrimitivePolygon:
            minCount = 3; maxCount = -1; break;
          case kOfxDrawPrimitiveRectangle:
          case kOfxDrawPrimitiveEllipse:
            minCount = 2; maxCount = 2; break; // "defined by 2 opposite corner points"
          default:
            return kOfxStatErrValue; // "invalid primitive"
          }
          if ( !points || pointCount < minCount || (maxCount >= 0 && pointCount > maxCount) ) {
            return kOfxStatErrValue; // "point_count not valid for primitive"
          }

          GLStateSaver saver(colour(ctx));
          switch (primitive) {
          case kOfxDrawPrimitiveLines:
            setLineState(ctx);
            drawVertices(GL_LINES, points, pointCount);
            break;
          case kOfxDrawPrimitiveLineStrip:
            setLineState(ctx);
            drawVertices(GL_LINE_STRIP, points, pointCount);
            break;
          case kOfxDrawPrimitiveLineLoop:
            setLineState(ctx);
            drawVertices(GL_LINE_LOOP, points, pointCount);
            break;
          case kOfxDrawPrimitivePolygon:
            drawVertices(GL_POLYGON, points, pointCount);
            break;
          case kOfxDrawPrimitiveRectangle:
            glBegin(GL_QUADS);
            glVertex2d(points[0].x, points[0].y);
            glVertex2d(points[1].x, points[0].y);
            glVertex2d(points[1].x, points[1].y);
            glVertex2d(points[0].x, points[1].y);
            glEnd();
            break;
          case kOfxDrawPrimitiveEllipse:
            setLineState(ctx);
            drawEllipse(ctx, points[0], points[1]);
            break;
          default:
            break;
          }
          return kOfxStatOK;
        }

        virtual OfxStatus drawText(const DrawContext &ctx, const char *text, const OfxPointD *pos, int alignment) const
        {
          if (!text || !pos) {
            return kOfxStatErrValue; // "text or pos were not defined"
          }
          // the glyphs: printable ASCII, '?' for the other characters (one per UTF-8 sequence),
          // nothing for the control characters
          std::string glyphs;
          for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            if (*p < 0x80) {
              if (*p >= 0x20 && *p < 0x7F) {
                glyphs += (char)*p;
              }
            } else if ( (*p & 0xC0) != 0x80 ) { // the first byte of a UTF-8 sequence
              glyphs += '?';
            }
          }
          if ( glyphs.empty() ) {
            return kOfxStatOK;
          }

          OfxPointD ps;
          getPixelScale(ctx, ps);
          const int width = (int)glyphs.size() * kFontCellWidth - (kFontCellWidth - kFontGlyphWidth);

          // horizontal: Left (default), Right, or both (CenterH)
          double x0 = pos->x;
          const bool left = (alignment & kOfxDrawTextAlignmentLeft) != 0;
          const bool right = (alignment & kOfxDrawTextAlignmentRight) != 0;
          if (right && left) {
            x0 -= 0.5 * width * ps.x;
          } else if (right) {
            x0 -= width * ps.x;
          }
          // vertical: Baseline (default), Top, Bottom, or Top with Bottom or Baseline (centred:
          // kOfxDrawTextAlignmentCenterV is Top | Baseline)
          double baseline = pos->y;
          const bool top = (alignment & kOfxDrawTextAlignmentTop) != 0;
          const bool bottom = (alignment & kOfxDrawTextAlignmentBottom) != 0;
          const bool onBaseline = (alignment & kOfxDrawTextAlignmentBaseline) != 0;
          if ( top && (bottom || onBaseline) ) {
            baseline -= 0.5 * (kTextAscent - kTextDescent) * ps.y;
          } else if (top) {
            baseline -= kTextAscent * ps.y;
          } else if (bottom) {
            baseline += kTextDescent * ps.y;
          }

          GLStateSaver saver(colour(ctx));
          // one quad per vertical run of pixels in each glyph column. A quad of one screen
          // pixel covers exactly one pixel centre, so the text is sharp at any position.
          glBegin(GL_QUADS);
          for (size_t i = 0; i < glyphs.size(); ++i) {
            const unsigned char *glyph = kFont5x7[(unsigned char)glyphs[i] - 0x20];
            for (int col = 0; col < kFontGlyphWidth; ++col) {
              const double xl = x0 + ( (int)i * kFontCellWidth + col ) * ps.x;
              const double xr = xl + ps.x;
              int row = 0;
              while (row < kFontGlyphHeight) {
                if ( !( (glyph[col] >> row) & 1 ) ) {
                  ++row;
                  continue;
                }
                const int first = row; // top row of the run
                while ( row < kFontGlyphHeight && ( (glyph[col] >> row) & 1 ) ) {
                  ++row;
                }
                // row kFontGlyphHeight - 1 sits on the baseline
                const double yt = baseline + (kFontGlyphHeight - first) * ps.y;
                const double yb = baseline + (kFontGlyphHeight - row) * ps.y;
                glVertex2d(xl, yb);
                glVertex2d(xr, yb);
                glVertex2d(xr, yt);
                glVertex2d(xl, yt);
              }
            }
          }
          glEnd();
          return kOfxStatOK;
        }

      private:
        // the screen pixel ratio, 1 if the host doesn't give it
        static double getScreenPixelRatio(const DrawContext &ctx)
        {
          const double r = screenPixelRatio(ctx);
          return isFinitePositive(r) ? r : 1.;
        }

        // the size of a screen pixel in canonical coordinates (1 if the host gave none)
        static void getPixelScale(const DrawContext &ctx, OfxPointD &ps)
        {
          const OfxPointD &s = pixelScale(ctx);
          ps.x = isFinitePositive(std::fabs(s.x)) ? std::fabs(s.x) : 1.;
          ps.y = isFinitePositive(std::fabs(s.y)) ? std::fabs(s.y) : 1.;
        }

        // the line width and stipple, for the lines and the ellipse
        static void setLineState(const DrawContext &ctx)
        {
          // "Use width 0 for a single pixel line or non-zero for a smooth line of the desired
          // width. The host should adjust for screen density.": widths are in screen pixels,
          // and OpenGL's are device pixels
          const double ratio = getScreenPixelRatio(ctx);
          const float width = lineWidth(ctx);
          if (width > 0.f) {
            glEnable(GL_LINE_SMOOTH);
            glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
            glLineWidth( (GLfloat)((double)width * ratio) );
          } else {
            glDisable(GL_LINE_SMOOTH);
            glLineWidth( (GLfloat)std::max(1., std::floor(ratio + 0.5)) );
          }
          const OfxDrawLineStipplePattern pattern = lineStipple(ctx);
          if (pattern == kOfxDrawLineStipplePatternSolid) {
            glDisable(GL_LINE_STIPPLE);
          } else {
            glEnable(GL_LINE_STIPPLE);
            // the patterns are in screen pixels
            const GLint factor = (GLint)std::min(256., std::max(1., std::floor(ratio + 0.5)));
            glLineStipple(factor, kStipplePatterns[(int)pattern]);
          }
        }

        static void drawVertices(GLenum mode, const OfxPointD *points, int pointCount)
        {
          glBegin(mode);
          for (int i = 0; i < pointCount; ++i) {
            glVertex2d(points[i].x, points[i].y);
          }
          glEnd();
        }

        // "an axis-aligned elliptical line (not filled) within the rectangle defined by 2
        // opposite corner points": a line loop, within 1/4 screen pixel of the ellipse
        static void drawEllipse(const DrawContext &ctx, const OfxPointD &p0, const OfxPointD &p1)
        {
          OfxPointD ps;
          getPixelScale(ctx, ps);
          const double cx = 0.5 * (p0.x + p1.x);
          const double cy = 0.5 * (p0.y + p1.y);
          const double rx = 0.5 * std::fabs(p1.x - p0.x);
          const double ry = 0.5 * std::fabs(p1.y - p0.y);
          // the largest radius in screen pixels: the sagitta of a segment of angle a is
          // r (1 - cos(a/2)), at most tolerance for a = 2 acos(1 - tolerance / r)
          const double r = std::max(rx / ps.x, ry / ps.y);
          const double tolerance = 0.25;
          const double pi = 3.14159265358979323846;
          int n = 8;
          if ( r > tolerance && r < 1e300 ) {
            const double a = 2. * std::acos(1. - tolerance / r);
            if (a > 0.) {
              n = (int)std::min( 4096., std::max( 8., std::ceil(2. * pi / a) ) );
            } else {
              n = 4096;
            }
          }
          glBegin(GL_LINE_LOOP);
          for (int i = 0; i < n; ++i) {
            const double t = 2. * pi * i / n;
            glVertex2d(cx + rx * std::cos(t), cy + ry * std::sin(t));
          }
          glEnd();
        }
      };

    } // namespace

#endif // !OFXS_DRAWCONTEXT_NO_OPENGL

    const DrawContextBackend *openGLDrawContextBackend()
    {
#ifndef OFXS_DRAWCONTEXT_NO_OPENGL
      static const OpenGLBackend backend;
      return &backend;
#else
      return NULL;
#endif
    }

  } // namespace Private

  const DrawContext &DrawArgs::getDrawContext() const
  {
    const DrawContext &drawSuite = Private::DrawArgsAccess::drawSuiteContext(*this);
    switch ( Private::DrawArgsAccess::getOverlayRegistration(*this) ) {
    case Private::eOverlayRegistrationV2:
      // "If this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use
      // DrawSuite": the DrawSuite, or nothing if the host passed no draw context
      return drawSuite;
    case Private::eOverlayRegistrationV1:
      // "If this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use
      // OpenGL": OpenGL, even if the host passed a draw context
      break;
    case Private::eOverlayRegistrationUnknown:
    default:
      // DrawArgs not made by the dispatcher: the draw context if there is one
      if ( drawSuite.usesDrawSuite() ) {
        return drawSuite;
      }
      break;
    }
    DrawContext &openGL = Private::DrawArgsAccess::openGLContext(*this);
    Private::DrawArgsAccess::setOpenGLBackend(openGL, Private::openGLDrawContextBackend());
    return openGL;
  }

} // namespace OFX
