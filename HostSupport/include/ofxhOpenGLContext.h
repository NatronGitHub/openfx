#ifndef OFXH_OPENGL_CONTEXT_H
#define OFXH_OPENGL_CONTEXT_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhOpenGLContext.h

  Offscreen OpenGL contexts for the OpenGL render suite (OfxImageEffectOpenGLRenderSuiteV1,
  ofxGPURender.h): backends chosen at run time, the renderers they offer, a context pool and a
  scope that makes a context current on the calling thread.

  Part of the optional library openfx::HostSupportOpenGL (CMake option OFX_HOSTSUPPORT_OPENGL);
  ofxhOpenGLTexture.h and ofxhOpenGLRender.h build on it. No OpenGL, EGL, GLX or CGL header is
  needed: EGL, GLX (libGL) and libX11 are loaded at run time, CGL is a system framework, WGL is
  opengl32.dll (linked).
  No global state: everything hangs off the objects the host creates, except the context each
  thread holds (thread_local, see CurrentContext) and, with WGL, each thread's hidden windows.

  Backends (Backend):
  - EGLSurfaceless: EGL_MESA_platform_surfaceless (Mesa, Linux without a window system; the
    GPU of the default render node, or llvmpipe);
  - EGLDevice: EGL_EXT_platform_device (one renderer per EGL device, e.g. NVIDIA without X11);
  - CGL: macOS;
  - WGL: Windows (the driver's renderer, or a GPU chosen with WGL_NV_gpu_affinity or, in 64-bit
    builds, WGL_AMD_gpu_association). A context is made current on a hidden window of the
    calling thread (one per thread and pixel format, destroyed when the thread ends), so it
    may be used on any thread, whichever thread created it; on a GPU, on its affinity DC, or as
    an associated context.
  - GLX: X11 (GLX 1.3, libGL with or without GLVND), on an X connection of the context's own
    ($DISPLAY) and a 1x1 pbuffer: for X11 without GLVND or without an EGL driver. libX11 is
    loaded at run time; the library serializes its own Xlib calls that touch Xlib's global state
    (opening and closing connections, and the X error handler it sets while it creates or
    destroys a context, so that an X error makes that attempt fail instead of exiting). A host
    that uses Xlib from several threads with libX11 older than 1.8 (e.g. EL9's 1.7) calls
    XInitThreads() first, before any other Xlib call; libX11 1.8 does it by default.
  defaultBackends(): CGL; WGL; on Linux and BSD, EGLSurfaceless, EGLDevice, then GLX. A new backend
  implements the Context interface (make current, done current, getProcAddress, its native
  context), a renderer listing, and a case in the backend dispatch (Detail::createBackendContext()
  and Detail::rendererIds(): a switch on Backend on Linux and BSD, one backend each on macOS and
  Windows).

  The contexts have the compatibility (macOS: legacy) profile, as OFX OpenGL plug-ins may use
  the fixed-function pipeline, and no default framebuffer (WGL: a 1x1 one, unused): rendering
  goes to textures (ofxhOpenGLTexture.h).

  Typical use, one context per render tree:

    OFX::Host::OpenGL::Hooks hooks;          // logger, loader, FP-exception masking (optional)
    OFX::Host::OpenGL::ContextPool pool([hooks] { return OFX::Host::OpenGL::createContext(hooks); }, 2);
    ...
    OFX::Host::OpenGL::ContextPool::Lease lease(pool, renderTreeKey);   // may wait
    if (lease.context()) {
      OFX::Host::OpenGL::CurrentContext current(*lease.context());     // current on this thread
      if (current.ok()) { ... render (ofxhOpenGLRender.h) ... }
    }
*/

#include "ofxCore.h"
#include "ofxhUtilities.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OFX {
  namespace Host {
    namespace OpenGL {

/// The calling convention of the OpenGL functions (APIENTRY)
#ifdef _WIN32
#define OFXH_GLAPIENTRY __stdcall
#else
#define OFXH_GLAPIENTRY
#endif

      /// an OpenGL (or EGL/CGL) function, as returned by a loader
      typedef void (OFXH_GLAPIENTRY *ProcAddress)();

      /// Offscreen context backends (see the file documentation)
      enum class Backend { EGLSurfaceless, EGLDevice, CGL, WGL, GLX };

      /// "egl-surfaceless", "egl-device", "cgl", "wgl", "glx"
      const char *backendName(Backend backend);
      /// the backend called name (backendName()); false if none
      bool backendFromName(const std::string &name, Backend &backend);
      /// the backends of this build and platform, in the default preference order
      /// (Linux and BSD: EGLSurfaceless, EGLDevice, GLX; macOS: CGL; Windows: WGL)
      std::vector<Backend> defaultBackends();


      /// Services the host gives the contexts; all optional
      struct Hooks {
        /// receives the messages, with their level (OFX::Host::LogLevel, ofxhUtilities.h; default: dropped)
        std::function<void(LogLevel level, const std::string &message)> log;
        /// runs call, a call into the context API or the OpenGL driver. Drivers may raise
        /// floating-point exceptions: a host that unmasks them masks them here (default: calls it).
        /// It runs with no lock of this library held, but may run while the thread holds a
        /// context: it must not make another context current.
        std::function<void(const std::function<void()> &call)> driverCall;
        /// the OpenGL function loader, e.g. the host's own (default: the backend's,
        /// eglGetProcAddress then libOpenGL/libGL; the OpenGL framework; wglGetProcAddress, for
        /// the current context, then opengl32.dll). nullptr if not found.
        std::function<ProcAddress(const char *name)> getProcAddress;
      };

      /// A renderer (a GPU or a software rasterizer) a backend can create contexts on
      struct RendererInfo {
        Backend backend = Backend::EGLSurfaceless;
        /// which renderer of the backend, for createContext(): the EGL device index, the CGL
        /// renderer ID, the WGL GPU (NVIDIA's GPU index, AMD's GPU ID; then listRenderers()
        /// lists the GPUs only); -1: the default (the only one of EGLSurfaceless, GLX, and WGL
        /// without a GPU choice), which createContext() accepts on every backend. Stable while
        /// the system's GPUs and drivers don't change.
        int id = -1;
        std::string vendor, renderer, version;  ///< GL_VENDOR, GL_RENDERER, GL_VERSION
        bool software = false;                  ///< no hardware acceleration
        int maxTextureSize = 0;                 ///< GL_MAX_TEXTURE_SIZE
        /// GL_SHADING_LANGUAGE_VERSION (OpenGL 2.0), "" if none
        std::string shadingLanguageVersion;
        /// the renderer's dedicated video memory, 0 if the driver doesn't say:
        /// GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX (GL_NVX_gpu_memory_info), CGL's
        /// kCGLRPVideoMemoryMegabytes, GLX_RENDERER_VIDEO_MEMORY_MESA (GLX_MESA_query_renderer:
        /// Mesa's drivers, e.g. Intel and AMD on Linux), WGL_GPU_RAM_AMD (WGL_AMD_gpu_association)
        std::int64_t videoMemoryBytes = 0;
      };

      namespace Detail { struct GLFunctions; }

      /// An offscreen OpenGL context. Use it through CurrentContext, on one thread at a time;
      /// destroy it when it isn't current.
      class Context {
      public:
        virtual ~Context();
        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;

        /// the renderer, backend and GL strings of this context
        const RendererInfo &info() const { return _info; }
        /// an OpenGL function (Hooks::getProcAddress, else the backend's); nullptr if not found.
        /// WGL: call it with this context current (wglGetProcAddress); with none current, only the
        /// OpenGL 1.1 functions are found (opengl32.dll).
        ProcAddress getProcAddress(const char *name) const;
        /// the host's hooks (Hooks::driverCall wraps the driver calls of ofxhOpenGLTexture.h)
        const Hooks &hooks() const { return _hooks; }
        /// runs call through Hooks::driverCall
        void driverCall(const std::function<void()> &call) const;
        /// the OpenGL functions HostSupport uses (loaded when the context is created)
        const Detail::GLFunctions &gl() const { return *_gl; }
        /// the context is current on the calling thread (held by a CurrentContext of this thread)
        bool isCurrent() const;
        /// the backend's context: an EGLContext, a CGLContextObj, an HGLRC
        virtual void *nativeContext() const = 0;
        /// the instances attached to it (kOfxActionOpenGLContextAttached without Detached yet,
        /// see Attachment in ofxhOpenGLRender.h)
        int attachments() const;

      protected:
        explicit Context(const Hooks &hooks);
        /// backend: make the context current on (respectively release it from) this thread
        virtual bool makeCurrentImpl() = 0;
        virtual void doneCurrentImpl() = 0;
        /// backend: its own loader
        virtual ProcAddress backendProcAddress(const char *name) const = 0;
        /// backend: called once current, fills _info's GL strings and loads the functions
        bool initialize(Backend backend, int rendererId, bool software, std::string *error);
        /// backend: info() as it fills it, after initialize(), with what the backend knows and
        /// OpenGL doesn't say (e.g. CGL's video memory); info() itself is const
        RendererInfo &backendInfo() { return _info; }

      private:
        friend class CurrentContext;
        friend class ContextPool;
        friend class Attachment;
        /// claimed (held, or reserved by a suspended render) by a thread other than the caller
        bool claimedByAnotherThread() const;
        /// no thread holds or reserves it, and no instance is attached to it: it can be destroyed
        bool unused() const;
        /// Attachment: an instance was attached (+1) or detached (-1)
        void attachmentChanged(int delta);
        Hooks _hooks;
        RendererInfo _info;
        std::unique_ptr<Detail::GLFunctions> _gl;
        mutable std::mutex _mutex;
        std::condition_variable _released;
        /// the CurrentContext scopes of the thread that holds it (0: free). The holder keeps it
        /// at 1 or more while it makes the context current or releases it, so that no other
        /// thread makes it current in between.
        int _depth = 0;
        /// suspended renders of the holding thread (CurrentContext::suspend()): while > 0, the
        /// context is reserved to that thread but current nowhere
        int _suspended = 0;
        std::thread::id _owner;   ///< the thread that claimed it last
        int _attachments = 0;     ///< attachments()
      };

      /// the context the calling thread holds (through a CurrentContext), nullptr if none
      Context *heldContext();
      /// the innermost context a render of the calling thread reserves while suspended
      /// (CurrentContext::suspend()), nullptr if none. For a render nested in getImage() on this
      /// thread: it opens its CurrentContext on reservedContext() instead of asking the pool.
      Context *reservedContext();

      /// A new context: the first backend of backends that succeeds, on its renderer rendererId
      /// (RendererInfo::id of that backend; -1: its default). nullptr if none: then *error (if
      /// not null) says why, and the messages went to hooks.log. The context that was current on
      /// the calling thread (the host's, or one held by a CurrentContext) is current again after.
      std::unique_ptr<Context> createContext(const Hooks &hooks,
                                             const std::vector<Backend> &backends = defaultBackends(),
                                             int rendererId = -1,
                                             std::string *error = nullptr);

      /// The renderers of the given backends (a context is created on each to read its strings)
      std::vector<RendererInfo> listRenderers(const Hooks &hooks,
                                              const std::vector<Backend> &backends = defaultBackends());

      /// Makes a context current on the calling thread for the lifetime of the scope, waiting
      /// while another thread holds or reserves it; on exit, releases it from the thread. Scopes
      /// on the same context nest on a thread. A thread holds at most one context: a scope on
      /// another context while the thread holds one fails (ok() false, the held one stays
      /// current). While a render of the thread is suspended (suspend()), a scope on its context
      /// makes it current for a render nested on the thread, and a scope on another context
      /// doesn't wait: it fails if another thread holds that context.
      /// A thread that holds or reserves a context never waits for another one, so contexts
      /// can't deadlock (the lock order idea of CP.21, for contexts). A thread must not end while it
      /// holds or reserves a context: its scopes end before the thread (on WGL, the context would be
      /// left current on the hidden window of an ended thread).
      class CurrentContext {
      public:
        explicit CurrentContext(Context &context);
        ~CurrentContext();
        CurrentContext(const CurrentContext &) = delete;
        CurrentContext &operator=(const CurrentContext &) = delete;

        /// the context is current on this thread through this scope (false if the thread holds
        /// another context, if the backend failed to make it current, while it is released, and
        /// for the enclosing scopes while a nested one is released)
        bool ok() const { return _ok && _context.isCurrent(); }
        Context &context() const { return _context; }
        /// Between render actions (Attachment switching contexts): frees the context to other
        /// threads. Suspends the thread's hold on the context, with the enclosing scopes on it: the
        /// context is no longer current on this thread, and another thread (or a new scope of this
        /// thread) may make it current. Call it on the innermost scope of the thread only.
        /// reacquire() makes it current again for all of them (waiting while another thread holds
        /// it); the destructor does it too if an enclosing scope needs it. If it fails, the hold is
        /// lost: no scope of the thread is ok(), and their destructors leave the context alone.
        /// (not while a render of the thread is suspended: then it does nothing)
        void release();
        bool reacquire();

        /// Inside a render action (around a fetch): keeps the context reserved. Suspends a render
        /// on this scope (the innermost one of the thread) while it waits for
        /// something else (RenderSession::loadTexture() around ClipInstance::getImage()): the
        /// context is current nowhere, so the thread holds none while it waits, but it stays
        /// reserved to this render: no other thread may make it current until resume() and the
        /// end of the render. A render nested on this thread may use it (a new scope on it).
        void suspend();
        /// Makes the context current again after suspend(). false: the hold is lost (the
        /// backend failed), as after a failed reacquire()
        bool resume();

      private:
        bool claim(int depth, bool wait);
        bool reservedHere() const;
        bool makeCurrent();
        void doneCurrent();
        Context &_context;
        bool _ok = false;
        bool _held = false;
        int _released = 0;          ///< after release(): the depth to restore
        bool _suspendedHere = false; ///< suspend() without resume() yet
        Context *_previousReserved = nullptr; ///< reservedContext() before suspend()
        bool _overSuspended = false; ///< a nested render's scope on a context its thread reserves
      };

      /// A pool of contexts lent to renders: one context per render key (a render tree, chosen
      /// by the host), at most maxContexts contexts. Destroy it after the attachments
      /// (ofxhOpenGLRender.h) and once no context is acquired.
      class ContextPool {
      public:
        typedef std::function<std::unique_ptr<Context>()> Factory;
        typedef const void *Key;

        /// factory creates a context (e.g. calls createContext()), nullptr if it can't
        ContextPool(Factory factory, int maxContexts);
        ~ContextPool();
        ContextPool(const ContextPool &) = delete;
        ContextPool &operator=(const ContextPool &) = delete;

        /// A key per render tree; keep its Lease for the whole render, including the
        /// ClipInstance::getImage() calls of its render actions. The context of render key: the
        /// one key holds already (waiting if it is still being
        /// created), else an idle one (preferably the last one key held), else a new one if
        /// there are fewer than maxContexts, else waits until one is released or abort()
        /// returns true (checked whenever the waiter is woken, see wake()). nullptr if aborted,
        /// or if no context exists and the factory fails; then nothing is held. Each non-null
        /// result needs a release(key). A failed factory call is retried by a later acquire().
        /// Called from a thread that holds or reserves a context, it waits for nothing: it is
        /// tryAcquire(). Don't call it on behalf of a render in progress on another thread (a
        /// fetch's tile renders): it may wait forever for the context that render reserves (such a
        /// thread can't tell, reservedContext() being per thread); use tryAcquire().
        Context *acquire(Key key, const std::function<bool()> &abort = {});
        /// Like acquire(), but never waits: for a render on another thread on behalf of a render
        /// in progress (the tiles of an input that a fetch renders on demand), which must not
        /// wait for the context that render reserves. With a key of its own, it gets an idle or
        /// a new context (up to the maximum); nullptr instead of waiting, then the host renders
        /// on the CPU if the plug-in can (chooseRenderMode()), else fails that render.
        Context *tryAcquire(Key key);
        /// gives back a context acquired for key
        void release(Key key);
        /// wakes the waiting acquire() calls, e.g. after setting the flag their abort() reads
        void wake();
        /// the number of contexts created
        int size() const;
        /// The maximum number of contexts, which a host may change at run time (e.g. a setting).
        /// More: the waiting acquire() calls may lend or create contexts. Fewer: at most this many
        /// are lent at once (acquire() waits, tryAcquire() gives nullptr, until releases bring
        /// the count below it), and none is created; the contexts made already stay allocated
        /// (instances may be attached to them) until the pool is destroyed.
        void setMaxContexts(int maxContexts);
        int maxContexts() const;
        /// Destroys the idle contexts that no instance is attached to (Context::attachments()) and
        /// no thread holds, with the GPU memory they hold: OfxImageEffectOpenGLRenderSuiteV1's
        /// flushResources, "When a plug-in fails to allocate GPU resources, it can call this
        /// function to request the host to flush its GPU resources if it holds any"
        /// (ofxGPURender.h). The contexts lent, and those instances are attached to, stay. Returns
        /// the number destroyed; acquire() creates new ones as needed. A context is checked under
        /// the pool's lock and destroyed after it: while releaseIdle() runs, no thread may claim a
        /// pool context outside a lease (a CurrentContext without acquire(), other than through
        /// Attachment, whose contexts stay). An Attachment destroyed without sending Detached on a
        /// context no longer counts there.
        int releaseIdle();

        /// acquire() and release() as a scope (release() only if acquire() gave a context)
        class Lease {
        public:
          Lease(ContextPool &pool, Key key, const std::function<bool()> &abort = {})
            : _pool(pool), _key(key), _context(pool.acquire(key, abort)) {}
          ~Lease() { if (_context) _pool.release(_key); }
          Lease(const Lease &) = delete;
          Lease &operator=(const Lease &) = delete;
          Context *context() const { return _context; }
        private:
          ContextPool &_pool;
          Key _key;
          Context *_context;
        };

      private:
        /// a new context for key in a new slot, created without the lock; nullptr if the
        /// factory fails
        Context *create(std::unique_lock<std::mutex> &lock, Key key);
        int inUse() const; ///< the slots lent (holds > 0); with _mutex held
        struct Slot {
          std::unique_ptr<Context> context; ///< nullptr while the factory creates it
          Key key = nullptr;    ///< the render holding it, or the last one if idle
          int holds = 0;        ///< acquire() calls not released yet
        };
        Factory _factory;
        int _max;
        mutable std::mutex _mutex;
        std::condition_variable _changed;
        std::list<Slot> _slots;   ///< (a list: a slot stays put while its context is created)
      };

      /// flushResources (OfxImageEffectOpenGLRenderSuiteV1) for a host whose contexts are pool's: a
      /// host's ImageEffect::Host::flushOpenGLResources() returns flushResources(pool), which
      /// releases pool's idle contexts (ContextPool::releaseIdle()) and returns "kOfxStatOK - the
      /// host has actually released some resources" if it destroyed one, else "kOfxStatReplyDefault -
      /// nothing the host could do.." (ofxGPURender.h)
      OfxStatus flushResources(ContextPool &pool);

    } // OpenGL
  } // Host
} // OFX

#endif // OFXH_OPENGL_CONTEXT_H
