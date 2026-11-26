// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// The backend-independent part of ofxhOpenGLContext.h: Context, CurrentContext, ContextPool,
// createContext() and listRenderers().

#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLFunctions.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace OFX {
  namespace Host {
    namespace OpenGL {

      static const struct { Backend backend; const char *name; } kBackendNames[] = {
        { Backend::EGLSurfaceless, "egl-surfaceless" },
        { Backend::EGLDevice, "egl-device" },
        { Backend::CGL, "cgl" },
        { Backend::WGL, "wgl" },
      };

      const char *backendName(Backend backend)
      {
        for (const auto &b : kBackendNames) {
          if (b.backend == backend) {
            return b.name;
          }
        }
        return "unknown";
      }

      bool backendFromName(const std::string &name, Backend &backend)
      {
        for (const auto &b : kBackendNames) {
          if (name == b.name) {
            backend = b.backend;
            return true;
          }
        }
        return false;
      }

      std::vector<Backend> defaultBackends()
      {
#if defined(__APPLE__)
        return { Backend::CGL };
#elif defined(_WIN32)
        return { Backend::WGL };
#else
        return { Backend::EGLSurfaceless, Backend::EGLDevice };
#endif
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Context

      Context::Context(const Hooks &hooks)
        : _hooks(hooks)
        , _gl(new Detail::GLFunctions)
      {
      }

      Context::~Context() = default;

      ProcAddress Context::getProcAddress(const char *name) const
      {
        if (_hooks.getProcAddress) {
          return _hooks.getProcAddress(name);
        }
        return backendProcAddress(name);
      }

      void Context::driverCall(const std::function<void()> &call) const
      {
        Detail::driverCall(_hooks, call);
      }

      // The state of a context and of the threads (CurrentContext):
      // - free: _depth == 0; any thread may claim it (waiting while it isn't free);
      // - held by a thread (_owner): _depth > 0 (its scopes), current on it, tHeld of that thread;
      // - reserved by a thread: _depth > 0 and _suspended > 0 (suspend(), inside a render action
      //   around a fetch), current nowhere; only _owner may make it current (a nested render),
      //   until resume() and the end of the render.
      // A thread holds at most one context (tHeld), and may reserve several, innermost last:
      // tReserved is the innermost, each suspend() keeps the previous one and resume() restores
      // it. A thread that holds or reserves a context waits for no context (no deadlock).

      /// the context the thread holds (CurrentContext): a thread holds at most one
      static thread_local Context *tHeld = nullptr;
      /// the innermost context a render of this thread reserves while it is suspended
      static thread_local Context *tReserved = nullptr;

      bool Context::isCurrent() const
      {
        return tHeld == this;
      }

      Context *heldContext()
      {
        return tHeld;
      }

      Context *reservedContext()
      {
        return tReserved;
      }

      bool Context::claimedByAnotherThread() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _depth > 0 && _owner != std::this_thread::get_id();
      }

      static std::string glString(const Detail::GLFunctions &gl, Detail::GLenum name)
      {
        const Detail::GLubyte *s = gl.GetString(name);
        return s ? std::string(reinterpret_cast<const char *>(s)) : std::string();
      }

      bool Context::initialize(Backend backend, int rendererId, bool software, std::string *error)
      {
        _info.backend = backend;
        _info.id = rendererId;
        std::string missing;
        if (!Detail::loadGLFunctions(*this, *_gl, &missing)) {
          if (error) {
            *error = std::string("OpenGL function not found: ") + missing;
          }
          return false;
        }
        driverCall([&] {
          _info.vendor = glString(*_gl, Detail::kGLVendor);
          _info.renderer = glString(*_gl, Detail::kGLRenderer);
          _info.version = glString(*_gl, Detail::kGLVersion);
          _gl->GetIntegerv(Detail::kGLMaxTextureSize, &_info.maxTextureSize);
        });
        // Mesa's and Apple's software rasterizers
        static const char *const kSoftware[] = { "llvmpipe", "softpipe", "Software" };
        _info.software = software;
        for (const char *s : kSoftware) {
          _info.software = _info.software || _info.renderer.find(s) != std::string::npos;
        }
        return true;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // CurrentContext

      // the backend's make-current and done-current through the host's hook; they never throw
      // (also used in destructors): a throwing hook is a failed make-current
      bool CurrentContext::makeCurrent()
      {
        bool ok = false;
        try {
          _context.driverCall([&] { ok = _context.makeCurrentImpl(); });
        } catch (...) {
          ok = false;
        }
        return ok;
      }

      void CurrentContext::doneCurrent()
      {
        try {
          _context.driverCall([&] { _context.doneCurrentImpl(); });
        } catch (...) {
        }
      }

      CurrentContext::CurrentContext(Context &context)
        : _context(context)
      {
        if (tHeld == &context) {
          // nested in a scope of this thread
          std::lock_guard<std::mutex> lock(context._mutex);
          ++context._depth;
          _held = true;
        } else if (tHeld) {
          // the thread holds another context: refused
        } else if (reservedHere()) {
          // a render of this thread suspended it (getImage()): a render nested on this thread
          // uses it, the thread being serial
          if (makeCurrent()) {
            std::lock_guard<std::mutex> lock(context._mutex);
            ++context._depth;
            tHeld = &context;
            _held = _overSuspended = true;
          }
        } else {
          // a thread that reserves a context waits for no other one (no deadlock)
          _held = claim(1, /*wait=*/tReserved == nullptr);
        }
        _ok = _held;
      }

      bool CurrentContext::reservedHere() const
      {
        std::lock_guard<std::mutex> lock(_context._mutex);
        return _context._suspended > 0 && _context._owner == std::this_thread::get_id();
      }

      bool CurrentContext::claim(int depth, bool wait)
      {
        // claimed (depth > 0) before making it current, so that no other thread makes it current
        // in between; the driver is called without the lock (CP.22)
        {
          std::unique_lock<std::mutex> lock(_context._mutex);
          if (!wait && _context._depth != 0) {
            return false;
          }
          _context._released.wait(lock, [&] { return _context._depth == 0; });
          _context._depth = depth;
          _context._owner = std::this_thread::get_id();
        }
        if (!makeCurrent()) {
          std::lock_guard<std::mutex> lock(_context._mutex);
          _context._depth = 0; // no claim left behind
          _context._released.notify_all();
          return false;
        }
        tHeld = &_context;
        return true;
      }

      CurrentContext::~CurrentContext()
      {
        // (never throws: makeCurrent() and doneCurrent() catch a throwing Hooks::driverCall)
        if (_suspendedHere) {
          resume();
        }
        if (!_held && _released > 1) {
          reacquire(); // the enclosing scopes hold it
        }
        if (!_held) {
          return;
        }
        if (tHeld != &_context) {
          // the hold was lost (a failed reacquire() or resume() of a nested scope): if an
          // enclosing render of this thread still reserves the context, give back this scope's
          // increment; else _depth isn't ours
          std::lock_guard<std::mutex> lock(_context._mutex);
          if (_context._suspended > 0 && _context._owner == std::this_thread::get_id()) {
            --_context._depth;
          }
          return;
        }
        {
          std::lock_guard<std::mutex> lock(_context._mutex);
          if (_context._depth > 1 && !_overSuspended) {
            --_context._depth;
            return;
          }
          if (_overSuspended) {
            --_context._depth; // back to the suspended render's reservation
          }
        }
        doneCurrent();
        tHeld = nullptr;
        if (_overSuspended) {
          return;
        }
        std::lock_guard<std::mutex> lock(_context._mutex);
        _context._depth = 0;
        _context._released.notify_all();
      }

      void CurrentContext::suspend()
      {
        if (!_held || tHeld != &_context || _suspendedHere) {
          return;
        }
        doneCurrent();
        tHeld = nullptr;
        std::lock_guard<std::mutex> lock(_context._mutex);
        ++_context._suspended;
        _previousReserved = tReserved;
        tReserved = &_context;
        _suspendedHere = true;
      }

      bool CurrentContext::resume()
      {
        if (!_suspendedHere) {
          return ok();
        }
        _suspendedHere = false;
        const bool current = makeCurrent();
        std::lock_guard<std::mutex> lock(_context._mutex);
        --_context._suspended;
        tReserved = _previousReserved; // the enclosing renders' reservations
        if (!current) {
          // the hold is lost (the enclosing scopes see a lost hold): this scope's increment is
          // given back if an enclosing render still reserves the context, else the reservation
          // ends
          if (_context._suspended > 0) {
            --_context._depth;
          } else {
            _context._depth = 0;
            _context._released.notify_all();
          }
          _held = false;
          _ok = false;
          return false;
        }
        tHeld = &_context;
        return true;
      }

      void CurrentContext::release()
      {
        if (!_held) {
          return;
        }
        if (tHeld != &_context) {
          _held = false; // the hold was lost: _depth isn't ours
          _ok = false;
          return;
        }
        int depth = 0;
        {
          std::lock_guard<std::mutex> lock(_context._mutex);
          if (_context._suspended > 0) {
            return; // reserved by a suspended render of this thread: can't be released
          }
          depth = _context._depth; // (still claimed until it is released)
        }
        doneCurrent();
        tHeld = nullptr;
        {
          std::lock_guard<std::mutex> lock(_context._mutex);
          _context._depth = 0;
          _context._released.notify_all();
        }
        _held = false;
        _ok = false;
        _released = depth;
      }

      bool CurrentContext::reacquire()
      {
        if (_held) {
          return ok();
        }
        if (_released == 0 || tHeld) {
          return false; // not released, or the thread holds another context
        }
        _held = claim(_released, /*wait=*/tReserved == nullptr);
        if (_held) {
          _released = 0;
        }
        _ok = _held;
        return _held;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // createContext(), listRenderers()

      std::unique_ptr<Context> createContext(const Hooks &hooks, const std::vector<Backend> &backends,
                                             int rendererId, std::string *error)
      {
        std::string errors;
        for (Backend backend : backends) {
          std::string why;
          std::unique_ptr<Context> context = Detail::createBackendContext(hooks, backend, rendererId, why);
          if (context) {
            if (hooks.log) {
              const RendererInfo &info = context->info();
              hooks.log(std::string("OpenGL: ") + backendName(backend) + ": " + info.renderer + " (" +
                        info.vendor + ", " + info.version + ")");
            }
            return context;
          }
          const std::string message = std::string("OpenGL: ") + backendName(backend) + ": " + why;
          if (hooks.log) {
            hooks.log(message);
          }
          errors += (errors.empty() ? "" : "; ") + message;
        }
        if (error) {
          *error = backends.empty() ? std::string("OpenGL: no context backend in this build") : errors;
        }
        return nullptr;
      }

      std::vector<RendererInfo> listRenderers(const Hooks &hooks, const std::vector<Backend> &backends)
      {
        std::vector<RendererInfo> renderers;
        for (Backend backend : backends) {
          for (int id : Detail::rendererIds(hooks, backend)) {
            std::string why;
            std::unique_ptr<Context> context = Detail::createBackendContext(hooks, backend, id, why);
            if (context) {
              renderers.push_back(context->info());
            } else if (hooks.log) {
              hooks.log(std::string("OpenGL: ") + backendName(backend) + " renderer " + std::to_string(id) + ": " + why);
            }
          }
        }
        return renderers;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // ContextPool

      ContextPool::ContextPool(Factory factory, int maxContexts)
        : _factory(std::move(factory))
        , _max(std::max(1, maxContexts))
      {
      }

      ContextPool::~ContextPool() = default;

      Context *ContextPool::acquire(Key key, const std::function<bool()> &abort)
      {
        if (heldContext() || reservedContext()) {
          return tryAcquire(key); // a thread that holds or reserves a context waits for none
        }
        std::unique_lock<std::mutex> lock(_mutex);
        bool factoryFailed = false; // by this call: don't retry it in a loop
        for (;;) {
          // the context key holds, else the idle one it held last, else any idle one
          Slot *idle = nullptr;
          bool creating = false; // key's context is being created by another thread
          for (Slot &slot : _slots) {
            if (slot.holds > 0 && slot.key == key) {
              if (slot.context) {
                ++slot.holds;
                return slot.context.get();
              }
              creating = true;
            } else if (slot.holds == 0 && slot.context && (!idle || slot.key == key)) {
              idle = &slot;
            }
          }
          if (!creating && idle) {
            idle->key = key;
            idle->holds = 1;
            return idle->context.get();
          }
          if (!creating && !factoryFailed && static_cast<int>(_slots.size()) < _max) {
            if (Context *context = create(lock, key)) {
              return context;
            }
            factoryFailed = true;
            continue;
          }
          if (factoryFailed && _slots.empty()) {
            return nullptr; // no context can be created
          }
          if (abort && abort()) {
            return nullptr;
          }
          _changed.wait(lock);
          if (abort && abort()) {
            return nullptr;
          }
        }
      }

      Context *ContextPool::create(std::unique_lock<std::mutex> &lock, Key key)
      {
        // create it without the lock: creating a context may be slow. Reserve the slot.
        const auto slot = _slots.emplace(_slots.end());
        slot->key = key;
        slot->holds = 1;
        lock.unlock();
        std::unique_ptr<Context> context;
        try {
          context = _factory();
        } catch (...) {
        }
        lock.lock();
        _changed.notify_all(); // the waiters for key's context, or for a free slot
        if (context) {
          slot->context = std::move(context);
          return slot->context.get();
        }
        _slots.erase(slot);
        return nullptr;
      }

      Context *ContextPool::tryAcquire(Key key)
      {
        std::unique_lock<std::mutex> lock(_mutex);
        Slot *idle = nullptr;
        for (Slot &slot : _slots) {
          if (slot.holds > 0 && slot.key == key) {
            // key's context, unless a thread holds or reserves it (or it is being created)
            if (!slot.context || slot.context->claimedByAnotherThread()) {
              return nullptr;
            }
            ++slot.holds;
            return slot.context.get();
          }
          if (slot.holds == 0 && slot.context && (!idle || slot.key == key)) {
            idle = &slot;
          }
        }
        if (idle) {
          idle->key = key;
          idle->holds = 1;
          return idle->context.get();
        }
        return static_cast<int>(_slots.size()) < _max ? create(lock, key) : nullptr;
      }

      void ContextPool::release(Key key)
      {
        std::lock_guard<std::mutex> lock(_mutex);
        for (Slot &slot : _slots) {
          if (slot.holds > 0 && slot.key == key) {
            if (--slot.holds == 0) {
              _changed.notify_all();
            }
            return;
          }
        }
      }

      void ContextPool::wake()
      {
        std::lock_guard<std::mutex> lock(_mutex);
        _changed.notify_all();
      }

      int ContextPool::size() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        int n = 0;
        for (const Slot &slot : _slots) {
          n += slot.context ? 1 : 0;
        }
        return n;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // the OpenGL functions

      namespace Detail {
        template <typename F>
        static bool load(const Context &context, F &f, const char *name, const char *alternative = nullptr)
        {
          ProcAddress p = context.getProcAddress(name);
          if (!p && alternative) {
            p = context.getProcAddress(alternative);
          }
          f = reinterpret_cast<F>(p);
          return p != nullptr;
        }

        bool loadGLFunctions(const Context &context, GLFunctions &gl, std::string *missing)
        {
#define OFXH_LOAD(f, ...) \
          if (!load(context, gl.f, "gl" #f, ##__VA_ARGS__)) { if (missing) *missing = "gl" #f; return false; }
          OFXH_LOAD(GetString)
          OFXH_LOAD(GetIntegerv)
          OFXH_LOAD(GetFloatv)
          OFXH_LOAD(GetBooleanv)
          OFXH_LOAD(IsEnabled)
          OFXH_LOAD(Enable)
          OFXH_LOAD(Disable)
          OFXH_LOAD(ColorMask)
          OFXH_LOAD(GetError)
          OFXH_LOAD(GenTextures)
          OFXH_LOAD(DeleteTextures)
          OFXH_LOAD(BindTexture)
          OFXH_LOAD(IsTexture)
          OFXH_LOAD(TexImage2D)
          OFXH_LOAD(TexSubImage2D)
          OFXH_LOAD(TexParameteri)
          OFXH_LOAD(PixelStorei)
          OFXH_LOAD(ReadPixels)
          OFXH_LOAD(Viewport)
          OFXH_LOAD(ClearColor)
          OFXH_LOAD(Clear)
          OFXH_LOAD(Finish)
          OFXH_LOAD(GenFramebuffers, "glGenFramebuffersEXT")
          OFXH_LOAD(DeleteFramebuffers, "glDeleteFramebuffersEXT")
          OFXH_LOAD(BindFramebuffer, "glBindFramebufferEXT")
          OFXH_LOAD(FramebufferTexture2D, "glFramebufferTexture2DEXT")
          OFXH_LOAD(CheckFramebufferStatus, "glCheckFramebufferStatusEXT")
#undef OFXH_LOAD
          // the pixel buffer bindings (GL_PIXEL_*_BUFFER_BINDING): OpenGL 2.1 or
          // GL_ARB_pixel_buffer_object (glBindBuffer alone is OpenGL 1.5, without them)
          const GLubyte *version = gl.GetString(kGLVersion);
          const GLubyte *extensions = gl.GetString(0x1F03 /* GL_EXTENSIONS */);
          int major = 0, minor = 0;
          if (version) {
            std::sscanf(reinterpret_cast<const char *>(version), "%d.%d", &major, &minor);
          }
          const char *e = extensions ? reinterpret_cast<const char *>(extensions) : "";
          if (major > 2 || (major == 2 && minor >= 1) || std::strstr(e, "GL_ARB_pixel_buffer_object") ||
              std::strstr(e, "GL_EXT_pixel_buffer_object")) {
            load(context, gl.BindBuffer, "glBindBuffer", "glBindBufferARB");
          }
          return true;
        }
      } // Detail

    } // OpenGL
  } // Host
} // OFX
