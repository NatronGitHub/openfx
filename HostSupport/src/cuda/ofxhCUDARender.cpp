// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhCUDARender.h: the render mode, and the images of a CUDA render.

#include "ofxhCUDARender.h"
#include "ofxhCUDAFunctions.h"
#include "../gpu/ofxhGPUImages.h"

#include "ofxGPURender.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace CUDA {

      using namespace Detail;
      using GPU::PixelLayout;

      /// the plug-in's prop is "true", or "needed", a value of upstream main's @propdef (values
      /// "false", "true", needed)
      static bool pluginSupports(const ImageEffect::Instance &effect, const char *prop)
      {
        const std::string &v = effect.getProps().getStringProperty(prop);
        return v == "true" || v == "needed";
      }

      /// the host descriptor's prop is "true"
      static bool hostSupports(const char *prop)
      {
        return ImageEffect::gImageEffectHost && ImageEffect::gImageEffectHost->getProperties().getStringProperty(prop) == "true";
      }

      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostCUDA)
      {
        if (hostCUDA && pluginSupports(effect, kOfxImageEffectPropCudaRenderSupported)) {
          return RenderMode::CUDA;
        }
        return ImageEffect::canRenderOnCPU(effect) ? RenderMode::CPU : RenderMode::None;
      }

      bool cudaFailed(const RenderSession &session, OfxStatus st)
      {
        return session.failed() || ImageEffect::gpuRenderFailed(st);
      }

      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st)
      {
        return cudaFailed(session, st) && ImageEffect::canRenderOnCPU(effect);
      }

      /// kOfxImageEffectPropCudaEnabled may be 1: "If a plug-in and host have both set
      /// kOfxImageEffectPropCudaRenderSupported="true" then the host MAY set this property"; with
      /// the context current, and a stream if both support streams (opened here: "This property
      /// will only be set if the host and plug-in both support CUDA streams",
      /// kOfxImageEffectPropCudaStream). Logs why not.
      static bool sessionAllowed(const ImageEffect::Instance &effect, Stream &stream, const ContextScope &context)
      {
        const char *prop = kOfxImageEffectPropCudaRenderSupported;
        std::string why;
        if (!pluginSupports(effect, prop)) {
          why = std::string("the plug-in's ") + prop + " is \"" + effect.getProps().getStringProperty(prop) + "\"";
        } else if (!hostSupports(prop)) {
          why = std::string("the host's ") + prop + " isn't \"true\"";
        } else if (!stream.device().usable()) {
          why = "the device's context is unusable after a sticky error";
        } else if (!context.ok()) {
          why = "the device's context: " + context.error();
        } else if (ImageEffect::cudaStreamsSupported(effect) && !stream.open()) {
          why = "no stream: " + stream.error();
        }
        if (!why.empty()) {
          stream.device().log(LogLevel::Error, "CUDA: no CUDA render (kOfxImageEffectPropCudaEnabled 0): " + why);
        }
        return why.empty();
      }

      ////////////////////////////////////////////////////////////////////////////////
      // RenderSession

      /// every allocation the session made: a plug-in releases its images (clipReleaseImage) while
      /// its work on the stream may still use them
      struct RenderSession::Allocations {
        std::mutex mutex;
        std::vector<CUdeviceptr> all;
        std::atomic<int> error{ kCudaSuccess };  ///< the first CUDA error of the session (status())
      };

      /// err, a CUDA error of session's (the first is kept for status()), through Device::checkError()
      static void noteError(std::atomic<int> &first, const Device &device, CUresult err)
      {
        device.checkError(err);
        int none = kCudaSuccess;
        first.compare_exchange_strong(none, err);
      }

      /// an image of the session: some of its device memory
      class RenderSession::Handle : public ImageEffect::Image {
      public:
        Handle(RenderSession &session, ImageEffect::ClipInstance &clip, const ImageEffect::Image &image,
               CUdeviceptr memory, int rowBytes)
          : ImageEffect::Image(clip,
                               image.getDoubleProperty(kOfxImageEffectPropRenderScale, 0),
                               image.getDoubleProperty(kOfxImageEffectPropRenderScale, 1),
                               reinterpret_cast<void *>(static_cast<uintptr_t>(memory)), image.getBounds(),
                               image.getROD(), rowBytes, image.getStringProperty(kOfxImagePropField),
                               image.getStringProperty(kOfxImagePropUniqueIdentifier))
          , _images(*session._images)
          , _memory(memory)
        {
          GPU::copyImageProperties(*this, image);
          _images.add(this);
        }

        ~Handle() override
        {
          // (the session keeps the memory until it ends: RenderSession::Allocations)
          _images.remove(this);
        }

        CUdeviceptr memory() const { return _memory; }

      private:
        /// (not through the session: its unique_ptr is null while ~SessionImages deletes the images left)
        GPU::SessionImages &_images;
        CUdeviceptr _memory;
      };

      RenderSession::RenderSession(ImageEffect::Instance &effect, Stream &stream)
        : _stream(stream)
        , _context(stream.device())
        , _ok(sessionAllowed(effect, stream, _context))
        , _scope(effect, _ok ? ImageEffect::GPURenderScope::API::CUDA : ImageEffect::GPURenderScope::API::CPU,
                 _ok ? stream.native() : nullptr)
        , _allocations(new Allocations)
        , _images(new GPU::SessionImages("CUDA", [&stream](LogLevel level, const std::string &message) {
            stream.device().log(level, message);
          }))
      {
      }

      RenderSession::~RenderSession()
      {
        // the plug-in's work on the stream first, whatever happened (readOutput() may not have
        // run); then the memory is freed (_images then deletes the images not released)
        std::vector<CUdeviceptr> all;
        {
          std::lock_guard<std::mutex> lock(_allocations->mutex);
          all.swap(_allocations->all);
        }
        try {
          // (also after an aborted render: CUDA can't cancel the work enqueued)
          if (!_stream.synchronize()) {
            _images->log(LogLevel::Error, "waiting for the render's stream (cuStreamSynchronize) before freeing its memory");
          }
          const Device &device = _stream.device();
          // (a sticky error may show only here, e.g. without a stream: the device becomes unusable)
          device.withContext([&] {
            for (CUdeviceptr p : all) {
              device.checkError(device.cu().MemFree(p));
            }
            return kCudaSuccess;
          });
        } catch (...) {
          // (Hooks::driverCall or the host's logger threw: the memory is leaked; C.36)
        }
      }

      bool RenderSession::failed() const
      {
        return _images->failed();
      }

      OfxStatus RenderSession::status() const
      {
        if (!failed()) {
          return kOfxStatOK;
        }
        return _allocations->error == kCudaErrorOutOfMemory ? kOfxStatGPUOutOfMemory : kOfxStatGPURenderFailed;
      }

      int RenderSession::liveImages() const
      {
        return _images->liveImages();
      }

      int RenderSession::transfer(unsigned long long memory, const ImageEffect::Image &image, const OfxRectI &rect,
                                  bool toDevice)
      {
        const PixelLayout l(image);
        GPU::StagedRect staged(l, rect, toDevice);
        if (staged.empty()) {
          return kCudaSuccess;
        }
        const OfxRectI &r = staged.r;
        const size_t rowSize = staged.rowSize;
        std::vector<char> &staging = staged.rows;
        // the device memory is packed: (width * pixel bytes) per row
        CUDA_MEMCPY2D params = {};
        const size_t x = static_cast<size_t>(r.x1 - l.bounds.x1) * static_cast<size_t>(l.pixelBytes);
        const size_t y = static_cast<size_t>(r.y1 - l.bounds.y1);
        if (toDevice) {
          params.srcMemoryType = kCuMemoryTypeHost;
          params.srcHost = staging.data();
          params.srcPitch = rowSize;
          params.dstMemoryType = kCuMemoryTypeDevice;
          params.dstDevice = memory;
          params.dstXInBytes = x;
          params.dstY = y;
          params.dstPitch = l.packedRow();
        } else {
          params.srcMemoryType = kCuMemoryTypeDevice;
          params.srcDevice = memory;
          params.srcXInBytes = x;
          params.srcY = y;
          params.srcPitch = l.packedRow();
          params.dstMemoryType = kCuMemoryTypeHost;
          params.dstHost = staging.data();
          params.dstPitch = rowSize;
        }
        params.WidthInBytes = rowSize;
        params.Height = static_cast<size_t>(staged.height());
        // on the session's stream (after the plug-in's work there), or the legacy default stream
        // without one; then waited for: the staging memory goes, and with a non-blocking stream a
        // copy on the default stream wouldn't be ordered with the plug-in's work
        const Device &device = _stream.device();
        CUstream stream = static_cast<CUstream>(_stream.native());
        CUresult err = device.withContext([&] {
          CUresult e = device.cu().Memcpy2DAsync(&params, stream);
          const CUresult s = device.cu().StreamSynchronize(stream);
          return e != kCudaSuccess ? e : s;
        });
        noteError(_allocations->error, device, err);
        if (err == kCudaSuccess && !toDevice) {
          staged.unstage(l);
        }
        return err;
      }

      RenderSession::Handle *RenderSession::copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image,
                                                 const OfxRectI &rect)
      {
        if (!_ok) {
          _images->fail("no CUDA render in this session (see its constructor)");
          return nullptr;
        }
        const PixelLayout l(image);
        if (!_images->copyable(l, "copied to CUDA memory")) {
          return nullptr;
        }
        const int w = l.width(), h = l.height();
        const size_t bytes = l.packedRow() * static_cast<size_t>(h);
        const Device &device = _stream.device();
        const std::string what = std::to_string(w) + "x" + std::to_string(h) + " " + l.componentsName + " " + l.depth;
        CUdeviceptr memory = 0;
        CUresult err = device.withContext([&] { return device.cu().MemAlloc(&memory, bytes); });
        noteError(_allocations->error, device, err);
        if (err != kCudaSuccess) {
          _images->fail("cuMemAlloc of " + std::to_string(bytes) + " bytes (" + what + "): " + errorName(err));
          return nullptr;
        }
        {
          std::lock_guard<std::mutex> lock(_allocations->mutex);
          _allocations->all.push_back(memory);
        }
        err = transfer(memory, image, rect, /*toDevice=*/true);
        if (err != kCudaSuccess) {
          _images->fail("copying " + what + " to CUDA memory (cuMemcpy2DAsync): " + errorName(err));
          return nullptr;
        }
        return new Handle(*this, clip, image, memory, static_cast<int>(l.packedRow()));
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
        if (!_stream.synchronize()) {
          noteError(_allocations->error, _stream.device(), _stream.lastError());
          _images->fail("the render's work on its stream failed (cuStreamSynchronize)");
          return false;
        }
        ImageEffect::Image &outputImage = *_images->outputImage();
        if (!outputImage.getPointerProperty(kOfxImagePropData)) {
          _images->fail("an output image without pixel data");
          return false;
        }
        // the render window only: the plug-in renders "the window (in PixelCoordinates) to render"
        // (kOfxImageEffectPropRenderWindow), and the host's other pixels stay as they are
        const CUresult err = transfer(static_cast<Handle *>(_images->output())->memory(), outputImage,
                                      _images->renderWindow(), /*toDevice=*/false);
        if (err != kCudaSuccess) {
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

    } // CUDA
  } // Host
} // OFX
