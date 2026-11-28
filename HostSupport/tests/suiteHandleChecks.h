#ifndef SUITE_HANDLE_CHECKS_H
#define SUITE_HANDLE_CHECKS_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// What the two parts of suiteHandleChecks share: the functional checks (functional/suiteHandleChecks.cpp:
/// the contracts of the OFX suites and of HostSupport's API) and the regression checks
/// (regression/suiteHandleRegressions.cpp: each one reproduces a fixed bug and names its issue or
/// PR). check() counts each part on its own and labels the regression failures "regression:", so
/// that a CI failure says which kind failed.

#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxParam.h"

#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhParam.h"
#include "ofxhPropertySuite.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
#include "hostDemoHostDescriptor.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

namespace SuiteHandleChecks {

  using namespace OFX::Host;

  /// the checks and failures of the functional checks [0] and of the regression checks [1]
  inline int gChecks[2] = { 0, 0 };
  inline int gFailures[2] = { 0, 0 };
  /// > 0 inside a RegressionSection: check() counts a regression check
  inline int gRegression = 0;

  /// the checks made while it lives are regression checks (each function of
  /// regression/suiteHandleRegressions.cpp opens one)
  struct RegressionSection {
    RegressionSection() { ++gRegression; }
    ~RegressionSection() { --gRegression; }
    RegressionSection(const RegressionSection &) = delete;
    RegressionSection &operator=(const RegressionSection &) = delete;
  };

  inline void check(const char *what, bool ok)
  {
    const int kind = gRegression > 0 ? 1 : 0;
    ++gChecks[kind];
    if (!ok) {
      ++gFailures[kind];
      std::printf("suiteHandleChecks: %s%s: FAILED\n", kind ? "regression: " : "", what);
    }
  }
  inline void check(const std::string &what, bool ok) { check(what.c_str(), ok); }

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

  class TestSetInstance : public Param::SetInstance {
    Property::Set _props;
  public:
    virtual Property::Set &getParamSetProps() { return _props; }
    virtual void paramChangedByPlugin(Param::Instance *) {}
    virtual Param::Instance *newParam(const std::string &, Param::Descriptor &d) { return new TestInteger(d, this); }
    virtual OfxStatus editBegin(const std::string &) { return kOfxStatOK; }
    virtual OfxStatus editEnd() { return kOfxStatOK; }
  };

  /// the GPU properties of the inArgs of an action, as setGPURenderArgs() (ofxhImageEffect.cpp)
  /// sets them for a GPURenderScope: -1 and NULL for a property that is absent
  struct GPUArgs {
    int openCL = -1;              ///< kOfxImageEffectPropOpenCLEnabled
    void *openCLQueue = nullptr;  ///< kOfxImageEffectPropOpenCLCommandQueue
    int metal = -1;               ///< kOfxImageEffectPropMetalEnabled
    int metalTextures = -1;       ///< kOfxImageEffectPropMetalTextureEnabled
    void *metalQueue = nullptr;   ///< kOfxImageEffectPropMetalCommandQueue
    int cuda = -1;                ///< kOfxImageEffectPropCudaEnabled
    bool hasStream = false;       ///< kOfxImageEffectPropCudaStream is set
    void *stream = nullptr;       ///< kOfxImageEffectPropCudaStream
    bool operator==(const GPUArgs &o) const
    {
      return openCL == o.openCL && openCLQueue == o.openCLQueue && metal == o.metal && metalTextures == o.metalTextures &&
        metalQueue == o.metalQueue && cuda == o.cuda && hasStream == o.hasStream && stream == o.stream;
    }
    bool operator!=(const GPUArgs &o) const { return !(*this == o); }
  };

  /// hostDemo's effect instance, with test hooks:
  /// - the statuses a host may throw (as Natron does): progressUpdate and progressEnd without an
  ///   ongoing progress throw kOfxStatErrBadHandle ("the progress handle was invalid");
  ///   timeLineGotoTime throws kOfxStatErrValue for |t| > 1e9 (a time the host can't go to), and
  ///   kOfxStatErrFatal for t == 12345 (any other status);
  /// - the inArgs of each action, recorded host-side (setCustomInArgs(), just before the action is
  ///   passed to the plug-in): kOfxImageEffectPropThumbnailRender ("" if absent),
  ///   kOfxImageEffectPropBehaviourWhenUnlicensed ("absent" if absent), and the GPU properties
  ///   (gpuArgs);
  /// - what getBehaviourWhenUnlicensed() and abort() return: unlicensedBehaviour and abortValue;
  /// - a write to the output clip's depth in the GetClipPreferences outArgs, through the property
  ///   suite as a plug-in would, just before the plug-in's action: outputDepthWrite (none if empty),
  ///   with its status in outputDepthWriteStatus;
  /// - the kOfxActionSyncPrivateData actions sent to the plug-in: syncs
  class TestEffectInstance : public MyHost::MyEffectInstance {
    bool ongoing() const { return !_progress.empty() && _progress.back().ends == 0; }
  public:
    std::map<std::string, std::string> thumbnailArg;
    std::map<std::string, std::string> unlicensedArg;
    std::map<std::string, GPUArgs> gpuArgs;
    std::string unlicensedBehaviour;
    int abortValue; ///< what abort() returns; -1: it throws
    std::string outputDepthWrite;
    OfxStatus outputDepthWriteStatus;
    int syncs;
    TestEffectInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context), abortValue(0), outputDepthWriteStatus(kOfxStatOK), syncs(0) {}
    /// a copy, as a host's render clone (Natron's OfxEffectInstance::createRenderClone())
    TestEffectInstance(const TestEffectInstance &) = default;
    virtual OfxStatus mainEntry(const char *action, const void *handle, Property::Set *inArgs, Property::Set *outArgs)
    {
      if (action && std::string(action) == kOfxActionSyncPrivateData) {
        ++syncs;
      }
      return MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
    }
    virtual void setCustomOutArgs(const std::string &action, Property::Set &outArgs)
    {
      MyHost::MyEffectInstance::setCustomOutArgs(action, outArgs);
      if (action == kOfxImageEffectActionGetClipPreferences && !outputDepthWrite.empty()) {
        const OfxPropertySuiteV1 *ps = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
        outputDepthWriteStatus = ps->propSetString(outArgs.getHandle(), "OfxImageClipPropDepth_Output", 0, outputDepthWrite.c_str());
      }
    }
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
      auto intArg = [&](const char *name) { return inArgs.fetchProperty(name) ? inArgs.getIntProperty(name) : -1; };
      auto pointerArg = [&](const char *name) { return inArgs.fetchProperty(name) ? inArgs.getPointerProperty(name) : nullptr; };
      GPUArgs &a = gpuArgs[action];
      a.openCL = intArg(kOfxImageEffectPropOpenCLEnabled);
      a.openCLQueue = pointerArg(kOfxImageEffectPropOpenCLCommandQueue);
      a.metal = intArg(kOfxImageEffectPropMetalEnabled);
      a.metalTextures = intArg(kOfxImageEffectPropMetalTextureEnabled);
      a.metalQueue = pointerArg(kOfxImageEffectPropMetalCommandQueue);
      a.cuda = intArg(kOfxImageEffectPropCudaEnabled);
      a.hasStream = inArgs.fetchProperty(kOfxImageEffectPropCudaStream) != nullptr;
      a.stream = pointerArg(kOfxImageEffectPropCudaStream);
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

  /// BeginSequenceRender, then EndSequenceRender, of the instance. No Render is called in between,
  /// unlike the \pre of EndSequenceRender ("kOfxImageEffectActionRender action was called at least
  /// once on the instance"): harmless with Invert, which does nothing in Begin/End, and enough to
  /// check their inArgs
  inline void beginEndSequenceRender(ImageEffect::Instance *instance)
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
  inline void render(ImageEffect::Instance *instance)
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

  /// the plug-in cache the singleton writes, its number of plug-ins with one identifier, and the
  /// bundle of that plug-in's binary (its directory name after the path separator:
  /// example-Invert-support.ofx.bundle with CMake, invert.ofx.bundle with the Makefiles)
  struct CacheSnapshot {
    std::string xml;
    int nInvert = 0;
    std::string bundle;
  };

  /// the regression checks (regression/suiteHandleRegressions.cpp), called by main() and by
  /// checkEffectInstance() where their state is ready
  namespace Regression {
    void propGetDimension(const OfxPropertySuiteV1 *propSuite, OfxPropertySetHandle paramDescriptor);
    void bytesDefault(const OfxPropertySuiteV1 *propSuite, TestSetDescriptor &setDesc, TestSetInstance &setInst);
    void abort(MyHost::Host &host, OfxImageEffectHandle descriptor, TestEffectInstance *instance);
    void badAlloc(MyHost::Host &host);
    void memoryFree(MyHost::Host &host);
    void hostDescriptor(MyHost::Host &host);
    void propertyValues();
    void setCopyThrows();
    void effectLabels();
    void pluginHostPath();
    CacheSnapshot pluginCacheRescanAndRead(MyHost::Host &host, ImageEffect::PluginCache &imageEffectPluginCache,
                                           ImageEffect::Instance &instance, const std::string &pluginId);
    void clips(ImageEffect::Instance *instance);
    void changedBinary(MyHost::Host &host, const CacheSnapshot &snapshot, const std::string &pluginId);
    void cachedInvalidBinary(MyHost::Host &host, const CacheSnapshot &snapshot);
    void scanThrows(MyHost::Host &host);
  }

} // SuiteHandleChecks

#endif // SUITE_HANDLE_CHECKS_H
