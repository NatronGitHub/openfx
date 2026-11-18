// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Self-test of OFX::DrawArgs::getDrawContext() and of the OpenGL backend of OFX::DrawContext
  (Support/Library/ofxsDrawContextOpenGL.cpp), macOS only: it draws into an offscreen CGL
  context (the software renderer, an FBO, no window) and reads the pixels back.

  - which backend getDrawContext() picks, for the overlay registrations (V1, V2, unknown) with and
    without a host draw context, with a fake property suite and a fake DrawSuite;
  - the OpenGL backend: primitives, "over" compositing, line width and stipple, text size and
    alignment, the error statuses, and that the OpenGL state is restored.

  Built with -DBUILD_EXAMPLE_PLUGINS=ON on macOS (target drawcontext-opengl-test). Prints one line
  per failed check, and exits with 0 if all passed.
*/

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ofxsSupportPrivate.h" // OFX::Private::DrawArgsAccess, gPropSuite, gDrawSuite

static int gFailures = 0;

#define CHECK(cond) \
  do { if (!(cond)) { ++gFailures; std::printf("FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

////////////////////////////////////////////////////////////////////////////////
// A fake property suite: the draw action's inArgs. Everything is 0 or missing, except these.

static void *gFakeDrawContextHandle = NULL; // kOfxInteractPropDrawContext
static double gFakeScreenPixelRatio = 1.;

static OfxStatus fakeSetPointer(OfxPropertySetHandle, const char *, int, void *) { return kOfxStatOK; }
static OfxStatus fakeSetString(OfxPropertySetHandle, const char *, int, const char *) { return kOfxStatOK; }
static OfxStatus fakeSetDouble(OfxPropertySetHandle, const char *, int, double) { return kOfxStatOK; }
static OfxStatus fakeSetInt(OfxPropertySetHandle, const char *, int, int) { return kOfxStatOK; }
static OfxStatus fakeSetPointerN(OfxPropertySetHandle, const char *, int, void *const *) { return kOfxStatOK; }
static OfxStatus fakeSetStringN(OfxPropertySetHandle, const char *, int, const char *const *) { return kOfxStatOK; }
static OfxStatus fakeSetDoubleN(OfxPropertySetHandle, const char *, int, const double *) { return kOfxStatOK; }
static OfxStatus fakeSetIntN(OfxPropertySetHandle, const char *, int, const int *) { return kOfxStatOK; }
static OfxStatus fakeGetPointer(OfxPropertySetHandle, const char *property, int, void **value)
{
  if (std::strcmp(property, kOfxInteractPropDrawContext) == 0) {
    *value = gFakeDrawContextHandle;
    return gFakeDrawContextHandle ? kOfxStatOK : kOfxStatErrUnknown;
  }
  return kOfxStatErrUnknown;
}
static OfxStatus fakeGetString(OfxPropertySetHandle, const char *, int, const char **value) { *value = ""; return kOfxStatErrUnknown; }
static OfxStatus fakeGetDouble(OfxPropertySetHandle, const char *property, int index, double *value)
{
  *value = 0.;
  if (std::strcmp(property, kOfxInteractPropPixelScale) == 0 || std::strcmp(property, kOfxImageEffectPropRenderScale) == 0) {
    *value = 1.;
  }
#ifdef kOfxInteractPropScreenPixelRatio
  if (std::strcmp(property, kOfxInteractPropScreenPixelRatio) == 0) {
    *value = gFakeScreenPixelRatio;
  }
#endif
  (void)index;
  return kOfxStatOK;
}
static OfxStatus fakeGetInt(OfxPropertySetHandle, const char *, int, int *value) { *value = 0; return kOfxStatErrUnknown; }
static OfxStatus fakeGetPointerN(OfxPropertySetHandle h, const char *p, int count, void **value)
{
  for (int i = 0; i < count; ++i) fakeGetPointer(h, p, i, &value[i]);
  return kOfxStatOK;
}
static OfxStatus fakeGetStringN(OfxPropertySetHandle, const char *, int, const char **) { return kOfxStatErrUnknown; }
static OfxStatus fakeGetDoubleN(OfxPropertySetHandle h, const char *p, int count, double *value)
{
  for (int i = 0; i < count; ++i) fakeGetDouble(h, p, i, &value[i]);
  return kOfxStatOK;
}
static OfxStatus fakeGetIntN(OfxPropertySetHandle, const char *, int count, int *value)
{
  for (int i = 0; i < count; ++i) value[i] = 0;
  return kOfxStatErrUnknown;
}
static OfxStatus fakeReset(OfxPropertySetHandle, const char *) { return kOfxStatOK; }
static OfxStatus fakeGetDimension(OfxPropertySetHandle, const char *, int *count) { *count = 0; return kOfxStatErrUnknown; }

static OfxPropertySuiteV1 gFakePropSuite = {
  fakeSetPointer, fakeSetString, fakeSetDouble, fakeSetInt,
  fakeSetPointerN, fakeSetStringN, fakeSetDoubleN, fakeSetIntN,
  fakeGetPointer, fakeGetString, fakeGetDouble, fakeGetInt,
  fakeGetPointerN, fakeGetStringN, fakeGetDoubleN, fakeGetIntN,
  fakeReset, fakeGetDimension
};

////////////////////////////////////////////////////////////////////////////////
// A fake DrawSuite, which counts its calls

static int gDrawSuiteCalls = 0;
static OfxStatus dsGetColour(OfxDrawContextHandle, OfxStandardColour, OfxRGBAColourF *) { ++gDrawSuiteCalls; return kOfxStatOK; }
static OfxStatus dsSetColour(OfxDrawContextHandle, const OfxRGBAColourF *) { ++gDrawSuiteCalls; return kOfxStatOK; }
static OfxStatus dsSetLineWidth(OfxDrawContextHandle, float) { ++gDrawSuiteCalls; return kOfxStatOK; }
static OfxStatus dsSetLineStipple(OfxDrawContextHandle, OfxDrawLineStipplePattern) { ++gDrawSuiteCalls; return kOfxStatOK; }
static OfxStatus dsDraw(OfxDrawContextHandle, OfxDrawPrimitive, const OfxPointD *, int) { ++gDrawSuiteCalls; return kOfxStatOK; }
static OfxStatus dsDrawText(OfxDrawContextHandle, const char *, const OfxPointD *, int) { ++gDrawSuiteCalls; return kOfxStatOK; }
static OfxDrawSuiteV1 gFakeDrawSuite = { dsGetColour, dsSetColour, dsSetLineWidth, dsSetLineStipple, dsDraw, dsDrawText };

////////////////////////////////////////////////////////////////////////////////
// Offscreen OpenGL: a W x H RGBA FBO, with a projection where canonical coordinates are pixels
// (pixel (i, j) covers [i, i+1] x [j, j+1])

static const int W = 64;
static const int H = 64;
static std::vector<float> gPixels;

static bool initGL()
{
  CGLPixelFormatAttribute attribs[] = {
    kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
    kCGLPFARendererID, (CGLPixelFormatAttribute)kCGLRendererGenericFloatID, // software, deterministic
    kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
    kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
    (CGLPixelFormatAttribute)0
  };
  CGLPixelFormatObj pix = NULL;
  GLint npix = 0;
  if (CGLChoosePixelFormat(attribs, &pix, &npix) != kCGLNoError || !pix) {
    std::printf("CGLChoosePixelFormat failed\n");
    return false;
  }
  CGLContextObj ctx = NULL;
  CGLError err = CGLCreateContext(pix, NULL, &ctx);
  CGLDestroyPixelFormat(pix);
  if (err != kCGLNoError || !ctx || CGLSetCurrentContext(ctx) != kCGLNoError) {
    std::printf("CGLCreateContext failed\n");
    return false;
  }
  GLuint fbo = 0, rb = 0;
  glGenFramebuffersEXT(1, &fbo);
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
  glGenRenderbuffersEXT(1, &rb);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, W, H);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
  if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
    std::printf("incomplete framebuffer\n");
    return false;
  }
  glViewport(0, 0, W, H);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, W, 0, H, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  return true;
}

static void clear()
{
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void readBack()
{
  glFinish();
  gPixels.assign(W * H * 4, 0.f);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, &gPixels[0]);
}

static const float *pixel(int x, int y) { return &gPixels[(y * W + x) * 4]; }

static bool near(float a, float b, float tol = 0.02f) { return std::fabs(a - b) <= tol; }

static bool isColour(int x, int y, float r, float g, float b, float tol = 0.02f)
{
  const float *p = pixel(x, y);
  return near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol);
}

static bool lit(int x, int y) { const float *p = pixel(x, y); return p[0] + p[1] + p[2] > 0.1f; }

// bounding box of the lit pixels
struct Box { int x0, y0, x1, y1; int count; };
static Box litBox()
{
  Box b = { W, H, -1, -1, 0 };
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      if (lit(x, y)) {
        b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y);
        b.x1 = std::max(b.x1, x); b.y1 = std::max(b.y1, y);
        ++b.count;
      }
    }
  }
  return b;
}

static OfxPointD pt(double x, double y) { OfxPointD p = { x, y }; return p; }

////////////////////////////////////////////////////////////////////////////////

static void testBackendSelection()
{
  using namespace OFX;
  using namespace OFX::Private;
  OfxPropertySetHandle inArgs = (OfxPropertySetHandle)&gFakePropSuite; // any non-NULL handle
  int dummyHandle = 0;

  // no host draw context
  gFakeDrawContextHandle = NULL;
  {
    DrawArgs args((PropertySet(inArgs)));
    CHECK(args.drawContext == NULL);
    // unknown registration (DrawArgs not made by the dispatcher): OpenGL
    CHECK(args.getDrawContext().usesOpenGL());
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(args.getDrawContext().usesOpenGL());
    // V2 without a draw context: nothing (never OpenGL)
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV2);
    CHECK(!args.getDrawContext().canDraw());
    CHECK(!args.getDrawContext().setColour(1.f, 1.f, 1.f));
  }
  // with a host draw context
  gFakeDrawContextHandle = &dummyHandle;
  {
    DrawArgs args((PropertySet(inArgs)));
    CHECK(args.drawContext != NULL && args.drawContext->usesDrawSuite()); // unchanged meaning
    CHECK(args.getDrawContext().usesDrawSuite());                         // unknown: DrawSuite
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV2);
    CHECK(args.getDrawContext().usesDrawSuite());
    gDrawSuiteCalls = 0;
    CHECK(args.getDrawContext().setColour(1.f, 0.f, 0.f));
    CHECK(gDrawSuiteCalls == 1);
    // V1: OpenGL, even with a draw context; drawContext still points to the DrawSuite context
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(args.getDrawContext().usesOpenGL());
    CHECK(args.drawContext != NULL && args.drawContext->usesDrawSuite());
    // copies keep it
    DrawArgs copy(args);
    CHECK(copy.drawContext == NULL || copy.drawContext->usesDrawSuite());
    CHECK(copy.drawContext != args.drawContext);
    CHECK(copy.getDrawContext().usesOpenGL());
  }
  gFakeDrawContextHandle = NULL;
  // a default DrawContext draws nothing
  DrawContext none;
  CHECK(!none.canDraw());
  CHECK(!none.draw(kOfxDrawPrimitiveLines, NULL, 0));
}

static void testOpenGL()
{
  using namespace OFX;
  using namespace OFX::Private;
  OfxPropertySetHandle inArgs = (OfxPropertySetHandle)&gFakePropSuite;
  gFakeDrawContextHandle = NULL;
  DrawArgs args((PropertySet(inArgs)));
  DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
  const DrawContext &dc = args.getDrawContext();
  CHECK(dc.usesOpenGL() && !dc.usesDrawSuite() && dc.getHandle() == NULL);

  // the state the backend must restore
  glColor4f(0.f, 1.f, 0.f, 1.f);
  glDisable(GL_BLEND);
  glLineWidth(1.f);

  // a filled rectangle from 2 corners (in any order), then "over" compositing
  clear();
  OfxPointD rect[2] = { pt(20, 20), pt(10, 10) };
  CHECK(dc.setColour(1.f, 0.f, 0.f, 1.f));
  CHECK(dc.draw(kOfxDrawPrimitiveRectangle, rect, 2));
  OfxPointD rect2[2] = { pt(12, 12), pt(18, 18) };
  CHECK(dc.setColour(0.f, 0.f, 1.f, 0.5f));
  CHECK(dc.draw(kOfxDrawPrimitiveRectangle, rect2, 2));
  readBack();
  CHECK(isColour(10, 10, 1, 0, 0));
  CHECK(isColour(19, 19, 1, 0, 0));
  CHECK(isColour(20, 20, 0, 0, 0));
  CHECK(isColour(9, 15, 0, 0, 0));
  CHECK(isColour(15, 15, 0.5f, 0, 0.5f, 0.03f));

  // a polygon (triangle)
  clear();
  OfxPointD tri[3] = { pt(0, 0), pt(40, 0), pt(0, 40) };
  CHECK(dc.setColour(1.f, 1.f, 1.f, 1.f));
  CHECK(dc.draw(kOfxDrawPrimitivePolygon, tri, 3));
  readBack();
  CHECK(isColour(5, 5, 1, 1, 1));
  CHECK(isColour(30, 30, 0, 0, 0));

  // a single-pixel line (width 0): exactly one row lit
  clear();
  OfxPointD line[2] = { pt(10, 30.5), pt(50, 30.5) };
  CHECK(dc.setLineWidth(0.f));
  CHECK(dc.draw(kOfxDrawPrimitiveLines, line, 2));
  readBack();
  {
    Box b = litBox();
    CHECK(b.y0 == 30 && b.y1 == 30);
    CHECK(b.x0 >= 9 && b.x0 <= 11 && b.x1 >= 48 && b.x1 <= 50);
  }
  // with the screen pixel ratio 2 of a Retina screen: 2 device pixels
  {
    gFakeScreenPixelRatio = 2.;
    DrawArgs args2((PropertySet(inArgs)));
    DrawArgsAccess::setOverlayRegistration(args2, eOverlayRegistrationV1);
    const DrawContext &dc2 = args2.getDrawContext();
    clear();
    OfxPointD line2[2] = { pt(10, 30), pt(50, 30) };
    CHECK(dc2.setColour(1.f, 1.f, 1.f));
    CHECK(dc2.draw(kOfxDrawPrimitiveLines, line2, 2));
    readBack();
    Box b = litBox();
#ifdef OFX_EXTENSIONS_NATRON
    CHECK(b.y1 - b.y0 + 1 == 2);
#else
    CHECK(b.y1 - b.y0 + 1 == 1);
#endif
    gFakeScreenPixelRatio = 1.;
  }
  // a smooth line of width 4: about 4 rows covered
  clear();
  CHECK(dc.setLineWidth(4.f));
  CHECK(dc.draw(kOfxDrawPrimitiveLineStrip, line, 2));
  readBack();
  {
    float coverage = 0.f;
    for (int y = 20; y < 40; ++y) coverage += pixel(30, y)[0];
    CHECK(coverage > 3.f && coverage < 5.f);
  }
  CHECK(dc.setLineWidth(0.f));

  // a dotted line: about half the pixels
  clear();
  OfxPointD hline[2] = { pt(0, 10.5), pt(64, 10.5) };
  CHECK(dc.setLineStipple(kOfxDrawLineStipplePatternDot));
  CHECK(dc.draw(kOfxDrawPrimitiveLines, hline, 2));
  readBack();
  {
    int n = 0;
    for (int x = 0; x < W; ++x) n += lit(x, 10) ? 1 : 0;
    CHECK(n >= 28 && n <= 36);
    CHECK(lit(0, 10) && lit(1, 10) && !lit(2, 10) && !lit(3, 10)); // 2 on, 2 off
  }
  CHECK(dc.setLineStipple(kOfxDrawLineStipplePatternSolid));

  // an ellipse: a line, not filled
  clear();
  OfxPointD ell[2] = { pt(10, 10), pt(50, 30) }; // centre (30, 20), radii 20 and 10
  CHECK(dc.draw(kOfxDrawPrimitiveEllipse, ell, 2));
  readBack();
  {
    Box b = litBox();
    CHECK(b.x0 >= 9 && b.x0 <= 11 && b.x1 >= 48 && b.x1 <= 50);
    CHECK(b.y0 >= 9 && b.y0 <= 11 && b.y1 >= 28 && b.y1 <= 30);
    CHECK(!lit(30, 20));
    CHECK(b.count < 200); // a 1-pixel outline, roughly the perimeter (~97 pixels)
  }

  // text: "HH" on the baseline at (5, 40): 11 x 7 pixels above the baseline
  clear();
  CHECK(dc.drawText("HH", pt(5, 40)));
  readBack();
  {
    Box b = litBox();
    CHECK(b.x0 == 5 && b.x1 == 15 && b.y0 == 40 && b.y1 == 46);
    CHECK(lit(5, 43) && lit(9, 43) && !lit(7, 46)); // the H's bar and stems
  }
  // right / top alignment
  clear();
  CHECK(dc.drawText("HH", pt(40, 40), kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentTop));
  readBack();
  {
    Box b = litBox();
    CHECK(b.x0 == 29 && b.x1 == 39 && b.y0 == 33 && b.y1 == 39);
  }
  // bottom: 1 pixel of descent below the baseline
  clear();
  CHECK(dc.drawText("HH", pt(5, 40), kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom));
  readBack();
  {
    Box b = litBox();
    CHECK(b.y0 == 41 && b.y1 == 47);
  }
  // centred (CenterH | CenterV)
  clear();
  CHECK(dc.drawText("HHH", pt(32, 32), kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV));
  readBack();
  {
    Box b = litBox();
    CHECK(b.x1 - b.x0 + 1 == 17 && b.y1 - b.y0 + 1 == 7);
    CHECK(std::fabs(0.5 * (b.x0 + b.x1 + 1) - 32.) <= 1. && std::fabs(0.5 * (b.y0 + b.y1 + 1) - 32.) <= 1.);
  }
  // non-ASCII: one '?' per UTF-8 character, control characters skipped
  clear();
  CHECK(dc.drawText("\xc3\xa9\n", pt(5, 40)));
  readBack();
  {
    Box b = litBox();
    CHECK(b.x0 == 5 && b.x1 == 9);
  }
  CHECK(dc.drawText("", pt(5, 40)));

  // the standard colours, and the errors (as the DrawSuite statuses)
  OfxRGBAColourF c;
  CHECK(dc.getColour(kOfxStandardColourOverlayText, c) && c.a == 1.f);
  CHECK(!dc.getColour((OfxStandardColour)42, c));
  CHECK(!dc.setLineStipple((OfxDrawLineStipplePattern)42));
  CHECK(!dc.setLineWidth(-1.f));
  CHECK(!dc.draw((OfxDrawPrimitive)42, rect, 2));
  CHECK(!dc.draw(kOfxDrawPrimitiveRectangle, rect, 1));
  CHECK(!dc.draw(kOfxDrawPrimitiveRectangle, tri, 3));
  CHECK(!dc.draw(kOfxDrawPrimitivePolygon, rect, 2));
  CHECK(!dc.draw(kOfxDrawPrimitiveLines, NULL, 2));
  CHECK(!dc.draw(kOfxDrawPrimitiveLines, line, 1));

  // the OpenGL state is unchanged
  GLfloat current[4];
  glGetFloatv(GL_CURRENT_COLOR, current);
  CHECK(current[0] == 0.f && current[1] == 1.f && current[2] == 0.f && current[3] == 1.f);
  CHECK(!glIsEnabled(GL_BLEND));
  CHECK(!glIsEnabled(GL_LINE_STIPPLE));
  CHECK(!glIsEnabled(GL_LINE_SMOOTH));
  GLfloat width = 0.f;
  glGetFloatv(GL_LINE_WIDTH, &width);
  CHECK(width == 1.f);
  GLint depth = 0;
  glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &depth);
  CHECK(depth == 0);
  CHECK(glGetError() == GL_NO_ERROR);
}

int main()
{
  OFX::Private::gPropSuite = &gFakePropSuite;
  OFX::Private::gDrawSuite = &gFakeDrawSuite;
  if (!OFX::Private::openGLDrawContextBackend()) {
    std::printf("built with OFXS_DRAWCONTEXT_NO_OPENGL: nothing to test\n");
    return 0;
  }
  testBackendSelection();
  if (!initGL()) {
    return 2;
  }
  testOpenGL();
  if (gFailures) {
    std::printf("%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
