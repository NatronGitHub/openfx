#ifndef OFXH_OPENCL_DEVICE_H
#define OFXH_OPENCL_DEVICE_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhOpenCLDevice.h

  OpenCL devices and command queues for OpenCL renders (ofxGPURender.h, OFX 1.5: "OpenCL
  Acceleration of Rendering"): the devices of the OpenCL platforms, a context on one of them, and
  the in-order command queues that renders borrow from it.

  Part of the optional library openfx::HostSupportOpenCL (CMake option OFX_HOSTSUPPORT_OPENCL);
  ofxhOpenCLRender.h builds on it. No OpenCL header or library is needed to build it: the OpenCL
  ICD loader (libOpenCL.so.1, OpenCL.dll; the OpenCL framework on macOS) is loaded at run time,
  unless the host gives its own loader (Hooks::getProcAddress). No global state: everything hangs
  off the objects the host creates.

  Typical use:

    OFX::Host::OpenCL::Hooks hooks;                                   // logger, loader (optional)
    std::unique_ptr<OFX::Host::OpenCL::Device> device = OFX::Host::OpenCL::createDevice(hooks);
    ...
    OFX::Host::OpenCL::Queue queue(*device);   // one per render, given back at the end of the scope
    ... render (ofxhOpenCLRender.h) ...
*/

#include "ofxhUtilities.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace OpenCL {


      /// Services the host gives the devices; all optional
      struct Hooks {
        /// receives the messages, with their level (OFX::Host::LogLevel, ofxhUtilities.h, as the
        /// OpenGL component's; default: dropped). May be called from the
        /// plug-in's threads (clipGetImage).
        std::function<void(LogLevel level, const std::string &message)> log;
        /// runs call, a call into the OpenCL driver. Drivers may raise floating-point exceptions:
        /// a host that unmasks them masks them here (default: calls it). It runs with no lock of
        /// this library held. It should not throw: the destructors (of Device, Queue and the
        /// images) catch what it throws, and then leak the OpenCL object.
        std::function<void(const std::function<void()> &call)> driverCall;
        /// the OpenCL API function name (e.g. "clGetPlatformIDs") from the host's own OpenCL
        /// library, nullptr if not found (default: from the ICD loader, libOpenCL.so.1 or
        /// libOpenCL.so; OpenCL.dll, from System32 only, against DLL planting;
        /// /System/Library/Frameworks/OpenCL.framework). A host that bundles its own OpenCL.dll, or
        /// relies on one in PATH, gives it here.
        std::function<void *(const char *name)> getProcAddress;
      };

      /// An OpenCL device of a platform
      struct DeviceInfo {
        int platform = -1;  ///< the index of its platform (clGetPlatformIDs)
        int device = -1;    ///< its index in the platform (clGetDeviceIDs, CL_DEVICE_TYPE_ALL)
        std::string platformName, platformVersion;      ///< CL_PLATFORM_NAME, CL_PLATFORM_VERSION
        std::string name, vendor, version, driverVersion; ///< CL_DEVICE_NAME, _VENDOR, _VERSION, CL_DRIVER_VERSION
        bool gpu = false;            ///< CL_DEVICE_TYPE_GPU
        bool imageSupport = false;   ///< CL_DEVICE_IMAGE_SUPPORT: OpenCL Images can be used
        unsigned long long globalMemSize = 0; ///< CL_DEVICE_GLOBAL_MEM_SIZE, bytes
        unsigned long long maxAllocSize = 0;  ///< CL_DEVICE_MAX_MEM_ALLOC_SIZE, bytes
      };

      /// The devices of all the platforms. Empty if there is no OpenCL (then *error, if not null,
      /// says why).
      std::vector<DeviceInfo> listDevices(const Hooks &hooks, std::string *error = nullptr);

      namespace Detail { struct CLFunctions; }

      class Queue;

      /// An OpenCL context on one device, and the command queues it lends to renders (Queue).
      /// Destroy it after every Queue and every image of its renders.
      class Device {
      public:
        ~Device();
        Device(const Device &) = delete;
        Device &operator=(const Device &) = delete;

        const DeviceInfo &info() const { return _info; }
        const Hooks &hooks() const { return _hooks; }
        /// the cl_context, the cl_device_id
        void *nativeContext() const { return _context; }
        void *nativeDevice() const { return _device; }
        /// the OpenCL functions HostSupport uses
        const Detail::CLFunctions &cl() const { return *_cl; }
        /// runs call through Hooks::driverCall
        void driverCall(const std::function<void()> &call) const;
        /// sends message to Hooks::log, if any
        void log(LogLevel level, const std::string &message) const;
        /// the command queues created so far (in use or idle)
        int queueCount() const;

      private:
        friend class Queue;
        friend std::unique_ptr<Device> createDevice(const Hooks &, int, int, std::string *);
        Device(const Hooks &hooks, std::shared_ptr<const Detail::CLFunctions> cl, const DeviceInfo &info,
               void *device, void *context);
        void *takeQueue(std::string &error);
        void giveBack(void *queue);
        /// releases a queue that failed
        void discard(void *queue);

        Hooks _hooks;
        std::shared_ptr<const Detail::CLFunctions> _cl;
        DeviceInfo _info;
        void *_device;
        void *_context;
        mutable std::mutex _mutex;
        std::vector<void *> _idle;  ///< idle queues
        int _queues = 0;            ///< queues created
      };

      /// A context on the device device of the platform platform (DeviceInfo::platform and
      /// DeviceInfo::device), or by default (-1, -1) on the first GPU device, else the first device.
      /// nullptr if none: then *error (if not null) says why, and the message went to hooks.log.
      std::unique_ptr<Device> createDevice(const Hooks &hooks, int platform = -1, int device = -1,
                                           std::string *error = nullptr);

      /// A command queue of device for the lifetime of the scope: an idle one, else a new one. It is
      /// in order and doesn't profile: "Assume an in-order command queue. Do not assume a profiling
      /// command queue" (ofxGPURender.h). On exit, waits for its commands (clFinish) and gives it
      /// back; if that fails, the queue is released instead, so that no render gets a broken one.
      /// OpenCL can't cancel enqueued commands: after an aborted render, which returns from its
      /// action, the destructor still waits for the kernels the plug-in enqueued.
      /// Use it on one render at a time.
      class Queue {
      public:
        explicit Queue(Device &device);
        ~Queue();
        Queue(const Queue &) = delete;
        Queue &operator=(const Queue &) = delete;

        /// the cl_command_queue, nullptr if it couldn't be created (then error() says why)
        void *native() const { return _queue; }
        const std::string &error() const { return _error; }
        Device &device() const { return _device; }
        /// waits for the queue's commands (clFinish); false if that fails
        bool finish();

      private:
        Device &_device;
        std::string _error;  ///< (before _queue: the constructor fills it)
        void *_queue;
      };

    } // OpenCL
  } // Host
} // OFX

#endif // OFXH_OPENCL_DEVICE_H
