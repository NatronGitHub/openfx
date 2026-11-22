// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Self-test of Support library code that needs no host (target support-library-test, built with
  -DBUILD_EXAMPLE_PLUGINS=ON):

  - a null action passed to the interact entry point returns kOfxStatFailed (it was turned into a
    std::string, which is undefined behaviour);
  - OFX::MultiThread::AutoMutexT's destructor doesn't throw (std::terminate()) when the mutex's
    unlock() throws;
  - with OFX_EXTENSIONS_NATRON, OFX::Private::decodeNativeOverlayHandle(), which decodes the host's
    kNatronOfxPropNativeOverlays strings (ofxNatron.h) into ImageEffectHostDescription::nativeInteracts.
  - kOfxActionCreateInstance (OFX::Private::createInstanceAction()) deletes the new instance, and so
    clears kOfxPropInstanceData, when a step after PluginFactory::createInstance() throws (it
    leaked, and the host kept the pointer), on a minimal fake host.

  The null action through an image effect's entry point (OfxPlugin::mainEntry) is tested on a
  plug-in bundle, in Support/Plugins/tests/test_entry_point.py.

  Prints one line per failed check, and exits with 0 if all passed.
*/

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "ofxsSupportPrivate.h"
#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include "ofxsMultiThread.h"

static int gFailures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures; \
    } \
  } while (0)

namespace {

  class TestInteract : public OFX::OverlayInteract {
  public:
    TestInteract(OfxInteractHandle handle, OFX::ImageEffect* effect) : OFX::OverlayInteract(handle) { (void)effect; }
  };

  class TestInteractDescriptor : public OFX::DefaultEffectOverlayDescriptor<TestInteractDescriptor, TestInteract> {};

  void testNullInteractAction()
  {
    TestInteractDescriptor desc;
    CHECK(OFX::Private::interactMainEntry(NULL, NULL, NULL, NULL, desc) == kOfxStatFailed);
  }

  struct ThrowingMutex {
    int locks;
    int unlocks;
    ThrowingMutex() : locks(0), unlocks(0) {}
    void lock() { ++locks; }
    void unlock() { ++unlocks; throw std::runtime_error("unlock failed"); }
  };

  void testAutoMutexDestructorDoesNotThrow()
  {
    ThrowingMutex m;
    bool caught = false;
    try {
      // would call std::terminate() if the exception left ~AutoMutexT()
      OFX::MultiThread::AutoMutexT<ThrowingMutex> lock(m);
    } catch (...) {
      caught = true;
    }
    CHECK(!caught);
    CHECK(m.locks == 1);
    CHECK(m.unlocks == 1);

    // an explicit unlock() still throws, and the destructor doesn't unlock again
    caught = false;
    ThrowingMutex m2;
    {
      OFX::MultiThread::AutoMutexT<ThrowingMutex> lock2(m2);
      try {
        lock2.unlock();
      } catch (const std::runtime_error&) {
        caught = true;
      }
    }
    CHECK(caught);
    CHECK(m2.unlocks == 1);
  }

#ifdef OFX_EXTENSIONS_NATRON
  typedef OFX::ImageEffectHostDescription::NativeOverlayHandle Handle;

  void testDecodeNativeOverlayHandle()
  {
    // the examples of ofxNatron.h
    Handle h;
    CHECK(OFX::Private::decodeNativeOverlayHandle(
      "NatronNativeOverlayType_Point2D_NatronNativeOverlayParameterHint_Position_NatronNativeOverlayParameterType_OfxParamTypeDouble2D", h));
    CHECK(h.identifier == "Point2D");
    CHECK(h.parameters.size() == 1);
    if (h.parameters.size() == 1) {
      CHECK(h.parameters[0].first == "Position");
      CHECK(h.parameters[0].second == kOfxParamTypeDouble2D);
    }

    Handle c;
    CHECK(OFX::Private::decodeNativeOverlayHandle(
      "NatronNativeOverlayType_CornerPin2D"
      "_NatronNativeOverlayParameterHint_TopLeft_NatronNativeOverlayParameterType_OfxParamTypeDouble2D"
      "_NatronNativeOverlayParameterHint_TopRight_NatronNativeOverlayParameterType_OfxParamTypeDouble2D"
      "_NatronNativeOverlayParameterHint_BottomRIght_NatronNativeOverlayParameterType_OfxParamTypeDouble2D"
      "_NatronNativeOverlayParameterHint_BottomLeft_NatronNativeOverlayParameterType_OfxParamTypeInteger2D", c));
    CHECK(c.identifier == "CornerPin2D");
    CHECK(c.parameters.size() == 4);
    if (c.parameters.size() == 4) {
      CHECK(c.parameters[0].first == "TopLeft" && c.parameters[0].second == kOfxParamTypeDouble2D);
      CHECK(c.parameters[1].first == "TopRight" && c.parameters[1].second == kOfxParamTypeDouble2D);
      CHECK(c.parameters[2].first == "BottomRIght" && c.parameters[2].second == kOfxParamTypeDouble2D);
      CHECK(c.parameters[3].first == "BottomLeft" && c.parameters[3].second == kOfxParamTypeInteger2D);
    }

    // identifiers and hints may contain '_'
    Handle u;
    CHECK(OFX::Private::decodeNativeOverlayHandle(
      "NatronNativeOverlayType_My_Overlay_NatronNativeOverlayParameterHint_The_Center_NatronNativeOverlayParameterType_OfxParamTypeDouble2D", u));
    CHECK(u.identifier == "My_Overlay");
    CHECK(u.parameters.size() == 1 && u.parameters[0].first == "The_Center" && u.parameters[0].second == kOfxParamTypeDouble2D);

    // badly encoded strings are refused, and leave the handle unchanged
    const char* bad[] = {
      "",
      "NatronNativeOverlayType",
      "NatronNativeOverlayType_Point2D",                                       // no parameter
      "XNatronNativeOverlayType_Point2D_NatronNativeOverlayParameterHint_Position_NatronNativeOverlayParameterType_OfxParamTypeDouble2D",
      "NatronNativeOverlayType__NatronNativeOverlayParameterHint_Position_NatronNativeOverlayParameterType_OfxParamTypeDouble2D", // no identifier
      "NatronNativeOverlayType_Point2D_NatronNativeOverlayParameterHint_Position", // no type
      "NatronNativeOverlayType_Point2D_NatronNativeOverlayParameterHint__NatronNativeOverlayParameterType_OfxParamTypeDouble2D", // no hint
      "NatronNativeOverlayType_Point2D_NatronNativeOverlayParameterHint_Position_NatronNativeOverlayParameterType_", // empty type
      // a second parameter without a type
      ("NatronNativeOverlayType_Point2D_NatronNativeOverlayParameterHint_Position_NatronNativeOverlayParameterType_OfxParamTypeDouble2D"
       "_NatronNativeOverlayParameterHint_Scale"),
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      Handle b = h;
      if (OFX::Private::decodeNativeOverlayHandle(bad[i], b)) {
        std::printf("FAILED: decoded the bad string '%s'\n", bad[i]);
        ++gFailures;
      }
      CHECK(b.identifier == h.identifier && b.parameters == h.parameters);
    }
  }
#endif

  // A minimal fake host for kOfxActionCreateInstance: the property suite knows the instance's
  // kOfxImageEffectPropContext and kOfxPropInstanceData only (the host description gets the
  // defaults), and the effect suite's getPropertySet can be made to fail.
  namespace FakeHost {
    int gEffectPropsStorage = 0;
    OfxPropertySetHandle const kEffectProps = reinterpret_cast<OfxPropertySetHandle>(&gEffectPropsStorage);
    int gEffectStorage = 0;
    OfxImageEffectHandle const kEffect = reinterpret_cast<OfxImageEffectHandle>(&gEffectStorage);
    int gHostPropsStorage = 0;

    void *gInstanceData = NULL;
    int gGetPropertySetCalls = 0;
    int gFailGetPropertySetFrom = -1; // the calls from that one on fail, -1: none

    OfxStatus unknown() { return kOfxStatErrUnknown; }

    OfxStatus propSetPointer(OfxPropertySetHandle props, const char *name, int index, void *value)
    {
      if (props == kEffectProps && index == 0 && std::strcmp(name, kOfxPropInstanceData) == 0) {
        gInstanceData = value;
        return kOfxStatOK;
      }
      return unknown();
    }
    OfxStatus propSetString(OfxPropertySetHandle, const char *, int, const char *) { return unknown(); }
    OfxStatus propSetDouble(OfxPropertySetHandle, const char *, int, double) { return unknown(); }
    OfxStatus propSetInt(OfxPropertySetHandle, const char *, int, int) { return unknown(); }
    OfxStatus propSetPointerN(OfxPropertySetHandle, const char *, int, void *const *) { return unknown(); }
    OfxStatus propSetStringN(OfxPropertySetHandle, const char *, int, const char *const *) { return unknown(); }
    OfxStatus propSetDoubleN(OfxPropertySetHandle, const char *, int, const double *) { return unknown(); }
    OfxStatus propSetIntN(OfxPropertySetHandle, const char *, int, const int *) { return unknown(); }
    OfxStatus propGetPointer(OfxPropertySetHandle props, const char *name, int index, void **value)
    {
      if (props == kEffectProps && index == 0 && std::strcmp(name, kOfxPropInstanceData) == 0) {
        *value = gInstanceData;
        return kOfxStatOK;
      }
      return unknown();
    }
    OfxStatus propGetString(OfxPropertySetHandle props, const char *name, int index, const char **value)
    {
      if (props == kEffectProps && index == 0 && std::strcmp(name, kOfxImageEffectPropContext) == 0) {
        *value = kOfxImageEffectContextFilter;
        return kOfxStatOK;
      }
      return unknown();
    }
    OfxStatus propGetDouble(OfxPropertySetHandle, const char *, int, double *) { return unknown(); }
    OfxStatus propGetInt(OfxPropertySetHandle, const char *, int, int *) { return unknown(); }
    OfxStatus propGetPointerN(OfxPropertySetHandle, const char *, int, void **) { return unknown(); }
    OfxStatus propGetStringN(OfxPropertySetHandle, const char *, int, const char **) { return unknown(); }
    OfxStatus propGetDoubleN(OfxPropertySetHandle, const char *, int, double *) { return unknown(); }
    OfxStatus propGetIntN(OfxPropertySetHandle, const char *, int, int *) { return unknown(); }
    OfxStatus propReset(OfxPropertySetHandle, const char *) { return unknown(); }
    OfxStatus propGetDimension(OfxPropertySetHandle, const char *, int *) { return unknown(); }

    OfxStatus getPropertySet(OfxImageEffectHandle effect, OfxPropertySetHandle *props)
    {
      const int call = gGetPropertySetCalls++;
      if (effect != kEffect || (gFailGetPropertySetFrom >= 0 && call >= gFailGetPropertySetFrom)) {
        return kOfxStatErrBadHandle;
      }
      *props = kEffectProps;
      return kOfxStatOK;
    }
    OfxStatus getParamSet(OfxImageEffectHandle, OfxParamSetHandle *paramSet)
    {
      *paramSet = NULL; // ParamSet::setParamSetHandle(NULL) fetches nothing
      return kOfxStatOK;
    }

    OfxPropertySuiteV1 gPropSuite;
    OfxImageEffectSuiteV1 gEffectSuite;
    // never called by the actions tested: only their presence is checked by the load action
    OfxParameterSuiteV1 gParamSuite;
    OfxMemorySuiteV1 gMemorySuite;
    OfxMultiThreadSuiteV1 gThreadSuite;
    OfxMessageSuiteV1 gMessageSuite;

    const void *fetchSuite(OfxPropertySetHandle, const char *name, int version)
    {
      if (version != 1) return NULL;
      if (std::strcmp(name, kOfxPropertySuite) == 0) return &gPropSuite;
      if (std::strcmp(name, kOfxImageEffectSuite) == 0) return &gEffectSuite;
      if (std::strcmp(name, kOfxParameterSuite) == 0) return &gParamSuite;
      if (std::strcmp(name, kOfxMemorySuite) == 0) return &gMemorySuite;
      if (std::strcmp(name, kOfxMultiThreadSuite) == 0) return &gThreadSuite;
      if (std::strcmp(name, kOfxMessageSuite) == 0) return &gMessageSuite;
      return NULL;
    }

    OfxHost gHost;

    /** @brief sets the suites, and loads (kOfxActionLoad: the host description) */
    void load()
    {
      std::memset(&gPropSuite, 0, sizeof(gPropSuite));
      gPropSuite.propSetPointer = propSetPointer;
      gPropSuite.propSetString = propSetString;
      gPropSuite.propSetDouble = propSetDouble;
      gPropSuite.propSetInt = propSetInt;
      gPropSuite.propSetPointerN = propSetPointerN;
      gPropSuite.propSetStringN = propSetStringN;
      gPropSuite.propSetDoubleN = propSetDoubleN;
      gPropSuite.propSetIntN = propSetIntN;
      gPropSuite.propGetPointer = propGetPointer;
      gPropSuite.propGetString = propGetString;
      gPropSuite.propGetDouble = propGetDouble;
      gPropSuite.propGetInt = propGetInt;
      gPropSuite.propGetPointerN = propGetPointerN;
      gPropSuite.propGetStringN = propGetStringN;
      gPropSuite.propGetDoubleN = propGetDoubleN;
      gPropSuite.propGetIntN = propGetIntN;
      gPropSuite.propReset = propReset;
      gPropSuite.propGetDimension = propGetDimension;
      std::memset(&gEffectSuite, 0, sizeof(gEffectSuite));
      gEffectSuite.getPropertySet = getPropertySet;
      gEffectSuite.getParamSet = getParamSet;
      std::memset(&gParamSuite, 0, sizeof(gParamSuite));
      std::memset(&gMemorySuite, 0, sizeof(gMemorySuite));
      std::memset(&gThreadSuite, 0, sizeof(gThreadSuite));
      std::memset(&gMessageSuite, 0, sizeof(gMessageSuite));
      gHost.host = reinterpret_cast<OfxPropertySetHandle>(&gHostPropsStorage);
      gHost.fetchSuite = fetchSuite;

      // the host lacks every host property: read them as absent instead of throwing
      OFX::PropertySet::setThrowOnUnsupportedProperties(false);
      OFX::Private::setHost(&gHost);
      OFX::Private::loadAction();
      OFX::PropertySet::setThrowOnUnsupportedProperties(true);
    }
  } // namespace FakeHost

  int gTestEffectsAlive = 0;

  class TestEffect : public OFX::ImageEffect {
  public:
    TestEffect(OfxImageEffectHandle handle, bool failAfterCreation)
      : OFX::ImageEffect(handle)
    {
      ++gTestEffectsAlive;
      if (failAfterCreation) {
        // the host fails from now on, i.e. after PluginFactory::createInstance()
        FakeHost::gFailGetPropertySetFrom = FakeHost::gGetPropertySetCalls;
      }
    }
    virtual ~TestEffect() { --gTestEffectsAlive; }
    virtual void render(const OFX::RenderArguments &) {}
  };

  class TestFactory : public OFX::PluginFactoryHelper<TestFactory> {
  public:
    explicit TestFactory(bool failAfterCreation)
      : OFX::PluginFactoryHelper<TestFactory>("net.sf.openfx.supportLibraryTest", 1, 0)
      , _failAfterCreation(failAfterCreation)
    {}
    virtual void describe(OFX::ImageEffectDescriptor &) {}
    virtual void describeInContext(OFX::ImageEffectDescriptor &, OFX::ContextEnum) {}
    virtual OFX::ImageEffect *createInstance(OfxImageEffectHandle handle, OFX::ContextEnum)
    {
      return new TestEffect(handle, _failAfterCreation);
    }
  private:
    bool _failAfterCreation;
  };

  void testCreateInstanceFailureDeletesInstance()
  {
    FakeHost::load();

    // success: the instance is in kOfxPropInstanceData
    {
      TestFactory factory(false);
      OFX::ImageEffect *instance = NULL;
      try {
        instance = OFX::Private::createInstanceAction(&factory, FakeHost::kEffect);
      } catch (...) {
        CHECK(!"createInstanceAction threw");
      }
      CHECK(instance != NULL);
      CHECK(FakeHost::gInstanceData == instance);
      CHECK(gTestEffectsAlive == 1);
      delete instance; // kOfxActionDestroyInstance
      CHECK(FakeHost::gInstanceData == NULL);
      CHECK(gTestEffectsAlive == 0);
    }

    // the step after createInstance() throws: the exception goes on, the instance is deleted, and
    // the host's kOfxPropInstanceData doesn't point to it any more
    {
      TestFactory factory(true);
      bool caught = false;
      try {
        OFX::Private::createInstanceAction(&factory, FakeHost::kEffect);
      } catch (const OFX::Exception::Suite &e) {
        caught = e.status() == kOfxStatErrBadHandle;
      } catch (...) {
      }
      CHECK(caught);
      CHECK(gTestEffectsAlive == 0);
      CHECK(FakeHost::gInstanceData == NULL);
    }
    FakeHost::gFailGetPropertySetFrom = -1;
  }

} // namespace

int main()
{
  testNullInteractAction();
  testAutoMutexDestructorDoesNotThrow();
  testCreateInstanceFailureDeletesInstance();
#ifdef OFX_EXTENSIONS_NATRON
  testDecodeNativeOverlayHandle();
#else
  std::printf("support-library-test: OFX_EXTENSIONS_NATRON is off, decodeNativeOverlayHandle() not tested\n");
#endif
  if (gFailures) {
    std::printf("support-library-test: %d failed check(s)\n", gFailures);
    return 1;
  }
  std::printf("support-library-test: all checks passed\n");
  return 0;
}
