#ifndef SUPPORT_LIBRARY_TEST_H
#define SUPPORT_LIBRARY_TEST_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// What the two parts of support-library-test share: the functional checks
// (SupportLibraryTest.cpp) and the regression checks (regression/SupportLibraryRegressions.cpp,
// each for a fixed bug, with its issue or PR). CHECK counts each part on its own, and a failed
// regression check prints "FAILED regression: ...", so that a CI failure says which kind failed.
// (No thread_local here: the binary must have no thread-local variables, testNoThreadLocalVariables.)

#include <atomic>
#include <cstdio>
#include <string>

#include "ofxsSupportPrivate.h"
#include "ofxsImageEffect.h"

namespace SupportLibraryTest {

  /// the checks and failures of the functional checks [0] and of the regression checks [1]
  /// (CHECK runs on worker threads too)
  inline std::atomic<int> gChecks[2] = { { 0 }, { 0 } };
  inline std::atomic<int> gFailures[2] = { { 0 }, { 0 } };
  /// > 0 while a regression test runs (on the main thread: they start no thread)
  inline std::atomic<int> gRegression(0);

  /// the checks made while it lives are regression checks (each test of
  /// regression/SupportLibraryRegressions.cpp opens one)
  struct RegressionSection {
    RegressionSection() { ++gRegression; }
    ~RegressionSection() { --gRegression; }
    RegressionSection(const RegressionSection&) = delete;
    RegressionSection& operator=(const RegressionSection&) = delete;
  };

  inline void report(bool ok, const char* file, int line, const std::string& what)
  {
    const int kind = gRegression > 0 ? 1 : 0;
    ++gChecks[kind];
    if (!ok) {
      ++gFailures[kind];
      std::printf("FAILED %s%s:%d: %s\n", kind ? "regression: " : "", file, line, what.c_str());
    }
  }

  // a param suite with only the edit block functions, which count their calls and return the
  // given statuses
  inline int gEditBegins = 0;
  inline int gEditEnds = 0;
  inline OfxStatus gEditBeginStatus = kOfxStatOK;
  inline OfxStatus gEditEndStatus = kOfxStatOK;
  inline OfxStatus editBegin(OfxParamSetHandle, const char*) { ++gEditBegins; return gEditBeginStatus; }
  inline OfxStatus editEnd(OfxParamSetHandle) { ++gEditEnds; return gEditEndStatus; }

  class TestEditParamSet : public OFX::ParamSet {};

  typedef OFX::ParamSet::EditBlock EditBlock;

  inline void resetEditSuite(OfxStatus beginStatus, OfxStatus endStatus)
  {
    gEditBegins = 0;
    gEditEnds = 0;
    gEditBeginStatus = beginStatus;
    gEditEndStatus = endStatus;
  }

  /// the regression tests (regression/SupportLibraryRegressions.cpp), run by main() after the
  /// functional ones
  void runRegressions();

} // SupportLibraryTest

#define CHECK(cond) \
  do { \
    SupportLibraryTest::report(static_cast<bool>(cond), __FILE__, __LINE__, #cond); \
  } while (0)

#endif // SUPPORT_LIBRARY_TEST_H
