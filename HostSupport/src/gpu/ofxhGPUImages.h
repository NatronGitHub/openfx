#ifndef OFXH_GPU_IMAGES_H
#define OFXH_GPU_IMAGES_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// Internal to HostSupport's GPU components (openfx::HostSupportOpenCL, openfx::HostSupportMetal),
// not installed: what their RenderSessions share, apart from the device memory itself.
// - PixelLayout and copyRect(): the CPU pixels of an image, and the copy of a rectangle of them
//   to and from packed device-side rows (bottom up);
// - SessionImages: the images a session gives the plug-in (registered by the API's Image
//   subclass), its output, and its failure flag and log.

#include "ofxhClip.h"
#include "ofxhImageEffect.h"
#include "ofxhUtilities.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace GPU {

      /// the bytes of a component of depth, 0 if unknown
      inline int componentBytes(const std::string &depth)
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
      inline int componentCount(const std::string &components)
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
        /// the bytes of a packed row of the bounds
        size_t packedRow() const { return static_cast<size_t>(width()) * static_cast<size_t>(pixelBytes); }
        /// the CPU pixel at (x, y)
        char *at(int x, int y) const
        {
          return data + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes +
            static_cast<std::ptrdiff_t>(x - bounds.x1) * pixelBytes;
        }
      };

      /// r within b, empty (x1 == x2) if they don't overlap
      inline OfxRectI intersect(const OfxRectI &r, const OfxRectI &b)
      {
        OfxRectI i = { std::max(r.x1, b.x1), std::max(r.y1, b.y1), std::min(r.x2, b.x2), std::min(r.y2, b.y2) };
        if (i.x2 <= i.x1 || i.y2 <= i.y1) {
          i.x2 = i.x1;
          i.y2 = i.y1;
        }
        return i;
      }

      /// Copies the pixels of r (within l's bounds) between l's CPU pixels and rows, packed bottom
      /// up memory whose pixel (x, y) is at rows + (y - originY) * rowsBytes + (x - originX) *
      /// l.pixelBytes: toRows from the CPU image to rows, else back.
      inline void copyRect(const PixelLayout &l, const OfxRectI &r, char *rows, size_t rowsBytes, int originX,
                           int originY, bool toRows)
      {
        const size_t rowSize = static_cast<size_t>(r.x2 - r.x1) * static_cast<size_t>(l.pixelBytes);
        for (int y = r.y1; y < r.y2; ++y) {
          char *p = rows + static_cast<size_t>(y - originY) * rowsBytes +
            static_cast<size_t>(r.x1 - originX) * static_cast<size_t>(l.pixelBytes);
          if (toRows) {
            std::memcpy(p, l.at(r.x1, y), rowSize);
          } else {
            std::memcpy(l.at(r.x1, y), p, rowSize);
          }
        }
      }

      /// the rows of r (within l's bounds) packed bottom up, for a copy between l's CPU pixels and
      /// device memory (rowBytes may be negative or padded there): toDevice, filled from l; else
      /// filled by the copy from the device, then given back to l with unstage()
      struct StagedRect {
        OfxRectI r;
        size_t rowSize;  ///< the bytes of a row of r
        std::vector<char> rows;

        StagedRect(const PixelLayout &l, const OfxRectI &rect, bool toDevice)
          : r(intersect(rect, l.bounds))
          , rowSize(static_cast<size_t>(r.x2 - r.x1) * static_cast<size_t>(l.pixelBytes))
          , rows(rowSize * static_cast<size_t>(r.y2 - r.y1))
        {
          if (toDevice && !empty()) {
            copyRect(l, r, rows.data(), rowSize, r.x1, r.y1, /*toRows=*/true);
          }
        }
        bool empty() const { return r.x2 <= r.x1 || r.y2 <= r.y1; }
        int width() const { return r.x2 - r.x1; }
        int height() const { return r.y2 - r.y1; }
        /// the rows, read back from the device, to l's CPU pixels
        void unstage(const PixelLayout &l)
        {
          if (!empty()) {
            copyRect(l, r, rows.data(), rowSize, r.x1, r.y1, /*toRows=*/false);
          }
        }
      };

      /// the properties a GPU copy of image keeps from it: its depth, components,
      /// premultiplication and pixel aspect ratio (the Image constructor sets the others)
      inline void copyImageProperties(ImageEffect::Image &copy, const ImageEffect::Image &image)
      {
        copy.setStringProperty(kOfxImageEffectPropPixelDepth, image.getStringProperty(kOfxImageEffectPropPixelDepth));
        copy.setStringProperty(kOfxImageEffectPropComponents, image.getStringProperty(kOfxImageEffectPropComponents));
        copy.setStringProperty(kOfxImageEffectPropPreMultiplication,
                               image.getStringProperty(kOfxImageEffectPropPreMultiplication));
        copy.setDoubleProperty(kOfxImagePropPixelAspectRatio, image.getDoubleProperty(kOfxImagePropPixelAspectRatio));
      }

      /// The image bookkeeping of a GPU RenderSession: the images it gave out and the plug-in hasn't
      /// released yet (the API's Image subclass registers itself), the output, and whether a copy
      /// failed. Thread-safe, except setOutput().
      class SessionImages {
      public:
        typedef std::function<void(LogLevel level, const std::string &message)> Log;
        /// api: "OpenCL" or "Metal", the prefix of the messages, which go to log
        SessionImages(const char *api, Log log) : _api(api), _log(std::move(log)) {}
        /// releases the output, and deletes the images the plug-in didn't release
        ~SessionImages()
        {
          if (_output) {
            _output->releaseReference();
          }
          std::vector<ImageEffect::Image *> left;
          {
            std::lock_guard<std::mutex> lock(_mutex);
            left.assign(_images.begin(), _images.end());
          }
          // "image handle to be disposed of by clipReleaseImage before the action returns"
          // (clipGetImage): the handles left are the plug-in's leaks
          if (!left.empty()) {
            try {
              log(LogLevel::Warning, "the plug-in didn't release " + std::to_string(left.size()) +
                  " image(s) with clipReleaseImage");
            } catch (...) {
              // (the host's logger threw: dropped, in a destructor; C.36)
            }
          }
          for (ImageEffect::Image *i : left) {
            delete i;
          }
        }
        SessionImages(const SessionImages &) = delete;
        SessionImages &operator=(const SessionImages &) = delete;

        /// (the API's Image subclass, constructed and destroyed)
        void add(ImageEffect::Image *image)
        {
          std::lock_guard<std::mutex> lock(_mutex);
          _images.insert(image);
        }
        void remove(ImageEffect::Image *image)
        {
          std::lock_guard<std::mutex> lock(_mutex);
          _images.erase(image);
        }

        bool failed() const
        {
          std::lock_guard<std::mutex> lock(_mutex);
          return _failed;
        }
        int liveImages() const
        {
          std::lock_guard<std::mutex> lock(_mutex);
          return static_cast<int>(_images.size());
        }
        void fail(const std::string &why)
        {
          {
            std::lock_guard<std::mutex> lock(_mutex);
            _failed = true;
          }
          log(LogLevel::Error, why);
        }
        /// the message to the session's log, prefixed with "<api>: "
        void log(LogLevel level, const std::string &message) const
        {
          if (_log) {
            _log(level, _api + ": " + message);
          }
        }

        /// whether image can be copied to device memory, a ("an OpenCL buffer or image", "a Metal
        /// buffer"); fails if not
        bool copyable(const PixelLayout &l, const char *a)
        {
          if (l.pixelBytes == 0) {
            fail("an image of depth " + l.depth + " and components " + l.componentsName + " can't be copied");
            return false;
          }
          if (l.width() <= 0 || l.height() <= 0 || !l.data) {
            fail(!l.data ? std::string("an image without pixel data") : std::string("an empty image can't be ") + a);
            return false;
          }
          return true;
        }

        /// RenderSession::setOutput(): replaces the output by copy(clip, image, renderWindow)
        template <class Copy>
        ImageEffect::Image *setOutput(ImageEffect::ClipInstance &clip, ImageEffect::Image &image,
                                      const OfxRectI &renderWindow, Copy copy)
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
        /// RenderSession::getImage(): the output (another reference) for the output clip, else
        /// copy(clip, *image, its bounds)
        template <class Copy>
        ImageEffect::Image *getImage(ImageEffect::ClipInstance &clip, ImageEffect::Image *image, Copy copy)
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
        /// RenderSession::readOutput()'s precondition: there is an output; fails if not
        bool hasOutput()
        {
          if (!_output || !_outputImage) {
            fail("no output to read back");
            return false;
          }
          return true;
        }

        ImageEffect::Image *output() const { return _output; }
        ImageEffect::Image *outputImage() const { return _outputImage; }
        const OfxRectI &renderWindow() const { return _renderWindow; }

      private:
        std::string _api;
        Log _log;
        ImageEffect::ClipInstance *_outputClip = nullptr;
        ImageEffect::Image *_outputImage = nullptr; ///< the host's
        OfxRectI _renderWindow = { 0, 0, 0, 0 };   ///< of setOutput()
        ImageEffect::Image *_output = nullptr;      ///< (the session's reference)
        mutable std::mutex _mutex;
        std::set<ImageEffect::Image *> _images;
        bool _failed = false;
      };

    } // GPU
  } // Host
} // OFX

#endif // OFXH_GPU_IMAGES_H
