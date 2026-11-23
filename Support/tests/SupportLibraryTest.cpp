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
  - define*Param() throws OFX::Exception::TypeRequest when a param of the same name but another
    type was already defined (it returned NULL in release builds);
  - with OFX_EXTENSIONS_NATRON, OFX::Private::decodeNativeOverlayHandle(), which decodes the host's
    kNatronOfxPropNativeOverlays strings (ofxNatron.h) into ImageEffectHostDescription::nativeInteracts.

  The null action through an image effect's entry point (OfxPlugin::mainEntry) is tested on a
  plug-in bundle, in Support/Plugins/tests/test_entry_point.py.

  Prints one line per failed check, and exits with 0 if all passed.
*/

#include <cstdio>
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
