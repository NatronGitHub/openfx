// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// openglRenderChecks: checks of openfx::HostSupportOpenGL (ofxhOpenGLContext.h,
/// ofxhOpenGLTexture.h, ofxhOpenGLRender.h), on hostDemo's host classes:
/// - without OpenGL: the backend names, the context pool (one key on two threads, a failing
///   factory) and CurrentContext (nesting, release(), one context per thread), on fake contexts;
/// - with an OpenGL context: the renderer list, the context restored by createContext(), a context
///   made current after the thread that created it ended (WGL: a new window), texture
///   upload and readback (all depths, RGBA, RGB and Alpha, padded and negative rowBytes, partial
///   rectangles), Alpha and RGB textures sampled as (0, 0, 0, a) and (r, g, b, 1), clear and render
///   target (RGB too);
/// - with Examples/OpenGL (com.genarts:OpenGLSamplePlugin, from OFX_PLUGIN_PATH): OpenGL renders
///   in RenderSession compared with the input (RGBA and Alpha; the output in a depth of the
///   plug-in's kOfxOpenGLPropPixelDepth), kOfxActionOpenGLContextAttached and Detached (their
///   pairing, and the context really current when they are sent, also on two threads crosswise
///   and in a nested scope), one render action at a time per context (a fetch rendering on
///   another thread gets another context or none, and a render with the same key waits), the
///   texture lifetime of clipFreeTexture, the cleanup of textures the plug-in didn't free, and
///   chooseRenderMode(); kOfxStatGLOutOfMemory and clipLoadTexture's kOfxStatErrMemory (the CPU
///   retry), clipLoadTexture with each explicit format and a region (and the formats it refuses),
///   kOfxImageEffectPropOpenGLRenderSupported switched in kOfxActionInstanceChanged, and
///   flushResources (ContextPool::releaseIdle(), the suite's C boundary);
/// - the renderer selection: a context on each renderer of listRenderers(), by its id.
/// Without an OpenGL context or the plug-in, those checks are skipped, unless OFXH_REQUIRE=1 (CI).
/// OFXH_OPENGL_BACKENDS: the backends to try, by name, comma-separated (default: defaultBackends()),
/// e.g. "glx" under an X server.
/// All these checks are functional (#362, part 1 of #361; #365, #368, #369, #370, #378). The
/// checks and the pixel comparison are those of gpuRenderChecks.h.
/// Exits with 1 if a check fails.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
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
#include "ofxhUtilities.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
#include "hostDemoHostDescriptor.h"
#include "gpuRenderChecks.h"
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

using GPUChecks::check;
using GPUChecks::skip;

namespace {

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

  /// backendName() and backendFromName() (#362): every backend's name reads back, an unknown name
  /// is refused
  void checkBackendNames()
  {
    for (GL::Backend b : { GL::Backend::EGLSurfaceless, GL::Backend::EGLDevice, GL::Backend::CGL, GL::Backend::WGL,
                           GL::Backend::GLX }) {
      GL::Backend parsed = GL::Backend::CGL;
      check(std::string("backendFromName(backendName(") + GL::backendName(b) + "))",
            GL::backendFromName(GL::backendName(b), parsed) && parsed == b);
    }
    GL::Backend parsed;
    check("backendFromName(\"vulkan\") = false", !GL::backendFromName("vulkan", parsed));
  }

  /// the backends of the context checks: OFXH_OPENGL_BACKENDS, else defaultBackends()
  std::vector<GL::Backend> gBackends = GL::defaultBackends();

  bool parseBackends()
  {
    const char *env = std::getenv("OFXH_OPENGL_BACKENDS");
    if (!env || !*env) {
      return true;
    }
    std::vector<GL::Backend> backends;
    std::string list(env);
    for (size_t start = 0; start <= list.size();) {
      const size_t end = std::min(list.find(',', start), list.size());
      GL::Backend b;
      if (!GL::backendFromName(list.substr(start, end - start), b)) {
        std::printf("OFXH_OPENGL_BACKENDS: unknown backend \"%s\"\n", list.substr(start, end - start).c_str());
        return false;
      }
      backends.push_back(b);
      start = end + 1;
    }
    gBackends = backends;
    return true;
  }

  /// GL::ContextPool (#362, #368), on fake contexts: one context per render key, at most the
  /// maximum (a third key waits), abort and wake(), the context a key held last, a failing
  /// factory, one key on two threads during the creation, a transient factory failure
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
      // (t1 is in the factory: t2 comes during the creation; at most 5 s, if acquire() doesn't
      // call the factory, which the next check reports)
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (n.load() == 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check("one key on two threads during creation: the factory runs (within 5 s)", n.load() > 0);
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

  /// GL::CurrentContext (#362), on fake contexts: nesting on a thread, another thread waits until
  /// the context is released, release() and reacquire() of a nested scope, heldContext(), and a
  /// scope on a second context while the thread holds one is refused
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
  /// #362 (part 1 of #361).
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
  /// #362.
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
    // (EGL or GLX, whichever the backend loaded: RTLD_NOLOAD)
    typedef void *(*GetCurrentContextFn)();
    const auto load = [](const char *library, const char *name) {
      void *h = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
      return h ? reinterpret_cast<GetCurrentContextFn>(dlsym(h, name)) : nullptr;
    };
    void *current = nullptr;
    if (GetCurrentContextFn egl = load("libEGL.so.1", "eglGetCurrentContext")) {
      current = egl();
    }
    if (!current) {
      if (GetCurrentContextFn glx = load("libGL.so.1", "glXGetCurrentContext")) {
        current = glx();
      }
    }
    return current;
#else
    return wglGetCurrentContext();
#endif
  }

  /// listRenderers(): each renderer is selectable by its id, createContext(backends, id); an
  /// unknown id gives no context
  /// #369.
  void checkRendererSelection(const GL::Hooks &hooks, const std::vector<GL::RendererInfo> &renderers)
  {
    for (const GL::RendererInfo &r : renderers) {
      std::unique_ptr<GL::Context> c = GL::createContext(hooks, { r.backend }, r.id);
      check(std::string("createContext(") + GL::backendName(r.backend) + ", renderer " + std::to_string(r.id) +
              "): that renderer (" + r.renderer + ")",
            c && c->info().backend == r.backend && c->info().id == r.id && c->info().renderer == r.renderer);
    }
    if (!renderers.empty()) {
      std::string error;
      check("createContext() on an unknown renderer id: none, with an error",
            GL::createContext(hooks, { renderers.front().backend }, 1 << 30, &error) == nullptr && !error.empty());
    }
  }

  /// The thread that created a context ends (#365; WGL: Windows destroys the creator's window,
  /// and the context gets a window on the thread that makes it current), in two cases:
  /// - before any other thread made the context current: it is then current on this thread;
  /// - while another thread has the context current: that thread still renders with it, and the
  ///   context is then made current again on a third thread.
  /// #365.
  void checkCreatorThreadEnds(const GL::Hooks &hooks)
  {
    {
      std::unique_ptr<GL::Context> orphan;
      std::thread([&] { orphan = GL::createContext(hooks, gBackends); }).join();
      check("a context created on a thread that ended", orphan != nullptr);
      if (orphan) {
        typedef const unsigned char *(OFXH_GLAPIENTRY *GetStringFn)(unsigned int);
        GL::CurrentContext current(*orphan);
        const GetStringFn getString = reinterpret_cast<GetStringFn>(orphan->getProcAddress("glGetString"));
        check("that context: current on another thread", current.ok() && currentNativeContext() == orphan->nativeContext() &&
              getString && getString(0x1F02 /* GL_VERSION */) != nullptr);
      }
    }
    typedef unsigned int (OFXH_GLAPIENTRY *GetErrorFn)();
    typedef void (OFXH_GLAPIENTRY *ClearColorFn)(float, float, float, float);
    // renders with the context current on this thread: a clear of a texture as the render
    // target, read back (the same on every backend; no window's default framebuffer)
    auto renders = [](GL::Context &context) {
      const auto getError = reinterpret_cast<GetErrorFn>(context.getProcAddress("glGetError"));
      const auto clearColor = reinterpret_cast<ClearColorFn>(context.getProcAddress("glClearColor"));
      typedef void (OFXH_GLAPIENTRY *ClearFn)(unsigned int);
      const auto clear = reinterpret_cast<ClearFn>(context.getProcAddress("glClear"));
      if (!getError || !clearColor || !clear) {
        return false;
      }
      try {
        const OfxRectI one = { 0, 0, 1, 1 };
        GL::Texture texture(context, one, GL::PixelDepth::Byte, GL::Components::RGBA);
        texture.bindAsRenderTarget();
        clearColor(1.f, 0.f, 0.f, 1.f);
        clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
        unsigned char pixel[4] = { 0, 0, 0, 0 };
        GL::ImageRect rect;
        rect.data = pixel;
        rect.bounds = one;
        rect.rowBytes = 4;
        rect.depth = GL::PixelDepth::Byte;
        rect.components = GL::Components::RGBA;
        texture.readback(rect);
        return getError() == 0 && pixel[0] == 255 && pixel[1] == 0 && pixel[3] == 255;
      } catch (const std::exception &) {
        return false;
      }
    };
    std::unique_ptr<GL::Context> context;
    std::promise<void> created, held;
    std::future<void> createdFuture = created.get_future(), heldFuture = held.get_future();
    std::thread creator([&] {
      context = GL::createContext(hooks, gBackends);
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
    int nComps() const { return GL::componentCount(rect.components); }
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

  /// GL::Texture (#362): upload and readback at every depth and components,
  /// padded and negative rowBytes, partial rectangles; Alpha sampled as (0, 0, 0, a); the render
  /// target; ~Texture deletes it
  void checkTextures(GL::Context &context)
  {
    const IsTextureFn isTexture = reinterpret_cast<IsTextureFn>(context.getProcAddress("glIsTexture"));
    check("glIsTexture", isTexture != nullptr);
    const OfxRectI bounds = { -3, 5, 13, 12 };
    // the CPU depths of HostSupport images (half textures from float images)
    const GL::PixelDepth cpuDepths[] = { GL::PixelDepth::Byte, GL::PixelDepth::Short, GL::PixelDepth::Float, GL::PixelDepth::Float };
    const GL::PixelDepth texDepths[] = { GL::PixelDepth::Byte, GL::PixelDepth::Short, GL::PixelDepth::Float, GL::PixelDepth::Half };
    for (int i = 0; i < 4; ++i) {
      for (GL::Components comps : { GL::Components::RGBA, GL::Components::RGB, GL::Components::Alpha }) {
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

    // an Alpha texture samples as (0, 0, 0, a), an RGB one as (r, g, b, 1): drawn with GL_REPLACE
    // into an RGBA target
    for (GL::Components comps : { GL::Components::Alpha, GL::Components::RGB }) {
      const bool alphaOnly = comps == GL::Components::Alpha;
      try {
        const OfxRectI small = { 0, 0, 4, 4 };
        TestImage image(small, GL::PixelDepth::Float, comps, 0, false);
        image.fill();
        GL::Texture texture(context, small, GL::PixelDepth::Float, comps);
        texture.upload(image.rect);
        GL::Texture target(context, small, GL::PixelDepth::Float, GL::Components::RGBA);
        target.clear();
        target.bindAsRenderTarget();
        auto f = [&](const char *name) { return context.getProcAddress(name); };
        reinterpret_cast<ViewportFn>(f("glViewport"))(0, 0, 4, 4);
        reinterpret_cast<EnumFn>(f("glEnable"))(0x0DE1); // GL_TEXTURE_2D
        reinterpret_cast<BindTextureFn>(f("glBindTexture"))(0x0DE1, texture.index());
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
            for (int c = 0; c < 4; ++c) {
              const float expected = alphaOnly ? (c < 3 ? 0.f : TestImage::pattern(x, y, 0))
                                               : (c < 3 ? TestImage::pattern(x, y, c) : 1.f);
              ok = ok && std::fabs(drawn.get(x, y, c) - expected) <= 1e-6f;
            }
          }
        }
        check(alphaOnly ? "an Alpha texture samples as (0, 0, 0, alpha)" : "an RGB texture samples as (r, g, b, 1)", ok);
      } catch (const std::exception &e) {
        check(std::string(alphaOnly ? "Alpha" : "RGB") + " sampling: " + e.what(), false);
      }
    }

    // an RGB texture as the render target: r, g and b are read back
    try {
      const OfxRectI small = { 0, 0, 4, 4 };
      GL::Texture rgb(context, small, GL::PixelDepth::Byte, GL::Components::RGB);
      rgb.bindAsRenderTarget();
      reinterpret_cast<ClearColorFn>(context.getProcAddress("glClearColor"))(0.2f, 0.4f, 0.6f, 0.f);
      reinterpret_cast<ClearFn>(context.getProcAddress("glClear"))(0x4000);
      TestImage back(small, GL::PixelDepth::Byte, GL::Components::RGB, 0, false);
      rgb.readback(back.rect);
      check("an RGB render target: r, g and b read back",
            back.pixel(1, 2)[0] == 51 && back.pixel(1, 2)[1] == 102 && back.pixel(1, 2)[2] == 153);
    } catch (const std::exception &e) {
      check(std::string("RGB render target: ") + e.what(), false);
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

  GL::Context *gContexts[3] = { nullptr, nullptr, nullptr };

  /// the name of the test context current on this thread, from the driver (not from the
  /// library's own bookkeeping)
  std::string currentContextName()
  {
    void *current = currentNativeContext();
    for (int i = 0; i < 3; ++i) {
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
    /// Attached returns kNatronOfxImageEffectPropOpenGLContextData (1, 2... in order), as a
    /// plug-in that renders on several contexts at once; the actions then record the data
    /// ("attached:c1:1", "detached:c1:1")
    bool returnContextData = false;
    std::mutex actionsMutex; ///< (renders on several threads)
    int contextDataCount = 0;
    /// Attached waits while blockAttached, with inAttached set (a slow Attached on another thread)
    std::atomic<bool> blockAttached{ false }, inAttached{ false };
    /// not kOfxStatOK: what the render action returns after the plug-in's (a plug-in that ran out
    /// of GPU memory, kOfxStatGLOutOfMemory)
    OfxStatus forceRenderStatus = kOfxStatOK;
    /// the input textures fail with std::bad_alloc (OpenGL out of memory in clipLoadTexture)
    bool inputOutOfMemory = false;
    /// not empty: kOfxActionInstanceChanged sets kOfxImageEffectPropOpenGLRenderSupported to it on
    /// the instance through the property suite, as a plug-in switching OpenGL on or off ("V1.4:
    /// It is now expected from host reporting v1.4 that the plug-in can during instance change
    /// switch from true to false and false to true.", ofxGPURender.h)
    std::string switchOpenGLTo;
    OfxStatus switchStatus = kOfxStatFailed; ///< of that propSetString
    std::atomic<int> inputFetches{ 0 }; ///< the getImage() calls of the input clips

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
        return forceRenderStatus != kOfxStatOK && st == kOfxStatOK ? forceRenderStatus : st;
      }
      if (std::strcmp(action, kOfxActionInstanceChanged) == 0 && !switchOpenGLTo.empty()) {
        const OfxPropertySuiteV1 *ps =
          static_cast<const OfxPropertySuiteV1 *>(ImageEffect::gImageEffectHost->fetchSuite(kOfxPropertySuite, 1));
        OfxPropertySetHandle h = nullptr;
        const OfxImageEffectSuiteV1 *es =
          static_cast<const OfxImageEffectSuiteV1 *>(ImageEffect::gImageEffectHost->fetchSuite(kOfxImageEffectSuite, 1));
        if (ps && es && es->getPropertySet(getHandle(), &h) == kOfxStatOK) {
          switchStatus = ps->propSetString(h, kOfxImageEffectPropOpenGLRenderSupported, 0, switchOpenGLTo.c_str());
        }
      }
      if (std::strcmp(action, kOfxActionOpenGLContextAttached) == 0) {
        inAttached = true;
        while (blockAttached) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        inAttached = false;
        const OfxStatus st = MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
        std::lock_guard<std::mutex> lock(actionsMutex);
        std::string data;
#ifdef OFX_EXTENSIONS_NATRON
        if (returnContextData && outArgs) {
          const int k = ++contextDataCount;
          outArgs->setPointerProperty(kNatronOfxImageEffectPropOpenGLContextData,
                                      reinterpret_cast<void *>(static_cast<intptr_t>(k)));
          data = ":" + std::to_string(k);
        }
#endif
        actions.push_back("attached:" + currentContextName() + data);
        return st;
      }
      if (std::strcmp(action, kOfxActionOpenGLContextDetached) == 0) {
        std::lock_guard<std::mutex> lock(actionsMutex);
        std::string data;
#ifdef OFX_EXTENSIONS_NATRON
        // (the data Attached returned on this context, if any)
        if (const intptr_t k = inArgs ? reinterpret_cast<intptr_t>(inArgs->getPointerProperty(kNatronOfxImageEffectPropOpenGLContextData)) : 0) {
          data = ":" + std::to_string(k);
        }
#endif
        actions.push_back("detached:" + currentContextName() + data);
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
    if (_glEffect->inputOutOfMemory && !isOutput()) {
      throw std::bad_alloc(); // (RenderSession::loadTexture()'s, when OpenGL is out of memory)
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
    if (!isOutput()) {
      ++e.inputFetches;
    }
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
    /// flushResources: GL::flushResources(*pool) if set (else hostDemo's kOfxStatReplyDefault)
    GL::ContextPool *pool = nullptr;
    /// flushOpenGLResources() throws (the suite must not let it reach the plug-in)
    bool throwOnFlush = false;

    ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc,
                                       const std::string &context) override
    {
      return new GLTestInstance(plugin, desc, context);
    }
    OfxStatus flushOpenGLResources() const override
    {
      if (throwOnFlush) {
        throw std::runtime_error("flushOpenGLResources");
      }
      return pool ? GL::flushResources(*pool) : MyHost::Host::flushOpenGLResources();
    }
  };

  struct RenderResult {
    OfxStatus status = kOfxStatFailed;
    int liveTextures = -1;              ///< after the render action
    bool outputTextureKept = false;     ///< after the plug-in freed its handle
    bool inputTexturesDeleted = false;  ///< by clipFreeTexture
    float maxError = INFINITY;          ///< vs the expected image
    float inputRange = 0.f;             ///< max - min of the input's red in the window
    std::string mismatch;               ///< the first pixel component off by more than 1e-3, and the renderer
    bool glFailed = false;              ///< GL::openGLFailed()
    bool cpuRetry = false;              ///< GL::cpuRetryAllowed()
  };

  /// the max error of r, and where the render differs first (none if it doesn't)
  std::string describeError(const RenderResult &r)
  {
    return "max error " + std::to_string(r.maxError) + (r.mismatch.empty() ? std::string() : "; " + r.mismatch);
  }

  /// an OpenGL render of window at time 0 on context, of the input (a hostDemo input spec),
  /// compared with what Examples/OpenGL draws (scale and source_scale 1): the input, with a red
  /// rectangle from (10, 10). duringSession runs after the render action, in its session.
  RenderResult glRender(GLTestInstance &instance, GL::Attachment &attachment, GL::Context &context,
                        const OfxRectI &window, const std::string &input, bool leakInputTexture = false,
                        const std::function<void(GL::RenderSession &, MyHost::MyClipInstance &)> &duringSession = {})
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
                                          session.contextData(),
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
    result.glFailed = GL::openGLFailed(session, result.status);
    result.cpuRetry = GL::cpuRetryAllowed(instance, session, result.status);
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
    if (duringSession) {
      // (still in the session, the render action done: as a plug-in's calls during it)
      duringSession(session, *source);
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
              const float got = out->getComponentFloat(x, y, c);
              GPUChecks::addError(result.maxError, got, expected);
              if (result.mismatch.empty() && !(std::fabs(got - expected) <= 1e-3f)) {
                result.mismatch = "first at (" + std::to_string(x) + ", " + std::to_string(y) + ") component " +
                                  std::to_string(c) + ": expected " + std::to_string(expected) + ", got " +
                                  std::to_string(got) + ", on " + context.info().renderer;
              }
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

  /// readOutput() copies the render window back, and leaves the host's other pixels: a pixel outside the
  /// window, changed after setOutput(), keeps its new value (the output image is the clip's, over window;
  /// the session's render window is smaller)
  /// #370.
  void checkReadbackWindow(GLTestInstance &instance, GL::Attachment &attachment, GL::Context &context)
  {
    const OfxPointD scale = { 1., 1. };
    const OfxRectI window = { 0, 0, 64, 48 }, inner = { 8, 8, 32, 24 };
    std::map<ImageEffect::ClipInstance *, OfxRectD> rois;
    instance.setRenderState(scale, window, rois);
    MyHost::MyClipInstance *output = dynamic_cast<MyHost::MyClipInstance *>(instance.getClip(kOfxImageEffectOutputClipName));
    output->releaseOutputImage();
    ImageEffect::Image *image = output->getImage(0., nullptr);
    MyHost::MyImage *out = dynamic_cast<MyHost::MyImage *>(image);
    {
      GL::CurrentContext current(context);
      GL::RenderSession session(attachment, current);
      if (session.ok() && out) {
        session.setOutput(*image, inner);
        const size_t pixel = static_cast<size_t>(out->getNComponents() * out->getBytesPerComponent());
        unsigned char *outside = const_cast<unsigned char *>(out->getPixelData(40, 30));
        std::vector<unsigned char> marker(pixel, 0x5A);
        std::memcpy(outside, marker.data(), pixel); // (after setOutput())
        session.readOutput();
        const bool kept = std::memcmp(out->getPixelData(40, 30), marker.data(), pixel) == 0;
        // inside: the cleared texture (transparent black)
        bool cleared = true;
        for (int c = 0; c < out->getNComponents(); ++c) {
          cleared = cleared && out->getComponentFloat(10, 10, c) == 0.f;
        }
        check("readOutput() with a render window smaller than the output image: a pixel outside it, changed after "
              "setOutput(), keeps its value, and the window is read back", kept && cleared);
      } else {
        check("a RenderSession and an output image for the readback window check", false);
      }
    }
    image->releaseReference();
    instance.clearRenderState();
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
  /// #362.
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

  /// OFX 1.5.1, one context at a time: a render on c2 while one on c1 is in progress waits for it
  /// (without holding c2), then Detached on c1 and Attached on c2: "A host cannot call
  /// kOfxActionOpenGLContextAttached on the same instance without an intervening
  /// kOfxActionOpenGLContextDetached"
  /// #369.
  void checkSwitchWaits(GLTestInstance &instance, GL::Context &c1, GL::Context &c2)
  {
    instance.actions.clear();
    GL::Attachment attachment(instance);
    std::atomic<int> stage(0);
    std::thread t1([&] {
      GL::CurrentContext current(c1);
      GL::RenderSession session(attachment, current);
      stage = session.ok() ? 1 : -1;
      while (stage == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
    while (stage == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::atomic<bool> done(false);
    bool ok2 = false;
    std::thread t2([&] {
      GL::CurrentContext current(c2);
      GL::RenderSession session(attachment, current);
      ok2 = session.ok();
      done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool waited = !done && instance.actions == std::vector<std::string>({ "attached:c1" });
    stage = 2;
    t1.join();
    t2.join();
    check("one context at a time: a render on c2 waits for the one in progress on c1", waited);
    check("one context at a time: then Detached on c1 and Attached on c2",
          ok2 && instance.actions == std::vector<std::string>({ "attached:c1", "detached:c1", "attached:c2" }));
    check("one context at a time: detach() sends Detached on c2", attachment.detach() == kOfxStatOK && instance.actions.back() == "detached:c2");
    instance.actions.clear();
  }

  /// A plug-in that returns kNatronOfxImageEffectPropOpenGLContextData from Attached is attached to
  /// several contexts at once ("This permits running several parallel renders on different OpenGL
  /// contexts", ofxNatron.h): a render on c2 doesn't wait for one on c1, there is no Detached in
  /// between, each session gets its context's data, and detach() sends Detached on each context
  /// with it current and its data. (Natron builds; without the extension, OFX 1.5.1's one context
  /// at a time is checked by checkCrosswiseAndNested().)
  /// #368 (a Natron extension).
  void checkConcurrentAttachment(GLTestInstance &instance, GL::Context &c1, GL::Context &c2)
  {
#ifdef OFX_EXTENSIONS_NATRON
    instance.returnContextData = true;
    instance.actions.clear();
    {
      GL::Attachment attachment(instance);
      std::atomic<int> stage(0);
      bool ok1 = false, ok2 = false;
      void *d1 = nullptr, *d2 = nullptr;
      std::thread t1([&] {
        GL::CurrentContext current(c1);
        GL::RenderSession session(attachment, current);
        ok1 = session.ok();
        d1 = session.contextData();
        stage = 1;
        while (stage < 2) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
      while (stage < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::atomic<bool> done(false);
      std::thread t2([&] {
        GL::CurrentContext current(c2);
        GL::RenderSession session(attachment, current);
        ok2 = session.ok();
        d2 = session.contextData();
        done = true;
      });
      for (int i = 0; i < 100 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      check("with context data: a render on c2 runs while one on c1 is in progress (5 s)", done.load());
      stage = 2;
      t1.join();
      t2.join();
      check("with context data: attached to c1 and c2, no Detached in between, each session gets its data",
            ok1 && ok2 && attachment.contexts() == 2 && d1 && d2 && d1 != d2 &&
            instance.actions == std::vector<std::string>({ "attached:c1:1", "attached:c2:2" }));
      check("with context data: detach() sends Detached on each context, current, with its data",
            attachment.detach() == kOfxStatOK && attachment.contexts() == 0 && instance.actions.size() == 4 &&
            instance.actions[2] == "detached:c2:2" && instance.actions[3] == "detached:c1:1");
    }
    // a render on c1 joins while another thread's Attached on c2 runs (a concurrent attach),
    // without waiting for it: a nested render, which can't wait, would fail
    {
      GL::Attachment attachment(instance);
      void *d1 = nullptr;
      {
        GL::CurrentContext current(c1);
        GL::RenderSession session(attachment, current);
        d1 = session.contextData();
      }
      instance.blockAttached = true;
      std::thread t2([&] {
        GL::CurrentContext current(c2);
        GL::RenderSession session(attachment, current);
      });
      for (int i = 0; i < 500 && !instance.inAttached; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::atomic<bool> done(false);
      bool joined = false;
      std::thread t1([&] {
        GL::CurrentContext current(c1);
        GL::RenderSession session(attachment, current);
        joined = session.ok() && d1 && session.contextData() == d1;
        done = true;
      });
      for (int i = 0; i < 100 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      check("with context data: a render on c1 joins while Attached runs on c2 on another thread (5 s)",
            instance.inAttached.load() && done.load() && joined);
      instance.blockAttached = false;
      t1.join();
      t2.join();
      check("with context data: then attached to c1 and c2", attachment.contexts() == 2 && attachment.detach() == kOfxStatOK);
    }
    // mixed: data on c1, NULL on c2. The plug-in is then "supposed to follow the OpenFX spec, and
    // will have to be detached/attached when switching the render context" (ofxNatron.h), and "A
    // host cannot call kOfxActionOpenGLContextAttached on the same instance without an
    // intervening kOfxActionOpenGLContextDetached" (OFX 1.5.1): the next render detaches it from
    // every other context; on c1 or c2 it stays attached there (no Detached and Attached again)
    std::unique_ptr<GL::Context> c3 = GL::createContext(GL::Hooks(), gBackends);
    check("a third context", c3 != nullptr);
    if (c3) {
      gContexts[2] = c3.get();
      struct Case {
        GL::Context *context;
        const char *name;
        std::vector<std::string> then; ///< the actions after attached:c1:k, attached:c2 ("k": c1's data)
        std::string detached;          ///< then sent by detach()
      };
      const Case cases[] = {
        { c3.get(), "c3", { "detached:c2", "detached:c1:k", "attached:c3" }, "detached:c3" },
        { &c1, "c1", { "detached:c2" }, "detached:c1:k" },
        { &c2, "c2", { "detached:c1:k" }, "detached:c2" },
      };
      for (const Case &c : cases) {
        instance.returnContextData = true;
        instance.actions.clear();
        GL::Attachment attachment(instance);
        bool ok = true;
        {
          GL::CurrentContext current(c1);
          GL::RenderSession session(attachment, current);
          ok = ok && session.ok();
        }
        const std::string k = std::to_string(instance.contextDataCount);
        instance.returnContextData = false;
        {
          GL::CurrentContext current(c2);
          GL::RenderSession session(attachment, current);
          ok = ok && session.ok();
        }
        check(std::string("mixed: data on c1, then NULL on c2 attaches c2 too (then ") + c.name + ")",
              ok && attachment.contexts() == 2);
        void *data = nullptr;
        {
          GL::CurrentContext current(*c.context);
          GL::RenderSession session(attachment, current);
          ok = ok && session.ok();
          data = session.contextData();
        }
        std::vector<std::string> expected = { "attached:c1:" + k, "attached:c2" };
        for (std::string a : c.then) {
          if (a.back() == 'k') {
            a.replace(a.size() - 1, 1, k);
          }
          expected.push_back(a);
        }
        std::string last = c.detached;
        if (last.back() == 'k') {
          last.replace(last.size() - 1, 1, k);
        }
        const bool withData = c.context == &c1;
        check(std::string("mixed: a render on ") + c.name + " leaves it attached to " + c.name + " only: " +
                (c.context == c3.get() ? "Detached on c2 and c1, then Attached on c3"
                                       : withData ? "Detached on c2, nothing on c1 (its data kept)"
                                                  : "Detached on c1, nothing on c2"),
              ok && attachment.contexts() == 1 && attachment.context() == c.context && instance.actions == expected &&
                (withData ? data != nullptr : data == nullptr));
        check(std::string("mixed: then detach() sends Detached on ") + c.name,
              attachment.detach() == kOfxStatOK && instance.actions.size() == expected.size() + 1 && instance.actions.back() == last);
      }
      gContexts[2] = nullptr;
    }
    instance.returnContextData = false;
    instance.actions.clear();
#else
    (void)instance;
    (void)c1;
    (void)c2;
#endif
  }

  /// One render action at a time per context: during the plug-in's fetch, a render on another
  /// thread gets another context (tryAcquire(), up to the maximum) or none (then the CPU), never
  /// the reserved one, and never waits; a render with the same key waits for the end of the
  /// render action.
  /// #362.
  void checkExclusiveContext(GLTestInstance &instance)
  {
    GL::Hooks hooks;
    const OfxRectI window = { 0, 0, 64, 48 };
    int key = 0;
    {
      GL::ContextPool pool([hooks] { return GL::createContext(hooks, gBackends); }, 2);
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
      check("a fetch rendering on another thread: another context (tryAcquire), current there, and the render is right (" +
            describeError(r) + ")", instance.fetchContext && instance.fetchContext != lease.context() &&
            instance.fetchRendered && r.status == kOfxStatOK && r.maxError <= 1e-3f);

      instance.fetch = GLTestInstance::Fetch::SameKey;
      instance.fetchKey = &key;
      instance.sameKeyDuringRender = true;
      instance.sameKeyGot = false;
      r = glRender(instance, attachment, *lease.context(), window, "checker:5");
      if (instance.fetchThread.joinable()) {
        instance.fetchThread.join();
      }
      check("a render with the same key on another thread waits for the end of the render action (" + describeError(r) + ")",
            instance.sameKeyGot && !instance.sameKeyDuringRender && r.status == kOfxStatOK && r.maxError <= 1e-3f);
    }
    {
      GL::ContextPool pool([hooks] { return GL::createContext(hooks, gBackends); }, 1);
      GL::ContextPool::Lease lease(pool, &key);
      if (!lease.context()) {
        return;
      }
      GL::Attachment attachment(instance);
      instance.fetch = GLTestInstance::Fetch::TryOwnKey;
      instance.fetchPool = &pool;
      instance.fetchContext = lease.context();
      RenderResult r = glRender(instance, attachment, *lease.context(), window, "checker:5");
      check("a fetch rendering on another thread with no context left: tryAcquire() gives none (the CPU), at once (" +
            describeError(r) + ")",
            instance.fetchContext == nullptr && r.status == kOfxStatOK && r.maxError <= 1e-3f);
    }
  }

  /// setMaxContexts() lowered from 4 to 1, also below the number of contexts in use: the four
  /// contexts stay allocated, those lent stay with their renders, and no other is lent until
  /// fewer than the maximum are in use (tryAcquire() gives none, acquire() waits); raising it
  /// wakes a waiting acquire()
  /// #368.
  void checkPoolLowered()
  {
    GL::Hooks hooks;
    GL::ContextPool pool([hooks] { return GL::createContext(hooks, gBackends); }, 4);
    int keys[5] = { 0, 0, 0, 0, 0 };
    std::unique_ptr<GL::ContextPool::Lease> leases[4];
    for (int i = 0; i < 4; ++i) {
      leases[i].reset(new GL::ContextPool::Lease(pool, &keys[i]));
    }
    check("a pool of 4: 4 contexts lent", pool.size() == 4 && leases[3]->context() != nullptr);
    leases[3].reset();
    pool.setMaxContexts(1); // (3 in use)
    check("setMaxContexts(1) with 3 in use: maxContexts() is 1, the 4 contexts stay",
          pool.maxContexts() == 1 && pool.size() == 4);
    GL::Context *const held = leases[0]->context();
    check("setMaxContexts(1) with 3 in use: a render holding one keeps it, no idle one is lent",
          pool.tryAcquire(&keys[0]) == held && pool.tryAcquire(&keys[4]) == nullptr);
    pool.release(&keys[0]); // (the tryAcquire() above)
    leases[1].reset();
    check("setMaxContexts(1) with 2 in use: still none lent", pool.tryAcquire(&keys[4]) == nullptr);
    leases[2].reset();
    {
      std::atomic<bool> got(false);
      std::thread waiter([&] {
        GL::ContextPool::Lease other(pool, &keys[4]);
        got = other.context() != nullptr;
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      check("setMaxContexts(1) with 1 in use: tryAcquire() gives none, acquire() waits",
            pool.tryAcquire(&keys[3]) == nullptr && !got.load());
      leases[0].reset();
      waiter.join();
      check("setMaxContexts(1): acquire() gets one after the release", got.load());
    }
    {
      GL::ContextPool::Lease first(pool, &keys[0]);
      std::atomic<bool> got(false);
      std::thread waiter([&] {
        GL::ContextPool::Lease other(pool, &keys[1]);
        got = other.context() != nullptr && other.context() != first.context();
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const bool waited = !got.load();
      pool.setMaxContexts(2);
      waiter.join();
      check("setMaxContexts(2): a waiting acquire() gets a second context", waited && got.load() && pool.size() == 4);
    }
  }

  /// flushResources: ContextPool::releaseIdle() destroys the idle contexts that no instance is
  /// attached to; the suite's flushResources returns kOfxStatOK if the host released something,
  /// else kOfxStatReplyDefault (also without a host, and when the host throws)
  /// #387.
  void checkFlushResources(GLTestHost &host, GLTestInstance &instance)
  {
    const OfxImageEffectOpenGLRenderSuiteV1 *suite =
      static_cast<const OfxImageEffectOpenGLRenderSuiteV1 *>(host.fetchSuite(kOfxOpenGLRenderSuite, 1));
    check("the OpenGL render suite", suite && suite->flushResources);
    if (!suite || !suite->flushResources) {
      return;
    }
    check("flushResources, hostDemo's host (one context in use): kOfxStatReplyDefault",
          suite->flushResources() == kOfxStatReplyDefault);
    GL::Hooks hooks;
    GL::ContextPool pool([hooks] { return GL::createContext(hooks, gBackends); }, 2);
    host.pool = &pool;
    int keys[2] = { 0, 0 };
    {
      GL::ContextPool::Lease a(pool, &keys[0]);
      {
        GL::ContextPool::Lease b(pool, &keys[1]);
        check("a pool of 2: 2 contexts", a.context() && b.context() && pool.size() == 2);
      }
      // one idle, one lent: "When a plug-in fails to allocate GPU resources, it can call this
      // function to request the host to flush its GPU resources if it holds any"
      check("flushResources with an idle context: kOfxStatOK, \"the host has actually released some resources\"",
            suite->flushResources() == kOfxStatOK && pool.size() == 1 && a.context() != nullptr);
      check("flushResources again (the other context is lent): kOfxStatReplyDefault, \"nothing the host could do..\"",
            suite->flushResources() == kOfxStatReplyDefault && pool.size() == 1);
      {
        GL::ContextPool::Lease b(pool, &keys[1]);
        check("acquire() after a flush: a new context", b.context() && pool.size() == 2);
      }
    }
    // a context an instance is attached to stays (Detached must be sent on it first)
    {
      GL::Attachment attachment(instance);
      GL::Context *attached = nullptr;
      {
        GL::ContextPool::Lease lease(pool, &keys[0]);
        GL::CurrentContext current(*lease.context());
        GL::RenderSession session(attachment, current);
        attached = session.ok() ? lease.context() : nullptr;
      }
      check("an instance attached to a pool context", attached && attached->attachments() == 1);
      const int kept = pool.releaseIdle();
      check("releaseIdle(): the context the instance is attached to stays, the other one goes",
            kept == 1 && pool.size() == 1);
      attachment.detach();
      check("after Detached: no attachment, released", attached && attached->attachments() == 0 &&
            suite->flushResources() == kOfxStatOK && pool.size() == 0);
    }
    host.pool = nullptr;
    // the C boundary: no host, a host that throws
    ImageEffect::Host *const saved = ImageEffect::gImageEffectHost;
    ImageEffect::gImageEffectHost = nullptr;
    check("flushResources without a host: kOfxStatReplyDefault", suite->flushResources() == kOfxStatReplyDefault);
    ImageEffect::gImageEffectHost = saved;
    host.throwOnFlush = true;
    check("flushResources when the host throws: kOfxStatReplyDefault, no exception to the plug-in",
          suite->flushResources() == kOfxStatReplyDefault);
    host.throwOnFlush = false;
  }

  /// the OpenGL statuses a plug-in may return: kOfxStatGLOutOfMemory from the render, and
  /// clipLoadTexture's kOfxStatErrMemory; an explicit clipLoadTexture format and a region; and the
  /// plug-in's switch of kOfxImageEffectPropOpenGLRenderSupported in kOfxActionInstanceChanged
  /// #387.
  void checkStatusesAndFormats(MyHost::Host &host, GLTestInstance &instance, GL::Attachment &attachment, GL::Context &context)
  {
    const OfxRectI window = { 0, 0, 64, 48 };
    Property::Set &props = instance.getProps();
    // the CPU retry (#387): "If a plug-in returns kOfxStatGLOutOfMemory, the host may choose to
    // free resources on the GPU and retry the OpenGL render, rather than immediately falling back
    // to CPU rendering"
    instance.forceRenderStatus = kOfxStatGLOutOfMemory;
    RenderResult r = glRender(instance, attachment, context, window, "checker:5");
    check("a render that returns kOfxStatGLOutOfMemory: OpenGL failed, the CPU retry allowed (\"true\")",
          r.status == kOfxStatGLOutOfMemory && r.glFailed && r.cpuRetry);
    props.setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "needed");
    r = glRender(instance, attachment, context, window, "checker:5");
    check("the same with \"needed\": no CPU retry", r.status == kOfxStatGLOutOfMemory && r.glFailed && !r.cpuRetry);
    props.setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
    instance.forceRenderStatus = kOfxStatOK;
    // clipLoadTexture: "kOfxStatErrMemory - not enough OpenGL memory was available for the effect
    // to load the texture. The plug-in should abort the GL render and return kOfxStatErrMemory,
    // after which the host can decide to retry the operation with CPU based processing."
    instance.inputOutOfMemory = true;
    r = glRender(instance, attachment, context, window, "checker:5");
    instance.inputOutOfMemory = false;
    check("an input texture out of memory: clipLoadTexture's kOfxStatErrMemory returned by the plug-in, OpenGL "
          "failed, the CPU retry allowed", r.status == kOfxStatErrMemory && r.glFailed && r.cpuRetry && r.liveTextures == 0);

    // "When a specific format is requested, the host ensures it gives the requested format";
    // "If the region parameter is set to non-NULL, then it will be clipped to the clip's Region of
    // Definition for the given time. The returned image will be at least as big as this region."
    const OfxImageEffectOpenGLRenderSuiteV1 *suite =
      static_cast<const OfxImageEffectOpenGLRenderSuiteV1 *>(host.fetchSuite(kOfxOpenGLRenderSuite, 1));
    const OfxPropertySuiteV1 *ps = static_cast<const OfxPropertySuiteV1 *>(host.fetchSuite(kOfxPropertySuite, 1));
    std::vector<std::pair<std::string, bool>> results;
    const OfxRectD region = { 8., 4., 24., 20. };
    r = glRender(instance, attachment, context, window, "checker:5", false,
                 [&](GL::RenderSession &session, MyHost::MyClipInstance &source) {
      for (const char *depth : { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthHalf, kOfxBitDepthFloat }) {
        OfxPropertySetHandle tex = nullptr;
        const OfxStatus st = suite->clipLoadTexture(source.getHandle(), 0., depth, &region, &tex);
        bool ok = st == kOfxStatOK && tex;
        std::string got = "status " + std::to_string(st);
        if (ok) {
          const char *d = nullptr;
          int b[4] = { 0, 0, 0, 0 };
          ps->propGetString(tex, kOfxImageEffectPropPixelDepth, 0, &d);
          ps->propGetIntN(tex, kOfxImagePropBounds, 4, b);
          got = std::string(d ? d : "no depth") + ", bounds (" + std::to_string(b[0]) + ", " + std::to_string(b[1]) + ", " +
            std::to_string(b[2]) + ", " + std::to_string(b[3]) + ")";
          // the region in pixels (render scale 1, the project's PAR)
          const OfxPointD one = { 1., 1. };
          const OfxRectI p = MyHost::canonicalToPixelEnclosing(region, one, instance.getProjectPixelAspectRatio());
          ok = d && std::string(d) == depth && b[0] <= p.x1 && b[1] <= p.y1 && b[2] >= p.x2 && b[3] >= p.y2;
          suite->clipFreeTexture(tex);
        }
        results.emplace_back(std::string("clipLoadTexture(format ") + depth + ", a region): a texture of that depth, at "
                             "least the region in pixels (" + got + ")", ok);
      }
      results.emplace_back("those textures: no input failure", !session.inputFailed());
      for (const char *bad : { kOfxBitDepthNone, "OfxBitDepthWord", "" }) {
        OfxPropertySetHandle tex = reinterpret_cast<OfxPropertySetHandle>(&source); // (not NULL: must be set)
        const int fetches = instance.inputFetches;
        const OfxStatus st = suite->clipLoadTexture(source.getHandle(), 0., bad, nullptr, &tex);
        results.emplace_back(std::string("clipLoadTexture(format \"") + bad + "\", not a texture depth): kOfxStatFailed, "
                             "no texture, the input not fetched, the render's input failed (no silent fallback)",
                             st == kOfxStatFailed && tex == nullptr && session.inputFailed() &&
                             instance.inputFetches == fetches);
      }
    });
    check("the render of the format checks", r.status == kOfxStatOK);
    for (const auto &c : results) {
      check(c.first, c.second);
    }

    // kOfxImageEffectPropOpenGLRenderSupported, "plug-in instance change (read/write)": the
    // plug-in switches it in kOfxActionInstanceChanged, the next render follows
    const std::string descriptorValue = instance.getDescriptor().getProps().getStringProperty(kOfxImageEffectPropOpenGLRenderSupported);
    const OfxPointD scale = { 1., 1. };
    for (const char *value : { "false", "true" }) {
      instance.switchOpenGLTo = value;
      instance.switchStatus = kOfxStatFailed;
      instance.beginInstanceChangedAction(kOfxChangeUserEdited);
      instance.paramInstanceChangedAction("scale", kOfxChangeUserEdited, 0., scale);
      instance.endInstanceChangedAction(kOfxChangeUserEdited);
      const bool on = std::string(value) == "true";
      check(std::string("kOfxActionInstanceChanged sets kOfxImageEffectPropOpenGLRenderSupported \"") + value +
            "\" on the instance: the next render is " + (on ? "OpenGL" : "on the CPU") + ", the descriptor's unchanged",
            instance.switchStatus == kOfxStatOK &&
            GL::chooseRenderMode(instance, true) == (on ? GL::RenderMode::OpenGL : GL::RenderMode::CPU) &&
            instance.getDescriptor().getProps().getStringProperty(kOfxImageEffectPropOpenGLRenderSupported) == descriptorValue);
    }
    instance.switchOpenGLTo.clear();
    r = glRender(instance, attachment, context, window, "checker:5");
    check("an OpenGL render after the switch back (" + describeError(r) + ")", r.status == kOfxStatOK && r.maxError <= 1e-3f);
  }

  /// #362, #368, #369, #387; #378, which fixed #373 in Examples/OpenGL (its rectangle stays in
  /// the render window): the host was right, so the output comparison stays functional.
  void checkRenders(GLTestHost &host, GL::Context &c1, GL::Context &c2)
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

    // chooseRenderMode() (#362): Examples/OpenGL says "needed"; with "true": OpenGL if the host
    // has it, else the CPU; "needed" without OpenGL: None; "false": the CPU, or None if the
    // plug-in can't render on the CPU either
    Property::Set &props = instance->getProps();
    const char *const gl = kOfxImageEffectPropOpenGLRenderSupported, *const cpu = kOfxImageEffectPropCPURenderSupported;
    check("chooseRenderMode(\"needed\", OpenGL) = OpenGL (Examples/OpenGL's own \"needed\")",
          props.getStringProperty(gl) == "needed" && GL::chooseRenderMode(*instance, true) == GL::RenderMode::OpenGL);
    GPUChecks::checkRules({
      { "chooseRenderMode(\"true\", OpenGL) = OpenGL", { { &props, gl, "true" } },
        [&] { return GL::chooseRenderMode(*instance, true) == GL::RenderMode::OpenGL; } },
      { "chooseRenderMode(\"true\", no OpenGL) = CPU", { { &props, gl, "true" } },
        [&] { return GL::chooseRenderMode(*instance, false) == GL::RenderMode::CPU; } },
      { "chooseRenderMode(\"needed\", no OpenGL) = None", { { &props, gl, "needed" } },
        [&] { return GL::chooseRenderMode(*instance, false) == GL::RenderMode::None; } },
      { "chooseRenderMode(\"false\", OpenGL) = CPU", { { &props, gl, "false" } },
        [&] { return GL::chooseRenderMode(*instance, true) == GL::RenderMode::CPU; } },
      { "chooseRenderMode(\"false\", CPU \"false\") = None", { { &props, gl, "false" }, { &props, cpu, "false" } },
        [&] { return GL::chooseRenderMode(*instance, true) == GL::RenderMode::None; } },
    });
    // (the renders below: "true", the CPU "true")
    props.setStringProperty(gl, "true");
    props.setStringProperty(cpu, "true");

    gContexts[0] = &c1;
    gContexts[1] = &c2;
    {
      GL::Attachment attachment(*instance);
      const OfxRectI window = { 0, 0, 64, 48 };
      RenderResult r = glRender(*instance, attachment, c1, window, "checker:5");
      check("render on c1: kOfxStatOK", r.status == kOfxStatOK);
      check("render on c1 (checker input): the output is the input with the red rectangle (" + describeError(r) + ")",
            r.maxError <= 1e-3f && r.inputRange > 0.5f);
      check("render on c1: Attached, with c1 current",
            instance->actions == std::vector<std::string>({ "attached:c1" }));
      check("clipFreeTexture: the plug-in freed its handles", r.liveTextures == 0);
      check("clipFreeTexture: input textures deleted", r.inputTexturesDeleted);
      check("clipFreeTexture: the output texture kept for the host", r.outputTextureKept);
      check("attachment: c1", attachment.context() == &c1);
      checkReadbackWindow(*instance, attachment, c1);

      r = glRender(*instance, attachment, c1, window, "checker:5");
      check("second render on c1: no other context action", r.status == kOfxStatOK && instance->actions.size() == 1);

      // a window that doesn't start at 0: the viewport is the render window's
      r = glRender(*instance, attachment, c2, { 16, 8, 80, 40 }, "ramp");
      check("render on c2: Detached with c1 current, then Attached with c2 current",
            instance->actions == std::vector<std::string>({ "attached:c1", "detached:c1", "attached:c2" }));
      check("render on c2 (ramp input), window at (16, 8): the output (" + describeError(r) + ")",
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
      check("render of an Alpha input and output: the output is written to .a and read back (" + describeError(r) + ")",
            r.status == kOfxStatOK && r.maxError <= 1e-3f && r.inputRange > 0.5f);
      checkStatusesAndFormats(host, *instance, attachment, c2);

      // kOfxOpenGLPropPixelDepth: "the target buffers where the plug-in renders to will be set to
      // one of the supported formats": with float only, the output texture is float, whatever
      // the output image's depth
      Property::String *depths = instance->getProps().fetchStringProperty(kOfxOpenGLPropPixelDepth, true);
      check("the plug-in's kOfxOpenGLPropPixelDepth", depths != nullptr);
      if (depths) {
        depths->reset();
        depths->setValue(kOfxBitDepthFloat, 0);
        r = glRender(*instance, attachment, c2, window, "checker:5");
        check("kOfxOpenGLPropPixelDepth float only: a float output texture (" + instance->outputTextureDepth +
              "), read back to the image (" + describeError(r) + ")",
              instance->outputTextureDepth == kOfxBitDepthFloat && r.status == kOfxStatOK && r.maxError <= 1e-3f);
      }

      const OfxStatus detached = attachment.detach();
      check("detach(): Detached with c2 current",
            (detached == kOfxStatOK || detached == kOfxStatReplyDefault) && instance->actions.size() == 4 &&
            instance->actions.back() == "detached:c2" && attachment.context() == nullptr);
      check("detach() again: nothing sent", attachment.detach() == kOfxStatReplyDefault && instance->actions.size() == 4);
    }
    check("~Attachment of a detached instance: nothing sent", instance->actions.size() == 4);
    checkCrosswiseAndNested(plugin, *instance, c1, c2);
    checkSwitchWaits(*instance, c1, c2);
    checkConcurrentAttachment(*instance, c1, c2);
    checkExclusiveContext(*instance);
    checkPoolLowered();
    checkFlushResources(host, *instance);
    instance->destroyInstanceAction();
    delete instance;
  }

} // namespace

int main()
{
  checkBackendNames();
  if (!parseBackends()) {
    return 1;
  }
  checkPool();
  checkCurrentContext();
  checkLostHold();
  checkSuspend();

  GL::Hooks hooks;
  hooks.log = [](OFX::Host::LogLevel level, const std::string &message) {
    static const char *const kLevels[] = { "error", "warning", "info", "debug" };
    std::printf("  [%s] %s\n", kLevels[static_cast<int>(level)], message.c_str());
  };
  std::string error;
  std::unique_ptr<GL::Context> c1 = GL::createContext(hooks, gBackends, -1, &error);
  if (!c1) {
    skip("no OpenGL context (" + error + "): the texture and render checks");
  } else {
    const std::vector<GL::RendererInfo> renderers = GL::listRenderers(hooks, gBackends);
    check("listRenderers: at least one", !renderers.empty());
    checkRendererSelection(hooks, renderers);
    for (const GL::RendererInfo &r : renderers) {
      std::printf("  renderer %s %d: %s, %s, %s%s, max texture %d, GLSL %s, video memory %lld MiB\n",
                  GL::backendName(r.backend), r.id, r.renderer.c_str(), r.vendor.c_str(), r.version.c_str(),
                  r.software ? " (software)" : "", r.maxTextureSize, r.shadingLanguageVersion.c_str(),
                  static_cast<long long>(r.videoMemoryBytes >> 20));
    }
    // OpenGL 2.0 and later have GL_SHADING_LANGUAGE_VERSION (the compatibility and legacy profiles)
    check("the context's GLSL version (GL_SHADING_LANGUAGE_VERSION)",
          c1->info().version.compare(0, 2, "1.") == 0 || !c1->info().shadingLanguageVersion.empty());
    {
      GL::CurrentContext current(*c1);
      check("CurrentContext on an OpenGL context: current in the driver", current.ok() && currentNativeContext() == c1->nativeContext());
      std::unique_ptr<GL::Context> other = GL::createContext(hooks, gBackends);
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
    checkCreatorThreadEnds(hooks);
    std::unique_ptr<GL::Context> c2 = GL::createContext(hooks, gBackends, -1, &error);
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
  std::printf("openglRenderChecks: %d failure(s)\n", GPUChecks::gFailures);
  return GPUChecks::gFailures ? 1 : 0;
}
