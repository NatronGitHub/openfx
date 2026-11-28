// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  The regression tests of support-library-test: each one reproduces a bug that was found and
  fixed in the Support library, and names its issue or PR; it stays as long as the bug could come
  back. Run by main() (../SupportLibraryTest.cpp, which has the functional tests) after the
  functional tests; a failure prints "FAILED regression: ...".
*/

#include <map>
#include <stdexcept>
#include <string>

#include "../SupportLibraryTest.h"
#include "ofxsInteract.h"
#include "ofxsMultiThread.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

using namespace SupportLibraryTest;

namespace {

  class TestInteract : public OFX::OverlayInteract {
  public:
    TestInteract(OfxInteractHandle handle, OFX::ImageEffect* effect) : OFX::OverlayInteract(handle) { (void)effect; }
  };

  class TestInteractDescriptor : public OFX::DefaultEffectOverlayDescriptor<TestInteractDescriptor, TestInteract> {};

  /// #219 (8a3428e6): a NULL action passed to the interact entry point returns kOfxStatFailed
  /// (std::string(NULL) is undefined behaviour). The image effect entry point's twin is
  /// Support/Plugins/tests/regression/test_entry_point.py.
  void testNullInteractAction()
  {
    RegressionSection section;
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

  /// #219: OFX::MultiThread::AutoMutexT's destructor doesn't throw (std::terminate()) when the
  /// mutex's unlock() throws; an explicit unlock() still throws, and the destructor doesn't unlock
  /// again
  void testAutoMutexDestructorDoesNotThrow()
  {
    RegressionSection section;
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

  /// #304 (45578b69): OFX::ParamSet::EditBlock's destructor doesn't throw (std::terminate()) when
  /// the host's paramEditEnd() fails, and ParamSet::endEditBlock() still reports the failure (the
  /// options are functional checks, testEditBlock() in SupportLibraryTest.cpp)
  void testEditBlockDestructorDoesNotThrow()
  {
    RegressionSection section;
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

    OFX::Private::gParamSuite = saved;
  }

  // a param set with a param "p" already defined, without a host (no param suite is called)
  class TestParamSet : public OFX::ParamSetDescriptor {
  public:
    TestParamSet() { _definedParams["p"] = new OFX::DummyParamDescriptor("p"); }
  };

  /// #282, fixes #257: define*Param() throws OFX::Exception::TypeRequest when a param of the same
  /// name but another type was already defined (it returned the other type's descriptor)
  void testDefineParamTypeMismatchThrows()
  {
    RegressionSection section;
    TestParamSet set;
    bool thrown = false;
    try {
      set.defineIntParam("p");
    } catch (const OFX::Exception::TypeRequest&) {
      thrown = true;
    }
    CHECK(thrown);
  }

  // the property suite functions the clip property validation of debug builds calls
  OfxStatus propGetInt(OfxPropertySetHandle, const char*, int, int* v) { *v = 7; return kOfxStatOK; }
  OfxStatus propGetString(OfxPropertySetHandle, const char*, int, const char** v) { *v = "set"; return kOfxStatOK; }

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

  /// #355 (5e0963ce): ClipPreferencesSetter::setPixelAspectRatio(): the host's PAR isn't written
  /// again (openfx-io GenericReader in a host without multiple clip PARs), and a refused or
  /// unknown PAR is a warning, not an exception; other failures throw
  void testSetPixelAspectRatio()
  {
    RegressionSection section;
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

#ifdef __APPLE__
  // dyld never unloads an image that has thread-local variables (MachOAnalyzer::neverUnload()):
  // the Support library must have none, so that plug-ins built with it can be unloaded
  /// #385 (#386): this binary, which links the Support library (with OFX_USE_MULTITHREAD_MUTEX,
  /// a configuration no plug-in has), has no thread-local variables (MH_HAS_TLV_DESCRIPTORS).
  /// PluginUnloadTest checks the same on every example plug-in.
  void testNoThreadLocalVariables()
  {
    RegressionSection section;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
      const struct mach_header* h = _dyld_get_image_header(i);
      if (h && h->filetype == MH_EXECUTE) {
        report(!(h->flags & MH_HAS_TLV_DESCRIPTORS), __FILE__, __LINE__,
               std::string(_dyld_get_image_name(i)) + " has no thread-local variables (MH_HAS_TLV_DESCRIPTORS)");
        return;
      }
    }
    report(false, __FILE__, __LINE__, "an executable image found");
  }
#endif

} // namespace

void SupportLibraryTest::runRegressions()
{
  testNullInteractAction();
  testAutoMutexDestructorDoesNotThrow();
  testEditBlockDestructorDoesNotThrow();
  testDefineParamTypeMismatchThrows();
  testSetPixelAspectRatio();
#ifdef __APPLE__
  testNoThreadLocalVariables();
#endif
}
