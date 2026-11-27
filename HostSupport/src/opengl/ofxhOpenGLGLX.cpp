// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// The GLX backend of ofxhOpenGLContext.h (Linux and BSD, X11): GLX 1.3 contexts on an X connection
// of their own, made current on a 1x1 GLX pbuffer. libGL and libX11 are loaded at run time:
// libGL.so.1 is GLVND's dispatcher or, without GLVND, the driver's own libGL, and both export the
// GLX functions. For hosts whose other contexts are GLX ones (e.g. a Qt viewer on X11 without
// GLVND, where OpenGL functions from EGL wouldn't work with them). The EGL backends are in
// ofxhOpenGLEGL.cpp.

#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLFunctions.h"
#include "ofxhOpenGLX11.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <dlfcn.h>

#include <cstdint>
#include <cstring>

namespace OFX {
  namespace Host {
    namespace OpenGL {
      namespace Detail {

        // the GLX 1.3 types and enums used here (GL/glx.h, GL/glxext.h)
        typedef void *GLXFBConfig;
        typedef void *GLXContext;
        typedef unsigned long GLXDrawable; // (an XID)
        static const int kGLXNone = 0;
        static const int kGLXRedSize = 8, kGLXGreenSize = 9, kGLXBlueSize = 10, kGLXAlphaSize = 11;
        static const int kGLXDrawableType = 0x8010, kGLXRenderType = 0x8011;
        static const int kGLXRGBABit = 0x0001, kGLXPbufferBit = 0x0004, kGLXRGBAType = 0x8014;
        static const int kGLXPbufferHeight = 0x8040, kGLXPbufferWidth = 0x8041;
        static const int kGLXContextProfileMask = 0x9126;           // GLX_CONTEXT_PROFILE_MASK_ARB
        static const int kGLXContextCompatibilityProfileBit = 0x0002; // ..._COMPATIBILITY_PROFILE_BIT_ARB
        static const int kGLXRendererVideoMemoryMESA = 0x8187;        // GLX_RENDERER_VIDEO_MEMORY_MESA

        /// libGL's GLX functions; the library is never unloaded (RTLD_NODELETE: drivers don't
        /// survive an unload)
        struct GLXLibrary {
          int (*QueryVersion)(void *, int *, int *) = nullptr;
          const char *(*QueryExtensionsString)(void *, int) = nullptr;
          GLXFBConfig *(*ChooseFBConfig)(void *, int, const int *, int *) = nullptr;
          GLXContext (*CreateNewContext)(void *, GLXFBConfig, int, GLXContext, int) = nullptr;
          void (*DestroyContext)(void *, GLXContext) = nullptr;
          GLXDrawable (*CreatePbuffer)(void *, GLXFBConfig, const int *) = nullptr;
          void (*DestroyPbuffer)(void *, GLXDrawable) = nullptr;
          int (*MakeContextCurrent)(void *, GLXDrawable, GLXDrawable, GLXContext) = nullptr;
          GLXContext (*GetCurrentContext)() = nullptr;
          void *(*GetCurrentDisplay)() = nullptr;
          GLXDrawable (*GetCurrentDrawable)() = nullptr;
          GLXDrawable (*GetCurrentReadDrawable)() = nullptr;
          ProcAddress (*GetProcAddress)(const unsigned char *) = nullptr;
          GLXContext (*CreateContextAttribs)(void *, GLXFBConfig, GLXContext, int, const int *) = nullptr;
          int (*QueryCurrentRendererInteger)(int, unsigned int *) = nullptr; // GLX_MESA_query_renderer
          const X11Library *x11 = nullptr;

          bool load(std::string &error)
          {
            x11 = X11Library::get();
            void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            if (!gl || !x11) {
              error = gl ? "libX11 not found" : "libGL not found";
              return false;
            }
#define OFXH_GLX(f) f = reinterpret_cast<decltype(f)>(dlsym(gl, "glX" #f)); if (!f) { error = "glX" #f " not found"; return false; }
            OFXH_GLX(QueryVersion)
            OFXH_GLX(QueryExtensionsString)
            OFXH_GLX(ChooseFBConfig)
            OFXH_GLX(CreateNewContext)
            OFXH_GLX(DestroyContext)
            OFXH_GLX(CreatePbuffer)
            OFXH_GLX(DestroyPbuffer)
            OFXH_GLX(MakeContextCurrent)
            OFXH_GLX(GetCurrentContext)
            OFXH_GLX(GetCurrentDisplay)
            OFXH_GLX(GetCurrentDrawable)
            OFXH_GLX(GetCurrentReadDrawable)
#undef OFXH_GLX
            GetProcAddress = reinterpret_cast<decltype(GetProcAddress)>(dlsym(gl, "glXGetProcAddressARB"));
            if (!GetProcAddress) {
              GetProcAddress = reinterpret_cast<decltype(GetProcAddress)>(dlsym(gl, "glXGetProcAddress"));
            }
            if (!GetProcAddress) {
              error = "glXGetProcAddressARB not found";
              return false;
            }
            return true;
          }
        };

        class GLXBackendContext : public Context {
        public:
          GLXBackendContext(const Hooks &hooks, const GLXLibrary &glx) : Context(hooks), _glx(glx) {}

          ~GLXBackendContext() override
          {
            if (!_display) {
              return;
            }
            driverCall([&] {
              X11ErrorTrap trap(*_glx.x11, _display.get());
              if (_context) {
                _glx.DestroyContext(_display.get(), _context);
              }
              if (_pbuffer) {
                _glx.DestroyPbuffer(_display.get(), _pbuffer);
              }
            });
            // (then _display closes the X connection)
          }

          bool create(std::string &error)
          {
            _display = openX11Display(error);
            if (!_display) {
              return false;
            }
            bool ok = false;
            driverCall([&] {
              X11ErrorTrap trap(*_glx.x11, _display.get());
              ok = createOn(trap, error);
            });
            if (!ok) {
              return false;
            }
            // the thread's current context (the host's, or one it holds), restored after
            void *previousDisplay = nullptr;
            GLXContext previous = nullptr;
            GLXDrawable draw = 0, read = 0;
            bool current = false;
            driverCall([&] {
              previous = _glx.GetCurrentContext();
              previousDisplay = _glx.GetCurrentDisplay();
              draw = _glx.GetCurrentDrawable();
              read = _glx.GetCurrentReadDrawable();
              X11ErrorTrap trap(*_glx.x11, _display.get());
              current = makeCurrentImpl() && !trap.failed();
            });
            if (current) {
              ok = initialize(Backend::GLX, -1, false, &error);
              if (ok) {
                std::int64_t bytes = 0;
                driverCall([&] { bytes = videoMemory(); });
                if (bytes > 0) {
                  backendInfo().videoMemoryBytes = bytes; // (else NVIDIA's, if initialize() found it)
                }
              }
            } else {
              error = "glXMakeContextCurrent failed";
              ok = false;
            }
            driverCall([&] {
              X11ErrorTrap trap(*_glx.x11, _display.get());
              if (previous && previousDisplay) {
                _glx.MakeContextCurrent(previousDisplay, draw, read, previous);
              } else {
                doneCurrentImpl();
              }
            });
            return ok;
          }

          void *nativeContext() const override { return _context; }

        protected:
          bool makeCurrentImpl() override
          {
            return _glx.MakeContextCurrent(_display.get(), _pbuffer, _pbuffer, _context) != 0;
          }
          void doneCurrentImpl() override { _glx.MakeContextCurrent(_display.get(), kGLXNone, kGLXNone, nullptr); }

          ProcAddress backendProcAddress(const char *name) const override
          {
            // "glXGetProcAddressARB [...] returns the address of the extension function named by
            // procName" (GLX_ARB_get_proc_address): of the core functions too with GLVND and Mesa
            return _glx.GetProcAddress(reinterpret_cast<const unsigned char *>(name));
          }

        private:
          /// with the context current: "Number of megabytes of video memory available to the
          /// renderer" (GLX_RENDERER_VIDEO_MEMORY_MESA, GLX_MESA_query_renderer: Mesa's drivers,
          /// e.g. Intel and AMD), 0 if unknown
          std::int64_t videoMemory() const
          {
            const char *ext = _glx.QueryExtensionsString(_display.get(), _glx.x11->DefaultScreen(_display.get()));
            if (!ext || !hasExtension(ext, "GLX_MESA_query_renderer")) {
              return 0;
            }
            const auto query = reinterpret_cast<decltype(_glx.QueryCurrentRendererInteger)>(
              _glx.GetProcAddress(reinterpret_cast<const unsigned char *>("glXQueryCurrentRendererIntegerMESA")));
            unsigned int mb = 0;
            return query && query(kGLXRendererVideoMemoryMESA, &mb) ? std::int64_t(1024) * 1024 * mb : 0;
          }

          /// the config, context and pbuffer, with X errors going to trap: an X error fails the
          /// attempt (then the next one, or the next backend, is tried)
          bool createOn(X11ErrorTrap &trap, std::string &error)
          {
            void *dpy = _display.get();
            int major = 0, minor = 0;
            // glXChooseFBConfig, glXCreateNewContext, glXCreatePbuffer and glXMakeContextCurrent
            // are GLX 1.3
            if (!_glx.QueryVersion(dpy, &major, &minor) || major < 1 || (major == 1 && minor < 3)) {
              error = "GLX 1.3 not supported";
              return false;
            }
            const int screen = _glx.x11->DefaultScreen(dpy);
            const int attribs[] = { kGLXDrawableType, kGLXPbufferBit, kGLXRenderType, kGLXRGBABit,
                                    kGLXRedSize, 8, kGLXGreenSize, 8, kGLXBlueSize, 8, kGLXAlphaSize, 8, kGLXNone };
            int n = 0;
            GLXFBConfig *configs = _glx.ChooseFBConfig(dpy, screen, attribs, &n);
            if (!configs || n < 1) {
              if (configs) {
                _glx.x11->Free(configs);
              }
              error = "no GLX config with a pbuffer";
              return false;
            }
            const GLXFBConfig config = configs[0];
            _glx.x11->Free(configs);
            // the compatibility profile: GLX_ARB_create_context_profile, else glXCreateNewContext
            // (whose context is a compatibility one)
            const char *ext = _glx.QueryExtensionsString(dpy, screen);
            if (ext && hasExtension(ext, "GLX_ARB_create_context_profile")) {
              const auto create = reinterpret_cast<decltype(_glx.CreateContextAttribs)>(
                _glx.GetProcAddress(reinterpret_cast<const unsigned char *>("glXCreateContextAttribsARB")));
              const int profile[] = { kGLXContextProfileMask, kGLXContextCompatibilityProfileBit, kGLXNone };
              if (create) {
                // "if the implementation does not support the requested profile, then
                // GLXBadProfileARB is generated" (GLX_ARB_create_context): an X error
                _context = create(dpy, config, nullptr, 1, profile);
                if (trap.failed()) {
                  if (_context) {
                    _glx.DestroyContext(dpy, _context);
                    _context = nullptr;
                  }
                  trap.clear();
                }
              }
            }
            if (!_context) {
              _context = _glx.CreateNewContext(dpy, config, kGLXRGBAType, nullptr, 1);
              if (trap.failed() && _context) {
                _glx.DestroyContext(dpy, _context);
                _context = nullptr;
              }
            }
            if (!_context) {
              error = "glXCreateNewContext failed";
              return false;
            }
            // (BadAlloc if the server can't make it)
            const int size[] = { kGLXPbufferWidth, 1, kGLXPbufferHeight, 1, kGLXNone };
            _pbuffer = _glx.CreatePbuffer(dpy, config, size);
            if (trap.failed()) {
              error = "glXCreatePbuffer failed (X error)";
              return false;
            }
            if (!_pbuffer) {
              error = "glXCreatePbuffer failed";
              return false;
            }
            return true;
          }

          GLXLibrary _glx;
          std::shared_ptr<void> _display; ///< the context's own X connection
          GLXContext _context = nullptr;
          GLXDrawable _pbuffer = 0;
        };

        std::unique_ptr<Context> glxCreateContext(const Hooks &hooks, std::string &error)
        {
          GLXLibrary glx;
          bool ok = false;
          driverCall(hooks, [&] { ok = glx.load(error); });
          if (!ok) {
            return nullptr;
          }
          std::unique_ptr<GLXBackendContext> context(new GLXBackendContext(hooks, glx));
          if (!context->create(error)) {
            return nullptr;
          }
          return context;
        }

      } // Detail
    } // OpenGL
  } // Host
} // OFX

#endif
