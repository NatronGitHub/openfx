// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhOpenCLRender.h: the render mode, and the images of an OpenCL render.

#include "ofxhOpenCLRender.h"
#include "ofxhOpenCLFunctions.h"
#include "../gpu/ofxhGPUImages.h"

#include "ofxGPURender.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace OpenCL {

      using namespace Detail;
      using GPU::PixelLayout;
      using GPU::componentCount;

      /// the plug-in supports OpenCL Buffers: kOfxImageEffectPropOpenCLRenderSupported "true" - "the
      /// host or plug-in can support OpenCL Buffers render" (ofxGPURender.h), or "needed", a value
      /// of upstream main's @propdef (values "false", "true", needed)
      static bool pluginBuffers(const ImageEffect::Instance &effect)
      {
        const std::string &v = effect.getProps().getStringProperty(kOfxImageEffectPropOpenCLRenderSupported);
        return v == "true" || v == "needed";
      }

      /// the plug-in supports OpenCL Images: kOfxImageEffectPropOpenCLSupported "true"
      static bool pluginImages(const ImageEffect::Instance &effect)
      {
        return effect.getProps().getStringProperty(kOfxImageEffectPropOpenCLSupported) == "true";
      }

      /// the output and the connected inputs are RGBA: OpenCL Images can be used (CL_RGBA)
      static bool allClipsRGBA(const ImageEffect::Instance &effect)
      {
        for (const auto &c : effect.getDescriptor().getClips()) {
          const ImageEffect::ClipInstance *clip = effect.getClip(c.first);
          if (clip && (clip->isOutput() || clip->getConnected()) && clip->getComponents() != kOfxImageComponentRGBA) {
            return false;
          }
        }
        return true;
      }

      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostBuffers, bool hostImages, bool preferImages)
      {
        const bool buffers = hostBuffers && pluginBuffers(effect);
        const bool images = hostImages && pluginImages(effect) && allClipsRGBA(effect);
        if (buffers || images) {
          return images && (preferImages || !buffers) ? RenderMode::Images : RenderMode::Buffers;
        }
        return ImageEffect::canRenderOnCPU(effect) ? RenderMode::CPU : RenderMode::None;
      }

      void *currentQueue()
      {
        const ImageEffect::GPURenderScope *scope = ImageEffect::GPURenderScope::current();
        return scope && scope->api() == ImageEffect::GPURenderScope::API::OpenCL ? scope->queue() : nullptr;
      }

      bool openCLFailed(const RenderSession &session, OfxStatus st)
      {
        return session.failed() || ImageEffect::gpuRenderFailed(st);
      }

      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st)
      {
        return openCLFailed(session, st) && ImageEffect::canRenderOnCPU(effect);
      }

      /// kOfxImageEffectPropOpenCLEnabled may be 1: "If a plug-in and host have both set
      /// kOfxImageEffectPropOpenCLRenderSupported="true" or have both set
      /// kOfxImageEffectPropOpenCLSupported="true" then the host MAY set this property"; with a
      /// queue. Logs why not.
      static bool sessionAllowed(const ImageEffect::Instance &effect, const Queue &queue, RenderMode mode)
      {
        if (mode != RenderMode::Buffers && mode != RenderMode::Images) {
          return false; // (a CPU render: nothing to say)
        }
        const bool buffers = mode == RenderMode::Buffers;
        const char *prop = buffers ? kOfxImageEffectPropOpenCLRenderSupported : kOfxImageEffectPropOpenCLSupported;
        std::string why;
        if (!queue.native()) {
          why = "no command queue";
        } else if (!(buffers ? pluginBuffers(effect) : pluginImages(effect))) {
          why = std::string("the plug-in's ") + prop + " is \"" + effect.getProps().getStringProperty(prop) + "\"";
        } else if (!ImageEffect::gImageEffectHost ||
                   ImageEffect::gImageEffectHost->getProperties().getStringProperty(prop) != "true") {
          why = std::string("the host's ") + prop + " isn't \"true\"";
        }
        if (!why.empty()) {
          queue.device().log(LogLevel::Error, std::string("OpenCL: no OpenCL ") + (buffers ? "Buffers" : "Images") +
                             " render (kOfxImageEffectPropOpenCLEnabled 0): " + why);
        }
        return why.empty();
      }

      /// the OpenCL Image channel type of depth (channel order CL_RGBA)
      static cl_uint channelType(const std::string &depth)
      {
        if (depth == kOfxBitDepthByte) {
          return kCLUnormInt8;
        }
        if (depth == kOfxBitDepthShort) {
          return kCLUnormInt16;
        }
        return depth == kOfxBitDepthHalf ? kCLHalfFloat : kCLFloat;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // RenderSession

      /// owns a cl_mem until a Handle does (R.12)
      struct MemDeleter {
        const Device *device;
        void operator()(_cl_mem *mem) const { device->driverCall([&] { device->cl().ReleaseMemObject(mem); }); }
      };
      typedef std::unique_ptr<_cl_mem, MemDeleter> MemPtr;

      /// an image of the session: a buffer or an OpenCL Image, which it owns
      class RenderSession::Handle : public ImageEffect::Image {
      public:
        /// takes mem once constructed
        Handle(RenderSession &session, ImageEffect::ClipInstance &clip, const ImageEffect::Image &image, MemPtr &mem,
               int rowBytes)
          : ImageEffect::Image(clip,
                               image.getDoubleProperty(kOfxImageEffectPropRenderScale, 0),
                               image.getDoubleProperty(kOfxImageEffectPropRenderScale, 1),
                               mem.get(), image.getBounds(), image.getROD(), rowBytes,
                               image.getStringProperty(kOfxImagePropField),
                               image.getStringProperty(kOfxImagePropUniqueIdentifier))
          , _session(session)
          , _images(*session._images)
          , _mem(mem.get())
        {
          GPU::copyImageProperties(*this, image);
          if (session._mode == RenderMode::Images) {
            // "when OpenCL Images are being used (kOfxImageEffectPropOpenCLSupported) the clip image
            // property kOfxImageEffectPropOpenCLImage will be set and non-null"; kOfxImagePropData,
            // "a cl_mem, if the Render action arguments includes kOfxImageEffectPropOpenCLEnabled=1",
            // is the same cl_mem
            setPointerProperty(kOfxImageEffectPropOpenCLImage, _mem);
          }
          _images.add(this);
          mem.release(); // (owned from here)
        }

        ~Handle() override
        {
          _images.remove(this);
          const Device &device = _session._queue.device();
          try {
            device.driverCall([&] { device.cl().ReleaseMemObject(_mem); });
          } catch (...) {
            // (Hooks::driverCall threw: the cl_mem is leaked; C.36)
          }
        }

        cl_mem mem() const { return _mem; }

      private:
        RenderSession &_session;
        /// (not through _session: its unique_ptr is null while ~SessionImages deletes the images left)
        GPU::SessionImages &_images;
        cl_mem _mem;
      };

      RenderSession::RenderSession(ImageEffect::Instance &effect, Queue &queue, RenderMode mode)
        : _queue(queue)
        , _mode(mode)
        , _ok(sessionAllowed(effect, queue, mode))
        , _scope(effect, _ok ? ImageEffect::GPURenderScope::API::OpenCL : ImageEffect::GPURenderScope::API::CPU,
                 queue.native())
        , _images(new GPU::SessionImages("OpenCL", [&queue](LogLevel level, const std::string &message) {
            queue.device().log(level, message);
          }))
      {
      }

      RenderSession::~RenderSession() = default; // (_images: the output, and the images not released)

      bool RenderSession::failed() const
      {
        return _images->failed();
      }

      int RenderSession::liveImages() const
      {
        return _images->liveImages();
      }

      int RenderSession::transfer(void *memory, const ImageEffect::Image &image, const OfxRectI &rect, bool write)
      {
        cl_mem mem = static_cast<cl_mem>(memory);
        const PixelLayout l(image);
        GPU::StagedRect staged(l, rect, write);
        if (staged.empty()) {
          return kCLSuccess;
        }
        const OfxRectI &r = staged.r;
        const int w = staged.width(), h = staged.height();
        const size_t rowSize = staged.rowSize;
        std::vector<char> &staging = staged.rows;
        const Device &device = _queue.device();
        const CLFunctions &cl = device.cl();
        cl_command_queue q = static_cast<cl_command_queue>(_queue.native());
        cl_int err = kCLSuccess;
        device.driverCall([&] {
          if (_mode == RenderMode::Images) {
            const size_t origin[3] = { static_cast<size_t>(r.x1 - l.bounds.x1), static_cast<size_t>(r.y1 - l.bounds.y1), 0 };
            const size_t region[3] = { static_cast<size_t>(w), static_cast<size_t>(h), 1 };
            err = write ? cl.EnqueueWriteImage(q, mem, kCLTrue, origin, region, rowSize, 0, staging.data(), 0, nullptr, nullptr)
                        : cl.EnqueueReadImage(q, mem, kCLTrue, origin, region, rowSize, 0, staging.data(), 0, nullptr, nullptr);
          } else {
            // the device buffer is packed: (width * pixel bytes) per row
            const size_t bufferRow = static_cast<size_t>(l.width()) * static_cast<size_t>(l.pixelBytes);
            const size_t bufferOrigin[3] = { static_cast<size_t>(r.x1 - l.bounds.x1) * static_cast<size_t>(l.pixelBytes),
                                             static_cast<size_t>(r.y1 - l.bounds.y1), 0 };
            const size_t hostOrigin[3] = { 0, 0, 0 };
            const size_t region[3] = { rowSize, static_cast<size_t>(h), 1 };
            err = write ? cl.EnqueueWriteBufferRect(q, mem, kCLTrue, bufferOrigin, hostOrigin, region, bufferRow, 0, rowSize, 0,
                                                    staging.data(), 0, nullptr, nullptr)
                        : cl.EnqueueReadBufferRect(q, mem, kCLTrue, bufferOrigin, hostOrigin, region, bufferRow, 0, rowSize, 0,
                                                   staging.data(), 0, nullptr, nullptr);
          }
        });
        if (err == kCLSuccess && !write) {
          staged.unstage(l);
        }
        return err;
      }

      RenderSession::Handle *RenderSession::copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image,
                                                 const OfxRectI &rect)
      {
        if (!_ok) {
          _images->fail("no OpenCL render in this session (see its constructor)");
          return nullptr;
        }
        const PixelLayout l(image);
        const int w = l.width(), h = l.height();
        if (!_images->copyable(l, "an OpenCL buffer or image")) {
          return nullptr;
        }
        const bool images = _mode == RenderMode::Images;
        if (images && l.components != 4) {
          // OFX doesn't say which OpenCL Image format other components have
          _images->fail("an image of components " + l.componentsName + " can't be an OpenCL Image (RGBA only)");
          return nullptr;
        }
        const size_t packed = static_cast<size_t>(w) * static_cast<size_t>(l.pixelBytes);
        const Device &device = _queue.device();
        const CLFunctions &cl = device.cl();
        cl_context context = static_cast<cl_context>(device.nativeContext());
        cl_int err = kCLSuccess;
        cl_mem mem = nullptr;
        device.driverCall([&] {
          if (images) {
            const cl_image_format format = { kCLRGBA, channelType(l.depth) };
            mem = cl.CreateImage2D(context, kCLMemReadWrite, &format, static_cast<size_t>(w), static_cast<size_t>(h), 0,
                                   nullptr, &err);
          } else {
            mem = cl.CreateBuffer(context, kCLMemReadWrite, packed * static_cast<size_t>(h), nullptr, &err);
          }
        });
        MemPtr owned(mem, MemDeleter{ &device });
        if (mem) {
          err = transfer(mem, image, rect, /*write=*/true);
        }
        if (!mem || err != kCLSuccess) {
          _images->fail(std::string(images ? "an OpenCL Image of " : "an OpenCL buffer of ") + std::to_string(w) + "x" +
               std::to_string(h) + " " + l.componentsName + " " + l.depth + ": " + errorName(err));
          return nullptr;
        }
        // "OpenCL Images do not have the concept of row bytes": 0 for them
        return new Handle(*this, clip, image, owned, images ? 0 : static_cast<int>(packed));
      }

      ImageEffect::Image *RenderSession::setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                                   const OfxRectI &renderWindow)
      {
        return _images->setOutput(clip, image, renderWindow, [this](ImageEffect::ClipInstance &c, const ImageEffect::Image &i,
                                                                    const OfxRectI &r) { return copy(c, i, r); });
      }

      bool RenderSession::readOutput()
      {
        if (!_images->hasOutput()) {
          return false;
        }
        if (!_queue.finish()) {
          _images->fail("the render's commands failed (clFinish)");
          return false;
        }
        ImageEffect::Image &outputImage = *_images->outputImage();
        if (!outputImage.getPointerProperty(kOfxImagePropData)) {
          _images->fail("an output image without pixel data");
          return false;
        }
        // the render window only: the plug-in renders "the window (in PixelCoordinates) to render"
        // (kOfxImageEffectPropRenderWindow), and the host's other pixels stay as they are
        const cl_int err = transfer(static_cast<Handle *>(_images->output())->mem(), outputImage, _images->renderWindow(),
                                    /*write=*/false);
        if (err != kCLSuccess) {
          _images->fail("reading the output back: " + errorName(err));
          return false;
        }
        return true;
      }

      ImageEffect::Image *RenderSession::getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image)
      {
        return _images->getImage(clip, image, [this](ImageEffect::ClipInstance &c, const ImageEffect::Image &i,
                                                     const OfxRectI &r) { return copy(c, i, r); });
      }

    } // OpenCL
  } // Host
} // OFX
