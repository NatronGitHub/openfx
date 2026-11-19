// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _ofxDrawSuiteOpenGL_h_
#define _ofxDrawSuiteOpenGL_h_

/** @file ofxDrawSuiteOpenGL.h
  The OpenGL DrawSuite core: the OFX 1.5 DrawSuite (ofxDrawSuite.h, OfxDrawSuiteV1) drawn with
  OpenGL 1.1 compatibility-profile calls, shared by the Support library (the OpenGL backend of
  OFX::DrawContext, Support/Library/ofxsDrawContextOpenGL.cpp, which emulates the DrawSuite in V1
  overlay draws) and by hosts that implement the DrawSuite with OpenGL (HostSupport's
  ofxhDrawSuiteOpenGL.h, or a host's own code). Not part of the OFX standard: a NatronGitHub/openfx
  addition, in namespace OFX::DrawSuiteOpenGL.

  Header-only, C++98 or later. It includes no OpenGL header: it declares the few OpenGL scalar
  types and OpenGL 1.1 constants it uses (under names that don't clash with gl.h's macros, so it
  can be included before or after gl.h), and calls OpenGL only through a GLFunctions table that
  the caller fills (loadGLFunctions(), with the caller's symbol lookup: dlsym, GetProcAddress,
  glad, Qt's getProcAddress, ...). So it compiles without the OpenGL headers and links no
  OpenGL.

  Visibility: the functions are inline, with external linkage, and ScopedGLState is a class, so
  every binary that uses this header has its own copy. Plug-ins (and hosts that load plug-ins
  built with other versions of it) must keep hidden visibility (-fvisibility=hidden, as the
  Support library, Makefile.master and cmake/OpenFX.cmake do, and the export lists), so that the
  dynamic linker never binds one binary's copy to another's.

  The caller must make an OpenGL context current, with the GL_PROJECTION and GL_MODELVIEW
  matrices of the overlay (canonical coordinates: "The GL_PROJECTION matrix will be set up so
  that it maps openGL coordinates to canonical image coordinates. The GL_MODELVIEW matrix will be
  the identity matrix.", ofxInteracts.rst). drawPrimitive() and drawText() save and restore all
  the OpenGL state they change (glPushAttrib/glPopAttrib, see ScopedGLState).

  Behaviour (the same as Support's OpenGL backend, documented in Support/include/ofxsInteract.h):
  - State: colour, line width, stipple, the pixel scale (kOfxInteractPropPixelScale: the size of
    a screen pixel in canonical coordinates), the screen pixel ratio (device pixels per screen
    pixel, kOfxInteractPropScreenPixelRatio) and the shadows flag. resetState() sets it for a new
    draw action: opaque white, width 0, solid.
  - setColour: "over" compositing (GL_BLEND, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA). NULL:
    kOfxStatErrValue.
  - setLineWidth: in screen pixels. Width 0 is an aliased line of max(1, round(ratio)) device
    pixels, any other width a smooth (GL_LINE_SMOOTH) line of width * ratio device pixels
    ("Use width 0 for a single pixel line or non-zero for a smooth line of the desired width.
    The host should adjust for screen density."). Negative, NaN or infinite: kOfxStatFailed
    (the spec lists no kOfxStatErrValue for setLineWidth).
  - setLineStipple: GL_LINE_STIPPLE with the patterns of stipplePattern(), in screen pixels
    (factor round(ratio)). An invalid pattern: kOfxStatErrValue.
  - drawPrimitive: Lines needs an even point count, Rectangle and Ellipse exactly 2 points,
    Polygon at least 3, LineStrip and LineLoop at least 2; else, or for NULL points or an invalid
    primitive, kOfxStatErrValue. Rectangle is a filled GL_QUADS. Ellipse is a GL_LINE_LOOP of
    ellipseSegmentCount() vertices, within about 1/4 screen pixel of the ellipse.
  - drawText: the built-in 5x7 font (ofxFont5x7.h): printable ASCII, '?' for any other
    character (one per UTF-8 sequence), nothing for the control characters; one font pixel per
    screen pixel, 6 pixels per character, 7 above the baseline and 1 below. NULL text or pos:
    kOfxStatErrValue. Aligned with alignText().
  - standardColour: fixed colours, close to the Natron and SupportExt OpenGL overlays.

  Shadows: if State::shadows is true, drawPrimitive() and drawText() first draw the same element
  offset by one device pixel right and down (shadowOffset(): (+pixelScale.x / screenPixelRatio,
  -pixelScale.y / screenPixelRatio) in canonical coordinates, as the OpenGL shadows of the
  Natron and SupportExt overlays), in the standard background colour
  (kOfxStandardColourOverlayBackground) with the current colour's alpha, then the element. Off by
  default. Plug-ins never draw shadows with DrawSuite calls: the shadows are for hosts (a host
  setting) and for Support's DrawSuite-to-OpenGL compatibility layer (see
  kNatronOfxInteractPropDrawSuiteShadows in ofxNatron.h).
 */

#ifndef __cplusplus
#error "ofxDrawSuiteOpenGL.h is C++"
#endif

#include <cmath>
#include <cstddef>
#include <cstring>

#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxFont5x7.h"

/** @brief The calling convention of the OpenGL functions (APIENTRY: __stdcall on Windows) */
#ifdef _WIN32
#define OFX_DRAWGL_APIENTRY __stdcall
#else
#define OFX_DRAWGL_APIENTRY
#endif

namespace OFX {
  namespace DrawSuiteOpenGL {

    /** @name The OpenGL scalar types (the same as gl.h's on every platform) */
    /// @{
    typedef unsigned int GLenumT;
    typedef unsigned int GLbitfieldT;
    typedef int GLintT;
    typedef unsigned short GLushortT;
    typedef float GLfloatT;
    typedef double GLdoubleT;
    /// @}

    /** @name The OpenGL 1.1 constants used (gl.h's values) */
    /// @{
    static const GLenumT kGLLines = 0x0001;
    static const GLenumT kGLLineLoop = 0x0002;
    static const GLenumT kGLLineStrip = 0x0003;
    static const GLenumT kGLQuads = 0x0007;
    static const GLenumT kGLPolygon = 0x0009;
    static const GLenumT kGLSrcAlpha = 0x0302;
    static const GLenumT kGLOneMinusSrcAlpha = 0x0303;
    static const GLenumT kGLFrontAndBack = 0x0408;
    static const GLenumT kGLLineSmooth = 0x0B20;
    static const GLenumT kGLLineStipple = 0x0B24;
    static const GLenumT kGLPolygonSmooth = 0x0B41;
    static const GLenumT kGLPolygonStipple = 0x0B42;
    static const GLenumT kGLCullFace = 0x0B44;
    static const GLenumT kGLLighting = 0x0B50;
    static const GLenumT kGLDepthTest = 0x0B71;
    static const GLenumT kGLAlphaTest = 0x0BC0;
    static const GLenumT kGLBlend = 0x0BE2;
    static const GLenumT kGLLineSmoothHint = 0x0C52;
    static const GLenumT kGLTexture1D = 0x0DE0;
    static const GLenumT kGLTexture2D = 0x0DE1;
    static const GLenumT kGLDontCare = 0x1100;
    static const GLenumT kGLFill = 0x1B02;
    static const GLenumT kGLTextureRectangle = 0x84F5; // GL_TEXTURE_RECTANGLE(_ARB)
    static const GLbitfieldT kGLCurrentBit = 0x00000001;
    static const GLbitfieldT kGLLineBit = 0x00000004;
    static const GLbitfieldT kGLPolygonBit = 0x00000008;
    static const GLbitfieldT kGLEnableBit = 0x00002000;
    static const GLbitfieldT kGLColorBufferBit = 0x00004000;
    static const GLbitfieldT kGLHintBit = 0x00008000;
    /** @brief the attributes that ScopedGLState saves */
    static const GLbitfieldT kSavedAttribMask = kGLCurrentBit | kGLEnableBit | kGLColorBufferBit | kGLLineBit |
                                                kGLPolygonBit | kGLHintBit;
    /// @}

    /** @brief The OpenGL 1.1 functions the core calls (glBegin, glEnd, ...), filled by the caller,
        e.g. with loadGLFunctions(). All must be set (isComplete()). */
    struct GLFunctions {
      void (OFX_DRAWGL_APIENTRY *Begin)(GLenumT mode);
      void (OFX_DRAWGL_APIENTRY *End)(void);
      void (OFX_DRAWGL_APIENTRY *Vertex2d)(GLdoubleT x, GLdoubleT y);
      void (OFX_DRAWGL_APIENTRY *Color4f)(GLfloatT r, GLfloatT g, GLfloatT b, GLfloatT a);
      void (OFX_DRAWGL_APIENTRY *Enable)(GLenumT cap);
      void (OFX_DRAWGL_APIENTRY *Disable)(GLenumT cap);
      void (OFX_DRAWGL_APIENTRY *Hint)(GLenumT target, GLenumT mode);
      void (OFX_DRAWGL_APIENTRY *LineWidth)(GLfloatT width);
      void (OFX_DRAWGL_APIENTRY *LineStipple)(GLintT factor, GLushortT pattern);
      void (OFX_DRAWGL_APIENTRY *PolygonMode)(GLenumT face, GLenumT mode);
      void (OFX_DRAWGL_APIENTRY *BlendFunc)(GLenumT sfactor, GLenumT dfactor);
      void (OFX_DRAWGL_APIENTRY *PushAttrib)(GLbitfieldT mask);
      void (OFX_DRAWGL_APIENTRY *PopAttrib)(void);
    };

    /** @brief A symbol lookup: the address of the OpenGL function name ("glBegin", ...), NULL if
        unknown. userData is the pointer passed to loadGLFunctions(). */
    typedef void *(*GLProcResolver)(const char *name, void *userData);

    namespace Detail {
      // A function pointer can't be converted from a void* in standard C++: copy the bits (the
      // sizes are checked at compile time, C++98-compatible static assertion)
      template<class F>
      inline void functionFromPointer(void *p, F &f)
      {
        typedef char function_and_object_pointers_have_the_same_size[(sizeof(F) == sizeof(void *)) ? 1 : -1];
        (void)sizeof(function_and_object_pointers_have_the_same_size);
        std::memcpy(&f, &p, sizeof(f));
      }

      template<class F>
      inline bool resolve(GLProcResolver resolver, void *userData, const char *name, F &f)
      {
        void *p = resolver ? resolver(name, userData) : NULL;
        if (p) {
          functionFromPointer(p, f);
        } else {
          f = NULL;
        }
        return f != NULL;
      }

      inline bool isFinitePositive(double v)
      {
        return v > 0. && v < 1e300; // false for NaN and infinities
      }

      // limits, so that the conversions to OpenGL types are defined
      static const double kMaxScreenPixelRatio = 64.;
      static const double kMaxLineWidth = 1e4; // device pixels
      static const double kPi = 3.14159265358979323846;

      // the glyph of the byte at p (an index in kFont5x7), -1 for none: printable ASCII, '?' for
      // the first byte of any other UTF-8 sequence, nothing for the control characters and the
      // continuation bytes
      inline int glyphIndex(unsigned char c)
      {
        if (c < 0x80) {
          return (c >= 0x20 && c < 0x7F) ? (int)c - 0x20 : -1;
        }
        return ( (c & 0xC0) != 0x80 ) ? (int)'?' - 0x20 : -1;
      }
    } // namespace Detail

    /** @brief true if all the functions of gl are set */
    inline bool isComplete(const GLFunctions &gl)
    {
      return gl.Begin && gl.End && gl.Vertex2d && gl.Color4f && gl.Enable && gl.Disable && gl.Hint &&
             gl.LineWidth && gl.LineStipple && gl.PolygonMode && gl.BlendFunc && gl.PushAttrib && gl.PopAttrib;
    }

    /** @brief Fill gl with resolver ("glBegin", "glEnd", ...: the OpenGL names). Every function is
        set (NULL if the resolver doesn't find it). Returns isComplete(gl).
        On Windows, wglGetProcAddress returns NULL for the OpenGL 1.1 functions (opengl32.dll
        exports them, it only returns extension and later-version functions): a resolver must use
        GetProcAddress on opengl32.dll, or a loader that falls back to it (glad, Qt's
        QOpenGLContext::getProcAddress). */
    inline bool loadGLFunctions(GLFunctions &gl, GLProcResolver resolver, void *userData)
    {
      bool ok = true;
      ok = Detail::resolve(resolver, userData, "glBegin", gl.Begin) && ok;
      ok = Detail::resolve(resolver, userData, "glEnd", gl.End) && ok;
      ok = Detail::resolve(resolver, userData, "glVertex2d", gl.Vertex2d) && ok;
      ok = Detail::resolve(resolver, userData, "glColor4f", gl.Color4f) && ok;
      ok = Detail::resolve(resolver, userData, "glEnable", gl.Enable) && ok;
      ok = Detail::resolve(resolver, userData, "glDisable", gl.Disable) && ok;
      ok = Detail::resolve(resolver, userData, "glHint", gl.Hint) && ok;
      ok = Detail::resolve(resolver, userData, "glLineWidth", gl.LineWidth) && ok;
      ok = Detail::resolve(resolver, userData, "glLineStipple", gl.LineStipple) && ok;
      ok = Detail::resolve(resolver, userData, "glPolygonMode", gl.PolygonMode) && ok;
      ok = Detail::resolve(resolver, userData, "glBlendFunc", gl.BlendFunc) && ok;
      ok = Detail::resolve(resolver, userData, "glPushAttrib", gl.PushAttrib) && ok;
      ok = Detail::resolve(resolver, userData, "glPopAttrib", gl.PopAttrib) && ok;
      return ok && isComplete(gl);
    }

    /** @brief The DrawSuite state of a draw action */
    struct State {
      OfxRGBAColourF colour;             ///< setColour (opaque white at first)
      float lineWidth;                   ///< setLineWidth, screen pixels (0 at first)
      OfxDrawLineStipplePattern stipple; ///< setLineStipple (solid at first)
      OfxPointD pixelScale;              ///< size of a screen pixel in canonical coordinates (> 0)
      double screenPixelRatio;           ///< device pixels per screen pixel, in (0, 64]
      bool shadows;                      ///< draw drop shadows (see the file documentation)
    };

    /** @brief The state at the start of a draw action: opaque white, width 0, solid, and the
        given pixel scale (kOfxInteractPropPixelScale), screen pixel ratio and shadows flag. The
        pixel scale is made positive (|scale|, 1 if it is not finite or 0), the ratio is 1 if it is
        not finite or <= 0, at most 64. */
    inline void resetState(State &state, const OfxPointD &pixelScale, double screenPixelRatio, bool shadows)
    {
      state.colour.r = state.colour.g = state.colour.b = state.colour.a = 1.f;
      state.lineWidth = 0.f;
      state.stipple = kOfxDrawLineStipplePatternSolid;
      const double sx = std::fabs(pixelScale.x), sy = std::fabs(pixelScale.y);
      state.pixelScale.x = Detail::isFinitePositive(sx) ? sx : 1.;
      state.pixelScale.y = Detail::isFinitePositive(sy) ? sy : 1.;
      state.screenPixelRatio = Detail::isFinitePositive(screenPixelRatio) ?
                               (screenPixelRatio < Detail::kMaxScreenPixelRatio ? screenPixelRatio : Detail::kMaxScreenPixelRatio) : 1.;
      state.shadows = shadows;
    }

    /** @brief The colours of getColour(), for an OfxStandardColour. The spec gives no values:
        these are close to the colours of the Natron and SupportExt OpenGL overlays.
        kOfxStatErrValue for NULL colour or an invalid stdColour ("std_colour was invalid"). */
    inline OfxStatus standardColour(OfxStandardColour stdColour, OfxRGBAColourF *colour)
    {
      if (!colour) {
        return kOfxStatErrValue;
      }
      OfxRGBAColourF c = { 0.f, 0.f, 0.f, 1.f };
      switch (stdColour) {
      case kOfxStandardColourOverlayBackground: // black (the overlays' shadow)
        break;
      case kOfxStandardColourOverlayActive: // green (hovered, being dragged)
        c.g = 1.f; break;
      case kOfxStandardColourOverlaySelected: // red
        c.r = 1.f; break;
      case kOfxStandardColourOverlayDeselected: // light grey (the default)
        c.r = c.g = c.b = 0.8f; break;
      case kOfxStandardColourOverlayMarqueeFG: // light blue (Natron's selection rectangle)
        c.r = 0.5f; c.g = 0.8f; c.b = 1.f; break;
      case kOfxStandardColourOverlayMarqueeBG: // translucent light blue
        c.r = 0.5f; c.g = 0.8f; c.b = 1.f; c.a = 0.4f; break;
      case kOfxStandardColourOverlayText: // white
        c.r = c.g = c.b = 1.f; break;
      default:
        return kOfxStatErrValue;
      }
      *colour = c;
      return kOfxStatOK;
    }

    /** @brief setColour: kOfxStatErrValue for NULL (as HostSupport's DrawSuite). The spec lists
        only kOfxStatOK and kOfxStatFailed for setColour: kOfxStatErrValue is deliberate, the
        behaviour of Support's OpenGL backend and of HostSupport's DrawSuite before this header. */
    inline OfxStatus setColour(State &state, const OfxRGBAColourF *colour)
    {
      if (!colour) {
        return kOfxStatErrValue;
      }
      state.colour = *colour;
      return kOfxStatOK;
    }

    /** @brief setLineWidth, in screen pixels: kOfxStatFailed if negative, NaN or infinite */
    inline OfxStatus setLineWidth(State &state, float width)
    {
      if ( !(width >= 0.f && width < 1e30f) ) {
        return kOfxStatFailed; // (the spec lists no kOfxStatErrValue for setLineWidth)
      }
      state.lineWidth = width;
      return kOfxStatOK;
    }

    /** @brief setLineStipple: kOfxStatErrValue for an invalid pattern ("pattern was not valid") */
    inline OfxStatus setLineStipple(State &state, OfxDrawLineStipplePattern pattern)
    {
      if ( (int)pattern < (int)kOfxDrawLineStipplePatternSolid || (int)pattern > (int)kOfxDrawLineStipplePatternDotDash ) {
        return kOfxStatErrValue;
      }
      state.stipple = pattern;
      return kOfxStatOK;
    }

    /** @brief The GL_LINE_STIPPLE pattern of an OfxDrawLineStipplePattern, in screen pixels (bit 0
        is the first pixel): solid 0xFFFF, dot 0x3333 (2 on, 2 off), dash 0x00FF (8 on, 8 off),
        alt dash 0xFF00 (8 off, 8 on), dot dash 0x1FE3 (2 on, 3 off, 8 on, 3 off). 0xFFFF for an
        invalid pattern. */
    inline GLushortT stipplePattern(OfxDrawLineStipplePattern pattern)
    {
      switch (pattern) {
      case kOfxDrawLineStipplePatternDot: return 0x3333;
      case kOfxDrawLineStipplePatternDash: return 0x00FF;
      case kOfxDrawLineStipplePatternAltDash: return 0xFF00;
      case kOfxDrawLineStipplePatternDotDash: return 0x1FE3;
      case kOfxDrawLineStipplePatternSolid:
      default: return 0xFFFF;
      }
    }

    /** @brief The point count check of draw ("kOfxStatErrValue - invalid primitive, or point_count
        not valid for primitive"): Lines an even count >= 2 (pairs, as HostSupport's DrawSuite),
        LineStrip and LineLoop >= 2, Polygon >= 3, Rectangle and Ellipse exactly 2 ("defined by 2
        opposite corner points"), points not NULL. kOfxStatOK or kOfxStatErrValue. */
    inline OfxStatus validatePrimitive(OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount)
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
        minCount = 2; maxCount = 2; break;
      default:
        return kOfxStatErrValue; // "invalid primitive"
      }
      if ( !points || pointCount < minCount || (maxCount >= 0 && pointCount > maxCount) ||
           (primitive == kOfxDrawPrimitiveLines && pointCount % 2 != 0) ) {
        return kOfxStatErrValue;
      }
      return kOfxStatOK;
    }

    /** @brief The vertex count of an ellipse of radii rxScreenPx, ryScreenPx (screen pixels: the
        canonical radii divided by the pixel scale; the signs are ignored), so that its segments
        are within 1/4 screen pixel of the ellipse: r = max(|rx|, |ry|); 8 if r <= 1/4, or r is not
        finite (or a radius is NaN), else ceil(2 pi / (2 acos(1 - 0.25 / r))) (the sagitta of a segment of angle a is
        r (1 - cos(a/2))), clamped to [8, 4096]. */
    inline int ellipseSegmentCount(double rxScreenPx, double ryScreenPx)
    {
      const double ax = std::fabs(rxScreenPx), ay = std::fabs(ryScreenPx);
      // (NaN if either radius is NaN: then 8)
      const double r = (ax != ax || ay != ay) ? ax + ay : (ax > ay ? ax : ay);
      const double tolerance = 0.25;
      if ( !(r > tolerance && r < 1e300) ) {
        return 8;
      }
      const double a = 2. * std::acos(1. - tolerance / r);
      double n = (a > 0.) ? std::ceil(2. * Detail::kPi / a) : 4096.;
      if ( !(n >= 8.) ) {
        n = 8.;
      } else if (n > 4096.) {
        n = 4096.;
      }
      return (int)n;
    }

    /** @brief Saves the OpenGL state that the core changes (glPushAttrib(kSavedAttribMask)), and
        sets the state common to all the primitives: no texture (1D, 2D, rectangle), lighting,
        depth or alpha test, face culling or polygon smoothing/stipple, filled polygons, "over"
        compositing ("The host should use "over" compositing when using a non-opaque colour"),
        and the colour. The destructor restores it (glPopAttrib).
        Not handled (the host's overlay state is expected to have the defaults, as for any
        OpenGL 1.x overlay): a bound GLSL program (glUseProgram), glBlendEquation, glColorMask,
        the stencil and scissor tests, and the texture units other than the active one.
        Disabling GL_TEXTURE_RECTANGLE sets GL_INVALID_ENUM on an implementation without
        ARB_texture_rectangle (OpenGL 1.1 software renderers); it has no other effect. */
    class ScopedGLState {
    public:
      ScopedGLState(const GLFunctions &gl, const OfxRGBAColourF &colour)
        : _gl(gl)
      {
        _gl.PushAttrib(kSavedAttribMask);
        _gl.Disable(kGLTexture1D);
        _gl.Disable(kGLTexture2D);
        _gl.Disable(kGLTextureRectangle);
        _gl.Disable(kGLLighting);
        _gl.Disable(kGLDepthTest);
        _gl.Disable(kGLCullFace);
        _gl.Disable(kGLAlphaTest);
        _gl.Disable(kGLPolygonSmooth);
        _gl.Disable(kGLPolygonStipple);
        _gl.PolygonMode(kGLFrontAndBack, kGLFill);
        _gl.Enable(kGLBlend);
        _gl.BlendFunc(kGLSrcAlpha, kGLOneMinusSrcAlpha);
        _gl.Color4f(colour.r, colour.g, colour.b, colour.a);
      }
      ~ScopedGLState()
      {
        _gl.PopAttrib();
      }
    private:
      ScopedGLState(const ScopedGLState &);
      ScopedGLState &operator=(const ScopedGLState &);
      const GLFunctions &_gl;
    };

    /** @brief The line width and stipple of state, for the lines and the ellipse (inside a
        ScopedGLState): width 0 is GL_LINE_SMOOTH off and max(1, round(ratio)) device pixels, any
        other width GL_LINE_SMOOTH (GL_DONT_CARE hint) and width * ratio device pixels (at most
        1e4; OpenGL clamps it to its own maximum). Solid: GL_LINE_STIPPLE off; else on, with
        stipplePattern() and the factor round(ratio) (1 to 256). */
    inline void applyLineState(const GLFunctions &gl, const State &state)
    {
      const double ratio = state.screenPixelRatio;
      if (state.lineWidth > 0.f) {
        gl.Enable(kGLLineSmooth);
        gl.Hint(kGLLineSmoothHint, kGLDontCare);
        const double w = (double)state.lineWidth * ratio;
        gl.LineWidth( (GLfloatT)(w < Detail::kMaxLineWidth ? w : Detail::kMaxLineWidth) );
      } else {
        gl.Disable(kGLLineSmooth);
        const double w = std::floor(ratio + 0.5);
        gl.LineWidth( (GLfloatT)(w > 1. ? w : 1.) );
      }
      if (state.stipple == kOfxDrawLineStipplePatternSolid) {
        gl.Disable(kGLLineStipple);
      } else {
        gl.Enable(kGLLineStipple);
        double f = std::floor(ratio + 0.5);
        f = f < 1. ? 1. : (f > 256. ? 256. : f);
        gl.LineStipple( (GLintT)f, stipplePattern(state.stipple) );
      }
    }

    /** @brief Text metrics, in screen pixels */
    struct TextMetrics {
      double width;   ///< from the left of the first glyph to the right of the last one
      double ascent;  ///< above the baseline
      double descent; ///< below the baseline (positive)
    };

    /** @brief The metrics of utf8 in the built-in font (see drawText): width 6 n - 1 for n glyphs
        (0 if there is none), ascent 7, descent 1. NULL is "". */
    inline TextMetrics builtinTextMetrics(const char *utf8)
    {
      int n = 0;
      for (const unsigned char *p = (const unsigned char *)utf8; p && *p; ++p) {
        if (Detail::glyphIndex(*p) >= 0) {
          ++n;
        }
      }
      TextMetrics m;
      m.width = n > 0 ? (double)n * kFontCellWidth - (kFontCellWidth - kFontGlyphWidth) : 0.;
      m.ascent = kFontGlyphHeight; // the glyphs sit on the baseline
      m.descent = kFontCellHeight - kFontGlyphHeight;
      return m;
    }

    /** @brief The left end of the baseline of a text of metrics m (screen pixels) drawn at pos
        (canonical coordinates) with the kOfxDrawTextAlignment* flags. Horizontal: Left (also
        the default, no flag), Right, or both (CenterH: centred). Vertical: Baseline (also the
        default), Top, Bottom, or Top with Bottom or Baseline (centred: kOfxDrawTextAlignmentCenterV
        is Top | Baseline, so Top | Bottom is accepted too); centred on the middle of the ascent
        and descent box. */
    inline OfxPointD alignText(const OfxPointD &pos, int alignment, const TextMetrics &m, const OfxPointD &pixelScale)
    {
      OfxPointD p = pos;
      const bool left = (alignment & kOfxDrawTextAlignmentLeft) != 0;
      const bool right = (alignment & kOfxDrawTextAlignmentRight) != 0;
      if (right && left) {
        p.x -= 0.5 * m.width * pixelScale.x;
      } else if (right) {
        p.x -= m.width * pixelScale.x;
      }
      const bool top = (alignment & kOfxDrawTextAlignmentTop) != 0;
      const bool bottom = (alignment & kOfxDrawTextAlignmentBottom) != 0;
      const bool onBaseline = (alignment & kOfxDrawTextAlignmentBaseline) != 0;
      if ( top && (bottom || onBaseline) ) {
        p.y -= 0.5 * (m.ascent - m.descent) * pixelScale.y;
      } else if (top) {
        p.y -= m.ascent * pixelScale.y;
      } else if (bottom) {
        p.y += m.descent * pixelScale.y;
      }
      return p;
    }

    /** @brief Draws utf8 with the built-in font, the left end of its baseline at baselineLeft
        (canonical coordinates), one font pixel per screen pixel (pixelScale): one GL_QUADS
        block, one quad per vertical run of pixels in each glyph column (a quad of one screen
        pixel covers exactly one pixel centre, so the text is sharp at any position). Only the
        glBegin/glVertex2d/glEnd calls: the caller sets the state and the colour (ScopedGLState).
        No OpenGL call if utf8 has no glyph. */
    inline void drawBuiltinText(const GLFunctions &gl, const char *utf8, const OfxPointD &baselineLeft, const OfxPointD &pixelScale)
    {
      bool begun = false;
      int i = 0; // the glyph position
      for (const unsigned char *p = (const unsigned char *)utf8; p && *p; ++p) {
        const int g = Detail::glyphIndex(*p);
        if (g < 0) {
          continue;
        }
        if (!begun) {
          gl.Begin(kGLQuads);
          begun = true;
        }
        const unsigned char *glyph = kFont5x7[g];
        for (int col = 0; col < kFontGlyphWidth; ++col) {
          const double xl = baselineLeft.x + (i * kFontCellWidth + col) * pixelScale.x;
          const double xr = xl + pixelScale.x;
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
            const double yt = baselineLeft.y + (kFontGlyphHeight - first) * pixelScale.y;
            const double yb = baselineLeft.y + (kFontGlyphHeight - row) * pixelScale.y;
            gl.Vertex2d(xl, yb);
            gl.Vertex2d(xr, yb);
            gl.Vertex2d(xr, yt);
            gl.Vertex2d(xl, yt);
          }
        }
        ++i;
      }
      if (begun) {
        gl.End();
      }
    }

    /** @brief The colour of the shadows of state: the standard background colour, with the
        alpha of the current colour */
    inline OfxRGBAColourF shadowColour(const State &state)
    {
      OfxRGBAColourF c;
      (void)standardColour(kOfxStandardColourOverlayBackground, &c);
      c.a = state.colour.a;
      return c;
    }

    /** @brief The offset of the shadows, in canonical coordinates: one device pixel right and
        down, (+pixelScale.x / screenPixelRatio, -pixelScale.y / screenPixelRatio) with the
        state's sanitised values (a screen pixel at ratio 1, half a screen pixel at ratio 2), as
        the OpenGL shadows of the Natron and SupportExt overlays */
    inline OfxPointD shadowOffset(const State &state)
    {
      OfxPointD o;
      o.x = state.pixelScale.x / state.screenPixelRatio;
      o.y = -state.pixelScale.y / state.screenPixelRatio;
      return o;
    }

    namespace Detail {
      inline void vertices(const GLFunctions &gl, GLenumT mode, const OfxPointD *points, int pointCount, const OfxPointD &o)
      {
        gl.Begin(mode);
        for (int i = 0; i < pointCount; ++i) {
          gl.Vertex2d(points[i].x + o.x, points[i].y + o.y);
        }
        gl.End();
      }

      // one pass of a valid primitive, offset by o (inside a ScopedGLState, line state applied)
      inline void primitive(const GLFunctions &gl, OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount,
                            const OfxPointD &o, int ellipseCount)
      {
        switch (primitive) {
        case kOfxDrawPrimitiveLines:
          vertices(gl, kGLLines, points, pointCount, o);
          break;
        case kOfxDrawPrimitiveLineStrip:
          vertices(gl, kGLLineStrip, points, pointCount, o);
          break;
        case kOfxDrawPrimitiveLineLoop:
          vertices(gl, kGLLineLoop, points, pointCount, o);
          break;
        case kOfxDrawPrimitivePolygon:
          vertices(gl, kGLPolygon, points, pointCount, o);
          break;
        case kOfxDrawPrimitiveRectangle:
          gl.Begin(kGLQuads);
          gl.Vertex2d(points[0].x + o.x, points[0].y + o.y);
          gl.Vertex2d(points[1].x + o.x, points[0].y + o.y);
          gl.Vertex2d(points[1].x + o.x, points[1].y + o.y);
          gl.Vertex2d(points[0].x + o.x, points[1].y + o.y);
          gl.End();
          break;
        case kOfxDrawPrimitiveEllipse: {
          // "an axis-aligned elliptical line (not filled) within the rectangle defined by 2
          // opposite corner points"
          const double cx = 0.5 * (points[0].x + points[1].x) + o.x;
          const double cy = 0.5 * (points[0].y + points[1].y) + o.y;
          const double rx = 0.5 * std::fabs(points[1].x - points[0].x);
          const double ry = 0.5 * std::fabs(points[1].y - points[0].y);
          gl.Begin(kGLLineLoop);
          for (int i = 0; i < ellipseCount; ++i) {
            const double t = 2. * kPi * i / ellipseCount;
            gl.Vertex2d(cx + rx * std::cos(t), cy + ry * std::sin(t));
          }
          gl.End();
          break;
        }
        default:
          break;
        }
      }
    } // namespace Detail

    /** @brief draw (OfxDrawSuiteV1::draw): validatePrimitive(), then the primitive with the
        state (and its shadow first if state.shadows), inside a ScopedGLState.
        kOfxStatErrValue for invalid arguments, kOfxStatFailed if gl is not complete. */
    inline OfxStatus drawPrimitive(const GLFunctions &gl, const State &state, OfxDrawPrimitive primitive,
                                   const OfxPointD *points, int pointCount)
    {
      const OfxStatus st = validatePrimitive(primitive, points, pointCount);
      if (st != kOfxStatOK) {
        return st;
      }
      if ( !isComplete(gl) ) {
        return kOfxStatFailed;
      }
      int ellipseCount = 0;
      if (primitive == kOfxDrawPrimitiveEllipse) {
        ellipseCount = ellipseSegmentCount(0.5 * (points[1].x - points[0].x) / state.pixelScale.x,
                                           0.5 * (points[1].y - points[0].y) / state.pixelScale.y);
      }
      ScopedGLState saver( gl, state.shadows ? shadowColour(state) : state.colour );
      if (primitive != kOfxDrawPrimitivePolygon && primitive != kOfxDrawPrimitiveRectangle) {
        applyLineState(gl, state); // the lines and the ellipse
      }
      OfxPointD none;
      none.x = none.y = 0.;
      if (state.shadows) {
        Detail::primitive( gl, primitive, points, pointCount, shadowOffset(state), ellipseCount );
        gl.Color4f(state.colour.r, state.colour.g, state.colour.b, state.colour.a);
      }
      Detail::primitive(gl, primitive, points, pointCount, none, ellipseCount);
      return kOfxStatOK;
    }

    /** @brief drawText (OfxDrawSuiteV1::drawText) with the built-in font, aligned with
        alignText() and builtinTextMetrics(), (and its shadow first if state.shadows), inside a
        ScopedGLState. kOfxStatErrValue for NULL utf8 or pos ("text or pos were not defined"),
        kOfxStatOK without any OpenGL call if utf8 has no glyph, kOfxStatFailed if gl is not
        complete. */
    inline OfxStatus drawText(const GLFunctions &gl, const State &state, const char *utf8, const OfxPointD *pos, int alignment)
    {
      if (!utf8 || !pos) {
        return kOfxStatErrValue;
      }
      const TextMetrics m = builtinTextMetrics(utf8);
      if ( !(m.width > 0.) ) {
        return kOfxStatOK; // no glyph
      }
      if ( !isComplete(gl) ) {
        return kOfxStatFailed;
      }
      const OfxPointD baselineLeft = alignText(*pos, alignment, m, state.pixelScale);
      ScopedGLState saver( gl, state.shadows ? shadowColour(state) : state.colour );
      if (state.shadows) {
        const OfxPointD o = shadowOffset(state);
        OfxPointD s;
        s.x = baselineLeft.x + o.x;
        s.y = baselineLeft.y + o.y;
        drawBuiltinText(gl, utf8, s, state.pixelScale);
        gl.Color4f(state.colour.r, state.colour.g, state.colour.b, state.colour.a);
      }
      drawBuiltinText(gl, utf8, baselineLeft, state.pixelScale);
      return kOfxStatOK;
    }

  } // namespace DrawSuiteOpenGL
} // namespace OFX

#endif // _ofxDrawSuiteOpenGL_h_
