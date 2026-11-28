#ifndef OFXH_CUDA_RENDER_H
#define OFXH_CUDA_RENDER_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhCUDARender.h

  CUDA renders of HostSupport effect instances (ofxGPURender.h, OFX 1.5), on the devices and
  streams of ofxhCUDADevice.h. Part of openfx::HostSupportCUDA.

  - chooseRenderMode(): the CPU or CUDA, from kOfxImageEffectPropCudaRenderSupported and
    kOfxImageEffectPropCPURenderSupported, and the CPU retry after a failed render;
  - RenderSession: the render actions of one render (Begin/Render/EndSequenceRender) with
    kOfxImageEffectPropCudaEnabled 1, and kOfxImageEffectPropCudaStream if the plug-in and the
    host both support CUDA streams (an ImageEffect::GPURenderScope), with the device's primary
    context current on the thread; and the images the host's ClipInstance::getImage() returns
    meanwhile: the CPU images it would return, copied to device memory ("1 indicates that the
    kOfxImagePropData of each image of each clip is a CUDA memory pointer",
    kOfxImageEffectPropCudaEnabled); the output's render window is copied back to the host's
    output image.

  The device images have the bounds, depth and components of the CPU images, with no conversion,
  packed (kOfxImagePropRowBytes is the width times the pixel size), in memory of cuMemAlloc. Their
  rows are bottom up, as in CPU memory: OFX doesn't define the row order of CUDA memory
  (kOfxImagePropRowBytes may be negative "for CPU images only, not CUDA/Metal/OpenCL Buffers"), so
  this is this library's choice. The copies are on the session's stream, after the plug-in's
  work there (or on the legacy default stream without one), and waited for (cuMemcpy2DAsync,
  cuStreamSynchronize).

  A host's render with CUDA, one stream per render:

    if (chooseRenderMode(effect, hostCUDA) == RenderMode::CUDA) {
      Stream stream(device);
      RenderSession session(effect, stream);
      session.setOutput(*outputClip, *outputImage, renderWindow);  // the host's output image, before the actions
      // the host's ClipInstance::getImage() override returns session.getImage(clip, image) during:
      st = effect.renderAction(...);
      if (st == kOfxStatOK) session.readOutput();
      if (cpuRetryAllowed(effect, session, st)) ... render again on the CPU (after the session)
    }
*/

#include "ofxhClip.h"
#include "ofxhCUDADevice.h"
#include "ofxhImageEffect.h"

#include <memory>

namespace OFX {
  namespace Host {
    namespace GPU { class SessionImages; } // (internal: src/gpu/ofxhGPUImages.h)

    namespace CUDA {

      /// how to render an instance
      enum class RenderMode {
        CPU,   ///< kOfxImageEffectPropCudaEnabled 0
        CUDA,  ///< kOfxImageEffectPropCudaEnabled 1, images in CUDA device memory
        None   ///< the plug-in can't render on the CPU, and can't use the host's CUDA
      };

      /// The mode of the next render of effect, for a host that can pass its images now in CUDA
      /// memory (hostCUDA: it has a Device that is usable(), and its
      /// kOfxImageEffectPropCudaRenderSupported is "true"): CUDA if the plug-in can too (its kOfxImageEffectPropCudaRenderSupported is
      /// "true", or "needed", a value of upstream main's metadata); else the CPU if
      /// ImageEffect::canRenderOnCPU() (kOfxImageEffectPropCPURenderSupported alone: "needed" says
      /// nothing about the CPU here); else None.
      /// Also the retry after a failed CUDA render: chooseRenderMode(effect, false).
      /// Reads the instance properties, which the plug-in may change in kOfxActionInstanceChanged.
      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostCUDA);

      class RenderSession;

      /// After a CUDA action of session that returned st: whether CUDA failed: the session failed
      /// (RenderSession::failed()), or ImageEffect::gpuRenderFailed(st), the rule of the other
      /// GPU retries too.
      bool cudaFailed(const RenderSession &session, OfxStatus st);

      /// After a CUDA action of effect in session that returned st: the host may render it again
      /// on the CPU. CUDA failed (cudaFailed()), and ImageEffect::canRenderOnCPU(effect)
      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st);

      /// The render actions of one CUDA render of effect on stream's device, sent by the thread that
      /// creates the session while it lives (an ImageEffect::GPURenderScope, and a ContextScope),
      /// and its images. An automatic object: destroy it on its thread, before the stream, and
      /// before the CPU images it copied to or from.
      ///
      /// The host finds the session of a render itself: its ClipInstance::getImage() override may
      /// run on the plug-in's threads (the multi-thread suite), where the GPURenderScope of the
      /// render's thread isn't seen, so the host keeps the session with its own per-render state.
      /// The primary context is current on the session's thread only. A host that runs the
      /// multi-thread suite's functions on threads of its own opens a ContextScope on each of them
      /// meanwhile (see ContextScope): the plug-in isn't told the host's device, and on a thread
      /// with no current context the CUDA runtime uses device 0.
      class RenderSession {
      public:
        /// The session is ok() only if the plug-in and the host descriptor
        /// (ImageEffect::gImageEffectHost) both support CUDA: "If a plug-in and host have both set
        /// kOfxImageEffectPropCudaRenderSupported="true" then the host MAY set this property"
        /// (kOfxImageEffectPropCudaEnabled; the plug-in's "needed" counts as "true"), the device is
        /// usable() (no sticky error), and its primary context could be made current (pushed by the
        /// session, popped when it ends: current during the actions on this thread); then, if
        /// ImageEffect::cudaStreamsSupported(effect)
        /// (the host descriptor and the plug-in support CUDA streams), it opens stream
        /// (Stream::open()) and passes it, and isn't ok() if that fails. Otherwise (which is logged) the
        /// actions get kOfxImageEffectPropCudaEnabled 0 and no stream, and nothing is copied.
        RenderSession(ImageEffect::Instance &effect, Stream &stream);
        /// Waits for the stream (if any), whether readOutput() ran or not, then frees the device
        /// memory, and deletes the images the plug-in didn't release. A plug-in releases its images
        /// (clipReleaseImage) while its work on the stream may still use them: the session keeps
        /// every allocation until then.
        ~RenderSession();
        RenderSession(const RenderSession &) = delete;
        RenderSession &operator=(const RenderSession &) = delete;

        bool ok() const { return _ok; }
        Stream &stream() const { return _stream; }

        /// Sets the output of a render, before its actions: device memory of image's bounds that
        /// getImage() returns for clip. Only renderWindow (within the bounds) is copied to it, so
        /// that the pixels the plug-in doesn't write keep the host's values, and readOutput() copies
        /// only renderWindow back to image: the plug-in renders "the window (in PixelCoordinates) to
        /// render" (kOfxImageEffectPropRenderWindow), and the host's other pixels stay as they are
        /// (another render may be writing them). image must outlive the session. Another call,
        /// between renders, replaces it (a render of another frame). nullptr if it fails (then
        /// failed()).
        ImageEffect::Image *setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                      const OfxRectI &renderWindow);
        /// waits for the stream, if the actions had one, since then "the plug-in SHOULD NOT wait
        /// for final asynchronous operations to complete before returning from the render action"
        /// (kOfxImageEffectPropCudaStream); without one, "the plug-in SHOULD ensure that any
        /// asynchronous operations it enqueues have completed before returning". Then copies the
        /// output's render window to the host's output image. false if that fails (then failed()).
        bool readOutput();

        /// ClipInstance::getImage() during the session: for the output clip, the output
        /// (setOutput(); image is ignored); for an input clip, image (what the host returns for a
        /// CPU render, not consumed: the caller releases it) copied to new device memory. Each
        /// result is a new reference, for clipReleaseImage. nullptr if image is nullptr, and if the
        /// copy fails (then failed(): a CUDA error, an empty image, an unknown depth or
        /// components). Thread-safe while the output doesn't change (clipGetImage on the plug-in's
        /// threads during the actions); setOutput() and readOutput() aren't.
        ImageEffect::Image *getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image);

        /// an image couldn't be copied to or from CUDA, or the stream failed: drop this output, or
        /// render it again on the CPU (cpuRetryAllowed())
        bool failed() const;
        /// the session's failure as an OFX status: kOfxStatOK if not failed(); kOfxStatGPUOutOfMemory
        /// if its first CUDA error was CUDA_ERROR_OUT_OF_MEMORY ("GPU render ran out of memory");
        /// else kOfxStatGPURenderFailed ("GPU render failed in a non-memory-related way")
        OfxStatus status() const;
        /// the images not released (clipReleaseImage) yet, the output included (a diagnostic)
        int liveImages() const;

      private:
        class Handle;
        friend class Handle;
        /// new device memory of image's bounds, with rect's pixels copied to it
        Handle *copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image, const OfxRectI &rect);
        /// copies the pixels of rect (within image's bounds) between image and memory, device memory
        /// of image's bounds; a CUresult
        int transfer(unsigned long long memory, const ImageEffect::Image &image, const OfxRectI &rect, bool toDevice);

        Stream &_stream;
        ContextScope _context;
        bool _ok;
        ImageEffect::GPURenderScope _scope;
        struct Allocations;
        std::unique_ptr<Allocations> _allocations;    ///< every allocation of the session, until it ends
        std::unique_ptr<GPU::SessionImages> _images;  ///< the images, the output, failed()
      };

    } // CUDA
  } // Host
} // OFX

#endif // OFXH_CUDA_RENDER_H
