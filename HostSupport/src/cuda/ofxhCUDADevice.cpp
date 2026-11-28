// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhCUDADevice.h: the CUDA driver library, the devices, their primary contexts and streams.

#include "ofxhCUDADevice.h"
#include "ofxhCUDAFunctions.h"
#include "../gpu/ofxhGPULibrary.h"

#include <cstdio>
#include <utility>

namespace OFX {
  namespace Host {
    namespace CUDA {
      namespace Detail {

        std::string errorName(CUresult err)
        {
          const char *name = nullptr;
          switch (err) {
          case kCudaSuccess: name = "CUDA_SUCCESS"; break;
          case kCudaErrorInvalidValue: name = "CUDA_ERROR_INVALID_VALUE"; break;
          case kCudaErrorOutOfMemory: name = "CUDA_ERROR_OUT_OF_MEMORY"; break;
          case kCudaErrorNotInitialized: name = "CUDA_ERROR_NOT_INITIALIZED"; break;
          case kCudaErrorDeinitialized: name = "CUDA_ERROR_DEINITIALIZED"; break;
          case kCudaErrorStubLibrary: name = "CUDA_ERROR_STUB_LIBRARY"; break;
          case kCudaErrorNoDevice: name = "CUDA_ERROR_NO_DEVICE"; break;
          case kCudaErrorInvalidDevice: name = "CUDA_ERROR_INVALID_DEVICE"; break;
          case kCudaErrorInvalidContext: name = "CUDA_ERROR_INVALID_CONTEXT"; break;
          case kCudaErrorIllegalAddress: name = "CUDA_ERROR_ILLEGAL_ADDRESS"; break;
          case kCudaErrorLaunchOutOfResources: name = "CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES"; break;
          case kCudaErrorLaunchTimeout: name = "CUDA_ERROR_LAUNCH_TIMEOUT"; break;
          case kCudaErrorLaunchFailed: name = "CUDA_ERROR_LAUNCH_FAILED"; break;
          case kCudaErrorEccUncorrectable: name = "CUDA_ERROR_ECC_UNCORRECTABLE"; break;
          case kCudaErrorAssert: name = "CUDA_ERROR_ASSERT"; break;
          case 714: name = "CUDA_ERROR_HARDWARE_STACK_ERROR"; break;
          case 715: name = "CUDA_ERROR_ILLEGAL_INSTRUCTION"; break;
          case 716: name = "CUDA_ERROR_MISALIGNED_ADDRESS"; break;
          case 717: name = "CUDA_ERROR_INVALID_ADDRESS_SPACE"; break;
          case 718: name = "CUDA_ERROR_INVALID_PC"; break;
          case kCudaErrorExternalDevice: name = "CUDA_ERROR_EXTERNAL_DEVICE"; break;
          case 810: name = "CUDA_ERROR_MPS_CLIENT_TERMINATED"; break;
          case kCudaErrorUnknown: name = "CUDA_ERROR_UNKNOWN"; break;
          default: break;
          }
          return std::string(name ? name : "CUDA error") + " (" + std::to_string(err) + ")";
        }

        /// the default loader: the driver's library, never unloaded: drivers may not survive an
        /// unload. Not libcuda.so: the CUDA toolkit's stub has that name (it fails with
        /// CUDA_ERROR_STUB_LIBRARY), the driver installs libcuda.so.1.
        static std::shared_ptr<void> openLibrary(std::string &error)
        {
#if defined(__APPLE__)
          error = "there is no CUDA driver on macOS";
          return nullptr;
#else
#if defined(_WIN32)
          static const char *const names[] = { "nvcuda.dll", nullptr };
#else
          static const char *const names[] = { "libcuda.so.1", nullptr };
#endif
          return GPU::openLibrary(names, error);
#endif
        }

        std::shared_ptr<const CUFunctions> loadCUFunctions(const Hooks &hooks, std::string &error)
        {
          auto cu = std::make_shared<CUFunctions>();
          std::function<void *(const char *)> get = hooks.getProcAddress;
          if (!get) {
            cu->library = openLibrary(error);
            if (!cu->library) {
              return nullptr;
            }
            void *library = cu->library.get();
            get = [library](const char *name) { return GPU::librarySymbol(library, name); };
          }
#define OFXH_CU(f, name) \
          cu->f = reinterpret_cast<decltype(cu->f)>(get(name)); \
          if (!cu->f) { error = std::string(name) + " not found"; return nullptr; }
          OFXH_CU(Init, "cuInit")
          OFXH_CU(DriverGetVersion, "cuDriverGetVersion")
          OFXH_CU(DeviceGetCount, "cuDeviceGetCount")
          OFXH_CU(DeviceGet, "cuDeviceGet")
          OFXH_CU(DeviceGetName, "cuDeviceGetName")
          OFXH_CU(DeviceTotalMem, "cuDeviceTotalMem_v2")
          OFXH_CU(DeviceGetAttribute, "cuDeviceGetAttribute")
          // cuDeviceGetUuid_v2 (driver 11.4+) gives each MIG instance its own UUID ("MIG-..."); v1
          // gives the parent GPU's
          cu->DeviceGetUuid = reinterpret_cast<decltype(cu->DeviceGetUuid)>(get("cuDeviceGetUuid_v2"));
          if (!cu->DeviceGetUuid) {
            OFXH_CU(DeviceGetUuid, "cuDeviceGetUuid")
          }
          OFXH_CU(DevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
          OFXH_CU(DevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2")
          OFXH_CU(CtxPushCurrent, "cuCtxPushCurrent_v2")
          OFXH_CU(CtxPopCurrent, "cuCtxPopCurrent_v2")
          OFXH_CU(StreamCreate, "cuStreamCreate")
          OFXH_CU(StreamSynchronize, "cuStreamSynchronize")
          OFXH_CU(StreamDestroy, "cuStreamDestroy_v2")
          OFXH_CU(MemAlloc, "cuMemAlloc_v2")
          OFXH_CU(MemFree, "cuMemFree_v2")
          OFXH_CU(Memcpy2DAsync, "cuMemcpy2DAsync_v2")
#undef OFXH_CU
          return cu;
        }

        static void driverCall(const Hooks &hooks, const std::function<void()> &call)
        {
          if (hooks.driverCall) {
            hooks.driverCall(call);
          } else {
            call();
          }
        }

        /// "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", as nvidia-smi -L prints it
        static std::string uuidString(const CUuuid &uuid)
        {
          std::string s = "GPU-";
          char hex[3];
          for (int i = 0; i < 16; ++i) {
            std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned char>(uuid.bytes[i]));
            s += hex;
            if (i == 3 || i == 5 || i == 7 || i == 9) {
              s += '-';
            }
          }
          return s;
        }

        /// uuid without its "GPU-" or "MIG-" prefix (nvidia-smi -L prints a MIG instance's as "MIG-...")
        static std::string uuidKey(const std::string &uuid)
        {
          return uuid.compare(0, 4, "GPU-") == 0 || uuid.compare(0, 4, "MIG-") == 0 ? uuid.substr(4) : uuid;
        }

        static int attribute(const CUFunctions &cu, CUdevice d, int what)
        {
          int v = 0;
          return cu.DeviceGetAttribute(&v, what, d) == kCudaSuccess ? v : 0;
        }

        /// cuInit, and the devices, with their CUdevice; false and error if there are none
        static bool enumerate(const Hooks &hooks, const CUFunctions &cu, std::vector<DeviceInfo> &infos,
                              std::vector<CUdevice> &devices, std::string &error)
        {
          CUresult err = kCudaSuccess;
          const char *call = "cuInit";
          driverCall(hooks, [&] {
            err = cu.Init(0);
            int count = 0;
            if (err == kCudaSuccess) {
              call = "cuDeviceGetCount";
              err = cu.DeviceGetCount(&count);
            }
            if (err != kCudaSuccess) {
              return;
            }
            int version = 0;
            cu.DriverGetVersion(&version);
            for (int i = 0; i < count; ++i) {
              CUdevice d = 0;
              if (cu.DeviceGet(&d, i) != kCudaSuccess) {
                continue;
              }
              DeviceInfo info;
              info.index = i;
              char name[256] = { 0 };
              if (cu.DeviceGetName(name, static_cast<int>(sizeof(name)) - 1, d) == kCudaSuccess) {
                info.name = name;
              }
              info.computeMajor = attribute(cu, d, kCuDeviceAttributeComputeCapabilityMajor);
              info.computeMinor = attribute(cu, d, kCuDeviceAttributeComputeCapabilityMinor);
              char bus[32];
              std::snprintf(bus, sizeof(bus), "%04x:%02x:%02x", attribute(cu, d, kCuDeviceAttributePciDomainId),
                            attribute(cu, d, kCuDeviceAttributePciBusId), attribute(cu, d, kCuDeviceAttributePciDeviceId));
              info.pciBusId = bus;
              CUuuid uuid = {};
              if (cu.DeviceGetUuid(&uuid, d) == kCudaSuccess) {
                info.uuid = uuidString(uuid);
              }
              size_t bytes = 0;
              if (cu.DeviceTotalMem(&bytes, d) == kCudaSuccess) {
                info.totalMem = bytes;
              }
              info.driverVersion = version;
              infos.push_back(info);
              devices.push_back(d);
            }
          });
          if (infos.empty()) {
            error = err == kCudaSuccess ? std::string("no CUDA device") : std::string(call) + ": " + errorName(err);
            return false;
          }
          return true;
        }

      } // Detail

      using namespace Detail;

      std::vector<DeviceInfo> listDevices(const Hooks &hooks, std::string *error)
      {
        std::string why;
        std::vector<DeviceInfo> infos;
        std::shared_ptr<const CUFunctions> cu = loadCUFunctions(hooks, why);
        std::vector<CUdevice> devices;
        if (cu) {
          enumerate(hooks, *cu, infos, devices, why);
        }
        if (error) {
          *error = infos.empty() ? why : std::string();
        }
        return infos;
      }

      struct DeviceFactory {
        static std::unique_ptr<Device> make(const Hooks &hooks, int index, const std::string &uuid, std::string *error);
      };

      /// the device of index (by default -1, the first), or of uuid if not empty
      std::unique_ptr<Device> DeviceFactory::make(const Hooks &hooks, int index, const std::string &uuid, std::string *error)
      {
        std::string why;
        std::unique_ptr<Device> result;
        std::shared_ptr<const CUFunctions> cu = loadCUFunctions(hooks, why);
        std::vector<DeviceInfo> infos;
        std::vector<CUdevice> devices;
        bool noCUDA = true;  ///< no driver, or no device
        if (cu && enumerate(hooks, *cu, infos, devices, why)) {
          noCUDA = false;
          size_t chosen = infos.size();
          for (size_t i = 0; i < infos.size() && chosen == infos.size(); ++i) {
            if (uuid.empty() ? index < 0 || infos[i].index == index : uuidKey(infos[i].uuid) == uuidKey(uuid)) {
              chosen = i;
            }
          }
          if (chosen == infos.size()) {
            why = "no CUDA device " + (uuid.empty() ? std::to_string(index) : uuid);
          } else {
            CUcontext context = nullptr;
            CUresult err = kCudaSuccess;
            driverCall(hooks, [&] { err = cu->DevicePrimaryCtxRetain(&context, devices[chosen]); });
            if (err != kCudaSuccess || !context) {
              why = "cuDevicePrimaryCtxRetain: " + errorName(err);
            } else {
              result.reset(new Device(hooks, cu, infos[chosen], devices[chosen], context));
              if (hooks.log) {
                const DeviceInfo &d = infos[chosen];
                hooks.log(LogLevel::Info, "CUDA: " + d.name + " (" + d.uuid + ", compute capability " +
                          std::to_string(d.computeMajor) + "." + std::to_string(d.computeMinor) + ", PCI " + d.pciBusId + ", driver " +
                          std::to_string(d.driverVersion) + ")");
              }
            }
          }
        }
        if (!result && hooks.log) {
          // no driver or no device is the normal case on most machines: Info; a device that exists
          // and fails: Error
          hooks.log(noCUDA ? LogLevel::Info : LogLevel::Error, "CUDA: " + why);
        }
        if (error) {
          *error = result ? std::string() : why;
        }
        return result;
      }

      std::unique_ptr<Device> createDevice(const Hooks &hooks, int index, std::string *error)
      {
        return DeviceFactory::make(hooks, index, std::string(), error);
      }

      std::unique_ptr<Device> createDeviceWithUUID(const Hooks &hooks, const std::string &uuid, std::string *error)
      {
        if (uuid.empty()) {
          if (error) {
            *error = "no CUDA device UUID";
          }
          return nullptr;
        }
        return DeviceFactory::make(hooks, -1, uuid, error);
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Device

      Device::Device(const Hooks &hooks, std::shared_ptr<const CUFunctions> cu, const DeviceInfo &info, int device,
                     void *context)
        : _hooks(hooks), _cu(std::move(cu)), _info(info), _device(device), _context(context)
      {
      }

      Device::~Device()
      {
        try {
          withContext([this] {
            for (void *s : _idle) {
              _cu->StreamDestroy(static_cast<CUstream>(s));
            }
            return kCudaSuccess;
          });
          driverCall([this] { _cu->DevicePrimaryCtxRelease(_device); });
        } catch (...) {
          // (Hooks::driverCall threw: the streams and the context are leaked; C.36)
        }
      }

      void Device::log(LogLevel level, const std::string &message) const
      {
        if (_hooks.log) {
          _hooks.log(level, message);
        }
      }

      void Device::driverCall(const std::function<void()> &call) const
      {
        Detail::driverCall(_hooks, call);
      }

      int Device::withContext(const std::function<int()> &call) const
      {
        CUresult err = kCudaSuccess;
        driverCall([&] {
          err = _cu->CtxPushCurrent(static_cast<CUcontext>(_context));
          if (err != kCudaSuccess) {
            return;
          }
          // popped also if call throws (R.1)
          struct Pop {
            const CUFunctions &cu;
            ~Pop()
            {
              CUcontext popped = nullptr;
              cu.CtxPopCurrent(&popped);
            }
          } pop = { *_cu };
          err = call();
        });
        return err;
      }

      bool Device::checkError(int err) const
      {
        if (stickyError(err) && _usable.exchange(false)) {
          log(LogLevel::Error, "CUDA: " + errorName(err) + ": the device's context is unusable from now on");
        }
        return err == kCudaSuccess;
      }

      int Device::streamCount() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _streams;
      }

      void *Device::takeStream(std::string &error)
      {
        {
          std::lock_guard<std::mutex> lock(_mutex);
          if (!_idle.empty()) {
            void *s = _idle.back();
            _idle.pop_back();
            return s;
          }
        }
        CUstream s = nullptr;
        const CUresult err = withContext([&] { return _cu->StreamCreate(&s, kCuStreamNonBlocking); });
        if (err != kCudaSuccess || !s) {
          error = "cuStreamCreate: " + errorName(err);
          log(LogLevel::Error, "CUDA: " + error);
          return nullptr;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        ++_streams;
        return s;
      }

      void Device::giveBack(void *stream)
      {
        std::lock_guard<std::mutex> lock(_mutex);
        _idle.push_back(stream);
      }

      void Device::discard(void *stream)
      {
        withContext([&] { return _cu->StreamDestroy(static_cast<CUstream>(stream)); });
        std::lock_guard<std::mutex> lock(_mutex);
        --_streams;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // ContextScope

      ContextScope::ContextScope(const Device &device)
        : _device(device), _ok(false)
      {
        CUresult err = kCudaSuccess;
        device.driverCall([&] { err = device.cu().CtxPushCurrent(static_cast<CUcontext>(device.nativeContext())); });
        _ok = err == kCudaSuccess;
        if (!_ok) {
          _error = "cuCtxPushCurrent: " + errorName(err);
          device.log(LogLevel::Error, "CUDA: " + _error);
        }
      }

      ContextScope::~ContextScope()
      {
        if (!_ok) {
          return;
        }
        try {
          _device.driverCall([this] {
            CUcontext popped = nullptr;
            _device.cu().CtxPopCurrent(&popped);
          });
        } catch (...) {
          // (Hooks::driverCall threw: the context stays current on this thread; C.36)
        }
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Stream

      Stream::Stream(Device &device)
        : _device(device)
      {
      }

      Stream::~Stream()
      {
        if (!_stream) {
          return;
        }
        try {
          if (synchronize()) {
            _device.giveBack(_stream);
          } else {
            // (e.g. CUDA_ERROR_LAUNCH_FAILED): not lent to another render
            _device.discard(_stream);
          }
        } catch (...) {
          // (Hooks::driverCall threw: the stream is leaked; C.36)
        }
      }

      bool Stream::open()
      {
        if (!_stream) {
          _errorText.clear();
          _stream = _device.takeStream(_errorText);
        }
        return _stream != nullptr;
      }

      bool Stream::synchronize()
      {
        if (!_stream) {
          return true;
        }
        _error = _device.withContext([&] { return _device.cu().StreamSynchronize(static_cast<CUstream>(_stream)); });
        if (_error != kCudaSuccess) {
          _device.log(LogLevel::Error, "CUDA: cuStreamSynchronize: " + errorName(_error));
        }
        return _device.checkError(_error);
      }

    } // CUDA
  } // Host
} // OFX
