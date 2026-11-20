// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// suiteHandleChecks: checks that the HostSupport param, progress and timeline suites, and the
/// host's fetchSuite(), reject NULL and describe-time (descriptor) handles, NULL names and NULL
/// return pointers with the statuses of the spec, instead of crashing; and that Bytes params copy
/// their default (descriptor and instance). Run by .github/scripts/hostdemo-checks.sh; exits with 1
/// if a check fails.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxTimeLine.h"

#include "ofxhPropertySuite.h"
#include "ofxhParam.h"
#include "ofxhImageEffect.h"
#include "hostDemoHostDescriptor.h"

// the interact and draw suites, and an effect instance of a plug-in found in OFX_PLUGIN_PATH
#include <cstdlib>
#include <limits>
#include "ofxInteract.h"
#include "ofxDrawSuite.h"
#include "ofxhInteract.h"
#include "ofxhPluginCache.h"
#include "ofxhImageEffectAPI.h"
#include "hostDemoEffectInstance.h"

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

  /// hostDemo's host, with StatusEffectInstance effect instances
  class TestHost : public MyHost::Host {
  public:
    virtual ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin,
                                               ImageEffect::Descriptor &desc, const std::string &context)
    {
      return new StatusEffectInstance(plugin, desc, context);
    }
  };

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

  // Bytes params (OFX 1.5) copy their default, which the plug-in owns (#188 for the descriptor).
  // The C++ API, as the param suite's paramDefine refuses Bytes if the host doesn't support them.
  const OfxPropertySuiteV1 *propSuite = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
  check("property suite", propSuite != NULL);
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

  // the C fetchSuite of the host
  OfxHost *ofxHost = host.getHandle();
  Property::Set notHostProps;
  check("fetchSuite(host) != NULL", ofxHost->fetchSuite(ofxHost->host, kOfxPropertySuite, 1) != NULL);
  check("fetchSuite(NULL host) = NULL", ofxHost->fetchSuite(NULL, kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(not the host property set) = NULL", ofxHost->fetchSuite(notHostProps.getHandle(), kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(NULL name) = NULL", ofxHost->fetchSuite(ofxHost->host, NULL, 1) == NULL);

  // a real effect instance (the progress and timeline functions on a valid handle)
  checkEffectInstance(host);

  ImageEffect::gImageEffectHost = NULL;
  std::printf("suiteHandleChecks: %d of %d checks OK\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
