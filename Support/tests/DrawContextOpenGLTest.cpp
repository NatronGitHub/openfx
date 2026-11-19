// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Self-test of OFX::DrawArgs::getDrawContext() and of the OpenGL backend of OFX::DrawContext
  (Support/Library/ofxsDrawContextOpenGL.cpp). On macOS it draws into an offscreen CGL context
  (the software renderer, an FBO, no window) and reads the pixels back. On the other systems
  (Linux and BSD CI) there is no OpenGL context: it checks that the OpenGL functions and the
  current-context functions load at run time (the test doesn't link OpenGL), that no context is
  current, and the backend selection without one.

  - which backend getDrawContext() picks, for the overlay registrations (V1, V2, unknown) with and
    without a host draw context, with a fake property suite and a fake DrawSuite;
  - the registry of the overlay registrations (setOverlayInteractV1/V2, isOverlayInteractV2);
  - ImageEffectDescriptor::setOverlayInteractDescriptorV2(): on a DrawSuite host, V2 only, or (with
    hasOpenGLDraw) the V2 entry and the distinct OpenGL V1 entry
    (InteractMainEntry<DESC>::overlayInteractMainEntryV1); V1 only on the other hosts and when the
    host refuses the V2 property;
  - the interact dispatcher, through these two entries: V2 draws use only the DrawSuite, V1 draws
    only OpenGL, with DrawArgs::drawContext == NULL even if the host passed a draw context;
  - without a current OpenGL context (the CGL context made non-current), no OpenGL and no
    DrawSuite in V1 draws: nothing is drawn;
  - the OpenGL backend: primitives, "over" compositing, line width and stipple, text size and
    alignment, the error statuses, and that the OpenGL state is restored.

  - DrawArgs::isOpenGLContextCurrent().

  Built with -DBUILD_EXAMPLE_PLUGINS=ON, except on Windows (target drawcontext-opengl-test). Prints
  one line per failed check, and exits with 0 if all passed.
*/

#if defined(__APPLE__) && !defined(OFXS_TEST_NO_GL_CONTEXT) // (define it to test as on Linux)
// an offscreen CGL context: the OpenGL drawing tests
#define OFXS_TEST_GL_CONTEXT 1
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ofxsSupportPrivate.h" // OFX::Private::DrawArgsAccess, gPropSuite, gDrawSuite
#include "ofxsImageEffect.h"
#include "ofxsInteract.h"

namespace OFX {
  extern ImageEffectHostDescription gHostDescription; // ofxsImageEffect.cpp
  extern bool gHostDescriptionHasInit;               // ofxsImageEffect.cpp
}

static int gFailures = 0;

#define CHECK(cond) \
  do { if (!(cond)) { ++gFailures; std::printf("FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

////////////////////////////////////////////////////////////////////////////////
// A fake property suite: the draw action's inArgs. Everything is 0 or missing, except these.
// The pointer and string properties that are set are stored (per property set handle), for
// the effect, descriptor and interact property sets.

static void *gFakeDrawContextHandle = NULL; // kOfxInteractPropDrawContext
static double gFakeScreenPixelRatio = 1.;
static bool gRefuseOverlayInteractV2 = false; // the host refuses kOfxImageEffectPluginPropOverlayInteractV2

typedef std::pair<OfxPropertySetHandle, std::string> PropKey;
static std::map<PropKey, void *> gPointerProps;
static std::map<PropKey, std::string> gStringProps;

static OfxStatus fakeSetPointer(OfxPropertySetHandle h, const char *property, int, void *value)
{
  if (gRefuseOverlayInteractV2 && std::strcmp(property, kOfxImageEffectPluginPropOverlayInteractV2) == 0) {
    return kOfxStatErrUnknown;
  }
  gPointerProps[PropKey(h, property)] = value;
  return kOfxStatOK;
}
static OfxStatus fakeSetString(OfxPropertySetHandle h, const char *property, int, const char *value)
{
  gStringProps[PropKey(h, property)] = value ? value : "";
  return kOfxStatOK;
}
static OfxStatus fakeSetDouble(OfxPropertySetHandle, const char *, int, double) { return kOfxStatOK; }
static OfxStatus fakeSetInt(OfxPropertySetHandle, const char *, int, int) { return kOfxStatOK; }
static OfxStatus fakeSetPointerN(OfxPropertySetHandle, const char *, int, void *const *) { return kOfxStatOK; }
static OfxStatus fakeSetStringN(OfxPropertySetHandle, const char *, int, const char *const *) { return kOfxStatOK; }
static OfxStatus fakeSetDoubleN(OfxPropertySetHandle, const char *, int, const double *) { return kOfxStatOK; }
static OfxStatus fakeSetIntN(OfxPropertySetHandle, const char *, int, const int *) { return kOfxStatOK; }
static OfxStatus fakeGetPointer(OfxPropertySetHandle h, const char *property, int, void **value)
{
  if (std::strcmp(property, kOfxInteractPropDrawContext) == 0) {
    *value = gFakeDrawContextHandle;
    return gFakeDrawContextHandle ? kOfxStatOK : kOfxStatErrUnknown;
  }
  std::map<PropKey, void *>::const_iterator it = gPointerProps.find(PropKey(h, property));
  if (it != gPointerProps.end()) {
    *value = it->second;
    return kOfxStatOK;
  }
  *value = NULL;
  return kOfxStatErrUnknown;
}
static OfxStatus fakeGetString(OfxPropertySetHandle h, const char *property, int, const char **value)
{
  std::map<PropKey, std::string>::const_iterator it = gStringProps.find(PropKey(h, property));
  if (it != gStringProps.end()) {
    *value = it->second.c_str();
    return kOfxStatOK;
  }
  *value = "";
  return kOfxStatErrUnknown;
}
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

// true while the test's OpenGL context is current (never on the systems without one)
static bool gGLCurrent = false;

// what getDrawContext() gives where it may use OpenGL (V1, unknown without a draw context):
// OpenGL if a context is current, else nothing (never the DrawSuite)
static bool isOpenGLOrNothing(const OFX::DrawContext &dc)
{
  if (gGLCurrent) {
    return dc.usesOpenGL();
  }
  return !dc.canDraw() && !dc.usesDrawSuite();
}

#ifdef OFXS_TEST_GL_CONTEXT
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
#endif // OFXS_TEST_GL_CONTEXT

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
    CHECK(isOpenGLOrNothing(args.getDrawContext()));
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(isOpenGLOrNothing(args.getDrawContext()));
    CHECK(args.isOpenGLContextCurrent() == gGLCurrent);
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
    CHECK(args.drawContext->getHandle() == (OfxDrawContextHandle)&dummyHandle);
    CHECK(args.getDrawContext().usesDrawSuite());                         // unknown: DrawSuite
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV2);
    CHECK(args.getDrawContext().usesDrawSuite());
    gDrawSuiteCalls = 0;
    CHECK(args.getDrawContext().setColour(1.f, 0.f, 0.f));
    CHECK(gDrawSuiteCalls == 1);
    // copies keep the draw context
    {
      DrawArgs copy(args);
      CHECK(copy.drawContext != NULL && copy.drawContext->usesDrawSuite());
      CHECK(copy.drawContext != args.drawContext);
      CHECK(copy.getDrawContext().usesDrawSuite());
    }
    // V1: only OpenGL, even with a draw context (never the DrawSuite)
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(isOpenGLOrNothing(args.getDrawContext()));
    // as the dispatcher makes V1 draw args: no draw context
    DrawArgsAccess::clearDrawSuiteContext(args);
    CHECK(args.drawContext == NULL);
    CHECK(isOpenGLOrNothing(args.getDrawContext()));
    DrawArgs copy(args);
    CHECK(copy.drawContext == NULL);
    CHECK(isOpenGLOrNothing(copy.getDrawContext()));
  }
  gFakeDrawContextHandle = NULL;
  // a default DrawContext draws nothing
  DrawContext none;
  CHECK(!none.canDraw());
  CHECK(!none.draw(kOfxDrawPrimitiveLines, NULL, 0));
}

////////////////////////////////////////////////////////////////////////////////
// A fake image effect suite and interact suite: the property set of an effect, descriptor or
// interact handle is the handle itself

static OfxStatus fakeEffectGetPropertySet(OfxImageEffectHandle h, OfxPropertySetHandle *props)
{
  *props = (OfxPropertySetHandle)h;
  return kOfxStatOK;
}
static OfxStatus fakeEffectGetParamSet(OfxImageEffectHandle, OfxParamSetHandle *paramSet)
{
  *paramSet = NULL; // no parameters (ParamSet(Descriptor)::setParamSetHandle(NULL) fetches nothing)
  return kOfxStatOK;
}
static OfxStatus fakeInteractGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle *props)
{
  *props = (OfxPropertySetHandle)h;
  return kOfxStatOK;
}
static OfxImageEffectSuiteV1 gFakeEffectSuite;
static OfxInteractSuiteV1 gFakeInteractSuite;

// what the last draw() saw
struct DrawRecord {
  bool drawn;
  bool hasDrawContext;      // DrawArgs::drawContext != NULL
  bool calledViaV2;         // DrawArgs::calledViaOverlayInteractV2()
  bool usesDrawSuite;       // getDrawContext().usesDrawSuite()
  bool usesOpenGL;          // getDrawContext().usesOpenGL()
  bool glCurrent;           // DrawArgs::isOpenGLContextCurrent()
};
static DrawRecord gDrawRecord;

class TestEffect : public OFX::ImageEffect {
public:
  explicit TestEffect(OfxImageEffectHandle h) : OFX::ImageEffect(h) {}
  virtual void render(const OFX::RenderArguments &) {}
};

class TestInteract : public OFX::OverlayInteract {
public:
  TestInteract(OfxInteractHandle h, OFX::ImageEffect *) : OFX::OverlayInteract(h) {}
  virtual bool draw(const OFX::DrawArgs &args)
  {
    gDrawRecord.drawn = true;
    gDrawRecord.hasDrawContext = (args.drawContext != NULL);
    gDrawRecord.calledViaV2 = args.calledViaOverlayInteractV2();
    gDrawRecord.usesDrawSuite = args.getDrawContext().usesDrawSuite();
    gDrawRecord.usesOpenGL = args.getDrawContext().usesOpenGL();
    gDrawRecord.glCurrent = args.isOpenGLContextCurrent();
    return true;
  }
};

class TestOverlayDescriptor : public OFX::DefaultEffectOverlayDescriptor<TestOverlayDescriptor, TestInteract> {
public:
  static OfxPluginEntryPoint *mainEntry() { return &OFX::InteractMainEntry<TestOverlayDescriptor>::overlayInteractMainEntry; }
  static OfxPluginEntryPoint *mainEntryV1() { return &OFX::InteractMainEntry<TestOverlayDescriptor>::overlayInteractMainEntryV1; }
};

// a descriptor class with its own main entry (not an InteractMainEntry<DESC> one)
static OfxStatus customOverlayEntry(const char *, const void *, OfxPropertySetHandle, OfxPropertySetHandle) { return kOfxStatReplyDefault; }
class CustomEntryOverlayDescriptor : public OFX::EffectOverlayDescriptor {
public:
  virtual OFX::Interact *createInstance(OfxInteractHandle, OFX::ImageEffect *) { return NULL; }
  virtual OfxPluginEntryPoint *getMainEntry() { return customOverlayEntry; }
};

static void *pointerProp(OfxPropertySetHandle h, const char *property)
{
  std::map<PropKey, void *>::const_iterator it = gPointerProps.find(PropKey(h, property));
  return it != gPointerProps.end() ? it->second : NULL;
}

// setOverlayInteractDescriptorV2(): which entries are registered
static void testSetOverlayInteractDescriptorV2()
{
  using namespace OFX;
  using namespace OFX::Private;
  static int descHandles[6];
  const bool savedOverlays = gHostDescription.supportsOverlays;
  const bool savedDrawSuite = gHostDescription.supportsDrawSuite;
  gHostDescription.supportsOverlays = true;
  clearOverlayInteractRegistrations();

  // the two entry points of a descriptor class are distinct, and the constructor records the pair
  CHECK(TestOverlayDescriptor::mainEntry() != TestOverlayDescriptor::mainEntryV1());
  {
    TestOverlayDescriptor d;
    CHECK(d.getMainEntry() == TestOverlayDescriptor::mainEntry());
    CHECK(getOverlayInteractV1Sibling(TestOverlayDescriptor::mainEntry()) == TestOverlayDescriptor::mainEntryV1());
  }
  CHECK(getOverlayInteractV1Sibling(customOverlayEntry) == NULL);
  CHECK(getOverlayInteractV1Sibling(NULL) == NULL);

  // DrawSuite host, DrawSuite drawing only: V2 = the main entry, no V1 entry
  gHostDescription.supportsDrawSuite = true;
  {
    OfxPropertySetHandle props = (OfxPropertySetHandle)&descHandles[0];
    ImageEffectDescriptor desc((OfxImageEffectHandle)props);
    CHECK(desc.setOverlayInteractDescriptorV2(new TestOverlayDescriptor));
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV2) == (void *)TestOverlayDescriptor::mainEntry());
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV1) == NULL);
    CHECK(isOverlayInteractV2(TestOverlayDescriptor::mainEntry()));
  }
  // DrawSuite host, hasOpenGLDraw: V2 = the main entry, V1 = the OpenGL entry
  clearOverlayInteractRegistrations();
  {
    OfxPropertySetHandle props = (OfxPropertySetHandle)&descHandles[4];
    ImageEffectDescriptor desc((OfxImageEffectHandle)props);
    CHECK(desc.setOverlayInteractDescriptorV2(new TestOverlayDescriptor, true));
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV2) == (void *)TestOverlayDescriptor::mainEntry());
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV1) == (void *)TestOverlayDescriptor::mainEntryV1());
    CHECK(isOverlayInteractV2(TestOverlayDescriptor::mainEntry()));
    CHECK(!isOverlayInteractV2(TestOverlayDescriptor::mainEntryV1()));
  }
  // DrawSuite host, a descriptor with its own main entry: V2 only, even with hasOpenGLDraw
  {
    OfxPropertySetHandle props = (OfxPropertySetHandle)&descHandles[1];
    ImageEffectDescriptor desc((OfxImageEffectHandle)props);
    CHECK(desc.setOverlayInteractDescriptorV2(new CustomEntryOverlayDescriptor, true));
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV2) == (void *)customOverlayEntry);
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV1) == NULL);
  }
  // DrawSuite host that refuses the V2 property: V1 = the main entry
  clearOverlayInteractRegistrations();
  gRefuseOverlayInteractV2 = true;
  {
    OfxPropertySetHandle props = (OfxPropertySetHandle)&descHandles[2];
    ImageEffectDescriptor desc((OfxImageEffectHandle)props);
    CHECK(!desc.setOverlayInteractDescriptorV2(new TestOverlayDescriptor));
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV2) == NULL);
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV1) == (void *)TestOverlayDescriptor::mainEntry());
    CHECK(!isOverlayInteractV2(TestOverlayDescriptor::mainEntry()));
  }
  gRefuseOverlayInteractV2 = false;
  // host without the DrawSuite (e.g. OFX 1.4), the compatibility layer: V1 = the main entry, as
  // before, with or without hasOpenGLDraw
  gHostDescription.supportsDrawSuite = false;
  for (int hasOpenGLDraw = 0; hasOpenGLDraw < 2; ++hasOpenGLDraw) {
    OfxPropertySetHandle props = (OfxPropertySetHandle)&descHandles[3 + 2 * hasOpenGLDraw];
    ImageEffectDescriptor desc((OfxImageEffectHandle)props);
    CHECK(!desc.setOverlayInteractDescriptorV2(new TestOverlayDescriptor, hasOpenGLDraw != 0));
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV2) == NULL);
    CHECK(pointerProp(props, kOfxImageEffectPluginPropOverlayInteractV1) == (void *)TestOverlayDescriptor::mainEntry());
    CHECK(!isOverlayInteractV2(TestOverlayDescriptor::mainEntry()));
  }
  clearOverlayInteractRegistrations();
  gHostDescription.supportsOverlays = savedOverlays;
  gHostDescription.supportsDrawSuite = savedDrawSuite;
}

// create an interact through entry, draw it (with or without a host draw context), destroy it
static DrawRecord drawThrough(OfxPluginEntryPoint *entry, bool withDrawContext)
{
  static int effectHandle = 0;
  static int interactHandle = 0;
  static int dummyDrawContext = 0;
  OfxPropertySetHandle effectProps = (OfxPropertySetHandle)&effectHandle;
  OfxPropertySetHandle interactProps = (OfxPropertySetHandle)&interactHandle;
  gStringProps[PropKey(effectProps, kOfxImageEffectPropContext)] = kOfxImageEffectContextFilter;
  gPointerProps[PropKey(interactProps, kOfxPropEffectInstance)] = &effectHandle;
  DrawRecord none = { false, false, false, false, false, false };
  gDrawRecord = none;
  TestEffect *effect = new TestEffect((OfxImageEffectHandle)&effectHandle);
  CHECK(entry(kOfxActionCreateInstance, &interactHandle, NULL, NULL) == kOfxStatOK);
  gFakeDrawContextHandle = withDrawContext ? &dummyDrawContext : NULL;
  OfxPropertySetHandle inArgs = (OfxPropertySetHandle)&gFakePropSuite; // any other handle
  CHECK(entry(kOfxInteractActionDraw, &interactHandle, inArgs, NULL) == kOfxStatOK);
  gFakeDrawContextHandle = NULL;
  CHECK(entry(kOfxActionDestroyInstance, &interactHandle, NULL, NULL) == kOfxStatOK);
  delete effect;
  CHECK(gDrawRecord.drawn);
  return gDrawRecord;
}

// the dispatcher: the backend follows the entry point the host called
static void testDispatchPerEntry()
{
  using namespace OFX;
  using namespace OFX::Private;
  static int descHandle = 0;
  const bool savedOverlays = gHostDescription.supportsOverlays;
  const bool savedDrawSuite = gHostDescription.supportsDrawSuite;
  gHostDescription.supportsOverlays = true;
  gHostDescription.supportsDrawSuite = true;
  clearOverlayInteractRegistrations();
  {
    ImageEffectDescriptor desc((OfxImageEffectHandle)&descHandle);
    CHECK(desc.setOverlayInteractDescriptorV2(new TestOverlayDescriptor, true));
  }
  OfxPluginEntryPoint *v2 = (OfxPluginEntryPoint *)pointerProp((OfxPropertySetHandle)&descHandle, kOfxImageEffectPluginPropOverlayInteractV2);
  OfxPluginEntryPoint *v1 = (OfxPluginEntryPoint *)pointerProp((OfxPropertySetHandle)&descHandle, kOfxImageEffectPluginPropOverlayInteractV1);
  CHECK(v2 && v1 && v1 != v2);
  if (v2 && v1) {
    // the V2 entry: "drawing MUST use DrawSuite"
    DrawRecord r = drawThrough(v2, true);
    CHECK(r.hasDrawContext && r.calledViaV2 && r.usesDrawSuite && !r.usesOpenGL);
    r = drawThrough(v2, false); // no host context: nothing, never OpenGL
    CHECK(!r.hasDrawContext && r.calledViaV2 && !r.usesDrawSuite && !r.usesOpenGL);
    // the V1 entry: "drawing SHOULD use OpenGL": only OpenGL, even if the host passes a draw
    // context (without a current OpenGL context: nothing)
    r = drawThrough(v1, true);
    CHECK(!r.hasDrawContext && !r.calledViaV2 && !r.usesDrawSuite && r.glCurrent == gGLCurrent);
    CHECK(r.usesOpenGL == gGLCurrent);
    r = drawThrough(v1, false);
    CHECK(!r.hasDrawContext && !r.calledViaV2 && !r.usesDrawSuite);
    CHECK(r.usesOpenGL == gGLCurrent);
#ifdef OFXS_TEST_GL_CONTEXT
    // the V1 entry without a current OpenGL context (a DrawSuite host without OpenGL): nothing,
    // even if the host passes a draw context; never OpenGL
    CGLContextObj current = CGLGetCurrentContext();
    CGLSetCurrentContext(NULL);
    r = drawThrough(v1, true);
    CHECK(!r.hasDrawContext && !r.calledViaV2 && !r.usesDrawSuite && !r.usesOpenGL);
    r = drawThrough(v1, false);
    CHECK(!r.hasDrawContext && !r.calledViaV2 && !r.usesDrawSuite && !r.usesOpenGL);
    CGLSetCurrentContext(current);
#endif
  }
  // the same main entry registered as V1 (host without the DrawSuite, the compatibility
  // layer): only OpenGL, and the host's draw context (if any) is dropped
  clearOverlayInteractRegistrations();
  setOverlayInteractV1(TestOverlayDescriptor::mainEntry());
  {
    DrawRecord r = drawThrough(TestOverlayDescriptor::mainEntry(), true);
    CHECK(!r.hasDrawContext && !r.calledViaV2 && !r.usesDrawSuite && r.usesOpenGL == gGLCurrent);
  }
  clearOverlayInteractRegistrations();
  gHostDescription.supportsOverlays = savedOverlays;
  gHostDescription.supportsDrawSuite = savedDrawSuite;
}

#ifdef OFXS_TEST_GL_CONTEXT
// getDrawContext() without a current OpenGL context: no OpenGL call (they usually crash)
static void testNoCurrentContext()
{
  using namespace OFX;
  using namespace OFX::Private;
  OfxPropertySetHandle inArgs = (OfxPropertySetHandle)&gFakePropSuite; // any non-NULL handle
  int dummyHandle = 0;
  CHECK(openGLContextIsCurrent());
  CGLContextObj current = CGLGetCurrentContext();
  CGLSetCurrentContext(NULL);
  gGLCurrent = false;
  CHECK(!openGLContextIsCurrent());
  // no host draw context: nothing
  gFakeDrawContextHandle = NULL;
  {
    DrawArgs args((PropertySet(inArgs)));
    CHECK(!args.getDrawContext().canDraw());                // unknown registration
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(!args.getDrawContext().canDraw());
    CHECK(!args.getDrawContext().setColour(1.f, 1.f, 1.f));
    OfxPointD pts[2] = { { 0., 0. }, { 1., 1. } };
    CHECK(!args.getDrawContext().draw(kOfxDrawPrimitiveRectangle, pts, 2));
  }
  // with a host draw context: V1 never uses it (V1 draws use only OpenGL): nothing
  gFakeDrawContextHandle = &dummyHandle;
  {
    DrawArgs args((PropertySet(inArgs)));
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(!args.getDrawContext().usesDrawSuite());
    CHECK(!args.getDrawContext().canDraw());
    gDrawSuiteCalls = 0;
    CHECK(!args.getDrawContext().setColour(1.f, 0.f, 0.f));
    CHECK(gDrawSuiteCalls == 0);
  }
  gFakeDrawContextHandle = NULL;
  CGLSetCurrentContext(current);
  gGLCurrent = true;
  CHECK(openGLContextIsCurrent());
  {
    DrawArgs args((PropertySet(inArgs)));
    DrawArgsAccess::setOverlayRegistration(args, eOverlayRegistrationV1);
    CHECK(args.getDrawContext().usesOpenGL());
  }
}

#endif // OFXS_TEST_GL_CONTEXT

// the run-time loading: the OpenGL functions, and the functions that tell whether a context is
// current (on all systems: the Linux CI has libGL but no context)
static void testLoader()
{
  using namespace OFX::Private;
  const bool loaded = openGLDrawContextBackend() != NULL;
#ifdef OFXS_DRAWCONTEXT_TEST_EXPECT_OPENGL
  CHECK(loaded); // the system has OpenGL
#endif
  if (!loaded) {
    std::printf("note: the OpenGL functions can't be loaded: no OpenGL backend\n");
    CHECK(!openGLContextIsCurrent());
    CHECK(openGLCurrentContextFunctionCount() == 0);
    return;
  }
  // (on glibc, RTLD_DEFAULT is NULL: the list must not be empty because of it)
  CHECK(openGLCurrentContextFunctionCount() > 0);
  CHECK(openGLContextIsCurrent() == gGLCurrent);
}

// fake overlay main entries, for the registry
static OfxStatus fakeEntry1(const char *, const void *, OfxPropertySetHandle, OfxPropertySetHandle) { return kOfxStatOK; }
static OfxStatus fakeEntry2(const char *, const void *, OfxPropertySetHandle, OfxPropertySetHandle) { return kOfxStatOK; }

// the registry that setOverlayInteractDescriptor() and setOverlayInteractDescriptorV2() fill,
// and that the interact dispatcher reads to set the DrawArgs registration
static void testRegistry()
{
  using namespace OFX::Private;
  clearOverlayInteractRegistrations();
  CHECK(!isOverlayInteractV2(fakeEntry1));
  CHECK(!isOverlayInteractV2(NULL));
  setOverlayInteractV2(fakeEntry1);
  CHECK(isOverlayInteractV2(fakeEntry1));
  CHECK(!isOverlayInteractV2(fakeEntry2));
  setOverlayInteractV1(fakeEntry2);
  CHECK(!isOverlayInteractV2(fakeEntry2));
  setOverlayInteractV2(fakeEntry1); // twice: no change
  CHECK(isOverlayInteractV2(fakeEntry1));
  setOverlayInteractV2(NULL);
  CHECK(!isOverlayInteractV2(NULL));
  // registered both as V1 and as V2: a warning, and V2 wins
  setOverlayInteractV2(fakeEntry2);
  CHECK(isOverlayInteractV2(fakeEntry2));
  setOverlayInteractV1(fakeEntry2); // (no second warning)
  CHECK(isOverlayInteractV2(fakeEntry2));
  // the last kOfxActionUnload forgets them
  clearOverlayInteractRegistrations();
  CHECK(!isOverlayInteractV2(fakeEntry1));
  CHECK(!isOverlayInteractV2(fakeEntry2));
}

#ifdef OFXS_TEST_GL_CONTEXT
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
  // the other patterns, from the first pixel of the line
  {
    struct { OfxDrawLineStipplePattern pattern; unsigned short bits; } patterns[] = {
      { kOfxDrawLineStipplePatternDash, 0x00FF },    // 8 on, 8 off
      { kOfxDrawLineStipplePatternAltDash, 0xFF00 }, // 8 off, 8 on
      { kOfxDrawLineStipplePatternDotDash, 0x1FE3 }, // 2 on, 3 off, 8 on, 3 off
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
      clear();
      CHECK(dc.setLineStipple(patterns[i].pattern));
      CHECK(dc.draw(kOfxDrawPrimitiveLines, hline, 2));
      readBack();
      int mismatches = 0;
      for (int x = 0; x < 32; ++x) {
        const bool expected = ( (patterns[i].bits >> (x % 16)) & 1 ) != 0;
        mismatches += (lit(x, 10) != expected) ? 1 : 0;
      }
      if (mismatches != 0) {
        std::printf("stipple pattern %d: %d mismatched pixels\n", (int)patterns[i].pattern, mismatches);
      }
      CHECK(mismatches == 0);
    }
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
  CHECK(!dc.draw(kOfxDrawPrimitiveLines, tri, 3)); // pairs of points, as HostSupport's DrawSuite
  CHECK(dc.draw(kOfxDrawPrimitiveLineStrip, tri, 3));
  CHECK(OFX::Private::openGLDrawContextBackend()->setColour(dc, NULL) == kOfxStatErrValue);
  CHECK(OFX::Private::openGLDrawContextBackend()->drawText(dc, NULL, &rect[0], 0) == kOfxStatErrValue);

  // NaN and infinite inputs: refused widths, and no hang or OpenGL error for the rest
  const double inf = HUGE_VAL;
  const double nan = std::sqrt(-1.);
  CHECK(!dc.setLineWidth((float)nan));
  CHECK(!dc.setLineWidth((float)inf));
  CHECK(dc.setLineWidth(1e20f)); // accepted, clamped when drawn
  CHECK(dc.draw(kOfxDrawPrimitiveLines, line, 2));
  CHECK(dc.setLineWidth(0.f));
  OfxPointD bad[2] = { pt(nan, 0), pt(inf, -inf) };
  CHECK(dc.draw(kOfxDrawPrimitiveEllipse, bad, 2));
  CHECK(dc.draw(kOfxDrawPrimitiveLines, bad, 2));
  CHECK(dc.draw(kOfxDrawPrimitiveRectangle, bad, 2));
  CHECK(dc.drawText("nan", bad[0]));
  CHECK(dc.drawText("inf", bad[1], kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV));
  CHECK(dc.setColour((float)nan, 0.f, 0.f, 1.f));
  CHECK(dc.draw(kOfxDrawPrimitiveRectangle, rect, 2));
  CHECK(dc.setColour(1.f, 1.f, 1.f, 1.f));
  // a large ellipse (capped tessellation): its left arc crosses the viewport near x = 10
  clear();
  OfxPointD big[2] = { pt(10, -1e6), pt(10 + 2e6, 1e6) };
  CHECK(dc.draw(kOfxDrawPrimitiveEllipse, big, 2));
  readBack();
  {
    Box b = litBox();
    CHECK(b.count > 0 && b.x0 >= 8 && b.x1 <= 12);
  }
  CHECK(glGetError() == GL_NO_ERROR);

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

#endif // OFXS_TEST_GL_CONTEXT

int main()
{
  OFX::Private::gPropSuite = &gFakePropSuite;
  OFX::Private::gDrawSuite = &gFakeDrawSuite;
  gFakeEffectSuite.getPropertySet = fakeEffectGetPropertySet;
  gFakeEffectSuite.getParamSet = fakeEffectGetParamSet;
  gFakeInteractSuite.interactGetPropertySet = fakeInteractGetPropertySet;
  OFX::Private::gEffectSuite = &gFakeEffectSuite;
  OFX::Private::gInteractSuite = &gFakeInteractSuite;
  OFX::gHostDescriptionHasInit = true; // getImageEffectHostDescription() (ImageEffect's constructor)
  testRegistry();
  testLoader(); // no context current yet
#ifdef OFXS_TEST_GL_CONTEXT
  // getDrawContext() only uses OpenGL if a context is current: make the offscreen one current
  if (!OFX::Private::openGLDrawContextBackend()) {
    std::printf("built with OFXS_DRAWCONTEXT_NO_OPENGL, or OpenGL can't be loaded\n");
    return 2;
  }
  if (!initGL()) {
    return 2;
  }
  gGLCurrent = true;
  testLoader();
#endif
  testBackendSelection();
#ifdef OFXS_TEST_GL_CONTEXT
  testNoCurrentContext();
#endif
  testSetOverlayInteractDescriptorV2();
  testDispatchPerEntry();
#ifdef OFXS_TEST_GL_CONTEXT
  testOpenGL();
#endif
  if (gFailures) {
    std::printf("%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
