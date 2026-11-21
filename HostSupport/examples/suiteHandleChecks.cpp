// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// suiteHandleChecks: checks that the HostSupport param, progress and timeline suites, and the
/// host's fetchSuite(), reject NULL and describe-time (descriptor) handles, NULL names and NULL
/// return pointers with the statuses of the spec, instead of crashing; that Bytes params copy
/// their default (descriptor and instance); that RGB and RGBA params have
/// kOfxParamPropColourManagement; and that the render inArgs have the
/// kOfxImageEffectPropBehaviourWhenUnlicensed the host sets. Run by
/// .github/scripts/hostdemo-checks.sh; exits with 1 if a check fails.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxTimeLine.h"
#include "ofxMemory.h"

#include "ofxhPropertySuite.h"
#include "ofxhParam.h"
#include "ofxhImageEffect.h"
#include "hostDemoHostDescriptor.h"

// the interact and draw suites, and an effect instance of a plug-in found in OFX_PLUGIN_PATH
#include <cstdlib>
#include <limits>
#include <map>
#include "ofxInteract.h"
#include "ofxDrawSuite.h"
#include "ofxhInteract.h"
#include "ofxhPluginCache.h"
#include "ofxhImageEffectAPI.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"

// std::bad_alloc from the host is kOfxStatErrMemory
#include <new>
#include "ofxhClip.h"
#include "ofxhMemory.h"
#include "ofxGPURender.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif
#ifdef OFX_EXTENSIONS_VEGAS
#include "ofxSonyVegas.h"
#endif

using namespace OFX::Host;

namespace {
  int gFailures = 0;
  int gChecks = 0;

  void check(const char *what, bool ok)
  {
    ++gChecks;
    if (!ok) {
      ++gFailures;
      std::printf("suiteHandleChecks: %s: FAILED\n", what);
    }
  }

  class TestSetDescriptor : public Param::SetDescriptor {
    Property::Set _props;
  public:
    virtual Property::Set &getParamSetProps() { return _props; }
  };

  class TestInteger : public Param::IntegerInstance {
  public:
    TestInteger(Param::Descriptor &d, Param::SetInstance *s) : Param::IntegerInstance(d, s) {}
    virtual OfxStatus get(int &v) { v = 1; return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, int &v) { v = 1; return kOfxStatOK; }
    virtual OfxStatus set(int) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, int) { return kOfxStatOK; }
  };

  /// a Bytes instance that records what setDefault() sees, as a host that overrides it (Natron)
  class TestBytes : public Param::BytesInstance {
  public:
    int setDefaultCalls;
    const OfxBytes *seenDefault;              ///< kOfxParamPropDefault in the last setDefault()
    std::vector<unsigned char> seenGetDefault; ///< getDefault() in the last setDefault()
    TestBytes(Param::Descriptor &d, Param::SetInstance *s) : Param::BytesInstance(d, s), setDefaultCalls(0), seenDefault(NULL) {}
    virtual void setDefault()
    {
      ++setDefaultCalls;
      seenDefault = (const OfxBytes *)getProperties().getPointerProperty(kOfxParamPropDefault);
      seenGetDefault = getDefault();
    }
    virtual OfxStatus get(std::vector<unsigned char> &v) { v.clear(); return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, std::vector<unsigned char> &v) { v.clear(); return kOfxStatOK; }
    virtual OfxStatus set(const unsigned char *, size_t) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, const unsigned char *, size_t) { return kOfxStatOK; }
  };

  /// the OfxBytes of kOfxParamPropDefault, through the property suite
  const OfxBytes gGetFailed = { NULL, 0 }; ///< what getBytesDefault() returns if propGetPointer fails
  const OfxBytes *getBytesDefault(const OfxPropertySuiteV1 *propSuite, OfxPropertySetHandle h)
  {
    void *p = NULL;
    if (propSuite->propGetPointer(h, kOfxParamPropDefault, 0, &p) != kOfxStatOK) {
      return &gGetFailed; // never a valid default
    }
    return (const OfxBytes *)p;
  }

  bool sameBytes(const OfxBytes *b, const unsigned char *data, size_t length)
  {
    return b && b->length == length && b->data && std::memcmp(b->data, data, length) == 0;
  }

  /// the Bytes default checks, on a descriptor (bytes == NULL) or on an instance
  void checkBytesDefault(const OfxPropertySuiteV1 *propSuite, OfxPropertySetHandle h, const char *what, TestBytes *bytes)
  {
    const unsigned char orig[4] = { 1, 2, 3, 4 };
    const OfxBytes *stackAddress = NULL;
    const unsigned char *bufAddress = NULL;
    int calls = bytes ? bytes->setDefaultCalls : 0;
    {
      unsigned char buf[4];
      std::memcpy(buf, orig, sizeof(buf));
      OfxBytes stackBytes = { buf, sizeof(buf) };
      stackAddress = &stackBytes;
      bufAddress = buf;
      check((std::string(what) + ": propSetPointer(default, stack OfxBytes) = kOfxStatOK").c_str(),
            propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &stackBytes) == kOfxStatOK);
      // the plug-in reuses its buffer
      std::memset(buf, 0xff, sizeof(buf));
      stackBytes.data = NULL;
      stackBytes.length = 0;
    }
    const OfxBytes *def = getBytesDefault(propSuite, h);
    check((std::string(what) + ": the default is a copy, not the plug-in's OfxBytes").c_str(),
          def != NULL && def != stackAddress && def->data != bufAddress);
    check((std::string(what) + ": the copy has the original length and bytes").c_str(), sameBytes(def, orig, sizeof(orig)));
    if (bytes) {
      check((std::string(what) + ": getDefault() has the original bytes").c_str(),
            bytes->getDefault().size() == sizeof(orig) && std::memcmp(&bytes->getDefault()[0], orig, sizeof(orig)) == 0);
      check((std::string(what) + ": setDefault() called once, and sees the copy").c_str(),
            bytes->setDefaultCalls == calls + 1 && bytes->seenDefault == def && bytes->seenGetDefault == bytes->getDefault());
    }

    // the plug-in may pass a pointer into the host's copy (alias-safe copy)
    if (def && def->data && def->length == sizeof(orig)) {
      OfxBytes alias = { def->data + 1, 2 };
      check((std::string(what) + ": propSetPointer(default, into the host's copy) = kOfxStatOK").c_str(),
            propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &alias) == kOfxStatOK);
      check((std::string(what) + ": a default into the host's copy is copied").c_str(),
            sameBytes(getBytesDefault(propSuite, h), orig + 1, 2) && (!bytes || bytes->getDefault().size() == 2));
    }

    check((std::string(what) + ": propSetPointer(default, NULL) = kOfxStatOK").c_str(),
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, NULL) == kOfxStatOK);
    check((std::string(what) + ": a NULL default stays NULL").c_str(), getBytesDefault(propSuite, h) == NULL && (!bytes || bytes->getDefault().empty()));

    unsigned char buf2[2] = { 5, 6 };
    OfxBytes b2 = { buf2, sizeof(buf2) };
    check((std::string(what) + ": propReset(default) = kOfxStatOK").c_str(),
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &b2) == kOfxStatOK &&
          sameBytes(getBytesDefault(propSuite, h), buf2, sizeof(buf2)) &&
          propSuite->propReset(h, kOfxParamPropDefault) == kOfxStatOK);
    check((std::string(what) + ": propReset gives a NULL default").c_str(), getBytesDefault(propSuite, h) == NULL && (!bytes || bytes->getDefault().empty()));

    OfxBytes noData = { NULL, 4 };
    check((std::string(what) + ": propSetPointer(default, {NULL, 4}) = kOfxStatErrValue").c_str(),
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &b2) == kOfxStatOK &&
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &noData) == kOfxStatErrValue);
    check((std::string(what) + ": {NULL, 4} gives a NULL default").c_str(), getBytesDefault(propSuite, h) == NULL && (!bytes || bytes->getDefault().empty()));
    if (bytes) {
      check((std::string(what) + ": {NULL, 4}: setDefault() sees the NULL default").c_str(), bytes->seenDefault == NULL && bytes->seenGetDefault.empty());
    }
  }

  /// an RGBA instance with a constant value, (0, 0, 0, 1)
  class TestRGBA : public Param::RGBAInstance {
  public:
    TestRGBA(Param::Descriptor &d, Param::SetInstance *s) : Param::RGBAInstance(d, s) {}
    virtual OfxStatus get(double &r, double &g, double &b, double &a) { r = g = b = 0.; a = 1.; return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, double &r, double &g, double &b, double &a) { return get(r, g, b, a); }
    virtual OfxStatus set(double, double, double, double) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, double, double, double, double) { return kOfxStatOK; }
  };

  /// a group instance that counts the setOpen() calls (the host is told of kOfxParamPropGroupOpen)
  class TestGroup : public Param::GroupInstance {
  public:
    int opens;
    TestGroup(Param::Descriptor &d, Param::SetInstance *s) : Param::GroupInstance(d, s), opens(0) {}
    virtual void setOpen() { ++opens; }
  };

  class TestSetInstance : public Param::SetInstance {
    Property::Set _props;
  public:
    virtual Property::Set &getParamSetProps() { return _props; }
    virtual void paramChangedByPlugin(Param::Instance *) {}
    virtual Param::Instance *newParam(const std::string &, Param::Descriptor &d) { return new TestInteger(d, this); }
    virtual OfxStatus editBegin(const std::string &) { return kOfxStatOK; }
    virtual OfxStatus editEnd() { return kOfxStatOK; }
  };
}

namespace {
  /// an interact instance with no view: swapBuffers() and redraw() count their calls
  class TestInteract : public Interact::Instance {
  public:
    int swaps, redraws;
    TestInteract(Interact::Descriptor &d) : Interact::Instance(d, NULL), swaps(0), redraws(0) {}
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    virtual void getViewportSize(double &w, double &h) const { w = h = 100.; }
#endif
    virtual void getPixelScale(double &x, double &y) const { x = y = 1.; }
#ifdef OFX_EXTENSIONS_NATRON
    virtual double getScreenPixelRatio() const { return 1.; }
#endif
    virtual void getBackgroundColour(double &r, double &g, double &b) const { r = g = b = 0.; }
    virtual bool getSuggestedColour(double &, double &, double &) const { return false; }
    virtual OfxStatus swapBuffers() { ++swaps; return kOfxStatOK; }
    virtual OfxStatus redraw() { ++redraws; return kOfxStatOK; }
  };

  /// what the interact's kOfxInteractActionDraw saw (gInteractEntry)
  const OfxDrawSuiteV1 *gDrawSuite = NULL;
  void *gDrawInteractHandle = NULL;  ///< the handle of the interact instance being drawn
  void *gDrawDescriptorHandle = NULL; ///< the handle of its descriptor
  int gDraws = 0;
  bool gDrawContextIsHandle = false;
  OfxStatus gDrawInstanceStatus = kOfxStatOK, gDrawDescriptorStatus = kOfxStatOK, gDrawNullStatus = kOfxStatOK;

  /// the interact's entry point: in kOfxInteractActionDraw, use the DrawSuite with the draw
  /// context, with the descriptor's handle and with NULL
  OfxStatus gInteractEntry(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle)
  {
    if (std::string(action) != kOfxInteractActionDraw || !inArgs) {
      return kOfxStatReplyDefault;
    }
    ++gDraws;
    void *context = reinterpret_cast<Property::Set *>(inArgs)->getPointerProperty(kOfxInteractPropDrawContext);
    gDrawContextIsHandle = context != NULL && context == handle && handle == gDrawInteractHandle;
    // the base Instance doesn't implement the DrawSuite: "kOfxStatErrMissingHostFeature" means
    // that the call reached the instance
    gDrawInstanceStatus = gDrawSuite->setLineWidth((OfxDrawContextHandle)context, 1.f);
    gDrawDescriptorStatus = gDrawSuite->setLineWidth((OfxDrawContextHandle)gDrawDescriptorHandle, 1.f);
    gDrawNullStatus = gDrawSuite->setLineWidth(NULL, 1.f);
    return kOfxStatOK;
  }

  /// the interact suite: NULL and descriptor handles, NULL outputs; the DrawSuite: NULL, descriptor
  /// and instance handles, outside and inside kOfxInteractActionDraw
  void checkInteractSuites()
  {
    const OfxInteractSuiteV1 *is = (const OfxInteractSuiteV1 *)Interact::GetSuite(1);
    gDrawSuite = (const OfxDrawSuiteV1 *)Interact::GetDrawSuite(1);
    check("interact and draw suites", is && gDrawSuite);
    if (!is || !gDrawSuite) {
      return;
    }
    Interact::Descriptor desc;
    desc.setEntryPoint(gInteractEntry);
    TestInteract inst(desc);
    OfxInteractHandle descHandle = desc.getHandle();
    OfxInteractHandle instHandle = inst.getHandle();

    check("interactSwapBuffers(NULL) = kOfxStatErrBadHandle", is->interactSwapBuffers(NULL) == kOfxStatErrBadHandle);
    check("interactSwapBuffers(descriptor) = kOfxStatErrBadHandle", is->interactSwapBuffers(descHandle) == kOfxStatErrBadHandle);
    check("interactSwapBuffers(instance) = kOfxStatOK", is->interactSwapBuffers(instHandle) == kOfxStatOK && inst.swaps == 1);
    check("interactRedraw(NULL) = kOfxStatErrBadHandle", is->interactRedraw(NULL) == kOfxStatErrBadHandle);
    check("interactRedraw(descriptor) = kOfxStatErrBadHandle", is->interactRedraw(descHandle) == kOfxStatErrBadHandle);
    check("interactRedraw(instance) = kOfxStatOK", is->interactRedraw(instHandle) == kOfxStatOK && inst.redraws == 1);

    OfxPropertySetHandle props = (OfxPropertySetHandle)&inst;
    check("interactGetPropertySet(NULL) = kOfxStatErrBadHandle", is->interactGetPropertySet(NULL, &props) == kOfxStatErrBadHandle);
    check("interactGetPropertySet(NULL): *property = NULL", props == NULL);
    check("interactGetPropertySet(NULL output) = kOfxStatErrBadHandle", is->interactGetPropertySet(instHandle, NULL) == kOfxStatErrBadHandle);
    check("interactGetPropertySet(descriptor) = kOfxStatOK",
          is->interactGetPropertySet(descHandle, &props) == kOfxStatOK && props == desc.getPropHandle());
    check("interactGetPropertySet(instance) = kOfxStatOK",
          is->interactGetPropertySet(instHandle, &props) == kOfxStatOK && props == inst.getPropHandle());

    // ofxDrawSuite.h: "kOfxStatFailed - failure, e.g. if function is called outside kOfxInteractActionDraw"
    check("DrawSuite(NULL) = kOfxStatFailed", gDrawSuite->setLineWidth(NULL, 1.f) == kOfxStatFailed);
    check("DrawSuite(descriptor) = kOfxStatFailed", gDrawSuite->setLineWidth((OfxDrawContextHandle)descHandle, 1.f) == kOfxStatFailed);
    check("DrawSuite(instance, outside the draw action) = kOfxStatFailed",
          gDrawSuite->setLineWidth((OfxDrawContextHandle)instHandle, 1.f) == kOfxStatFailed);

    gDrawInteractHandle = instHandle;
    gDrawDescriptorHandle = descHandle;
    OfxPointD renderScale = { 1., 1. };
    const OfxStatus drawStatus = inst.drawAction(0., renderScale
#ifdef OFX_EXTENSIONS_NUKE
                                                 , 0
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                                 , NULL
#endif
                                                 );
    check("drawAction = kOfxStatOK", drawStatus == kOfxStatOK && gDraws == 1);
    check("kOfxInteractPropDrawContext is the interact handle", gDrawContextIsHandle);
    check("DrawSuite(draw context, in the draw action) reaches the instance", gDrawInstanceStatus == kOfxStatErrMissingHostFeature);
    check("DrawSuite(descriptor, in the draw action) = kOfxStatFailed", gDrawDescriptorStatus == kOfxStatFailed);
    check("DrawSuite(NULL, in the draw action) = kOfxStatFailed", gDrawNullStatus == kOfxStatFailed);
    gDrawInteractHandle = gDrawDescriptorHandle = NULL;
  }

  /// hostDemo's effect instance, with the statuses a host may throw (as Natron does): progressUpdate
  /// and progressEnd without an ongoing progress throw kOfxStatErrBadHandle ("the progress handle
  /// was invalid"); timeLineGotoTime throws kOfxStatErrValue for |t| > 1e9 (a time the host can't
  /// go to), and kOfxStatErrFatal for t == 12345 (any other status)
  class StatusEffectInstance : public MyHost::MyEffectInstance {
    bool ongoing() const { return !_progress.empty() && !_progress.back().ended; }
  public:
    StatusEffectInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context) {}
    virtual void progressEnd()
    {
      if (!ongoing()) {
        throw Property::Exception(kOfxStatErrBadHandle);
      }
      MyHost::MyEffectInstance::progressEnd();
    }
    virtual bool progressUpdate(double t)
    {
      if (!ongoing()) {
        throw Property::Exception(kOfxStatErrBadHandle);
      }
      return MyHost::MyEffectInstance::progressUpdate(t);
    }
    virtual void timeLineGotoTime(double t)
    {
      if (t > 1e9 || t < -1e9) {
        throw Property::Exception(kOfxStatErrValue);
      }
      if (t == 12345.) {
        throw Property::Exception(kOfxStatErrFatal);
      }
      MyHost::MyEffectInstance::timeLineGotoTime(t);
    }
  };

  /// a StatusEffectInstance that also records, host-side (in setCustomInArgs(), just before the
  /// action is passed to the plug-in), the kOfxImageEffectPropThumbnailRender of the inArgs of each
  /// action ("" if absent), and their kOfxImageEffectPropBehaviourWhenUnlicensed ("absent" if
  /// absent), which is getBehaviourWhenUnlicensed(): unlicensedBehaviour
  class RecordingEffectInstance : public StatusEffectInstance {
  public:
    std::map<std::string, std::string> thumbnailArg;
    std::map<std::string, std::string> unlicensedArg;
    std::string unlicensedBehaviour;
    RecordingEffectInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : StatusEffectInstance(plugin, desc, context) {}
    virtual std::string getBehaviourWhenUnlicensed() const { return unlicensedBehaviour; }
    virtual void setCustomInArgs(const std::string &action, Property::Set &inArgs)
    {
      StatusEffectInstance::setCustomInArgs(action, inArgs);
      thumbnailArg[action] = inArgs.fetchProperty(kOfxImageEffectPropThumbnailRender) ?
        inArgs.getStringProperty(kOfxImageEffectPropThumbnailRender) : std::string();
      unlicensedArg[action] = inArgs.fetchProperty(kOfxImageEffectPropBehaviourWhenUnlicensed) ?
        inArgs.getStringProperty(kOfxImageEffectPropBehaviourWhenUnlicensed) : std::string("absent");
    }
  };

  /// hostDemo's host, with RecordingEffectInstance (StatusEffectInstance) effect instances
  class TestHost : public MyHost::Host {
  public:
    virtual ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin,
                                               ImageEffect::Descriptor &desc, const std::string &context)
    {
      return new RecordingEffectInstance(plugin, desc, context);
    }
  };

  /// a host that doesn't implement kOfxParamPropColourManagement (the HostSupport default; hostDemo,
  /// and so TestHost, opt in)
  class NoParamColourManagementHost : public TestHost {
  public:
    virtual bool supportsParamColourManagement() const { return false; }
  };

  /// BeginSequenceRender, then EndSequenceRender, of the instance. No Render is called in between,
  /// unlike the \pre of EndSequenceRender ("kOfxImageEffectActionRender action was called at least
  /// once on the instance"): harmless with Invert, which does nothing in Begin/End, and enough to
  /// check their inArgs
  void beginEndSequenceRender(ImageEffect::Instance *instance)
  {
    OfxPointD renderScale = { 1., 1. };
    instance->beginRenderAction(0., 0., 1., false, renderScale, false, false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                false,
# ifdef OFX_EXTENSIONS_NATRON
                                NULL,
# endif
#endif
                                false
#ifdef OFX_EXTENSIONS_NUKE
                                , 0
#endif
                                );
    instance->endRenderAction(0., 0., 1., false, renderScale, false, false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                              false,
# ifdef OFX_EXTENSIONS_NATRON
                              NULL,
# endif
#endif
                              false
#ifdef OFX_EXTENSIONS_NUKE
                              , 0
#endif
                              );
  }

  /// a Render of the instance, at time 0, scale 1, on a 16x16 window (the status is not checked:
  /// only its inArgs are)
  void render(ImageEffect::Instance *instance)
  {
    OfxPointD renderScale = { 1., 1. };
    OfxRectI window = { 0, 0, 16, 16 };
    std::map<ImageEffect::ClipInstance *, OfxRectD> rois;
    MyHost::MyEffectInstance *myInstance = dynamic_cast<MyHost::MyEffectInstance *>(instance);
    if (myInstance) {
      myInstance->setRenderState(renderScale, window, rois);
    }
#ifdef OFX_EXTENSIONS_NUKE
    std::list<std::string> planes;
    planes.push_back(kFnOfxImagePlaneColour);
#endif
    instance->renderAction(0., kOfxImageFieldNone, window, renderScale, false, false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                           false,
# ifdef OFX_EXTENSIONS_NATRON
                           NULL,
# endif
#endif
                           false
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
                           , 0
#endif
#ifdef OFX_EXTENSIONS_VEGAS
                           , 1
#endif
#ifdef OFX_EXTENSIONS_NUKE
                           , planes
#endif
                           );
    if (myInstance) {
      myInstance->clearRenderState();
    }
  }

  /// the host descriptor lists the pixel depths, components and contexts that hostDemo supports
  /// (openfx-misc#222: HostSupport leaves them empty): the depths and components of MyImage, and
  /// the contexts of -c. With --ofx14 too: they are OFX 1.4 host properties.
  void checkHostDescriptor(MyHost::Host &host, const std::string &profile)
  {
    Property::Set &p = host.getProperties();
    const std::string what = "host descriptor (" + profile + "): ";
    check((what + "kOfxImageEffectPropSupportedPixelDepths is not empty").c_str(),
          p.getDimension(kOfxImageEffectPropSupportedPixelDepths) > 0);
    const char *const depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthHalf, kOfxBitDepthFloat };
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
      const bool listed = p.findStringPropValueIndex(kOfxImageEffectPropSupportedPixelDepths, depths[i]) != -1;
      check((what + depths[i] + " is listed if and only if MyImage makes it").c_str(),
            listed == MyHost::MyImage::isSupported(depths[i], kOfxImageComponentRGBA));
    }
    const int nComps = p.getDimension(kOfxImageEffectPropSupportedComponents);
    check((what + "kOfxImageEffectPropSupportedComponents is not empty").c_str(), nComps > 0);
    for (int i = 0; i < nComps; ++i) {
      const std::string c = p.getStringProperty(kOfxImageEffectPropSupportedComponents, i);
      check((what + "MyImage makes the listed components " + c).c_str(),
            MyHost::MyImage::isSupported(kOfxBitDepthFloat, c));
    }
    check((what + "RGBA and Alpha are listed").c_str(),
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA) != -1 &&
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedComponents, kOfxImageComponentAlpha) != -1);
    check((what + "kOfxImageEffectPropSupportedContexts lists filter and general (-c)").c_str(),
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter) != -1 &&
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral) != -1);
  }

  /// the progress and timeline suites on a valid effect instance handle: a real instance of a
  /// plug-in (HostSupport needs its binary to make one), found in OFX_PLUGIN_PATH
  void checkEffectInstance(MyHost::Host &host)
  {
    // SUITE_HANDLE_CHECKS_REQUIRE_PLUGIN=1 (hostdemo-checks.sh): the checks must run
    const char *require = std::getenv("SUITE_HANDLE_CHECKS_REQUIRE_PLUGIN");
    const bool required = require && std::string(require) == "1";
    const char *path = std::getenv("OFX_PLUGIN_PATH");
    if (!path || !*path) {
      if (required) {
        check("OFX_PLUGIN_PATH is set (SUITE_HANDLE_CHECKS_REQUIRE_PLUGIN=1)", false);
      } else {
        std::printf("suiteHandleChecks: OFX_PLUGIN_PATH is not set: the effect instance checks are skipped\n");
      }
      return;
    }
    const char *pluginId = "net.sf.openfx.invertPlugin"; // Support/Plugins/Invert, as hostDemo
    ImageEffect::PluginCache imageEffectPluginCache(&host);
    imageEffectPluginCache.registerInCache(*PluginCache::getPluginCache());
    PluginCache::getPluginCache()->scanPluginFiles();
    ImageEffect::ImageEffectPlugin *plugin = imageEffectPluginCache.getPluginById(pluginId);
    check((std::string("plug-in ") + pluginId + " in OFX_PLUGIN_PATH").c_str(), plugin != NULL);
    ImageEffect::Instance *instance = plugin ? plugin->createInstance(kOfxImageEffectContextFilter, NULL) : NULL;
    MyHost::MyEffectInstance *myInstance = dynamic_cast<MyHost::MyEffectInstance *>(instance);
    check("createInstance", myInstance != NULL);
    if (myInstance) {
      const OfxStatus stat = instance->createInstanceAction();
      check("createInstanceAction", stat == kOfxStatOK || stat == kOfxStatReplyDefault);

      // kOfxImageEffectPropThumbnailRender is in the inArgs of BeginSequenceRender and of
      // EndSequenceRender (OFX 1.5.1), with the value of the ThumbnailRenderScope
      RecordingEffectInstance *rec = dynamic_cast<RecordingEffectInstance *>(instance);
      check("RecordingEffectInstance", rec != NULL);
      if (rec) {
        beginEndSequenceRender(instance);
        check("Begin/EndSequenceRender inArgs: kOfxImageEffectPropThumbnailRender = \"false\"",
              rec->thumbnailArg[kOfxImageEffectActionBeginSequenceRender] == "false" &&
              rec->thumbnailArg[kOfxImageEffectActionEndSequenceRender] == "false");
        {
          ImageEffect::ThumbnailRenderScope thumbnail(true);
          beginEndSequenceRender(instance);
        }
        check("Begin/EndSequenceRender inArgs in a ThumbnailRenderScope: kOfxImageEffectPropThumbnailRender = \"true\"",
              rec->thumbnailArg[kOfxImageEffectActionBeginSequenceRender] == "true" &&
              rec->thumbnailArg[kOfxImageEffectActionEndSequenceRender] == "true");

        // kOfxImageEffectPropBehaviourWhenUnlicensed (next OFX release) is in the inArgs of
        // BeginSequenceRender and Render if the host sets it (Instance::getBehaviourWhenUnlicensed()),
        // and absent by default: "not set (indicating the host has not implemented this
        // functionality)"
        instance->getClipPreferences();
        beginEndSequenceRender(instance);
        render(instance);
        check("BeginSequenceRender and Render inArgs: no kOfxImageEffectPropBehaviourWhenUnlicensed by default",
              rec->unlicensedArg[kOfxImageEffectActionBeginSequenceRender] == "absent" &&
              rec->unlicensedArg[kOfxImageEffectActionRender] == "absent");
        const char *const behaviours[] = { kOfxUnlicensedFail, kOfxUnlicensedContinue };
        for (int i = 0; i < 2; ++i) {
          rec->unlicensedBehaviour = behaviours[i];
          beginEndSequenceRender(instance);
          render(instance);
          check((std::string("BeginSequenceRender and Render inArgs: kOfxImageEffectPropBehaviourWhenUnlicensed = ") + behaviours[i]).c_str(),
                rec->unlicensedArg[kOfxImageEffectActionBeginSequenceRender] == behaviours[i] &&
                rec->unlicensedArg[kOfxImageEffectActionRender] == behaviours[i]);
          check("EndSequenceRender inArgs: no kOfxImageEffectPropBehaviourWhenUnlicensed",
                rec->unlicensedArg[kOfxImageEffectActionEndSequenceRender] == "absent");
        }
        rec->unlicensedBehaviour = "OfxUnlicensedNoSuchValue";
        beginEndSequenceRender(instance);
        render(instance);
        check("BeginSequenceRender and Render inArgs: no kOfxImageEffectPropBehaviourWhenUnlicensed for an invalid value",
              rec->unlicensedArg[kOfxImageEffectActionBeginSequenceRender] == "absent" &&
              rec->unlicensedArg[kOfxImageEffectActionRender] == "absent");
        rec->unlicensedBehaviour.clear();
      }
      void *h = (void *)instance->getHandle();
      const OfxProgressSuiteV1 *progress1 = (const OfxProgressSuiteV1 *)host.fetchSuite(kOfxProgressSuite, 1);
      const OfxProgressSuiteV2 *progress2 = (const OfxProgressSuiteV2 *)host.fetchSuite(kOfxProgressSuite, 2);
      const OfxTimeLineSuiteV1 *timeline = (const OfxTimeLineSuiteV1 *)host.fetchSuite(kOfxTimeLineSuite, 1);
      check("progress V1/V2 and timeline suites", progress1 && progress2 && timeline);
      if (progress1 && progress2 && timeline) {
        // a NULL label or message id is an empty string
        const size_t n = myInstance->getProgress().size();
        // V1 and V2 share progressUpdate and progressEnd (so the checks below are for both)
        check("progress V1 and V2: the same progressUpdate and progressEnd",
              progress1->progressUpdate == progress2->progressUpdate && progress1->progressEnd == progress2->progressEnd);
        // StatusEffectInstance throws kOfxStatErrBadHandle without an ongoing progress: the suite returns it
        check("progressUpdate(instance, no progress started) = kOfxStatErrBadHandle", progress2->progressUpdate(h, 0.5) == kOfxStatErrBadHandle);
        check("progressEnd(instance, no progress started) = kOfxStatErrBadHandle", progress2->progressEnd(h) == kOfxStatErrBadHandle);
        check("progressUpdate V1(instance, no progress started) = kOfxStatErrBadHandle", progress1->progressUpdate(h, 0.5) == kOfxStatErrBadHandle);
        check("progressStart V1(instance, NULL label) = kOfxStatOK",
              progress1->progressStart(h, NULL) == kOfxStatOK && myInstance->getProgress().size() == n + 1 &&
              myInstance->getProgress().back().message.empty() && myInstance->getProgress().back().id.empty());
        check("progressEnd(instance) = kOfxStatOK", progress1->progressEnd(h) == kOfxStatOK && myInstance->getProgress().back().ended);
        check("progressStart V2(instance, NULL message, NULL messageId) = kOfxStatOK",
              progress2->progressStart(h, NULL, NULL) == kOfxStatOK && myInstance->getProgress().size() == n + 2 &&
              myInstance->getProgress().back().message.empty() && myInstance->getProgress().back().id.empty());
        check("progressStart V2(instance, message, NULL messageId) = kOfxStatOK",
              progress2->progressStart(h, "message", NULL) == kOfxStatOK && myInstance->getProgress().size() == n + 3 &&
              myInstance->getProgress().back().message == "message" && myInstance->getProgress().back().id.empty());
        check("progressUpdate(instance) = kOfxStatOK", progress2->progressUpdate(h, 0.5) == kOfxStatOK);
        check("progressEnd V2(instance) = kOfxStatOK", progress2->progressEnd(h) == kOfxStatOK);
        check("progressUpdate(instance, after progressEnd) = kOfxStatErrBadHandle", progress2->progressUpdate(h, 0.5) == kOfxStatErrBadHandle);
        check("progressEnd(instance, after progressEnd) = kOfxStatErrBadHandle", progress2->progressEnd(h) == kOfxStatErrBadHandle);

        double t = -1., t1 = -1., t2 = -1.;
        check("gotoTime(instance, 3) = kOfxStatOK", timeline->gotoTime(h, 3.) == kOfxStatOK);
        check("gotoTime(instance, NaN) = kOfxStatErrValue", timeline->gotoTime(h, std::numeric_limits<double>::quiet_NaN()) == kOfxStatErrValue);
        check("gotoTime(instance, +infinity) = kOfxStatErrValue", timeline->gotoTime(h, std::numeric_limits<double>::infinity()) == kOfxStatErrValue);
        check("gotoTime(instance, -infinity) = kOfxStatErrValue", timeline->gotoTime(h, -std::numeric_limits<double>::infinity()) == kOfxStatErrValue);
        // a thrown kOfxStatErrValue is passed through; any other thrown status is kOfxStatFailed
        check("gotoTime(instance, 1e300), the host throws kOfxStatErrValue: kOfxStatErrValue", timeline->gotoTime(h, 1e300) == kOfxStatErrValue);
        check("gotoTime(instance, 12345), the host throws kOfxStatErrFatal: kOfxStatFailed", timeline->gotoTime(h, 12345.) == kOfxStatFailed);
        check("getTime(instance) = kOfxStatOK, the time before the refused times", timeline->getTime(h, &t) == kOfxStatOK && t == 3.);
        check("getTime(instance, NULL) = kOfxStatErrBadHandle", timeline->getTime(h, NULL) == kOfxStatErrBadHandle);
        check("getTimeBounds(instance) = kOfxStatOK", timeline->getTimeBounds(h, &t1, &t2) == kOfxStatOK && t1 <= t2);
        check("getTimeBounds(instance, NULL, &lastTime) = kOfxStatErrBadHandle", timeline->getTimeBounds(h, NULL, &t2) == kOfxStatErrBadHandle);
        check("getTimeBounds(instance, &firstTime, NULL) = kOfxStatErrBadHandle", timeline->getTimeBounds(h, &t1, NULL) == kOfxStatErrBadHandle);
      }
      // before deleting the instance, while it is whole (ofxhImageEffect.h)
      instance->destroyInstanceAction();
    }
    delete instance;
    PluginCache::clearPluginCache();
  }
}

namespace {
  /// a clip instance whose image and texture fetches fail with std::bad_alloc, as a host's do
  /// when it runs out of memory
  class OutOfMemoryClip : public ImageEffect::ClipInstance {
    std::string _s;
  public:
    OutOfMemoryClip(ImageEffect::ClipDescriptor &d) : ImageEffect::ClipInstance(NULL, d), _s(kOfxBitDepthFloat) {}
    virtual const std::string &getUnmappedBitDepth() const { return _s; }
    virtual const std::string &getUnmappedComponents() const { return _s; }
    virtual const std::string &getPremult() const { return _s; }
#ifdef OFX_EXTENSIONS_NATRON
    virtual OfxRectI getFormat() const { OfxRectI r = { 0, 0, 1, 1 }; return r; }
#endif
    virtual double getAspectRatio() const { return 1.; }
    virtual double getFrameRate() const { return 25.; }
    virtual void getFrameRange(double &s, double &e) const { s = 0.; e = 1.; }
    virtual const std::string &getFieldOrder() const { return _s; }
    virtual bool getConnected() const { return true; }
    virtual double getUnmappedFrameRate() const { return 25.; }
    virtual void getUnmappedFrameRange(double &s, double &e) const { s = 0.; e = 1.; }
    virtual bool getContinuousSamples() const { return false; }
    virtual ImageEffect::Image *getImage(OfxTime, const OfxRectD *) { throw std::bad_alloc(); }
#ifdef OFX_EXTENSIONS_NUKE
    virtual ImageEffect::Image *getImagePlane(OfxTime, int, const std::string &, const OfxRectD *) { throw std::bad_alloc(); }
    virtual OfxRectD getRegionOfDefinition(OfxTime, int) const { throw std::bad_alloc(); }
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
    virtual ImageEffect::Texture *loadTexture(OfxTime, const char *, const OfxRectD *) { throw std::bad_alloc(); }
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    virtual ImageEffect::Image *getStereoscopicImage(OfxTime, int, const OfxRectD *) { throw std::bad_alloc(); }
#endif
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    virtual void setView(int) {}
#endif
    virtual OfxRectD getRegionOfDefinition(OfxTime) const { throw std::bad_alloc(); }
  };

  /// image memory whose lock fails with std::bad_alloc (e.g. a host that pages it back in)
  class OutOfMemoryImageMemory : public Memory::Instance {
  public:
    virtual void lock() { throw std::bad_alloc(); }
  };

  /// a std::bad_alloc from the host is kOfxStatErrMemory, and the returned handle or pointer NULL
  void checkBadAlloc(MyHost::Host &host)
  {
    ImageEffect::ClipDescriptor desc(kOfxImageEffectSimpleSourceClipName);
    OutOfMemoryClip clip(desc);
    OfxImageClipHandle ch = clip.getHandle();
    const OfxRectD region = { 0., 0., 1., 1. };
    void *bad = &clip; // not NULL: the functions must set it to NULL
    const OfxImageEffectSuiteV1 *ies = (const OfxImageEffectSuiteV1 *)host.fetchSuite(kOfxImageEffectSuite, 1);
    check("image effect suite", ies != NULL);
    if (ies) {
      OfxPropertySetHandle img = (OfxPropertySetHandle)bad;
      check("clipGetImage(std::bad_alloc) = kOfxStatErrMemory", ies->clipGetImage(ch, 0., &region, &img) == kOfxStatErrMemory && img == NULL);
      OfxRectD rod = region;
      check("clipGetRegionOfDefinition(std::bad_alloc) = kOfxStatErrMemory", ies->clipGetRegionOfDefinition(ch, 0., &rod) == kOfxStatErrMemory);
      OutOfMemoryImageMemory mem;
      void *ptr = bad;
      check("imageMemoryLock(std::bad_alloc) = kOfxStatErrMemory", ies->imageMemoryLock(mem.getHandle(), &ptr) == kOfxStatErrMemory && ptr == NULL);
    }
#ifdef OFX_SUPPORTS_OPENGLRENDER
    const OfxImageEffectOpenGLRenderSuiteV1 *gl = (const OfxImageEffectOpenGLRenderSuiteV1 *)host.fetchSuite(kOfxOpenGLRenderSuite, 1);
    check("OpenGL render suite", gl != NULL);
    if (gl) {
      OfxPropertySetHandle tex = (OfxPropertySetHandle)bad;
      check("clipLoadTexture(std::bad_alloc) = kOfxStatErrMemory", gl->clipLoadTexture(ch, 0., NULL, &region, &tex) == kOfxStatErrMemory && tex == NULL);
      tex = (OfxPropertySetHandle)bad;
      check("clipLoadTexture(NULL clip) = kOfxStatErrBadHandle, NULL", gl->clipLoadTexture(NULL, 0., NULL, &region, &tex) == kOfxStatErrBadHandle && tex == NULL);
    }
#endif
#ifdef OFX_EXTENSIONS_NUKE
    const FnOfxImageEffectPlaneSuiteV2 *planes = (const FnOfxImageEffectPlaneSuiteV2 *)host.fetchSuite(kFnOfxImageEffectPlaneSuite, 2);
    check("image plane suite V2", planes != NULL);
    if (planes) {
      OfxPropertySetHandle img = (OfxPropertySetHandle)bad;
      check("clipGetImagePlane(std::bad_alloc) = kOfxStatErrMemory",
            planes->clipGetImagePlane(ch, 0., 0, kFnOfxImagePlaneColour, &region, &img) == kOfxStatErrMemory && img == NULL);
      OfxRectD rod = region;
      check("clipGetRegionOfDefinition V2 (view)(std::bad_alloc) = kOfxStatErrMemory", planes->clipGetRegionOfDefinition(ch, 0., 0, &rod) == kOfxStatErrMemory);
    }
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    const OfxVegasStereoscopicImageSuiteV1 *stereo =
      (const OfxVegasStereoscopicImageSuiteV1 *)host.fetchSuite(kOfxVegasStereoscopicImageEffectSuite, 1);
    check("Vegas stereoscopic image suite", stereo != NULL);
    if (stereo) {
      OfxRectD r = region;
      OfxPropertySetHandle img = (OfxPropertySetHandle)bad;
      check("clipGetStereoscopicImage(std::bad_alloc) = kOfxStatErrMemory",
            stereo->clipGetStereoscopicImage(ch, 0., 0, &r, &img) == kOfxStatErrMemory && img == NULL);
    }
#endif
  }
}

int main()
{
  // only the plug-ins of OFX_PLUGIN_PATH (checkEffectInstance()); before the first getPluginCache()
  PluginCache::useStdOFXPluginsLocation(false);
  TestHost host;
  ImageEffect::gImageEffectHost = &host; // (set by ImageEffectPluginCache in a real host)

  const OfxParameterSuiteV1 *ps = (const OfxParameterSuiteV1 *)Param::GetSuite(1);
  check("param suite", ps != NULL);
  if (!ps) {
    return 1;
  }

  // paramDefine
  TestSetDescriptor setDesc;
  OfxParamSetHandle descSet = setDesc.getParamSetHandle();
  OfxPropertySetHandle props = (OfxPropertySetHandle)&setDesc;
  check("paramDefine(NULL type) = kOfxStatErrUnknown", ps->paramDefine(descSet, NULL, "a", &props) == kOfxStatErrUnknown);
  check("paramDefine(NULL type): *propertySet = NULL", props == NULL);
  check("paramDefine(NULL name) = kOfxStatErrValue", ps->paramDefine(descSet, kOfxParamTypeInteger, NULL, &props) == kOfxStatErrValue);
  check("paramDefine(unknown type) = kOfxStatErrUnknown", ps->paramDefine(descSet, "OfxParamTypeNoSuchType", "a", &props) == kOfxStatErrUnknown);
  check("paramDefine(integer) = kOfxStatOK", ps->paramDefine(descSet, kOfxParamTypeInteger, "i", &props) == kOfxStatOK && props != NULL);
  check("paramDefine(existing name) = kOfxStatErrExists", ps->paramDefine(descSet, kOfxParamTypeInteger, "i", &props) == kOfxStatErrExists && props != NULL);
  check("paramDefine(NULL set) = kOfxStatErrBadHandle", ps->paramDefine(NULL, kOfxParamTypeInteger, "j", &props) == kOfxStatErrBadHandle);

  TestSetInstance setInst;
  Param::Descriptor *intDesc = setDesc.getParams().find("i")->second;
  setInst.addParam("i", setInst.newParam("i", *intDesc));
  OfxParamSetHandle instSet = setInst.getParamSetHandle();
  check("paramDefine(instance's set) = kOfxStatErrBadHandle", ps->paramDefine(instSet, kOfxParamTypeInteger, "j", &props) == kOfxStatErrBadHandle);

  // paramGetHandle
  OfxParamHandle param = (OfxParamHandle)&setInst;
  props = (OfxPropertySetHandle)&setInst;
  check("paramGetHandle(NULL name) = kOfxStatErrUnknown", ps->paramGetHandle(instSet, NULL, &param, &props) == kOfxStatErrUnknown);
  check("paramGetHandle(NULL name): *param and *propertySet = NULL", param == NULL && props == NULL);
  OfxParamHandle instParam = NULL;
  check("paramGetHandle(instance) = kOfxStatOK", ps->paramGetHandle(instSet, "i", &instParam, NULL) == kOfxStatOK && instParam != NULL);
  OfxParamHandle descParam = NULL;
  check("paramGetHandle(descriptor) = kOfxStatOK", ps->paramGetHandle(descSet, "i", &descParam, NULL) == kOfxStatOK && descParam != NULL);

  // the value and key functions need an instance; paramGetPropertySet works on both
  int v = 0;
  unsigned int n = 0;
  check("paramGetValue(instance) = kOfxStatOK", ps->paramGetValue(instParam, &v) == kOfxStatOK && v == 1);
  check("paramGetValue(descriptor) = kOfxStatErrBadHandle", ps->paramGetValue(descParam, &v) == kOfxStatErrBadHandle);
  check("paramGetValue(NULL) = kOfxStatErrBadHandle", ps->paramGetValue(NULL, &v) == kOfxStatErrBadHandle);
  check("paramSetValue(descriptor) = kOfxStatErrBadHandle", ps->paramSetValue(descParam, 2) == kOfxStatErrBadHandle);
  check("paramGetNumKeys(descriptor) = kOfxStatErrBadHandle", ps->paramGetNumKeys(descParam, &n) == kOfxStatErrBadHandle);
  check("paramGetNumKeys(NULL output) = kOfxStatErrBadHandle", ps->paramGetNumKeys(instParam, NULL) == kOfxStatErrBadHandle);
  check("paramGetKeyTime(NULL output) = kOfxStatErrBadHandle", ps->paramGetKeyTime(instParam, 0, NULL) == kOfxStatErrBadHandle);
  check("paramGetKeyIndex(NULL output) = kOfxStatErrBadHandle", ps->paramGetKeyIndex(instParam, 0., 0, NULL) == kOfxStatErrBadHandle);
  check("paramCopy(instance, descriptor) = kOfxStatErrBadHandle", ps->paramCopy(instParam, descParam, 0., NULL) == kOfxStatErrBadHandle);
  props = NULL;
  check("paramGetPropertySet(descriptor) = kOfxStatOK", ps->paramGetPropertySet(descParam, &props) == kOfxStatOK && props == intDesc->getPropHandle());

  // the "descriptor (read/write) and instance (read only)" properties are read-only on instances
  // (refused with kOfxStatErrValue: the default ePluginReadOnlyEnforce policy), but writable on
  // descriptors; kOfxParamPropGroupOpen stays writable on instances (ofxNatron.h, 4 and 9)
  {
    const OfxPropertySuiteV1 *ps1 = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
    OfxPropertySetHandle ip = NULL, dp = NULL;
    check("paramGetPropertySet(instance), (descriptor)", ps1 &&
          ps->paramGetPropertySet(instParam, &ip) == kOfxStatOK && ps->paramGetPropertySet(descParam, &dp) == kOfxStatOK);
    if (ps1 && ip && dp) {
      check("propSetInt(descriptor, kOfxParamPropAnimates) = kOfxStatOK", ps1->propSetInt(dp, kOfxParamPropAnimates, 0, 1) == kOfxStatOK);
      int animates = -1;
      check("propSetInt(instance, kOfxParamPropAnimates) = kOfxStatErrValue, unchanged",
            ps1->propSetInt(ip, kOfxParamPropAnimates, 0, 0) == kOfxStatErrValue &&
            ps1->propGetInt(ip, kOfxParamPropAnimates, 0, &animates) == kOfxStatOK && animates == 1);
      check("propReset(instance, kOfxParamPropAnimates) = kOfxStatErrValue", ps1->propReset(ip, kOfxParamPropAnimates) == kOfxStatErrValue);
      check("propSetString(instance, kOfxParamPropScriptName) = kOfxStatErrValue", ps1->propSetString(ip, kOfxParamPropScriptName, 0, "x") == kOfxStatErrValue);
      check("propSetInt(instance, kOfxParamPropCanUndo) = kOfxStatErrValue", ps1->propSetInt(ip, kOfxParamPropCanUndo, 0, 0) == kOfxStatErrValue);
      check("propSetInt(instance, kOfxParamPropPersistent) = kOfxStatErrValue", ps1->propSetInt(ip, kOfxParamPropPersistent, 0, 0) == kOfxStatErrValue);
      check("propSetString(instance, kOfxParamPropParent) = kOfxStatErrValue", ps1->propSetString(ip, kOfxParamPropParent, 0, "g") == kOfxStatErrValue);
      check("propSetInt(instance, kOfxParamPropIsAnimating) = kOfxStatErrValue", ps1->propSetInt(ip, kOfxParamPropIsAnimating, 0, 1) == kOfxStatErrValue);
      // under ePluginReadOnlyWarn (Natron's policy), the write is accepted (with a warning)
      const Property::PluginReadOnlyPolicyEnum policy = Property::getPluginReadOnlyPolicy();
      Property::setPluginReadOnlyPolicy(Property::ePluginReadOnlyWarn);
      check("ePluginReadOnlyWarn: propSetInt(instance, kOfxParamPropCanUndo) = kOfxStatOK",
            ps1->propSetInt(ip, kOfxParamPropCanUndo, 0, 0) == kOfxStatOK);
      Property::setPluginReadOnlyPolicy(policy);
      check("policy restored: propSetInt(instance, kOfxParamPropCanUndo) = kOfxStatErrValue",
            ps1->propSetInt(ip, kOfxParamPropCanUndo, 0, 1) == kOfxStatErrValue);
      // the plug-in-writable instance properties stay writable
      check("propSetInt(instance, kOfxParamPropEnabled) = kOfxStatOK", ps1->propSetInt(ip, kOfxParamPropEnabled, 0, 1) == kOfxStatOK);
      check("propSetString(instance, kOfxPropLabel) = kOfxStatOK", ps1->propSetString(ip, kOfxPropLabel, 0, "label") == kOfxStatOK);
      // the host's C++ setters are not restricted
      Param::Instance *inst = setInst.getParams().find("i")->second;
      inst->getProperties().setIntProperty(kOfxParamPropIsAnimating, 1);
      check("host setIntProperty(instance, kOfxParamPropIsAnimating)", inst->getProperties().getIntProperty(kOfxParamPropIsAnimating) == 1);
    }
    // kOfxParamPropGroupOpen on a group instance: writable, and the host is told
    Param::Descriptor *groupDesc = setDesc.paramDefine(kOfxParamTypeGroup, "group");
    check("paramDefine(group)", groupDesc != NULL);
    if (ps1 && groupDesc) {
      TestGroup group(*groupDesc, &setInst);
      check("propSetInt(group instance, kOfxParamPropGroupOpen) = kOfxStatOK, setOpen() called",
            ps1->propSetInt(group.getPropHandle(), kOfxParamPropGroupOpen, 0, 0) == kOfxStatOK && group.opens == 1 &&
            group.getProperties().getIntProperty(kOfxParamPropGroupOpen) == 0);
      check("propSetString(group instance, kOfxParamPropParent) = kOfxStatErrValue",
            ps1->propSetString(group.getPropHandle(), kOfxParamPropParent, 0, "g") == kOfxStatErrValue);
    }

    // kOfxParamPropColourManagement (next OFX release) on RGB and RGBA param descriptors, with a
    // host that opts in (TestHost, as hostDemo: Host::supportsParamColourManagement()): "It
    // defaults to ::kOfxParamColourManagementNone", and "A host that supports this property MUST
    // accept all three values"; "A plug-in MUST NOT change it on a parameter instance"; "The
    // property is ignored on parameter types other than RGB and RGBA"
    check("TestHost::supportsParamColourManagement()", host.supportsParamColourManagement());
    const char *const colourTypes[] = { kOfxParamTypeRGBA, kOfxParamTypeRGB };
    const char *const colourNames[] = { "rgba", "rgb" };
    const char *const managements[] = { kOfxParamColourManagementManaged, kOfxParamColourManagementSRGB,
                                        kOfxParamColourManagementNone, kOfxParamColourManagementManaged };
    for (int t = 0; ps1 && t < 2; ++t) {
      const std::string what = std::string("paramDefine(") + colourTypes[t] + ")";
      OfxPropertySetHandle cp = NULL;
      check((what + " = kOfxStatOK").c_str(), ps->paramDefine(descSet, colourTypes[t], colourNames[t], &cp) == kOfxStatOK && cp != NULL);
      if (!cp) {
        continue;
      }
      const char *value = NULL;
      check((what + ": kOfxParamPropColourManagement = kOfxParamColourManagementNone").c_str(),
            ps1->propGetString(cp, kOfxParamPropColourManagement, 0, &value) == kOfxStatOK && value &&
            std::string(value) == kOfxParamColourManagementNone);
      for (int m = 0; m < 4; ++m) {
        value = NULL;
        check((what + ": propSetString(kOfxParamPropColourManagement, " + managements[m] + ") = kOfxStatOK, reads back").c_str(),
              ps1->propSetString(cp, kOfxParamPropColourManagement, 0, managements[m]) == kOfxStatOK &&
              ps1->propGetString(cp, kOfxParamPropColourManagement, 0, &value) == kOfxStatOK && value &&
              std::string(value) == managements[m]);
      }
    }
    Param::Descriptor *rgbaDesc = setDesc.getParams().count("rgba") ? setDesc.getParams().find("rgba")->second : NULL;
    if (ps1 && rgbaDesc) {
      TestRGBA rgba(*rgbaDesc, &setInst);
      const char *value = NULL;
      check("RGBA instance: kOfxParamPropColourManagement = kOfxParamColourManagementManaged, from the descriptor",
            ps1->propGetString(rgba.getPropHandle(), kOfxParamPropColourManagement, 0, &value) == kOfxStatOK && value &&
            std::string(value) == kOfxParamColourManagementManaged);
      check("RGBA instance: propSetString(kOfxParamPropColourManagement) = kOfxStatErrValue",
            ps1->propSetString(rgba.getPropHandle(), kOfxParamPropColourManagement, 0, kOfxParamColourManagementSRGB) == kOfxStatErrValue);
    }
    if (ps1 && dp) {
      const char *value = NULL;
      check("integer descriptor: no kOfxParamPropColourManagement (kOfxStatErrUnknown)",
            ps1->propGetString(dp, kOfxParamPropColourManagement, 0, &value) == kOfxStatErrUnknown);
    }

    // a host that doesn't opt in (the HostSupport default): "A host that does not implement it
    // will fail the set; in that case the plug-in MUST fall back to
    // ::kOfxParamColourManagementNone behaviour"
    {
      NoParamColourManagementHost noColourHost;
      ImageEffect::gImageEffectHost = &noColourHost;
      const char *const noNames[] = { "rgbaNoCM", "rgbNoCM" };
      for (int t = 0; ps1 && t < 2; ++t) {
        const std::string what = std::string("host without param colour management: paramDefine(") + colourTypes[t] + ")";
        OfxPropertySetHandle cp = NULL;
        check((what + " = kOfxStatOK").c_str(), ps->paramDefine(descSet, colourTypes[t], noNames[t], &cp) == kOfxStatOK && cp != NULL);
        if (!cp) {
          continue;
        }
        const char *value = NULL;
        int dim = -1;
        check((what + ": no kOfxParamPropColourManagement").c_str(),
              ps1->propGetString(cp, kOfxParamPropColourManagement, 0, &value) == kOfxStatErrUnknown &&
              ps1->propGetDimension(cp, kOfxParamPropColourManagement, &dim) == kOfxStatErrUnknown);
        check((what + ": propSetString(kOfxParamPropColourManagement, Managed) = kOfxStatErrUnknown").c_str(),
              ps1->propSetString(cp, kOfxParamPropColourManagement, 0, kOfxParamColourManagementManaged) == kOfxStatErrUnknown);
      }
      ImageEffect::gImageEffectHost = &host;
    }
  }

  // Bytes params (OFX 1.5) copy their default, which the plug-in owns (#188 for the descriptor).
  // The C++ API, as the param suite's paramDefine refuses Bytes if the host doesn't support them.
  const OfxPropertySuiteV1 *propSuite = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
  check("property suite", propSuite != NULL);
  if (propSuite) {
    // propGetDimension checks the magic number of the set, as the other property functions do:
    // a handle that is not a property set (here zeroed memory) gives kOfxStatErrBadHandle
    std::vector<unsigned char> notASet(2 * sizeof(Property::Set), 0);
    int dim = -1;
    check("propGetDimension(NULL) = kOfxStatErrBadHandle", propSuite->propGetDimension(NULL, kOfxParamPropDefault, &dim) == kOfxStatErrBadHandle && dim == -1);
    check("propGetDimension(NULL count) = kOfxStatErrBadHandle", propSuite->propGetDimension(intDesc->getPropHandle(), kOfxParamPropDefault, NULL) == kOfxStatErrBadHandle);
    check("propGetDimension(not a property set) = kOfxStatErrBadHandle",
          propSuite->propGetDimension((OfxPropertySetHandle)&notASet[0], kOfxParamPropDefault, &dim) == kOfxStatErrBadHandle && dim == -1);
    check("propGetDimension(param descriptor, kOfxParamPropDefault) = kOfxStatOK",
          propSuite->propGetDimension(intDesc->getPropHandle(), kOfxParamPropDefault, &dim) == kOfxStatOK && dim == 1);
    check("propGetDimension(param descriptor, unknown property) = kOfxStatErrUnknown",
          propSuite->propGetDimension(intDesc->getPropHandle(), "OfxNoSuchProperty", &dim) == kOfxStatErrUnknown);
  }
  Param::Descriptor *bytesDesc = setDesc.paramDefine(kOfxParamTypeBytes, "bytes");
  check("paramDefine(Bytes)", bytesDesc != NULL);
  if (propSuite && bytesDesc) {
    checkBytesDefault(propSuite, bytesDesc->getPropHandle(), "Bytes descriptor", NULL);

    // an instance copies the descriptor's default, and points its property to its own copy
    const unsigned char descBytes[3] = { 7, 8, 9 };
    OfxBytes descDefault = { descBytes, sizeof(descBytes) };
    check("Bytes descriptor: propSetPointer(default) = kOfxStatOK",
          propSuite->propSetPointer(bytesDesc->getPropHandle(), kOfxParamPropDefault, 0, &descDefault) == kOfxStatOK);
    const OfxBytes *descCopy = getBytesDefault(propSuite, bytesDesc->getPropHandle());
    TestBytes bytesInst(*bytesDesc, &setInst);
    const OfxBytes *instDefault = getBytesDefault(propSuite, bytesInst.getPropHandle());
    check("Bytes instance: the default is the instance's copy",
          instDefault != NULL && instDefault != descCopy && instDefault != &descDefault && instDefault->data != descCopy->data &&
          sameBytes(instDefault, descBytes, sizeof(descBytes)) &&
          bytesInst.getDefault().size() == sizeof(descBytes) && std::memcmp(&bytesInst.getDefault()[0], descBytes, sizeof(descBytes)) == 0);
    check("Bytes instance: setDefault() not called by the constructor", bytesInst.setDefaultCalls == 0);
    checkBytesDefault(propSuite, bytesInst.getPropHandle(), "Bytes instance", &bytesInst);
    check("Bytes instance: the descriptor's default is unchanged", sameBytes(getBytesDefault(propSuite, bytesDesc->getPropHandle()), descBytes, sizeof(descBytes)));

    // an instance of a descriptor with a NULL default has a NULL default
    check("Bytes descriptor: propSetPointer(default, NULL) = kOfxStatOK",
          propSuite->propSetPointer(bytesDesc->getPropHandle(), kOfxParamPropDefault, 0, NULL) == kOfxStatOK);
    TestBytes nullInst(*bytesDesc, &setInst);
    check("Bytes instance of a NULL default: NULL", getBytesDefault(propSuite, nullInst.getPropHandle()) == NULL && nullInst.getDefault().empty());
  }

  // progress and timeline suites: NULL and an effect descriptor
  ImageEffect::Descriptor effectDesc("/nonexistent", NULL);
  void *effectDescHandle = (void *)effectDesc.getHandle();
  const OfxProgressSuiteV1 *progress1 = (const OfxProgressSuiteV1 *)host.fetchSuite(kOfxProgressSuite, 1);
  const OfxProgressSuiteV2 *progress2 = (const OfxProgressSuiteV2 *)host.fetchSuite(kOfxProgressSuite, 2);
  const OfxTimeLineSuiteV1 *timeline = (const OfxTimeLineSuiteV1 *)host.fetchSuite(kOfxTimeLineSuite, 1);
  check("progress and timeline suites", progress1 && progress2 && timeline);
  if (progress1 && progress2 && timeline) {
    void *handles[2] = { NULL, effectDescHandle };
    for (int i = 0; i < 2; ++i) {
      void *h = handles[i];
      double t = 0., t1 = 0., t2 = 0.;
      const bool bad =
        progress1->progressStart(h, "label") == kOfxStatErrBadHandle &&
        progress2->progressStart(h, "message", NULL) == kOfxStatErrBadHandle &&
        progress2->progressUpdate(h, 0.5) == kOfxStatErrBadHandle &&
        progress2->progressEnd(h) == kOfxStatErrBadHandle &&
        timeline->getTime(h, &t) == kOfxStatErrBadHandle &&
        timeline->gotoTime(h, 1.) == kOfxStatErrBadHandle &&
        timeline->getTimeBounds(h, &t1, &t2) == kOfxStatErrBadHandle;
      check(i == 0 ? "progress and timeline(NULL) = kOfxStatErrBadHandle" :
                     "progress and timeline(descriptor) = kOfxStatErrBadHandle", bad);
    }
  }

  // the interact suite and the DrawSuite (no plug-in needed: a test interact)
  checkInteractSuites();

  // std::bad_alloc in the image, image memory and texture functions (no plug-in needed: test clips)
  checkBadAlloc(host);

  // the memory suite: memoryFree of a pointer that memoryAlloc didn't return is kOfxStatErrBadHandle
  const OfxMemorySuiteV1 *mem = (const OfxMemorySuiteV1 *)host.fetchSuite(kOfxMemorySuite, 1);
  check("memory suite", mem != NULL);
  if (mem) {
    void *p = NULL;
    check("memoryAlloc(100) = kOfxStatOK", mem->memoryAlloc(NULL, 100, &p) == kOfxStatOK && p != NULL);
    // "Allocated memory will be aligned for any use."
    check("memoryAlloc: aligned for any use", p && (size_t)p % 16 == 0);
    // the header is 64 bytes: the data pointer and malloc()'s pointer agree modulo 64, so a large
    // block keeps the page (or 64-byte) alignment that malloc() gives it
    void *big = NULL;
    const size_t bigSize = 4 << 20;
    check("memoryAlloc(4 MB) = kOfxStatOK", mem->memoryAlloc(NULL, bigSize, &big) == kOfxStatOK && big != NULL);
    if (big) {
      void *m = std::malloc(bigSize + 64); // what malloc() gives a block of that size
      check("memoryAlloc(4 MB): the same alignment modulo 64 as malloc()", m && (size_t)big % 64 == (size_t)m % 64);
      std::free(m);
      check("memoryFree(4 MB) = kOfxStatOK", mem->memoryFree(big) == kOfxStatOK);
    }
    if (p) {
      std::memset(p, 0, 100);
      // 64 bytes into the block, not returned by memoryAlloc: its "header" is p[0..63], zeroed above
      check("memoryFree(inside a block) = kOfxStatErrBadHandle", mem->memoryFree((char *)p + 64) == kOfxStatErrBadHandle);
      check("memoryFree(memoryAlloc's pointer) = kOfxStatOK", mem->memoryFree(p) == kOfxStatOK);
    }
    p = NULL;
    check("memoryAlloc(0) = kOfxStatOK", mem->memoryAlloc(NULL, 0, &p) == kOfxStatOK && p != NULL && mem->memoryFree(p) == kOfxStatOK);
    p = &host;
    check("memoryAlloc(too large) = kOfxStatErrMemory, NULL", mem->memoryAlloc(NULL, (size_t)-1, &p) == kOfxStatErrMemory && p == NULL);
    check("memoryAlloc(NULL output) = kOfxStatErrBadHandle", mem->memoryAlloc(NULL, 1, NULL) == kOfxStatErrBadHandle);
    check("memoryFree(NULL) = kOfxStatErrBadHandle", mem->memoryFree(NULL) == kOfxStatErrBadHandle);
    // 64 bytes into a zeroed buffer: the 64-byte "header" before it is inside the buffer
    std::vector<double> notABlock(16, 0.);
    check("memoryFree(not from memoryAlloc) = kOfxStatErrBadHandle", mem->memoryFree(&notABlock[8]) == kOfxStatErrBadHandle);
  }

  // the C fetchSuite of the host
  OfxHost *ofxHost = host.getHandle();
  Property::Set notHostProps;
  check("fetchSuite(host) != NULL", ofxHost->fetchSuite(ofxHost->host, kOfxPropertySuite, 1) != NULL);
  check("fetchSuite(NULL host) = NULL", ofxHost->fetchSuite(NULL, kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(not the host property set) = NULL", ofxHost->fetchSuite(notHostProps.getHandle(), kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(NULL name) = NULL", ofxHost->fetchSuite(ofxHost->host, NULL, 1) == NULL);

  // the supported depths, components and contexts, in both profiles
  checkHostDescriptor(host, "default");
  {
    MyHost::Host host14;
    host14.restrictToOFX14();
    checkHostDescriptor(host14, "ofx14");
  }

  // a real effect instance (the progress and timeline functions on a valid handle)
  checkEffectInstance(host);

  ImageEffect::gImageEffectHost = NULL;
  std::printf("suiteHandleChecks: %d of %d checks OK\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
