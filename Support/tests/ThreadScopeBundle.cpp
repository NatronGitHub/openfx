// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  thread-scope-test.ofx: a binary with no plug-in that uses OFX::MultiThread::ThreadScope, for
  plugin-unload-test (Support/tests/PluginUnloadTest.cpp, #385). It is built twice: as a plug-in
  is (thread-scope-test.ofx: hidden visibility, the OFX entry points only), and with default
  visibility and no export list (thread-scope-test-default.ofx). The test checks that a binary
  that uses ThreadScope (its keys created, then deleted at dlclose()) unloads, and works again
  when it is loaded again.

  OfxGetNumberOfPlugins() runs nested scopes of two kinds and returns 0 if they behave, or -1
  (which plugin-unload-test reports as a failure).
*/

#include "ofxsMultiThread.h"

// (kinds with external linkage, as in most plug-ins: ThreadScope's statics stay in this binary
// also when these have default visibility; see ThreadScope)
namespace ThreadScopeBundle {

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

bool check()
{
  typedef OFX::MultiThread::ThreadScope<TimeScope> Times;
  typedef OFX::MultiThread::ThreadScope<WindowScope> Windows;
  if (Times::current() || Windows::current()) {
    return false;
  }
  bool ok = true;
  {
    TimeScope a(1.);
    {
      TimeScope b(2.);
      WindowScope w(3);
      ok = ok && Times::current() == &b && Windows::current() == &w;
    }
    ok = ok && Times::current() == &a && !Windows::current();
  }
  return ok && !Times::current() && !Windows::current();
}

} // namespace ThreadScopeBundle

OfxExport int OfxGetNumberOfPlugins(void)
{
  try {
    return ThreadScopeBundle::check() ? 0 : -1;
  } catch (...) {
    return -1;
  }
}

OfxExport OfxPlugin* OfxGetPlugin(int)
{
  return nullptr;
}
