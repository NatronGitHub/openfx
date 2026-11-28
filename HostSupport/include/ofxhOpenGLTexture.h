#ifndef OFXH_OPENGL_TEXTURE_H
#define OFXH_OPENGL_TEXTURE_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhOpenGLTexture.h

  OpenGL textures of OFX image rectangles, for the OpenGL render suite (ofxGPURender.h):
  upload of an image (bounds, rowBytes, components, depth) to a texture, readback of a texture
  to an image, and a texture as the render target. The texture target and index are those of
  the kOfxImageEffectPropOpenGLTextureTarget and kOfxImageEffectPropOpenGLTextureIndex
  properties. Part of openfx::HostSupportOpenGL (see ofxhOpenGLContext.h).

  Pixels are copied as they are: no colour, premultiplication or orientation change (OFX images
  and OpenGL textures both start at the bottom row). A depth change between the image and the
  texture is OpenGL's normalized conversion (byte 255 and short 65535 are 1.0).
  Textures are GL_RGBA8, GL_RGBA16, GL_RGBA16F or GL_RGBA32F, also for Alpha and RGB images,
  which are uploaded and read back with the client format GL_ALPHA or GL_RGB: an Alpha texture
  samples as (0, 0, 0, alpha) and an RGB one as (r, g, b, 1) on every context, and a plug-in
  rendering to one writes alpha to .a (as in Natron and the openfx-misc OpenGL plug-ins), or
  r, g and b to .rgb. The transfers save and restore the pixel-store state and the pixel buffer
  bindings they change.

  Create, use and destroy a Texture with its context current (CurrentContext). Errors throw:
  std::bad_alloc when OpenGL is out of memory, std::runtime_error otherwise.
*/

#include "ofxCore.h"
#include "ofxhOpenGLContext.h"

#include <string>

namespace OFX {
  namespace Host {
    namespace OpenGL {

      /// the OFX pixel depths a texture can have (kOfxBitDepthByte, Short, Half, Float)
      enum class PixelDepth { Byte, Short, Half, Float };
      /// the OFX components a texture can have (kOfxImageComponentRGBA, RGB, Alpha)
      enum class Components { RGBA, RGB, Alpha };

      /// from and to the OFX strings; false if s isn't one
      bool pixelDepthFromOfx(const std::string &s, PixelDepth &depth);
      const char *pixelDepthToOfx(PixelDepth depth);
      bool componentsFromOfx(const std::string &s, Components &components);
      const char *componentsToOfx(Components components);
      /// the number of components (4, 3, 1), and the size of a pixel
      int componentCount(Components components);
      int bytesPerPixel(PixelDepth depth, Components components);

      /// An OFX image rectangle in memory, as in an image handle (kOfxImagePropData,
      /// kOfxImagePropBounds, kOfxImagePropRowBytes, which may be negative)
      struct ImageRect {
        void *data = nullptr;           ///< the pixel at (bounds.x1, bounds.y1)
        OfxRectI bounds = { 0, 0, 0, 0 };
        int rowBytes = 0;
        PixelDepth depth = PixelDepth::Float;
        Components components = Components::RGBA;
      };

      /// A GL_TEXTURE_2D texture of an OFX pixel rectangle: texel (0, 0) is pixel
      /// (bounds.x1, bounds.y1). Its contents are undefined until uploaded, cleared or rendered.
      class Texture {
      public:
        Texture(const Context &context, const OfxRectI &bounds, PixelDepth depth, Components components);
        ~Texture();
        Texture(const Texture &) = delete;
        Texture &operator=(const Texture &) = delete;

        /// kOfxImageEffectPropOpenGLTextureIndex (a GLuint) and Target (a GLenum)
        unsigned int index() const { return _index; }
        int target() const;
        const OfxRectI &bounds() const { return _bounds; }
        PixelDepth depth() const { return _depth; }
        Components components() const { return _components; }
        /// the rowBytes of an image of the texture, as in the texture handle (kOfxImagePropRowBytes)
        int rowBytes() const;

        /// copies the pixels of image inside the texture bounds to the texture (the other
        /// texels are unchanged); the components must be the texture's, the depth may differ
        void upload(const ImageRect &image);
        /// copies the texels inside image.bounds to the image (its other pixels are unchanged)
        void readback(const ImageRect &image) const;
        /// sets all texels to transparent black (whatever the scissor and the colour mask; the
        /// clear colour is restored)
        void clear();
        /// makes the texture the color buffer (render target): binds a framebuffer with it as
        /// color attachment 0 (the viewport is unchanged)
        void bindAsRenderTarget();
        /// After a lost context: forgets the texture and framebuffer names, so that the
        /// destructor makes no OpenGL call
        void abandon() { _index = 0; _framebuffer = 0; }

      private:
        void bindFramebuffer() const;
        const Context &_context;
        OfxRectI _bounds;
        PixelDepth _depth;
        Components _components;
        unsigned int _index = 0;
        mutable unsigned int _framebuffer = 0;   ///< created by the first bindFramebuffer()
      };

    } // OpenGL
  } // Host
} // OFX

#endif // OFXH_OPENGL_TEXTURE_H
