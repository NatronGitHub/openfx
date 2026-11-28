// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  Self-test of Support library code that needs no host (target support-library-test, built with
  -DBUILD_EXAMPLE_PLUGINS=ON). This file has the functional tests (each one's comment gives its
  PR); the regression tests, each for a fixed bug, are in regression/SupportLibraryRegressions.cpp,
  linked into the same program and run after these:

  - OFX::ParamSet::EditBlock's options (eBeginOnChange, eHostErrorsIgnore) begin and end the edit
    block when they should;
  - lazy registration (a static OFX::PluginFactories(f) object): the function set from a static
    initializer is called once, at the first OfxGetNumberOfPlugins(), and its factories are the
    plug-ins of the binary;
  - the accessors of a property the host may not have (OFX::Private::propGetIntOr() and the
    others, ofxsProperty.cpp): a property the host doesn't have gives the default, or writes
    nothing; other failures throw; propSetStringIfSupported() returns whether the host took it;
  - OFX::Param::getUseHostNativeOverlayHandle(): kOfxParamPropUseHostOverlayHandle's value on the
    instance, or false, without an exception, if the host doesn't have it;
  - OFX::MultiThread::Processor::multiThread() and getNumCPUs() with a thread suite that lacks
    multiThread or multiThreadNumCPUs: single-threaded, no crash;
  - OFX::MultiThread::Mutex (built with OFX_USE_MULTITHREAD_MUTEX) without the host's mutex
    functions: a lock count, nested in the holding thread, positive or negative initially, with
    two threads that lock and unlock in turn, and destroyed while locked;
  - the double param descriptors' default coordinate system: no write while Plain or non-spatial;
    setDoubleType() to a spatial type writes the plug-in's setDefaultCoordinateSystem() choice,
    made before or after, or canonical, over a host's reset (Nuke: normalised);
  - OFX::MultiThread::ThreadScope: nullptr outside any scope, nesting, restored when an exception
    leaves a scope, two independent kinds, and per-thread values on several threads at once;
  - with OFX_EXTENSIONS_NATRON, OFX::Private::decodeNativeOverlayHandle(), which decodes the host's
    kNatronOfxPropNativeOverlays strings (ofxNatron.h) into ImageEffectHostDescription::nativeInteracts.

  The regression tests: a null action passed to the interact entry point; AutoMutexT's and
  EditBlock's destructors don't throw; define*Param() of an existing name with another type
  throws; setPixelAspectRatio() doesn't write the host's PAR again; on macOS, this binary has no
  thread-local variables (#385).

  The null action through an image effect's entry point (OfxPlugin::mainEntry) is tested on a
  plug-in bundle, in Support/Plugins/tests/regression/test_entry_point.py.

  Prints one line per failed check, the number of checks of each kind, and exits with 0 if all
  passed.
*/

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

#include "SupportLibraryTest.h"
#include "ofxsInteract.h"
#include "ofxsMultiThread.h"

using namespace SupportLibraryTest;

namespace {

  // #304 (45578b69): EditBlock's options (the no-throw destructor is a regression test)
  void testEditBlock()
  {
    OfxParameterSuiteV1 suite = OfxParameterSuiteV1();
    suite.paramEditBegin = editBegin;
    suite.paramEditEnd = editEnd;
    OfxParameterSuiteV1* saved = OFX::Private::gParamSuite;
    OFX::Private::gParamSuite = &suite;
    TestEditParamSet set;
    bool caught = false;

    // by default, a failed begin throws from the constructor, and there is no end
    resetEditSuite(kOfxStatFailed, kOfxStatOK);
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

  // lazy registration (#306): one plug-in, registered at the first OfxGetNumberOfPlugins()
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

  /// #306.
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

  // the accessors of a property an OFX 1.1 host may not have (#351): a property suite whose
  // functions return gPropStatus (and 7, 0.5, "set" on success)
  OfxStatus gPropStatus = kOfxStatOK;
  int gPropSets = 0;
  OfxStatus propSetInt(OfxPropertySetHandle, const char*, int, int) { ++gPropSets; return gPropStatus; }
  OfxStatus propSetDouble(OfxPropertySetHandle, const char*, int, double) { ++gPropSets; return gPropStatus; }
  OfxStatus propSetString(OfxPropertySetHandle, const char*, int, const char*) { ++gPropSets; return gPropStatus; }
  OfxStatus propSetPointer(OfxPropertySetHandle, const char*, int, void*) { ++gPropSets; return gPropStatus; }
  OfxStatus propGetInt(OfxPropertySetHandle, const char*, int, int* v) { *v = 7; return gPropStatus; }
  OfxStatus propGetDouble(OfxPropertySetHandle, const char*, int, double* v) { *v = 0.5; return gPropStatus; }
  OfxStatus propGetString(OfxPropertySetHandle, const char*, int, const char** v) { *v = "set"; return gPropStatus; }

  /// #351 (OFX 1.1 hosts).
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

    // propSetStringIfSupported(): whether the host took the value, whatever the throw flag
    for (bool throwOnUnsupported : {true, false}) {
      OFX::PropertySet::setThrowOnUnsupportedProperties(throwOnUnsupported);
      for (OfxStatus st : {kOfxStatOK, kOfxStatErrUnknown, kOfxStatErrValue, kOfxStatErrBadHandle}) {
        gPropStatus = st;
        CHECK(propSetStringIfSupported(props, "p", "value") == (st == kOfxStatOK));
      }
    }
    OFX::PropertySet::setThrowOnUnsupportedProperties(true);

    gPropStatus = kOfxStatOK;
    OFX::Private::gPropSuite = saved;
  }

  // Param::getUseHostNativeOverlayHandle() (#364) on an instance: the host has
  // kOfxParamPropUseHostOverlayHandle (gOverlayValue), or not (kOfxStatErrUnknown)
  OfxStatus gOverlayStatus = kOfxStatOK;
  int gOverlayValue = 0;
  OfxStatus overlayGetInt(OfxPropertySetHandle, const char* name, int idx, int* v)
  {
    if (std::strcmp(name, kOfxParamPropUseHostOverlayHandle) != 0 || idx != 0) {
      return kOfxStatErrUnknown;
    }
    *v = gOverlayValue;
    return gOverlayStatus;
  }
  int gOverlayPropSet = 0;
  OfxStatus overlayGetPropertySet(OfxParamHandle, OfxPropertySetHandle* h)
  {
    *h = reinterpret_cast<OfxPropertySetHandle>(&gOverlayPropSet);
    return kOfxStatOK;
  }

  // the constructor is protected; eDummyParam has no property validation
  struct TestParam : public OFX::Param {
    TestParam() : OFX::Param(NULL, "p", OFX::eDummyParam, reinterpret_cast<OfxParamHandle>(&gOverlayPropSet)) {}
  };

  /// #364.
  void testUseHostNativeOverlayHandle()
  {
    OfxPropertySuiteV1 propSuite = OfxPropertySuiteV1();
    propSuite.propGetInt = overlayGetInt;
    OfxParameterSuiteV1 paramSuite = OfxParameterSuiteV1();
    paramSuite.paramGetPropertySet = overlayGetPropertySet;
    OfxPropertySuiteV1* savedProp = OFX::Private::gPropSuite;
    OfxParameterSuiteV1* savedParam = OFX::Private::gParamSuite;
    OFX::Private::gPropSuite = &propSuite;
    OFX::Private::gParamSuite = &paramSuite;
    {
      TestParam param;
      // present: its value
      gOverlayStatus = kOfxStatOK;
      gOverlayValue = 0;
      CHECK(!param.getUseHostNativeOverlayHandle());
      gOverlayValue = 1;
      CHECK(param.getUseHostNativeOverlayHandle());
      // absent: false ("Default - 0"), no exception
      for (OfxStatus st : {kOfxStatErrUnknown, kOfxStatErrUnsupported}) {
        gOverlayStatus = st;
        bool caught = false;
        bool v = true;
        try {
          v = param.getUseHostNativeOverlayHandle();
        } catch (...) {
          caught = true;
        }
        CHECK(!caught);
        CHECK(!v);
      }
    }
    gOverlayStatus = kOfxStatOK;
    OFX::Private::gPropSuite = savedProp;
    OFX::Private::gParamSuite = savedParam;
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

  // for the property validation of debug builds: the host has the string properties in gStrings
  // (dimension 1), and no other property
  OfxStatus recGetDimension(OfxPropertySetHandle, const char* name, int* count)
  {
    if (!gStrings.count(name)) {
      return kOfxStatErrUnknown;
    }
    *count = 1;
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

  // the double params' default coordinate system written by setDoubleType() (#354)
  void testDefaultCoordinateSystem()
  {
    OfxPropertySuiteV1 suite = OfxPropertySuiteV1();
    suite.propSetInt = propSetInt; // the ones of testPropertyDefaults()
    suite.propGetInt = propGetInt;
    suite.propGetDouble = propGetDouble;
    suite.propSetDouble = recSetDouble;
    suite.propSetString = recSetString;
    suite.propGetString = recGetString;
    suite.propGetDimension = recGetDimension;
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

  // a thread suite without some of its functions, as an OFX 1.1 host's (#351)
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

  // OFX::MultiThread::Mutex without the host's mutex functions (#351)
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
      CHECK(otherThreadTryLock(n));
      n.unlock(); // count -1
      CHECK(n.tryLock()); // count 0: no lock taken
      CHECK(otherThreadTryLock(n));
    }
    {
      // two threads that each lock and unlock once, interleaved: A locks (count 0), B locks (1),
      // A unlocks (0), B unlocks (-1); then the mutex is free for a third thread
      OFX::MultiThread::Mutex t(-1);
      std::atomic<int> step(0);
      // wait for step s, at most 10 s (a step that never comes fails instead of hanging)
      auto at = [&](int s) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (step.load() != s && std::chrono::steady_clock::now() < end) { std::this_thread::yield(); }
        CHECK(step.load() == s);
      };
      std::thread a([&] { at(0); t.lock(); step = 1; at(2); t.unlock(); step = 3; });
      std::thread b([&] { at(1); t.lock(); step = 2; at(3); t.unlock(); step = 4; });
      a.join();
      b.join();
      CHECK(otherThreadTryLock(t)); // count -1: it only counts up
      t.lock(); // count 0, no owner
      CHECK(otherThreadTryLock(t)); // the other thread takes it (count 1), then releases it
      t.unlock(); // count -1
    }
    {
      OFX::MultiThread::Mutex q(2); // destroyed while locked: harmless
    }
    OFX::Private::gThreadSuite = saved;
  }

  // two kinds of scope, as in openfx-io's GenericReader (render time, decoded window)
  struct TimeScope {
    explicit TimeScope(double t) : time(t), scope(this) {}
    double time;
    OFX::MultiThread::ThreadScope<TimeScope> scope;
  };
  struct WindowScope {
    explicit WindowScope(int w) : window(w), scope(this) {}
    int window;
    OFX::MultiThread::ThreadScope<WindowScope> scope;
  };

  double currentTime()
  {
    const TimeScope* s = OFX::MultiThread::ThreadScope<TimeScope>::current();
    return s ? s->time : -1.;
  }

  int currentWindow()
  {
    const WindowScope* s = OFX::MultiThread::ThreadScope<WindowScope>::current();
    return s ? s->window : -1;
  }

  // OFX::MultiThread::ThreadScope (#386): nullptr outside, nesting, restored on an exception, two
  // kinds, per thread (PluginUnloadTest's thread-scope-test.ofx tests it in a plug-in-like binary)
  void testThreadScope()
  {
    // before the first scope, the key doesn't exist yet
    CHECK(OFX::MultiThread::ThreadScope<TimeScope>::current() == nullptr);
    {
      TimeScope a(1.);
      CHECK(OFX::MultiThread::ThreadScope<TimeScope>::current() == &a);
      CHECK(currentWindow() == -1); // another kind
      {
        TimeScope b(2.); // nested
        WindowScope w(10);
        CHECK(currentTime() == 2. && currentWindow() == 10);
        try {
          TimeScope c(3.);
          WindowScope x(20);
          CHECK(currentTime() == 3. && currentWindow() == 20);
          throw std::runtime_error("leave the scopes");
        } catch (const std::runtime_error&) {
        }
        CHECK(currentTime() == 2. && currentWindow() == 10); // restored by the unwinding
      }
      CHECK(currentTime() == 1. && currentWindow() == -1);
      // another thread doesn't see this thread's scopes
      std::thread t([] { CHECK(currentTime() == -1. && currentWindow() == -1); });
      t.join();
    }
    CHECK(OFX::MultiThread::ThreadScope<TimeScope>::current() == nullptr);
    CHECK(OFX::MultiThread::ThreadScope<WindowScope>::current() == nullptr);

    // several threads in nested scopes at once, each sees its own values
    const int nThreads = 8;
    std::atomic<int> inside(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < nThreads; ++i) {
      threads.emplace_back([i, &inside] {
        CHECK(currentTime() == -1.);
        TimeScope a(i);
        for (int j = 0; j < 1000; ++j) {
          WindowScope w(i * 1000 + j);
          if (j == 0) {
            // wait (at most 10 s) until all threads are in a scope
            ++inside;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (inside.load() < nThreads && std::chrono::steady_clock::now() < end) { std::this_thread::yield(); }
            CHECK(inside.load() == nThreads);
          }
          TimeScope b(i + 0.5);
          CHECK(currentTime() == i + 0.5 && currentWindow() == i * 1000 + j);
        }
        CHECK(currentTime() == i && currentWindow() == -1);
      });
    }
    for (std::thread& t : threads) {
      t.join();
    }
    CHECK(currentTime() == -1. && currentWindow() == -1);
  }

#ifdef OFX_EXTENSIONS_NATRON
  typedef OFX::ImageEffectHostDescription::NativeOverlayHandle Handle;

  // decodeNativeOverlayHandle() (#219): ofxNatron.h's examples, '_' in names, bad strings refused
  // and the handle left unchanged
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
      report(!OFX::Private::decodeNativeOverlayHandle(bad[i], b), __FILE__, __LINE__,
             std::string("the bad string '") + bad[i] + "' is refused");
      CHECK(b.identifier == h.identifier && b.parameters == h.parameters);
    }
  }
#endif

} // namespace

int main()
{
  testEditBlock();
  testLazyRegistration();
  testPropertyDefaults();
  testUseHostNativeOverlayHandle();
  testDefaultCoordinateSystem();
  testThreadSuiteWithoutFunctions();
  testMutexWithoutMutexFunctions();
  testThreadScope();
#ifdef OFX_EXTENSIONS_NATRON
  testDecodeNativeOverlayHandle();
#else
  std::printf("support-library-test: OFX_EXTENSIONS_NATRON is off, decodeNativeOverlayHandle() not tested\n");
#endif
  runRegressions();
  const char* kinds[2] = { "functional", "regression" };
  for (int k = 0; k < 2; ++k) {
    std::printf("support-library-test: %s: %d of %d checks passed\n", kinds[k], gChecks[k] - gFailures[k], gChecks[k].load());
  }
  const int failures = gFailures[0] + gFailures[1];
  if (failures) {
    std::printf("support-library-test: %d failed check(s)\n", failures);
    return 1;
  }
  std::printf("support-library-test: all checks passed\n");
  return 0;
}
