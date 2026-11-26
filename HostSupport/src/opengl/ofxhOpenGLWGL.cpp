// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// The WGL backend of ofxhOpenGLContext.h (Windows): contexts made with WGL_ARB_create_context
// (compatibility profile) when the driver has it, else wglCreateContext, and made current on a
// hidden window of the calling thread. The contexts render to textures (ofxhOpenGLTexture.h),
// never to the window. The EGL backends are in ofxhOpenGLEGL.cpp, the CGL backend in
// ofxhOpenGLCGL.cpp.

#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLFunctions.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX // no min/max macros: they break std::min and std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>

namespace OFX {
  namespace Host {
    namespace OpenGL {
      namespace Detail {

        // WGL_ARB_create_context and WGL_ARB_create_context_profile (wglext.h)
        static const int kWGLContextProfileMaskARB = 0x9126;
        static const int kWGLContextCompatibilityProfileBitARB = 0x00000002;
        typedef HGLRC (WINAPI *CreateContextAttribsARBFn)(HDC, HGLRC, const int *);
        // WGL_ARB_extensions_string, WGL_EXT_extensions_string
        typedef const char *(WINAPI *GetExtensionsStringARBFn)(HDC);
        typedef const char *(WINAPI *GetExtensionsStringEXTFn)();

        /// the class of the hidden windows: registered by the first context of the process (with
        /// the executable's instance) and never unregistered. Its window procedure is
        /// DefWindowProcW, so it refers to no code of this library (which may be in a DLL that
        /// is unloaded). CS_OWNDC: each window has its own device context, which keeps the pixel
        /// format.
        static const wchar_t kWindowClass[] = L"OpenFXHostSupportWGL";

        static std::string lastError(const char *what)
        {
          return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
        }

        /// wglGetProcAddress(name) for the current context; nullptr if not found (some drivers
        /// return 1, 2, 3 or -1 instead of NULL)
        static PROC wglProc(const char *name)
        {
          PROC p = wglGetProcAddress(name);
          const std::intptr_t v = reinterpret_cast<std::intptr_t>(p);
          return v >= -1 && v <= 3 ? nullptr : p;
        }

        /// a WGL extension function of the current context, nullptr if not found (cast through
        /// void (*)(), the generic function pointer type of -Wcast-function-type)
        template <typename F>
        static F wglFunction(const char *name)
        {
          return reinterpret_cast<F>(reinterpret_cast<void (*)()>(wglProc(name)));
        }

        /// The hidden windows of a thread, one per pixel format. A context is made current with
        /// the device context of the calling thread's window of its pixel format (wglMakeCurrent:
        /// "It need not be the same hdc that was passed to wglCreateContext when hglrc was created,
        /// but it must be on the same device and have the same pixel format."). A window lives as
        /// long as its thread, which can't end while it has a context current: a context never
        /// works on a destroyed window, and each window is destroyed by its own thread.
        class ThreadWindows {
        public:
          ThreadWindows() = default;
          ThreadWindows(const ThreadWindows &) = delete;
          ThreadWindows &operator=(const ThreadWindows &) = delete;
          ~ThreadWindows()
          {
            for (const Window &w : _windows) {
              DestroyWindow(w.window);
            }
          }

          /// the device context of this thread's window of pixelFormat (made if needed); nullptr
          /// and error if that fails
          HDC dc(int pixelFormat, const PIXELFORMATDESCRIPTOR &pfd, std::string &error)
          {
            for (const Window &w : _windows) {
              if (w.pixelFormat == pixelFormat) {
                return w.dc;
              }
            }
            Window w;
            if (!create(w, error)) {
              return nullptr;
            }
            if (!SetPixelFormat(w.dc, pixelFormat, &pfd)) {
              error = lastError("SetPixelFormat");
              DestroyWindow(w.window);
              return nullptr;
            }
            w.pixelFormat = pixelFormat;
            _windows.push_back(w);
            return w.dc;
          }

          /// the pixel format of the contexts, chosen on a temporary window of this thread
          static bool choosePixelFormat(int &pixelFormat, PIXELFORMATDESCRIPTOR &pfd, std::string &error)
          {
            Window w;
            if (!create(w, error)) {
              return false;
            }
            // double-buffered 32-bit RGBA only so that ChoosePixelFormat returns an accelerated
            // format (drivers may have none that is single-buffered or shallower): the contexts
            // render to framebuffer objects, never to the window's buffers
            PIXELFORMATDESCRIPTOR want = {};
            want.nSize = sizeof(want);
            want.nVersion = 1;
            want.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            want.iPixelType = PFD_TYPE_RGBA;
            want.cColorBits = 32;
            want.cAlphaBits = 8;
            want.iLayerType = PFD_MAIN_PLANE;
            pixelFormat = ChoosePixelFormat(w.dc, &want);
            if (!pixelFormat) {
              error = lastError("ChoosePixelFormat");
            } else if (!DescribePixelFormat(w.dc, pixelFormat, sizeof(pfd), &pfd)) {
              error = lastError("DescribePixelFormat");
              pixelFormat = 0;
            }
            DestroyWindow(w.window);
            return pixelFormat != 0;
          }

        private:
          struct Window {
            int pixelFormat = 0;
            HWND window = nullptr;
            HDC dc = nullptr;
          };

          /// a new hidden window of this thread and its device context, with no pixel format yet
          static bool create(Window &w, std::string &error)
          {
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = kWindowClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
              error = lastError("RegisterClassExW");
              return false;
            }
            // a message-only window (HWND_MESSAGE): never shown, and it gets no broadcast
            // messages, so the threads that use contexts need no message loop
            w.window = CreateWindowExW(0, kWindowClass, L"OpenFX HostSupport OpenGL",
                                       WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                       HWND_MESSAGE, nullptr, instance, nullptr);
            if (!w.window) {
              error = lastError("CreateWindowExW");
              return false;
            }
            w.dc = GetDC(w.window);
            if (!w.dc) {
              error = lastError("GetDC");
              DestroyWindow(w.window);
              return false;
            }
            return true;
          }

          std::vector<Window> _windows;
        };

        /// the hidden windows of the calling thread, destroyed when it ends (like the context each
        /// thread holds, ofxhOpenGLContext.cpp, this is per-thread state)
        static ThreadWindows &threadWindows()
        {
          static thread_local ThreadWindows windows;
          return windows;
        }

        class WGLBackendContext : public Context {
        public:
          explicit WGLBackendContext(const Hooks &hooks)
            : Context(hooks)
            , _opengl32(GetModuleHandleW(L"opengl32.dll")) {} // linked: loaded with the program

          ~WGLBackendContext() override
          {
            driverCall([&] {
              if (_context) {
                wglDeleteContext(_context);
              }
            });
          }

          bool create(std::string &error)
          {
            bool ok = false;
            HDC dc = nullptr;
            driverCall([&] {
              ok = ThreadWindows::choosePixelFormat(_pixelFormat, _pfd, error);
              if (ok) {
                dc = threadWindows().dc(_pixelFormat, _pfd, error);
                ok = dc != nullptr;
              }
            });
            if (!ok) {
              return false;
            }
            // the thread's current context (the host's, or one it holds), restored after
            HDC previousDC = nullptr;
            HGLRC previous = nullptr;
            driverCall([&] {
              previousDC = wglGetCurrentDC();
              previous = wglGetCurrentContext();
              ok = createGLContext(dc, error);
            });
            if (ok) {
              const bool software = (_pfd.dwFlags & PFD_GENERIC_FORMAT) && !(_pfd.dwFlags & PFD_GENERIC_ACCELERATED);
              ok = initialize(Backend::WGL, -1, software, &error);
            }
            driverCall([&] {
              if (previous) {
                wglMakeCurrent(previousDC, previous);
              } else {
                wglMakeCurrent(nullptr, nullptr);
              }
            });
            return ok;
          }

          void *nativeContext() const override { return _context; }

        protected:
          bool makeCurrentImpl() override
          {
            // the calling thread's window of the context's pixel format
            std::string error;
            HDC dc = threadWindows().dc(_pixelFormat, _pfd, error);
            if (!dc) {
              if (hooks().log) {
                hooks().log("OpenGL: wgl: no window to make the context current: " + error);
              }
              return false;
            }
            return wglMakeCurrent(dc, _context) != FALSE;
          }

          void doneCurrentImpl() override { wglMakeCurrent(nullptr, nullptr); }

          ProcAddress backendProcAddress(const char *name) const override
          {
            // wglGetProcAddress gives the extensions and the functions after OpenGL 1.1, for the
            // current context; the OpenGL 1.1 functions are exported by opengl32.dll
            PROC p = wglProc(name);
            if (!p && _opengl32) {
              p = GetProcAddress(_opengl32, name);
            }
            return reinterpret_cast<ProcAddress>(p);
          }

        private:
          /// the WGL extensions (WGL_ARB_extensions_string, else WGL_EXT_extensions_string), with a
          /// context current on dc
          static std::string extensions(HDC dc)
          {
            if (auto arb = wglFunction<GetExtensionsStringARBFn>("wglGetExtensionsStringARB")) {
              const char *s = arb(dc);
              return s ? s : "";
            }
            if (auto ext = wglFunction<GetExtensionsStringEXTFn>("wglGetExtensionsStringEXT")) {
              const char *s = ext();
              return s ? s : "";
            }
            return std::string();
          }

          /// creates _context and leaves it current on dc. wglCreateContextAttribsARB needs a
          /// current context to be found: a legacy context first, replaced by one with the
          /// compatibility profile if the driver has WGL_ARB_create_context (OFX plug-ins may use
          /// the fixed-function pipeline, which the core profile doesn't have).
          bool createGLContext(HDC dc, std::string &error)
          {
            HGLRC legacy = wglCreateContext(dc);
            if (!legacy) {
              error = lastError("wglCreateContext");
              return false;
            }
            if (!wglMakeCurrent(dc, legacy)) {
              error = lastError("wglMakeCurrent");
              wglDeleteContext(legacy);
              return false;
            }
            _context = legacy;
            const std::string ext = extensions(dc);
            auto createAttribs = wglFunction<CreateContextAttribsARBFn>("wglCreateContextAttribsARB");
            if (!createAttribs || !hasExtension(ext.c_str(), "WGL_ARB_create_context")) {
              return true;
            }
            // no version attribute: "implementations will typically return the most recent
            // version of OpenGL they support which is backwards compatible with OpenGL 1.0"
            // (WGL_ARB_create_context); the profile attribute needs WGL_ARB_create_context_profile
            const int compatibility[] = { kWGLContextProfileMaskARB, kWGLContextCompatibilityProfileBitARB, 0 };
            const int none[] = { 0 };
            const bool profile = hasExtension(ext.c_str(), "WGL_ARB_create_context_profile");
            HGLRC arb = createAttribs(dc, nullptr, profile ? compatibility : none);
            if (arb && wglMakeCurrent(dc, arb)) {
              wglDeleteContext(legacy);
              _context = arb;
              return true;
            }
            // keep the legacy context (a failed wglMakeCurrent left none current)
            if (arb) {
              wglDeleteContext(arb);
            }
            if (!wglMakeCurrent(dc, legacy)) {
              error = lastError("wglMakeCurrent");
              return false;
            }
            return true;
          }

          HMODULE _opengl32;
          int _pixelFormat = 0;
          PIXELFORMATDESCRIPTOR _pfd = {};
          HGLRC _context = nullptr;
        };

        std::vector<int> rendererIds(const Hooks &, Backend backend)
        {
          // WGL has one renderer, the driver's (the GPU choice is the system's)
          if (backend == Backend::WGL) {
            return { -1 };
          }
          return {};
        }

        std::unique_ptr<Context> createBackendContext(const Hooks &hooks, Backend backend, int rendererId,
                                                      std::string &error)
        {
          if (backend != Backend::WGL) {
            error = "not available on this platform";
            return nullptr;
          }
          if (rendererId != -1) {
            error = "no renderer " + std::to_string(rendererId) + " (WGL has only the default renderer, -1)";
            return nullptr;
          }
          std::unique_ptr<WGLBackendContext> context(new WGLBackendContext(hooks));
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
