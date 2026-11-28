// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhMetalRender.h: the render mode, and the images of a Metal render. Objective-C++ with ARC:
// the session holds every id<MTLBuffer> and id<MTLTexture> it made as a strong reference until it
// ends. Every Objective-C call runs in an @autoreleasepool: some autorelease (e.g.
// MTLBuffer.contents, its buffer), and a host's render thread may have no pool, which would keep
// them alive until it ends.

#import <Metal/Metal.h>

#include "ofxhMetalRender.h"
#include "../gpu/ofxhGPUImages.h"

#include "ofxGPURender.h"

#include <mutex>
#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "ofxhMetalRender.mm needs ARC (-fobjc-arc)"
#endif

namespace OFX {
  namespace Host {
    namespace Metal {

      /// the plug-in supports Metal buffers: kOfxImageEffectPropMetalRenderSupported "true" - "the
      /// host or plug-in can support Metal render" (ofxGPURender.h), or "needed", a value of
      /// upstream main's @propdef (values "false", "true", needed)
      static bool pluginBuffers(const ImageEffect::Instance &effect)
      {
        const std::string &v = effect.getProps().getStringProperty(kOfxImageEffectPropMetalRenderSupported);
        return v == "true" || v == "needed";
      }

      /// the plug-in supports Metal textures: kOfxImageEffectPropMetalTextureSupported "true" - "the
      /// host or plug-in can support Metal texture render" (upstream main; values "false", "true")
      static bool pluginTextures(const ImageEffect::Instance &effect)
      {
        return effect.getProps().getStringProperty(kOfxImageEffectPropMetalTextureSupported) == "true";
      }

      /// the output and the connected inputs can be textures: RGBA or Alpha, as "a host MUST NOT
      /// pass ::kOfxImageComponentRGB images as textures"
      static bool clipsTexturable(const ImageEffect::Instance &effect)
      {
        for (const auto &c : effect.getDescriptor().getClips()) {
          const ImageEffect::ClipInstance *clip = effect.getClip(c.first);
          if (clip && (clip->isOutput() || clip->getConnected()) && clip->getComponents() != kOfxImageComponentRGBA &&
              clip->getComponents() != kOfxImageComponentAlpha) {
            return false;
          }
        }
        return true;
      }

      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostBuffers, bool hostTextures,
                                  bool preferTextures)
      {
        const bool buffers = hostBuffers && pluginBuffers(effect);
        const bool textures = hostTextures && pluginTextures(effect) && clipsTexturable(effect);
        if (buffers || textures) {
          // "both: the host chooses one of the two per action"
          return textures && (preferTextures || !buffers) ? RenderMode::Textures : RenderMode::Buffers;
        }
        return ImageEffect::canRenderOnCPU(effect) ? RenderMode::CPU : RenderMode::None;
      }

      bool metalFailed(const RenderSession &session, OfxStatus st)
      {
        return session.failed() || ImageEffect::gpuRenderFailed(st);
      }

      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st)
      {
        return metalFailed(session, st) && ImageEffect::canRenderOnCPU(effect);
      }

      /// the mode's Enabled property may be 1: "If a plug-in and host have both set
      /// kOfxImageEffectPropMetalRenderSupported="true" then the host MAY set this property"
      /// (buffers); "only for a path that both host and plug-in declared as supported" (textures);
      /// with a queue. Logs why not.
      static bool sessionAllowed(const ImageEffect::Instance &effect, const Queue &queue, RenderMode mode)
      {
        if (mode != RenderMode::Buffers && mode != RenderMode::Textures) {
          return false; // (a CPU render: nothing to say)
        }
        const bool buffers = mode == RenderMode::Buffers;
        const char *prop = buffers ? kOfxImageEffectPropMetalRenderSupported : kOfxImageEffectPropMetalTextureSupported;
        std::string why;
        if (!queue.native()) {
          why = "no command queue";
        } else if (!(buffers ? pluginBuffers(effect) : pluginTextures(effect))) {
          why = std::string("the plug-in's ") + prop + " is \"" + effect.getProps().getStringProperty(prop) + "\"";
        } else if (!ImageEffect::gImageEffectHost ||
                   ImageEffect::gImageEffectHost->getProperties().getStringProperty(prop) != "true") {
          why = std::string("the host's ") + prop + " isn't \"true\"";
        }
        if (!why.empty()) {
          queue.device().log(LogLevel::Error, std::string("Metal: no Metal ") + (buffers ? "buffer" : "texture") +
                             " render (" + (buffers ? kOfxImageEffectPropMetalEnabled : kOfxImageEffectPropMetalTextureEnabled) +
                             " 0): " + why);
        }
        return why.empty();
      }

      /// The texture format of an image: "for RGBA images ::kOfxBitDepthByte is
      /// MTLPixelFormatRGBA8Unorm, ::kOfxBitDepthShort is MTLPixelFormatRGBA16Unorm,
      /// ::kOfxBitDepthHalf is MTLPixelFormatRGBA16Float and ::kOfxBitDepthFloat is
      /// MTLPixelFormatRGBA32Float; alpha-only images use the single-channel R formats"; Invalid for
      /// others (RGB: "a host MUST NOT pass ::kOfxImageComponentRGB images as textures")
      static MTLPixelFormat textureFormat(const GPU::PixelLayout &l)
      {
        const bool rgba = l.components == 4, alpha = l.components == 1;
        if (!rgba && !alpha) {
          return MTLPixelFormatInvalid;
        }
        if (l.depth == kOfxBitDepthByte) {
          return rgba ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatR8Unorm;
        }
        if (l.depth == kOfxBitDepthShort) {
          return rgba ? MTLPixelFormatRGBA16Unorm : MTLPixelFormatR16Unorm;
        }
        if (l.depth == kOfxBitDepthHalf) {
          return rgba ? MTLPixelFormatRGBA16Float : MTLPixelFormatR16Float;
        }
        if (l.depth == kOfxBitDepthFloat) {
          return rgba ? MTLPixelFormatRGBA32Float : MTLPixelFormatR32Float;
        }
        return MTLPixelFormatInvalid;
      }

      /// the largest 2D texture of the Macs' GPUs (Apple's Metal feature set tables: 16384 x 16384 for
      /// the Apple and Mac families); a larger image isn't a texture (the render then fails, and the
      /// host may render on the CPU), rather than an error of Metal's validation
      static const int kMaxTextureSize = 16384;

      /// the bytes after the pixels of each buffer of the session, which waitForWork() writes
      static const NSUInteger kPadding = 4;
      /// the largest texel (RGBA32Float), the scratch row of waitForWork()'s texture copies
      static const NSUInteger kTexelBytes = 16;

      /// Waits for the work committed to queue before this call that reads or writes buffers (of
      /// the session, each with kPadding bytes after its pixels) or textures: commits a blit that
      /// reads the first 4 bytes of each buffer and writes its padding, and reads texel (0, 0) of
      /// each texture and, if writeTextures, writes that value back to it (the session's
      /// destructor), and copies readback's region of its texture to its staging buffer
      /// (readOutput()), then waits until it completes. "" or why it failed; *committed (if not
      /// null): the blit was committed and waited for (else nothing waited, e.g. out of memory: the
      /// resources may still be in use).
      ///
      /// The host doesn't know the plug-in's last command buffer, and the completion of a later
      /// command buffer isn't documented to imply that of the earlier ones: commit() only says "The
      /// GPU starts the command buffer after it starts any command buffers that are ahead of it in
      /// the same command queue". What Apple documents is hazard tracking: "Metal tracks hazards for
      /// MTLBuffer and MTLTexture instances when you create them with default"
      /// (MTLHazardTrackingModeTracked), and will "Prevent subsequent commands from running until
      /// write operations finish" and "Delay write operations until all previous read operations
      /// finish". So the blit's reads wait for the plug-in's writes to the resources, and its writes
      /// for the plug-in's reads. Apple states this per command, not explicitly across command
      /// buffers; and it needs the plug-in's work on this queue ("The plug-in SHOULD ensure that
      /// its render action enqueues any asynchronous Metal operations onto the supplied queue",
      /// kOfxImageEffectPropMetalCommandQueue). A command buffer of the plug-in that failed goes
      /// unseen: only this one's status is known (see ofxhMetalDevice.h).
      struct Readback {
        id<MTLTexture> texture;
        MTLRegion region;
        id<MTLBuffer> staging;  ///< packed rows of the region
        NSUInteger rowBytes;
      };
      static std::string waitForWork(id<MTLCommandQueue> queue, const std::vector<id<MTLBuffer>> &buffers,
                                     const std::vector<id<MTLTexture>> &textures, bool writeTextures,
                                     const Readback *readback = nullptr, bool *committed = nullptr)
      {
        std::string why;
        if (committed) {
          *committed = false;
        }
        @autoreleasepool {
          // one scratch slot per resource: Apple doesn't document that the blits of an encoder run in order
          const NSUInteger slots = buffers.size() + textures.size();
          id<MTLBuffer> scratch = [queue.device newBufferWithLength:kTexelBytes * (slots ? slots : 1)
                                                            options:MTLResourceStorageModeShared];
          // (unretained references: the caller keeps the resources, and scratch lives until the wait)
          id<MTLCommandBuffer> cb = [queue commandBufferWithUnretainedReferences];
          id<MTLBlitCommandEncoder> blit = cb ? [cb blitCommandEncoder] : nil;
          if (!scratch || !blit) {
            return "no blit command buffer";
          }
          cb.label = @"OFX HostSupport wait";
          NSUInteger slot = 0;
          for (id<MTLBuffer> b : buffers) {
            // (macOS: offsets and sizes multiple of 4, as the buffers' lengths are)
            [blit copyFromBuffer:b sourceOffset:0 toBuffer:scratch destinationOffset:kTexelBytes * slot++ size:4];
            [blit fillBuffer:b range:NSMakeRange(b.length - kPadding, kPadding) value:0];
          }
          const MTLOrigin origin = MTLOriginMake(0, 0, 0);
          const MTLSize texel = MTLSizeMake(1, 1, 1);
          for (id<MTLTexture> t : textures) {
            const NSUInteger offset = kTexelBytes * slot++;
            [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:texel toBuffer:scratch
                destinationOffset:offset destinationBytesPerRow:kTexelBytes destinationBytesPerImage:kTexelBytes];
            if (writeTextures) {
              // (the value just read: the texture is unchanged)
              [blit copyFromBuffer:scratch sourceOffset:offset sourceBytesPerRow:kTexelBytes sourceBytesPerImage:kTexelBytes
                        sourceSize:texel toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
            }
          }
          if (readback) {
            [blit copyFromTexture:readback->texture sourceSlice:0 sourceLevel:0 sourceOrigin:readback->region.origin
                       sourceSize:readback->region.size toBuffer:readback->staging destinationOffset:0
           destinationBytesPerRow:readback->rowBytes
         destinationBytesPerImage:readback->rowBytes * readback->region.size.height];
          }
          [blit endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          if (committed) {
            *committed = true;
          }
          if (cb.status != MTLCommandBufferStatusCompleted) {
            why = cb.error ? cb.error.localizedDescription.UTF8String : "the command buffer failed";
          }
        }
        return why;
      }

      /// The window r (within l's bounds) of a texture of l's bounds: texel (i, j) is the pixel
      /// (x1 + i, y1 + j) ("their origin is the bottom left with y increasing upwards"), so the
      /// texture's rows go in the CPU image's order, bottom up, and its region of r is at (r.x1 - x1,
      /// r.y1 - y1)
      static MTLRegion textureRegion(const GPU::PixelLayout &l, const OfxRectI &r)
      {
        return MTLRegionMake2D(static_cast<NSUInteger>(r.x1 - l.bounds.x1), static_cast<NSUInteger>(r.y1 - l.bounds.y1),
                               static_cast<NSUInteger>(r.x2 - r.x1), static_cast<NSUInteger>(r.y2 - r.y1));
      }

      /// a shared buffer of the packed rows of r (within l's bounds), from l's CPU pixels if toGPU;
      /// nil if it can't be made
      static id<MTLBuffer> stagingBuffer(id<MTLDevice> device, const GPU::PixelLayout &l, const OfxRectI &r, bool toGPU)
      {
        const size_t rowSize = static_cast<size_t>(r.x2 - r.x1) * static_cast<size_t>(l.pixelBytes);
        id<MTLBuffer> staging = [device newBufferWithLength:rowSize * static_cast<size_t>(r.y2 - r.y1)
                                                    options:MTLResourceStorageModeShared];
        if (staging && toGPU) {
          GPU::copyRect(l, r, static_cast<char *>(staging.contents), rowSize, r.x1, r.y1, /*toRows=*/true);
        }
        return staging;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // RenderSession

      /// every buffer and texture the session made (strong): a plug-in releases its images
      /// (clipReleaseImage) while its command buffers may still use them, one made with
      /// commandBufferWithUnretainedReferences doesn't keep them alive, and for textures "The host
      /// MUST keep them alive until the work the plug-in enqueued on the command queue has completed"
      struct RenderSession::Resources {
        std::mutex mutex;
        std::vector<id<MTLBuffer>> buffers;
        std::vector<id<MTLTexture>> textures;
      };

      /// an image of the session: one of its Metal buffers or textures
      class RenderSession::Handle : public ImageEffect::Image {
      public:
        Handle(RenderSession &session, ImageEffect::ClipInstance &clip, const ImageEffect::Image &image,
               id<MTLResource> resource, int rowBytes)
          : ImageEffect::Image(clip,
                               image.getDoubleProperty(kOfxImageEffectPropRenderScale, 0),
                               image.getDoubleProperty(kOfxImageEffectPropRenderScale, 1),
                               (__bridge void *)resource, image.getBounds(), image.getROD(), rowBytes,
                               image.getStringProperty(kOfxImagePropField),
                               image.getStringProperty(kOfxImagePropUniqueIdentifier))
          , _images(*session._images)
          , _resource(resource)
        {
          GPU::copyImageProperties(*this, image);
          _images.add(this);
        }

        ~Handle() override
        {
          // (the session keeps the resource until it ends: RenderSession::Resources)
          _images.remove(this);
        }

        id<MTLResource> resource() const { return _resource; }

      private:
        /// (not through the session: its unique_ptr is null while ~SessionImages deletes the images left)
        GPU::SessionImages &_images;
        id<MTLResource> _resource;  ///< an id<MTLBuffer> or id<MTLTexture> (strong; Resources has it too)
      };

      RenderSession::RenderSession(ImageEffect::Instance &effect, Queue &queue, RenderMode mode)
        : _queue(queue)
        , _mode(mode)
        , _ok(sessionAllowed(effect, queue, mode))
        , _scope(effect, !_ok ? ImageEffect::GPURenderScope::API::CPU
                              : mode == RenderMode::Textures ? ImageEffect::GPURenderScope::API::MetalTextures
                                                             : ImageEffect::GPURenderScope::API::Metal,
                 queue.native())
        , _resources(new Resources)
        , _images(new GPU::SessionImages("Metal", [&queue](LogLevel level, const std::string &message) {
            queue.device().log(level, message);
          }))
      {
      }

      RenderSession::~RenderSession()
      {
        // the plug-in's work on the resources first, whatever happened (readOutput() may not have
        // run); then they are released (_images deletes the images not released, then _resources goes)
        @autoreleasepool {
          std::vector<id<MTLBuffer>> buffers;
          std::vector<id<MTLTexture>> textures;
          {
            std::lock_guard<std::mutex> lock(_resources->mutex);
            buffers = _resources->buffers;
            textures = _resources->textures;
          }
          if (!buffers.empty() || !textures.empty()) {
            bool committed = false;
            const std::string why = waitForWork((__bridge id<MTLCommandQueue>)_queue.native(), buffers, textures,
                                                /*writeTextures=*/true, nullptr, &committed);
            if (!committed) {
              // nothing waited: the plug-in's work may still use them, so they are leaked rather
              // than released under it
              new Resources{ {}, buffers, textures }; // (never deleted)
            }
            if (!why.empty()) {
              try {
                _images->log(LogLevel::Error, "waiting for the render's command buffers: " + why +
                             (committed ? std::string()
                                        : " (its " + std::to_string(buffers.size() + textures.size()) +
                                          " buffer(s) and texture(s) are leaked)"));
              } catch (...) {
                // (the host's logger threw: dropped, in a destructor; C.36)
              }
            }
          }
          // then the images the plug-in didn't release, and the session's references, in the pool:
          // a Metal object's dealloc may autorelease (its device), and the thread may have no pool
          _images.reset();
          std::lock_guard<std::mutex> lock(_resources->mutex);
          _resources->buffers.clear();
          _resources->textures.clear();
        }
      }

      bool RenderSession::failed() const
      {
        return _images->failed();
      }

      int RenderSession::liveImages() const
      {
        return _images->liveImages();
      }

      RenderSession::Handle *RenderSession::copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image,
                                                 const OfxRectI &rect, bool output)
      {
        if (!_ok) {
          _images->fail("no Metal render in this session (see its constructor)");
          return nullptr;
        }
        const GPU::PixelLayout l(image);
        const bool textures = _mode == RenderMode::Textures;
        if (!_images->copyable(l, textures ? "a Metal texture" : "a Metal buffer")) {
          return nullptr;
        }
        const int w = l.width(), h = l.height();
        @autoreleasepool {
          id<MTLDevice> device = (__bridge id<MTLDevice>)_queue.device().nativeDevice();
          if (textures) {
            const MTLPixelFormat format = textureFormat(l);
            if (format == MTLPixelFormatInvalid) {
              // "a host MUST NOT pass ::kOfxImageComponentRGB images as textures"
              _images->fail("an image of components " + l.componentsName + " can't be a Metal texture (RGBA or Alpha only)");
              return nullptr;
            }
            if (w > kMaxTextureSize || h > kMaxTextureSize) {
              _images->fail("an image of " + std::to_string(w) + "x" + std::to_string(h) + " can't be a Metal texture (" +
                            std::to_string(kMaxTextureSize) + " at most)");
              return nullptr;
            }
            MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                            width:static_cast<NSUInteger>(w)
                                                                                           height:static_cast<NSUInteger>(h)
                                                                                        mipmapped:NO];
            // "Source textures MUST be readable from shaders; the output texture MUST be writable
            // from shaders" (and readable: a plug-in may read what it wrote)
            desc.usage = output ? (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite) : MTLTextureUsageShaderRead;
            // GPU-only: the pixels go through shared staging buffers and blits, on every Mac
            desc.storageMode = MTLStorageModePrivate;
            id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
            if (!texture) {
              _images->fail("a Metal texture of " + std::to_string(w) + "x" + std::to_string(h) + " " + l.componentsName +
                            " " + l.depth + ": newTextureWithDescriptor failed");
              return nullptr;
            }
            texture.label = @"OFX HostSupport image";
            {
              std::lock_guard<std::mutex> lock(_resources->mutex);
              _resources->textures.push_back(texture);
            }
            const OfxRectI r = GPU::intersect(rect, l.bounds);
            if (r.x2 > r.x1) {
              // the upload, on the queue, waited for (as #370's OpenCL copies are blocking): the
              // plug-in's commands find the pixels in place, with no hazard to rely on
              id<MTLBuffer> staging = stagingBuffer(device, l, r, /*toGPU=*/true);
              id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)_queue.native();
              // (unretained references: the wait below keeps the staging buffer and the texture alive)
              id<MTLCommandBuffer> cb = staging ? [queue commandBufferWithUnretainedReferences] : nil;
              id<MTLBlitCommandEncoder> blit = cb ? [cb blitCommandEncoder] : nil;
              if (!blit) {
                _images->fail("uploading a Metal texture: no blit command buffer");
                return nullptr;
              }
              cb.label = @"OFX HostSupport upload";
              const MTLRegion region = textureRegion(l, r);
              const NSUInteger rowBytes = region.size.width * static_cast<NSUInteger>(l.pixelBytes);
              [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:rowBytes
                   sourceBytesPerImage:rowBytes * region.size.height sourceSize:region.size toTexture:texture
                      destinationSlice:0 destinationLevel:0 destinationOrigin:region.origin];
              [blit endEncoding];
              [cb commit];
              [cb waitUntilCompleted];
              if (cb.status != MTLCommandBufferStatusCompleted) {
                _images->fail(std::string("uploading a Metal texture: ") +
                              (cb.error ? cb.error.localizedDescription.UTF8String : "the command buffer failed"));
                return nullptr;
              }
            }
            // "::kOfxImagePropRowBytes has no meaning for a texture and is 0"
            return new Handle(*this, clip, image, texture, 0);
          }
          // the pixels, then kPadding bytes for waitForWork(), at a multiple of 4
          const size_t pixels = l.packedRow() * static_cast<size_t>(h);
          const size_t length = (pixels + 3) / 4 * 4 + kPadding;
          // shared storage: the CPU copies to and from it, and it needs no synchronization on any Mac
          id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
          if (!buffer) {
            _images->fail("a Metal buffer of " + std::to_string(w) + "x" + std::to_string(h) + " " + l.componentsName +
                          " " + l.depth + " (" + std::to_string(length) + " bytes): newBufferWithLength failed");
            return nullptr;
          }
          buffer.label = @"OFX HostSupport image";
          {
            std::lock_guard<std::mutex> lock(_resources->mutex);
            _resources->buffers.push_back(buffer);
          }
          // the pixels of rect, in the buffer of the bounds
          GPU::copyRect(l, GPU::intersect(rect, l.bounds), static_cast<char *>(buffer.contents), l.packedRow(),
                        l.bounds.x1, l.bounds.y1, /*toRows=*/true);
          return new Handle(*this, clip, image, buffer, static_cast<int>(l.packedRow()));
        }
      }

      ImageEffect::Image *RenderSession::setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                                   const OfxRectI &renderWindow)
      {
        return _images->setOutput(clip, image, renderWindow, [this](ImageEffect::ClipInstance &c, const ImageEffect::Image &i,
                                                                    const OfxRectI &r) { return copy(c, i, r, true); });
      }

      bool RenderSession::readOutput()
      {
        if (!_images->hasOutput()) {
          return false;
        }
        @autoreleasepool {
          id<MTLResource> output = static_cast<Handle *>(_images->output())->resource();
          const bool textures = _mode == RenderMode::Textures;
          id<MTLBuffer> buffer = textures ? nil : (id<MTLBuffer>)output;
          id<MTLTexture> texture = textures ? (id<MTLTexture>)output : nil;
          const GPU::PixelLayout l(*_images->outputImage());
          if (!l.data) {
            _images->fail("an output image without pixel data");
            return false;
          }
          // the render window only: the plug-in renders "the window (in PixelCoordinates) to render"
          // (kOfxImageEffectPropRenderWindow), and the host's other pixels stay as they are
          const OfxRectI r = GPU::intersect(_images->renderWindow(), l.bounds);
          Readback readback = { texture, MTLRegion(), nil, 0 };
          if (textures && r.x2 > r.x1) {
            readback.region = textureRegion(l, r);
            readback.rowBytes = readback.region.size.width * static_cast<NSUInteger>(l.pixelBytes);
            readback.staging = stagingBuffer((__bridge id<MTLDevice>)_queue.device().nativeDevice(), l, r, /*toGPU=*/false);
            if (!readback.staging) {
              _images->fail("reading a Metal texture back: no staging buffer");
              return false;
            }
          }
          const std::string why = waitForWork((__bridge id<MTLCommandQueue>)_queue.native(),
                                              textures ? std::vector<id<MTLBuffer>>() : std::vector<id<MTLBuffer>>{ buffer },
                                              textures ? std::vector<id<MTLTexture>>{ texture } : std::vector<id<MTLTexture>>(),
                                              /*writeTextures=*/false, readback.staging ? &readback : nullptr);
          if (!why.empty()) {
            _images->fail("waiting for the render's command buffers: " + why);
            return false;
          }
          if (textures) {
            if (readback.staging) {
              GPU::copyRect(l, r, static_cast<char *>(readback.staging.contents), readback.rowBytes, r.x1, r.y1,
                            /*toRows=*/false);
            }
          } else {
            GPU::copyRect(l, GPU::intersect(_images->renderWindow(), l.bounds), static_cast<char *>(buffer.contents),
                          l.packedRow(), l.bounds.x1, l.bounds.y1, /*toRows=*/false);
          }
          return true;
        }
      }

      ImageEffect::Image *RenderSession::getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image)
      {
        return _images->getImage(clip, image, [this](ImageEffect::ClipInstance &c, const ImageEffect::Image &i,
                                                     const OfxRectI &r) { return copy(c, i, r, false); });
      }

    } // Metal
  } // Host
} // OFX
