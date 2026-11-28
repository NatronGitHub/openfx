#ifndef OFXH_METAL_DEVICE_H
#define OFXH_METAL_DEVICE_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhMetalDevice.h

  Metal devices and command queues for Metal renders (ofxGPURender.h, OFX 1.5: "Apple Metal
  Rendering"): the Metal devices of the Mac, one of them, and the command queues that renders
  borrow from it.

  Part of the optional library openfx::HostSupportMetal (CMake option OFX_HOSTSUPPORT_METAL, macOS
  only); ofxhMetalRender.h builds on it. A plain C++ header: the Metal objects are given as void *
  (id<MTLDevice>, id<MTLCommandQueue>), which the library owns; the Objective-C++ is in the
  library. No global state: everything hangs off the objects the host creates.

  Typical use:

    OFX::Host::Metal::Hooks hooks;                                    // logger (optional)
    std::unique_ptr<OFX::Host::Metal::Device> device = OFX::Host::Metal::createDevice(hooks);
    ...
    OFX::Host::Metal::Queue queue(*device);   // one per render, given back at the end of the scope
    ... render (ofxhMetalRender.h) ...
*/

#include "ofxhUtilities.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace Metal {

      /// Services the host gives the devices; all optional
      struct Hooks {
        /// receives the messages, with their level (OFX::Host::LogLevel, ofxhUtilities.h, as the
        /// OpenGL and OpenCL components'; default: dropped). May be called from the plug-in's
        /// threads (clipGetImage).
        std::function<void(LogLevel level, const std::string &message)> log;
      };

      /// A Metal device
      struct DeviceInfo {
        int index = -1;              ///< its index in MTLCopyAllDevices()
        std::string name;            ///< MTLDevice.name
        unsigned long long registryID = 0;  ///< MTLDevice.registryID
        bool systemDefault = false;  ///< it is MTLCreateSystemDefaultDevice()
        bool unifiedMemory = false;  ///< MTLDevice.hasUnifiedMemory
        unsigned long long recommendedMaxWorkingSetSize = 0; ///< bytes
        unsigned long long maxBufferLength = 0;              ///< bytes
      };

      /// The Metal devices (MTLCopyAllDevices()). Empty if there is none (then *error, if not
      /// null, says why).
      std::vector<DeviceInfo> listDevices(const Hooks &hooks, std::string *error = nullptr);

      class Queue;

      /// An Objective-C autorelease pool for the lifetime of the scope (objc_autoreleasePoolPush()
      /// and objc_autoreleasePoolPop(), the ARC runtime's), for the host's C++ render threads: the
      /// plug-in's Metal calls (e.g. [queue commandBuffer]) autorelease objects on the calling
      /// thread, which a thread without a pool keeps until it ends. Create one around each Metal
      /// render action (with its RenderSession), on the thread that sends it; pools nest, and each
      /// is destroyed on its thread, innermost first.
      class AutoreleasePool {
      public:
        AutoreleasePool();
        ~AutoreleasePool();
        AutoreleasePool(const AutoreleasePool &) = delete;
        AutoreleasePool &operator=(const AutoreleasePool &) = delete;
      private:
        void *_pool;
      };

      /// A Metal device, and the command queues it lends to renders (Queue). Destroy it after
      /// every Queue and every image of its renders.
      class Device {
      public:
        ~Device();
        Device(const Device &) = delete;
        Device &operator=(const Device &) = delete;

        const DeviceInfo &info() const { return _info; }
        const Hooks &hooks() const { return _hooks; }
        /// the id<MTLDevice> (owned by the Device)
        void *nativeDevice() const;
        /// sends message to Hooks::log, if any
        void log(LogLevel level, const std::string &message) const;
        /// the command queues created so far (in use or idle)
        int queueCount() const;

      private:
        struct Impl;  ///< (the Objective-C++ state, ofxhMetalDevice.mm)
        friend class Queue;
        friend struct DeviceFactory;
        Device(const Hooks &hooks, const DeviceInfo &info, std::unique_ptr<Impl> impl);
        /// an idle queue, else a new one; the caller owns it (+1), nullptr if none could be made
        void *takeQueue(std::string &error);
        /// takes back queue, from takeQueue(), as idle
        void giveBack(void *queue);

        Hooks _hooks;
        DeviceInfo _info;
        std::unique_ptr<Impl> _impl;
      };

      /// The device of DeviceInfo::index index, or by default (-1) the system default device
      /// (MTLCreateSystemDefaultDevice()), else the first device. nullptr if none: then *error (if
      /// not null) says why, and the message went to hooks.log.
      std::unique_ptr<Device> createDevice(const Hooks &hooks, int index = -1, std::string *error = nullptr);

      /// The device of DeviceInfo::registryID registryID, looked up in MTLCopyAllDevices() (the
      /// identity to save in a host's preferences: the index changes when a removable GPU comes or
      /// goes). nullptr if none, as createDevice().
      std::unique_ptr<Device> createDeviceWithRegistryID(const Hooks &hooks, unsigned long long registryID,
                                                         std::string *error = nullptr);

      /// A command queue of device for the lifetime of the scope: an idle one, else a new one (there
      /// is no maximum, so taking one never waits), given back on exit. It doesn't wait for the
      /// commands committed to it: the RenderSession waits for the plug-in's work on its buffers
      /// (ofxhMetalRender.h). Use it on one render at a time.
      ///
      /// A command buffer of the plug-in that fails (MTLCommandBufferStatusError) goes undetected:
      /// the host only sees its own command buffers' status, and Metal has no queue-wide error (as
      /// OpenCL's clFinish has). The render then reads back what the output buffer holds, and there
      /// is no CPU retry, unless the plug-in returns kOfxStatGPURenderFailed itself.
      class Queue {
      public:
        explicit Queue(Device &device);
        ~Queue();
        Queue(const Queue &) = delete;
        Queue &operator=(const Queue &) = delete;

        /// the id<MTLCommandQueue>, nullptr if it couldn't be created (then error() says why)
        void *native() const { return _queue; }
        const std::string &error() const { return _error; }
        Device &device() const { return _device; }

      private:
        Device &_device;
        std::string _error;  ///< (before _queue: the constructor fills it)
        void *_queue;        ///< (+1, from Device::takeQueue())
      };

    } // Metal
  } // Host
} // OFX

#endif // OFXH_METAL_DEVICE_H
