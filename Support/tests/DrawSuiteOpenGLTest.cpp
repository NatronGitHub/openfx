// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Self-test of the OpenGL DrawSuite core, include/ofxDrawSuiteOpenGL.h, with a recording fake
  OpenGL function table: no OpenGL header, no OpenGL library, no context, so it is built and run
  on every platform (CMake target drawsuite-opengl-test, run by .github/scripts/cmake-build-test.sh).

  It checks the OpenGL calls of every DrawSuite function: the glPushAttrib/glPopAttrib balance and
  mask, the blend function, the colour, the glBegin mode and the vertex count of every primitive,
  the line width (smooth or width 0) and stipple (factor and patterns), the ellipse tessellation,
  the error statuses, the text alignment and metrics, the built-in font's quads, resetState()'s
  sanitising, and the shadow pass. It also tests HostSupport's helper,
  OFX::Host::Interact::OpenGLDrawSuite (HostSupport/include/ofxhDrawSuiteOpenGL.h): beginDraw(),
  the built-in font fallback, the host text hooks, kOfxStatFailed without a table, and the
  shadow passes.

  Prints one line per failed check, and exits with 0 if all passed.
*/

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ofxDrawSuiteOpenGL.h"
#include "ofxhDrawSuiteOpenGL.h"

using namespace OFX::DrawSuiteOpenGL;

static int gFailures = 0;

#define CHECK(cond) \
  do { if (!(cond)) { ++gFailures; std::printf("FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

////////////////////////////////////////////////////////////////////////////////
// The recording fake OpenGL

enum Fn { eBegin, eEnd, eVertex2d, eColor4f, eEnable, eDisable, eHint, eLineWidth, eLineStipple,
          ePolygonMode, eBlendFunc, ePushAttrib, ePopAttrib };

struct Call {
  Fn fn;
  double a[4];
};

static std::vector<Call> gCalls;

static void record(Fn fn, double a0 = 0., double a1 = 0., double a2 = 0., double a3 = 0.)
{
  Call c;
  c.fn = fn;
  c.a[0] = a0; c.a[1] = a1; c.a[2] = a2; c.a[3] = a3;
  gCalls.push_back(c);
}

static void OFX_DRAWGL_APIENTRY fakeBegin(GLenumT mode) { record(eBegin, mode); }
static void OFX_DRAWGL_APIENTRY fakeEnd(void) { record(eEnd); }
static void OFX_DRAWGL_APIENTRY fakeVertex2d(GLdoubleT x, GLdoubleT y) { record(eVertex2d, x, y); }
static void OFX_DRAWGL_APIENTRY fakeColor4f(GLfloatT r, GLfloatT g, GLfloatT b, GLfloatT a) { record(eColor4f, r, g, b, a); }
static void OFX_DRAWGL_APIENTRY fakeEnable(GLenumT cap) { record(eEnable, cap); }
static void OFX_DRAWGL_APIENTRY fakeDisable(GLenumT cap) { record(eDisable, cap); }
static void OFX_DRAWGL_APIENTRY fakeHint(GLenumT target, GLenumT mode) { record(eHint, target, mode); }
static void OFX_DRAWGL_APIENTRY fakeLineWidth(GLfloatT width) { record(eLineWidth, width); }
static void OFX_DRAWGL_APIENTRY fakeLineStipple(GLintT factor, GLushortT pattern) { record(eLineStipple, factor, pattern); }
static void OFX_DRAWGL_APIENTRY fakePolygonMode(GLenumT face, GLenumT mode) { record(ePolygonMode, face, mode); }
static void OFX_DRAWGL_APIENTRY fakeBlendFunc(GLenumT s, GLenumT d) { record(eBlendFunc, s, d); }
static void OFX_DRAWGL_APIENTRY fakePushAttrib(GLbitfieldT mask) { record(ePushAttrib, mask); }
static void OFX_DRAWGL_APIENTRY fakePopAttrib(void) { record(ePopAttrib); }

// the resolver of loadGLFunctions(): userData, if not NULL, is a name to leave unresolved
static void *fakeResolver(const char *name, void *userData)
{
  if ( userData && std::strcmp(name, (const char *)userData) == 0 ) {
    return NULL;
  }
  struct Entry { const char *name; void (*f)(); };
#define OFX_TEST_ENTRY(n, f) { n, (void (*)())(f) }
  // (the function pointers are converted back to their own type by loadGLFunctions)
  static const Entry entries[] = {
    OFX_TEST_ENTRY("glBegin", fakeBegin), OFX_TEST_ENTRY("glEnd", fakeEnd),
    OFX_TEST_ENTRY("glVertex2d", fakeVertex2d), OFX_TEST_ENTRY("glColor4f", fakeColor4f),
    OFX_TEST_ENTRY("glEnable", fakeEnable), OFX_TEST_ENTRY("glDisable", fakeDisable),
    OFX_TEST_ENTRY("glHint", fakeHint), OFX_TEST_ENTRY("glLineWidth", fakeLineWidth),
    OFX_TEST_ENTRY("glLineStipple", fakeLineStipple), OFX_TEST_ENTRY("glPolygonMode", fakePolygonMode),
    OFX_TEST_ENTRY("glBlendFunc", fakeBlendFunc), OFX_TEST_ENTRY("glPushAttrib", fakePushAttrib),
    OFX_TEST_ENTRY("glPopAttrib", fakePopAttrib),
  };
#undef OFX_TEST_ENTRY
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
    if (std::strcmp(name, entries[i].name) == 0) {
      void *p = NULL;
      std::memcpy(&p, &entries[i].f, sizeof(p));
      return p;
    }
  }
  return NULL;
}

static GLFunctions fakeGL()
{
  GLFunctions gl;
  std::memset(&gl, 0, sizeof(gl));
  gl.Begin = fakeBegin; gl.End = fakeEnd; gl.Vertex2d = fakeVertex2d; gl.Color4f = fakeColor4f;
  gl.Enable = fakeEnable; gl.Disable = fakeDisable; gl.Hint = fakeHint; gl.LineWidth = fakeLineWidth;
  gl.LineStipple = fakeLineStipple; gl.PolygonMode = fakePolygonMode; gl.BlendFunc = fakeBlendFunc;
  gl.PushAttrib = fakePushAttrib; gl.PopAttrib = fakePopAttrib;
  return gl;
}

////////////////////////////////////////////////////////////////////////////////
// Analysis of the recorded calls

struct Block {
  unsigned mode;
  std::vector<OfxPointD> vertices;
  double colour[4]; // the last glColor4f before glBegin
};

static std::vector<Block> blocks()
{
  std::vector<Block> r;
  double colour[4] = { -1., -1., -1., -1. };
  bool inBlock = false;
  for (size_t i = 0; i < gCalls.size(); ++i) {
    const Call &c = gCalls[i];
    if (c.fn == eColor4f) {
      for (int k = 0; k < 4; ++k) {
        colour[k] = c.a[k];
      }
    } else if (c.fn == eBegin) {
      CHECK(!inBlock);
      inBlock = true;
      Block b;
      b.mode = (unsigned)c.a[0];
      for (int k = 0; k < 4; ++k) {
        b.colour[k] = colour[k];
      }
      r.push_back(b);
    } else if (c.fn == eVertex2d) {
      CHECK(inBlock);
      if (!r.empty()) {
        OfxPointD p;
        p.x = c.a[0];
        p.y = c.a[1];
        r.back().vertices.push_back(p);
      }
    } else if (c.fn == eEnd) {
      CHECK(inBlock);
      inBlock = false;
    } else {
      CHECK(!inBlock); // only glVertex2d (and glColor4f) between glBegin and glEnd
    }
  }
  CHECK(!inBlock);
  return r;
}

static int count(Fn fn, double a0)
{
  int n = 0;
  for (size_t i = 0; i < gCalls.size(); ++i) {
    if (gCalls[i].fn == fn && gCalls[i].a[0] == a0) {
      ++n;
    }
  }
  return n;
}

static int count(Fn fn)
{
  int n = 0;
  for (size_t i = 0; i < gCalls.size(); ++i) {
    n += gCalls[i].fn == fn;
  }
  return n;
}

static const Call *find(Fn fn)
{
  for (size_t i = 0; i < gCalls.size(); ++i) {
    if (gCalls[i].fn == fn) {
      return &gCalls[i];
    }
  }
  return NULL;
}

static bool near(double a, double b, double eps = 1e-6)
{
  return std::fabs(a - b) <= eps;
}

// the state save of one draw: glPushAttrib(kSavedAttribMask) first, glPopAttrib last, once each,
// "over" compositing, filled polygons, no texture
static void checkSavedState()
{
  CHECK(!gCalls.empty());
  if ( gCalls.empty() ) {
    return;
  }
  CHECK(gCalls.front().fn == ePushAttrib);
  CHECK((unsigned)gCalls.front().a[0] == kSavedAttribMask);
  CHECK(kSavedAttribMask == (0x1u | 0x4u | 0x8u | 0x2000u | 0x4000u | 0x8000u));
  CHECK(gCalls.back().fn == ePopAttrib);
  CHECK(count(ePushAttrib) == 1);
  CHECK(count(ePopAttrib) == 1);
  const Call *b = find(eBlendFunc);
  CHECK(b && b->a[0] == 0x0302 && b->a[1] == 0x0303); // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
  CHECK(count(eEnable, 0x0BE2) == 1); // GL_BLEND
  const Call *pm = find(ePolygonMode);
  CHECK(pm && pm->a[0] == 0x0408 && pm->a[1] == 0x1B02); // GL_FRONT_AND_BACK, GL_FILL
  CHECK(count(eDisable, 0x0DE0) == 1 && count(eDisable, 0x0DE1) == 1 && count(eDisable, 0x84F5) == 1);
  CHECK(count(eDisable, 0x0B50) == 1 && count(eDisable, 0x0B71) == 1); // lighting, depth test
}

static OfxPointD pt(double x, double y)
{
  OfxPointD p;
  p.x = x;
  p.y = y;
  return p;
}

static State makeState(double psx = 1., double psy = 1., double ratio = 1., bool shadows = false)
{
  State s;
  resetState(s, pt(psx, psy), ratio, shadows);
  return s;
}

////////////////////////////////////////////////////////////////////////////////
// The tests

static void testLoad()
{
  GLFunctions gl;
  std::memset(&gl, 0xFF, sizeof(gl)); // every pointer is set by loadGLFunctions
  CHECK(loadGLFunctions(gl, fakeResolver, NULL));
  CHECK(isComplete(gl));
  CHECK(gl.Begin == fakeBegin && gl.PopAttrib == fakePopAttrib && gl.LineStipple == fakeLineStipple);
  char missing[] = "glHint";
  CHECK(!loadGLFunctions(gl, fakeResolver, missing));
  CHECK(gl.Hint == NULL && gl.Begin == fakeBegin && gl.PopAttrib == fakePopAttrib);
  CHECK(!isComplete(gl));
  CHECK(!loadGLFunctions(gl, NULL, NULL));
  CHECK(gl.Begin == NULL);
  CHECK(isComplete(fakeGL()));
}

static void testResetState()
{
  State s;
  std::memset(&s, 0, sizeof(s));
  resetState(s, pt(-2., 0.5), 2., true);
  CHECK(s.colour.r == 1.f && s.colour.g == 1.f && s.colour.b == 1.f && s.colour.a == 1.f);
  CHECK(s.lineWidth == 0.f && s.stipple == kOfxDrawLineStipplePatternSolid);
  CHECK(s.pixelScale.x == 2. && s.pixelScale.y == 0.5);
  CHECK(s.screenPixelRatio == 2. && s.shadows);
  const double nan = std::sqrt(-1.), inf = 1e308 * 10.;
  resetState(s, pt(nan, 0.), nan, false);
  CHECK(s.pixelScale.x == 1. && s.pixelScale.y == 1. && s.screenPixelRatio == 1. && !s.shadows);
  resetState(s, pt(inf, -inf), inf, false);
  CHECK(s.pixelScale.x == 1. && s.pixelScale.y == 1. && s.screenPixelRatio == 1.);
  resetState(s, pt(1., 1.), -1., false);
  CHECK(s.screenPixelRatio == 1.);
  resetState(s, pt(1., 1.), 0., false);
  CHECK(s.screenPixelRatio == 1.);
  resetState(s, pt(1., 1.), 100., false);
  CHECK(s.screenPixelRatio == 64.);
  resetState(s, pt(1., 1.), 0.25, false);
  CHECK(s.screenPixelRatio == 0.25);
}

static void testStateSetters()
{
  OfxRGBAColourF c;
  CHECK(standardColour(kOfxStandardColourOverlayBackground, &c) == kOfxStatOK);
  CHECK(c.r == 0.f && c.g == 0.f && c.b == 0.f && c.a == 1.f);
  CHECK(standardColour(kOfxStandardColourOverlayActive, &c) == kOfxStatOK && c.g == 1.f && c.r == 0.f);
  CHECK(standardColour(kOfxStandardColourOverlaySelected, &c) == kOfxStatOK && c.r == 1.f && c.g == 0.f);
  CHECK(standardColour(kOfxStandardColourOverlayDeselected, &c) == kOfxStatOK && c.r == 0.8f && c.b == 0.8f);
  CHECK(standardColour(kOfxStandardColourOverlayMarqueeFG, &c) == kOfxStatOK && c.r == 0.5f && c.a == 1.f);
  CHECK(standardColour(kOfxStandardColourOverlayMarqueeBG, &c) == kOfxStatOK && c.b == 1.f && c.a == 0.4f);
  CHECK(standardColour(kOfxStandardColourOverlayText, &c) == kOfxStatOK && c.r == 1.f && c.g == 1.f && c.a == 1.f);
  CHECK(standardColour((OfxStandardColour)7, &c) == kOfxStatErrValue);
  CHECK(standardColour((OfxStandardColour)-1, &c) == kOfxStatErrValue);
  CHECK(standardColour(kOfxStandardColourOverlayText, NULL) == kOfxStatErrValue);

  State s = makeState();
  const OfxRGBAColourF red = { 1.f, 0.f, 0.f, 0.5f };
  CHECK(setColour(s, &red) == kOfxStatOK && s.colour.r == 1.f && s.colour.g == 0.f && s.colour.a == 0.5f);
  CHECK(setColour(s, NULL) == kOfxStatErrValue && s.colour.r == 1.f && s.colour.a == 0.5f);
  CHECK(setLineWidth(s, 2.5f) == kOfxStatOK && s.lineWidth == 2.5f);
  CHECK(setLineWidth(s, -1.f) == kOfxStatFailed && s.lineWidth == 2.5f);
  CHECK(setLineWidth(s, std::sqrt(-1.f)) == kOfxStatFailed && s.lineWidth == 2.5f);
  CHECK(setLineWidth(s, 1e30f * 1e30f) == kOfxStatFailed && s.lineWidth == 2.5f);
  CHECK(setLineWidth(s, 0.f) == kOfxStatOK && s.lineWidth == 0.f);
  CHECK(setLineStipple(s, kOfxDrawLineStipplePatternDotDash) == kOfxStatOK && s.stipple == kOfxDrawLineStipplePatternDotDash);
  CHECK(setLineStipple(s, (OfxDrawLineStipplePattern)5) == kOfxStatErrValue && s.stipple == kOfxDrawLineStipplePatternDotDash);
  CHECK(setLineStipple(s, (OfxDrawLineStipplePattern)-1) == kOfxStatErrValue);

  CHECK(stipplePattern(kOfxDrawLineStipplePatternSolid) == 0xFFFF);
  CHECK(stipplePattern(kOfxDrawLineStipplePatternDot) == 0x3333);
  CHECK(stipplePattern(kOfxDrawLineStipplePatternDash) == 0x00FF);
  CHECK(stipplePattern(kOfxDrawLineStipplePatternAltDash) == 0xFF00);
  CHECK(stipplePattern(kOfxDrawLineStipplePatternDotDash) == 0x1FE3);
}

static void testStatuses()
{
  const GLFunctions gl = fakeGL();
  const State s = makeState();
  OfxPointD p[5];
  for (int i = 0; i < 5; ++i) {
    p[i] = pt(i, 2 * i);
  }
  struct Case { int prim; int n; bool nullPoints; OfxStatus expected; };
  const Case cases[] = {
    { kOfxDrawPrimitiveLines, 2, false, kOfxStatOK }, { kOfxDrawPrimitiveLines, 4, false, kOfxStatOK },
    { kOfxDrawPrimitiveLines, 1, false, kOfxStatErrValue }, { kOfxDrawPrimitiveLines, 3, false, kOfxStatErrValue },
    { kOfxDrawPrimitiveLines, 5, false, kOfxStatErrValue }, { kOfxDrawPrimitiveLines, 0, false, kOfxStatErrValue },
    { kOfxDrawPrimitiveLineStrip, 2, false, kOfxStatOK }, { kOfxDrawPrimitiveLineStrip, 1, false, kOfxStatErrValue },
    { kOfxDrawPrimitiveLineStrip, 3, false, kOfxStatOK }, { kOfxDrawPrimitiveLineLoop, 3, false, kOfxStatOK },
    { kOfxDrawPrimitiveLineLoop, 1, false, kOfxStatErrValue }, { kOfxDrawPrimitivePolygon, 3, false, kOfxStatOK },
    { kOfxDrawPrimitivePolygon, 2, false, kOfxStatErrValue }, { kOfxDrawPrimitivePolygon, 5, false, kOfxStatOK },
    { kOfxDrawPrimitiveRectangle, 2, false, kOfxStatOK }, { kOfxDrawPrimitiveRectangle, 1, false, kOfxStatErrValue },
    { kOfxDrawPrimitiveRectangle, 3, false, kOfxStatErrValue }, { kOfxDrawPrimitiveEllipse, 2, false, kOfxStatOK },
    { kOfxDrawPrimitiveEllipse, 1, false, kOfxStatErrValue }, { kOfxDrawPrimitiveEllipse, 3, false, kOfxStatErrValue },
    { kOfxDrawPrimitiveLineStrip, 2, true, kOfxStatErrValue }, { kOfxDrawPrimitiveEllipse, 2, true, kOfxStatErrValue },
    { 6, 2, false, kOfxStatErrValue }, { -1, 2, false, kOfxStatErrValue },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const Case &c = cases[i];
    const OfxPointD *pts = c.nullPoints ? NULL : p;
    CHECK(validatePrimitive((OfxDrawPrimitive)c.prim, pts, c.n) == c.expected);
    gCalls.clear();
    const OfxStatus st = drawPrimitive(gl, s, (OfxDrawPrimitive)c.prim, pts, c.n);
    if (st != c.expected) {
      std::printf("  case %d: primitive %d, %d points%s: status %d\n", (int)i, c.prim, c.n, c.nullPoints ? " (NULL)" : "", st);
    }
    CHECK(st == c.expected);
    CHECK( (st == kOfxStatOK) == !gCalls.empty() ); // no OpenGL call on error
  }
  // text
  gCalls.clear();
  OfxPointD pos = pt(0., 0.);
  CHECK(drawText(gl, s, NULL, &pos, 0) == kOfxStatErrValue);
  CHECK(drawText(gl, s, "A", NULL, 0) == kOfxStatErrValue);
  CHECK(drawText(gl, s, "", &pos, 0) == kOfxStatOK);
  CHECK(drawText(gl, s, "\x01\t\n\x1f\x7f", &pos, 0) == kOfxStatOK); // control characters: nothing
  CHECK(gCalls.empty());
  // an incomplete table: kOfxStatFailed, no call (the argument errors first)
  GLFunctions partial = gl;
  partial.Hint = NULL;
  CHECK(drawPrimitive(partial, s, kOfxDrawPrimitiveLines, p, 2) == kOfxStatFailed);
  CHECK(drawPrimitive(partial, s, kOfxDrawPrimitiveLines, p, 3) == kOfxStatErrValue);
  CHECK(drawText(partial, s, "A", &pos, 0) == kOfxStatFailed);
  CHECK(drawText(partial, s, "", &pos, 0) == kOfxStatOK);
  CHECK(gCalls.empty());
}

static void testPrimitives()
{
  const GLFunctions gl = fakeGL();
  State s = makeState(0.5, 0.25);
  const OfxRGBAColourF col = { 0.25f, 0.5f, 0.75f, 0.5f };
  setColour(s, &col);
  OfxPointD p[5];
  for (int i = 0; i < 5; ++i) {
    p[i] = pt(10. + i, 20. - 3 * i);
  }
  struct Case { OfxDrawPrimitive prim; int n; unsigned mode; int vertices; bool lines; };
  const Case cases[] = {
    { kOfxDrawPrimitiveLines, 4, 0x0001, 4, true },
    { kOfxDrawPrimitiveLineStrip, 3, 0x0003, 3, true },
    { kOfxDrawPrimitiveLineLoop, 5, 0x0002, 5, true },
    { kOfxDrawPrimitivePolygon, 5, 0x0009, 5, false },
    { kOfxDrawPrimitiveRectangle, 2, 0x0007, 4, false },
    // radii 0.5 / 0.5 = 1 and 1.5 / 0.25 = 6 screen pixels
    { kOfxDrawPrimitiveEllipse, 2, 0x0002, ellipseSegmentCount(1., 6.), true },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const Case &c = cases[i];
    gCalls.clear();
    CHECK(drawPrimitive(gl, s, c.prim, p, c.n) == kOfxStatOK);
    checkSavedState();
    const std::vector<Block> b = blocks();
    CHECK(b.size() == 1);
    if (b.size() != 1) {
      continue;
    }
    CHECK(b[0].mode == c.mode);
    CHECK((int)b[0].vertices.size() == c.vertices);
    CHECK(b[0].colour[0] == 0.25 && b[0].colour[1] == 0.5 && b[0].colour[2] == 0.75 && b[0].colour[3] == 0.5);
    CHECK(count(eColor4f) == 1);
    CHECK(count(eLineWidth) == (c.lines ? 1 : 0)); // the line state only for the lines and the ellipse
    if (c.prim != kOfxDrawPrimitiveRectangle && c.prim != kOfxDrawPrimitiveEllipse) {
      for (int k = 0; k < c.n && k < (int)b[0].vertices.size(); ++k) {
        CHECK(b[0].vertices[k].x == p[k].x && b[0].vertices[k].y == p[k].y);
      }
    }
  }
  // the rectangle's corners, from 2 opposite corners
  gCalls.clear();
  drawPrimitive(gl, s, kOfxDrawPrimitiveRectangle, p, 2);
  std::vector<Block> b = blocks();
  if (b.size() == 1 && b[0].vertices.size() == 4) {
    const std::vector<OfxPointD> &v = b[0].vertices;
    CHECK(v[0].x == 10. && v[0].y == 20.);
    CHECK(v[1].x == 11. && v[1].y == 20.);
    CHECK(v[2].x == 11. && v[2].y == 17.);
    CHECK(v[3].x == 10. && v[3].y == 17.);
  }
  // the ellipse: on the ellipse inscribed in the rectangle (any 2 opposite corners)
  const OfxPointD e[2] = { pt(40., 10.), pt(0., 30.) }; // centre (20, 20), radii 20, 10
  s = makeState(0.5, 0.25);                             // 40 and 40 screen pixels
  gCalls.clear();
  drawPrimitive(gl, s, kOfxDrawPrimitiveEllipse, e, 2);
  b = blocks();
  CHECK(b.size() == 1);
  if (b.size() == 1) {
    CHECK((int)b[0].vertices.size() == ellipseSegmentCount(40., 40.));
    CHECK((int)b[0].vertices.size() == 29); // ceil(pi / acos(1 - 0.25 / 40))
    bool onEllipse = true;
    for (size_t k = 0; k < b[0].vertices.size(); ++k) {
      const double dx = (b[0].vertices[k].x - 20.) / 20., dy = (b[0].vertices[k].y - 20.) / 10.;
      onEllipse = onEllipse && near(dx * dx + dy * dy, 1., 1e-9);
    }
    CHECK(onEllipse);
    CHECK(near(b[0].vertices[0].x, 40.) && near(b[0].vertices[0].y, 20.));
  }
}

static void testEllipseSegmentCount()
{
  const double nan = std::sqrt(-1.), inf = 1e308 * 10.;
  struct Case { double rx, ry; int n; };
  const Case cases[] = {
    { 0., 0., 8 }, { 2., 1., 8 }, { nan, 1., 8 }, { 1., nan, 8 }, { inf, 1., 8 }, { 10., 0., 15 },
    { 50., 0., 32 }, { 100., 10., 45 }, { 10., -100., 45 }, { 1000., 0., 141 }, { 1e4, 0., 445 },
    { 1e5, 0., 1405 }, { 1e6, 0., 4096 }, { 1e9, 1e9, 4096 }, { 0.25, 0., 8 }, { -inf, 0., 8 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const int n = ellipseSegmentCount(cases[i].rx, cases[i].ry);
    if (n != cases[i].n) {
      std::printf("  ellipseSegmentCount(%g, %g) = %d, expected %d\n", cases[i].rx, cases[i].ry, n, cases[i].n);
    }
    CHECK(n == cases[i].n);
  }
  // monotonic, and within 1/4 pixel: r (1 - cos(pi / n)) <= 0.25
  int last = 0;
  bool ok = true;
  for (double r = 0.5; r < 2e6; r *= 1.1) {
    const int n = ellipseSegmentCount(r, 0.);
    ok = ok && n >= last && n >= 8 && n <= 4096;
    if (n < 4096) {
      ok = ok && r * (1. - std::cos(3.14159265358979323846 / n)) <= 0.25 + 1e-12;
    }
    last = n;
  }
  CHECK(ok);
}

static void testLineState()
{
  const GLFunctions gl = fakeGL();
  const OfxPointD p[2] = { pt(0., 0.), pt(1., 1.) };
  struct Case { float width; double ratio; bool smooth; double glWidth; };
  const Case cases[] = {
    { 0.f, 1., false, 1. }, { 0.f, 2., false, 2. }, { 0.f, 1.4, false, 1. }, { 0.f, 1.5, false, 2. },
    { 0.f, 0.3, false, 1. }, { 0.f, 100., false, 64. }, { 1.5f, 2., true, 3. }, { 1.f, 1., true, 1. },
    { 0.5f, 1., true, 0.5 }, { 1e20f, 64., true, 1e4 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const Case &c = cases[i];
    State s = makeState(1., 1., c.ratio);
    CHECK(setLineWidth(s, c.width) == kOfxStatOK);
    gCalls.clear();
    CHECK(drawPrimitive(gl, s, kOfxDrawPrimitiveLines, p, 2) == kOfxStatOK);
    const Call *w = find(eLineWidth);
    CHECK(w && near(w->a[0], c.glWidth, 1e-3));
    CHECK(count(eEnable, 0x0B20) == (c.smooth ? 1 : 0));  // GL_LINE_SMOOTH
    CHECK(count(eDisable, 0x0B20) == (c.smooth ? 0 : 1));
    const Call *h = find(eHint);
    CHECK( c.smooth ? (h && h->a[0] == 0x0C52 && h->a[1] == 0x1100) : !h ); // GL_LINE_SMOOTH_HINT, GL_DONT_CARE
    CHECK(count(eDisable, 0x0B24) == 1 && count(eLineStipple) == 0); // solid: no stipple
  }
  // stipple
  struct SCase { OfxDrawLineStipplePattern pattern; double ratio; int factor; unsigned bits; };
  const SCase scases[] = {
    { kOfxDrawLineStipplePatternDot, 1., 1, 0x3333 }, { kOfxDrawLineStipplePatternDash, 2., 2, 0x00FF },
    { kOfxDrawLineStipplePatternAltDash, 1.4, 1, 0xFF00 }, { kOfxDrawLineStipplePatternDotDash, 64., 64, 0x1FE3 },
    { kOfxDrawLineStipplePatternDot, 0.3, 1, 0x3333 }, { kOfxDrawLineStipplePatternDash, 2.5, 3, 0x00FF },
  };
  for (size_t i = 0; i < sizeof(scases) / sizeof(scases[0]); ++i) {
    const SCase &c = scases[i];
    State s = makeState(1., 1., c.ratio);
    CHECK(setLineStipple(s, c.pattern) == kOfxStatOK);
    const OfxPointD e[2] = { pt(0., 0.), pt(10., 10.) };
    for (int prim = 0; prim < 2; ++prim) { // the lines and the ellipse
      gCalls.clear();
      CHECK(drawPrimitive(gl, s, prim ? kOfxDrawPrimitiveEllipse : kOfxDrawPrimitiveLineStrip, e, 2) == kOfxStatOK);
      const Call *st = find(eLineStipple);
      CHECK(st && st->a[0] == c.factor && (unsigned)st->a[1] == c.bits);
      CHECK(count(eEnable, 0x0B24) == 1); // GL_LINE_STIPPLE
    }
    // the filled primitives have no line state
    gCalls.clear();
    CHECK(drawPrimitive(gl, s, kOfxDrawPrimitiveRectangle, e, 2) == kOfxStatOK);
    CHECK(count(eLineStipple) == 0 && count(eLineWidth) == 0);
  }
}

static void testAlignText()
{
  TextMetrics m;
  m.width = 23.;
  m.ascent = 7.;
  m.descent = 1.;
  const OfxPointD ps = pt(2., 0.5);
  const OfxPointD pos = pt(100., 50.);
  // x offset in screen pixels, by (alignment & 3): none, Left, Right, Left|Right
  const double dx[4] = { 0., 0., -23., -11.5 };
  // y offset in screen pixels, by Top 1, Bottom 2, Baseline 4
  const double dy[8] = {
    0.,  // none: baseline
    -7., // Top
    1.,  // Bottom
    -3., // Top|Bottom: centred, -(7 - 1) / 2
    0.,  // Baseline
    -3., // Top|Baseline (CenterV)
    1.,  // Bottom|Baseline: Bottom
    -3., // Top|Bottom|Baseline: centred
  };
  for (int a = 0; a < 32; ++a) {
    const OfxPointD r = alignText(pos, a, m, ps);
    const int h = a & 3;
    const int v = ( (a & kOfxDrawTextAlignmentTop) ? 1 : 0 ) | ( (a & kOfxDrawTextAlignmentBottom) ? 2 : 0 ) |
                  ( (a & kOfxDrawTextAlignmentBaseline) ? 4 : 0 );
    const bool ok = near(r.x, 100. + dx[h] * ps.x) && near(r.y, 50. + dy[v] * ps.y);
    if (!ok) {
      std::printf("  alignText(alignment 0x%x) = (%g, %g)\n", a, r.x, r.y);
    }
    CHECK(ok);
  }
  CHECK(kOfxDrawTextAlignmentCenterH == 3 && kOfxDrawTextAlignmentCenterV == (kOfxDrawTextAlignmentTop | kOfxDrawTextAlignmentBaseline));
}

static void testText()
{
  TextMetrics m = builtinTextMetrics("A");
  CHECK(m.width == 5. && m.ascent == 7. && m.descent == 1.);
  CHECK(builtinTextMetrics("AB").width == 11.);
  CHECK(builtinTextMetrics("").width == 0.);
  CHECK(builtinTextMetrics(NULL).width == 0.);
  CHECK(builtinTextMetrics("\t\n\x01").width == 0.);
  CHECK(builtinTextMetrics("\xc3\xa9").width == 5.);            // "é": one '?'
  CHECK(builtinTextMetrics("a\xe2\x82\xac" "b").width == 17.);  // "a€b": 3 glyphs
  CHECK(builtinTextMetrics("A\tB").width == 11.);

  const GLFunctions gl = fakeGL();
  State s = makeState(2., 3.);
  const OfxRGBAColourF col = { 1.f, 0.5f, 0.f, 1.f };
  setColour(s, &col);
  const OfxPointD pos = pt(10., 20.);
  // "A" (and '?') is 8 quads: one per vertical run of pixels in each column
  const char *const texts[] = { "A", "\xc3\xa9", "?", "\001A\n" };
  for (int t = 0; t < 4; ++t) {
    gCalls.clear();
    CHECK(drawText(gl, s, texts[t], &pos, 0) == kOfxStatOK);
    checkSavedState();
    const std::vector<Block> b = blocks();
    CHECK(b.size() == 1);
    if (b.size() == 1) {
      CHECK(b[0].mode == 0x0007); // GL_QUADS
      CHECK(b[0].vertices.size() == 8 * 4);
      CHECK(b[0].colour[0] == 1. && b[0].colour[1] == 0.5 && b[0].colour[2] == 0.);
    }
    CHECK(count(eLineWidth) == 0);
  }
  // "A" at (10, 20), pixel scale (2, 3), baseline-left: x in [10, 20], y in [20, 41]
  gCalls.clear();
  drawText(gl, s, "A", &pos, kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline);
  std::vector<Block> b = blocks();
  if (b.size() == 1 && !b[0].vertices.empty()) {
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (size_t k = 0; k < b[0].vertices.size(); ++k) {
      x0 = std::min(x0, b[0].vertices[k].x); x1 = std::max(x1, b[0].vertices[k].x);
      y0 = std::min(y0, b[0].vertices[k].y); y1 = std::max(y1, b[0].vertices[k].y);
    }
    CHECK(x0 == 10. && x1 == 20. && y0 == 20. && y1 == 41.);
  }
  // two glyphs: 6 pixels apart
  gCalls.clear();
  drawText(gl, s, "AA", &pos, 0);
  b = blocks();
  CHECK(b.size() == 1 && b[0].vertices.size() == 16 * 4);
  // drawBuiltinText: only glBegin/glVertex2d/glEnd
  gCalls.clear();
  drawBuiltinText(gl, "A", pos, s.pixelScale);
  CHECK(count(eBegin) == 1 && count(eEnd) == 1 && count(eVertex2d) == 32 && (int)gCalls.size() == 34);
  gCalls.clear();
  drawBuiltinText(gl, "\n", pos, s.pixelScale);
  CHECK(gCalls.empty());
}

static void testShadows()
{
  const GLFunctions gl = fakeGL();
  State s = makeState(2., 3., 1., true);
  const OfxRGBAColourF col = { 0.2f, 0.4f, 0.6f, 0.5f };
  setColour(s, &col);
  CHECK(shadowOffset(s).x == 2. && shadowOffset(s).y == -3.); // one device pixel at ratio 1
  {
    // ratio 2: one device pixel is half a screen pixel
    State s2 = makeState(2., 3., 2., true);
    CHECK(shadowOffset(s2).x == 1. && shadowOffset(s2).y == -1.5);
    const OfxPointD l[2] = { pt(0., 0.), pt(10., 5.) };
    gCalls.clear();
    CHECK(drawPrimitive(gl, s2, kOfxDrawPrimitiveLines, l, 2) == kOfxStatOK);
    const std::vector<Block> b2 = blocks();
    CHECK(b2.size() == 2 && b2[0].vertices.size() == 2 && b2[0].vertices[1].x == 11. && b2[0].vertices[1].y == 3.5);
    gCalls.clear();
    const OfxPointD tp = pt(10., 20.);
    CHECK(drawText(gl, s2, "A", &tp, 0) == kOfxStatOK);
    const std::vector<Block> b3 = blocks();
    CHECK(b3.size() == 2 && !b3[0].vertices.empty() &&
          near(b3[0].vertices[0].x, b3[1].vertices[0].x + 1.) && near(b3[0].vertices[0].y, b3[1].vertices[0].y - 1.5));
    // a ratio over 64 is sanitised to 64
    State s3 = makeState(64., 64., 1000., true);
    CHECK(shadowOffset(s3).x == 1. && shadowOffset(s3).y == -1.);
  }
  const OfxPointD p[4] = { pt(0., 0.), pt(10., 5.), pt(3., 8.), pt(-1., 2.) };
  const OfxDrawPrimitive prims[] = { kOfxDrawPrimitiveLines, kOfxDrawPrimitiveLineStrip, kOfxDrawPrimitiveLineLoop,
                                     kOfxDrawPrimitivePolygon, kOfxDrawPrimitiveRectangle, kOfxDrawPrimitiveEllipse };
  const int counts[] = { 4, 3, 4, 4, 2, 2 };
  for (int i = 0; i < 6; ++i) {
    gCalls.clear();
    CHECK(drawPrimitive(gl, s, prims[i], p, counts[i]) == kOfxStatOK);
    checkSavedState(); // one save for both passes
    const std::vector<Block> b = blocks();
    const bool filled = prims[i] == kOfxDrawPrimitivePolygon || prims[i] == kOfxDrawPrimitiveRectangle;
    CHECK(hasShadow(prims[i]) == !filled);
    if (filled) {
      // no shadow under the filled primitives: one pass, in the colour
      CHECK(b.size() == 1 && count(eColor4f) == 1);
      CHECK(b.size() == 1 && near(b[0].colour[0], 0.2) && b[0].colour[3] == 0.5);
      continue;
    }
    CHECK(b.size() == 2);
    if (b.size() != 2) {
      continue;
    }
    CHECK(b[0].mode == b[1].mode && b[0].vertices.size() == b[1].vertices.size());
    // the shadow first, in the background colour with the colour's alpha
    CHECK(b[0].colour[0] == 0. && b[0].colour[1] == 0. && b[0].colour[2] == 0. && b[0].colour[3] == 0.5);
    CHECK(near(b[1].colour[0], 0.2) && near(b[1].colour[1], 0.4) && near(b[1].colour[2], 0.6) && b[1].colour[3] == 0.5);
    bool offset = true;
    for (size_t k = 0; k < b[0].vertices.size() && k < b[1].vertices.size(); ++k) {
      offset = offset && near(b[0].vertices[k].x, b[1].vertices[k].x + 2.) && near(b[0].vertices[k].y, b[1].vertices[k].y - 3.);
    }
    CHECK(offset);
    CHECK(count(eLineWidth) == ( (prims[i] == kOfxDrawPrimitivePolygon || prims[i] == kOfxDrawPrimitiveRectangle) ? 0 : 1 ));
  }
  // text
  gCalls.clear();
  const OfxPointD pos = pt(10., 20.);
  CHECK(drawText(gl, s, "AB", &pos, kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV) == kOfxStatOK);
  checkSavedState();
  std::vector<Block> b = blocks();
  CHECK(b.size() == 2);
  if (b.size() == 2) {
    CHECK(b[0].mode == 0x0007 && b[1].mode == 0x0007 && b[0].vertices.size() == b[1].vertices.size());
    CHECK(b[0].colour[0] == 0. && b[0].colour[3] == 0.5 && near(b[1].colour[2], 0.6));
    bool offset = true;
    for (size_t k = 0; k < b[0].vertices.size() && k < b[1].vertices.size(); ++k) {
      offset = offset && near(b[0].vertices[k].x, b[1].vertices[k].x + 2.) && near(b[0].vertices[k].y, b[1].vertices[k].y - 3.);
    }
    CHECK(offset);
  }
  // off: one pass
  s.shadows = false;
  gCalls.clear();
  drawText(gl, s, "AB", &pos, 0);
  CHECK(blocks().size() == 1 && count(eColor4f) == 1);
  gCalls.clear();
  drawPrimitive(gl, s, kOfxDrawPrimitiveLines, p, 2);
  CHECK(blocks().size() == 1 && count(eColor4f) == 1);
  // errors: no call, shadows or not
  s.shadows = true;
  gCalls.clear();
  CHECK(drawPrimitive(gl, s, kOfxDrawPrimitiveLines, p, 3) == kOfxStatErrValue);
  CHECK(drawText(gl, s, "\n", &pos, 0) == kOfxStatOK);
  CHECK(gCalls.empty());
}

////////////////////////////////////////////////////////////////////////////////
// HostSupport's OpenGLDrawSuite

// a host with its own font (measureText() true) and colours, which records the hook calls
class HostDrawSuite : public OFX::Host::Interact::OpenGLDrawSuite {
public:
  struct TextCall {
    OfxPointD baselineLeft;
    TextMetrics metrics;
    OfxRGBAColourF colour;
    bool shadows;
  };
  struct PrimitiveCall {
    OfxDrawPrimitive primitive;
    std::vector<OfxPointD> points;
    OfxRGBAColourF colour;
    bool shadows;
  };

  explicit HostDrawSuite(const GLFunctions *gl) : OpenGLDrawSuite(gl), hostFont(true), recordPrimitives(false), replyDefault(false) {}

  bool hostFont;
  bool recordPrimitives; // record renderPrimitive() instead of drawing with the core
  bool replyDefault; // renderText() returns kOfxStatReplyDefault (can't render text)
  std::vector<TextCall> texts;
  std::vector<PrimitiveCall> primitives;

  // the host's background colour: dark blue
  virtual OfxStatus getColour(OfxStandardColour c, OfxRGBAColourF *colour) const
  {
    if (c == kOfxStandardColourOverlayBackground && colour) {
      colour->r = 0.f; colour->g = 0.f; colour->b = 0.25f; colour->a = 1.f;
      return kOfxStatOK;
    }
    return OpenGLDrawSuite::getColour(c, colour);
  }

protected:
  virtual bool measureText(const char *utf8, TextMetrics &m)
  {
    if (!hostFont) {
      return false;
    }
    m.width = 10. * (double)std::strlen(utf8); // 10 screen pixels per byte
    m.ascent = 12.;
    m.descent = 4.;
    return true;
  }

  virtual OfxStatus renderText(const char *utf8, const OfxPointD &baselineLeft, const TextMetrics &m)
  {
    TextCall c;
    c.baselineLeft = baselineLeft;
    c.metrics = m;
    c.colour = state().colour;
    c.shadows = state().shadows;
    texts.push_back(c);
    if (replyDefault) {
      return kOfxStatReplyDefault;
    }
    return OpenGLDrawSuite::renderText(utf8, baselineLeft, m); // the built-in font there
  }

  virtual OfxStatus renderPrimitive(OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount)
  {
    if (!recordPrimitives) {
      return OpenGLDrawSuite::renderPrimitive(primitive, points, pointCount);
    }
    PrimitiveCall c;
    c.primitive = primitive;
    c.points.assign(points, points + pointCount);
    c.colour = state().colour;
    c.shadows = state().shadows;
    primitives.push_back(c);
    return kOfxStatOK;
  }
};

// a host that overrides the shadow colour (black, not its grey background) and the offset
// (one screen pixel instead of the default one device pixel)
class ScreenPixelShadowDrawSuite : public HostDrawSuite {
public:
  explicit ScreenPixelShadowDrawSuite(const GLFunctions *gl) : HostDrawSuite(gl) {}
protected:
  virtual OfxRGBAColourF shadowColour() const
  {
    OfxRGBAColourF c = { 0.f, 0.f, 0.f, state().colour.a };
    return c;
  }
  virtual OfxPointD shadowOffset() const
  {
    OfxPointD o;
    o.x = state().pixelScale.x;
    o.y = -state().pixelScale.y;
    return o;
  }
};

static void testHostHelper()
{
  using OFX::Host::Interact::OpenGLDrawSuite;
  const GLFunctions gl = fakeGL();
  const OfxPointD pos = pt(100., 50.);
  const OfxPointD line[2] = { pt(0., 0.), pt(10., 5.) };

  // beginDraw resets the state, and sanitises it
  {
    OpenGLDrawSuite ds(&gl);
    CHECK(ds.glFunctions() == &gl);
    const OfxRGBAColourF red = { 1.f, 0.f, 0.f, 0.5f };
    CHECK(ds.setColour(&red) == kOfxStatOK && ds.setLineWidth(3.f) == kOfxStatOK &&
          ds.setLineStipple(kOfxDrawLineStipplePatternDash) == kOfxStatOK);
    CHECK(ds.state().colour.r == 1.f && ds.state().colour.g == 0.f && ds.state().lineWidth == 3.f &&
          ds.state().stipple == kOfxDrawLineStipplePatternDash);
    ds.beginDraw(pt(-2., 0.), 100., true);
    CHECK(ds.state().colour.r == 1.f && ds.state().colour.g == 1.f && ds.state().colour.a == 1.f);
    CHECK(ds.state().lineWidth == 0.f && ds.state().stipple == kOfxDrawLineStipplePatternSolid);
    CHECK(ds.state().pixelScale.x == 2. && ds.state().pixelScale.y == 1. && ds.state().screenPixelRatio == 64.);
    CHECK(ds.state().shadows);
    ds.beginDraw(pt(1., 1.), 2., false);
    CHECK(!ds.state().shadows && ds.state().screenPixelRatio == 2.);
    // statuses
    CHECK(ds.setColour(NULL) == kOfxStatErrValue);
    CHECK(ds.setLineWidth(-1.f) == kOfxStatFailed);
    CHECK(ds.setLineStipple((OfxDrawLineStipplePattern)9) == kOfxStatErrValue);
    OfxRGBAColourF c;
    CHECK(ds.getColour(kOfxStandardColourOverlaySelected, &c) == kOfxStatOK && c.r == 1.f && c.g == 0.f);
    CHECK(ds.getColour((OfxStandardColour)7, &c) == kOfxStatErrValue);
    gCalls.clear();
    CHECK(ds.drawPrimitive(kOfxDrawPrimitiveLines, line, 1) == kOfxStatErrValue);
    CHECK(ds.drawPrimitive((OfxDrawPrimitive)17, line, 2) == kOfxStatErrValue);
    CHECK(ds.drawText(NULL, &pos, 0) == kOfxStatErrValue);
    CHECK(ds.drawText("A", NULL, 0) == kOfxStatErrValue);
    CHECK(ds.drawText("", &pos, 0) == kOfxStatOK);
    CHECK(gCalls.empty());
    // the core draws the primitives, the built-in font the text (measureText() is false)
    CHECK(ds.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatOK);
    checkSavedState();
    CHECK(blocks().size() == 1);
    gCalls.clear();
    CHECK(ds.drawText("A", &pos, kOfxDrawTextAlignmentRight) == kOfxStatOK);
    checkSavedState();
    const std::vector<Block> b = blocks();
    CHECK(b.size() == 1 && b[0].mode == 0x0007 && b[0].vertices.size() == 32);
    if (b.size() == 1 && !b[0].vertices.empty()) {
      double x0 = 1e9;
      for (size_t k = 0; k < b[0].vertices.size(); ++k) {
        x0 = std::min(x0, b[0].vertices[k].x);
      }
      CHECK(x0 == 95.); // right-aligned: 5 pixels wide
    }
  }

  // no table: kOfxStatFailed, after the argument checks
  {
    OpenGLDrawSuite ds;
    CHECK(ds.glFunctions() == NULL);
    gCalls.clear();
    CHECK(ds.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatFailed);
    CHECK(ds.drawPrimitive(kOfxDrawPrimitiveLines, line, 3) == kOfxStatErrValue);
    CHECK(ds.drawText("A", &pos, 0) == kOfxStatFailed);
    CHECK(ds.drawText("", &pos, 0) == kOfxStatOK);
    GLFunctions partial = gl;
    partial.End = NULL;
    ds.setGLFunctions(&partial);
    CHECK(ds.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatFailed);
    CHECK(ds.drawText("A", &pos, 0) == kOfxStatFailed);
    CHECK(gCalls.empty());
    HostDrawSuite host(NULL);
    CHECK(host.drawText("A", &pos, 0) == kOfxStatFailed); // the default renderText needs a table
    CHECK(host.texts.size() == 1);
  }

  // host text: renderText() gets the left end of the baseline and the metrics
  {
    HostDrawSuite host(&gl);
    host.beginDraw(pt(2., 0.5), 1., false);
    const OfxRGBAColourF col = { 0.2f, 0.4f, 0.6f, 0.8f };
    host.setColour(&col);
    struct Case { int alignment; double x, y; };
    // "AB": width 20, ascent 12, descent 4 screen pixels; pixel scale (2, 0.5)
    const Case cases[] = {
      { 0, 100., 50. },
      { kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline, 100., 50. },
      { kOfxDrawTextAlignmentRight, 60., 50. },
      { kOfxDrawTextAlignmentCenterH, 80., 50. },
      { kOfxDrawTextAlignmentTop, 100., 44. },
      { kOfxDrawTextAlignmentBottom, 100., 52. },
      { kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV, 80., 48. },
      { kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentTop | kOfxDrawTextAlignmentBottom, 60., 48. },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      host.texts.clear();
      gCalls.clear();
      CHECK(host.drawText("AB", &pos, cases[i].alignment) == kOfxStatOK);
      CHECK(host.texts.size() == 1);
      if (host.texts.size() == 1) {
        const HostDrawSuite::TextCall &t = host.texts[0];
        const bool ok = near(t.baselineLeft.x, cases[i].x) && near(t.baselineLeft.y, cases[i].y);
        if (!ok) {
          std::printf("  host text alignment 0x%x: baseline left (%g, %g)\n", cases[i].alignment, t.baselineLeft.x, t.baselineLeft.y);
        }
        CHECK(ok);
        CHECK(t.metrics.width == 20. && t.metrics.ascent == 12. && t.metrics.descent == 4.);
        CHECK(near(t.colour.r, 0.2) && near(t.colour.a, 0.8) && !t.shadows);
      }
      CHECK(blocks().size() == 1); // the default renderText: the built-in font
    }
    // measureText() false: the built-in font, no renderText()
    host.hostFont = false;
    host.texts.clear();
    gCalls.clear();
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK);
    CHECK(host.texts.empty() && blocks().size() == 1);
  }

  // shadows: two passes, the shadow first, offset one device pixel (+1, -1 at ratio 1), in the host's
  // background colour with the current alpha; the hooks see shadows false
  {
    HostDrawSuite host(&gl);
    host.beginDraw(pt(2., 3.), 1., true);
    const OfxRGBAColourF col = { 0.2f, 0.4f, 0.6f, 0.5f };
    host.setColour(&col);
    CHECK(host.drawText("AB", &pos, kOfxDrawTextAlignmentLeft) == kOfxStatOK);
    CHECK(host.texts.size() == 2);
    if (host.texts.size() == 2) {
      const HostDrawSuite::TextCall &s = host.texts[0], &t = host.texts[1];
      CHECK(near(s.baselineLeft.x, t.baselineLeft.x + 2.) && near(s.baselineLeft.y, t.baselineLeft.y - 3.));
      CHECK(s.colour.r == 0.f && s.colour.g == 0.f && s.colour.b == 0.25f && s.colour.a == 0.5f);
      CHECK(near(t.colour.r, 0.2) && near(t.colour.b, 0.6) && t.colour.a == 0.5f);
      CHECK(!s.shadows && !t.shadows);
      CHECK(t.baselineLeft.x == 100. && t.baselineLeft.y == 50.);
    }
    CHECK(host.state().shadows && near(host.state().colour.r, 0.2)); // restored
    // the built-in font with shadows: two GL_QUADS blocks
    host.hostFont = false;
    gCalls.clear();
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK);
    std::vector<Block> b = blocks();
    CHECK(b.size() == 2);
    if (b.size() == 2) {
      CHECK(b[0].vertices.size() == b[1].vertices.size() && b[0].colour[2] == 0.25 && b[0].colour[3] == 0.5);
      CHECK(near(b[0].vertices[0].x, b[1].vertices[0].x + 2.) && near(b[0].vertices[0].y, b[1].vertices[0].y - 3.));
    }
    // primitives, with the core: two blocks, the shadow in the host's colour
    const OfxPointD e[2] = { pt(0., 0.), pt(40., 20.) };
    gCalls.clear();
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveEllipse, e, 2) == kOfxStatOK);
    b = blocks();
    CHECK(b.size() == 2);
    if (b.size() == 2) {
      CHECK(b[0].vertices.size() == b[1].vertices.size());
      CHECK((int)b[1].vertices.size() == ellipseSegmentCount(20. / 2., 10. / 3.));
      CHECK(b[0].colour[0] == 0. && b[0].colour[2] == 0.25 && b[0].colour[3] == 0.5);
      CHECK(near(b[1].colour[0], 0.2) && b[1].colour[3] == 0.5);
      CHECK(near(b[0].vertices[3].x, b[1].vertices[3].x + 2.) && near(b[0].vertices[3].y, b[1].vertices[3].y - 3.));
    }
    // primitives, with the host's renderer
    host.recordPrimitives = true;
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatOK);
    CHECK(host.primitives.size() == 2);
    if (host.primitives.size() == 2) {
      const HostDrawSuite::PrimitiveCall &s = host.primitives[0], &p = host.primitives[1];
      CHECK(s.points.size() == 2 && p.points.size() == 2);
      CHECK(s.points[1].x == 12. && s.points[1].y == 2. && p.points[1].x == 10. && p.points[1].y == 5.);
      CHECK(s.colour.b == 0.25f && s.colour.a == 0.5f && near(p.colour.r, 0.2) && !s.shadows && !p.shadows);
    }
    // the filled primitives: no shadow, one pass
    host.primitives.clear();
    const OfxPointD tri[3] = { pt(0., 0.), pt(10., 0.), pt(0., 10.) };
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveRectangle, line, 2) == kOfxStatOK);
    CHECK(host.drawPrimitive(kOfxDrawPrimitivePolygon, tri, 3) == kOfxStatOK);
    CHECK(host.primitives.size() == 2);
    if (host.primitives.size() == 2) {
      CHECK(host.primitives[0].points[1].x == 10. && near(host.primitives[0].colour.r, 0.2) && host.primitives[0].colour.a == 0.5);
      CHECK(host.primitives[1].points[1].x == 10. && near(host.primitives[1].colour.r, 0.2));
    }
    host.recordPrimitives = false;
    gCalls.clear();
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveRectangle, line, 2) == kOfxStatOK);
    CHECK(blocks().size() == 1 && count(eColor4f) == 1);
    // renderText() returns kOfxStatReplyDefault (the view can't render text): the built-in
    // font, for both passes
    host.hostFont = true;
    host.replyDefault = true;
    host.texts.clear();
    gCalls.clear();
    CHECK(host.drawText("AB", &pos, kOfxDrawTextAlignmentRight) == kOfxStatOK);
    CHECK(host.texts.size() == 1); // only the shadow pass asked the host
    b = blocks();
    CHECK(b.size() == 2);
    if (b.size() == 2) {
      CHECK(!b[0].vertices.empty() && b[0].vertices.size() == b[1].vertices.size() && b[0].mode == 0x0007);
      CHECK(b[0].colour[2] == 0.25 && near(b[1].colour[0], 0.2));
      CHECK(near(b[0].vertices[0].x, b[1].vertices[0].x + 2.) && near(b[0].vertices[0].y, b[1].vertices[0].y - 3.));
      double x0 = 1e9;
      for (size_t k = 0; k < b[1].vertices.size(); ++k) {
        x0 = std::min(x0, b[1].vertices[k].x);
      }
      CHECK(x0 == 100. - 11. * 2.); // right-aligned with the built-in metrics (11 pixels)
    }
    host.replyDefault = false;
    // without shadows, a declined text: the built-in font
    host.beginDraw(pt(2., 3.), 1., false);
    host.setColour(&col);
    host.replyDefault = true;
    host.texts.clear();
    gCalls.clear();
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK);
    CHECK(host.texts.size() == 1 && blocks().size() == 1);
    host.replyDefault = false;
    host.beginDraw(pt(2., 3.), 1., true);
    host.setColour(&col);
    // off: one pass
    host.recordPrimitives = true;
    host.beginDraw(pt(2., 3.), 1., false);
    host.primitives.clear();
    host.texts.clear();
    host.hostFont = true;
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatOK && host.primitives.size() == 1);
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK && host.texts.size() == 1);
  }

  // the default offset at ratio 2: one device pixel, half a screen pixel
  {
    HostDrawSuite host(&gl);
    host.beginDraw(pt(2., 3.), 2., true);
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK);
    CHECK(host.texts.size() == 2);
    if (host.texts.size() == 2) {
      CHECK(near(host.texts[0].baselineLeft.x, host.texts[1].baselineLeft.x + 1.) &&
            near(host.texts[0].baselineLeft.y, host.texts[1].baselineLeft.y - 1.5));
    }
    host.recordPrimitives = true;
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatOK);
    CHECK(host.primitives.size() == 2 && host.primitives[0].points[1].x == 11. && host.primitives[0].points[1].y == 3.5);
  }

  // overridden shadowColour() and shadowOffset(): black, one screen pixel (ratio 2)
  {
    ScreenPixelShadowDrawSuite host(&gl);
    host.beginDraw(pt(2., 3.), 2., true);
    const OfxRGBAColourF col = { 0.2f, 0.4f, 0.6f, 0.5f };
    host.setColour(&col);
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK);
    CHECK(host.texts.size() == 2);
    if (host.texts.size() == 2) {
      const HostDrawSuite::TextCall &sh = host.texts[0], &t = host.texts[1];
      CHECK(near(sh.baselineLeft.x, t.baselineLeft.x + 2.) && near(sh.baselineLeft.y, t.baselineLeft.y - 3.));
      CHECK(sh.colour.r == 0.f && sh.colour.b == 0.f && sh.colour.a == 0.5f);
    }
    host.recordPrimitives = true;
    CHECK(host.drawPrimitive(kOfxDrawPrimitiveLines, line, 2) == kOfxStatOK);
    CHECK(host.primitives.size() == 2);
    if (host.primitives.size() == 2) {
      CHECK(host.primitives[0].points[1].x == 12. && host.primitives[0].points[1].y == 2.);
      CHECK(host.primitives[0].colour.b == 0.f && host.primitives[0].colour.a == 0.5f);
    }
    // the built-in font: the same overrides
    host.hostFont = false;
    gCalls.clear();
    CHECK(host.drawText("AB", &pos, 0) == kOfxStatOK);
    const std::vector<Block> b = blocks();
    CHECK(b.size() == 2);
    if (b.size() == 2) {
      CHECK(b[0].colour[2] == 0. && b[0].colour[3] == 0.5);
      CHECK(near(b[0].vertices[0].x, b[1].vertices[0].x + 2.) && near(b[0].vertices[0].y, b[1].vertices[0].y - 3.));
    }
  }
}

int main()
{
  testLoad();
  testResetState();
  testStateSetters();
  testStatuses();
  testPrimitives();
  testEllipseSegmentCount();
  testLineState();
  testAlignText();
  testText();
  testShadows();
  testHostHelper();
  if (gFailures) {
    std::printf("drawsuite-opengl-test: %d check(s) FAILED\n", gFailures);
    return 1;
  }
  std::printf("drawsuite-opengl-test: all checks passed\n");
  return 0;
}
