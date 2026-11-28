#ifndef OFXH_METAL_RENDER_H
#define OFXH_METAL_RENDER_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhMetalRender.h

  Metal renders of HostSupport effect instances (ofxGPURender.h, OFX 1.5), on the devices and
  queues of ofxhMetalDevice.h. Part of openfx::HostSupportMetal.

  - chooseRenderMode(): the CPU, Metal buffers or Metal textures, from
    kOfxImageEffectPropMetalRenderSupported (buffers), kOfxImageEffectPropMetalTextureSupported
    (textures, upstream main, after OFX 1.5.1) and kOfxImageEffectPropCPURenderSupported, and the CPU
    retry after a failed render;
  - RenderSession: the render actions of one render (Begin/Render/EndSequenceRender) with
    kOfxImageEffectPropMetalEnabled 1 (buffers) or kOfxImageEffectPropMetalTextureEnabled 1
    (textures), and kOfxImageEffectPropMetalCommandQueue (an ImageEffect::GPURenderScope), and the
    images the host's ClipInstance::getImage() returns meanwhile: the CPU images it would return,
    copied to Metal buffers ("1 indicates that the kOfxImagePropData of each image of each clip is a
    Metal id<MTLBuffer>", kOfxImageEffectPropMetalEnabled) or textures ("1 indicates that
    ::kOfxImagePropData of each image of each clip is a Metal id<MTLTexture>",
    kOfxImageEffectPropMetalTextureEnabled); the output's render window is copied back to the
    host's output image.

  The buffers have the bounds, depth and components of the CPU images, with no conversion, packed
  (kOfxImagePropRowBytes is the width times the pixel size), in shared storage
  (MTLResourceStorageModeShared). A buffer's length includes up to 7 bytes of padding after the
  pixels (packedRow x height): a plug-in sizes its work from the image's bounds and row bytes, not
  from [buffer length]. Their rows are bottom up, as in CPU memory: OFX doesn't define
  the row order of Metal buffers (kOfxImagePropRowBytes may be negative "for CPU images only, not
  CUDA/Metal/OpenCL Buffers"), so this is this library's choice.

  The textures are 2D, of the CPU image's bounds, "for RGBA images ::kOfxBitDepthByte is
  MTLPixelFormatRGBA8Unorm, ::kOfxBitDepthShort is MTLPixelFormatRGBA16Unorm, ::kOfxBitDepthHalf is
  MTLPixelFormatRGBA16Float and ::kOfxBitDepthFloat is MTLPixelFormatRGBA32Float; alpha-only images
  use the single-channel R formats" (R8Unorm, R16Unorm, R16Float, R32Float); "a host MUST NOT pass
  ::kOfxImageComponentRGB images as textures", so chooseRenderMode() picks textures only when the
  output and the connected inputs are RGBA or Alpha. Texel (i, j) is the pixel (x1 + i, y1 + j) of
  the bounds: "their origin is the bottom left with y increasing upwards", so a plug-in "does not
  need to flip". Sources are MTLTextureUsageShaderRead, the output ShaderRead and ShaderWrite
  ("Source textures MUST be readable from shaders; the output texture MUST be writable from
  shaders"); kOfxImagePropRowBytes is 0 ("has no meaning for a texture and is 0"). They are
  private (GPU-only): the pixels go through shared staging buffers and blits on the queue (the
  uploads are waited for, as the buffers' copies are synchronous).

  A host's render with Metal, one queue per render:

    const RenderMode mode = chooseRenderMode(effect, hostBuffers, hostTextures);
    if (mode == RenderMode::Buffers || mode == RenderMode::Textures) {
      Queue queue(device);
      RenderSession session(effect, queue, mode);
      session.setOutput(*outputClip, *outputImage, renderWindow);  // the host's output image, before the actions
      // the host's ClipInstance::getImage() override returns session.getImage(clip, image) during:
      st = effect.renderAction(...);
      if (st == kOfxStatOK) session.readOutput();
      if (cpuRetryAllowed(effect, session, st)) ... render again on the CPU (after the session)
    }
*/

#include "ofxhClip.h"
#include "ofxhImageEffect.h"
#include "ofxhMetalDevice.h"

#include <memory>

namespace OFX {
  namespace Host {
    namespace GPU { class SessionImages; } // (internal: src/gpu/ofxhGPUImages.h)

    namespace Metal {

      /// how to render an instance
      enum class RenderMode {
        CPU,       ///< kOfxImageEffectPropMetalEnabled and kOfxImageEffectPropMetalTextureEnabled 0
        Buffers,   ///< kOfxImageEffectPropMetalEnabled 1, images in Metal buffers
        Textures,  ///< kOfxImageEffectPropMetalTextureEnabled 1, images in Metal textures
        None       ///< the plug-in can't render on the CPU, and can't use the host's Metal
      };

      /// The mode of the next render of effect, for a host that can pass its images now as Metal
      /// buffers (hostBuffers: it has a Device, and its kOfxImageEffectPropMetalRenderSupported is
      /// "true") and textures (hostTextures: likewise with kOfxImageEffectPropMetalTextureSupported).
      /// Buffers if the plug-in can too (its kOfxImageEffectPropMetalRenderSupported is "true", or
      /// "needed", a value of upstream main's metadata); Textures if its
      /// kOfxImageEffectPropMetalTextureSupported is "true" and the output and the connected inputs
      /// are RGBA or Alpha ("a host MUST NOT pass ::kOfxImageComponentRGB images as textures"); if
      /// both, Textures if preferTextures ("both: the host chooses one of the two per action");
      /// else the CPU if ImageEffect::canRenderOnCPU() (kOfxImageEffectPropCPURenderSupported
      /// alone: "needed" says nothing about the CPU here); else None.
      /// Also the retry after a failed Metal render: chooseRenderMode(effect, false).
      /// Reads the instance properties, which the plug-in may change in kOfxActionInstanceChanged.
      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostBuffers, bool hostTextures = false,
                                  bool preferTextures = false);

      class RenderSession;

      /// After a Metal action of session that returned st: whether Metal failed: the session
      /// failed (RenderSession::failed()), or ImageEffect::gpuRenderFailed(st), the rule of the
      /// OpenGL and OpenCL retries too.
      bool metalFailed(const RenderSession &session, OfxStatus st);

      /// After a Metal action of effect in session that returned st: the host may render it again
      /// on the CPU. Metal failed (metalFailed()), and ImageEffect::canRenderOnCPU(effect)
      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st);

      /// The render actions of one Metal render of effect on queue, sent by the thread that
      /// creates the session while it lives (an ImageEffect::GPURenderScope), and its images.
      /// An automatic object: destroy it on its thread, before the queue, and before the CPU images
      /// it copied to or from.
      ///
      /// The host finds the session of a render itself: its ClipInstance::getImage() override may
      /// run on the plug-in's threads (the multi-thread suite), where the GPURenderScope of the
      /// render's thread isn't seen, so the host keeps the session with its own per-render state.
      class RenderSession {
      public:
        /// mode: Buffers or Textures (chooseRenderMode()). The session is ok() only if the plug-in
        /// and the host descriptor (ImageEffect::gImageEffectHost) both support it: "If a plug-in and
        /// host have both set kOfxImageEffectPropMetalRenderSupported="true" then the host MAY set
        /// this property" (kOfxImageEffectPropMetalEnabled; the plug-in's "needed" counts as "true"),
        /// and "only for a path that both host and plug-in declared as supported" (textures).
        /// Otherwise (another mode, no queue, a property that doesn't allow it, which is logged) the
        /// actions get kOfxImageEffectPropMetalEnabled and kOfxImageEffectPropMetalTextureEnabled 0
        /// and no queue, and nothing is copied.
        RenderSession(ImageEffect::Instance &effect, Queue &queue, RenderMode mode = RenderMode::Buffers);
        /// Waits for the plug-in's work on every buffer and texture of the session (a blit on the
        /// queue that reads and writes each, which hazard tracking runs after it), whether
        /// readOutput() ran or not, then releases them, and deletes the images the plug-in didn't
        /// release. A plug-in releases its images (clipReleaseImage) while its command buffers may
        /// still use them, and for textures "The host MUST keep them alive until the work the
        /// plug-in enqueued on the command queue has completed": the session keeps them until then.
        ~RenderSession();
        RenderSession(const RenderSession &) = delete;
        RenderSession &operator=(const RenderSession &) = delete;

        bool ok() const { return _ok; }
        RenderMode mode() const { return _mode; }
        Queue &queue() const { return _queue; }

        /// Sets the output of a render, before its actions: a Metal buffer or texture of image's bounds that
        /// getImage() returns for clip. Only renderWindow (within the bounds) is copied to it, so
        /// that the pixels the plug-in doesn't write keep the host's values, and readOutput() copies
        /// only renderWindow back to image: the plug-in renders "the window (in PixelCoordinates) to
        /// render" (kOfxImageEffectPropRenderWindow), and the host's other pixels stay as they are
        /// (another render may be writing them). image must outlive the session. Another call,
        /// between renders, replaces it (a render of another frame). nullptr if it fails (then
        /// failed()).
        ImageEffect::Image *setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                      const OfxRectI &renderWindow);
        /// waits for the plug-in's writes to the output buffer, since "The plug-in SHOULD NOT wait
        /// for final asynchronous operations to complete before returning from the render action"
        /// (kOfxImageEffectPropMetalCommandQueue): a blit on the queue that reads the buffer, which
        /// Metal's hazard tracking runs after them (see ofxhMetalRender.mm); then copies the output's
        /// render window to the host's output image. false if that fails (then failed()).
        bool readOutput();

        /// ClipInstance::getImage() during the session: for the output clip, the output
        /// (setOutput(); image is ignored); for an input clip, image (what the host returns for a
        /// CPU render, not consumed: the caller releases it) copied to a new buffer or texture. Each result is
        /// a new reference, for clipReleaseImage. nullptr if image is nullptr, and if the copy
        /// fails (then failed(): a Metal error, an empty image, an unknown depth or components, RGB
        /// as a texture).
        /// Thread-safe while the output doesn't change (clipGetImage on the plug-in's threads
        /// during the actions); setOutput() and readOutput() aren't.
        ImageEffect::Image *getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image);

        /// an image couldn't be copied to or from Metal: drop this output, or render it again on
        /// the CPU (cpuRetryAllowed())
        bool failed() const;
        /// the images not released (clipReleaseImage) yet, the output included (a diagnostic)
        int liveImages() const;

      private:
        class Handle;
        friend class Handle;
        /// a new buffer or texture (in the session's mode) of image's bounds, with rect's pixels
        /// copied to it; output: for setOutput() (a texture the plug-in writes)
        Handle *copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image, const OfxRectI &rect, bool output);

        Queue &_queue;
        RenderMode _mode;
        bool _ok;
        ImageEffect::GPURenderScope _scope;
        struct Resources;                             ///< (Objective-C++: ofxhMetalRender.mm)
        std::unique_ptr<Resources> _resources;        ///< every buffer and texture of the session, until it ends
        std::unique_ptr<GPU::SessionImages> _images;  ///< the images, the output, failed()
      };

    } // Metal
  } // Host
} // OFX

#endif // OFXH_METAL_RENDER_H
