#ifndef OFXH_CUDA_DEVICE_H
#define OFXH_CUDA_DEVICE_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhCUDADevice.h

  CUDA devices and streams for CUDA renders (ofxGPURender.h, OFX 1.5: "CUDA Rendering"): the CUDA
  devices, the primary context of one of them, and the streams that renders borrow from it.

  Part of the optional library openfx::HostSupportCUDA (CMake option OFX_HOSTSUPPORT_CUDA);
  ofxhCUDARender.h builds on it. No CUDA toolkit is needed to build it: the CUDA driver API
  (libcuda.so.1 on Linux, nvcuda.dll on Windows; there is none on macOS) is loaded at run time,
  unless the host gives its own loader (Hooks::getProcAddress). It needs a CUDA 11 or later driver
  and a 64-bit build. No global state: everything hangs off the objects the host creates.

  The library uses each device's primary context (cuDevicePrimaryCtxRetain), the one the CUDA
  runtime API uses, so that a plug-in built with the runtime API (cudaMalloc, kernel launches...)
  works on the host's device memory: a RenderSession makes it current on its thread during the
  render actions (ContextScope).

  Typical use:

    OFX::Host::CUDA::Hooks hooks;                                  // logger, loader (optional)
    std::unique_ptr<OFX::Host::CUDA::Device> device = OFX::Host::CUDA::createDevice(hooks);
    ...
    OFX::Host::CUDA::Stream stream(*device);  // one per render, given back at the end of the scope
    ... render (ofxhCUDARender.h) ...
*/

#include "ofxhUtilities.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace CUDA {

      /// Services the host gives the devices; all optional
      struct Hooks {
        /// receives the messages, with their level (OFX::Host::LogLevel, ofxhUtilities.h, as the
        /// other GPU components'; default: dropped). May be called from the plug-in's threads
        /// (clipGetImage).
        std::function<void(LogLevel level, const std::string &message)> log;
        /// runs call, a call into the CUDA driver. Drivers may raise floating-point exceptions: a
        /// host that unmasks them masks them here (default: calls it). It runs with no lock of this
        /// library held. It should not throw: the destructors (of Device, Stream and the sessions)
        /// catch what it throws, and then leak the CUDA object.
        std::function<void(const std::function<void()> &call)> driverCall;
        /// the CUDA driver API function name, by its exported name (e.g. "cuInit",
        /// "cuMemAlloc_v2"), from the host's own CUDA library, nullptr if not found (default: from
        /// libcuda.so.1, the driver's library, not the toolkit's stub libcuda.so; nvcuda.dll, from
        /// System32 only, against DLL planting). A host that bundles its own library gives it here.
        std::function<void *(const char *name)> getProcAddress;
      };

      /// A CUDA device
      struct DeviceInfo {
        int index = -1;                   ///< its ordinal (cuDeviceGet), in CUDA's order
        std::string name;                 ///< cuDeviceGetName
        int computeMajor = 0, computeMinor = 0; ///< its compute capability
        /// "domain:bus:device" in hexadecimal (e.g. "0000:01:00"; nvidia-smi prints
        /// "00000000:01:00.0"), for display. The ordinals (index) change with CUDA_VISIBLE_DEVICES
        /// and CUDA_DEVICE_ORDER: save uuid instead
        std::string pciBusId;
        /// cuDeviceGetUuid_v2 (else cuDeviceGetUuid, before driver 11.4), as
        /// "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx": the identity to save in a host's preferences
        /// (createDeviceWithUUID()). The prefix is always "GPU-"; nvidia-smi -L and
        /// CUDA_VISIBLE_DEVICES write a MIG instance's as "MIG-...", which createDeviceWithUUID()
        /// accepts too
        std::string uuid;
        unsigned long long totalMem = 0;  ///< cuDeviceTotalMem, bytes
        int driverVersion = 0;            ///< cuDriverGetVersion (e.g. 12040 for 12.4)
      };

      /// The CUDA devices. Empty if there is no CUDA (then *error, if not null, says why).
      std::vector<DeviceInfo> listDevices(const Hooks &hooks, std::string *error = nullptr);

      namespace Detail { struct CUFunctions; }

      class Stream;

      /// A CUDA device, its primary context (retained while the Device lives), and the streams it
      /// lends to renders (Stream). Destroy it after every Stream and every RenderSession on it.
      ///
      /// A sticky CUDA error (e.g. CUDA_ERROR_ILLEGAL_ADDRESS after a faulty kernel of a plug-in)
      /// breaks the primary context for the whole process: every later call fails. The Device then
      /// becomes unusable() (logged once): its sessions refuse CUDA, and a host that passes
      /// usable() to chooseRenderMode() renders on the CPU instead of failing again. CUDA only
      /// recovers with cuDevicePrimaryCtxReset once nothing uses the context; this library doesn't
      /// do it (the host may destroy the Device and create another once its renders are done).
      class Device {
      public:
        ~Device();
        Device(const Device &) = delete;
        Device &operator=(const Device &) = delete;

        const DeviceInfo &info() const { return _info; }
        const Hooks &hooks() const { return _hooks; }
        /// the CUdevice, the CUcontext (the primary context)
        int nativeDevice() const { return _device; }
        void *nativeContext() const { return _context; }
        /// the CUDA functions HostSupport uses
        const Detail::CUFunctions &cu() const { return *_cu; }
        /// runs call through Hooks::driverCall
        void driverCall(const std::function<void()> &call) const;
        /// runs call through Hooks::driverCall, with the primary context current on the calling
        /// thread (pushed, and popped after it, also if it throws); its CUresult, or the push's if
        /// it failed
        int withContext(const std::function<int()> &call) const;
        /// sends message to Hooks::log, if any
        void log(LogLevel level, const std::string &message) const;
        /// the streams created so far (in use or idle)
        int streamCount() const;
        /// no sticky error happened on the device's context (see the class doc)
        bool usable() const { return _usable; }
        /// err (a CUresult) is CUDA_SUCCESS; a sticky error makes the device unusable()
        bool checkError(int err) const;

      private:
        friend class Stream;
        friend struct DeviceFactory;  ///< (createDevice(), createDeviceWithUUID())
        Device(const Hooks &hooks, std::shared_ptr<const Detail::CUFunctions> cu, const DeviceInfo &info, int device,
               void *context);
        void *takeStream(std::string &error);
        void giveBack(void *stream);
        /// destroys a stream that failed
        void discard(void *stream);

        Hooks _hooks;
        std::shared_ptr<const Detail::CUFunctions> _cu;
        DeviceInfo _info;
        int _device;
        void *_context;
        mutable std::mutex _mutex;
        std::vector<void *> _idle;  ///< idle streams
        int _streams = 0;           ///< streams created
        mutable std::atomic<bool> _usable{ true };
      };

      /// The device of DeviceInfo::index index, or by default (-1) the first one (CUDA's order:
      /// the fastest first, unless CUDA_DEVICE_ORDER says otherwise). nullptr if none: then *error
      /// (if not null) says why, and the message went to hooks.log.
      std::unique_ptr<Device> createDevice(const Hooks &hooks, int index = -1, std::string *error = nullptr);

      /// The device of DeviceInfo::uuid uuid, with its "GPU-" prefix or a MIG instance's "MIG-" (as
      /// nvidia-smi -L prints it). nullptr if none, as createDevice().
      std::unique_ptr<Device> createDeviceWithUUID(const Hooks &hooks, const std::string &uuid,
                                                   std::string *error = nullptr);

      /// The device's primary context, current on the calling thread for the lifetime of the scope
      /// (cuCtxPushCurrent, cuCtxPopCurrent). Scopes nest; each is destroyed on its thread,
      /// innermost first.
      ///
      /// A host that runs the multi-thread suite's functions (OfxMultiThreadSuiteV1::multiThread)
      /// on threads of its own opens one on each of them while the calling thread is in a CUDA
      /// RenderSession, as it carries the session itself (see RenderSession): the plug-in isn't
      /// told the host's device, and on a thread with no current context the CUDA runtime uses
      /// device 0. (HostSupport's own multi-thread suite, without OFX_SUPPORTS_MULTITHREAD, runs
      /// them on the calling thread, where the session's context is current.)
      class ContextScope {
      public:
        explicit ContextScope(const Device &device);
        ~ContextScope();
        ContextScope(const ContextScope &) = delete;
        ContextScope &operator=(const ContextScope &) = delete;
        /// the context is current (else error() says why)
        bool ok() const { return _ok; }
        const std::string &error() const { return _error; }

      private:
        const Device &_device;
        std::string _error;
        bool _ok;
      };

      /// The CUDA stream of a render, for the lifetime of the scope. It has none until open(),
      /// which a RenderSession calls only if the plug-in and the host both support CUDA streams:
      /// kOfxImageEffectPropCudaStream "will only be set if the host and plug-in both support CUDA
      /// streams" (ofxGPURender.h). open() takes an idle stream of the device, else creates one
      /// (cuStreamCreate, CU_STREAM_NON_BLOCKING: other work on the legacy default stream doesn't
      /// hold it up). There is no maximum: taking one never waits. On exit, an opened stream is
      /// waited for (cuStreamSynchronize) and given back; if that fails, it is destroyed instead,
      /// so that no render gets a broken one. CUDA can't cancel enqueued work: after an aborted
      /// render, which returns from its action, the sessions and the Stream still wait for it.
      ///
      /// A host may keep one Stream for a whole frame (the render of a tree of nodes, in its own
      /// per-render state) and give it to the RenderSession of each node, one after the other or
      /// nested (a node's clipGetImage renders its input): the work of the nodes is then ordered on
      /// one stream. A Stream is used by one thread at a time (open() and synchronize() aren't
      /// synchronized): don't hand it to the multi-thread suite's threads; a render that one of them
      /// starts (an input rendered from a spawned thread) needs a Stream of its own.
      class Stream {
      public:
        explicit Stream(Device &device);
        ~Stream();
        Stream(const Stream &) = delete;
        Stream &operator=(const Stream &) = delete;

        /// takes a stream, if it has none yet; false if it can't (then error() says why)
        bool open();
        /// the CUstream, nullptr until open() (or if it failed)
        void *native() const { return _stream; }
        const std::string &error() const { return _errorText; }
        Device &device() const { return _device; }
        /// waits for the stream's work (cuStreamSynchronize); true without a stream; false if that
        /// fails (then lastError() says why)
        bool synchronize();
        /// the CUresult of the last synchronize()
        int lastError() const { return _error; }

      private:
        Device &_device;
        std::string _errorText;
        void *_stream = nullptr;
        int _error = 0;
      };

    } // CUDA
  } // Host
} // OFX

#endif // OFXH_CUDA_DEVICE_H
