// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhOpenGLTexture.h: OpenGL textures of OFX image rectangles.

#include "ofxhOpenGLTexture.h"
#include "ofxhOpenGLFunctions.h"

#include "ofxImageEffect.h"
#include "ofxGPURender.h" // kOfxBitDepthHalf

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <stdexcept>

namespace OFX {
  namespace Host {
    namespace OpenGL {

      using namespace Detail;

      // Alpha textures are RGBA too, filled and read with the client format GL_ALPHA: they sample
      // as (0, 0, 0, alpha) on every context (GL_ALPHA "Each element is a single alpha component
      // [...] converted to floating point and assembled into an RGBA element by attaching 0 for
      // red, green, and blue", OpenGL 2.1), and a plug-in writes an Alpha output to .a. OFX
      // 1.5.1's OpenGL suite names no internal format.
      static const struct { PixelDepth depth; const char *ofx; int bytes; GLenum type, internalFormat; } kDepths[] = {
        { PixelDepth::Byte, kOfxBitDepthByte, 1, kGLUnsignedByte, kGLRGBA8 },
        { PixelDepth::Short, kOfxBitDepthShort, 2, kGLUnsignedShort, kGLRGBA16 },
        { PixelDepth::Half, kOfxBitDepthHalf, 2, kGLHalfFloat, kGLRGBA16F },
        { PixelDepth::Float, kOfxBitDepthFloat, 4, kGLFloat, kGLRGBA32F },
      };

      static const auto &depthInfo(PixelDepth depth)
      {
        for (const auto &d : kDepths) {
          if (d.depth == depth) {
            return d;
          }
        }
        return kDepths[3];
      }

      bool pixelDepthFromOfx(const std::string &s, PixelDepth &depth)
      {
        for (const auto &d : kDepths) {
          if (s == d.ofx) {
            depth = d.depth;
            return true;
          }
        }
        return false;
      }

      const char *pixelDepthToOfx(PixelDepth depth) { return depthInfo(depth).ofx; }

      bool componentsFromOfx(const std::string &s, Components &components)
      {
        if (s == kOfxImageComponentRGBA) {
          components = Components::RGBA;
        } else if (s == kOfxImageComponentAlpha) {
          components = Components::Alpha;
        } else {
          return false;
        }
        return true;
      }

      const char *componentsToOfx(Components components)
      {
        return components == Components::RGBA ? kOfxImageComponentRGBA : kOfxImageComponentAlpha;
      }

      int bytesPerPixel(PixelDepth depth, Components components)
      {
        return depthInfo(depth).bytes * (components == Components::RGBA ? 4 : 1);
      }

      /// the client format of an image's pixels
      static GLenum glFormat(Components components) { return components == Components::RGBA ? kGLRGBA : kGLAlpha; }

      /// throws if OpenGL reported an error (and clears all of them)
      static void checkGL(const GLFunctions &gl, const char *what)
      {
        GLenum first = kGLNoError;
        GLenum e = gl.GetError();
        for (int i = 0; i < kMaxGLErrors && e != kGLNoError; ++i, e = gl.GetError()) {
          first = first == kGLNoError ? e : first;
        }
        if (first == kGLOutOfMemory) {
          throw std::bad_alloc();
        }
        if (first != kGLNoError) {
          throw std::runtime_error(std::string("OpenGL error ") + std::to_string(first) + " in " + what);
        }
      }

      /// runs call through the context's driverCall, passing its exceptions on. The errors
      /// OpenGL recorded before (e.g. the plug-in's) are cleared first: checkGL() reports call's.
      template <typename F>
      static void driver(const Context &context, F &&call)
      {
        std::exception_ptr error;
        context.driverCall([&] {
          try {
            const GLFunctions &gl = context.gl();
            clearGLErrors(gl);
            call(gl);
          } catch (...) {
            error = std::current_exception();
          }
        });
        if (error) {
          std::rethrow_exception(error);
        }
      }

      /// the pixel-store state of one transfer direction (unpack: upload, pack: readback)
      struct PixelStore {
        GLenum buffer, bufferBinding, rowLength, skipRows, skipPixels, alignment, swapBytes;
      };
      static const PixelStore kUnpack = { kGLPixelUnpackBuffer, kGLPixelUnpackBufferBinding, kGLUnpackRowLength,
                                          kGLUnpackSkipRows, kGLUnpackSkipPixels, kGLUnpackAlignment, kGLUnpackSwapBytes };
      static const PixelStore kPack = { kGLPixelPackBuffer, kGLPixelPackBufferBinding, kGLPackRowLength,
                                        kGLPackSkipRows, kGLPackSkipPixels, kGLPackAlignment, kGLPackSwapBytes };

      /// Transfers rect r of image row by row, or in one call where the rows are evenly spaced
      /// (rowBytes a positive multiple of the pixel size: GL_*_ROW_LENGTH). The transfers run in
      /// the plug-in's render (clipLoadTexture), with its state: the pixel buffer binding (which
      /// would make the pointer an offset), skips, alignment and byte swapping are set for the
      /// transfer and restored after. transfer(x, y, width, height, pointer) copies one block.
      template <typename F>
      static void transferRows(const GLFunctions &gl, const ImageRect &image, const OfxRectI &r,
                               const PixelStore &ps, F &&transfer)
      {
        const int pixelBytes = bytesPerPixel(image.depth, image.components);
        char *const base = static_cast<char *>(image.data);
        auto pixel = [&](int x, int y) {
          return base + static_cast<ptrdiff_t>(y - image.bounds.y1) * image.rowBytes + static_cast<ptrdiff_t>(x - image.bounds.x1) * pixelBytes;
        };
        const GLenum params[] = { ps.rowLength, ps.skipRows, ps.skipPixels, ps.alignment, ps.swapBytes };
        GLint saved[5] = { 0, 0, 0, 4, 0 };
        for (int i = 0; i < 5; ++i) {
          gl.GetIntegerv(params[i], &saved[i]);
        }
        GLint buffer = 0;
        if (gl.BindBuffer) {
          gl.GetIntegerv(ps.bufferBinding, &buffer);
          gl.BindBuffer(ps.buffer, 0);
        }
        gl.PixelStorei(ps.skipRows, 0);
        gl.PixelStorei(ps.skipPixels, 0);
        gl.PixelStorei(ps.alignment, 1);
        gl.PixelStorei(ps.swapBytes, 0);
        if (image.rowBytes > 0 && image.rowBytes % pixelBytes == 0) {
          gl.PixelStorei(ps.rowLength, image.rowBytes / pixelBytes);
          transfer(r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1, pixel(r.x1, r.y1));
        } else {
          gl.PixelStorei(ps.rowLength, 0);
          for (int y = r.y1; y < r.y2; ++y) {
            transfer(r.x1, y, r.x2 - r.x1, 1, pixel(r.x1, y));
          }
        }
        for (int i = 0; i < 5; ++i) {
          gl.PixelStorei(params[i], saved[i]);
        }
        if (gl.BindBuffer) {
          gl.BindBuffer(ps.buffer, static_cast<GLuint>(buffer));
        }
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Texture

      Texture::Texture(const Context &context, const OfxRectI &bounds, PixelDepth depth, Components components)
        : _context(context)
        , _bounds(bounds)
        , _depth(depth)
        , _components(components)
      {
        const int w = bounds.x2 - bounds.x1, h = bounds.y2 - bounds.y1;
        if (w < 0 || h < 0 || (context.info().maxTextureSize > 0 &&
                               (w > context.info().maxTextureSize || h > context.info().maxTextureSize))) {
          throw std::runtime_error("OpenGL: no texture of size " + std::to_string(w) + "x" + std::to_string(h));
        }
        const auto &d = depthInfo(depth);
        driver(_context, [&](const GLFunctions &gl) {
          GLint saved = 0;
          gl.GetIntegerv(kGLTextureBinding2D, &saved);
          gl.GenTextures(1, &_index);
          gl.BindTexture(kGLTexture2D, _index);
          // no mipmaps: a non-mipmap filter, or the texture would be incomplete
          gl.TexParameteri(kGLTexture2D, kGLTextureMinFilter, kGLNearest);
          gl.TexParameteri(kGLTexture2D, kGLTextureMagFilter, kGLNearest);
          gl.TexParameteri(kGLTexture2D, kGLTextureWrapS, kGLClampToEdge);
          gl.TexParameteri(kGLTexture2D, kGLTextureWrapT, kGLClampToEdge);
          gl.TexImage2D(kGLTexture2D, 0, d.internalFormat, w, h, 0, kGLRGBA, d.type, nullptr);
          gl.BindTexture(kGLTexture2D, saved);
          try {
            checkGL(gl, "glTexImage2D");
          } catch (...) {
            gl.DeleteTextures(1, &_index);
            throw;
          }
        });
      }

      Texture::~Texture()
      {
        if (!_index && !_framebuffer) {
          return; // abandon()ed (or never created)
        }
        _context.driverCall([&] {
          const GLFunctions &gl = _context.gl();
          if (_framebuffer) {
            gl.DeleteFramebuffers(1, &_framebuffer);
          }
          gl.DeleteTextures(1, &_index);
        });
      }

      int Texture::target() const { return static_cast<int>(kGLTexture2D); }

      int Texture::rowBytes() const { return (_bounds.x2 - _bounds.x1) * bytesPerPixel(_depth, _components); }

      void Texture::upload(const ImageRect &image)
      {
        if (image.components != _components) {
          throw std::runtime_error("OpenGL: upload of an image with other components than the texture's");
        }
        const OfxRectI r = intersect(image.bounds, _bounds);
        if (r.x1 == r.x2 || !image.data) {
          return;
        }
        driver(_context, [&](const GLFunctions &gl) {
          GLint saved = 0;
          gl.GetIntegerv(kGLTextureBinding2D, &saved);
          gl.BindTexture(kGLTexture2D, _index);
          const GLenum type = depthInfo(image.depth).type, format = glFormat(_components);
          transferRows(gl, image, r, kUnpack, [&](int x, int y, int w, int h, const void *p) {
            gl.TexSubImage2D(kGLTexture2D, 0, x - _bounds.x1, y - _bounds.y1, w, h, format, type, p);
          });
          gl.BindTexture(kGLTexture2D, saved);
          checkGL(gl, "glTexSubImage2D");
        });
      }

      void Texture::bindFramebuffer() const
      {
        const GLFunctions &gl = _context.gl();
        if (!_framebuffer) {
          gl.GenFramebuffers(1, &_framebuffer);
          gl.BindFramebuffer(kGLFramebuffer, _framebuffer);
          gl.FramebufferTexture2D(kGLFramebuffer, kGLColorAttachment0, kGLTexture2D, _index, 0);
        } else {
          gl.BindFramebuffer(kGLFramebuffer, _framebuffer);
        }
        if (gl.CheckFramebufferStatus(kGLFramebuffer) != kGLFramebufferComplete) {
          checkGL(gl, "glFramebufferTexture2D");
          throw std::runtime_error("OpenGL: the texture can't be a render target (incomplete framebuffer)");
        }
      }

      void Texture::readback(const ImageRect &image) const
      {
        if (image.components != _components) {
          throw std::runtime_error("OpenGL: readback to an image with other components than the texture's");
        }
        const OfxRectI r = intersect(image.bounds, _bounds);
        if (r.x1 == r.x2 || !image.data) {
          return;
        }
        driver(_context, [&](const GLFunctions &gl) {
          GLint saved = 0;
          gl.GetIntegerv(kGLFramebufferBinding, &saved);
          bindFramebuffer();
          const GLenum type = depthInfo(image.depth).type, format = glFormat(_components);
          transferRows(gl, image, r, kPack, [&](int x, int y, int w, int h, void *p) {
            gl.ReadPixels(x - _bounds.x1, y - _bounds.y1, w, h, format, type, p);
          });
          gl.BindFramebuffer(kGLFramebuffer, saved);
          checkGL(gl, "glReadPixels");
        });
      }

      void Texture::clear()
      {
        driver(_context, [&](const GLFunctions &gl) {
          // what glClear honours (a scissor, a colour mask), and the clear colour, as they were
          GLint saved = 0;
          GLfloat colour[4] = { 0.f, 0.f, 0.f, 0.f };
          GLboolean mask[4] = { 1, 1, 1, 1 };
          gl.GetIntegerv(kGLFramebufferBinding, &saved);
          gl.GetFloatv(kGLColorClearValue, colour);
          gl.GetBooleanv(kGLColorWritemask, mask);
          const bool scissor = gl.IsEnabled(kGLScissorTest) != 0;
          bindFramebuffer();
          gl.Disable(kGLScissorTest);
          gl.ColorMask(1, 1, 1, 1);
          gl.ClearColor(0.f, 0.f, 0.f, 0.f);
          gl.Clear(kGLColorBufferBit);
          gl.ClearColor(colour[0], colour[1], colour[2], colour[3]);
          gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
          if (scissor) {
            gl.Enable(kGLScissorTest);
          }
          gl.BindFramebuffer(kGLFramebuffer, saved);
          checkGL(gl, "glClear");
        });
      }

      void Texture::bindAsRenderTarget()
      {
        driver(_context, [&](const GLFunctions &gl) {
          bindFramebuffer();
          checkGL(gl, "glBindFramebuffer");
        });
      }

    } // OpenGL
  } // Host
} // OFX
