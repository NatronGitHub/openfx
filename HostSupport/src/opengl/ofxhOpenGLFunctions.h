#ifndef OFXH_OPENGL_FUNCTIONS_H
#define OFXH_OPENGL_FUNCTIONS_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// Private to openfx::HostSupportOpenGL: the OpenGL functions and enums it uses, loaded with
// Context::getProcAddress(), so that no OpenGL header or library is needed at build time.

#include "ofxhOpenGLContext.h"
#include "ofxCore.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace OFX {
  namespace Host {
    namespace OpenGL {
      namespace Detail {

        typedef unsigned int GLenum;
        typedef unsigned int GLuint;
        typedef unsigned int GLbitfield;
        typedef int GLint;
        typedef int GLsizei;
        typedef float GLfloat;
        typedef unsigned char GLubyte;
        typedef unsigned char GLboolean;

        static const GLenum kGLNoError = 0;
        static const GLenum kGLOutOfMemory = 0x0505;
        static const GLenum kGLTexture2D = 0x0DE1;
        static const GLenum kGLUnpackRowLength = 0x0CF2;
        static const GLenum kGLUnpackAlignment = 0x0CF5;
        static const GLenum kGLPackRowLength = 0x0D02;
        static const GLenum kGLPackAlignment = 0x0D05;
        static const GLenum kGLMaxTextureSize = 0x0D33;
        static const GLenum kGLTextureBinding2D = 0x8069;
        static const GLenum kGLUnsignedByte = 0x1401;
        static const GLenum kGLUnsignedShort = 0x1403;
        static const GLenum kGLFloat = 0x1406;
        static const GLenum kGLHalfFloat = 0x140B;
        static const GLenum kGLAlpha = 0x1906;
        static const GLenum kGLRGBA = 0x1908;
        static const GLenum kGLVendor = 0x1F00;
        static const GLenum kGLRenderer = 0x1F01;
        static const GLenum kGLVersion = 0x1F02;
        static const GLenum kGLNearest = 0x2600;
        static const GLenum kGLTextureMagFilter = 0x2800;
        static const GLenum kGLTextureMinFilter = 0x2801;
        static const GLenum kGLTextureWrapS = 0x2802;
        static const GLenum kGLTextureWrapT = 0x2803;
        static const GLenum kGLClampToEdge = 0x812F;
        static const GLenum kGLColorBufferBit = 0x4000;
        static const GLenum kGLRGBA8 = 0x8058;
        static const GLenum kGLRGBA16 = 0x805B;
        static const GLenum kGLRGBA32F = 0x8814;
        static const GLenum kGLRGBA16F = 0x881A;
        static const GLenum kGLFramebuffer = 0x8D40;
        static const GLenum kGLFramebufferBinding = 0x8CA6;
        static const GLenum kGLFramebufferComplete = 0x8CD5;
        static const GLenum kGLColorAttachment0 = 0x8CE0;
        static const GLenum kGLViewport = 0x0BA2;
        static const GLenum kGLScissorTest = 0x0C11;
        static const GLenum kGLColorClearValue = 0x0C22;
        static const GLenum kGLColorWritemask = 0x0C23;
        // the pixel-store state of the transfers (GL_UNPACK_*, GL_PACK_*) and the pixel buffer
        // bindings (OpenGL 2.1)
        static const GLenum kGLUnpackSwapBytes = 0x0CF0;
        static const GLenum kGLUnpackSkipRows = 0x0CF3;
        static const GLenum kGLUnpackSkipPixels = 0x0CF4;
        static const GLenum kGLPackSwapBytes = 0x0D00;
        static const GLenum kGLPackSkipRows = 0x0D03;
        static const GLenum kGLPackSkipPixels = 0x0D04;
        static const GLenum kGLPixelPackBuffer = 0x88EB;
        static const GLenum kGLPixelUnpackBuffer = 0x88EC;
        static const GLenum kGLPixelPackBufferBinding = 0x88ED;
        static const GLenum kGLPixelUnpackBufferBinding = 0x88EF;

        /// the OpenGL 1.1 functions and the framebuffer objects (OpenGL 3.0, or
        /// GL_ARB/EXT_framebuffer_object); optional: glBindBuffer (OpenGL 1.5)
        struct GLFunctions {
          const GLubyte *(OFXH_GLAPIENTRY *GetString)(GLenum) = nullptr;
          void (OFXH_GLAPIENTRY *GetIntegerv)(GLenum, GLint *) = nullptr;
          void (OFXH_GLAPIENTRY *GetFloatv)(GLenum, GLfloat *) = nullptr;
          void (OFXH_GLAPIENTRY *GetBooleanv)(GLenum, GLboolean *) = nullptr;
          GLboolean (OFXH_GLAPIENTRY *IsEnabled)(GLenum) = nullptr;
          void (OFXH_GLAPIENTRY *Enable)(GLenum) = nullptr;
          void (OFXH_GLAPIENTRY *Disable)(GLenum) = nullptr;
          void (OFXH_GLAPIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
          GLenum (OFXH_GLAPIENTRY *GetError)() = nullptr;
          void (OFXH_GLAPIENTRY *GenTextures)(GLsizei, GLuint *) = nullptr;
          void (OFXH_GLAPIENTRY *DeleteTextures)(GLsizei, const GLuint *) = nullptr;
          void (OFXH_GLAPIENTRY *BindTexture)(GLenum, GLuint) = nullptr;
          GLboolean (OFXH_GLAPIENTRY *IsTexture)(GLuint) = nullptr;
          void (OFXH_GLAPIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = nullptr;
          void (OFXH_GLAPIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) = nullptr;
          void (OFXH_GLAPIENTRY *TexParameteri)(GLenum, GLenum, GLint) = nullptr;
          void (OFXH_GLAPIENTRY *PixelStorei)(GLenum, GLint) = nullptr;
          void (OFXH_GLAPIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = nullptr;
          void (OFXH_GLAPIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
          void (OFXH_GLAPIENTRY *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
          void (OFXH_GLAPIENTRY *Clear)(GLbitfield) = nullptr;
          void (OFXH_GLAPIENTRY *Finish)() = nullptr;
          void (OFXH_GLAPIENTRY *GenFramebuffers)(GLsizei, GLuint *) = nullptr;
          void (OFXH_GLAPIENTRY *DeleteFramebuffers)(GLsizei, const GLuint *) = nullptr;
          void (OFXH_GLAPIENTRY *BindFramebuffer)(GLenum, GLuint) = nullptr;
          void (OFXH_GLAPIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
          GLenum (OFXH_GLAPIENTRY *CheckFramebufferStatus)(GLenum) = nullptr;
          void (OFXH_GLAPIENTRY *BindBuffer)(GLenum, GLuint) = nullptr; ///< optional
        };

        /// the intersection of a and b, empty (x1 == x2, y1 == y2) if they don't overlap
        inline OfxRectI intersect(const OfxRectI &a, const OfxRectI &b)
        {
          OfxRectI r = { std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2) };
          if (r.x2 <= r.x1 || r.y2 <= r.y1) {
            r.x2 = r.x1;
            r.y2 = r.y1;
          }
          return r;
        }

        /// name is in list, an extension string (space-separated names; list may be null)
        inline bool hasExtension(const char *list, const char *name)
        {
          const size_t n = std::strlen(name);
          for (const char *p = list; p && (p = std::strstr(p, name)) != nullptr; p += n) {
            if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) {
              return true;
            }
          }
          return false;
        }

        /// at most this many glGetError() calls to clear the errors (a lost context may report
        /// errors forever)
        static const int kMaxGLErrors = 16;

        /// clears the errors OpenGL recorded
        inline void clearGLErrors(const GLFunctions &gl)
        {
          for (int i = 0; i < kMaxGLErrors && gl.GetError() != kGLNoError; ++i) {
          }
        }

        /// runs call through hooks.driverCall, or directly if the host gave none
        template <typename F>
        void driverCall(const Hooks &hooks, F &&call)
        {
          if (hooks.driverCall) {
            hooks.driverCall(std::function<void()>(std::forward<F>(call)));
          } else {
            call();
          }
        }

        /// loads the functions of gl with context's loader; false (and *missing: the first
        /// missing function) if a required one is missing
        bool loadGLFunctions(const Context &context, GLFunctions &gl, std::string *missing);

        /// The backends (ofxhOpenGLEGL.cpp, ofxhOpenGLCGL.cpp, ofxhOpenGLWGL.cpp): the renderer IDs (RendererInfo::id)
        /// of backend, and a context on one of them; nullptr and *error if it fails
        std::vector<int> rendererIds(const Hooks &hooks, Backend backend);
        std::unique_ptr<Context> createBackendContext(const Hooks &hooks, Backend backend, int rendererId,
                                                      std::string &error);
#if !defined(__APPLE__) && !defined(_WIN32)
        /// Linux and BSD: the EGL backends (ofxhOpenGLEGL.cpp) and GLX (ofxhOpenGLGLX.cpp), which
        /// rendererIds() and createBackendContext() dispatch to (ofxhOpenGLContext.cpp)
        std::vector<int> eglRendererIds(const Hooks &hooks, Backend backend);
        std::unique_ptr<Context> eglCreateContext(const Hooks &hooks, Backend backend, int rendererId, std::string &error);
        std::unique_ptr<Context> glxCreateContext(const Hooks &hooks, std::string &error);
#endif

      } // Detail
    } // OpenGL
  } // Host
} // OFX

#endif // OFXH_OPENGL_FUNCTIONS_H
