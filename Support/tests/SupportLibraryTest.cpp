// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Self-test of Support library code that needs no host (target support-library-test, built with
  -DBUILD_EXAMPLE_PLUGINS=ON):

  - a null action passed to the interact entry point returns kOfxStatFailed (it was turned into a
    std::string, which is undefined behaviour);
  - OFX::MultiThread::AutoMutexT's destructor doesn't throw (std::terminate()) when the mutex's
    unlock() throws;
  - OFX::ParamSet::EditBlock's destructor doesn't throw (std::terminate()) when the host's
    paramEditEnd() fails, and its options (eBeginOnChange, eHostErrorsIgnore) begin and end the
    edit block when they should;
  - lazy registration (a static OFX::PluginFactories(f) object): the function set from a static
    initializer is called once, at the first OfxGetNumberOfPlugins(), and its factories are the
    plug-ins of the binary;
  - define*Param() throws OFX::Exception::TypeRequest when a param of the same name but another
    type was already defined (it returned NULL in release builds);
  - the accessors of a property the host may not have (OFX::Private::propGetIntOr() and the
    others, ofxsProperty.cpp): a property the host doesn't have gives the default, or writes
    nothing; other failures throw;
  - OFX::MultiThread::Processor::multiThread() and getNumCPUs() with a thread suite that lacks
    multiThread or multiThreadNumCPUs: single-threaded, no crash;
  - OFX::MultiThread::Mutex (built with OFX_USE_MULTITHREAD_MUTEX) without the host's mutex
    functions: a lock count, nested in the holding thread, positive or negative initially;
  - the double param descriptors' default coordinate system: no write while Plain or non-spatial;
    setDoubleType() to a spatial type writes the plug-in's setDefaultCoordinateSystem() choice,
    made before or after, or canonical, over a host's reset (Nuke: normalised);
  - ClipPreferencesSetter::setPixelAspectRatio(): the host's PAR isn't written again, and a refused
    or unknown PAR is a warning, not an exception;
  - with OFX_EXTENSIONS_NATRON, OFX::Private::decodeNativeOverlayHandle(), which decodes the host's
    kNatronOfxPropNativeOverlays strings (ofxNatron.h) into ImageEffectHostDescription::nativeInteracts.

  The null action through an image effect's entry point (OfxPlugin::mainEntry) is tested on a
  plug-in bundle, in Support/Plugins/tests/test_entry_point.py.

  Prints one line per failed check, and exits with 0 if all passed.
*/

#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
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

  // a param suite with only the edit block functions, which count their calls and return the
  // given statuses
  int gEditBegins = 0;
  int gEditEnds = 0;
  OfxStatus gEditBeginStatus = kOfxStatOK;
  OfxStatus gEditEndStatus = kOfxStatOK;
  OfxStatus editBegin(OfxParamSetHandle, const char*) { ++gEditBegins; return gEditBeginStatus; }
  OfxStatus editEnd(OfxParamSetHandle) { ++gEditEnds; return gEditEndStatus; }

  class TestEditParamSet : public OFX::ParamSet {};

  typedef OFX::ParamSet::EditBlock EditBlock;

  void resetEditSuite(OfxStatus beginStatus, OfxStatus endStatus)
  {
    gEditBegins = 0;
    gEditEnds = 0;
    gEditBeginStatus = beginStatus;
    gEditEndStatus = endStatus;
  }

  void testEditBlock()
  {
    OfxParameterSuiteV1 suite = OfxParameterSuiteV1();
    suite.paramEditBegin = editBegin;
    suite.paramEditEnd = editEnd;
    OfxParameterSuiteV1* saved = OFX::Private::gParamSuite;
    OFX::Private::gParamSuite = &suite;
    TestEditParamSet set;

    // the destructor doesn't throw when paramEditEnd() fails
    resetEditSuite(kOfxStatOK, kOfxStatFailed);
    bool caught = false;
    try {
      // would call std::terminate() if the exception left ~EditBlock()
      EditBlock eb(set, "test");
    } catch (...) {
      caught = true;
    }
    CHECK(!caught);
    CHECK(gEditBegins == 1 && gEditEnds == 1);

    // ParamSet::endEditBlock() still reports the failure
    caught = false;
    try {
      set.endEditBlock();
    } catch (const OFX::Exception::Suite& e) {
      caught = e.status() == kOfxStatFailed;
    }
    CHECK(caught);

    // by default, a failed begin throws from the constructor, and there is no end
    resetEditSuite(kOfxStatFailed, kOfxStatOK);
    caught = false;
    try {
      EditBlock eb(set, "test");
    } catch (const OFX::Exception::Suite&) {
      caught = true;
    }
    CHECK(caught);
    CHECK(gEditBegins == 1 && gEditEnds == 0);

    // cond false: no edit block, even after open()
    resetEditSuite(kOfxStatOK, kOfxStatOK);
    {
      EditBlock eb(set, "test", false);
      eb.open();
    }
    CHECK(gEditBegins == 0 && gEditEnds == 0);

    // eHostErrorsIgnore: a failed begin is ignored, and there is no end
    resetEditSuite(kOfxStatFailed, kOfxStatOK);
    caught = false;
    try {
      EditBlock eb(set, "test", true, EditBlock::eBeginNow, EditBlock::eHostErrorsIgnore);
    } catch (...) {
      caught = true;
    }
    CHECK(!caught);
    CHECK(gEditBegins == 1 && gEditEnds == 0);

    // eBeginOnChange: no change, no edit block
    resetEditSuite(kOfxStatOK, kOfxStatOK);
    {
      EditBlock eb(set, "test", true, EditBlock::eBeginOnChange);
    }
    CHECK(gEditBegins == 0 && gEditEnds == 0);

    // eBeginOnChange: begun by the first open() only, ended once
    resetEditSuite(kOfxStatOK, kOfxStatOK);
    {
      EditBlock eb(set, "test", true, EditBlock::eBeginOnChange);
      CHECK(gEditBegins == 0);
      eb.open();
      eb.open();
      CHECK(gEditBegins == 1);
    }
    CHECK(gEditBegins == 1 && gEditEnds == 1);

    // eBeginOnChange: a failed begin throws from open() (not retried), and there is no end
    resetEditSuite(kOfxStatFailed, kOfxStatOK);
    caught = false;
    {
      EditBlock eb(set, "test", true, EditBlock::eBeginOnChange);
      try {
        eb.open();
      } catch (const OFX::Exception::Suite&) {
        caught = true;
      }
      eb.open();
    }
    CHECK(caught);
    CHECK(gEditBegins == 1 && gEditEnds == 0);

    // the 3-argument form with an option (cond true, not the enum converted to bool)
    resetEditSuite(kOfxStatOK, kOfxStatOK);
    {
      EditBlock eb(set, "test", EditBlock::eBeginOnChange);
      CHECK(gEditBegins == 0);
      eb.open();
      CHECK(gEditBegins == 1);
    }
    CHECK(gEditBegins == 1 && gEditEnds == 1);
    resetEditSuite(kOfxStatOK, kOfxStatOK);
    {
      EditBlock eb(set, "test", EditBlock::eBeginNow);
      CHECK(gEditBegins == 1);
    }
    CHECK(gEditBegins == 1 && gEditEnds == 1);

    // both options: a failed begin is ignored, not retried, and there is no end
    resetEditSuite(kOfxStatFailed, kOfxStatOK);
    caught = false;
    try {
      EditBlock eb(set, "test", EditBlock::eBeginOnChange, EditBlock::eHostErrorsIgnore);
      eb.open();
      eb.open();
    } catch (...) {
      caught = true;
    }
    CHECK(!caught);
    CHECK(gEditBegins == 1 && gEditEnds == 0);

    OFX::Private::gParamSuite = saved;
  }

  // lazy registration: one plug-in, registered at the first OfxGetNumberOfPlugins()
  mDeclarePluginFactory(LazyTestFactory, {}, {});
  void LazyTestFactory::describe(OFX::ImageEffectDescriptor&) {}
  void LazyTestFactory::describeInContext(OFX::ImageEffectDescriptor&, OFX::ContextEnum) {}
  OFX::ImageEffect* LazyTestFactory::createInstance(OfxImageEffectHandle, OFX::ContextEnum) { return NULL; }

  int gLazyRegistrations = 0;
  void registerLazyTestPlugins(OFX::PluginFactoryArray& factories)
  {
    ++gLazyRegistrations;
    static LazyTestFactory factory("net.sf.openfx.lazyRegistrationTest", 1, 0);
    factories.push_back(&factory);
  }
  OFX::PluginFactories gLazyRegistration(&registerLazyTestPlugins);

  void testLazyRegistration()
  {
    CHECK(OFX::PluginFactories::lazyRegistration() == &registerLazyTestPlugins);
    CHECK(gLazyRegistrations == 0); // not at static initialization
    CHECK(OfxGetNumberOfPlugins() == 1);
    CHECK(gLazyRegistrations == 1);
    OfxPlugin* plugin = OfxGetPlugin(0);
    CHECK(plugin != NULL && std::string(plugin->pluginIdentifier) == "net.sf.openfx.lazyRegistrationTest");
    // called once
    CHECK(OfxGetNumberOfPlugins() == 1);
    CHECK(OfxGetPlugin(0) == plugin);
    CHECK(gLazyRegistrations == 1);
  }

  // a param set with a param "p" already defined, without a host (no param suite is called)
  class TestParamSet : public OFX::ParamSetDescriptor {
  public:
    TestParamSet() { _definedParams["p"] = new OFX::DummyParamDescriptor("p"); }
  };

  void testDefineParamTypeMismatchThrows()
  {
    TestParamSet set;
    bool thrown = false;
    try {
      set.defineIntParam("p");
    } catch (const OFX::Exception::TypeRequest&) {
      thrown = true;
    }
    CHECK(thrown);
  }

  // a property suite whose functions return gPropStatus (and 7, 0.5, "set" on success)
  OfxStatus gPropStatus = kOfxStatOK;
  int gPropSets = 0;
  OfxStatus propSetInt(OfxPropertySetHandle, const char*, int, int) { ++gPropSets; return gPropStatus; }
  OfxStatus propSetDouble(OfxPropertySetHandle, const char*, int, double) { ++gPropSets; return gPropStatus; }
  OfxStatus propSetString(OfxPropertySetHandle, const char*, int, const char*) { ++gPropSets; return gPropStatus; }
  OfxStatus propSetPointer(OfxPropertySetHandle, const char*, int, void*) { ++gPropSets; return gPropStatus; }
  OfxStatus propGetInt(OfxPropertySetHandle, const char*, int, int* v) { *v = 7; return gPropStatus; }
  OfxStatus propGetDouble(OfxPropertySetHandle, const char*, int, double* v) { *v = 0.5; return gPropStatus; }
  OfxStatus propGetString(OfxPropertySetHandle, const char*, int, const char** v) { *v = "set"; return gPropStatus; }

  void testPropertyDefaults()
  {
    OfxPropertySuiteV1 suite = OfxPropertySuiteV1();
    suite.propSetInt = propSetInt;
    suite.propSetDouble = propSetDouble;
    suite.propSetString = propSetString;
    suite.propSetPointer = propSetPointer;
    suite.propGetInt = propGetInt;
    suite.propGetDouble = propGetDouble;
    suite.propGetString = propGetString;
    OfxPropertySuiteV1* saved = OFX::Private::gPropSuite;
    OFX::Private::gPropSuite = &suite;
    int dummy = 0;
    OFX::PropertySet props(reinterpret_cast<OfxPropertySetHandle>(&dummy));
    using namespace OFX::Private;

    // the host has it: its value, and the write
    gPropStatus = kOfxStatOK;
    CHECK(propGetIntOr(props, "p", 1) == 7);
    CHECK(propGetDoubleOr(props, "p", 1.) == 0.5);
    CHECK(propGetStringOr(props, "p", "default") == "set");
    gPropSets = 0;
    propSetIntOr(props, "p", 3, 1);
    CHECK(gPropSets == 1);

    // the host doesn't have it (kOfxStatErrUnknown, or kOfxStatErrUnsupported from some hosts): the
    // default, and no exception, whatever the value written
    for (OfxStatus st : {kOfxStatErrUnknown, kOfxStatErrUnsupported}) {
      gPropStatus = st;
      CHECK(propGetIntOr(props, "p", 1) == 1);
      CHECK(propGetDoubleOr(props, "p", 2.) == 2.);
      CHECK(propGetStringOr(props, "p", "default") == "default");
      bool caught = false;
      try {
        propSetIntOr(props, "p", 1, 1);
        propSetIntOr(props, "p", 3, 1);
        propSetDoubleOr(props, "p", 3., 1.);
        propSetStringOr(props, "p", "value", "");
        propSetPointerOr(props, "p", &dummy, NULL);
      } catch (...) {
        caught = true;
      }
      CHECK(!caught);
    }

    // other failures throw, as in PropertySet
    gPropStatus = kOfxStatErrValue;
    bool caught = false;
    try {
      propSetIntOr(props, "p", 3, 1);
    } catch (const OFX::Exception::PropertyValueIllegalToHost&) {
      caught = true;
    }
    CHECK(caught);
    gPropStatus = kOfxStatErrBadHandle;
    caught = false;
    try {
      propGetIntOr(props, "p", 1);
    } catch (const OFX::Exception::Suite&) {
      caught = true;
    }
    CHECK(caught);

    gPropStatus = kOfxStatOK;
    OFX::Private::gPropSuite = saved;
  }

  // A fake host for one param descriptor: string properties in a map. When the type becomes XY or
  // XYAbsolute, it resets kOfxParamPropDefaultCoordinateSystem to gHostReset (normalised, as Nuke
  // does). With resetHost(true), the property exists only from then on.
  std::map<std::string, std::string> gStrings;
  const char* gHostReset = kOfxParamCoordinatesNormalised;
  int gCoordWrites = 0; // writes of kOfxParamPropDefaultCoordinateSystem
  int gDoubleWrites = 0; // the descriptor constructors write only the four range properties
  OfxStatus recSetDouble(OfxPropertySetHandle, const char*, int, double) { ++gDoubleWrites; return kOfxStatOK; }
  OfxStatus recSetString(OfxPropertySetHandle, const char* name, int, const char* v)
  {
    const std::string n = name, value = v;
    if (n == kOfxParamPropDefaultCoordinateSystem) {
      ++gCoordWrites;
      if (!gStrings.count(n)) {
        return kOfxStatErrUnknown;
      }
    }
    gStrings[n] = value;
    if (n == kOfxParamPropDoubleType && (value == kOfxParamDoubleTypeXY || value == kOfxParamDoubleTypeXYAbsolute)) {
      gStrings[kOfxParamPropDefaultCoordinateSystem] = gHostReset;
    }
    return kOfxStatOK;
  }
  OfxStatus recGetString(OfxPropertySetHandle, const char* name, int, const char** v)
  {
    if (!gStrings.count(name)) {
      return kOfxStatErrUnknown;
    }
    *v = gStrings[name].c_str();
    return kOfxStatOK;
  }

  // the constructor is protected
  struct TestDouble2DDescriptor : public OFX::Double2DParamDescriptor {
    explicit TestDouble2DDescriptor(OfxPropertySetHandle h) : OFX::Double2DParamDescriptor("p", h) {}
  };

  std::string coords()
  {
    return gStrings.count(kOfxParamPropDefaultCoordinateSystem) ? gStrings[kOfxParamPropDefaultCoordinateSystem] : "absent";
  }

  void resetHost(bool coordsOnlyWhenSpatial, const char* hostReset = kOfxParamCoordinatesNormalised)
  {
    gStrings.clear();
    if (!coordsOnlyWhenSpatial) {
      gStrings[kOfxParamPropDefaultCoordinateSystem] = kOfxParamCoordinatesNormalised;
    }
    gHostReset = hostReset;
    gCoordWrites = gDoubleWrites = 0;
  }

  void testDefaultCoordinateSystem()
  {
    OfxPropertySuiteV1 suite = OfxPropertySuiteV1();
    suite.propSetInt = propSetInt; // the ones of testPropertyDefaults()
    suite.propGetInt = propGetInt;
    suite.propGetDouble = propGetDouble;
    suite.propSetDouble = recSetDouble;
    suite.propSetString = recSetString;
    suite.propGetString = recGetString;
    OfxPropertySuiteV1* saved = OFX::Private::gPropSuite;
    OFX::Private::gPropSuite = &suite;
    int dummy = 0;
    OfxPropertySetHandle h = reinterpret_cast<OfxPropertySetHandle>(&dummy);
    const std::string canonical = kOfxParamCoordinatesCanonical;
    const std::string normalised = kOfxParamCoordinatesNormalised;

    for (int onlyWhenSpatial = 0; onlyWhenSpatial < 2; ++onlyWhenSpatial) {
      const std::string initial = onlyWhenSpatial ? "absent" : normalised; // the host's own value
      // the default ranges are set (2 x 4); no coordinate system write while Plain or non-spatial;
      // a spatial type without a choice gives canonical
      resetHost(onlyWhenSpatial != 0);
      {
        TestDouble2DDescriptor d(h);
        CHECK(gDoubleWrites == 8);
        d.setDoubleType(OFX::eDoubleTypePlain);
        d.setDoubleType(OFX::eDoubleTypeScale);
        CHECK(gCoordWrites == 0 && coords() == initial);
        d.setDoubleType(OFX::eDoubleTypeXYAbsolute); // the host resets to normalised
        CHECK(coords() == canonical);
      }
      // normalised chosen before setDoubleType(), possibly while the host doesn't have the property
      // yet, is written by setDoubleType() over the host's reset to canonical
      resetHost(onlyWhenSpatial != 0, kOfxParamCoordinatesCanonical);
      {
        TestDouble2DDescriptor d(h);
        d.setDefaultCoordinateSystem(OFX::eCoordinatesNormalised);
        CHECK(coords() == initial);
        d.setDoubleType(OFX::eDoubleTypeXY);
        CHECK(coords() == normalised);
      }
    }
    // normalised chosen after setDoubleType() is written at once, and survives a later type change
    resetHost(false, kOfxParamCoordinatesCanonical);
    {
      TestDouble2DDescriptor d(h);
      d.setDoubleType(OFX::eDoubleTypeXY);
      CHECK(coords() == canonical);
      d.setDefaultCoordinateSystem(OFX::eCoordinatesNormalised);
      CHECK(coords() == normalised);
      d.setDoubleType(OFX::eDoubleTypeXYAbsolute); // the host resets to canonical
      CHECK(coords() == normalised);
    }
    OFX::Private::gPropSuite = saved;
  }

  OfxStatus numCPUs4(unsigned int* n) { *n = 4; return kOfxStatOK; }

  struct CountingProcessor : public OFX::MultiThread::Processor {
    int calls = 0;
    unsigned int lastThreads = 0;
    void multiThreadFunction(unsigned int /*threadID*/, unsigned int nThreads) override { ++calls; lastThreads = nThreads; }
  };

  // ClipPreferencesSetter::setPixelAspectRatio(): outArgs holds the host's PAR (1); the host
  // answers a write with gParSetStatus
  OfxStatus gParSetStatus = kOfxStatOK;
  int gParSets = 0;
  OfxStatus parGetDouble(OfxPropertySetHandle, const char*, int, double* v) { *v = 1.; return kOfxStatOK; }
  OfxStatus parSetDouble(OfxPropertySetHandle, const char*, int, double) { ++gParSets; return gParSetStatus; }
  OfxStatus parGetDimension(OfxPropertySetHandle, const char*, int*) { return kOfxStatErrUnknown; }

  // the constructor is protected
  struct TestClip : public OFX::Clip {
    explicit TestClip(OfxPropertySetHandle h) : OFX::Clip(NULL, kOfxImageEffectOutputClipName, NULL, h) {}
  };

  void testSetPixelAspectRatio()
  {
    OfxPropertySuiteV1 suite = OfxPropertySuiteV1();
    suite.propGetDouble = parGetDouble;
    suite.propSetDouble = parSetDouble;
    suite.propGetInt = propGetInt; // (for the clip property validation of debug builds)
    suite.propGetString = propGetString;
    suite.propGetDimension = parGetDimension;
    OfxPropertySuiteV1* saved = OFX::Private::gPropSuite;
    OFX::Private::gPropSuite = &suite;
    int dummy = 0;
    OfxPropertySetHandle h = reinterpret_cast<OfxPropertySetHandle>(&dummy);
    TestClip clip(h);
    std::map<std::string, std::string> none, pars;
    pars[kOfxImageEffectOutputClipName] = "OfxImageClipPropPAR_Output";
    OFX::ClipPreferencesSetter setter(OFX::PropertySet(h), none, none, pars);

    // the host's PAR (openfx-io GenericReader's 1 in a host without multiple clip PARs): no write
    gParSets = 0;
    setter.setPixelAspectRatio(clip, 1.);
    CHECK(gParSets == 0 && setter.didSomething());

    // another PAR: written; refused (read-only) or unknown to the host, a warning, not an exception
    for (OfxStatus st : {kOfxStatOK, kOfxStatErrValue, kOfxStatErrUnknown, kOfxStatErrUnsupported}) {
      gParSetStatus = st;
      gParSets = 0;
      bool caught = false;
      try {
        setter.setPixelAspectRatio(clip, 2.);
      } catch (...) {
        caught = true;
      }
      CHECK(gParSets == 1 && !caught);
    }

    // other failures throw
    gParSetStatus = kOfxStatErrBadHandle;
    bool caught = false;
    try {
      setter.setPixelAspectRatio(clip, 2.);
    } catch (const OFX::Exception::Suite&) {
      caught = true;
    }
    CHECK(caught);

    gParSetStatus = kOfxStatOK;
    OFX::Private::gPropSuite = saved;
  }

  void testThreadSuiteWithoutFunctions()
  {
    // the thread suite of a host that has only some of its functions (Sony Catalyst Edit has no
    // mutex functions): multiThread() runs in the calling thread instead of calling NULL
    OfxMultiThreadSuiteV1 suite = OfxMultiThreadSuiteV1();
    OfxMultiThreadSuiteV1* saved = OFX::Private::gThreadSuite;
    OFX::Private::gThreadSuite = &suite;
    CHECK(OFX::MultiThread::getNumCPUs() == 1); // no multiThreadNumCPUs
    CHECK(OFX::MultiThread::getThreadIndex() == 0); // no multiThreadIndex
    CHECK(!OFX::MultiThread::isSpawnedThread());
    suite.multiThreadNumCPUs = numCPUs4;
    CHECK(OFX::MultiThread::getNumCPUs() == 4);
    CountingProcessor p;
    bool caught = false;
    try {
      p.multiThread(); // 4 CPUs, but no multiThread function
    } catch (...) {
      caught = true;
    }
    CHECK(!caught);
    CHECK(p.calls == 1 && p.lastThreads == 1);
    OFX::Private::gThreadSuite = saved;
  }

  // whether another thread gets the mutex with tryLock() (and then releases it)
  bool otherThreadTryLock(OFX::MultiThread::Mutex& m)
  {
    bool got = false;
    std::thread t([&] { got = m.tryLock(); if (got) { m.unlock(); } });
    t.join();
    return got;
  }

  void testMutexWithoutMutexFunctions()
  {
    // a thread suite without the mutex functions (Sony Catalyst Edit)
    OfxMultiThreadSuiteV1 suite = OfxMultiThreadSuiteV1();
    OfxMultiThreadSuiteV1* saved = OFX::Private::gThreadSuite;
    OFX::Private::gThreadSuite = &suite;
    {
      OFX::MultiThread::Mutex m;
      CHECK(otherThreadTryLock(m));
      m.lock();
      CHECK(m.tryLock()); // nested in the holding thread
      CHECK(!otherThreadTryLock(m));
      m.unlock();
      CHECK(!otherThreadTryLock(m));
      m.unlock();
      CHECK(otherThreadTryLock(m));
    }
    {
      OFX::MultiThread::Mutex p(1); // held by the creating thread
      CHECK(!otherThreadTryLock(p));
      p.unlock();
      CHECK(otherThreadTryLock(p));
    }
    {
      OFX::MultiThread::Mutex n(-1); // "This can be negative": free until the count is positive
      n.lock(); // count 0
      CHECK(otherThreadTryLock(n));
      n.lock(); // count 1
      CHECK(!otherThreadTryLock(n));
      n.unlock();
      n.unlock();
    }
    {
      OFX::MultiThread::Mutex q(2); // destroyed while locked: harmless
    }
    OFX::Private::gThreadSuite = saved;
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

} // namespace

int main()
{
  testNullInteractAction();
  testAutoMutexDestructorDoesNotThrow();
  testEditBlock();
  testDefineParamTypeMismatchThrows();
  testLazyRegistration();
  testPropertyDefaults();
  testDefaultCoordinateSystem();
  testSetPixelAspectRatio();
  testThreadSuiteWithoutFunctions();
  testMutexWithoutMutexFunctions();
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
