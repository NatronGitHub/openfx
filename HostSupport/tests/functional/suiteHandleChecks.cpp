// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// suiteHandleChecks: unit checks of HostSupport on hostDemo's host classes. This file has the
/// functional checks: the contracts of the OFX suites (their handle and argument checks, the
/// statuses of the spec) and of HostSupport's API (property and param behaviour, the image effect
/// inArgs, GPURenderScope). The regression checks, each for a fixed bug, are in
/// ../regression/suiteHandleRegressions.cpp, linked into the same program, and print
/// "suiteHandleChecks: regression: ..." when they fail. Each check*() function says what it checks,
/// why, and its issue, PR or spec section.
/// The checks of an effect instance need Support/Plugins/Invert in OFX_PLUGIN_PATH; without it
/// they are skipped, unless OFXH_REQUIRE=1 (CI), which makes them fail.
/// Run by .github/scripts/hostdemo-checks.sh and by .github/workflows/build.yml (Windows, and the
/// extensions build); exits with 1 if a check fails.

#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

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
#include "ofxGPURender.h"
#include <sstream>
#include <stdexcept>

#include "suiteHandleChecks.h"

using namespace OFX::Host;
using namespace SuiteHandleChecks;

namespace {

  class TestChoice : public Param::ChoiceInstance {
  public:
    TestChoice(Param::Descriptor &d, Param::SetInstance *s) : Param::ChoiceInstance(d, s) {}
    virtual OfxStatus get(int &v) { v = 0; return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, int &v) { v = 0; return kOfxStatOK; }
    virtual OfxStatus set(int) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, int) { return kOfxStatOK; }
  };

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

  /// the interact suite (#192, 27aaa658): NULL and descriptor handles are kOfxStatErrBadHandle,
  /// NULL outputs; the DrawSuite: NULL, descriptor and instance handles, outside and inside
  /// kOfxInteractActionDraw (ofxDrawSuite.h: "kOfxStatFailed - failure, e.g. if function is called
  /// outside kOfxInteractActionDraw")
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

  /// a host that doesn't implement kOfxParamPropColourManagement (the HostSupport default; hostDemo,
  /// and so TestHost, opt in)
  class NoParamColourManagementHost : public TestHost {
  public:
    virtual bool supportsParamColourManagement() const { return false; }
  };

  /// kOfxParamPropChoiceOrder as the plug-in sees it (ChoiceOrderHook)
  std::vector<int> choiceOrder(const OfxPropertySuiteV1 *ps, OfxPropertySetHandle h)
  {
    int n = -1;
    ps->propGetDimension(h, kOfxParamPropChoiceOrder, &n);
    std::vector<int> order(n > 0 ? n : 0);
    for (int i = 0; i < n; ++i) {
      ps->propGetInt(h, kOfxParamPropChoiceOrder, i, &order[i]);
    }
    return order;
  }

  /// the spec's kOfxParamPropChoiceOrder (#357, 86a71041): the options' length, an index for each
  /// entry nobody set; partial orders completed, options appended or cut, propReset
  void checkChoiceOrder(const OfxPropertySuiteV1 *ps)
  {
    typedef std::vector<int> Order;
    const char *const options[] = { "P", "Q", "R", "S" };
    // a descriptor: the default, then a partial order set value by value (completed with the index)
    Param::Descriptor natural(kOfxParamTypeChoice, "natural");
    natural.addStandardParamProps(kOfxParamTypeChoice);
    OfxPropertySetHandle h = natural.getPropHandle();
    for (int i = 0; i < 3; ++i) {
      ps->propSetString(h, kOfxParamPropChoiceOption, i, options[i]);
    }
    const int ordinal[] = { 0, 1, 2 }, partial[] = { 5, 1, 2 };
    check("choice order: the default is 0..n-1", choiceOrder(ps, h) == Order(ordinal, ordinal + 3));
    Param::Descriptor part(kOfxParamTypeChoice, "partial");
    part.addStandardParamProps(kOfxParamTypeChoice);
    h = part.getPropHandle();
    for (int i = 0; i < 3; ++i) {
      ps->propSetString(h, kOfxParamPropChoiceOption, i, options[i]);
    }
    ps->propSetInt(h, kOfxParamPropChoiceOrder, 0, 5);
    ps->propSetInt(h, kOfxParamPropChoiceOrder, 1, 1);
    check("choice order: a partial order is completed with the indexes", choiceOrder(ps, h) == Order(partial, partial + 3));
    // A and B appended without an order, then C with the order 0: the duplicate is the plug-in's
    Param::Descriptor mixed(kOfxParamTypeChoice, "mixed");
    mixed.addStandardParamProps(kOfxParamTypeChoice);
    h = mixed.getPropHandle();
    for (int i = 0; i < 3; ++i) {
      ps->propSetString(h, kOfxParamPropChoiceOption, i, options[i]);
    }
    ps->propSetInt(h, kOfxParamPropChoiceOrder, 2, 0);
    const int dup[] = { 0, 1, 0 };
    check("choice order: the plug-in's values are kept", choiceOrder(ps, h) == Order(dup, dup + 3));
    // an instance: its descriptor's values; an option appended gets its index
    Param::Descriptor ordered(kOfxParamTypeChoice, "ordered");
    ordered.addStandardParamProps(kOfxParamTypeChoice);
    h = ordered.getPropHandle();
    for (int i = 0; i < 4; ++i) {
      ps->propSetString(h, kOfxParamPropChoiceOption, i, options[i]);
    }
    const int explicitOrder[] = { 0, 10, 30, 20 }, appended[] = { 0, 10, 30, 20, 4 };
    ps->propSetIntN(h, kOfxParamPropChoiceOrder, 4, explicitOrder);
    TestChoice inst(ordered, NULL);
    OfxPropertySetHandle ih = inst.getPropHandle();
    check("choice order: the instance has the descriptor's", choiceOrder(ps, ih) == Order(explicitOrder, explicitOrder + 4));
    ps->propSetString(ih, kOfxParamPropChoiceOption, 4, "Late");
    check("choice order: an appended option gets its index", choiceOrder(ps, ih) == Order(appended, appended + 5));
    ps->propSetInt(ih, kOfxParamPropChoiceOrder, 4, 15);
    check("choice order: the plug-in's order of the appended option", choiceOrder(ps, ih)[4] == 15);
    // options that shrink without a reset (as a host may set them) cut the order
    const char *const two[] = { "A", "B" };
    const int cut[] = { 0, 10 }, cutAppended[] = { 0, 10, 2 };
    ps->propSetStringN(ih, kOfxParamPropChoiceOption, 2, two);
    check("choice order: cut when the options shrink", choiceOrder(ps, ih) == Order(cut, cut + 2));
    ps->propSetString(ih, kOfxParamPropChoiceOption, 2, "C");
    check("choice order: then an appended option gets its index", choiceOrder(ps, ih) == Order(cutAppended, cutAppended + 3));
    // a value set on a default instance changes that value only
    TestChoice natInst(natural, NULL);
    ih = natInst.getPropHandle();
    ps->propSetInt(ih, kOfxParamPropChoiceOrder, 0, 5);
    check("choice order: one value set on the default", choiceOrder(ps, ih) == Order(partial, partial + 3));
    // resetting the order or the options goes back to the default
    ps->propReset(ih, kOfxParamPropChoiceOrder);
    check("choice order: reset of the order", choiceOrder(ps, ih) == Order(ordinal, ordinal + 3));
    ps->propSetInt(ih, kOfxParamPropChoiceOrder, 0, 5);
    ps->propReset(ih, kOfxParamPropChoiceOption);
    check("choice order: reset with the options", choiceOrder(ps, ih).empty());
    ps->propSetString(ih, kOfxParamPropChoiceOption, 0, "A");
    check("choice order: then the default again", choiceOrder(ps, ih) == Order(1, 0));
  }

  /// the inArgs of the sequence render and render actions (#203, #209): kOfxImageEffectPropThumbnailRender
  /// and kOfxImageEffectPropBehaviourWhenUnlicensed (the spec's text is quoted below)
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

  /// a copy of an instance (a host's render clone) reports the host's values of the hooked
  /// instance properties, not their stored defaults (0), finds the descriptor's properties through
  /// the chain, and starts with a NULL kOfxPropInstanceData (ofxCore.h: "This data pointer is
  /// unique to each plug-in instance"); #384
  void checkInstanceCopy(TestEffectInstance &instance)
  {
    const OfxPropertySuiteV1 *ps = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
    Property::Set &descProps = instance.getDescriptor().getProps();
    descProps.createProperty({ "TestDescriptorOnly", Property::eString, 1, false, "descriptor" });
    void *data = nullptr; // (set by the plug-in in CreateInstance: Support's ImageEffect)
    check("instance: kOfxPropInstanceData is set",
          ps->propGetPointer(instance.getProps().getHandle(), kOfxPropInstanceData, 0, &data) == kOfxStatOK && data != nullptr);
    {
      TestEffectInstance copy(instance);
      OfxPropertySetHandle h = copy.getProps().getHandle();
      double size[2] = { 0., 0. }, offset[2] = { -1., -1. }, extent[2] = { 0., 0. };
      double par = 0., duration = 0., rate = 0.;
      double xs = 0., ys = 0., xo = 0., yo = 0., xe = 0., ye = 0.;
      instance.getProjectSize(xs, ys);
      instance.getProjectOffset(xo, yo);
      instance.getProjectExtent(xe, ye);
      check("instance copy: kOfxImageEffectPropProjectSize, Offset and Extent are the host's",
            ps->propGetDoubleN(h, kOfxImageEffectPropProjectSize, 2, size) == kOfxStatOK &&
            ps->propGetDoubleN(h, kOfxImageEffectPropProjectOffset, 2, offset) == kOfxStatOK &&
            ps->propGetDoubleN(h, kOfxImageEffectPropProjectExtent, 2, extent) == kOfxStatOK &&
            xs > 0. && size[0] == xs && size[1] == ys && offset[0] == xo && offset[1] == yo &&
            extent[0] == xe && extent[1] == ye);
      check("instance copy: kOfxImageEffectPropProjectPixelAspectRatio, kOfxImageEffectInstancePropEffectDuration "
            "and kOfxImageEffectPropFrameRate are the host's",
            ps->propGetDouble(h, kOfxImageEffectPropProjectPixelAspectRatio, 0, &par) == kOfxStatOK &&
            ps->propGetDouble(h, kOfxImageEffectInstancePropEffectDuration, 0, &duration) == kOfxStatOK &&
            ps->propGetDouble(h, kOfxImageEffectPropFrameRate, 0, &rate) == kOfxStatOK &&
            par > 0. && par == instance.getProjectPixelAspectRatio() &&
            duration == instance.getEffectDuration() && rate > 0. && rate == instance.getFrameRate());
      const char *value = nullptr;
      check("instance copy: a descriptor property through the chained set",
            ps->propGetString(h, "TestDescriptorOnly", 0, &value) == kOfxStatOK && value && std::string(value) == "descriptor");
      void *copyData = data;
      check("instance copy: kOfxPropInstanceData is NULL",
            ps->propGetPointer(h, kOfxPropInstanceData, 0, &copyData) == kOfxStatOK && copyData == nullptr);
      void *mainData = nullptr;
      check("instance: kOfxPropInstanceData unchanged by the copy",
            ps->propGetPointer(instance.getProps().getHandle(), kOfxPropInstanceData, 0, &mainData) == kOfxStatOK && mainData == data);
    }
    descProps.removeProperty("TestDescriptorOnly");
  }

  /// the output depth: the plug-in may set it only if the host and the plug-in both support
  /// multiple clip depths (ofxClipPreferences.rst); otherwise the host's depth stays, even when the
  /// ePluginReadOnlyWarn policy accepts the plug-in's write (Invert doesn't support multiple clip
  /// depths, and supports 8 bits, 16 bits and float); #384
  void checkClipPreferencesDepths(MyHost::Host &host, TestEffectInstance &instance)
  {
    ImageEffect::ClipInstance *output = instance.getClip(kOfxImageEffectOutputClipName);
    check("output clip", output != nullptr);
    if (!output) {
      return;
    }
    instance.getClipPreferences();
    const std::string hostDepth = output->getPixelDepth();
    const std::string other = hostDepth == kOfxBitDepthFloat ? kOfxBitDepthByte : kOfxBitDepthFloat;
    const Property::PluginReadOnlyPolicyEnum policy = Property::getPluginReadOnlyPolicy();
    Property::setPluginReadOnlyPolicy(Property::ePluginReadOnlyWarn);
    instance.outputDepthWrite = other;
    const bool prefs = instance.getClipPreferences();
    check(("GetClipPreferences, no multiple clip depths, ePluginReadOnlyWarn: the write of the output depth " + other +
           " is accepted, and the output depth stays " + hostDepth).c_str(),
          prefs && instance.outputDepthWriteStatus == kOfxStatOK && output->getPixelDepth() == hostDepth);
    Property::Set &hostProps = host.getProperties();
    const int hostMultiple = hostProps.getIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths);
    const int pluginMultiple = instance.getProps().getIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths);
    hostProps.setIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths, 1);
    check("GetClipPreferences, the host supports multiple clip depths, not the plug-in: the output depth stays",
          instance.getClipPreferences() && output->getPixelDepth() == hostDepth);
    instance.getProps().setIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths, 1);
    check(("GetClipPreferences, both support multiple clip depths: the plug-in's output depth " + other).c_str(),
          instance.getClipPreferences() && instance.outputDepthWriteStatus == kOfxStatOK && output->getPixelDepth() == other);
    instance.outputDepthWrite.clear();
    instance.getProps().setIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths, pluginMultiple);
    hostProps.setIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths, hostMultiple);
    Property::setPluginReadOnlyPolicy(policy);
    instance.getClipPreferences();
  }

  typedef ImageEffect::GPURenderScope::API API;

  /// what setGPURenderArgs() (ofxhImageEffect.cpp) puts in the inArgs of BeginSequenceRender,
  /// Render and EndSequenceRender in a GPURenderScope of api with queue (ofxhImageEffect.h): the
  /// enabled property of api 1 and its queue, the other APIs' 0 and NULL, all 0 and NULL for API
  /// CPU (ofxGPURender.h: the host sets kOfxImageEffectPropOpenCLEnabled, MetalEnabled,
  /// MetalTextureEnabled or CudaEnabled "to indicate that it is passing images as" OpenCL, Metal
  /// or CUDA memory; "For a given action the host MUST set at most one of the two enabled
  /// properties" of Metal); kOfxImageEffectPropCudaStream "will only be set if the host and
  /// plug-in both support CUDA streams" (streams), and only with a queue
  GPUArgs expectedGPUArgs(API api, void *queue, bool streams)
  {
    GPUArgs e;
    e.openCL = api == API::OpenCL ? 1 : 0;
    e.openCLQueue = api == API::OpenCL ? queue : nullptr;
    e.metal = api == API::Metal ? 1 : 0;
    e.metalTextures = api == API::MetalTextures ? 1 : 0;
    e.metalQueue = api == API::Metal || api == API::MetalTextures ? queue : nullptr;
    e.cuda = api == API::CUDA ? 1 : 0;
    e.hasStream = api == API::CUDA && queue && streams;
    e.stream = e.hasStream ? queue : nullptr;
    return e;
  }

  /// the inArgs of no GPURenderScope: all 0 and NULL, no kOfxImageEffectPropCudaStream
  GPUArgs noGPUArgs() { return expectedGPUArgs(API::CPU, nullptr, false); }

  /// Begin/EndSequenceRender and Render of instance: the GPU properties of their inArgs are expected
  bool gpuArgsAre(TestEffectInstance &instance, const GPUArgs &expected)
  {
    instance.gpuArgs.clear();
    beginEndSequenceRender(&instance);
    render(&instance);
    bool ok = true;
    for (const char *a : { kOfxImageEffectActionBeginSequenceRender, kOfxImageEffectActionRender,
                           kOfxImageEffectActionEndSequenceRender }) {
      ok = ok && instance.gpuArgs.count(a) && instance.gpuArgs[a] == expected;
    }
    return ok;
  }

  /// The GPU render properties (#387), "plug-in descriptor (read/write) [...] plug-in instance change
  /// (read/write)" (ofxGPURender.h): "An instance inherits the value set on the plug-in
  /// descriptor until the plug-in sets this property on the instance; a host must not substitute
  /// its own default at the instance level" (upstream main). An instance of a descriptor that set
  /// a non-default value reads it, whatever the host's own value; the plug-in's set on the instance
  /// changes the instance only.
  void checkGPUPropertyInheritance(MyHost::Host &host, ImageEffect::ImageEffectPlugin *plugin)
  {
    const OfxPropertySuiteV1 *ps = (const OfxPropertySuiteV1 *)host.fetchSuite(kOfxPropertySuite, 1);
    const OfxImageEffectSuiteV1 *es = (const OfxImageEffectSuiteV1 *)host.fetchSuite(kOfxImageEffectSuite, 1);
    ImageEffect::Descriptor *desc = plugin->getContext(kOfxImageEffectContextFilter);
    check("GPU properties: the property and image effect suites, the filter descriptor", ps && es && desc);
    if (!ps || !es || !desc) {
      return;
    }
    // (name, a value other than the descriptor default, another value the plug-in may set)
    const struct { const char *name, *described, *set; } props[] = {
      { kOfxImageEffectPropOpenGLRenderSupported, "needed", "false" },
      { kOfxImageEffectPropOpenCLRenderSupported, "true", "false" },
      { kOfxImageEffectPropOpenCLSupported, "true", "false" },
      { kOfxImageEffectPropCudaRenderSupported, "true", "false" },
      { kOfxImageEffectPropCudaStreamSupported, "true", "false" },
      { kOfxImageEffectPropMetalRenderSupported, "true", "false" },
      { kOfxImageEffectPropCPURenderSupported, "false", "true" },
    };
    Property::Set &dp = desc->getProps();
    Property::Set &hp = host.getProperties();
    std::vector<std::string> savedDescriptor, savedHost;
    for (const auto &p : props) {
      savedDescriptor.push_back(dp.getStringProperty(p.name));
      savedHost.push_back(hp.getStringProperty(p.name));
      dp.setStringProperty(p.name, p.described);
      // the host's own value differs from the descriptor's: the instance must not take it
      hp.setStringProperty(p.name, std::string(p.described) == "false" ? "true" : "false");
    }
    ImageEffect::Instance *instance = plugin->createInstance(kOfxImageEffectContextFilter, NULL);
    check("GPU properties: an instance", instance != NULL);
    if (instance) {
      OfxPropertySetHandle ih = NULL;
      check("GPU properties: getPropertySet(instance)", es->getPropertySet(instance->getHandle(), &ih) == kOfxStatOK && ih);
      for (const auto &p : props) {
        const char *v = NULL;
        const bool inherited = ih && ps->propGetString(ih, p.name, 0, &v) == kOfxStatOK && v && std::string(v) == p.described;
        check((std::string("an instance reads the descriptor's ") + p.name + " (\"" + p.described + "\"), not the host's").c_str(),
              inherited);
        const bool set = ih && ps->propSetString(ih, p.name, 0, p.set) == kOfxStatOK &&
          ps->propGetString(ih, p.name, 0, &v) == kOfxStatOK && v && std::string(v) == p.set &&
          dp.getStringProperty(p.name) == p.described;
        check((std::string("the plug-in sets ") + p.name + " on the instance: the instance's only").c_str(), set);
      }
      instance->destroyInstanceAction();
    }
    delete instance;
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); ++i) {
      dp.setStringProperty(props[i].name, savedDescriptor[i]);
      hp.setStringProperty(props[i].name, savedHost[i]);
    }
  }

  /// ImageEffect::GPURenderScope (ofxhImageEffect.h; #370, #379, #388, parts of #361), for every
  /// API, on Invert, without a device: the GPU properties of the inArgs of BeginSequenceRender,
  /// Render and EndSequenceRender are those of the scope's API (expectedGPUArgs()); a scope
  /// applies to its own instance only ("Other instances' actions [...] are left alone"); scopes
  /// nest and the innermost one applies (a nested scope of each other API, and of API CPU: "as
  /// without a scope"); the outer scope is back after a nested one; none is left after them, and
  /// without a scope the inArgs are all 0 and NULL. One table: the scope code is the same for
  /// every API, and runs here on every CI OS (these rows replaced the per-API blocks of
  /// openclRenderChecks, metalRenderChecks and cudaRenderChecks, which ran only with a device or
  /// a fake driver). Both the host and the plug-in support CUDA streams here; checkCudaStreamArg()
  /// has the other cases.
  void checkGPURenderScope(MyHost::Host &host, TestEffectInstance &instance, ImageEffect::ImageEffectPlugin &plugin)
  {
    static const struct { API api; const char *name; } apis[] = {
      { API::CPU, "CPU" }, { API::OpenCL, "OpenCL" }, { API::Metal, "Metal" }, { API::MetalTextures, "MetalTextures" },
      { API::CUDA, "CUDA" },
    };
    Property::Set &hostProps = host.getProperties();
    Property::Set &pluginProps = instance.getProps();
    const std::string hostStreams = hostProps.getStringProperty(kOfxImageEffectPropCudaStreamSupported);
    const std::string pluginStreams = pluginProps.getStringProperty(kOfxImageEffectPropCudaStreamSupported);
    hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
    pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
    const std::unique_ptr<ImageEffect::Instance> otherInstance(plugin.createInstance(kOfxImageEffectContextFilter, NULL));
    TestEffectInstance *other = dynamic_cast<TestEffectInstance *>(otherInstance.get());
    check("GPURenderScope: another instance of the plug-in", other != nullptr);
    if (other) {
      const OfxStatus st = other->createInstanceAction();
      check("GPURenderScope: createInstanceAction of the other instance", st == kOfxStatOK || st == kOfxStatReplyDefault);
    }
    check("GPURenderScope: no scope: all 0 and NULL, no kOfxImageEffectPropCudaStream", gpuArgsAre(instance, noGPUArgs()));
    for (const auto &a : apis) {
      const std::string what = std::string("GPURenderScope of API ") + a.name;
      int queue = 0, otherQueue = 0;
      if (other) {
        ImageEffect::GPURenderScope scope(*other, a.api, &queue);
        check(what + " of another instance: this instance's inArgs all 0 and NULL", gpuArgsAre(instance, noGPUArgs()));
      }
      {
        ImageEffect::GPURenderScope outer(instance, a.api, &queue);
        check(what + (a.api == API::CPU ? ": all 0 and NULL, as without a scope" :
                      a.api == API::CUDA ? ": its enabled property 1 and its queue, the other APIs' 0 and NULL, "
                                           "kOfxImageEffectPropCudaStream the queue" :
                                           ": its enabled property 1 and its queue, the other APIs' 0 and NULL"),
              gpuArgsAre(instance, expectedGPUArgs(a.api, &queue, true)));
        for (const auto &b : apis) {
          if (b.api == a.api) {
            continue;
          }
          {
            ImageEffect::GPURenderScope inner(instance, b.api, &otherQueue);
            check(what + ", a nested scope of API " + b.name + ": the nested scope's inArgs",
                  gpuArgsAre(instance, expectedGPUArgs(b.api, &otherQueue, true)));
          }
          check(what + ", after the nested scope of API " + b.name + ": the outer scope again",
                ImageEffect::GPURenderScope::find(instance) == &outer && outer.api() == a.api &&
                outer.queue() == (a.api == API::CPU ? nullptr : &queue) &&
                gpuArgsAre(instance, expectedGPUArgs(a.api, &queue, true)));
        }
      }
      check(what + ": no scope left, all 0 and NULL",
            ImageEffect::GPURenderScope::find(instance) == nullptr && gpuArgsAre(instance, noGPUArgs()));
    }
    {
      // kOfxImageEffectPropCudaStream: "If not set: the plug-in SHOULD ensure that any
      // asynchronous operations it enqueues have completed before returning" (a scope without one)
      ImageEffect::GPURenderScope noStream(instance, API::CUDA, nullptr);
      check("GPURenderScope of API CUDA without a queue: kOfxImageEffectPropCudaEnabled 1, no kOfxImageEffectPropCudaStream",
            gpuArgsAre(instance, expectedGPUArgs(API::CUDA, nullptr, true)));
    }
    if (other) {
      other->destroyInstanceAction();
    }
    hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, hostStreams);
    pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, pluginStreams);
  }

  /// kOfxImageEffectPropCudaStream (#384; ofxGPURender.h): "This property will only be set if the
  /// host and plug-in both support CUDA streams", the plug-in's "needed" (upstream main's
  /// @propdef) included; and a nested CUDA scope without a stream. (The scopes of the other APIs,
  /// nested or not, and no scope: checkGPURenderScope().)
  void checkCudaStreamArg(MyHost::Host &host, TestEffectInstance &instance)
  {
    Property::Set &hostProps = host.getProperties();
    Property::Set &pluginProps = instance.getProps();
    const std::string hostStreams = hostProps.getStringProperty(kOfxImageEffectPropCudaStreamSupported);
    const std::string pluginStreams = pluginProps.getStringProperty(kOfxImageEffectPropCudaStreamSupported);
    int streamTag = 0;
    {
      ImageEffect::GPURenderScope scope(instance, API::CUDA, &streamTag);
      hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "false");
      pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
      check("CUDA scope, the host without CUDA streams: kOfxImageEffectPropCudaEnabled 1, no kOfxImageEffectPropCudaStream",
            gpuArgsAre(instance, expectedGPUArgs(API::CUDA, &streamTag, false)));
      hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
      pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "false");
      check("CUDA scope, the plug-in without CUDA streams: no kOfxImageEffectPropCudaStream",
            gpuArgsAre(instance, expectedGPUArgs(API::CUDA, &streamTag, false)));
      pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "needed");
      check("CUDA scope, the host supports CUDA streams, the plug-in says \"needed\" (upstream main's @propdef): "
            "kOfxImageEffectPropCudaStream is the scope's stream", gpuArgsAre(instance, expectedGPUArgs(API::CUDA, &streamTag, true)));
      pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
      check("CUDA scope, both support CUDA streams: kOfxImageEffectPropCudaStream is the scope's stream",
            gpuArgsAre(instance, expectedGPUArgs(API::CUDA, &streamTag, true)));
      {
        ImageEffect::GPURenderScope noStream(instance, API::CUDA, nullptr);
        check("a nested CUDA scope without a stream: kOfxImageEffectPropCudaEnabled 1, no kOfxImageEffectPropCudaStream",
              gpuArgsAre(instance, expectedGPUArgs(API::CUDA, nullptr, true)));
      }
    }
    hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, hostStreams);
    pluginProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, pluginStreams);
  }

  /// kOfxPropParamSetNeedsSyncing (ofxParam.h): "If this property is not set, the host will always
  /// call SyncPrivateData before copying or saving the effect (as if the property were set to 1 --
  /// but the host will not create or modify the property)": absent until the plug-in sets it; then
  /// 1 syncs and is set to 0, and 0 doesn't sync; copied to a render clone; only on an effect
  /// instance's param set; set by one thread while four others read (#384; the race half shows
  /// only under ThreadSanitizer, which CI doesn't run: without it, these checks see the values)
  void checkNeedsSyncing(TestEffectInstance &instance)
  {
    const OfxPropertySuiteV1 *ps = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
    OfxPropertySetHandle h = instance.getParamSetProps().getHandle();
    const size_t nProps = instance.getParamSetProps().getProperties().size();
    TestEffectInstance concurrent(instance); // (a copy taken while the property is hidden)
    int v = -1, dim = -1;
    check("kOfxPropParamSetNeedsSyncing: hidden at first (propGetInt, propGetDimension, propReset = kOfxStatErrUnknown)",
          ps->propGetInt(h, kOfxPropParamSetNeedsSyncing, 0, &v) == kOfxStatErrUnknown &&
          ps->propGetDimension(h, kOfxPropParamSetNeedsSyncing, &dim) == kOfxStatErrUnknown &&
          ps->propReset(h, kOfxPropParamSetNeedsSyncing) == kOfxStatErrUnknown);
    check("kOfxPropParamSetNeedsSyncing: declared with the instance (the property map doesn't change later)",
          instance.getParamSetProps().fetchIntProperty(kOfxPropParamSetNeedsSyncing) != nullptr);
    int syncs = instance.syncs;
    instance.syncPrivateDataAction();
    instance.syncPrivateDataAction();
    check("kOfxPropParamSetNeedsSyncing not set: SyncPrivateData every time, and the property stays hidden",
          instance.syncs == syncs + 2 && ps->propGetInt(h, kOfxPropParamSetNeedsSyncing, 0, &v) == kOfxStatErrUnknown);
    check("kOfxPropParamSetNeedsSyncing: propSetDouble, propSetInt(index 1), propSetIntN(count 2) = kOfxStatErrUnknown, still hidden",
          ps->propSetDouble(h, kOfxPropParamSetNeedsSyncing, 0, 1.) == kOfxStatErrUnknown &&
          ps->propSetInt(h, kOfxPropParamSetNeedsSyncing, 1, 1) == kOfxStatErrUnknown &&
          ps->propSetIntN(h, kOfxPropParamSetNeedsSyncing, 2, std::vector<int>(2, 1).data()) == kOfxStatErrUnknown &&
          ps->propGetInt(h, kOfxPropParamSetNeedsSyncing, 0, &v) == kOfxStatErrUnknown);
    check("kOfxPropParamSetNeedsSyncing: propSetInt(0) = kOfxStatOK, shows it, int X 1",
          ps->propSetInt(h, kOfxPropParamSetNeedsSyncing, 0, 0) == kOfxStatOK &&
          ps->propGetDimension(h, kOfxPropParamSetNeedsSyncing, &dim) == kOfxStatOK && dim == 1 &&
          ps->propGetInt(h, kOfxPropParamSetNeedsSyncing, 0, &v) == kOfxStatOK && v == 0);
    check("kOfxPropParamSetNeedsSyncing: the property map is unchanged",
          instance.getParamSetProps().getProperties().size() == nProps);
    syncs = instance.syncs;
    instance.syncPrivateDataAction();
    check("kOfxPropParamSetNeedsSyncing 0: no SyncPrivateData", instance.syncs == syncs);
    const int one = 1;
    check("kOfxPropParamSetNeedsSyncing: propSetIntN(1) = kOfxStatOK", ps->propSetIntN(h, kOfxPropParamSetNeedsSyncing, 1, &one) == kOfxStatOK);
    instance.syncPrivateDataAction();
    check("kOfxPropParamSetNeedsSyncing 1: one SyncPrivateData, then 0",
          instance.syncs == syncs + 1 && ps->propGetInt(h, kOfxPropParamSetNeedsSyncing, 0, &v) == kOfxStatOK && v == 0);
    instance.syncPrivateDataAction();
    check("kOfxPropParamSetNeedsSyncing 0 (set by the host after the sync): no SyncPrivateData", instance.syncs == syncs + 1);
    TestEffectInstance copy(instance);
    check("instance copy: kOfxPropParamSetNeedsSyncing copied, shown",
          ps->propGetInt(copy.getParamSetProps().getHandle(), kOfxPropParamSetNeedsSyncing, 0, &v) == kOfxStatOK && v == 0);
    // only on an effect instance's param set
    Property::Set other;
    check("kOfxPropParamSetNeedsSyncing on another property set: propSetInt = kOfxStatErrUnknown",
          ps->propSetInt(other.getHandle(), kOfxPropParamSetNeedsSyncing, 0, 1) == kOfxStatErrUnknown);
    check("kOfxPropParamSetNeedsSyncing on the effect descriptor: propSetInt = kOfxStatErrUnknown",
          ps->propSetInt(instance.getDescriptor().getParamSetProps().getHandle(), kOfxPropParamSetNeedsSyncing, 0, 1) == kOfxStatErrUnknown);

    // the plug-in sets it (e.g. in InstanceChanged) while render threads read the instance's
    // properties: the readers see it hidden, then 1, and the other properties throughout (a data
    // race on the property map would show under ThreadSanitizer)
    OfxPropertySetHandle ch = concurrent.getParamSetProps().getHandle();
    std::atomic<bool> stop(false);
    std::atomic<int> bad(0), hiddenReads(0), shownReads(0);
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&] {
        while (!stop.load()) {
          int value = -1;
          const char *context = nullptr;
          const OfxStatus st = ps->propGetInt(ch, kOfxPropParamSetNeedsSyncing, 0, &value);
          if (st == kOfxStatErrUnknown) {
            ++hiddenReads;
          } else if (st == kOfxStatOK && value == 1) {
            ++shownReads;
          } else {
            ++bad;
          }
          // (the host's own lookups too, of this property and of others, in the same map)
          if (ps->propGetString(ch, kOfxImageEffectPropContext, 0, &context) != kOfxStatOK || !context ||
              !concurrent.getParamSetProps().fetchProperty(kOfxPropIsInteractive) ||
              !concurrent.getParamSetProps().fetchProperty(kOfxPropParamSetNeedsSyncing)) {
            ++bad;
          }
        }
      });
    }
    while (hiddenReads.load() < 1000) {
      std::this_thread::yield();
    }
    // (one writer: two plug-in threads writing the same value while others read it would race on
    // the value, as for any property)
    std::atomic<int> writeFailures(0);
    std::thread writer([&] {
      if (ps->propSetInt(ch, kOfxPropParamSetNeedsSyncing, 0, 1) != kOfxStatOK) {
        ++writeFailures;
      }
    });
    writer.join();
    while (shownReads.load() < 1000) {
      std::this_thread::yield();
    }
    stop = true;
    for (auto &r : readers) {
      r.join();
    }
    check("kOfxPropParamSetNeedsSyncing set by a plug-in thread while four others read the set: the set is OK, "
          "the readers see it hidden then 1, and the other properties throughout",
          writeFailures == 0 && bad == 0 && hiddenReads >= 1000 && shownReads >= 1000);
  }

  /// the labels of an object, through the property suite (ofxCore.h, kOfxPropLabel: "Default -
  /// the ::kOfxPropName the object was created with", name, here the plug-in identifier for an
  /// effect descriptor; "Note that resetting this will also reset ::kOfxPropShortLabel and
  /// ::kOfxPropLongLabel"); #384. The C++ getters' fallbacks (#229) are Regression::effectLabels().
  void checkLabels(const OfxPropertySuiteV1 *ps, OfxPropertySetHandle h, const std::string &what, const std::string &name)
  {
    auto labels = [&](const std::string &label, const std::string &shortLabel, const std::string &longLabel) {
      const char *l = nullptr, *s = nullptr, *g = nullptr;
      return ps->propGetString(h, kOfxPropLabel, 0, &l) == kOfxStatOK && ps->propGetString(h, kOfxPropShortLabel, 0, &s) == kOfxStatOK &&
             ps->propGetString(h, kOfxPropLongLabel, 0, &g) == kOfxStatOK && l && s && g &&
             label == l && shortLabel == s && longLabel == g;
    };
    check((what + ": the labels default to " + name).c_str(), labels(name, name, name));
    check((what + ": propSetString(kOfxPropLabel): the short and long labels follow").c_str(),
          ps->propSetString(h, kOfxPropLabel, 0, "label") == kOfxStatOK && labels("label", "label", "label"));
    check((what + ": then the short and long labels: kept").c_str(),
          ps->propSetString(h, kOfxPropShortLabel, 0, "short") == kOfxStatOK &&
          ps->propSetString(h, kOfxPropLongLabel, 0, "long") == kOfxStatOK && labels("label", "short", "long"));
    check((what + ": propSetString(kOfxPropLabel) again: resets the short and long labels").c_str(),
          ps->propSetString(h, kOfxPropLabel, 0, "label2") == kOfxStatOK && labels("label2", "label2", "label2"));
    const char *const oneLabel[] = { "label3" };
    check((what + ": propSetStringN(kOfxPropLabel): the same").c_str(),
          ps->propSetString(h, kOfxPropShortLabel, 0, "short") == kOfxStatOK &&
          ps->propSetStringN(h, kOfxPropLabel, 1, oneLabel) == kOfxStatOK && labels("label3", "label3", "label3"));
    check((what + ": propReset(kOfxPropLabel): all back to " + name).c_str(),
          ps->propSetString(h, kOfxPropShortLabel, 0, "short") == kOfxStatOK &&
          ps->propReset(h, kOfxPropLabel) == kOfxStatOK && labels(name, name, name));
    // only the plug-in's writes: the host's own C++ set of the label leaves the short and long labels
    Property::Set *set = reinterpret_cast<Property::Set *>(h);
    set->setStringProperty(kOfxPropLabel, "host label");
    check((what + ": the host's setStringProperty(kOfxPropLabel): the short and long labels unchanged").c_str(),
          labels("host label", name, name));
    check((what + ": propReset(kOfxPropLabel) again").c_str(), ps->propReset(h, kOfxPropLabel) == kOfxStatOK && labels(name, name, name));
  }

  /// a Double2D instance that logs the host's setRange() and setDisplayRange() calls
  class TestDouble2D : public Param::Double2DInstance {
  public:
    std::string calls;
    TestDouble2D(Param::Descriptor &d, Param::SetInstance *s) : Param::Double2DInstance(d, s) {}
    virtual OfxStatus get(double &x, double &y) { x = y = 0.; return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, double &x, double &y) { return get(x, y); }
    virtual OfxStatus set(double, double) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, double, double) { return kOfxStatOK; }
    virtual void setRange() { calls += "range "; }
    virtual void setDisplayRange() { calls += "display "; }
  };

  /// the labels of clip and param descriptors and of param instances (see checkLabels()), and the
  /// display range of numeric params (ofxParam.h, kOfxParamPropMin: "Setting this will also reset
  /// ::kOfxParamPropDisplayMin"; Max: DisplayMax): it takes the new Min (Max), per dimension; the
  /// host's own writes don't (#384)
  void checkLinkedProperties(const OfxPropertySuiteV1 *ps, TestSetDescriptor &setDesc, TestSetInstance &setInst)
  {
    {
      ImageEffect::ClipDescriptor clip("Source");
      checkLabels(ps, clip.getPropHandle(), "clip descriptor", "Source");
    }
    Param::Descriptor *d2 = setDesc.paramDefine(kOfxParamTypeDouble2D, "d2");
    check("paramDefine(Double2D)", d2 != nullptr);
    if (!d2) {
      return;
    }
    checkLabels(ps, d2->getPropHandle(), "param descriptor", "d2");
    OfxPropertySetHandle h = d2->getPropHandle();
    auto display = [&](double min0, double min1, double max0, double max1) {
      double mins[2] = { 0., 0. }, maxs[2] = { 0., 0. };
      return ps->propGetDoubleN(h, kOfxParamPropDisplayMin, 2, mins) == kOfxStatOK &&
             ps->propGetDoubleN(h, kOfxParamPropDisplayMax, 2, maxs) == kOfxStatOK &&
             mins[0] == min0 && mins[1] == min1 && maxs[0] == max0 && maxs[1] == max1;
    };
    const double dmin[2] = { 0., 0. }, dmax[2] = { 10., 10. };
    check("Double2D descriptor: propSetDoubleN(display range) = kOfxStatOK",
          ps->propSetDoubleN(h, kOfxParamPropDisplayMin, 2, dmin) == kOfxStatOK &&
          ps->propSetDoubleN(h, kOfxParamPropDisplayMax, 2, dmax) == kOfxStatOK && display(0., 0., 10., 10.));
    const double rmin[2] = { -5., -6. }, rmax[2] = { 5., 6. };
    check("Double2D descriptor: propSetDoubleN(kOfxParamPropMin, Max): the display range is the range",
          ps->propSetDoubleN(h, kOfxParamPropMin, 2, rmin) == kOfxStatOK &&
          ps->propSetDoubleN(h, kOfxParamPropMax, 2, rmax) == kOfxStatOK && display(-5., -6., 5., 6.));
    check("Double2D descriptor: propSetDouble(kOfxParamPropMin, index 1): that index only",
          ps->propSetDouble(h, kOfxParamPropMin, 1, -1.) == kOfxStatOK && display(-5., -1., 5., 6.));
    check("Double2D descriptor: a display range set after the range is kept",
          ps->propSetDoubleN(h, kOfxParamPropDisplayMin, 2, dmin) == kOfxStatOK && display(0., 0., 5., 6.));
    const double maxDouble = std::numeric_limits<double>::max();
    check("Double2D descriptor: propReset(kOfxParamPropMax): the display max is the default max",
          ps->propReset(h, kOfxParamPropMax) == kOfxStatOK && display(0., 0., maxDouble, maxDouble));
    d2->getProperties().setDoubleProperty(kOfxParamPropMin, -7., 0);
    check("Double2D descriptor: the host's setDoubleProperty(kOfxParamPropMin): the display min unchanged",
          d2->getProperties().getDoubleProperty(kOfxParamPropMin, 0) == -7. && display(0., 0., maxDouble, maxDouble));
    check("Double2D descriptor: propSetDouble(kOfxParamPropMin) again: the display min follows",
          ps->propSetDouble(h, kOfxParamPropMin, 0, -5.) == kOfxStatOK && display(-5., 0., maxDouble, maxDouble));
    check("Double2D descriptor: propSetDouble(kOfxParamPropDisplayMin) doesn't change the min",
          ps->propSetDouble(h, kOfxParamPropDisplayMin, 0, 1.) == kOfxStatOK &&
          d2->getProperties().getDoubleProperty(kOfxParamPropMin, 0) == -5.);

    // an instance: the same, and the host is told of the range, then of the display range
    TestDouble2D inst(*d2, &setInst);
    OfxPropertySetHandle ih = inst.getPropHandle();
    checkLabels(ps, ih, "param instance", "d2");
    inst.calls.clear();
    check("Double2D instance: propSetDoubleN(kOfxParamPropMin): the display min follows; setRange(), then setDisplayRange()",
          ps->propSetDoubleN(ih, kOfxParamPropMin, 2, rmin) == kOfxStatOK && inst.calls == "range display " &&
          inst.getProperties().getDoubleProperty(kOfxParamPropDisplayMin, 0) == -5. &&
          inst.getProperties().getDoubleProperty(kOfxParamPropDisplayMin, 1) == -6.);
    inst.calls.clear();
    check("Double2D instance: propSetDouble(kOfxParamPropMax, index 0): that index only; setRange(), then setDisplayRange()",
          ps->propSetDouble(ih, kOfxParamPropMax, 0, 3.) == kOfxStatOK && inst.calls == "range display " &&
          inst.getProperties().getDoubleProperty(kOfxParamPropDisplayMax, 0) == 3. &&
          inst.getProperties().getDoubleProperty(kOfxParamPropDisplayMax, 1) == maxDouble);
    inst.calls.clear();
    check("Double2D instance: propSetDouble(kOfxParamPropDisplayMin): setDisplayRange() only",
          ps->propSetDouble(ih, kOfxParamPropDisplayMin, 0, 1.) == kOfxStatOK && inst.calls == "display ");
    // a host-side write (e.g. Natron's OfxParamToKnob::onMinMaxChanged(), which mirrors a knob's
    // new range): the display range stays, and the host is told of the range only
    inst.calls.clear();
    inst.getProperties().setDoubleProperty(kOfxParamPropMin, -9., 0);
    check("Double2D instance: the host's setDoubleProperty(kOfxParamPropMin): the display min unchanged, setRange() only",
          inst.calls == "range " && inst.getProperties().getDoubleProperty(kOfxParamPropMin, 0) == -9. &&
          inst.getProperties().getDoubleProperty(kOfxParamPropDisplayMin, 0) == 1.);
    inst.calls.clear();
    check("Double2D instance: then the plug-in's propSetDouble(kOfxParamPropMin): the display min follows",
          ps->propSetDouble(ih, kOfxParamPropMin, 0, -8.) == kOfxStatOK && inst.calls == "range display " &&
          inst.getProperties().getDoubleProperty(kOfxParamPropDisplayMin, 0) == -8.);

    // an integer param, whose range is int
    OfxPropertySetHandle ip = setInst.getParams().find("i")->second->getPropHandle();
    int idisplay = 0;
    check("Integer instance: propSetInt(kOfxParamPropMin): the display min follows",
          ps->propSetInt(ip, kOfxParamPropMin, 0, -3) == kOfxStatOK &&
          ps->propGetInt(ip, kOfxParamPropDisplayMin, 0, &idisplay) == kOfxStatOK && idisplay == -3);
  }

  /// the progress (ofxProgress.h) and timeline (ofxTimeLine.h) suites, one block for every handle:
  /// NULL and an effect descriptor are kOfxStatErrBadHandle (#187); a valid effect instance
  /// (instance, if not NULL: a real instance of a plug-in, HostSupport needs its binary to make
  /// one; #192, #201): a NULL label or message is an empty string; kOfxStatErrBadHandle, "the
  /// progress handle was invalid", without an ongoing progress (thrown by TestEffectInstance, as
  /// Natron does); gotoTime of NaN and of +-infinity, and a thrown kOfxStatErrValue, are
  /// kOfxStatErrValue ("the time was an illegal value"), another thrown status kOfxStatFailed;
  /// NULL outputs. (The #201 rows check the statuses the spec gives, in the middle of one
  /// progress and timeline sequence: they stay here.)
  void checkProgressAndTimeline(MyHost::Host &host, void *descriptor, MyHost::MyEffectInstance *myInstance)
  {
    const OfxProgressSuiteV1 *progress1 = (const OfxProgressSuiteV1 *)host.fetchSuite(kOfxProgressSuite, 1);
    const OfxProgressSuiteV2 *progress2 = (const OfxProgressSuiteV2 *)host.fetchSuite(kOfxProgressSuite, 2);
    const OfxTimeLineSuiteV1 *timeline = (const OfxTimeLineSuiteV1 *)host.fetchSuite(kOfxTimeLineSuite, 1);
    check("progress V1/V2 and timeline suites", progress1 && progress2 && timeline);
    if (!progress1 || !progress2 || !timeline) {
      return;
    }
    void *handles[2] = { NULL, descriptor };
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
    if (!myInstance) {
      return;
    }
    void *h = (void *)myInstance->getHandle();
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

  /// OFXH_REQUIRE=1 (CI, hostdemo-checks.sh): the checks that need a plug-in fail instead of
  /// being skipped (as in the GPU checks, gpuRenderChecks.h)
  bool required()
  {
    const char *r = std::getenv("OFXH_REQUIRE");
    return r && std::string(r) == "1";
  }

  /// the checks on an effect instance of Support/Plugins/Invert, found in OFX_PLUGIN_PATH (the
  /// functional ones, and the regression checks that need it); descriptor: an effect descriptor,
  /// for the handle checks of the same suites
  void checkEffectInstance(MyHost::Host &host, OfxImageEffectHandle descriptor)
  {
    const char *path = std::getenv("OFX_PLUGIN_PATH");
    if (!path || !*path) {
      if (required()) {
        check("OFX_PLUGIN_PATH is set (OFXH_REQUIRE=1)", false);
      } else {
        std::printf("suiteHandleChecks: OFX_PLUGIN_PATH is not set: the effect instance checks are skipped\n");
      }
      checkProgressAndTimeline(host, (void *)descriptor, nullptr);
      Regression::abort(host, descriptor, nullptr);
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
    CacheSnapshot snapshot; // for Regression::changedBinary() and cachedInvalidBinary()
    if (myInstance) {
      const OfxStatus stat = instance->createInstanceAction();
      check("createInstanceAction", stat == kOfxStatOK || stat == kOfxStatReplyDefault);

      snapshot = Regression::pluginCacheRescanAndRead(host, imageEffectPluginCache, *instance, pluginId);
      Regression::clips(instance);
      checkGPUPropertyInheritance(host, plugin);

      TestEffectInstance *test = dynamic_cast<TestEffectInstance *>(instance);
      check("TestEffectInstance", test != NULL);
      if (test) {
        checkRenderInArgs(*test);
        Regression::abort(host, descriptor, test);
        checkInstanceCopy(*test);
        checkClipPreferencesDepths(host, *test);
        checkGPURenderScope(host, *test, *plugin);
        checkCudaStreamArg(host, *test);
        checkNeedsSyncing(*test);
      } else {
        Regression::abort(host, descriptor, nullptr);
      }
      {
        ImageEffect::Descriptor fresh(plugin);
        checkLabels((const OfxPropertySuiteV1 *)Property::GetSuite(1), fresh.getProps().getHandle(), "effect descriptor",
                    plugin->getIdentifier());
      }
      checkProgressAndTimeline(host, (void *)descriptor, myInstance);
      // before deleting the instance, while it is whole (ofxhImageEffect.h)
      instance->destroyInstanceAction();
    } else {
      checkProgressAndTimeline(host, (void *)descriptor, nullptr);
      Regression::abort(host, descriptor, nullptr);
    }
    delete instance;
    PluginCache::clearPluginCache();
    if (!snapshot.xml.empty()) {
      Regression::changedBinary(host, snapshot, pluginId);
      Regression::cachedInvalidBinary(host, snapshot);
    }
    Regression::scanThrows(host);
  }
}

namespace {
  /// the "descriptor (read/write) and instance (read only)" properties are read-only on instances
  /// (refused with kOfxStatErrValue: the default ePluginReadOnlyEnforce policy), but writable on
  /// descriptors; kOfxParamPropGroupOpen stays writable on instances (ofxNatron.h, 4 and 9); #205
  /// (b9c68b7d)
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
  /// property is ignored on parameter types other than RGB and RGBA" (#209: 9a2ee8f2, c0166f6e)
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

  /// the memory suite (ofxMemory.h; #204): "Allocated memory will be aligned for any use", and a
  /// large block keeps malloc()'s alignment; 0 bytes; too large is kOfxStatErrMemory with a NULL
  /// output; NULL outputs and pointers are kOfxStatErrBadHandle. (memoryFree of a pointer that
  /// memoryAlloc didn't return: Regression::memoryFree().)
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
        check("memoryFree(memoryAlloc's pointer) = kOfxStatOK", mem->memoryFree(p) == kOfxStatOK);
      }
      p = NULL;
      check("memoryAlloc(0) = kOfxStatOK", mem->memoryAlloc(NULL, 0, &p) == kOfxStatOK && p != NULL && mem->memoryFree(p) == kOfxStatOK);
      p = &host;
      check("memoryAlloc(too large) = kOfxStatErrMemory, NULL", mem->memoryAlloc(NULL, (size_t)-1, &p) == kOfxStatErrMemory && p == NULL);
      check("memoryAlloc(NULL output) = kOfxStatErrBadHandle", mem->memoryAlloc(NULL, 1, NULL) == kOfxStatErrBadHandle);
      check("memoryFree(NULL) = kOfxStatErrBadHandle", mem->memoryFree(NULL) == kOfxStatErrBadHandle);
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

  // the param suite's handle and argument checks (ofxParam.h; #187): paramDefine
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

  // paramGetHandle: a NULL name zeroes the outputs (#187)
  OfxParamHandle param = (OfxParamHandle)&setInst;
  props = (OfxPropertySetHandle)&setInst;
  check("paramGetHandle(NULL name) = kOfxStatErrUnknown", ps->paramGetHandle(instSet, NULL, &param, &props) == kOfxStatErrUnknown);
  check("paramGetHandle(NULL name): *param and *propertySet = NULL", param == NULL && props == NULL);
  OfxParamHandle instParam = NULL;
  check("paramGetHandle(instance) = kOfxStatOK", ps->paramGetHandle(instSet, "i", &instParam, NULL) == kOfxStatOK && instParam != NULL);
  OfxParamHandle descParam = NULL;
  check("paramGetHandle(descriptor) = kOfxStatOK", ps->paramGetHandle(descSet, "i", &descParam, NULL) == kOfxStatOK && descParam != NULL);

  // the value and key functions need an instance; paramGetPropertySet works on both (#187)
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
    checkLinkedProperties(propSuite, setDesc, setInst);
    Regression::propGetDimension(propSuite, intDesc->getPropHandle());
  }

  Regression::bytesDefault(propSuite, setDesc, setInst);

  // the interact suite and the DrawSuite (no plug-in needed: a test interact)
  checkInteractSuites();

#ifdef OFX_SUPPORTS_OPENGLRENDER
  // clipLoadTexture of a NULL clip (ofxGPURender.h: "kOfxStatErrBadHandle - the clip handle
  // was invalid"), and the output set to NULL (#202)
  {
    const OfxImageEffectOpenGLRenderSuiteV1 *gl = (const OfxImageEffectOpenGLRenderSuiteV1 *)host.fetchSuite(kOfxOpenGLRenderSuite, 1);
    check("OpenGL render suite", gl != NULL);
    if (gl) {
      const OfxRectD region = { 0., 0., 1., 1. };
      OfxPropertySetHandle tex = (OfxPropertySetHandle)&host; // not NULL: the function must set it to NULL
      check("clipLoadTexture(NULL clip) = kOfxStatErrBadHandle, NULL", gl->clipLoadTexture(NULL, 0., NULL, &region, &tex) == kOfxStatErrBadHandle && tex == NULL);
    }
  }
#endif
  // std::bad_alloc in the image, image memory and texture functions (no plug-in needed: test clips)
  Regression::badAlloc(host);

  checkMemorySuite(host);
  Regression::memoryFree(host);

  // the C fetchSuite of the host (ofxCore.h; #187): NULL for a NULL host, a property set that
  // isn't the host's, or a NULL name
  OfxHost *ofxHost = host.getHandle();
  Property::Set notHostProps;
  check("fetchSuite(host) != NULL", ofxHost->fetchSuite(ofxHost->host, kOfxPropertySuite, 1) != NULL);
  check("fetchSuite(NULL host) = NULL", ofxHost->fetchSuite(NULL, kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(not the host property set) = NULL", ofxHost->fetchSuite(notHostProps.getHandle(), kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(NULL name) = NULL", ofxHost->fetchSuite(ofxHost->host, NULL, 1) == NULL);

  // the supported depths, components and contexts, in both profiles
  Regression::hostDescriptor(host);

  // pointer defaults, exact doubles, malformed properties in the plug-in cache
  Regression::propertyValues();
  Regression::setCopyThrows();

  // the effect labels' fallbacks
  Regression::effectLabels();

  // the default and the plug-in's kOfxParamPropChoiceOrder
  if (propSuite) {
    checkChoiceOrder(propSuite);
  }

  // the host's plug-in directories
  Regression::pluginHostPath();

  // a real effect instance (the progress and timeline functions on a valid handle, and on NULL
  // and on an effect descriptor, which needs no plug-in)
  ImageEffect::Descriptor effectDesc("/nonexistent", NULL);
  checkEffectInstance(host, effectDesc.getHandle());

  ImageEffect::gImageEffectHost = NULL;
  const char *kinds[2] = { "functional", "regression" };
  for (int k = 0; k < 2; ++k) {
    std::printf("suiteHandleChecks: %s: %d of %d checks OK\n", kinds[k], gChecks[k] - gFailures[k], gChecks[k]);
  }
  const int checks = gChecks[0] + gChecks[1], failures = gFailures[0] + gFailures[1];
  std::printf("suiteHandleChecks: %d of %d checks OK\n", checks - failures, checks);
  return failures ? 1 : 0;
}
