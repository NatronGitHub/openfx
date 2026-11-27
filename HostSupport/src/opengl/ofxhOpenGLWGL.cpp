// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// The WGL backend of ofxhOpenGLContext.h (Windows): contexts made with WGL_ARB_create_context
// (compatibility profile) when the driver has it, else wglCreateContext, and made current on a
// hidden window of the calling thread. Where the driver has a GPU choice, the renderers are its
// GPUs, through WGL_NV_gpu_affinity (the GPU index, an affinity DC) or WGL_AMD_gpu_association
// (the GPU ID, an associated context); else the only renderer is the driver's default, -1 (which
// createContext() also accepts with a GPU choice). The contexts render to textures
// (ofxhOpenGLTexture.h), never to the window. The EGL backends are in ofxhOpenGLEGL.cpp, the CGL
// backend in ofxhOpenGLCGL.cpp.

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
        // WGL_NV_gpu_affinity (HGPUNV is a handle)
        typedef BOOL (WINAPI *EnumGpusNVFn)(UINT, void **);
        typedef HDC (WINAPI *CreateAffinityDCNVFn)(void *const *);
        typedef BOOL (WINAPI *DeleteDCNVFn)(HDC);
        // WGL_AMD_gpu_association
        typedef UINT (WINAPI *GetGPUIDsAMDFn)(UINT, UINT *);
        typedef HGLRC (WINAPI *CreateAssociatedContextAMDFn)(UINT);
        typedef HGLRC (WINAPI *CreateAssociatedContextAttribsAMDFn)(UINT, HGLRC, const int *);
        typedef BOOL (WINAPI *DeleteAssociatedContextAMDFn)(HGLRC);
        typedef BOOL (WINAPI *MakeAssociatedContextCurrentAMDFn)(HGLRC);
        typedef INT (WINAPI *GetGPUInfoAMDFn)(UINT, INT, GLenum, UINT, void *);
        static const INT kWGLGPURAMAMD = 0x21A3;        // WGL_GPU_RAM_AMD
        static const GLenum kGLUnsignedInt = 0x1405;    // GL_UNSIGNED_INT

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

        /// the WGL extensions (WGL_ARB_extensions_string, else WGL_EXT_extensions_string), with a
        /// context current on dc
        static std::string wglExtensions(HDC dc)
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

        /// The GPU choice of the driver, if it has one: WGL_NV_gpu_affinity or
        /// WGL_AMD_gpu_association, loaded with a context current (wglGetProcAddress)
        struct GPUChoice {
          EnumGpusNVFn enumGpus = nullptr;
          CreateAffinityDCNVFn createAffinityDC = nullptr;
          DeleteDCNVFn deleteDC = nullptr;
          GetGPUIDsAMDFn getGPUIDs = nullptr;
          CreateAssociatedContextAMDFn createAssociated = nullptr;
          CreateAssociatedContextAttribsAMDFn createAssociatedAttribs = nullptr;
          DeleteAssociatedContextAMDFn deleteAssociated = nullptr;
          MakeAssociatedContextCurrentAMDFn makeAssociatedCurrent = nullptr;
          GetGPUInfoAMDFn getGPUInfo = nullptr;

          void load(const std::string &ext)
          {
            if (hasExtension(ext.c_str(), "WGL_NV_gpu_affinity")) {
              enumGpus = wglFunction<EnumGpusNVFn>("wglEnumGpusNV");
              createAffinityDC = wglFunction<CreateAffinityDCNVFn>("wglCreateAffinityDCNV");
              deleteDC = wglFunction<DeleteDCNVFn>("wglDeleteDCNV");
            }
            // 64-bit only: AMD's 32-bit drivers may misread the buffer sizes of wglGetGPUInfoAMD.
            // Unconfirmed lead: its only source is a comment in Natron's Engine/OSGLContext_win.cpp
            // (RB-2.6, GetGPUInfoAMDInternal_int: "they read a wrong buffer size").
#if defined(_WIN64)
            if (hasExtension(ext.c_str(), "WGL_AMD_gpu_association")) {
              getGPUIDs = wglFunction<GetGPUIDsAMDFn>("wglGetGPUIDsAMD");
              createAssociated = wglFunction<CreateAssociatedContextAMDFn>("wglCreateAssociatedContextAMD");
              createAssociatedAttribs =
                wglFunction<CreateAssociatedContextAttribsAMDFn>("wglCreateAssociatedContextAttribsAMD");
              deleteAssociated = wglFunction<DeleteAssociatedContextAMDFn>("wglDeleteAssociatedContextAMD");
              makeAssociatedCurrent = wglFunction<MakeAssociatedContextCurrentAMDFn>("wglMakeAssociatedContextCurrentAMD");
              getGPUInfo = wglFunction<GetGPUInfoAMDFn>("wglGetGPUInfoAMD");
            }
#endif
          }
          bool nv() const { return enumGpus && createAffinityDC && deleteDC; }
          bool amd() const { return getGPUIDs && createAssociated && deleteAssociated && makeAssociatedCurrent; }

          /// the renderer IDs of the GPUs: NVIDIA's GPU indices ("The function fails if <iGpuIndex>
          /// is greater or equal than the number of GPUs supported by the system",
          /// WGL_NV_gpu_affinity), else AMD's GPU IDs ("If the array <ids> is NULL, the function
          /// will only return the total number of GPUs", WGL_AMD_gpu_association)
          std::vector<int> ids() const
          {
            std::vector<int> result;
            if (nv()) {
              void *gpu = nullptr;
              for (UINT i = 0; i < 64 && enumGpus(i, &gpu); ++i) {
                result.push_back(static_cast<int>(i));
              }
            } else if (amd()) {
              std::vector<UINT> gpus(getGPUIDs(0, nullptr));
              gpus.resize(gpus.empty() ? 0 : getGPUIDs(static_cast<UINT>(gpus.size()), gpus.data()));
              for (UINT id : gpus) {
                result.push_back(static_cast<int>(id));
              }
            }
            return result;
          }
        };

        /// The hidden windows of a thread, one per pixel format. A context is made current with
        /// the device context of the calling thread's window of its pixel format (wglMakeCurrent:
        /// "It need not be the same hdc that was passed to wglCreateContext when hglrc was created,
        /// but it must be on the same device and have the same pixel format."). A window lives as
        /// long as its thread, which must not end while it holds or reserves a context (see
        /// CurrentContext): a context never works on a destroyed window.
        class ThreadWindows {
        public:
          ThreadWindows() = default;
          ThreadWindows(const ThreadWindows &) = delete;
          ThreadWindows &operator=(const ThreadWindows &) = delete;
          /// Nothing: a thread_local destructor runs at the thread's end, possibly under the loader
          /// lock (thread-detach notifications), where destroying a window or calling the OpenGL
          /// driver may deadlock. The system destroys the windows of a thread that ends.
          ~ThreadWindows() = default;

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
            const bool ok = choosePixelFormatOn(w.dc, pixelFormat, pfd, error);
            DestroyWindow(w.window);
            return ok;
          }

          /// the pixel format of the contexts on dc (a window's, or an affinity DC: pixel format
          /// indices are per device)
          static bool choosePixelFormatOn(HDC dc, int &pixelFormat, PIXELFORMATDESCRIPTOR &pfd, std::string &error)
          {
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
            pixelFormat = ChoosePixelFormat(dc, &want);
            if (!pixelFormat) {
              error = lastError("ChoosePixelFormat");
            } else if (!DescribePixelFormat(dc, pixelFormat, sizeof(pfd), &pfd)) {
              error = lastError("DescribePixelFormat");
              pixelFormat = 0;
            }
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
              if (_context && _associated) {
                _gpu.deleteAssociated(_context);
              } else if (_context) {
                wglDeleteContext(_context);
              }
              if (_affinityDC) {
                _gpu.deleteDC(_affinityDC);
              }
            });
          }

          /// a context on rendererId: -1, the driver's default, else a GPU of GPUChoice::ids()
          bool create(int rendererId, std::string &error)
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
              if (ok && rendererId >= 0) {
                ok = moveToGPU(dc, rendererId, error);
              }
            });
            if (ok) {
              const bool software = !_associated && (_pfd.dwFlags & PFD_GENERIC_FORMAT) &&
                !(_pfd.dwFlags & PFD_GENERIC_ACCELERATED);
              ok = initialize(Backend::WGL, rendererId, software, &error);
            }
            if (ok && _associated && _gpu.getGPUInfo) {
              // "Querying WGL_GPU_RAM_AMD returns the amount of RAM available to GPU in MB"
              UINT mb = 0;
              driverCall([&] {
                if (_gpu.getGPUInfo(static_cast<UINT>(rendererId), kWGLGPURAMAMD, kGLUnsignedInt, 1, &mb) != 1) {
                  mb = 0;
                }
              });
              if (mb > 0) {
                backendInfo().videoMemoryBytes = std::int64_t(1024) * 1024 * mb;
              }
            }
            driverCall([&] {
              // ("If an associated context is current and wglMakeCurrent is called with a valid
              // context", it is detached: WGL_AMD_gpu_association)
              if (previous) {
                wglMakeCurrent(previousDC, previous);
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
            if (_associated) {
              return _gpu.makeAssociatedCurrent(_context) != FALSE;
            }
            if (_affinityDC) {
              // "An affinity-context can only be made current to an affinity-DC with the same
              // affinity-mask" (WGL_NV_gpu_affinity, which says nothing of threads)
              return wglMakeCurrent(_affinityDC, _context) != FALSE;
            }
            // the calling thread's window of the context's pixel format
            std::string error;
            HDC dc = threadWindows().dc(_pixelFormat, _pfd, error);
            if (!dc) {
              if (hooks().log) {
                hooks().log(LogLevel::Error, "OpenGL: wgl: no window to make the context current: " + error);
              }
              return false;
            }
            return wglMakeCurrent(dc, _context) != FALSE;
          }

          void doneCurrentImpl() override
          {
            if (_associated) {
              // "To detach the current associated context, pass NULL"
              _gpu.makeAssociatedCurrent(nullptr);
            } else {
              wglMakeCurrent(nullptr, nullptr);
            }
          }

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
          /// replaces _context, current on the window's dc, by a context on GPU id, current there:
          /// on an affinity DC ("Setting a pixelformat on an affinity-DC is required"; "An
          /// affinity-DC has no window associated with it, and therefore it has no default
          /// window-system-provided framebuffer": the contexts render to textures anyway), else an
          /// associated context ("An app created framebuffer object must be bound for rendering to
          /// be valid")
          bool moveToGPU(HDC dc, int id, std::string &error)
          {
            const std::string ext = wglExtensions(dc);
            _gpu.load(ext);
            const HGLRC windowContext = _context;
            if (_gpu.nv()) {
              void *gpu = nullptr;
              if (!_gpu.enumGpus(static_cast<UINT>(id), &gpu)) {
                error = "no GPU " + std::to_string(id) + " (WGL_NV_gpu_affinity)";
                return false;
              }
              void *const gpus[] = { gpu, nullptr };
              _affinityDC = _gpu.createAffinityDC(gpus);
              if (!_affinityDC) {
                error = lastError("wglCreateAffinityDCNV");
                return false;
              }
              // (the context's pixel format, and its software flags, are then the affinity DC's)
              if (!ThreadWindows::choosePixelFormatOn(_affinityDC, _pixelFormat, _pfd, error)) {
                return false;
              }
              if (!SetPixelFormat(_affinityDC, _pixelFormat, &_pfd)) {
                error = lastError("SetPixelFormat (affinity DC)");
                return false;
              }
              _context = nullptr;
              if (!createGLContext(_affinityDC, error)) {
                if (_context) {
                  wglDeleteContext(_context); // (a legacy context it couldn't make current)
                }
                _context = windowContext; // (deleted by the destructor)
                return false;
              }
            } else if (_gpu.amd()) {
              // "All capabilities and limitations of wglCreateContextAttribsARB apply to
              // wglCreateAssociatedContextAttribsAMD": the compatibility profile, as in
              // createGLContext()
              const int compatibility[] = { kWGLContextProfileMaskARB, kWGLContextCompatibilityProfileBitARB, 0 };
              HGLRC associated = nullptr;
              if (_gpu.createAssociatedAttribs && hasExtension(ext.c_str(), "WGL_ARB_create_context_profile")) {
                associated = _gpu.createAssociatedAttribs(static_cast<UINT>(id), nullptr, compatibility);
              }
              if (!associated) {
                associated = _gpu.createAssociated(static_cast<UINT>(id));
              }
              if (!associated) {
                error = "no GPU " + std::to_string(id) + " (WGL_AMD_gpu_association)";
                return false;
              }
              _context = associated;
              _associated = true;
              if (!_gpu.makeAssociatedCurrent(associated)) {
                error = lastError("wglMakeAssociatedContextCurrentAMD");
                wglDeleteContext(windowContext);
                return false;
              }
            } else {
              error = "no GPU choice (WGL_NV_gpu_affinity or WGL_AMD_gpu_association)";
              return false;
            }
            wglDeleteContext(windowContext); // (current nowhere now)
            return true;
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
            const std::string ext = wglExtensions(dc);
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
          GPUChoice _gpu;               ///< (after moveToGPU())
          HDC _affinityDC = nullptr;    ///< WGL_NV_gpu_affinity: the context's affinity DC
          bool _associated = false;     ///< WGL_AMD_gpu_association: an associated _context
        };

        /// the GPUs of GPUChoice::ids(), read with a temporary context on this thread's window
        static std::vector<int> gpuIds(const Hooks &hooks)
        {
          std::vector<int> ids;
          driverCall(hooks, [&] {
            int pixelFormat = 0;
            PIXELFORMATDESCRIPTOR pfd = {};
            std::string error;
            HDC dc = ThreadWindows::choosePixelFormat(pixelFormat, pfd, error)
              ? threadWindows().dc(pixelFormat, pfd, error) : nullptr;
            HGLRC context = dc ? wglCreateContext(dc) : nullptr;
            if (!context) {
              return;
            }
            HDC previousDC = wglGetCurrentDC();
            HGLRC previous = wglGetCurrentContext();
            if (wglMakeCurrent(dc, context)) {
              GPUChoice gpu;
              gpu.load(wglExtensions(dc));
              ids = gpu.ids();
            }
            if (previous) {
              wglMakeCurrent(previousDC, previous);
            } else {
              wglMakeCurrent(nullptr, nullptr);
            }
            wglDeleteContext(context);
          });
          return ids;
        }

        std::vector<int> rendererIds(const Hooks &hooks, Backend backend)
        {
          // its GPUs where the driver lets the host choose, else the driver's default renderer
          if (backend != Backend::WGL) {
            return {};
          }
          // (not both: -1 would be one of the GPUs again)
          std::vector<int> ids = gpuIds(hooks);
          if (ids.empty()) {
            ids.push_back(-1);
          }
          return ids;
        }

        std::unique_ptr<Context> createBackendContext(const Hooks &hooks, Backend backend, int rendererId,
                                                      std::string &error)
        {
          if (backend != Backend::WGL) {
            error = "not available on this platform";
            return nullptr;
          }
          if (rendererId < -1) {
            error = "no renderer " + std::to_string(rendererId);
            return nullptr;
          }
          std::unique_ptr<WGLBackendContext> context(new WGLBackendContext(hooks));
          if (!context->create(rendererId, error)) {
            return nullptr;
          }
          return context;
        }

      } // Detail
    } // OpenGL
  } // Host
} // OFX

#endif
