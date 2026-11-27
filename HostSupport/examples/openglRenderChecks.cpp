// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// openglRenderChecks: checks of openfx::HostSupportOpenGL (ofxhOpenGLContext.h,
/// ofxhOpenGLTexture.h, ofxhOpenGLRender.h), on hostDemo's host classes:
/// - without OpenGL: the backend names, the context pool (one key on two threads, a failing
///   factory) and CurrentContext (nesting, release(), one context per thread), on fake contexts;
/// - with an OpenGL context: the renderer list, the context restored by createContext(), a context
///   made current after the thread that created it ended (WGL: a new window), texture
///   upload and readback (all depths and components, padded and negative rowBytes, partial
///   rectangles), Alpha textures sampled as (0, 0, 0, a), clear and render target;
/// - with Examples/OpenGL (com.genarts:OpenGLSamplePlugin, from OFX_PLUGIN_PATH): OpenGL renders
///   in RenderSession compared with the input (RGBA and Alpha; the output in a depth of the
///   plug-in's kOfxOpenGLPropPixelDepth), kOfxActionOpenGLContextAttached and Detached (their
///   pairing, and the context really current when they are sent, also on two threads crosswise
///   and in a nested scope), one render action at a time per context (a fetch rendering on
///   another thread gets another context or none, and a render with the same key waits), the
///   texture lifetime of clipFreeTexture, the cleanup of textures the plug-in didn't free, and
///   chooseRenderMode().
/// Without an OpenGL context or the plug-in, those checks are skipped, unless OFXH_REQUIRE=1 (CI).
/// Exits with 1 if a check fails.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "ofxOpenGLRender.h"

#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLRender.h"
#include "ofxhOpenGLTexture.h"
#include "ofxhPluginCache.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
#include "hostDemoHostDescriptor.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif
#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // without it, rpcndr.h defines small
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace OFX::Host;
namespace GL = OFX::Host::OpenGL;

namespace {

  int gFailures = 0;

  void check(const std::string &what, bool ok)
  {
    std::printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
    gFailures += ok ? 0 : 1;
  }

  bool required()
  {
    const char *r = std::getenv("OFXH_REQUIRE");
    return r && std::string(r) == "1";
  }

  void skip(const std::string &what)
  {
    if (required()) {
      check(what + " (OFXH_REQUIRE=1)", false);
    } else {
      std::printf("skipped: %s\n", what.c_str());
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // without OpenGL

  /// a context that is always made current, with no OpenGL
  class FakeContext : public GL::Context {
  public:
    std::atomic<int> made{ 0 }, done{ 0 }; ///< makeCurrentImpl() and doneCurrentImpl() calls
    std::atomic<bool> fail{ false };       ///< makeCurrentImpl() fails on failThread
    std::thread::id failThread;
    FakeContext() : GL::Context(GL::Hooks()) {}
    void *nativeContext() const override { return nullptr; }
  protected:
    bool makeCurrentImpl() override
    {
      if (fail && std::this_thread::get_id() == failThread) {
        return false;
      }
      ++made;
      return true;
    }
    void doneCurrentImpl() override { ++done; }
    GL::ProcAddress backendProcAddress(const char *) const override { return nullptr; }
  };

  void checkBackendNames()
  {
    for (GL::Backend b : { GL::Backend::EGLSurfaceless, GL::Backend::EGLDevice, GL::Backend::CGL, GL::Backend::WGL }) {
      GL::Backend parsed = GL::Backend::CGL;
      check(std::string("backendFromName(backendName(") + GL::backendName(b) + "))",
            GL::backendFromName(GL::backendName(b), parsed) && parsed == b);
    }
    GL::Backend parsed;
    check("backendFromName(\"glx\") = false (not a backend yet)", !GL::backendFromName("glx", parsed));
  }

  void checkPool()
  {
    std::atomic<int> created(0);
    GL::ContextPool pool([&] { ++created; return std::unique_ptr<GL::Context>(new FakeContext); }, 2);
    int a = 0, b = 0, c = 0;
    GL::Context *ca = pool.acquire(&a);
    check("acquire(a): a context", ca != nullptr && created == 1);
    check("acquire(a) again: the same context (one context per render key)", pool.acquire(&a) == ca && created == 1);
    GL::Context *cb = pool.acquire(&b);
    check("acquire(b): another context", cb != nullptr && cb != ca && created == 2);

    // a third key waits: the maximum is 2
    std::atomic<bool> done(false);
    GL::Context *cc = nullptr;
    std::thread t([&] { cc = pool.acquire(&c); done = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check("acquire(c) waits while both contexts are held", !done);
    pool.release(&a);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check("acquire(c) still waits: a holds its context twice", !done);
    pool.release(&a);
    t.join();
    check("acquire(c) gets a's context once a released it", done && cc == ca && created == 2 && pool.size() == 2);

    // abort: woken by wake(), the waiter checks its predicate
    std::atomic<bool> stop(false);
    GL::Context *aborted = ca;
    done = false;
    int d = 0;
    std::thread t2([&] { aborted = pool.acquire(&d, [&] { return stop.load(); }); done = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check("acquire(d, abort) waits", !done);
    stop = true;
    pool.wake();
    t2.join();
    check("acquire(d, abort): nullptr once abort() is true and wake() is called", done && aborted == nullptr);

    // an idle context goes back to the key that held it last
    pool.release(&b);
    pool.release(&c);
    check("acquire(b) after both are idle: b's last context", pool.acquire(&b) == cb);
    pool.release(&b);

    GL::ContextPool failing([] { return std::unique_ptr<GL::Context>(); }, 2);
    check("acquire() with a failing factory and no context: nullptr", failing.acquire(&a) == nullptr);

    // one key on two threads while its context is created (tile threads of one render tree)
    {
      std::atomic<int> n(0);
      GL::ContextPool slow([&] {
        ++n;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return std::unique_ptr<GL::Context>(new FakeContext);
      }, 1);
      GL::Context *first = nullptr, *second = nullptr;
      std::thread t1([&] { first = slow.acquire(&a); });
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::thread t2([&] { second = slow.acquire(&a); });
      t1.join();
      t2.join();
      check("one key on two threads during creation: the same context for both", first && first == second && n == 1);
      slow.release(&a);
      slow.release(&a);
      check("... and released twice, it is idle for another key", slow.acquire(&b) == first);
      slow.release(&b);
    }

    // a factory failure doesn't lower the maximum for good
    {
      std::atomic<int> calls(0);
      GL::ContextPool flaky([&] {
        return ++calls == 1 ? std::unique_ptr<GL::Context>() : std::unique_ptr<GL::Context>(new FakeContext);
      }, 2);
      check("a transient factory failure: nullptr", flaky.acquire(&a) == nullptr);
      GL::Context *f1 = flaky.acquire(&a), *f2 = flaky.acquire(&b);
      check("... then the factory is called again, up to the maximum", f1 && f2 && f1 != f2 && flaky.size() == 2);
      flaky.release(&a);
      flaky.release(&b);
    }
  }

  void checkCurrentContext()
  {
    FakeContext context;
    check("not current before a CurrentContext", !context.isCurrent());
    std::atomic<bool> released(false), otherWasCurrent(false), otherSawRelease(false);
    std::thread other;
    {
      GL::CurrentContext current(context);
      check("CurrentContext: ok and current", current.ok() && context.isCurrent());
      {
        GL::CurrentContext nested(context);
        check("nested CurrentContext on the same thread: ok", nested.ok() && context.isCurrent());
      }
      check("still current after the nested scope", context.isCurrent());
      other = std::thread([&] {
        check("not current on another thread", !context.isCurrent());
        GL::CurrentContext there(context); // waits for the main thread
        otherSawRelease = released.load();
        otherWasCurrent = there.ok() && context.isCurrent();
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      released = true;
    }
    other.join();
    check("CurrentContext on another thread waits until the context is released", otherSawRelease && otherWasCurrent);
    check("not current after the scope", !context.isCurrent());

    // release() of a nested scope suspends the whole hold, reacquire() restores it
    {
      GL::CurrentContext outer(context);
      const int made = context.made, done = context.done;
      {
        GL::CurrentContext inner(context);
        inner.release();
        check("release() of a nested scope: not current, released by the backend",
              !context.isCurrent() && !inner.ok() && context.done == done + 1);
        bool tookIt = false;
        std::thread t([&] { GL::CurrentContext there(context); tookIt = there.ok(); });
        t.join();
        check("... another thread can then make it current", tookIt);
        check("reacquire(): current again", inner.reacquire() && inner.ok() && context.isCurrent());
      }
      check("the enclosing scope still holds it after the nested one", context.isCurrent() && context.made == made + 2);
      // and a suspended nested scope that ends gives the context back to the enclosing one
      {
        GL::CurrentContext inner(context);
        inner.release();
      }
      check("a nested scope that ends released: current again for the enclosing scope", context.isCurrent());
      FakeContext second;
      GL::CurrentContext refused(second);
      check("heldContext(): the context of the enclosing scope", GL::heldContext() == &context);
      check("a scope on a second context while the thread holds one: refused", !refused.ok() && !second.isCurrent() &&
            context.isCurrent() && second.made == 0);
    }
  }

  /// suspend(): the context is current nowhere but reserved to the render; a render nested on
  /// the thread uses it; another thread waits until the render ends; a scope of this thread on
  /// another context doesn't wait for it
  void checkSuspend()
  {
    FakeContext context, busyElsewhere;
    std::atomic<bool> otherGot(false), busyHeld(false), stop(false);
    std::thread holder([&] {
      GL::CurrentContext held(busyElsewhere);
      busyHeld = true;
      while (!stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
    while (!busyHeld) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::thread other;
    {
      GL::CurrentContext render(context);
      render.suspend();
      check("suspend(): current nowhere, reserved by this thread", !context.isCurrent() && GL::reservedContext() == &context &&
            GL::heldContext() == nullptr);
      other = std::thread([&] { GL::CurrentContext there(context); otherGot = there.ok(); });
      {
        GL::CurrentContext nested(context);
        check("a render nested on the thread uses the reserved context", nested.ok() && context.isCurrent());
      }
      check("after the nested render: current nowhere again, still reserved", !context.isCurrent() && GL::reservedContext() == &context);
      {
        GL::CurrentContext second(busyElsewhere);
        check("a scope on another context, held by another thread, fails at once while this thread reserves one", !second.ok());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      check("another thread waits while the render is suspended", !otherGot);
      check("resume(): current again", render.resume() && render.ok() && GL::reservedContext() == nullptr);
    }
    other.join();
    check("... and the other thread gets it once the render ends", otherGot);
    stop = true;
    holder.join();

    // nested reservations: c1 suspended, a nested render on c2 suspends and resumes c2, then a
    // scope on c1 again (it would wait for its own thread's reservation if c2's resume() forgot
    // c1's); and the pool's acquire() from a thread that reserves a context doesn't wait
    std::atomic<bool> finished(false);
    std::thread watchdog([&] {
      for (int i = 0; i < 200 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!finished) {
        check("nested reservations on two contexts: no deadlock (10 s)", false);
        std::fflush(stdout);
        std::_Exit(1);
      }
    });
    {
      FakeContext c1, c2;
      GL::ContextPool pool([] { return std::unique_ptr<GL::Context>(new FakeContext); }, 1);
      int k1 = 0, k2 = 0;
      GL::ContextPool::Lease lease(pool, &k1); // the pool's only context
      GL::CurrentContext r1(c1);
      r1.suspend();
      {
        GL::CurrentContext r2(c2);
        check("a nested render on another, free context while c1 is reserved", r2.ok());
        r2.suspend();
        check("... its suspend(): c2 is the innermost reservation", GL::reservedContext() == &c2);
        check("... its resume(): c1 is reserved again", r2.resume() && GL::reservedContext() == &c1);
      }
      {
        GL::CurrentContext again(c1);
        check("a scope on c1 after the nested c2 render: uses this thread's reservation", again.ok() && c1.isCurrent());
      }
      check("ContextPool::acquire() from a thread that reserves a context: nullptr at once (no context free)",
            pool.acquire(&k2) == nullptr);
      check("resume() of c1", r1.resume() && r1.ok() && GL::reservedContext() == nullptr);
    }
    // a nested render's resume() fails while the outer render still reserves the context: the
    // nested scopes' claims are given back, so the context is free once the outer render ends.
    // Natron's case: the nested render's own scope suspends; deeper: a scope nested in it does.
    for (int deeper = 0; deeper < 2; ++deeper) {
      FakeContext c1;
      c1.failThread = std::this_thread::get_id();
      const std::string what = deeper ? " (a scope nested in the nested render suspends)" : "";
      {
        GL::CurrentContext outer(c1);
        outer.suspend();
        {
          GL::CurrentContext nested(c1); // on the reserved context
          auto failResume = [&](GL::CurrentContext &suspending) { // (the innermost scope)
            suspending.suspend();
            c1.fail = true;
            check("a nested render's failed resume()" + what, !suspending.resume());
            c1.fail = false;
          };
          if (deeper) {
            GL::CurrentContext inner(c1);
            failResume(inner);
          } else {
            failResume(nested);
          }
        }
        check("... then the outer render resumes" + what, outer.resume() && outer.ok());
      }
      std::atomic<bool> got(false);
      std::thread t([&] { GL::CurrentContext there(c1); got = there.ok(); });
      t.join();
      check("... and once it ends, another thread gets the context" + what, got);
    }
    // lost twice (the nested render's resume() fails, then the outer one's), and lost then
    // thrown (as loadTexture() around a throwing getImage(): resume() fails, the exception goes
    // on): the context is free once the outer render ends
    for (int thrown = 0; thrown < 2; ++thrown) {
      FakeContext c1;
      c1.failThread = std::this_thread::get_id();
      const std::string what = thrown ? "lost, then thrown" : "lost twice";
      bool outerResumed = true, caught = !thrown;
      {
        GL::CurrentContext outer(c1);
        outer.suspend();
        {
          GL::CurrentContext nested(c1);
          nested.suspend();
          c1.fail = true;
          if (thrown) {
            try {
              try {
                throw std::runtime_error("getImage");
              } catch (...) {
                nested.resume(); // fails
                throw;
              }
            } catch (const std::runtime_error &) {
              caught = true;
            }
          } else {
            nested.resume(); // fails
          }
        }
        if (!thrown) {
          outerResumed = outer.resume(); // fails too (c1.fail)
        }
        c1.fail = false;
        if (thrown) {
          outerResumed = outer.resume();
        }
      }
      std::atomic<bool> got(false);
      std::thread t([&] { GL::CurrentContext there(c1); got = there.ok(); });
      t.join();
      check(what + ": " + (thrown ? "the exception goes on, the outer render resumes," : "both resume() fail,") +
            " and another thread then gets the context", (thrown ? outerResumed : !outerResumed) && caught && got);
    }
    finished = true;
    watchdog.join();
  }

  /// A reacquire() that fails (e.g. a lost context) loses the thread's hold: the enclosing scope
  /// isn't ok() any more, and its end must leave alone the context another thread holds then.
  void checkLostHold()
  {
    FakeContext context;
    context.failThread = std::this_thread::get_id();
    std::atomic<int> stage(0);
    std::atomic<bool> thirdGot(false);
    std::thread second;
    {
      GL::CurrentContext outer(context);
      {
        GL::CurrentContext inner(context);
        inner.release();
        context.fail = true;
        check("a failing reacquire(): false", !inner.reacquire() && !inner.ok());
      } // (its destructor's reacquire() fails too)
      check("... the enclosing scope isn't ok() any more", !outer.ok() && !context.isCurrent() && GL::heldContext() == nullptr);
      second = std::thread([&] {
        GL::CurrentContext held(context);
        stage = 1;
        while (stage < 2) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
      while (stage < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } // the enclosing scope ends while the second thread holds the context
    std::thread third([&] { GL::CurrentContext there(context); thirdGot = there.ok(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check("... and its end leaves the other thread's hold alone: a third thread waits", !thirdGot);
    stage = 2;
    second.join();
    third.join();
    check("... until the second thread releases it", thirdGot);
  }

  ////////////////////////////////////////////////////////////////////////////////
  // with an OpenGL context: textures

  typedef unsigned char GLboolean_;
  typedef GLboolean_ (OFXH_GLAPIENTRY *IsTextureFn)(unsigned int);
  typedef void (OFXH_GLAPIENTRY *ClearColorFn)(float, float, float, float);
  typedef void (OFXH_GLAPIENTRY *ClearFn)(unsigned int);
  typedef void (OFXH_GLAPIENTRY *EnumFn)(unsigned int);
  typedef void (OFXH_GLAPIENTRY *BindTextureFn)(unsigned int, unsigned int);
  typedef void (OFXH_GLAPIENTRY *TexEnviFn)(unsigned int, unsigned int, int);
  typedef void (OFXH_GLAPIENTRY *VoidFn)();
  typedef void (OFXH_GLAPIENTRY *Float2Fn)(float, float);
  typedef void (OFXH_GLAPIENTRY *ViewportFn)(int, int, int, int);

  /// the native context current on this thread (nullptr if none)
  void *currentNativeContext()
  {
#if defined(__APPLE__)
    return CGLGetCurrentContext();
#elif !defined(_WIN32)
    typedef void *(*GetCurrentContextFn)();
    static GetCurrentContextFn get = [] {
      void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
      return h ? reinterpret_cast<GetCurrentContextFn>(dlsym(h, "eglGetCurrentContext")) : nullptr;
    }();
    return get ? get() : nullptr;
#else
    return wglGetCurrentContext();
#endif
  }

  /// The thread that created a context ends while another thread has the context current: that
  /// thread still renders with it (WGL: on its own window, not on the creator's, which Windows
  /// destroys), and the context is then made current again on a third thread.
  void checkCreatorThreadEnds(const GL::Hooks &hooks)
  {
    typedef unsigned int (OFXH_GLAPIENTRY *GetErrorFn)();
    typedef void (OFXH_GLAPIENTRY *ClearColorFn)(float, float, float, float);
    // renders with the context current on this thread: a clear of its window's framebuffer on
    // WGL (the window of this thread), else a GL call without error
    auto renders = [](GL::Context &context) {
      const auto getError = reinterpret_cast<GetErrorFn>(context.getProcAddress("glGetError"));
      if (!getError) {
        return false;
      }
      while (getError() != 0) {
      }
#if defined(_WIN32)
      typedef void (OFXH_GLAPIENTRY *ClearFn)(unsigned int);
      typedef void (OFXH_GLAPIENTRY *BindFramebufferFn)(unsigned int, unsigned int);
      typedef void (OFXH_GLAPIENTRY *ReadPixelsFn)(int, int, int, int, unsigned int, unsigned int, void *);
      const auto clearColor = reinterpret_cast<ClearColorFn>(context.getProcAddress("glClearColor"));
      const auto clear = reinterpret_cast<ClearFn>(context.getProcAddress("glClear"));
      const auto bind = reinterpret_cast<BindFramebufferFn>(context.getProcAddress("glBindFramebuffer"));
      const auto readPixels = reinterpret_cast<ReadPixelsFn>(context.getProcAddress("glReadPixels"));
      if (!clearColor || !clear || !bind || !readPixels) {
        return false;
      }
      unsigned char pixel[4] = { 0, 0, 0, 0 };
      bind(0x8D40 /* GL_FRAMEBUFFER */, 0);
      clearColor(1.f, 0.f, 0.f, 1.f);
      clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
      readPixels(0, 0, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixel);
      return getError() == 0 && pixel[0] == 255 && pixel[1] == 0 && pixel[3] == 255;
#else
      const auto clearColor = reinterpret_cast<ClearColorFn>(context.getProcAddress("glClearColor"));
      if (!clearColor) {
        return false;
      }
      clearColor(0.f, 0.f, 0.f, 0.f);
      return getError() == 0;
#endif
    };
    std::unique_ptr<GL::Context> context;
    std::promise<void> created, held;
    std::future<void> createdFuture = created.get_future(), heldFuture = held.get_future();
    std::thread creator([&] {
      context = GL::createContext(hooks);
      created.set_value();
      heldFuture.wait();
    });
    createdFuture.wait();
    bool currentBefore = false, rendersAfter = false;
    std::thread holder([&] {
      if (!context) {
        held.set_value();
        return;
      }
      GL::CurrentContext current(*context);
      currentBefore = current.ok() && currentNativeContext() == context->nativeContext();
      held.set_value();
      creator.join(); // the creator ends while this thread has the context current
      rendersAfter = current.ok() && currentNativeContext() == context->nativeContext() && renders(*context);
    });
    holder.join();
    if (creator.joinable()) {
      creator.join();
    }
    check("the creator thread ends while another thread has the context current: a context", context != nullptr);
    if (!context) {
      return;
    }
    check("... current on the other thread", currentBefore);
    check("... which still renders with it after the creator ended", rendersAfter);
    GL::CurrentContext again(*context);
    check("... and the context is made current again on a third thread, and renders", again.ok() &&
          currentNativeContext() == context->nativeContext() && renders(*context));
  }

  /// an image rectangle with its own memory; rowBytes: the pixel size times the width plus pad
  /// pixels, negative if flipped (rows top-down in memory)
  struct TestImage {
    std::vector<unsigned char> memory;
    GL::ImageRect rect;
    TestImage(const OfxRectI &bounds, GL::PixelDepth depth, GL::Components comps, int pad, bool flipped)
    {
      const int ps = GL::bytesPerPixel(depth, comps);
      const int stride = (bounds.x2 - bounds.x1 + pad) * ps;
      const int h = bounds.y2 - bounds.y1;
      memory.assign(static_cast<size_t>(stride) * h, 0xCD);
      rect.bounds = bounds;
      rect.depth = depth;
      rect.components = comps;
      rect.rowBytes = flipped ? -stride : stride;
      rect.data = flipped ? memory.data() + static_cast<size_t>(stride) * (h - 1) : memory.data();
    }
    unsigned char *pixel(int x, int y) const
    {
      return static_cast<unsigned char *>(rect.data) + static_cast<ptrdiff_t>(y - rect.bounds.y1) * rect.rowBytes +
        static_cast<ptrdiff_t>(x - rect.bounds.x1) * GL::bytesPerPixel(rect.depth, rect.components);
    }
    int nComps() const { return rect.components == GL::Components::RGBA ? 4 : 1; }
    /// component c of (x, y) as a float (byte /255, short /65535)
    float get(int x, int y, int c) const
    {
      const unsigned char *p = pixel(x, y);
      switch (rect.depth) {
      case GL::PixelDepth::Byte: return p[c] / 255.f;
      case GL::PixelDepth::Short: { unsigned short v; std::memcpy(&v, p + 2 * c, 2); return v / 65535.f; }
      case GL::PixelDepth::Float: { float v; std::memcpy(&v, p + 4 * c, 4); return v; }
      default: return NAN;
      }
    }
    /// the value fill() gives (x, y, c): k/255 with k < 256
    static float pattern(int x, int y, int c) { return ((x * 7 + y * 13 + c * 31) & 255) / 255.f; }
    void fill()
    {
      for (int y = rect.bounds.y1; y < rect.bounds.y2; ++y) {
        for (int x = rect.bounds.x1; x < rect.bounds.x2; ++x) {
          unsigned char *p = pixel(x, y);
          for (int c = 0; c < nComps(); ++c) {
            const int k = (x * 7 + y * 13 + c * 31) & 255;
            if (rect.depth == GL::PixelDepth::Byte) {
              p[c] = static_cast<unsigned char>(k);
            } else if (rect.depth == GL::PixelDepth::Short) {
              const unsigned short v = static_cast<unsigned short>(k * 257);
              std::memcpy(p + 2 * c, &v, 2);
            } else {
              const float v = k / 255.f;
              std::memcpy(p + 4 * c, &v, 4);
            }
          }
        }
      }
    }
  };

  const char *depthName(GL::PixelDepth d) { return GL::pixelDepthToOfx(d); }

  void checkTextures(GL::Context &context)
  {
    const IsTextureFn isTexture = reinterpret_cast<IsTextureFn>(context.getProcAddress("glIsTexture"));
    check("glIsTexture", isTexture != nullptr);
    const OfxRectI bounds = { -3, 5, 13, 12 };
    // the CPU depths of HostSupport images (half textures from float images)
    const GL::PixelDepth cpuDepths[] = { GL::PixelDepth::Byte, GL::PixelDepth::Short, GL::PixelDepth::Float, GL::PixelDepth::Float };
    const GL::PixelDepth texDepths[] = { GL::PixelDepth::Byte, GL::PixelDepth::Short, GL::PixelDepth::Float, GL::PixelDepth::Half };
    for (int i = 0; i < 4; ++i) {
      for (GL::Components comps : { GL::Components::RGBA, GL::Components::Alpha }) {
        for (bool flipped : { false, true }) {
          const std::string what = std::string(depthName(texDepths[i])) + " texture from " + depthName(cpuDepths[i]) + " " +
            GL::componentsToOfx(comps) + (flipped ? ", negative rowBytes" : ", padded rowBytes");
          try {
            TestImage src(bounds, cpuDepths[i], comps, 3, flipped);
            src.fill();
            GL::Texture texture(context, bounds, texDepths[i], comps);
            texture.upload(src.rect);
            TestImage dst(bounds, cpuDepths[i], comps, 1, !flipped);
            texture.readback(dst.rect);
            // exact, but for the rounding of k/255 to half floats
            const float tolerance = texDepths[i] == GL::PixelDepth::Half ? 1e-3f : 1e-6f;
            bool same = true;
            for (int y = bounds.y1; y < bounds.y2 && same; ++y) {
              for (int x = bounds.x1; x < bounds.x2 && same; ++x) {
                for (int c = 0; c < src.nComps(); ++c) {
                  same = same && std::fabs(dst.get(x, y, c) - TestImage::pattern(x, y, c)) <= tolerance;
                }
              }
            }
            check(what + ": upload then readback gives the image", same);
            check(what + ": rowBytes of the texture", texture.rowBytes() == 16 * GL::bytesPerPixel(texDepths[i], comps));
          } catch (const std::exception &e) {
            check(what + ": " + e.what(), false);
          }
        }
      }
    }

    // an Alpha texture samples as (0, 0, 0, a): drawn with GL_REPLACE into an RGBA target
    try {
      const OfxRectI small = { 0, 0, 4, 4 };
      TestImage alphaImage(small, GL::PixelDepth::Float, GL::Components::Alpha, 0, false);
      alphaImage.fill();
      GL::Texture alpha(context, small, GL::PixelDepth::Float, GL::Components::Alpha);
      alpha.upload(alphaImage.rect);
      GL::Texture target(context, small, GL::PixelDepth::Float, GL::Components::RGBA);
      target.clear();
      target.bindAsRenderTarget();
      auto f = [&](const char *name) { return context.getProcAddress(name); };
      reinterpret_cast<ViewportFn>(f("glViewport"))(0, 0, 4, 4);
      reinterpret_cast<EnumFn>(f("glEnable"))(0x0DE1); // GL_TEXTURE_2D
      reinterpret_cast<BindTextureFn>(f("glBindTexture"))(0x0DE1, alpha.index());
      reinterpret_cast<TexEnviFn>(f("glTexEnvi"))(0x2300, 0x2200, 0x1E01); // GL_TEXTURE_ENV_MODE GL_REPLACE
      const Float2Fn texCoord = reinterpret_cast<Float2Fn>(f("glTexCoord2f"));
      const Float2Fn vertex = reinterpret_cast<Float2Fn>(f("glVertex2f"));
      reinterpret_cast<EnumFn>(f("glBegin"))(0x0007); // GL_QUADS, with the default (identity) matrices
      texCoord(0.f, 0.f); vertex(-1.f, -1.f);
      texCoord(1.f, 0.f); vertex(1.f, -1.f);
      texCoord(1.f, 1.f); vertex(1.f, 1.f);
      texCoord(0.f, 1.f); vertex(-1.f, 1.f);
      reinterpret_cast<VoidFn>(f("glEnd"))();
      reinterpret_cast<BindTextureFn>(f("glBindTexture"))(0x0DE1, 0);
      reinterpret_cast<EnumFn>(f("glDisable"))(0x0DE1);
      TestImage drawn(small, GL::PixelDepth::Float, GL::Components::RGBA, 0, false);
      target.readback(drawn.rect);
      bool ok = true;
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          ok = ok && drawn.get(x, y, 0) == 0.f && drawn.get(x, y, 1) == 0.f && drawn.get(x, y, 2) == 0.f &&
            std::fabs(drawn.get(x, y, 3) - TestImage::pattern(x, y, 0)) <= 1e-6f;
        }
      }
      check("an Alpha texture samples as (0, 0, 0, alpha)", ok);
    } catch (const std::exception &e) {
      check(std::string("Alpha sampling: ") + e.what(), false);
    }

    // partial rectangles, clear, render target, deletion
    try {
      unsigned int index = 0;
      {
        GL::Texture texture(context, bounds, GL::PixelDepth::Float, GL::Components::RGBA);
        index = texture.index();
        check("texture target GL_TEXTURE_2D, index valid", texture.target() == 0x0DE1 && isTexture && isTexture(index));
        texture.clear();
        TestImage part({ 0, 0, 20, 8 }, GL::PixelDepth::Float, GL::Components::RGBA, 0, false);
        part.fill();
        texture.upload(part.rect); // only (0..13, 5..8) is inside
        TestImage all(bounds, GL::PixelDepth::Float, GL::Components::RGBA, 0, false);
        texture.readback(all.rect);
        bool ok = true;
        for (int y = bounds.y1; y < bounds.y2; ++y) {
          for (int x = bounds.x1; x < bounds.x2; ++x) {
            const bool inside = x >= 0 && y < 8;
            ok = ok && all.get(x, y, 0) == (inside ? TestImage::pattern(x, y, 0) : 0.f) && all.get(x, y, 3) == (inside ? TestImage::pattern(x, y, 3) : 0.f);
          }
        }
        check("upload of a partly overlapping image: the overlap only (clear() gave 0 elsewhere)", ok);
        TestImage outside({ 10, 0, 20, 7 }, GL::PixelDepth::Float, GL::Components::RGBA, 0, false);
        texture.readback(outside.rect);
        const unsigned char untouched[4] = { 0xCD, 0xCD, 0xCD, 0xCD };
        check("readback to a partly overlapping image: the overlap only",
              outside.get(11, 6, 0) == TestImage::pattern(11, 6, 0) && std::memcmp(outside.pixel(15, 0), untouched, 4) == 0);

        const ClearColorFn clearColor = reinterpret_cast<ClearColorFn>(context.getProcAddress("glClearColor"));
        const ClearFn clear = reinterpret_cast<ClearFn>(context.getProcAddress("glClear"));
        texture.bindAsRenderTarget();
        clearColor(0.25f, 0.5f, 0.75f, 1.f);
        clear(0x4000);
        texture.readback(all.rect);
        check("render target: glClear draws into the texture",
              all.get(0, 7, 0) == 0.25f && all.get(0, 7, 1) == 0.5f && all.get(0, 7, 2) == 0.75f && all.get(0, 7, 3) == 1.f);
      }
      check("~Texture deletes the OpenGL texture", isTexture && !isTexture(index));
    } catch (const std::exception &e) {
      check(std::string("textures: ") + e.what(), false);
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // with Examples/OpenGL: renders

  GL::Context *gContexts[2] = { nullptr, nullptr };

  /// the name of the test context current on this thread, from the driver (not from the
  /// library's own bookkeeping)
  std::string currentContextName()
  {
    void *current = currentNativeContext();
    for (int i = 0; i < 2; ++i) {
      if (current && gContexts[i] && gContexts[i]->nativeContext() == current) {
        return "c" + std::to_string(i + 1);
      }
    }
    return "none";
  }

  class GLTestInstance;

  /// hostDemo's clip, with loadTexture() from the RenderSession of its instance
  class GLTestClip : public MyHost::MyClipInstance {
  public:
    GLTestClip(GLTestInstance *effect, ImageEffect::ClipDescriptor *desc);
    ImageEffect::Texture *loadTexture(OfxTime time, const char *format, const OfxRectD *optionalBounds) override;
    ImageEffect::Image *getImage(OfxTime time, const OfxRectD *optionalBounds) override;
  private:
    GLTestInstance *_glEffect;
  };

  /// hostDemo's instance: records the OpenGL context actions, with the context then current
  class GLTestInstance : public MyHost::MyEffectInstance {
  public:
    std::vector<std::string> actions;
    GL::RenderSession *session = nullptr;
    std::vector<unsigned int> inputTextures; ///< the textures loaded for input clips
    std::string outputTextureDepth;          ///< the depth of the output texture handle
    /// What the next input fetch does on another thread (a host rendering an input's tiles on a
    /// thread pool) with fetchPool: TryOwnKey, tryAcquire() with a key of its own (fetchContext:
    /// what it got, fetchRendered: it was current there); SameKey, acquire() with fetchKey and
    /// a CurrentContext, in a thread that fetchThread runs on (sameKeyDuringRender: it got the
    /// context while the render action was in progress)
    enum class Fetch { None, TryOwnKey, SameKey } fetch = Fetch::None;
    GL::ContextPool *fetchPool = nullptr;
    GL::ContextPool::Key fetchKey = nullptr;
    GL::Context *fetchContext = nullptr;
    bool fetchRendered = false;
    std::thread fetchThread;
    std::atomic<bool> inRender{ false }, sameKeyDuringRender{ false }, sameKeyGot{ false };

    GLTestInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context) {}

    ImageEffect::ClipInstance *newClipInstance(ImageEffect::Instance *, ImageEffect::ClipDescriptor *descriptor, int) override
    {
      return new GLTestClip(this, descriptor);
    }

    OfxStatus mainEntry(const char *action, const void *handle, Property::Set *inArgs, Property::Set *outArgs) override
    {
      if (std::strcmp(action, kOfxImageEffectActionRender) == 0) {
        inRender = true;
        const OfxStatus st = MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
        inRender = false;
        return st;
      }
      if (std::strcmp(action, kOfxActionOpenGLContextAttached) == 0) {
        actions.push_back("attached:" + currentContextName());
      } else if (std::strcmp(action, kOfxActionOpenGLContextDetached) == 0) {
        actions.push_back("detached:" + currentContextName());
      }
      return MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
    }
  };

  GLTestClip::GLTestClip(GLTestInstance *effect, ImageEffect::ClipDescriptor *desc)
    : MyHost::MyClipInstance(effect, desc), _glEffect(effect) {}

  ImageEffect::Texture *GLTestClip::loadTexture(OfxTime time, const char *format, const OfxRectD *optionalBounds)
  {
    if (!_glEffect->session) {
      return nullptr;
    }
    ImageEffect::Texture *t = _glEffect->session->loadTexture(*this, time, format, optionalBounds);
    if (t && !isOutput()) {
      _glEffect->inputTextures.push_back(static_cast<unsigned int>(t->getIntProperty(kOfxImageEffectPropOpenGLTextureIndex)));
    } else if (t) {
      _glEffect->outputTextureDepth = t->getStringProperty(kOfxImageEffectPropPixelDepth);
    }
    return t;
  }

  ImageEffect::Image *GLTestClip::getImage(OfxTime time, const OfxRectD *optionalBounds)
  {
    GLTestInstance &e = *_glEffect;
    const GLTestInstance::Fetch fetch = isOutput() ? GLTestInstance::Fetch::None : e.fetch;
    if (fetch == GLTestInstance::Fetch::TryOwnKey) {
      e.fetch = GLTestInstance::Fetch::None; // once
      std::atomic<bool> done(false);
      std::thread t([&] {
        int ownKey = 0;
        e.fetchContext = e.fetchPool->tryAcquire(&ownKey);
        if (e.fetchContext) {
          GL::CurrentContext current(*e.fetchContext);
          e.fetchRendered = current.ok() && currentNativeContext() == e.fetchContext->nativeContext();
          e.fetchPool->release(&ownKey);
        }
        done = true;
      });
      for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!done) {
        check("a fetch that renders on another thread during an OpenGL render: no deadlock (10 s)", false);
        std::fflush(stdout);
        std::_Exit(1);
      }
      t.join();
    } else if (fetch == GLTestInstance::Fetch::SameKey) {
      e.fetch = GLTestInstance::Fetch::None;
      // not waited for here: it must wait for the end of the render action
      e.fetchThread = std::thread([&e] {
        GL::Context *context = e.fetchPool->acquire(e.fetchKey);
        if (context) {
          GL::CurrentContext current(*context);
          e.sameKeyDuringRender = e.inRender.load();
          e.sameKeyGot = current.ok();
          e.fetchPool->release(e.fetchKey);
        }
      });
    }
    return MyHost::MyClipInstance::getImage(time, optionalBounds);
  }

  class GLTestHost : public MyHost::Host {
  public:
    ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc,
                                       const std::string &context) override
    {
      return new GLTestInstance(plugin, desc, context);
    }
  };

  struct RenderResult {
    OfxStatus status = kOfxStatFailed;
    int liveTextures = -1;              ///< after the render action
    bool outputTextureKept = false;     ///< after the plug-in freed its handle
    bool inputTexturesDeleted = false;  ///< by clipFreeTexture
    float maxError = INFINITY;          ///< vs the expected image
    float inputRange = 0.f;             ///< max - min of the input's red in the window
  };

  /// an OpenGL render of window at time 0 on context, of the input (a hostDemo input spec),
  /// compared with what Examples/OpenGL draws (scale and source_scale 1): the input, with a red
  /// rectangle from (10, 10)
  RenderResult glRender(GLTestInstance &instance, GL::Attachment &attachment, GL::Context &context,
                        const OfxRectI &window, const std::string &input, bool leakInputTexture = false)
  {
    RenderResult result;
    const IsTextureFn isTexture = reinterpret_cast<IsTextureFn>(context.getProcAddress("glIsTexture"));
    GL::CurrentContext current(context);
    GL::RenderSession session(attachment, current);
    result.status = session.status();
    if (!session.ok()) {
      return result;
    }
    const OfxPointD scale = { 1., 1. };
    const double par = instance.getProjectPixelAspectRatio(); // canonical x = pixel x * PAR
    const OfxRectD canonical = { window.x1 * par, double(window.y1), window.x2 * par, double(window.y2) };
    std::map<ImageEffect::ClipInstance *, OfxRectD> rois;
    instance.getRegionOfInterestAction(0., scale,
#ifdef OFX_EXTENSIONS_NUKE
                                       0,
#endif
                                       canonical, rois);
    instance.setRenderState(scale, window, rois);
    MyHost::MyClipInstance *output = dynamic_cast<MyHost::MyClipInstance *>(instance.getClip(kOfxImageEffectOutputClipName));
    MyHost::MyClipInstance *source = dynamic_cast<MyHost::MyClipInstance *>(instance.getClip(kOfxImageEffectSimpleSourceClipName));
    MyHost::InputSpec spec;
    std::string error;
    MyHost::parseInputSpec(input, spec, error);
    source->setInput(spec);
    instance.getClipPreferences(); // (the input's components may have changed)
    output->releaseOutputImage();
    ImageEffect::Image *outputImage = output->getImage(0., nullptr);
    session.setOutput(*outputImage, window);
    instance.session = &session;
    instance.inputTextures.clear();
#ifdef OFX_EXTENSIONS_NUKE
    std::list<std::string> planes(1, kFnOfxImagePlaneColour);
#endif
    result.status = instance.renderAction(0., kOfxImageFieldNone, window, scale, false, false,
                                          true, // openGLRender
#ifdef OFX_EXTENSIONS_NATRON
                                          attachment.contextData(),
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
    result.liveTextures = session.liveTextures();
    result.outputTextureKept = session.output() && isTexture(session.output()->index());
    result.inputTexturesDeleted = !instance.inputTextures.empty();
    for (unsigned int t : instance.inputTextures) {
      result.inputTexturesDeleted = result.inputTexturesDeleted && !isTexture(t);
    }
    if (leakInputTexture) {
      // a plug-in that doesn't call clipFreeTexture: the session deletes the texture
      ImageEffect::Texture *leaked = session.loadTexture(*source, 0., nullptr, nullptr);
      instance.inputTextures.assign(1, leaked ? static_cast<unsigned int>(leaked->getIntProperty(kOfxImageEffectPropOpenGLTextureIndex)) : 0u);
      result.liveTextures = session.liveTextures();
    }
    instance.session = nullptr;
    if (result.status == kOfxStatOK) {
      session.readOutput();
      ImageEffect::Image *input = source->getImage(0., &canonical);
      MyHost::MyImage *in = dynamic_cast<MyHost::MyImage *>(input);
      MyHost::MyImage *out = dynamic_cast<MyHost::MyImage *>(outputImage);
      if (in && out) {
        result.maxError = 0.f;
        float lo = INFINITY, hi = -INFINITY;
        for (int y = window.y1; y < window.y2; ++y) {
          for (int x = window.x1; x < window.x2; ++x) {
            const bool red = x - window.x1 >= 10 && y - window.y1 >= 10 && x - window.x1 < 200 && y - window.y1 < 100;
            const int n = out->getNComponents(); // 4, or 1 (Alpha: the red rectangle's alpha is 1)
            for (int c = 0; c < n; ++c) {
              const float expected = red ? (n == 1 || c == 0 || c == 3 ? 1.f : 0.f) : in->getComponentFloat(x, y, c);
              result.maxError = std::max(result.maxError, std::fabs(out->getComponentFloat(x, y, c) - expected));
            }
            lo = std::min(lo, in->getComponentFloat(x, y, 0));
            hi = std::max(hi, in->getComponentFloat(x, y, 0));
          }
        }
        result.inputRange = hi - lo;
      }
      if (input) {
        input->releaseReference();
      }
    }
    instance.clearRenderState();
    outputImage->releaseReference();
    return result;
  }

  /// a RenderSession of the instance on context (no render): ok()
  bool attachOn(GL::Attachment &attachment, GL::Context &context)
  {
    GL::CurrentContext current(context);
    GL::RenderSession session(attachment, current);
    return session.ok();
  }

  /// Two threads swap two instances between c1 and c2 at the same time (each holds the context
  /// the other's instance is attached to), and an instance switches context in a scope nested in
  /// another scope of the thread (a host rendering an input in getImage()). Detached and Attached
  /// must run with the right context really current, without deadlock.
  void checkCrosswiseAndNested(ImageEffect::ImageEffectPlugin *plugin, GLTestInstance &x, GL::Context &c1, GL::Context &c2)
  {
    GLTestInstance *y = dynamic_cast<GLTestInstance *>(plugin->createInstance(kOfxImageEffectContextFilter, NULL));
    check("a second instance", y != nullptr && y->createInstanceAction() != kOfxStatFailed);
    if (!y) {
      return;
    }
    {
      GL::Attachment ax(x), ay(*y);
      x.actions.clear();
      check("x attached to c1, y to c2", attachOn(ax, c1) && attachOn(ay, c2));
      std::atomic<int> done(0);
      bool sx = false, sy = false;
      std::thread t1([&] { sx = attachOn(ax, c2); ++done; });
      std::thread t2([&] { sy = attachOn(ay, c1); ++done; });
      for (int i = 0; i < 400 && done < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (done < 2) {
        check("two threads swapping instances between two contexts: no deadlock (20 s)", false);
        std::fflush(stdout);
        std::_Exit(1);
      }
      t1.join();
      t2.join();
      check("crosswise: x Detached with c1 current, Attached with c2 current",
            sx && x.actions == std::vector<std::string>({ "attached:c1", "detached:c1", "attached:c2" }));
      check("crosswise: y Detached with c2 current, Attached with c1 current",
            sy && y->actions == std::vector<std::string>({ "attached:c2", "detached:c2", "attached:c1" }));

      // nested: x is attached to c2; this thread holds c1 in two scopes
      {
        GL::CurrentContext outer(c1);
        {
          GL::CurrentContext inner(c1);
          GL::RenderSession session(ax, inner);
          check("nested scope: x Detached with c2 current, then Attached with c1 current",
                session.ok() && x.actions.size() == 5 && x.actions[3] == "detached:c2" && x.actions[4] == "attached:c1");
          check("nested scope: c1 current again in the session (driver and bookkeeping)",
                currentNativeContext() == c1.nativeContext() && c1.isCurrent());
        }
        check("nested scope: c1 still current for the enclosing scope", currentNativeContext() == c1.nativeContext() && c1.isCurrent());
      }
    }
    y->destroyInstanceAction();
    delete y;
  }

  /// One render action at a time per context: during the plug-in's fetch, a render on another
  /// thread gets another context (tryAcquire(), up to the maximum) or none (then the CPU), never
  /// the reserved one, and never waits; a render with the same key waits for the end of the
  /// render action.
  void checkExclusiveContext(GLTestInstance &instance)
  {
    GL::Hooks hooks;
    const OfxRectI window = { 0, 0, 64, 48 };
    int key = 0;
    {
      GL::ContextPool pool([hooks] { return GL::createContext(hooks); }, 2);
      GL::ContextPool::Lease lease(pool, &key);
      check("a pool context", lease.context() != nullptr);
      if (!lease.context()) {
        return;
      }
      GL::Attachment attachment(instance);
      instance.fetch = GLTestInstance::Fetch::TryOwnKey;
      instance.fetchPool = &pool;
      instance.fetchContext = nullptr;
      instance.fetchRendered = false;
      RenderResult r = glRender(instance, attachment, *lease.context(), window, "checker:5");
      check("a fetch rendering on another thread: another context (tryAcquire), current there, and the render is right (max error " +
            std::to_string(r.maxError) + ")", instance.fetchContext && instance.fetchContext != lease.context() &&
            instance.fetchRendered && r.status == kOfxStatOK && r.maxError <= 1e-3f);

      instance.fetch = GLTestInstance::Fetch::SameKey;
      instance.fetchKey = &key;
      instance.sameKeyDuringRender = true;
      instance.sameKeyGot = false;
      r = glRender(instance, attachment, *lease.context(), window, "checker:5");
      if (instance.fetchThread.joinable()) {
        instance.fetchThread.join();
      }
      check("a render with the same key on another thread waits for the end of the render action",
            instance.sameKeyGot && !instance.sameKeyDuringRender && r.status == kOfxStatOK && r.maxError <= 1e-3f);
    }
    {
      GL::ContextPool pool([hooks] { return GL::createContext(hooks); }, 1);
      GL::ContextPool::Lease lease(pool, &key);
      if (!lease.context()) {
        return;
      }
      GL::Attachment attachment(instance);
      instance.fetch = GLTestInstance::Fetch::TryOwnKey;
      instance.fetchPool = &pool;
      instance.fetchContext = lease.context();
      RenderResult r = glRender(instance, attachment, *lease.context(), window, "checker:5");
      check("a fetch rendering on another thread with no context left: tryAcquire() gives none (the CPU), at once",
            instance.fetchContext == nullptr && r.status == kOfxStatOK && r.maxError <= 1e-3f);
    }
  }

  void checkRenders(MyHost::Host &host, GL::Context &c1, GL::Context &c2)
  {
    const char *path = std::getenv("OFX_PLUGIN_PATH");
    if (!path || !*path) {
      skip("OFX_PLUGIN_PATH is not set: the render checks");
      return;
    }
    const char *pluginId = "com.genarts:OpenGLSamplePlugin"; // Examples/OpenGL
    ImageEffect::PluginCache imageEffectPluginCache(&host);
    imageEffectPluginCache.registerInCache(*PluginCache::getPluginCache());
    PluginCache::getPluginCache()->scanPluginFiles();
    ImageEffect::ImageEffectPlugin *plugin = imageEffectPluginCache.getPluginById(pluginId);
    if (!plugin) {
      skip(std::string("plug-in ") + pluginId + " in OFX_PLUGIN_PATH");
      return;
    }
    ImageEffect::Instance *base = plugin->createInstance(kOfxImageEffectContextFilter, NULL);
    GLTestInstance *instance = dynamic_cast<GLTestInstance *>(base);
    check("createInstance", instance != nullptr);
    if (!instance) {
      return;
    }
    const OfxStatus created = instance->createInstanceAction();
    check("createInstanceAction", created == kOfxStatOK || created == kOfxStatReplyDefault);
    check("getClipPreferences", instance->getClipPreferences());

    // Examples/OpenGL says "needed"; with "true": OpenGL if the host has it, else the CPU
    check("chooseRenderMode(\"needed\", OpenGL) = OpenGL", GL::chooseRenderMode(*instance, true) == GL::RenderMode::OpenGL);
    instance->getProps().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
    check("chooseRenderMode(\"true\", OpenGL) = OpenGL", GL::chooseRenderMode(*instance, true) == GL::RenderMode::OpenGL);
    check("chooseRenderMode(\"true\", no OpenGL) = CPU", GL::chooseRenderMode(*instance, false) == GL::RenderMode::CPU);
    instance->getProps().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "needed");
    check("chooseRenderMode(\"needed\", no OpenGL) = None", GL::chooseRenderMode(*instance, false) == GL::RenderMode::None);
    instance->getProps().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "false");
    check("chooseRenderMode(\"false\", OpenGL) = CPU", GL::chooseRenderMode(*instance, true) == GL::RenderMode::CPU);
    instance->getProps().setStringProperty(kOfxImageEffectPropCPURenderSupported, "false");
    check("chooseRenderMode(\"false\", CPU \"false\") = None", GL::chooseRenderMode(*instance, true) == GL::RenderMode::None);
    instance->getProps().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
    instance->getProps().setStringProperty(kOfxImageEffectPropCPURenderSupported, "true");

    gContexts[0] = &c1;
    gContexts[1] = &c2;
    {
      GL::Attachment attachment(*instance);
      const OfxRectI window = { 0, 0, 64, 48 };
      RenderResult r = glRender(*instance, attachment, c1, window, "checker:5");
      check("render on c1: kOfxStatOK", r.status == kOfxStatOK);
      check("render on c1 (checker input): the output is the input with the red rectangle (max error " + std::to_string(r.maxError) + ")",
            r.maxError <= 1e-3f && r.inputRange > 0.5f);
      check("render on c1: Attached, with c1 current",
            instance->actions == std::vector<std::string>({ "attached:c1" }));
      check("clipFreeTexture: the plug-in freed its handles", r.liveTextures == 0);
      check("clipFreeTexture: input textures deleted", r.inputTexturesDeleted);
      check("clipFreeTexture: the output texture kept for the host", r.outputTextureKept);
      check("attachment: c1", attachment.context() == &c1);

      r = glRender(*instance, attachment, c1, window, "checker:5");
      check("second render on c1: no other context action", r.status == kOfxStatOK && instance->actions.size() == 1);

      // a window that doesn't start at 0: the viewport is the render window's
      r = glRender(*instance, attachment, c2, { 16, 8, 80, 40 }, "ramp");
      check("render on c2: Detached with c1 current, then Attached with c2 current",
            instance->actions == std::vector<std::string>({ "attached:c1", "detached:c1", "attached:c2" }));
      check("render on c2 (ramp input), window at (16, 8): the output (max error " + std::to_string(r.maxError) + ")",
            r.status == kOfxStatOK && r.maxError <= 1e-3f && r.inputRange > 0.01f);

      r = glRender(*instance, attachment, c2, window, "checker:5", /*leakInputTexture=*/true);
      check("an input texture not freed: live until the end of the session", r.liveTextures == 1);
      {
        GL::CurrentContext current(c2);
        const IsTextureFn isTexture = reinterpret_cast<IsTextureFn>(c2.getProcAddress("glIsTexture"));
        check("an input texture not freed: deleted at the end of the session",
              instance->inputTextures.size() == 1 && instance->inputTextures[0] != 0 && !isTexture(instance->inputTextures[0]));
      }

      {
        // Detached needs c2 current, and this thread holds c1: not sent, still attached
        GL::CurrentContext other(c1);
        check("detach() from a thread that holds another context: not sent, still attached",
              attachment.detach() == kOfxStatFailed && attachment.context() == &c2 && instance->actions.size() == 3);
      }

      r = glRender(*instance, attachment, c2, window, "checker:5#alpha");
      check("render of an Alpha input and output: the output is written to .a and read back (max error " +
            std::to_string(r.maxError) + ")", r.status == kOfxStatOK && r.maxError <= 1e-3f && r.inputRange > 0.5f);

      // kOfxOpenGLPropPixelDepth: "the target buffers where the plug-in renders to will be set to
      // one of the supported formats": with float only, the output texture is float, whatever
      // the output image's depth
      Property::String *depths = instance->getProps().fetchStringProperty(kOfxOpenGLPropPixelDepth, true);
      check("the plug-in's kOfxOpenGLPropPixelDepth", depths != nullptr);
      if (depths) {
        depths->reset();
        depths->setValue(kOfxBitDepthFloat, 0);
        r = glRender(*instance, attachment, c2, window, "checker:5");
        check("kOfxOpenGLPropPixelDepth float only: a float output texture (" + instance->outputTextureDepth + "), read back to the image (max error " +
              std::to_string(r.maxError) + ")", instance->outputTextureDepth == kOfxBitDepthFloat && r.status == kOfxStatOK && r.maxError <= 1e-3f);
      }

      const OfxStatus detached = attachment.detach();
      check("detach(): Detached with c2 current",
            (detached == kOfxStatOK || detached == kOfxStatReplyDefault) && instance->actions.size() == 4 &&
            instance->actions.back() == "detached:c2" && attachment.context() == nullptr);
      check("detach() again: nothing sent", attachment.detach() == kOfxStatReplyDefault && instance->actions.size() == 4);
    }
    check("~Attachment of a detached instance: nothing sent", instance->actions.size() == 4);
    checkCrosswiseAndNested(plugin, *instance, c1, c2);
    checkExclusiveContext(*instance);
    instance->destroyInstanceAction();
    delete instance;
  }

} // namespace

int main()
{
  checkBackendNames();
  checkPool();
  checkCurrentContext();
  checkLostHold();
  checkSuspend();

  GL::Hooks hooks;
  hooks.log = [](const std::string &message) { std::printf("  %s\n", message.c_str()); };
  std::string error;
  std::unique_ptr<GL::Context> c1 = GL::createContext(hooks, GL::defaultBackends(), -1, &error);
  if (!c1) {
    skip("no OpenGL context (" + error + "): the texture and render checks");
  } else {
    const std::vector<GL::RendererInfo> renderers = GL::listRenderers(hooks);
    check("listRenderers: at least one", !renderers.empty());
    for (const GL::RendererInfo &r : renderers) {
      std::printf("  renderer %s %d: %s, %s, %s%s, max texture %d\n", GL::backendName(r.backend), r.id, r.renderer.c_str(),
                  r.vendor.c_str(), r.version.c_str(), r.software ? " (software)" : "", r.maxTextureSize);
    }
    {
      GL::CurrentContext current(*c1);
      check("CurrentContext on an OpenGL context: current in the driver", current.ok() && currentNativeContext() == c1->nativeContext());
      std::unique_ptr<GL::Context> other = GL::createContext(hooks);
      check("createContext() while a context is current: that one is current again after",
            other && currentNativeContext() == c1->nativeContext());
      if (other) {
        GL::CurrentContext refused(*other);
        check("a scope on a second context while the thread holds one: refused, the first stays current",
              !refused.ok() && currentNativeContext() == c1->nativeContext());
      }
      if (current.ok()) {
        checkTextures(*c1);
      }
    }
    {
      // WGL: the context's window was destroyed with its thread; it gets a new one on this thread
      std::unique_ptr<GL::Context> orphan;
      std::thread([&] { orphan = GL::createContext(hooks); }).join();
      check("a context created on a thread that ended", orphan != nullptr);
      if (orphan) {
        typedef const unsigned char *(OFXH_GLAPIENTRY *GetStringFn)(unsigned int);
        GL::CurrentContext current(*orphan);
        const GetStringFn getString = reinterpret_cast<GetStringFn>(orphan->getProcAddress("glGetString"));
        check("that context: current on another thread", current.ok() && currentNativeContext() == orphan->nativeContext() &&
              getString && getString(0x1F02 /* GL_VERSION */) != nullptr);
      }
    }
    checkCreatorThreadEnds(hooks);
    std::unique_ptr<GL::Context> c2 = GL::createContext(hooks, GL::defaultBackends(), -1, &error);
    check("a second context", c2 != nullptr);
    if (c2) {
      PluginCache::useStdOFXPluginsLocation(false); // only OFX_PLUGIN_PATH
      GLTestHost host;
      ImageEffect::gImageEffectHost = &host;
      // the host can render with OpenGL (a plug-in reads it in kOfxActionDescribe)
      host.getProperties().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
      checkRenders(host, *c1, *c2);
    }
  }
  std::printf("openglRenderChecks: %d failure(s)\n", gFailures);
  return gFailures ? 1 : 0;
}
