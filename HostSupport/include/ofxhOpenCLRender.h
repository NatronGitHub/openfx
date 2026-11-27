#ifndef OFXH_OPENCL_RENDER_H
#define OFXH_OPENCL_RENDER_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhOpenCLRender.h

  OpenCL renders of HostSupport effect instances (ofxGPURender.h, OFX 1.5), on the devices and
  queues of ofxhOpenCLDevice.h. Part of openfx::HostSupportOpenCL.

  - chooseRenderMode(): the CPU, OpenCL Buffers or OpenCL Images, from
    kOfxImageEffectPropOpenCLRenderSupported (Buffers), kOfxImageEffectPropOpenCLSupported
    (Images) and kOfxImageEffectPropCPURenderSupported, and the CPU retry after a failed render;
  - RenderSession: the render actions of one render (Begin/Render/EndSequenceRender) with
    kOfxImageEffectPropOpenCLEnabled 1 and kOfxImageEffectPropOpenCLCommandQueue (an
    ImageEffect::GPURenderScope), and the images the host's ClipInstance::getImage() returns
    meanwhile: the CPU images it would return, copied to OpenCL buffers ("When rendering using
    OpenCL Buffers, the cl_mem of the buffers are retrieved using kOfxImagePropData") or images
    ("When rendering using OpenCL Images, the cl_mem of the images are retrieved using
    kOfxImageEffectPropOpenCLImage"); the output is copied back to the host's output image.

  The images have the bounds, depth and components of the CPU images, with no conversion. Their
  rows are bottom up, as in CPU memory: OFX doesn't define the row order of OpenCL buffers and
  images (kOfxImagePropRowBytes may be negative "for CPU images only, not CUDA/Metal/OpenCL
  Buffers", and "Row bytes is not supported for OpenCL Images"), so this is this library's
  choice. Buffers are packed (kOfxImagePropRowBytes is the width times the pixel size); OpenCL
  Images are 2D images of channel order CL_RGBA, of channel type
  CL_UNORM_INT8, CL_UNORM_INT16, CL_HALF_FLOAT or CL_FLOAT (byte, short, half, float). The OpenCL 1.1
  minimum list of image formats (5.3.2.1) has CL_RGBA with these four types, and CL_BGRA with
  CL_UNORM_INT8 only: CL_RGBA is the one channel order in OFX's RGBA order for every depth. OFX
  doesn't say how other components map to OpenCL
  Images, so chooseRenderMode() uses Images only when the clips are RGBA (Buffers or the CPU
  otherwise); the session also refuses another image (it fails, and the host may render on the
  CPU).

  A host's render with OpenCL, one queue per render:

    const RenderMode mode = chooseRenderMode(effect, hostBuffers, hostImages);
    if (mode == RenderMode::Buffers || mode == RenderMode::Images) {
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
#include "ofxhOpenCLDevice.h"

#include <memory>

namespace OFX {
  namespace Host {
    namespace GPU { class SessionImages; } // (internal: src/gpu/ofxhGPUImages.h)

    namespace OpenCL {

      /// how to render an instance
      enum class RenderMode {
        CPU,      ///< kOfxImageEffectPropOpenCLEnabled 0
        Buffers,  ///< kOfxImageEffectPropOpenCLEnabled 1, images in OpenCL buffers
        Images,   ///< kOfxImageEffectPropOpenCLEnabled 1, images in OpenCL Images
        None      ///< the plug-in can't render on the CPU, and can't use the host's OpenCL
      };

      /// The mode of the next render of effect, for a host that can pass its images now as OpenCL
      /// buffers (hostBuffers: it has a Device, and its kOfxImageEffectPropOpenCLRenderSupported is
      /// "true") and Images (hostImages: likewise with kOfxImageEffectPropOpenCLSupported, and the
      /// device's DeviceInfo::imageSupport). Buffers or Images if the plug-in can too (its property
      /// is "true"; for Buffers also "needed", a value of upstream main's metadata), Images only if
      /// the output and the connected inputs are RGBA (the clips' components: the session's
      /// OpenCL Images are CL_RGBA); if both, Images if preferImages ("If both host and plug-in
      /// support both, it is up to the host which it uses"); else the CPU if ImageEffect::canRenderOnCPU() (kOfxImageEffectPropCPURenderSupported
      /// alone: "needed" says nothing about the CPU here); else None.
      /// Also the retry after a failed OpenCL render: chooseRenderMode(effect, false, false).
      /// Reads the instance properties, which the plug-in may change in kOfxActionInstanceChanged.
      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostBuffers, bool hostImages,
                                  bool preferImages = false);

      class RenderSession;

      /// The command queue (a cl_command_queue) of the innermost OpenCL render of the calling
      /// thread (an ImageEffect::GPURenderScope of API OpenCL, e.g. a RenderSession's), nullptr if
      /// none: the queue a host's compileOpenCLProgram() passes to Device::compileProgram(), since
      /// OfxOpenCLProgramSuiteV1::compileProgram doesn't say which context to build for. On the
      /// threads of the host's multi-thread suite, the host passes the scope on (GPURenderScope).
      void *currentQueue();

      /// After an OpenCL action of session that returned st: whether OpenCL failed: the session
      /// failed (RenderSession::failed()), or ImageEffect::gpuRenderFailed(st), the rule of the
      /// OpenGL retry too. "If a call using OpenCL rendering fails, the host may re-attempt using
      /// CPU buffers instead, but this is not required" (ofxGPURender.h).
      bool openCLFailed(const RenderSession &session, OfxStatus st);

      /// After an OpenCL action of effect in session that returned st: the host may render it again
      /// on the CPU. OpenCL failed (openCLFailed()), and ImageEffect::canRenderOnCPU(effect)
      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st);

      /// The render actions of one OpenCL render of effect on queue, sent by the thread that
      /// creates the session while it lives (an ImageEffect::GPURenderScope), and its images.
      /// An automatic object: destroy it on its thread, before the queue, and before the CPU images
      /// it copied to or from.
      ///
      /// The host finds the session of a render itself: its ClipInstance::getImage() override may
      /// run on the plug-in's threads (the multi-thread suite), where the GPURenderScope of the
      /// render's thread isn't seen, so the host keeps the session with its own per-render state.
      class RenderSession {
      public:
        /// mode: Buffers or Images (chooseRenderMode()). The session is ok() only if the plug-in
        /// and the host descriptor (ImageEffect::gImageEffectHost) both support it: "If a plug-in
        /// and host have both set kOfxImageEffectPropOpenCLRenderSupported="true" or have both set
        /// kOfxImageEffectPropOpenCLSupported="true" then the host MAY set this property"
        /// (kOfxImageEffectPropOpenCLEnabled; for Buffers, the plug-in's "needed" counts as "true").
        /// Otherwise (another mode, no queue, a property that doesn't allow it, which is logged)
        /// the actions get kOfxImageEffectPropOpenCLEnabled 0 and no queue, and nothing is copied.
        RenderSession(ImageEffect::Instance &effect, Queue &queue, RenderMode mode);
        /// deletes the images the plug-in didn't release
        ~RenderSession();
        RenderSession(const RenderSession &) = delete;
        RenderSession &operator=(const RenderSession &) = delete;

        bool ok() const { return _ok; }
        RenderMode mode() const { return _mode; }
        Queue &queue() const { return _queue; }

        /// Sets the output of a render, before its actions: an OpenCL buffer or image of image's
        /// bounds, in the session's mode, that getImage() returns for clip. Only renderWindow (within
        /// the bounds) is copied to it, so that the pixels the plug-in doesn't write keep the host's
        /// values, and readOutput() copies only renderWindow back to image: the plug-in renders "the
        /// window (in PixelCoordinates) to render" (kOfxImageEffectPropRenderWindow), and the host's
        /// other pixels stay as they are (another render may be writing them). image must outlive the
        /// session. Another call, between renders, replaces it (a render of another frame). nullptr if
        /// it fails (then failed()).
        ImageEffect::Image *setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                      const OfxRectI &renderWindow);
        /// waits for the queue (clFinish), and copies the output to the host's output image;
        /// false if that fails (then failed())
        bool readOutput();

        /// ClipInstance::getImage() during the session: for the output clip, the output
        /// (setOutput(); image is ignored); for an input clip, image (what the host returns for a
        /// CPU render, not consumed: the caller releases it) copied to a new buffer or image.
        /// Each result is a new reference, for clipReleaseImage. nullptr if image is nullptr, and
        /// if the copy fails (then failed(): an OpenCL error, an empty image, components that
        /// can't be an OpenCL Image). Thread-safe while the output doesn't change (clipGetImage on
        /// the plug-in's threads during the actions); setOutput() and readOutput() aren't.
        ImageEffect::Image *getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image);

        /// an image couldn't be copied to or from OpenCL: drop this output, or render it again
        /// on the CPU (cpuRetryAllowed())
        bool failed() const;
        /// the images not released (clipReleaseImage) yet, the output included (a diagnostic)
        int liveImages() const;

      private:
        class Handle;
        friend class Handle;
        /// a new buffer or image of image's bounds, with rect's pixels copied to it
        Handle *copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image, const OfxRectI &rect);
        /// copies the pixels of rect (within image's bounds) between image and mem, an OpenCL buffer
        /// or image of image's bounds; a cl_int status
        int transfer(void *mem, const ImageEffect::Image &image, const OfxRectI &rect, bool write);

        Queue &_queue;
        RenderMode _mode;
        bool _ok;
        ImageEffect::GPURenderScope _scope;
        std::unique_ptr<GPU::SessionImages> _images;  ///< the images, the output, failed()
      };

    } // OpenCL
  } // Host
} // OFX

#endif // OFXH_OPENCL_RENDER_H
