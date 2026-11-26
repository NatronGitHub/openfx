// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// suiteHandleChecks: unit checks of HostSupport (the suites' handle and argument checks, property
/// and plug-in cache behaviour, the image effect inArgs), on hostDemo's host classes. Each check*()
/// function says what it checks. Run by .github/scripts/hostdemo-checks.sh and by
/// .github/workflows/build.yml (Windows, and the extensions build); exits with 1 if a check fails.

#include <cstdio>
#include <cstring>
#include <iterator> // std::next
#include <list>
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

// properties and the plug-in cache
#include <cfloat>
#include <climits>
#include <sstream>
#include <stdexcept>
#include "ofxhPluginAPICache.h"
#include "ofxhUtilities.h" // utf16_to_utf8 (Windows)
#if !defined(_WIN32)
#include <fcntl.h> // open (lowestFreeFd)
#include <unistd.h> // close
#endif
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

  /// hostDemo's effect instance, with test hooks:
  /// - the statuses a host may throw (as Natron does): progressUpdate and progressEnd without an
  ///   ongoing progress throw kOfxStatErrBadHandle ("the progress handle was invalid");
  ///   timeLineGotoTime throws kOfxStatErrValue for |t| > 1e9 (a time the host can't go to), and
  ///   kOfxStatErrFatal for t == 12345 (any other status);
  /// - the inArgs of each action, recorded host-side (setCustomInArgs(), just before the action is
  ///   passed to the plug-in): kOfxImageEffectPropThumbnailRender ("" if absent) and
  ///   kOfxImageEffectPropBehaviourWhenUnlicensed ("absent" if absent);
  /// - what getBehaviourWhenUnlicensed() and abort() return: unlicensedBehaviour and abortValue
  class TestEffectInstance : public MyHost::MyEffectInstance {
    bool ongoing() const { return !_progress.empty() && _progress.back().ends == 0; }
  public:
    std::map<std::string, std::string> thumbnailArg;
    std::map<std::string, std::string> unlicensedArg;
    std::string unlicensedBehaviour;
    int abortValue; ///< what abort() returns; -1: it throws
    TestEffectInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context), abortValue(0) {}
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
    virtual std::string getBehaviourWhenUnlicensed() const { return unlicensedBehaviour; }
    virtual int abort()
    {
      if (abortValue == -1) {
        throw std::runtime_error("abort");
      }
      return abortValue;
    }
    virtual void setCustomInArgs(const std::string &action, Property::Set &inArgs)
    {
      MyHost::MyEffectInstance::setCustomInArgs(action, inArgs);
      thumbnailArg[action] = inArgs.fetchProperty(kOfxImageEffectPropThumbnailRender) ?
        inArgs.getStringProperty(kOfxImageEffectPropThumbnailRender) : std::string();
      unlicensedArg[action] = inArgs.fetchProperty(kOfxImageEffectPropBehaviourWhenUnlicensed) ?
        inArgs.getStringProperty(kOfxImageEffectPropBehaviourWhenUnlicensed) : std::string("absent");
    }
  };

  /// hostDemo's host, with TestEffectInstance effect instances
  class TestHost : public MyHost::Host {
  public:
    virtual ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin,
                                               ImageEffect::Descriptor &desc, const std::string &context)
    {
      return new TestEffectInstance(plugin, desc, context);
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
    const OfxPointD renderScale = { 1., 1. };
    // (the same signature)
    for (auto action : { &ImageEffect::Instance::beginRenderAction, &ImageEffect::Instance::endRenderAction }) {
      (instance->*action)(0., 0., 1., false, renderScale, false, false,
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

  /// setPluginHostPath(hostId) adds the host's own directories, .../OFX/hostId, and not the standard
  /// one, .../OFX/Plugins, on every OS (#324)
  void checkPluginHostPath()
  {
    const auto endsWith = [](const std::string &s, const std::string &end) {
      return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    };
    const std::string hostId = "SuiteHandleChecksHost";
#if defined(WINDOWS)
    const std::string suffix = "\\OFX\\" + hostId;
    // (the default hostId first, as in a host: the PluginCache constructor)
    const std::string stdPath = OFX::utf16_to_utf8(PluginCache::getStdOFXPluginPath());
    check("getStdOFXPluginPath() ends with \\OFX\\Plugins", endsWith(stdPath, "\\OFX\\Plugins"));
    check("getStdOFXPluginPath(hostId) is getStdOFXPluginPath() with hostId instead of Plugins",
          OFX::utf16_to_utf8(PluginCache::getStdOFXPluginPath(hostId)) == stdPath.substr(0, stdPath.size() - std::strlen("Plugins")) + hostId);
#else
    const std::string suffix = "/OFX/" + hostId;
#endif
    PluginCache pc; // (not the singleton)
    const std::list<std::string> before = pc.getPluginPath();
    pc.setPluginHostPath(hostId);
    const std::list<std::string> &after = pc.getPluginPath();
    check("setPluginHostPath(hostId) adds directories", after.size() > before.size());
    bool allHost = true;
    for (std::list<std::string>::const_iterator it = std::next(after.begin(), before.size()); it != after.end(); ++it) {
      if (!endsWith(*it, suffix)) {
        std::printf("suiteHandleChecks: setPluginHostPath(\"%s\") added %s\n", hostId.c_str(), it->c_str());
        allHost = false;
      }
    }
    check("setPluginHostPath(hostId) adds only .../OFX/hostId directories", allHost);
  }

  /// the labels of an image effect fall back to the label, then to the name (#229: the short and
  /// long labels gave "" when only the label was set)
  void checkEffectLabels()
  {
    static const Property::PropSpec spec[] = {
      { kOfxPropName, Property::eString, 1, false, "name" },
      { kOfxPropLabel, Property::eString, 1, false, "" },
      { kOfxPropShortLabel, Property::eString, 1, false, "" },
      { kOfxPropLongLabel, Property::eString, 1, false, "" },
      Property::propSpecEnd
    };
    ImageEffect::Base effect(spec);
    check("effect labels: no label, the name",
          effect.getLabel() == "name" && effect.getShortLabel() == "name" && effect.getLongLabel() == "name");
    effect.getProps().setStringProperty(kOfxPropLabel, "label");
    check("effect labels: a label, no short or long label: the label",
          effect.getLabel() == "label" && effect.getShortLabel() == "label" && effect.getLongLabel() == "label");
    effect.getProps().setStringProperty(kOfxPropShortLabel, "short");
    effect.getProps().setStringProperty(kOfxPropLongLabel, "long");
    check("effect labels: the short and long labels",
          effect.getLabel() == "label" && effect.getShortLabel() == "short" && effect.getLongLabel() == "long");
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

  /// an int property that counts its live copies, and whose deepCopy() can throw
  /// (inside the class, Property names the base class OFX::Host::Property::Property)
  typedef OFX::Host::Property::Int IntProperty;
  class CountedInt : public IntProperty {
  public:
    static int live;
    bool throwOnCopy;
    CountedInt(const std::string &name, bool throws)
      : IntProperty(name, 1, false, 0), throwOnCopy(throws) { ++live; }
    CountedInt(const CountedInt &other) : IntProperty(other), throwOnCopy(other.throwOnCopy) { ++live; }
    virtual ~CountedInt() { --live; }
    virtual CountedInt *deepCopy() const
    {
      if (throwOnCopy) {
        throw std::bad_alloc();
      }
      return new CountedInt(*this);
    }
  };
  int CountedInt::live = 0;

  /// NatronGitHub/openfx#232: the copy of a property set that throws midway deletes the copies
  /// that it made (the properties are copied in the order of their names: C throws, after A and B)
  void checkSetCopyThrows()
  {
    {
      Property::Set set;
      set.addProperty(new CountedInt("A", false));
      set.addProperty(new CountedInt("B", false));
      set.addProperty(new CountedInt("C", true));
      bool thrown = false;
      try {
        Property::Set copy(set);
      } catch (const std::bad_alloc &) {
        thrown = true;
      }
      check("Set(const Set&): a property copy that throws: the exception reaches the caller, and no copy leaks",
            thrown && CountedInt::live == 3);
      Property::Set noThrow;
      noThrow.addProperty(new CountedInt("A", false));
      {
        Property::Set copy(noThrow);
        check("Set(const Set&) copies the properties", CountedInt::live == 5 && copy.fetchProperty("A") != NULL &&
              copy.fetchProperty("A") != noThrow.fetchProperty("A"));
      }
    }
    check("the properties are deleted with their sets", CountedInt::live == 0);
  }

  /// property values: a pointer property defaults to NULL, whatever the
  /// default string of its spec; doubles turn into strings that read back exactly (the plug-in
  /// cache, and the numeric param limits); a malformed property in the cache is an error
  void checkPropertyValues()
  {
    static const Property::PropSpec pointerSpec[] = {
      { "TestPointerZero", Property::ePointer, 1, true, "0" },
      { "TestPointerNull", Property::ePointer, 1, false, NULL },
      Property::propSpecEnd
    };
    Property::Set pointers(pointerSpec);
    check("a pointer property with the default \"0\" is NULL", pointers.getPointerProperty("TestPointerZero") == NULL);
    check("a pointer property with no default is NULL", pointers.getPointerProperty("TestPointerNull") == NULL);

    const double values[] = { DBL_MAX, -DBL_MAX, DBL_MIN, -DBL_MIN, 0.1, 1. / 3., 5e-324 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      const std::string s = Property::castToString(values[i]);
      check(("castToString(" + s + ") reads back exactly (stringToDouble)").c_str(), Property::stringToDouble(s) == values[i]);
      check(("castToString(" + s + ") reads back exactly (atof)").c_str(), std::atof(s.c_str()) == values[i]);
    }

    // a double param's limits are exactly -DBL_MAX and DBL_MAX, an int param's INT_MIN and INT_MAX
    Param::Descriptor dbl(kOfxParamTypeDouble, "dbl");
    Param::Descriptor dbl2(kOfxParamTypeDouble2D, "dbl2");
    Param::Descriptor integer(kOfxParamTypeInteger, "int");
    dbl.addStandardParamProps(kOfxParamTypeDouble);
    dbl2.addStandardParamProps(kOfxParamTypeDouble2D);
    integer.addStandardParamProps(kOfxParamTypeInteger);
    const Property::Set &dp = dbl.getProperties();
    const Property::Set &dp2 = dbl2.getProperties();
    const Property::Set &ip = integer.getProperties();
    check("double param: kOfxParamPropMax == DBL_MAX, kOfxParamPropMin == -DBL_MAX",
          dp.getDoubleProperty(kOfxParamPropMax) == DBL_MAX && dp.getDoubleProperty(kOfxParamPropMin) == -DBL_MAX);
    check("double param: kOfxParamPropDisplayMax == DBL_MAX, kOfxParamPropDisplayMin == -DBL_MAX",
          dp.getDoubleProperty(kOfxParamPropDisplayMax) == DBL_MAX && dp.getDoubleProperty(kOfxParamPropDisplayMin) == -DBL_MAX);
    check("double 2D param: kOfxParamPropMax[1] == DBL_MAX, kOfxParamPropMin[1] == -DBL_MAX",
          dp2.getDoubleProperty(kOfxParamPropMax, 1) == DBL_MAX && dp2.getDoubleProperty(kOfxParamPropMin, 1) == -DBL_MAX);
    check("int param: kOfxParamPropMax == INT_MAX, kOfxParamPropMin == INT_MIN",
          ip.getIntProperty(kOfxParamPropMax) == INT_MAX && ip.getIntProperty(kOfxParamPropMin) == INT_MIN);

    // the cache reader: a value index beyond the values read so far (a variable dimension) or the
    // dimension (a fixed one), a bad index, or an unknown element, is an error (not an assert, nor
    // a huge allocation)
    struct XmlCase { const char *what; const char *dimension; const char *index; const char *element; };
    static const XmlCase cases[] = {
      { "a variable-dimension property value at index 1000000000", "0", "1000000000", "value" },
      { "a variable-dimension property value at index 1 before index 0", "0", "1", "value" },
      { "a 1-dimension property value at index 1", "1", "1", "value" },
      { "a property value at index 99999999999999999999", "0", "99999999999999999999", "value" },
      { "a property value at index \"x\"", "0", "x", "value" },
      { "an unknown element", "0", "0", "bogus" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      Property::Set set;
      Property::Property *current = NULL;
      std::map<std::string, std::string> prop, value;
      prop["name"] = "TestStrings";
      prop["type"] = "string";
      prop["dimension"] = cases[i].dimension;
      value["index"] = cases[i].index;
      value["value"] = "v";
      bool refused = false;
      try {
        APICache::propertySetXMLRead("property", prop, set, current);
        APICache::propertySetXMLRead(cases[i].element, value, set, current);
      } catch (const std::runtime_error &) {
        refused = true;
      }
      check((std::string("plug-in cache: ") + cases[i].what + " is an error").c_str(),
            refused && (!current || current->getDimension() <= 1));
    }
    {
      Property::Set set;
      Property::Property *current = NULL;
      std::map<std::string, std::string> prop, value;
      prop["name"] = "TestDoubles";
      prop["type"] = "double";
      prop["dimension"] = "0";
      bool ok = true;
      try {
        APICache::propertySetXMLRead("property", prop, set, current);
        for (int i = 0; i < 3; ++i) {
          value["index"] = Property::castToString(i);
          value["value"] = Property::castToString(values[i]); // as propertySetXMLWrite
          APICache::propertySetXMLRead("value", value, set, current);
        }
      } catch (...) {
        ok = false;
      }
      check("plug-in cache: DBL_MAX, -DBL_MAX and DBL_MIN read back exactly",
            ok && set.getDimension("TestDoubles") == 3 && set.getDoubleProperty("TestDoubles", 0) == DBL_MAX &&
            set.getDoubleProperty("TestDoubles", 1) == -DBL_MAX && set.getDoubleProperty("TestDoubles", 2) == DBL_MIN);
    }
    checkSetCopyThrows();
  }

  /// the number of image effect plug-ins of a plug-in cache with this identifier
  int countPlugins(const std::list<Plugin *> &plugins, const std::string &id)
  {
    int n = 0;
    for (std::list<Plugin *>::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
      n += ((*it)->getIdentifier() == id);
    }
    return n;
  }

  /// the plug-in cache the singleton writes, and its number of plug-ins with one identifier
  struct CacheSnapshot {
    std::string xml;
    int nInvert = 0;
  };

  /// a PluginCache other than the singleton, with its image effect API cache (destroyed after the
  /// plug-ins of cache). registerInCache() sets ImageEffect::gImageEffectHost: the destructor
  /// restores it (the same host).
  struct ScratchCache {
    MyHost::Host &host;
    ImageEffect::PluginCache effects;
    PluginCache cache;
    explicit ScratchCache(MyHost::Host &h) : host(h), effects(&h) { effects.registerInCache(cache); }
    ~ScratchCache() { ImageEffect::gImageEffectHost = &host; }
    ScratchCache(const ScratchCache &) = delete;
    ScratchCache &operator=(const ScratchCache &) = delete;
    /// readCache(text): false if it threw std::runtime_error (a malformed cache), whose message is
    /// printed unless quiet
    bool read(const std::string &text, bool quiet = false)
    {
      std::istringstream is(text);
      try {
        cache.readCache(is);
      } catch (const std::runtime_error &e) {
        if (!quiet) {
          std::printf("suiteHandleChecks: readCache: %s\n", e.what());
        }
        return false;
      }
      return true;
    }
    /// writePluginCache()
    std::string written()
    {
      std::ostringstream os;
      cache.writePluginCache(os);
      return os.str();
    }
  };

  /// a binary changed since the cache (another mtime) is loaded by the first scan, and not again
  /// by the second one: the plug-in stays the same, and isn't added twice. Run when no other plug-in
  /// cache has the binary loaded (two caches loading one library would share its Load/Unload state).
  void checkChangedBinary(MyHost::Host &host, const CacheSnapshot &snapshot, const std::string &pluginId)
  {
    const int nInvert = snapshot.nInvert;
    std::string changed = snapshot.xml;
    const size_t bundle = changed.find("example-Invert-support.ofx.bundle");
    const size_t mtime = bundle == std::string::npos ? bundle : changed.find("mtime=\"", bundle);
    const size_t end = mtime == std::string::npos ? mtime : changed.find('"', mtime + strlen("mtime=\""));
    check("the cache has the mtime of the Invert binary", end != std::string::npos);
    if (end != std::string::npos) {
      changed.replace(mtime, end + 1 - mtime, "mtime=\"1\"");
      ScratchCache other(host);
      const bool ok = other.read(changed);
      other.cache.scanPluginFiles();
      ImageEffect::ImageEffectPlugin *first = other.effects.getPluginById(pluginId);
      const size_t n = other.cache.getPlugins().size();
      check("a changed binary: the cache is read, the first scan loads its plug-in, and the cache is dirty",
            ok && first != NULL && other.cache.dirty() && countPlugins(other.cache.getPlugins(), pluginId) == nInvert);
      other.cache.scanPluginFiles();
      // other.effects.getPlugins().size() == n holds because image effects are the only API registered here
      check("a changed binary: the second scan doesn't load it again (same plug-in, same count)",
            other.effects.getPluginById(pluginId) == first && other.cache.getPlugins().size() == n &&
            countPlugins(other.cache.getPlugins(), pluginId) == nInvert && other.effects.getPlugins().size() == n);
    }
  }

  /// NatronGitHub/openfx#235: a binary of the plug-in cache that can't be stat()ed (e.g. removed)
  /// is invalid, and the plug-ins the cache has for it are not used, even if the cache recorded
  /// the mtime and size that a failed stat() gives (0)
  void checkCachedInvalidBinary(MyHost::Host &host, const std::string &cache)
  {
    {
      PluginBinary pb("/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx", "/nonexistent/x.ofx.bundle", 0, 0);
      check("a cached binary that doesn't exist (mtime 0, size 0) is invalid, and not changed", pb.isInvalid() && !pb.hasBinaryChanged());
      PluginBinary pb2("/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx", "/nonexistent/x.ofx.bundle", 1, 2);
      check("a cached binary that doesn't exist (another mtime and size) is invalid, and changed", pb2.isInvalid() && pb2.hasBinaryChanged());
      // a binary cached valid that can no longer be stat()ed: its scan records the mtime and size
      // of a failed stat(), so that the cache is rewritten once, and then stays clean
      pb2.loadPluginInfo(NULL);
      check("a cached binary that doesn't exist: loadPluginInfo() records mtime 0 and size 0",
            pb2.getFileModificationTime() == 0 && pb2.getFileSize() == 0 && pb2.getNPlugins() == 0);
    }
    std::string missing = cache;
    const size_t invert = missing.find("example-Invert-support.ofx.bundle");
    const size_t bundle = invert == std::string::npos ? invert : missing.rfind("<bundle>", invert);
    const size_t path = bundle == std::string::npos ? bundle : missing.find(" path=\"", bundle);
    const size_t pathEnd = path == std::string::npos ? path : missing.find('"', path + strlen(" path=\""));
    const size_t mtime = pathEnd == std::string::npos ? pathEnd : missing.find(" mtime=\"", pathEnd);
    const size_t end = mtime == std::string::npos ? mtime : missing.find("/>", mtime);
    const size_t plugin = end == std::string::npos ? end : missing.find("<plugin ", end);
    const bool hasPlugin = plugin != std::string::npos && plugin < missing.find("</bundle>", end);
    check("the Invert <bundle> of the cache has a <binary> with a path and an mtime, and a <plugin>", hasPlugin);
    if (!hasPlugin) {
      return;
    }
    missing.replace(mtime, end - mtime, " mtime=\"0\" size=\"0\"");
    missing.replace(path, pathEnd + 1 - path, " path=\"/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx\"");
    ScratchCache other(host);
    const bool ok = other.read(missing);
    const std::string w = other.written();
    const size_t wBundle = w.find("/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx");
    const size_t wEnd = wBundle == std::string::npos ? wBundle : w.find("</bundle>", wBundle);
    check("a cached binary that doesn't exist (mtime 0, size 0): its cached plug-ins are not read",
          ok && wEnd != std::string::npos && w.substr(wBundle, wEnd - wBundle).find("<plugin ") == std::string::npos);
  }

  /// plug-in cache rescans and reads: a rescan doesn't duplicate the plug-ins; the cache reads into
  /// the PluginCache that reads it (not the singleton), and a malformed cache is discarded.
  /// Returns the cache the singleton writes, for checkChangedBinary() and checkCachedInvalidBinary().
  CacheSnapshot checkPluginCacheRescanAndRead(MyHost::Host &host, ImageEffect::PluginCache &imageEffectPluginCache,
                                              ImageEffect::Instance &instance, const std::string &pluginId)
  {
    PluginCache *pc = PluginCache::getPluginCache();
    const size_t nPlugins = pc->getPlugins().size();
    const size_t nEffects = imageEffectPluginCache.getPlugins().size();
    const size_t nIds = imageEffectPluginCache.getPluginsByID().size();
    const int nInvert = countPlugins(pc->getPlugins(), pluginId);
    ImageEffect::ImageEffectPlugin *const before = imageEffectPluginCache.getPluginById(pluginId);
    ImageEffect::ImageEffectPlugin *const instancePlugin = instance.getPlugin();
    pc->scanPluginFiles();
    check("rescan: getPluginById() is the same plug-in", imageEffectPluginCache.getPluginById(pluginId) == before);
    check("rescan: the instance's plug-in is the one getPluginById() gives",
          instance.getPlugin() == instancePlugin && instancePlugin == before);
    check("rescan: as many plug-ins (PluginCache::getPlugins())", pc->getPlugins().size() == nPlugins && countPlugins(pc->getPlugins(), pluginId) == nInvert);
    check("rescan: as many image effect plug-ins (ImageEffect::PluginCache::getPlugins())",
          imageEffectPluginCache.getPlugins().size() == nEffects && imageEffectPluginCache.getPluginsByID().size() == nIds);
    check("rescan: getPluginById() finds the plug-in", imageEffectPluginCache.getPluginById(pluginId) != NULL);
    check("getPluginById(no such plug-in) = NULL", imageEffectPluginCache.getPluginById("no.such.plugin") == NULL);
    check("getPluginById(id, major) finds the plug-in", imageEffectPluginCache.getPluginById(pluginId, imageEffectPluginCache.getPluginById(pluginId)->getVersionMajor()) == imageEffectPluginCache.getPluginById(pluginId));

    std::ostringstream written;
    pc->writePluginCache(written);
    const std::string cache = written.str();
    {
      // read into another PluginCache: the singleton doesn't change, and the other one writes
      // the same cache
      ScratchCache other(host);
      const bool ok = other.read(cache);
      std::ostringstream singleton;
      pc->writePluginCache(singleton);
      check("readCache() on a PluginCache other than the singleton reads into it", ok && other.written() == cache);
      check("readCache() on a PluginCache other than the singleton leaves the singleton", singleton.str() == cache && pc->getPlugins().size() == nPlugins);
      other.cache.scanPluginFiles();
      ImageEffect::ImageEffectPlugin *a = imageEffectPluginCache.getPluginById(pluginId);
      ImageEffect::ImageEffectPlugin *b = other.effects.getPluginById(pluginId);
      check("the plug-in read from the cache, after the scan", b != NULL && b != a && countPlugins(other.cache.getPlugins(), pluginId) == nInvert);
    }
    {
      // a malformed cache is discarded entirely (readCache() throws, nothing read is kept)
      std::string unknownElement = cache, hugeIndex = cache;
      const size_t api = unknownElement.find("<apiproperties>");
      if (api != std::string::npos) {
        unknownElement.insert(api + strlen("<apiproperties>"), "<bogus/>");
      }
      const size_t contexts = hugeIndex.find("\"" kOfxImageEffectPropSupportedContexts "\"");
      const size_t value = contexts == std::string::npos ? contexts : hugeIndex.find("index=\"0\"", contexts);
      if (value != std::string::npos) {
        hugeIndex.replace(value, strlen("index=\"0\""), "index=\"2000000000\"");
      }
      check("the cache has an <apiproperties> and a kOfxImageEffectPropSupportedContexts value", api != std::string::npos && value != std::string::npos);
      const std::string *bad[] = { &unknownElement, &hugeIndex };
      const char *what[] = { "an unknown element", "a value index of 2000000000 for a variable-dimension property" };
      for (int i = 0; i < 2; ++i) {
        ScratchCache other(host);
        const bool refused = !other.read(*bad[i], true);
        check((std::string("readCache() of a cache with ") + what[i] + " fails, and keeps nothing").c_str(),
              refused && other.written().find("<bundle>") == std::string::npos);
      }
    }

    CacheSnapshot snapshot;
    snapshot.xml = cache;
    snapshot.nInvert = nInvert;
    return snapshot;
  }

  /// the clips of an instance: getNthClip() is NULL out of range, and the base class's
  /// kFnOfxImageEffectPropComponentsPresent
  void checkClips(ImageEffect::Instance *instance)
  {
    // getNthClip(): NULL out of range
    const int nClips = (int)instance->getDescriptor().getClipsByOrder().size();
    check("getNthClip(0) != NULL", nClips > 0 && instance->getNthClip(0) != NULL);
    check("getNthClip(-1) = NULL", instance->getNthClip(-1) == NULL);
    check("getNthClip(number of clips) = NULL", instance->getNthClip(nClips) == NULL);
#ifdef OFX_EXTENSIONS_NUKE
    // kFnOfxImageEffectPropComponentsPresent: the base ClipInstance::getComponentsPresent() is
    // the colour plane whatever the components, none on a disconnected input (#342), and that of
    // one clip doesn't change with a query on another one
    ImageEffect::ClipInstance *source = instance->getClip(kOfxImageEffectSimpleSourceClipName);
    ImageEffect::ClipInstance *output = instance->getClip(kOfxImageEffectOutputClipName);
    const OfxPropertySuiteV1 *props = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
    check("Source and Output clips", source && output && props);
    if (source && output && props) {
      const std::string sourceComponents = source->getComponents(), outputComponents = output->getComponents();
      source->setComponents(kOfxImageComponentRGBA);
      output->setComponents(kOfxImageComponentAlpha);
      const char *s = NULL, *o = NULL;
      const OfxStatus ss = props->propGetString(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, 0, &s);
      const OfxStatus os = props->propGetString(output->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, 0, &o);
      int sn = -1, on = -1;
      props->propGetDimension(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &sn);
      props->propGetDimension(output->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &on);
      check("kFnOfxImageEffectPropComponentsPresent of Source and Output", ss == kOfxStatOK && os == kOfxStatOK && s && o);
      check("kFnOfxImageEffectPropComponentsPresent of Source and Output: dimension 1", sn == 1 && on == 1);
      if (s && o) {
        check("kFnOfxImageEffectPropComponentsPresent of Source and Output: the colour plane, whatever the components (RGBA, Alpha), one per clip",
              std::strcmp(s, kFnOfxImagePlaneColour) == 0 && std::strcmp(o, kFnOfxImagePlaneColour) == 0 && s != o);
      }
      source->setComponents(sourceComponents);
      output->setComponents(outputComponents);
      // a disconnected input: "the planes [...] that exist on the clip", none
      MyHost::MyClipInstance *mySource = dynamic_cast<MyHost::MyClipInstance *>(source);
      check("Source is a MyClipInstance", mySource != NULL);
      if (mySource) {
        const MyHost::InputSpec input = mySource->getInput();
        MyHost::InputSpec none;
        none.type = MyHost::InputSpec::eNone;
        mySource->setInput(none);
        int dn = -1;
        const OfxStatus ds = props->propGetDimension(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &dn);
        check("kFnOfxImageEffectPropComponentsPresent of a disconnected Source: dimension 0", ds == kOfxStatOK && dn == 0);
        mySource->setInput(input);
        dn = -1;
        props->propGetDimension(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &dn);
        check("kFnOfxImageEffectPropComponentsPresent of Source connected again: dimension 1", dn == 1);
      }
    }
#endif
  }

  /// an image effect API cache whose newPlugin() throws, as when it runs out of memory
  class ThrowingEffectsCache : public ImageEffect::PluginCache {
  public:
    std::string thrownFor; ///< the file of the binary whose plug-in made it throw
    explicit ThrowingEffectsCache(ImageEffect::Host *host) : ImageEffect::PluginCache(host) {}
    using ImageEffect::PluginCache::newPlugin;
    virtual Plugin *newPlugin(PluginBinary *pb, int /*pi*/, OfxPlugin * /*plug*/)
    {
      thrownFor = pb->getFilePath();
      throw std::bad_alloc();
    }
  };

#if !defined(_WIN32)
  /// the lowest free file descriptor: it is higher if one leaked
  int lowestFreeFd()
  {
    const int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
      close(fd);
    }
    return fd;
  }
#endif

  /// NatronGitHub/openfx#232: a scan that throws (here from the API cache, while the first binary
  /// is scanned) passes the exception to the caller, and closes the directories it opened
  void checkScanThrows(MyHost::Host &host)
  {
#if !defined(_WIN32)
    const int fd = lowestFreeFd();
#endif
    bool thrown = false;
    {
      ThrowingEffectsCache throwingEffects(&host); // (destroyed after the plug-ins of other)
      PluginCache other;
      throwingEffects.registerInCache(other);
      try {
        other.scanPluginFiles();
      } catch (const std::bad_alloc &) {
        thrown = true;
      }
      check("a scan that throws: the exception reaches the caller", thrown && !throwingEffects.thrownFor.empty());
    }
#if !defined(_WIN32)
    check("a scan that throws closes the directories it opened", lowestFreeFd() == fd);
#endif
    ImageEffect::gImageEffectHost = &host;
  }

  /// the inArgs of the sequence render and render actions
  void checkRenderInArgs(TestEffectInstance &instance)
  {
    // kOfxImageEffectPropThumbnailRender is in the inArgs of BeginSequenceRender and of
    // EndSequenceRender (OFX 1.5.1), with the value of the ThumbnailRenderScope
    beginEndSequenceRender(&instance);
    check("Begin/EndSequenceRender inArgs: kOfxImageEffectPropThumbnailRender = \"false\"",
          instance.thumbnailArg[kOfxImageEffectActionBeginSequenceRender] == "false" &&
          instance.thumbnailArg[kOfxImageEffectActionEndSequenceRender] == "false");
    {
      ImageEffect::ThumbnailRenderScope thumbnail(true);
      beginEndSequenceRender(&instance);
    }
    check("Begin/EndSequenceRender inArgs in a ThumbnailRenderScope: kOfxImageEffectPropThumbnailRender = \"true\"",
          instance.thumbnailArg[kOfxImageEffectActionBeginSequenceRender] == "true" &&
          instance.thumbnailArg[kOfxImageEffectActionEndSequenceRender] == "true");

    // kOfxImageEffectPropBehaviourWhenUnlicensed (next OFX release) is in the inArgs of
    // BeginSequenceRender and Render if the host sets a valid value
    // (Instance::getBehaviourWhenUnlicensed()), never in those of EndSequenceRender, and absent
    // by default: "not set (indicating the host has not implemented this functionality)"
    static const struct { const char *value, *expected; } unlicensed[] = {
      { "", "absent" },
      { kOfxUnlicensedFail, kOfxUnlicensedFail },
      { kOfxUnlicensedContinue, kOfxUnlicensedContinue },
      { "OfxUnlicensedNoSuchValue", "absent" },
    };
    instance.getClipPreferences();
    for (const auto &u : unlicensed) {
      instance.unlicensedBehaviour = u.value;
      beginEndSequenceRender(&instance);
      render(&instance);
      check((std::string("Begin/EndSequenceRender and Render inArgs, the host's behaviour \"") + u.value +
             "\": kOfxImageEffectPropBehaviourWhenUnlicensed " + u.expected + " (EndSequenceRender: absent)").c_str(),
            instance.unlicensedArg[kOfxImageEffectActionBeginSequenceRender] == u.expected &&
            instance.unlicensedArg[kOfxImageEffectActionRender] == u.expected &&
            instance.unlicensedArg[kOfxImageEffectActionEndSequenceRender] == "absent");
    }
    instance.unlicensedBehaviour.clear();
  }

  /// OfxImageEffectSuiteV1::abort: "0 if the effect should continue [...] 1 if the effect should
  /// abort" (ofxImageEffect.h), whatever the host's Instance::abort() returns
  void checkInstanceAbort(MyHost::Host &host, TestEffectInstance &instance)
  {
    const OfxImageEffectSuiteV1 *ies = (const OfxImageEffectSuiteV1 *)host.fetchSuite(kOfxImageEffectSuite, 1);
    check("image effect suite (abort)", ies != NULL);
    if (!ies) {
      return;
    }
    // (Instance::abort(), -1: it throws; what the suite returns)
    static const struct { int value, expected; const char *what; } aborts[] = {
      { 0, 0, "abort(instance), Instance::abort() = 0: 0" },
      { 1, 1, "abort(instance), Instance::abort() = 1: 1" },
      { 2, 1, "abort(instance), Instance::abort() = 2: 1" },
      { -1, 1, "abort(instance), Instance::abort() throws: 1" },
    };
    for (const auto &a : aborts) {
      instance.abortValue = a.value;
      check(a.what, ies->abort(instance.getHandle()) == a.expected);
    }
    instance.abortValue = 0;
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
    CacheSnapshot snapshot; // for checkChangedBinary() and checkCachedInvalidBinary()
    if (myInstance) {
      const OfxStatus stat = instance->createInstanceAction();
      check("createInstanceAction", stat == kOfxStatOK || stat == kOfxStatReplyDefault);

      snapshot = checkPluginCacheRescanAndRead(host, imageEffectPluginCache, *instance, pluginId);
      checkClips(instance);

      TestEffectInstance *test = dynamic_cast<TestEffectInstance *>(instance);
      check("TestEffectInstance", test != NULL);
      if (test) {
        checkRenderInArgs(*test);
        checkInstanceAbort(host, *test);
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
        // TestEffectInstance throws kOfxStatErrBadHandle without an ongoing progress: the suite returns it
        check("progressUpdate(instance, no progress started) = kOfxStatErrBadHandle", progress2->progressUpdate(h, 0.5) == kOfxStatErrBadHandle);
        check("progressEnd(instance, no progress started) = kOfxStatErrBadHandle", progress2->progressEnd(h) == kOfxStatErrBadHandle);
        check("progressUpdate V1(instance, no progress started) = kOfxStatErrBadHandle", progress1->progressUpdate(h, 0.5) == kOfxStatErrBadHandle);
        check("progressStart V1(instance, NULL label) = kOfxStatOK",
              progress1->progressStart(h, NULL) == kOfxStatOK && myInstance->getProgress().size() == n + 1 &&
              myInstance->getProgress().back().message.empty() && myInstance->getProgress().back().id.empty());
        check("progressEnd(instance) = kOfxStatOK", progress1->progressEnd(h) == kOfxStatOK && myInstance->getProgress().back().ends > 0);
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
    if (!snapshot.xml.empty()) {
      checkChangedBinary(host, snapshot, pluginId);
      checkCachedInvalidBinary(host, snapshot.xml);
    }
    checkScanThrows(host);
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
  /// the "descriptor (read/write) and instance (read only)" properties are read-only on instances
  /// (refused with kOfxStatErrValue: the default ePluginReadOnlyEnforce policy), but writable on
  /// descriptors; kOfxParamPropGroupOpen stays writable on instances (ofxNatron.h, 4 and 9)
  void checkParamInstanceReadOnly(const OfxParameterSuiteV1 *ps, const OfxPropertySuiteV1 *propSuite,
                                  TestSetDescriptor &setDesc, TestSetInstance &setInst,
                                  OfxParamHandle instParam, OfxParamHandle descParam)
  {
    OfxPropertySetHandle ip = NULL, dp = NULL;
    check("paramGetPropertySet(instance), (descriptor)",
          ps->paramGetPropertySet(instParam, &ip) == kOfxStatOK && ps->paramGetPropertySet(descParam, &dp) == kOfxStatOK);
    if (ip && dp) {
      check("propSetInt(descriptor, kOfxParamPropAnimates) = kOfxStatOK", propSuite->propSetInt(dp, kOfxParamPropAnimates, 0, 1) == kOfxStatOK);
      int animates = -1;
      check("propSetInt(instance, kOfxParamPropAnimates) = kOfxStatErrValue, unchanged",
            propSuite->propSetInt(ip, kOfxParamPropAnimates, 0, 0) == kOfxStatErrValue &&
            propSuite->propGetInt(ip, kOfxParamPropAnimates, 0, &animates) == kOfxStatOK && animates == 1);
      check("propReset(instance, kOfxParamPropAnimates) = kOfxStatErrValue", propSuite->propReset(ip, kOfxParamPropAnimates) == kOfxStatErrValue);
      check("propSetString(instance, kOfxParamPropScriptName) = kOfxStatErrValue", propSuite->propSetString(ip, kOfxParamPropScriptName, 0, "x") == kOfxStatErrValue);
      check("propSetInt(instance, kOfxParamPropCanUndo) = kOfxStatErrValue", propSuite->propSetInt(ip, kOfxParamPropCanUndo, 0, 0) == kOfxStatErrValue);
      check("propSetInt(instance, kOfxParamPropPersistent) = kOfxStatErrValue", propSuite->propSetInt(ip, kOfxParamPropPersistent, 0, 0) == kOfxStatErrValue);
      check("propSetString(instance, kOfxParamPropParent) = kOfxStatErrValue", propSuite->propSetString(ip, kOfxParamPropParent, 0, "g") == kOfxStatErrValue);
      check("propSetInt(instance, kOfxParamPropIsAnimating) = kOfxStatErrValue", propSuite->propSetInt(ip, kOfxParamPropIsAnimating, 0, 1) == kOfxStatErrValue);
      // under ePluginReadOnlyWarn (Natron's policy), the write is accepted (with a warning)
      const Property::PluginReadOnlyPolicyEnum policy = Property::getPluginReadOnlyPolicy();
      Property::setPluginReadOnlyPolicy(Property::ePluginReadOnlyWarn);
      check("ePluginReadOnlyWarn: propSetInt(instance, kOfxParamPropCanUndo) = kOfxStatOK",
            propSuite->propSetInt(ip, kOfxParamPropCanUndo, 0, 0) == kOfxStatOK);
      Property::setPluginReadOnlyPolicy(policy);
      check("policy restored: propSetInt(instance, kOfxParamPropCanUndo) = kOfxStatErrValue",
            propSuite->propSetInt(ip, kOfxParamPropCanUndo, 0, 1) == kOfxStatErrValue);
      // the plug-in-writable instance properties stay writable
      check("propSetInt(instance, kOfxParamPropEnabled) = kOfxStatOK", propSuite->propSetInt(ip, kOfxParamPropEnabled, 0, 1) == kOfxStatOK);
      check("propSetString(instance, kOfxPropLabel) = kOfxStatOK", propSuite->propSetString(ip, kOfxPropLabel, 0, "label") == kOfxStatOK);
      // the host's C++ setters are not restricted
      Param::Instance *inst = setInst.getParams().find("i")->second;
      inst->getProperties().setIntProperty(kOfxParamPropIsAnimating, 1);
      check("host setIntProperty(instance, kOfxParamPropIsAnimating)", inst->getProperties().getIntProperty(kOfxParamPropIsAnimating) == 1);
    }
    // kOfxParamPropGroupOpen on a group instance: writable, and the host is told
    Param::Descriptor *groupDesc = setDesc.paramDefine(kOfxParamTypeGroup, "group");
    check("paramDefine(group)", groupDesc != NULL);
    if (groupDesc) {
      TestGroup group(*groupDesc, &setInst);
      check("propSetInt(group instance, kOfxParamPropGroupOpen) = kOfxStatOK, setOpen() called",
            propSuite->propSetInt(group.getPropHandle(), kOfxParamPropGroupOpen, 0, 0) == kOfxStatOK && group.opens == 1 &&
            group.getProperties().getIntProperty(kOfxParamPropGroupOpen) == 0);
      check("propSetString(group instance, kOfxParamPropParent) = kOfxStatErrValue",
            propSuite->propSetString(group.getPropHandle(), kOfxParamPropParent, 0, "g") == kOfxStatErrValue);
    }
  }

  /// kOfxParamPropColourManagement (next OFX release) on RGB and RGBA param descriptors, with a
  /// host that opts in (TestHost, as hostDemo: Host::supportsParamColourManagement()): "It
  /// defaults to ::kOfxParamColourManagementNone", and "A host that supports this property MUST
  /// accept all three values"; "A plug-in MUST NOT change it on a parameter instance"; "The
  /// property is ignored on parameter types other than RGB and RGBA"
  void checkParamColourManagement(TestHost &host, const OfxParameterSuiteV1 *ps, const OfxPropertySuiteV1 *propSuite,
                                  TestSetDescriptor &setDesc, TestSetInstance &setInst, OfxPropertySetHandle dp)
  {
    OfxParamSetHandle descSet = setDesc.getParamSetHandle();
    check("TestHost::supportsParamColourManagement()", host.supportsParamColourManagement());
    const char *const colourTypes[] = { kOfxParamTypeRGBA, kOfxParamTypeRGB };
    const char *const colourNames[] = { "rgba", "rgb" };
    const char *const managements[] = { kOfxParamColourManagementManaged, kOfxParamColourManagementSRGB,
                                        kOfxParamColourManagementNone, kOfxParamColourManagementManaged };
    for (int t = 0; t < 2; ++t) {
      const std::string what = std::string("paramDefine(") + colourTypes[t] + ")";
      OfxPropertySetHandle cp = NULL;
      check((what + " = kOfxStatOK").c_str(), ps->paramDefine(descSet, colourTypes[t], colourNames[t], &cp) == kOfxStatOK && cp != NULL);
      if (!cp) {
        continue;
      }
      const char *value = NULL;
      check((what + ": kOfxParamPropColourManagement = kOfxParamColourManagementNone").c_str(),
            propSuite->propGetString(cp, kOfxParamPropColourManagement, 0, &value) == kOfxStatOK && value &&
            std::string(value) == kOfxParamColourManagementNone);
      for (int m = 0; m < 4; ++m) {
        value = NULL;
        check((what + ": propSetString(kOfxParamPropColourManagement, " + managements[m] + ") = kOfxStatOK, reads back").c_str(),
              propSuite->propSetString(cp, kOfxParamPropColourManagement, 0, managements[m]) == kOfxStatOK &&
              propSuite->propGetString(cp, kOfxParamPropColourManagement, 0, &value) == kOfxStatOK && value &&
              std::string(value) == managements[m]);
      }
    }
    Param::Descriptor *rgbaDesc = setDesc.getParams().count("rgba") ? setDesc.getParams().find("rgba")->second : NULL;
    if (rgbaDesc) {
      TestRGBA rgba(*rgbaDesc, &setInst);
      const char *value = NULL;
      check("RGBA instance: kOfxParamPropColourManagement = kOfxParamColourManagementManaged, from the descriptor",
            propSuite->propGetString(rgba.getPropHandle(), kOfxParamPropColourManagement, 0, &value) == kOfxStatOK && value &&
            std::string(value) == kOfxParamColourManagementManaged);
      check("RGBA instance: propSetString(kOfxParamPropColourManagement) = kOfxStatErrValue",
            propSuite->propSetString(rgba.getPropHandle(), kOfxParamPropColourManagement, 0, kOfxParamColourManagementSRGB) == kOfxStatErrValue);
    }
    const char *intValue = NULL;
    check("integer descriptor: no kOfxParamPropColourManagement (kOfxStatErrUnknown)",
          propSuite->propGetString(dp, kOfxParamPropColourManagement, 0, &intValue) == kOfxStatErrUnknown);

    // a host that doesn't opt in (the HostSupport default): "A host that does not implement it
    // will fail the set; in that case the plug-in MUST fall back to
    // ::kOfxParamColourManagementNone behaviour"
    {
      NoParamColourManagementHost noColourHost;
      ImageEffect::gImageEffectHost = &noColourHost;
      const char *const noNames[] = { "rgbaNoCM", "rgbNoCM" };
      for (int t = 0; t < 2; ++t) {
        const std::string what = std::string("host without param colour management: paramDefine(") + colourTypes[t] + ")";
        OfxPropertySetHandle cp = NULL;
        check((what + " = kOfxStatOK").c_str(), ps->paramDefine(descSet, colourTypes[t], noNames[t], &cp) == kOfxStatOK && cp != NULL);
        if (!cp) {
          continue;
        }
        const char *value = NULL;
        int dim = -1;
        check((what + ": no kOfxParamPropColourManagement").c_str(),
              propSuite->propGetString(cp, kOfxParamPropColourManagement, 0, &value) == kOfxStatErrUnknown &&
              propSuite->propGetDimension(cp, kOfxParamPropColourManagement, &dim) == kOfxStatErrUnknown);
        check((what + ": propSetString(kOfxParamPropColourManagement, Managed) = kOfxStatErrUnknown").c_str(),
              propSuite->propSetString(cp, kOfxParamPropColourManagement, 0, kOfxParamColourManagementManaged) == kOfxStatErrUnknown);
      }
      ImageEffect::gImageEffectHost = &host;
    }
  }

  /// OfxImageEffectSuiteV1::abort returns 0 or 1 ("1 if the effect should abort"), not a status: a
  /// handle that is not an effect instance gives 1 (#232)
  void checkAbort(MyHost::Host &host, OfxImageEffectHandle descriptor)
  {
    const OfxImageEffectSuiteV1 *ies = (const OfxImageEffectSuiteV1 *)host.fetchSuite(kOfxImageEffectSuite, 1);
    check("image effect suite", ies != NULL);
    if (ies) {
      check("abort(NULL) = 1", ies->abort(NULL) == 1);
      check("abort(descriptor) = 1", ies->abort(descriptor) == 1);
    }
  }

  /// the memory suite: memoryFree of a pointer that memoryAlloc didn't return is kOfxStatErrBadHandle
  void checkMemorySuite(MyHost::Host &host)
  {
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

  // the param properties, through the property suite
  const OfxPropertySuiteV1 *propSuite = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
  check("property suite", propSuite != NULL);
  if (propSuite) {
    checkParamInstanceReadOnly(ps, propSuite, setDesc, setInst, instParam, descParam);
    checkParamColourManagement(host, ps, propSuite, setDesc, setInst, intDesc->getPropHandle());

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

  // Bytes params (OFX 1.5) copy their default, which the plug-in owns (#188 for the descriptor).
  // The C++ API, as the param suite's paramDefine refuses Bytes if the host doesn't support them.
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

  checkAbort(host, (OfxImageEffectHandle)effectDescHandle);

  // the interact suite and the DrawSuite (no plug-in needed: a test interact)
  checkInteractSuites();

  // std::bad_alloc in the image, image memory and texture functions (no plug-in needed: test clips)
  checkBadAlloc(host);

  checkMemorySuite(host);

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

  // pointer defaults, exact doubles, malformed properties in the plug-in cache
  checkPropertyValues();

  // the effect labels' fallbacks (#229)
  checkEffectLabels();

  // the host's plug-in directories (#324)
  checkPluginHostPath();

  // a real effect instance (the progress and timeline functions on a valid handle)
  checkEffectInstance(host);

  ImageEffect::gImageEffectHost = NULL;
  std::printf("suiteHandleChecks: %d of %d checks OK\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
