// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// The CGL backend of ofxhOpenGLContext.h (macOS). The EGL backends are in ofxhOpenGLEGL.cpp, the
// WGL backend in ofxhOpenGLWGL.cpp.

#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLFunctions.h"

#if defined(__APPLE__)

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION // OpenGL is deprecated on macOS, but still there
#endif
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>

#include <cstdint>

namespace OFX {
  namespace Host {
    namespace OpenGL {
      namespace Detail {

        class CGLBackendContext : public Context {
        public:
          explicit CGLBackendContext(const Hooks &hooks)
            : Context(hooks)
            , _framework(dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL)) {}

          ~CGLBackendContext() override
          {
            driverCall([&] {
              if (_context) {
                CGLDestroyContext(_context);
              }
            });
            if (_framework) {
              dlclose(_framework);
            }
          }

          bool create(int rendererId, std::string &error)
          {
            bool software = false;
            CGLError err = kCGLNoError;
            driverCall([&] {
              // the legacy profile (OpenGL 2.1 and extensions): OFX plug-ins may use the
              // fixed-function pipeline, which the core profiles don't have
              CGLPixelFormatAttribute attribs[] = {
                kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
                kCGLPFAAllowOfflineRenderers,
                kCGLPFARendererID, (CGLPixelFormatAttribute)rendererId,
                (CGLPixelFormatAttribute)0
              };
              if (rendererId < 0) {
                attribs[3] = (CGLPixelFormatAttribute)0; // the default renderer
              }
              CGLPixelFormatObj pix = nullptr;
              GLint npix = 0;
              err = CGLChoosePixelFormat(attribs, &pix, &npix);
              if (err != kCGLNoError || !pix) {
                err = err != kCGLNoError ? err : kCGLBadPixelFormat;
                return;
              }
              GLint accelerated = 1;
              CGLDescribePixelFormat(pix, 0, kCGLPFAAccelerated, &accelerated);
              software = !accelerated;
              err = CGLCreateContext(pix, nullptr, &_context);
              CGLReleasePixelFormat(pix);
            });
            if (err != kCGLNoError || !_context) {
              error = std::string("CGL: ") + CGLErrorString(err);
              return false;
            }
            // the thread's current context (the host's, or one it holds), restored after
            CGLContextObj previous = nullptr;
            bool current = false;
            driverCall([&] {
              previous = CGLGetCurrentContext();
              current = makeCurrentImpl();
            });
            bool ok = false;
            if (current) {
              ok = initialize(Backend::CGL, rendererId, software, &error);
              if (ok) {
                driverCall([&] { backendInfo().videoMemoryBytes = videoMemory(); });
              }
            } else {
              error = "CGLSetCurrentContext failed";
            }
            driverCall([&] { CGLSetCurrentContext(previous); });
            return ok;
          }

          void *nativeContext() const override { return _context; }

          /// the video memory of the context's renderer (kCGLRPVideoMemoryMegabytes), 0 if unknown
          std::int64_t videoMemory() const
          {
            GLint id = 0, n = 0;
            if (CGLGetParameter(_context, kCGLCPCurrentRendererID, &id) != kCGLNoError) {
              return 0;
            }
            CGLRendererInfoObj info = nullptr;
            if (CGLQueryRendererInfo(0xFFFFFFFF, &info, &n) != kCGLNoError || !info) {
              return 0;
            }
            std::int64_t bytes = 0;
            for (GLint i = 0; i < n; ++i) {
              GLint rid = 0, mb = 0;
              if (CGLDescribeRenderer(info, i, kCGLRPRendererID, &rid) == kCGLNoError &&
                  (rid & kCGLRendererIDMatchingMask) == (id & kCGLRendererIDMatchingMask) &&
                  CGLDescribeRenderer(info, i, kCGLRPVideoMemoryMegabytes, &mb) == kCGLNoError) {
                bytes = std::int64_t(1024) * 1024 * mb;
                break;
              }
            }
            CGLDestroyRendererInfo(info);
            return bytes;
          }

        protected:
          bool makeCurrentImpl() override { return CGLSetCurrentContext(_context) == kCGLNoError; }
          void doneCurrentImpl() override { CGLSetCurrentContext(nullptr); }

          ProcAddress backendProcAddress(const char *name) const override
          {
            return _framework ? reinterpret_cast<ProcAddress>(dlsym(_framework, name)) : nullptr;
          }

        private:
          void *_framework;
          CGLContextObj _context = nullptr;
        };

        std::vector<int> rendererIds(const Hooks &hooks, Backend backend)
        {
          std::vector<int> ids;
          if (backend != Backend::CGL) {
            return ids;
          }
          auto query = [&] {
            CGLRendererInfoObj info = nullptr;
            GLint n = 0;
            if (CGLQueryRendererInfo(0xFFFFFFFF, &info, &n) != kCGLNoError || !info) {
              return;
            }
            for (GLint i = 0; i < n; ++i) {
              GLint id = 0;
              if (CGLDescribeRenderer(info, i, kCGLRPRendererID, &id) == kCGLNoError) {
                ids.push_back(id);
              }
            }
            CGLDestroyRendererInfo(info);
          };
          driverCall(hooks, query);
          return ids;
        }

        std::unique_ptr<Context> createBackendContext(const Hooks &hooks, Backend backend, int rendererId,
                                                      std::string &error)
        {
          if (backend != Backend::CGL) {
            error = "not available on this platform";
            return nullptr;
          }
          std::unique_ptr<CGLBackendContext> context(new CGLBackendContext(hooks));
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
