// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef OFX_DRAWSUITE_OPENGL_HOST_H
#define OFX_DRAWSUITE_OPENGL_HOST_H

/** @file ofxhDrawSuiteOpenGL.h
  An optional, header-only helper for hosts that implement the OFX 1.5 DrawSuite hooks of
  OFX::Host::Interact::Instance (drawGetColour, drawSetColour, drawSetLineWidth,
  drawSetLineStipple, drawPrimitive, drawText; see ofxhInteract.h) with OpenGL. It draws with
  the OpenGL DrawSuite core, include/ofxDrawSuiteOpenGL.h, which the Support library's
  DrawSuite-to-OpenGL emulation also uses: a DrawSuite overlay then looks the same in the host
  (V2 draws) and through Support's emulation (V1 draws). Not part of the OFX standard.

  OpenGLDrawSuite holds the DrawSuite state of one draw action and the OpenGL function table.
  It is a member of the host's interact instance (composition), not a base class: the instance
  calls beginDraw() at the start of each kOfxInteractActionDraw (before calling the plug-in) and
  forwards its DrawSuite hooks to it. HostSupport has already checked the arguments of the
  hooks; OpenGLDrawSuite checks them again, so it can also be used without HostSupport.

  @code
  class MyInteract : public OFX::Host::ImageEffect::OverlayInteract {
    OFX::Host::Interact::OpenGLDrawSuite _ds; // _ds.setGLFunctions(&myTable) once
    ...
    OfxStatus drawAction(...)
    {
      // the OpenGL context of the view is current, with the overlay's matrices
      _ds.beginDraw(pixelScale, devicePixelRatio, drawSuiteShadows());
      return OverlayInteract::drawAction(...);
    }
    OfxStatus drawGetColour(OfxStandardColour c, OfxRGBAColourF *col) { return _ds.getColour(c, col); }
    OfxStatus drawSetColour(const OfxRGBAColourF *c) { return _ds.setColour(c); }
    OfxStatus drawSetLineWidth(float w) { return _ds.setLineWidth(w); }
    OfxStatus drawSetLineStipple(OfxDrawLineStipplePattern p) { return _ds.setLineStipple(p); }
    OfxStatus drawPrimitive(OfxDrawPrimitive p, const OfxPointD *pts, int n) { return _ds.drawPrimitive(p, pts, n); }
    OfxStatus drawText(const char *t, const OfxPointD *pos, int a) { return _ds.drawText(t, pos, a); }
  };
  @endcode

  A host customises it by deriving from it:
  - getColour(): the host's standard overlay colours (e.g. from its settings); the default is
    OFX::DrawSuiteOpenGL::standardColour().
  - measureText() and renderText(): the host's own font (e.g. Qt text). If measureText() returns
    true, drawText() computes the left end of the baseline with
    OFX::DrawSuiteOpenGL::alignText() and the metrics, and calls renderText() (twice with
    shadows: first offset, in the shadow colour, see below). Else drawText() uses the built-in
    5x7 font of the core.
  - renderPrimitive(): another renderer for the primitives; the default is the core
    (OFX::DrawSuiteOpenGL::drawPrimitive()).

  Shadows (kNatronOfxInteractPropDrawSuiteShadows, ofxNatron.h): beginDraw(..., shadows) sets
  State::shadows. Then drawPrimitive() (the lines, strips, loops and the ellipse outline; not the
  filled Rectangle and Polygon, see OFX::DrawSuiteOpenGL::hasShadow()) and drawText() draw each
  element twice: first offset by shadowOffset() (by default one device pixel right and down, as
  the OpenGL shadows of the SupportExt and Natron overlays and Support's V1 emulation; a host
  can override it), in shadowColour() (by default
  getColour() of kOfxStandardColourOverlayBackground, so the host's background colour, unlike
  Support's V1 emulation, which can't query the host and uses the core's black; with the
  current colour's alpha; a host can override it), then normally. OpenGLDrawSuite makes the two passes for every element, with
  the built-in font or the host's: renderPrimitive() and renderText() (and the built-in font)
  are called once per pass, with state().colour the colour of the pass and state().shadows
  false, so they draw one pass only. Plug-ins never draw shadows with DrawSuite calls.

  Header-only C++98, no OpenGL header (see ofxDrawSuiteOpenGL.h); HostSupport doesn't use it.
 */

#include <cstddef>
#include <vector>

#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxDrawSuiteOpenGL.h"

namespace OFX {
  namespace Host {
    namespace Interact {

      /// The DrawSuite of an interact instance, drawn with OpenGL (see the file documentation)
      class OpenGLDrawSuite {
      public:
        typedef OFX::DrawSuiteOpenGL::GLFunctions GLFunctions;
        typedef OFX::DrawSuiteOpenGL::State State;
        typedef OFX::DrawSuiteOpenGL::TextMetrics TextMetrics;

        /// gl: the OpenGL functions (not copied: it must outlive this object), NULL if none yet
        explicit OpenGLDrawSuite(const GLFunctions *gl = NULL)
          : _gl(gl)
        {
          OfxPointD one;
          one.x = one.y = 1.;
          OFX::DrawSuiteOpenGL::resetState(_state, one, 1., false);
        }

        virtual ~OpenGLDrawSuite() {}

        /// the OpenGL functions (not copied), NULL for none: then drawing returns kOfxStatFailed
        void setGLFunctions(const GLFunctions *gl) { _gl = gl; }
        const GLFunctions *glFunctions() const { return _gl; }

        /// Start a draw action: reset the state (opaque white, width 0, solid) with the
        /// interact's kOfxInteractPropPixelScale, the screen pixel ratio (device pixels per
        /// screen pixel) and the shadows setting (sanitised, see
        /// OFX::DrawSuiteOpenGL::resetState())
        void beginDraw(const OfxPointD &pixelScale, double screenPixelRatio, bool shadows)
        {
          OFX::DrawSuiteOpenGL::resetState(_state, pixelScale, screenPixelRatio, shadows);
        }

        /// the DrawSuite state of the current draw action
        const State &state() const { return _state; }

        /// OfxDrawSuiteV1::getColour: kOfxStatErrValue for NULL or an invalid std_colour. The
        /// default returns OFX::DrawSuiteOpenGL::standardColour(); the shadow colour is
        /// kOfxStandardColourOverlayBackground from this function.
        virtual OfxStatus getColour(OfxStandardColour stdColour, OfxRGBAColourF *colour) const
        {
          return OFX::DrawSuiteOpenGL::standardColour(stdColour, colour);
        }

        /// OfxDrawSuiteV1::setColour (kOfxStatErrValue for NULL)
        OfxStatus setColour(const OfxRGBAColourF *colour)
        {
          return OFX::DrawSuiteOpenGL::setColour(_state, colour);
        }

        /// OfxDrawSuiteV1::setLineWidth, in screen pixels (kOfxStatFailed if negative, NaN or infinite)
        OfxStatus setLineWidth(float width)
        {
          return OFX::DrawSuiteOpenGL::setLineWidth(_state, width);
        }

        /// OfxDrawSuiteV1::setLineStipple (kOfxStatErrValue for an invalid pattern)
        OfxStatus setLineStipple(OfxDrawLineStipplePattern pattern)
        {
          return OFX::DrawSuiteOpenGL::setLineStipple(_state, pattern);
        }

        /// OfxDrawSuiteV1::draw: OFX::DrawSuiteOpenGL::validatePrimitive(), then
        /// renderPrimitive() (twice with shadows: first offset, in the shadow colour)
        OfxStatus drawPrimitive(OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount)
        {
          const OfxStatus st = OFX::DrawSuiteOpenGL::validatePrimitive(primitive, points, pointCount);
          if (st != kOfxStatOK) {
            return st;
          }
          PassState pass(_state);
          if ( pass.shadows && OFX::DrawSuiteOpenGL::hasShadow(primitive) ) {
            const OfxPointD o = shadowOffset();
            std::vector<OfxPointD> shadow(points, points + pointCount);
            for (size_t i = 0; i < shadow.size(); ++i) {
              shadow[i].x += o.x;
              shadow[i].y += o.y;
            }
            _state.colour = shadowColour();
            const OfxStatus sst = renderPrimitive(primitive, &shadow[0], pointCount);
            _state.colour = pass.colour;
            if (sst != kOfxStatOK) {
              return sst;
            }
          }
          return renderPrimitive(primitive, points, pointCount);
        }

        /// OfxDrawSuiteV1::drawText: kOfxStatErrValue for NULL text or pos, kOfxStatOK for "".
        /// If measureText() returns false, the built-in font (OFX::DrawSuiteOpenGL::drawText());
        /// else renderText() at OFX::DrawSuiteOpenGL::alignText() of the metrics. Twice with
        /// shadows: first offset (shadowOffset()), in shadowColour(), then the text.
        /// The font is decided once per call, by the first renderText() call (the shadow pass
        /// with shadows, else the text): if it returns kOfxStatReplyDefault, both passes use the
        /// built-in font, so the shadow and the text never get different fonts and the shadow
        /// is still drawn first (under the text). A renderText() that draws the shadow and then
        /// declines the text breaks its contract (see renderText()): the text is then drawn
        /// with the built-in font.
        OfxStatus drawText(const char *utf8, const OfxPointD *pos, int alignment)
        {
          if (!utf8 || !pos) {
            return kOfxStatErrValue; // "text or pos were not defined"
          }
          if (!*utf8) {
            return kOfxStatOK;
          }
          TextMetrics m;
          m.width = m.ascent = m.descent = 0.;
          bool hostFont = measureText(utf8, m);
          const bool builtinOK = _gl && OFX::DrawSuiteOpenGL::isComplete(*_gl);
          if (!hostFont && !builtinOK) {
            return kOfxStatFailed;
          }
          const OfxPointD baselineLeft = hostFont ? OFX::DrawSuiteOpenGL::alignText(*pos, alignment, m, _state.pixelScale) : *pos;
          PassState pass(_state);
          if (pass.shadows) {
            const OfxPointD o = shadowOffset();
            _state.colour = shadowColour();
            OfxStatus sst = kOfxStatReplyDefault;
            if (hostFont) {
              OfxPointD s;
              s.x = baselineLeft.x + o.x;
              s.y = baselineLeft.y + o.y;
              sst = renderText(utf8, s, m);
              if (sst == kOfxStatReplyDefault) {
                hostFont = false; // the host can't render this text: the built-in font, both passes
              }
            }
            if (!hostFont) {
              if (!builtinOK) {
                return kOfxStatFailed;
              }
              OfxPointD s;
              s.x = pos->x + o.x;
              s.y = pos->y + o.y;
              sst = OFX::DrawSuiteOpenGL::drawText(*_gl, _state, utf8, &s, alignment);
            }
            _state.colour = pass.colour;
            if (sst != kOfxStatOK) {
              return sst;
            }
          }
          if (hostFont) {
            const OfxStatus st = renderText(utf8, baselineLeft, m);
            if (st != kOfxStatReplyDefault) {
              return st;
            }
          }
          if (!builtinOK) {
            return kOfxStatFailed;
          }
          return OFX::DrawSuiteOpenGL::drawText(*_gl, _state, utf8, pos, alignment);
        }

      protected:
        /// Draw a valid primitive with state(), one pass (state().shadows is false during the
        /// call: drawPrimitive() makes the shadow pass). The default is the core
        /// (OFX::DrawSuiteOpenGL::drawPrimitive()); kOfxStatFailed without an OpenGL table.
        virtual OfxStatus renderPrimitive(OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount)
        {
          if (!_gl) {
            return kOfxStatFailed;
          }
          return OFX::DrawSuiteOpenGL::drawPrimitive(*_gl, _state, primitive, points, pointCount);
        }

        /// The metrics of utf8 (not empty) in the host's font, in screen pixels: return false
        /// (the default) to draw with the built-in font instead.
        virtual bool measureText(const char * /*utf8*/, TextMetrics & /*metrics*/)
        {
          return false;
        }

        /// Draw utf8 in the host's font, in state().colour, the left end of its baseline at
        /// baselineLeft (canonical coordinates), with the metrics that measureText() returned.
        /// Only called if measureText() returned true; drawText() handles the alignment and the
        /// shadows. Return kOfxStatReplyDefault, without drawing, if this view can't render text
        /// (e.g. Natron's CustomParamInteract, whose OverlaySupport has no way to say so in
        /// advance): drawText() then draws with the built-in font instead. Return it
        /// consistently for a given view within a draw, otherwise the shadow and the text are
        /// drawn with different fonts. The default draws the
        /// built-in font there (kOfxStatFailed without an OpenGL table).
        virtual OfxStatus renderText(const char *utf8, const OfxPointD &baselineLeft, const TextMetrics & /*metrics*/)
        {
          if ( !_gl || !OFX::DrawSuiteOpenGL::isComplete(*_gl) ) {
            return kOfxStatFailed;
          }
          OFX::DrawSuiteOpenGL::ScopedGLState saver(*_gl, _state.colour);
          OFX::DrawSuiteOpenGL::drawBuiltinText(*_gl, utf8, baselineLeft, _state.pixelScale);
          return kOfxStatOK;
        }

        /// The colour of the shadow passes. The default is
        /// getColour(kOfxStandardColourOverlayBackground) (black if it fails), with the alpha of
        /// the current colour (state().colour). A host whose background colour is not a good
        /// shadow colour (e.g. the grey of its viewer background) can return black instead; keep
        /// the current alpha.
        virtual OfxRGBAColourF shadowColour() const
        {
          OfxRGBAColourF c;
          if (getColour(kOfxStandardColourOverlayBackground, &c) != kOfxStatOK) {
            c.r = c.g = c.b = 0.f;
          }
          c.a = _state.colour.a;
          return c;
        }

        /// The offset of the shadow passes, in canonical coordinates. The default is one device
        /// pixel right and down, (+pixelScale.x / screenPixelRatio, -pixelScale.y /
        /// screenPixelRatio) of state(), as Support's DrawSuite-to-OpenGL emulation
        /// (OFX::DrawSuiteOpenGL::shadowOffset()) and the OpenGL shadows of the Natron and
        /// SupportExt overlays. A host can override it for another offset.
        virtual OfxPointD shadowOffset() const
        {
          return OFX::DrawSuiteOpenGL::shadowOffset(_state);
        }

      private:
        // During drawPrimitive() and drawText(): state().shadows is false (each hook call draws
        // one pass), and the colour and shadows flag are restored at the end, even if a hook
        // throws.
        struct PassState {
          explicit PassState(State &s) : state(s), colour(s.colour), shadows(s.shadows) { s.shadows = false; }
          ~PassState() { state.colour = colour; state.shadows = shadows; }
          State &state;
          const OfxRGBAColourF colour;
          const bool shadows;
        private:
          PassState(const PassState &);
          PassState &operator=(const PassState &);
        };

        const GLFunctions *_gl;
        State _state;
      };

    } // namespace Interact
  } // namespace Host
} // namespace OFX

#endif // OFX_DRAWSUITE_OPENGL_HOST_H
