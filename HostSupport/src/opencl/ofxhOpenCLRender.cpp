// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhOpenCLRender.h: the render mode, and the images of an OpenCL render.

#include "ofxhOpenCLRender.h"
#include "ofxhOpenCLFunctions.h"

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

      /// the bytes of a component of depth, 0 if unknown
      static int componentBytes(const std::string &depth)
      {
        if (depth == kOfxBitDepthByte) {
          return 1;
        }
        if (depth == kOfxBitDepthShort || depth == kOfxBitDepthHalf) {
          return 2;
        }
        if (depth == kOfxBitDepthFloat) {
          return 4;
        }
        return 0;
      }

      /// the number of components, 0 if unknown
      static int componentCount(const std::string &components)
      {
        if (components == kOfxImageComponentRGBA) {
          return 4;
        }
        if (components == kOfxImageComponentRGB) {
          return 3;
        }
        if (components == kOfxImageComponentAlpha) {
          return 1;
        }
        return 0;
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
          , _mem(mem.get())
        {
          // the CPU image's, which the copy keeps
          setStringProperty(kOfxImageEffectPropPixelDepth, image.getStringProperty(kOfxImageEffectPropPixelDepth));
          setStringProperty(kOfxImageEffectPropComponents, image.getStringProperty(kOfxImageEffectPropComponents));
          setStringProperty(kOfxImageEffectPropPreMultiplication, image.getStringProperty(kOfxImageEffectPropPreMultiplication));
          setDoubleProperty(kOfxImagePropPixelAspectRatio, image.getDoubleProperty(kOfxImagePropPixelAspectRatio));
          if (session._mode == RenderMode::Images) {
            // "when OpenCL Images are being used (kOfxImageEffectPropOpenCLSupported) the clip image
            // property kOfxImageEffectPropOpenCLImage will be set and non-null"; kOfxImagePropData,
            // "a cl_mem, if the Render action arguments includes kOfxImageEffectPropOpenCLEnabled=1",
            // is the same cl_mem
            setPointerProperty(kOfxImageEffectPropOpenCLImage, _mem);
          }
          std::lock_guard<std::mutex> lock(_session._mutex);
          _session._handles.insert(this);
          mem.release(); // (owned from here)
        }

        ~Handle() override
        {
          {
            std::lock_guard<std::mutex> lock(_session._mutex);
            _session._handles.erase(this);
          }
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
        cl_mem _mem;
      };

      RenderSession::RenderSession(ImageEffect::Instance &effect, Queue &queue, RenderMode mode)
        : _queue(queue)
        , _mode(mode)
        , _ok(sessionAllowed(effect, queue, mode))
        , _scope(effect, _ok ? ImageEffect::GPURenderScope::API::OpenCL : ImageEffect::GPURenderScope::API::CPU,
                 queue.native())
      {
      }

      RenderSession::~RenderSession()
      {
        if (_output) {
          _output->releaseReference();
        }
        std::vector<Handle *> left;
        {
          std::lock_guard<std::mutex> lock(_mutex);
          left.assign(_handles.begin(), _handles.end());
        }
        // "image handle to be disposed of by clipReleaseImage before the action returns"
        // (clipGetImage): the handles left are the plug-in's leaks
        if (!left.empty()) {
          _queue.device().log(LogLevel::Warning, "OpenCL: the plug-in didn't release " + std::to_string(left.size()) +
                              " image(s) with clipReleaseImage");
        }
        for (Handle *h : left) {
          delete h;
        }
      }

      bool RenderSession::failed() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _failed;
      }

      int RenderSession::liveImages() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return static_cast<int>(_handles.size());
      }

      void RenderSession::fail(const std::string &why)
      {
        {
          std::lock_guard<std::mutex> lock(_mutex);
          _failed = true;
        }
        _queue.device().log(LogLevel::Error, "OpenCL: " + why);
      }

      /// the pixel layout of an image: its bounds, pixel size and CPU pixels
      struct PixelLayout {
        OfxRectI bounds;
        int pixelBytes;  ///< 0: depth or components unknown
        int components;
        char *data;      ///< the pixel at (bounds.x1, bounds.y1)
        int rowBytes;    ///< may be negative
        std::string depth, componentsName;

        explicit PixelLayout(const ImageEffect::Image &image)
          : bounds(image.getBounds())
          , depth(image.getStringProperty(kOfxImageEffectPropPixelDepth))
          , componentsName(image.getStringProperty(kOfxImageEffectPropComponents))
        {
          components = componentCount(componentsName);
          pixelBytes = componentBytes(depth) * components;
          data = static_cast<char *>(image.getPointerProperty(kOfxImagePropData));
          rowBytes = image.getIntProperty(kOfxImagePropRowBytes);
        }
        int width() const { return bounds.x2 - bounds.x1; }
        int height() const { return bounds.y2 - bounds.y1; }
        /// the CPU pixel at (x, y)
        char *at(int x, int y) const
        {
          return data + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes +
            static_cast<std::ptrdiff_t>(x - bounds.x1) * pixelBytes;
        }
      };

      /// r within b, empty (x1 == x2) if they don't overlap
      static OfxRectI intersect(const OfxRectI &r, const OfxRectI &b)
      {
        OfxRectI i = { std::max(r.x1, b.x1), std::max(r.y1, b.y1), std::min(r.x2, b.x2), std::min(r.y2, b.y2) };
        if (i.x2 <= i.x1 || i.y2 <= i.y1) {
          i.x2 = i.x1;
          i.y2 = i.y1;
        }
        return i;
      }

      int RenderSession::transfer(void *memory, const ImageEffect::Image &image, const OfxRectI &rect, bool write)
      {
        cl_mem mem = static_cast<cl_mem>(memory);
        const PixelLayout l(image);
        const OfxRectI r = intersect(rect, l.bounds);
        const int w = r.x2 - r.x1, h = r.y2 - r.y1;
        if (w <= 0 || h <= 0) {
          return kCLSuccess;
        }
        // the rows of r, packed bottom up (rowBytes may be negative or padded)
        const size_t rowSize = static_cast<size_t>(w) * static_cast<size_t>(l.pixelBytes);
        std::vector<char> staging(rowSize * static_cast<size_t>(h));
        if (write) {
          for (int y = 0; y < h; ++y) {
            std::memcpy(&staging[rowSize * static_cast<size_t>(y)], l.at(r.x1, r.y1 + y), rowSize);
          }
        }
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
          for (int y = 0; y < h; ++y) {
            std::memcpy(l.at(r.x1, r.y1 + y), &staging[rowSize * static_cast<size_t>(y)], rowSize);
          }
        }
        return err;
      }

      RenderSession::Handle *RenderSession::copy(ImageEffect::ClipInstance &clip, const ImageEffect::Image &image,
                                                 const OfxRectI &rect)
      {
        if (!_ok) {
          fail("no OpenCL render in this session (see its constructor)");
          return nullptr;
        }
        const PixelLayout l(image);
        const int w = l.width(), h = l.height();
        if (l.pixelBytes == 0) {
          fail("an image of depth " + l.depth + " and components " + l.componentsName + " can't be copied");
          return nullptr;
        }
        if (w <= 0 || h <= 0 || !l.data) {
          fail(!l.data ? "an image without pixel data" : "an empty image can't be an OpenCL buffer or image");
          return nullptr;
        }
        const bool images = _mode == RenderMode::Images;
        if (images && l.components != 4) {
          // OFX doesn't say which OpenCL Image format other components have
          fail("an image of components " + l.componentsName + " can't be an OpenCL Image (RGBA only)");
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
          fail(std::string(images ? "an OpenCL Image of " : "an OpenCL buffer of ") + std::to_string(w) + "x" +
               std::to_string(h) + " " + l.componentsName + " " + l.depth + ": " + errorName(err));
          return nullptr;
        }
        // "OpenCL Images do not have the concept of row bytes": 0 for them
        return new Handle(*this, clip, image, owned, images ? 0 : static_cast<int>(packed));
      }

      ImageEffect::Image *RenderSession::setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                                   const OfxRectI &renderWindow)
      {
        if (_output) {
          _output->releaseReference();
          _output = nullptr;
        }
        _outputClip = &clip;
        _outputImage = &image;
        _renderWindow = renderWindow;
        _output = copy(clip, image, renderWindow);
        return _output;
      }

      bool RenderSession::readOutput()
      {
        if (!_output || !_outputImage) {
          fail("no output to read back");
          return false;
        }
        if (!_queue.finish()) {
          fail("the render's commands failed (clFinish)");
          return false;
        }
        if (!_outputImage->getPointerProperty(kOfxImagePropData)) {
          fail("an output image without pixel data");
          return false;
        }
        // the render window only: the plug-in renders "the window (in PixelCoordinates) to render"
        // (kOfxImageEffectPropRenderWindow), and the host's other pixels stay as they are
        const cl_int err = transfer(_output->mem(), *_outputImage, _renderWindow, /*write=*/false);
        if (err != kCLSuccess) {
          fail("reading the output back: " + errorName(err));
          return false;
        }
        return true;
      }

      ImageEffect::Image *RenderSession::getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image)
      {
        if (&clip == _outputClip) {
          std::lock_guard<std::mutex> lock(_mutex);
          if (_output) {
            _output->addReference();
          }
          return _output;
        }
        return image ? copy(clip, *image, image->getBounds()) : nullptr;
      }

    } // OpenCL
  } // Host
} // OFX
