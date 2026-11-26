// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// The EGL backends of ofxhOpenGLContext.h (Linux and BSD): EGL_MESA_platform_surfaceless and
// EGL_EXT_platform_device. libEGL is loaded at run time, so the library needs neither EGL headers
// nor libEGL to build, and runs without them (then these backends fail and the next one is tried).
// The macOS backend is ofxhOpenGLCGL.cpp, the Windows one ofxhOpenGLWGL.cpp.

#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLFunctions.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <dlfcn.h>

namespace OFX {
  namespace Host {
    namespace OpenGL {
      namespace Detail {

        // the EGL 1.4 types, enums and functions used here (EGL/egl.h, EGL/eglext.h)
        typedef void *EGLDisplay;
        typedef void *EGLConfig;
        typedef void *EGLContext;
        typedef void *EGLDeviceEXT;
        typedef int EGLint;
        typedef unsigned int EGLBoolean;
        typedef unsigned int EGLenum;
        static const EGLint kEGLNone = 0x3038;
        static const EGLint kEGLSurfaceType = 0x3033;
        static const EGLint kEGLRenderableType = 0x3040;
        static const EGLint kEGLOpenGLBit = 0x0008;
        static const EGLint kEGLExtensions = 0x3055;
        static const EGLenum kEGLOpenGLAPI = 0x30A2;
        static const EGLenum kEGLPlatformDeviceEXT = 0x313F;
        static const EGLenum kEGLPlatformSurfacelessMESA = 0x31DD;
        static const EGLint kEGLTrackReferencesKHR = 0x3352;
        static const EGLint kEGLDraw = 0x3059;
        static const EGLint kEGLRead = 0x305A;

        /// libEGL and its functions; the library stays loaded while an EGLLibrary refers to it
        struct EGLLibrary {
          std::shared_ptr<void> handle;
          ProcAddress (*GetProcAddress)(const char *) = nullptr;
          const char *(*QueryString)(EGLDisplay, EGLint) = nullptr;
          EGLint (*GetError)() = nullptr;
          EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *) = nullptr;
          EGLBoolean (*Terminate)(EGLDisplay) = nullptr;
          EGLBoolean (*BindAPI)(EGLenum) = nullptr;
          EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
          EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
          EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext) = nullptr;
          EGLBoolean (*MakeCurrent)(EGLDisplay, void *, void *, EGLContext) = nullptr;
          EGLContext (*GetCurrentContext)() = nullptr;
          EGLDisplay (*GetCurrentDisplay)() = nullptr;
          void *(*GetCurrentSurface)(EGLint) = nullptr;
          EGLenum (*QueryAPI)() = nullptr;
          // extensions (EGL_EXT_platform_base, EGL_EXT_device_enumeration, EGL_EXT_device_query)
          EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = nullptr;
          EGLBoolean (*QueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *) = nullptr;
          const char *(*QueryDeviceStringEXT)(EGLDeviceEXT, EGLint) = nullptr;
          std::string clientExtensions;

          bool hasClientExtension(const char *name) const { return hasExtension(clientExtensions.c_str(), name); }

          bool load(std::string &error)
          {
            // never unloaded (RTLD_NODELETE): drivers (GLVND vendor libraries, llvmpipe threads)
            // don't survive an unload, and a display without EGL_KHR_display_reference stays
            // initialized
            void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            if (!h) {
              h = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            }
            if (!h) {
              error = "libEGL not found";
              return false;
            }
            handle.reset(h, [](void *p) { dlclose(p); });
#define OFXH_EGL(f) f = reinterpret_cast<decltype(f)>(dlsym(h, "egl" #f)); if (!f) { error = "egl" #f " not found"; return false; }
            OFXH_EGL(GetProcAddress)
            OFXH_EGL(QueryString)
            OFXH_EGL(GetError)
            OFXH_EGL(Initialize)
            OFXH_EGL(Terminate)
            OFXH_EGL(BindAPI)
            OFXH_EGL(ChooseConfig)
            OFXH_EGL(CreateContext)
            OFXH_EGL(DestroyContext)
            OFXH_EGL(MakeCurrent)
            OFXH_EGL(GetCurrentContext)
            OFXH_EGL(GetCurrentDisplay)
            OFXH_EGL(GetCurrentSurface)
            OFXH_EGL(QueryAPI)
#undef OFXH_EGL
            // the client extensions (EGL_EXT_client_extensions); NULL without it
            const char *ext = QueryString(nullptr, kEGLExtensions);
            clientExtensions = ext ? ext : "";
            if (!hasClientExtension("EGL_EXT_platform_base")) {
              error = "EGL_EXT_platform_base not supported";
              return false;
            }
            GetPlatformDisplayEXT = reinterpret_cast<decltype(GetPlatformDisplayEXT)>(GetProcAddress("eglGetPlatformDisplayEXT"));
            // "a non-NULL return value does not guarantee that an extension function is actually
            // supported" (eglGetProcAddress, EGL 1.5): only with their extensions
            const bool deviceBase = hasClientExtension("EGL_EXT_device_base");
            if (deviceBase || hasClientExtension("EGL_EXT_device_enumeration")) {
              QueryDevicesEXT = reinterpret_cast<decltype(QueryDevicesEXT)>(GetProcAddress("eglQueryDevicesEXT"));
            }
            if (deviceBase || hasClientExtension("EGL_EXT_device_query")) {
              QueryDeviceStringEXT = reinterpret_cast<decltype(QueryDeviceStringEXT)>(GetProcAddress("eglQueryDeviceStringEXT"));
            }
            if (!GetPlatformDisplayEXT) {
              error = "eglGetPlatformDisplayEXT not found";
              return false;
            }
            return true;
          }

          /// the EGL devices (EGL_EXT_device_enumeration)
          std::vector<EGLDeviceEXT> devices() const
          {
            EGLint n = 0;
            if (!QueryDevicesEXT || !hasClientExtension("EGL_EXT_platform_device") ||
                !QueryDevicesEXT(0, nullptr, &n) || n <= 0) {
              return {};
            }
            std::vector<EGLDeviceEXT> d(n);
            if (!QueryDevicesEXT(n, d.data(), &n)) {
              return {};
            }
            d.resize(n);
            return d;
          }
        };

        class EGLBackendContext : public Context {
        public:
          EGLBackendContext(const Hooks &hooks, const EGLLibrary &egl)
            : Context(hooks), _egl(egl) {}

          ~EGLBackendContext() override
          {
            driverCall([&] {
              if (_context) {
                _egl.DestroyContext(_display, _context);
              }
              // with EGL_KHR_display_reference, each context has its own reference on the display;
              // without it the display is shared by the whole process: leave it initialized
              if (_display && _tracked) {
                _egl.Terminate(_display);
              }
            });
          }

          bool create(Backend backend, int rendererId, std::string &error)
          {
            void *native = nullptr;
            EGLenum platform = kEGLPlatformSurfacelessMESA;
            bool software = false;
            if (backend == Backend::EGLSurfaceless) {
              if (!_egl.hasClientExtension("EGL_MESA_platform_surfaceless")) {
                error = "EGL_MESA_platform_surfaceless not supported";
                return false;
              }
            } else {
              const std::vector<EGLDeviceEXT> devices = _egl.devices();
              const size_t index = rendererId < 0 ? 0 : static_cast<size_t>(rendererId);
              if (index >= devices.size()) {
                error = devices.empty() ? "no EGL device (EGL_EXT_platform_device)" : "no EGL device " + std::to_string(rendererId);
                return false;
              }
              native = devices[index];
              platform = kEGLPlatformDeviceEXT;
              const char *ext = _egl.QueryDeviceStringEXT ? _egl.QueryDeviceStringEXT(native, kEGLExtensions) : nullptr;
              software = hasExtension(ext, "EGL_MESA_device_software");
            }
            bool ok = false;
            driverCall([&] { ok = createOn(platform, native, error); });
            if (!ok) {
              return false;
            }
            // the thread's current context (the host's, or one it holds), restored after
            EGLDisplay previousDisplay = nullptr;
            EGLContext previous = nullptr;
            void *draw = nullptr, *read = nullptr;
            EGLenum api = kEGLOpenGLAPI;
            bool current = false;
            driverCall([&] {
              api = _egl.QueryAPI();
              previous = _egl.GetCurrentContext();
              previousDisplay = _egl.GetCurrentDisplay();
              draw = _egl.GetCurrentSurface(kEGLDraw);
              read = _egl.GetCurrentSurface(kEGLRead);
              current = makeCurrentImpl();
            });
            if (current) {
              ok = initialize(backend, rendererId, software, &error);
            } else {
              error = "eglMakeCurrent failed";
              ok = false;
            }
            driverCall([&] {
              if (previous) {
                _egl.BindAPI(api);
                _egl.MakeCurrent(previousDisplay, draw, read, previous);
              } else {
                doneCurrentImpl();
                _egl.BindAPI(api);
              }
            });
            return ok;
          }

          void *nativeContext() const override { return _context; }

        protected:
          bool makeCurrentImpl() override
          {
            // the client API is per thread (eglBindAPI): desktop OpenGL on this one too
            return _egl.BindAPI(kEGLOpenGLAPI) && _egl.MakeCurrent(_display, nullptr, nullptr, _context) != 0;
          }
          void doneCurrentImpl() override { _egl.MakeCurrent(_display, nullptr, nullptr, nullptr); }

          ProcAddress backendProcAddress(const char *name) const override
          {
            // eglGetProcAddress also returns the core functions with
            // EGL_KHR_client_get_all_proc_addresses (EGL 1.5, Mesa); else libOpenGL (GLVND) or libGL
            ProcAddress p = _egl.GetProcAddress(name);
            if (p) {
              return p;
            }
            for (const char *lib : { "libOpenGL.so.0", "libGL.so.1" }) {
              if (void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD)) {
                p = reinterpret_cast<ProcAddress>(dlsym(h, name));
                dlclose(h);
                if (p) {
                  return p;
                }
              }
            }
            return nullptr;
          }

        private:
          bool createOn(EGLenum platform, void *native, std::string &error)
          {
            const EGLint track[] = { kEGLTrackReferencesKHR, 1, kEGLNone };
            _tracked = _egl.hasClientExtension("EGL_KHR_display_reference");
            _display = _egl.GetPlatformDisplayEXT(platform, native, _tracked ? track : nullptr);
            if (!_display || !_egl.Initialize(_display, nullptr, nullptr)) {
              error = "eglInitialize failed (EGL error " + std::to_string(_egl.GetError()) + ")";
              _display = nullptr;
              return false;
            }
            const char *ext = _egl.QueryString(_display, kEGLExtensions);
            if (!hasExtension(ext, "EGL_KHR_surfaceless_context")) {
              error = "EGL_KHR_surfaceless_context not supported";
              return false;
            }
            // desktop OpenGL; a surface type of 0: any config, as there is no surface
            const EGLint attribs[] = { kEGLRenderableType, kEGLOpenGLBit, kEGLSurfaceType, 0, kEGLNone };
            EGLConfig config = nullptr;
            EGLint n = 0;
            if (!_egl.BindAPI(kEGLOpenGLAPI) || !_egl.ChooseConfig(_display, attribs, &config, 1, &n) || n < 1) {
              error = "no desktop OpenGL EGL config";
              return false;
            }
            // no version or profile attribute: the compatibility profile of the highest version
            _context = _egl.CreateContext(_display, config, nullptr, nullptr);
            if (!_context) {
              error = "eglCreateContext failed (EGL error " + std::to_string(_egl.GetError()) + ")";
              return false;
            }
            return true;
          }

          EGLLibrary _egl;
          EGLDisplay _display = nullptr;
          EGLContext _context = nullptr;
          bool _tracked = false;
        };

        static bool loadEGL(const Hooks &hooks, EGLLibrary &egl, std::string &error)
        {
          bool ok = false;
          driverCall(hooks, [&] { ok = egl.load(error); });
          return ok;
        }

        std::vector<int> rendererIds(const Hooks &hooks, Backend backend)
        {
          if (backend == Backend::EGLSurfaceless) {
            return { -1 };
          }
          std::vector<int> ids;
          EGLLibrary egl;
          std::string error;
          if (backend == Backend::EGLDevice && loadEGL(hooks, egl, error)) {
            for (int i = 0; i < static_cast<int>(egl.devices().size()); ++i) {
              ids.push_back(i);
            }
          }
          return ids;
        }

        std::unique_ptr<Context> createBackendContext(const Hooks &hooks, Backend backend, int rendererId,
                                                      std::string &error)
        {
          if (backend != Backend::EGLSurfaceless && backend != Backend::EGLDevice) {
            error = "not available on this platform";
            return nullptr;
          }
          EGLLibrary egl;
          if (!loadEGL(hooks, egl, error)) {
            return nullptr;
          }
          std::unique_ptr<EGLBackendContext> context(new EGLBackendContext(hooks, egl));
          if (!context->create(backend, rendererId, error)) {
            return nullptr;
          }
          return context;
        }

      } // Detail
    } // OpenGL
  } // Host
} // OFX

#endif
