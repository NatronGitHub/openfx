// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhOpenCLDevice.h: the OpenCL library, the devices, their contexts and command queues.

#include "ofxhOpenCLDevice.h"
#include "ofxhOpenCLFunctions.h"
#include "../gpu/ofxhGPULibrary.h"

#include <utility>

namespace OFX {
  namespace Host {
    namespace OpenCL {
      namespace Detail {

        std::string errorName(cl_int err)
        {
          const char *name = nullptr;
          switch (err) {
          case -1: name = "CL_DEVICE_NOT_FOUND"; break;
          case -2: name = "CL_DEVICE_NOT_AVAILABLE"; break;
          case kCLMemObjectAllocationFailure: name = "CL_MEM_OBJECT_ALLOCATION_FAILURE"; break;
          case kCLOutOfResources: name = "CL_OUT_OF_RESOURCES"; break;
          case kCLOutOfHostMemory: name = "CL_OUT_OF_HOST_MEMORY"; break;
          case -10: name = "CL_IMAGE_FORMAT_NOT_SUPPORTED"; break;
          case -11: name = "CL_BUILD_PROGRAM_FAILURE"; break;
          case -30: name = "CL_INVALID_VALUE"; break;
          case -36: name = "CL_INVALID_COMMAND_QUEUE"; break;
          case -38: name = "CL_INVALID_MEM_OBJECT"; break;
          case -40: name = "CL_INVALID_IMAGE_SIZE"; break;
          case -42: name = "CL_INVALID_BINARY"; break;
          case -43: name = "CL_INVALID_BUILD_OPTIONS"; break;
          case -61: name = "CL_INVALID_BUFFER_SIZE"; break;
          case -1001: name = "CL_PLATFORM_NOT_FOUND_KHR"; break;
          default: break;
          }
          return std::string(name ? name : "OpenCL error") + " (" + std::to_string(err) + ")";
        }

        /// the default loader: the ICD loader (or the macOS framework), never unloaded: drivers
        /// may not survive an unload
        static std::shared_ptr<void> openLibrary(std::string &error)
        {
#if defined(_WIN32)
          static const char *const names[] = { "OpenCL.dll", nullptr };
#elif defined(__APPLE__)
          static const char *const names[] = { "/System/Library/Frameworks/OpenCL.framework/OpenCL", nullptr };
#else
          static const char *const names[] = { "libOpenCL.so.1", "libOpenCL.so", nullptr };
#endif
          return GPU::openLibrary(names, error);
        }

        std::shared_ptr<const CLFunctions> loadCLFunctions(const Hooks &hooks, std::string &error)
        {
          auto cl = std::make_shared<CLFunctions>();
          std::function<void *(const char *)> get = hooks.getProcAddress;
          if (!get) {
            cl->library = openLibrary(error);
            if (!cl->library) {
              return nullptr;
            }
            void *library = cl->library.get();
            get = [library](const char *name) { return GPU::librarySymbol(library, name); };
          }
#define OFXH_CL(f) \
          cl->f = reinterpret_cast<decltype(cl->f)>(get("cl" #f)); \
          if (!cl->f) { error = "cl" #f " not found"; return nullptr; }
          OFXH_CL(GetPlatformIDs)
          OFXH_CL(GetPlatformInfo)
          OFXH_CL(GetDeviceIDs)
          OFXH_CL(GetDeviceInfo)
          OFXH_CL(CreateContext)
          OFXH_CL(ReleaseContext)
          OFXH_CL(CreateCommandQueue)
          OFXH_CL(ReleaseCommandQueue)
          OFXH_CL(CreateBuffer)
          OFXH_CL(CreateImage2D)
          OFXH_CL(ReleaseMemObject)
          OFXH_CL(EnqueueWriteBuffer)
          OFXH_CL(EnqueueReadBuffer)
          OFXH_CL(EnqueueWriteBufferRect)
          OFXH_CL(EnqueueReadBufferRect)
          OFXH_CL(EnqueueWriteImage)
          OFXH_CL(EnqueueReadImage)
          OFXH_CL(Finish)
          OFXH_CL(GetCommandQueueInfo)
          OFXH_CL(CreateProgramWithSource)
          OFXH_CL(CreateProgramWithBinary)
          OFXH_CL(BuildProgram)
          OFXH_CL(GetProgramInfo)
          OFXH_CL(GetProgramBuildInfo)
          OFXH_CL(ReleaseProgram)
#undef OFXH_CL
          return cl;
        }

        static void driverCall(const Hooks &hooks, const std::function<void()> &call)
        {
          if (hooks.driverCall) {
            hooks.driverCall(call);
          } else {
            call();
          }
        }

        static std::string platformString(const CLFunctions &cl, cl_platform_id p, cl_uint what)
        {
          size_t n = 0;
          if (cl.GetPlatformInfo(p, what, 0, nullptr, &n) != kCLSuccess || n == 0) {
            return std::string();
          }
          std::string s(n, '\0');
          cl.GetPlatformInfo(p, what, n, &s[0], nullptr);
          return std::string(s.c_str());
        }

        static std::string deviceString(const CLFunctions &cl, cl_device_id d, cl_uint what)
        {
          size_t n = 0;
          if (cl.GetDeviceInfo(d, what, 0, nullptr, &n) != kCLSuccess || n == 0) {
            return std::string();
          }
          std::string s(n, '\0');
          cl.GetDeviceInfo(d, what, n, &s[0], nullptr);
          return std::string(s.c_str());
        }

        template <typename T>
        static T deviceValue(const CLFunctions &cl, cl_device_id d, cl_uint what)
        {
          T v = T();
          if (cl.GetDeviceInfo(d, what, sizeof(v), &v, nullptr) != kCLSuccess) {
            return T();
          }
          return v;
        }

        /// the devices, with their ids; false and error if there are none
        static bool enumerate(const Hooks &hooks, const CLFunctions &cl, std::vector<DeviceInfo> &infos,
                              std::vector<cl_platform_id> &platformIds, std::vector<cl_device_id> &deviceIds,
                              std::string &error)
        {
          cl_int err = kCLSuccess;
          driverCall(hooks, [&] {
            cl_uint n = 0;
            err = cl.GetPlatformIDs(0, nullptr, &n);
            if (err == kCLSuccess && n > 0) {
              platformIds.resize(n);
              err = cl.GetPlatformIDs(n, platformIds.data(), nullptr);
            }
            if (err != kCLSuccess) {
              platformIds.clear();
              return;
            }
            for (cl_uint p = 0; p < platformIds.size(); ++p) {
              cl_uint nd = 0;
              if (cl.GetDeviceIDs(platformIds[p], kCLDeviceTypeAll, 0, nullptr, &nd) != kCLSuccess || nd == 0) {
                continue; // (CL_DEVICE_NOT_FOUND)
              }
              std::vector<cl_device_id> ids(nd);
              if (cl.GetDeviceIDs(platformIds[p], kCLDeviceTypeAll, nd, ids.data(), nullptr) != kCLSuccess) {
                continue;
              }
              for (cl_uint d = 0; d < nd; ++d) {
                DeviceInfo info;
                info.platform = static_cast<int>(p);
                info.device = static_cast<int>(d);
                info.platformName = platformString(cl, platformIds[p], kCLPlatformName);
                info.platformVersion = platformString(cl, platformIds[p], kCLPlatformVersion);
                info.name = deviceString(cl, ids[d], kCLDeviceName);
                info.vendor = deviceString(cl, ids[d], kCLDeviceVendor);
                info.version = deviceString(cl, ids[d], kCLDeviceVersion);
                info.driverVersion = deviceString(cl, ids[d], kCLDriverVersion);
                info.gpu = (deviceValue<cl_ulong>(cl, ids[d], kCLDeviceType) & kCLDeviceTypeGPU) != 0;
                info.imageSupport = deviceValue<cl_bool>(cl, ids[d], kCLDeviceImageSupport) != 0;
                info.globalMemSize = deviceValue<cl_ulong>(cl, ids[d], kCLDeviceGlobalMemSize);
                info.maxAllocSize = deviceValue<cl_ulong>(cl, ids[d], kCLDeviceMaxMemAllocSize);
                infos.push_back(info);
                deviceIds.push_back(ids[d]);
              }
            }
          });
          if (infos.empty()) {
            error = platformIds.empty() ? (err == kCLSuccess ? std::string("no OpenCL platform")
                                                             : "clGetPlatformIDs: " + errorName(err))
                                        : std::string("no OpenCL device");
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
        std::shared_ptr<const CLFunctions> cl = loadCLFunctions(hooks, why);
        std::vector<cl_platform_id> platforms;
        std::vector<cl_device_id> devices;
        if (cl) {
          enumerate(hooks, *cl, infos, platforms, devices, why);
        }
        if (error) {
          *error = infos.empty() ? why : std::string();
        }
        return infos;
      }

      std::unique_ptr<Device> createDevice(const Hooks &hooks, int platform, int device, std::string *error)
      {
        std::string why;
        std::unique_ptr<Device> result;
        std::shared_ptr<const CLFunctions> cl = loadCLFunctions(hooks, why);
        std::vector<DeviceInfo> infos;
        std::vector<cl_platform_id> platforms;
        std::vector<cl_device_id> devices;
        if (cl && enumerate(hooks, *cl, infos, platforms, devices, why)) {
          // the device asked for; by default the first GPU, else the first device
          size_t chosen = infos.size();
          for (size_t i = 0; i < infos.size() && chosen == infos.size(); ++i) {
            if (platform < 0 && device < 0 ? infos[i].gpu : (infos[i].platform == platform && infos[i].device == device)) {
              chosen = i;
            }
          }
          if (chosen == infos.size() && platform < 0 && device < 0) {
            chosen = 0;
          }
          if (chosen == infos.size()) {
            why = "no OpenCL device " + std::to_string(device) + " on platform " + std::to_string(platform);
          } else {
            const cl_context_properties props[] = {
              kCLContextPlatform,
              reinterpret_cast<cl_context_properties>(platforms[static_cast<size_t>(infos[chosen].platform)]), 0
            };
            cl_int err = kCLSuccess;
            cl_context context = nullptr;
            driverCall(hooks, [&] { context = cl->CreateContext(props, 1, &devices[chosen], nullptr, nullptr, &err); });
            if (!context) {
              why = "clCreateContext: " + errorName(err);
            } else {
              result.reset(new Device(hooks, cl, infos[chosen], devices[chosen], context));
              if (hooks.log) {
                hooks.log(LogLevel::Info, "OpenCL: " + infos[chosen].name + " (" + infos[chosen].vendor + ", " + infos[chosen].version +
                          ") on " + infos[chosen].platformName);
              }
            }
          }
        }
        if (!result && hooks.log) {
          hooks.log(LogLevel::Error, "OpenCL: " + why);
        }
        if (error) {
          *error = result ? std::string() : why;
        }
        return result;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Device

      Device::Device(const Hooks &hooks, std::shared_ptr<const CLFunctions> cl, const DeviceInfo &info,
                     void *device, void *context)
        : _hooks(hooks), _cl(std::move(cl)), _info(info), _device(device), _context(context)
        , _cacheDirectory(privateCacheDirectory(hooks))
      {
      }

      Device::~Device()
      {
        try {
          driverCall([this] {
            for (void *q : _idle) {
              _cl->ReleaseCommandQueue(static_cast<cl_command_queue>(q));
            }
            _cl->ReleaseContext(static_cast<cl_context>(_context));
          });
        } catch (...) {
          // (Hooks::driverCall threw: the context and queues are leaked; C.36)
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

      int Device::queueCount() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _queues;
      }

      void *Device::takeQueue(std::string &error)
      {
        {
          std::lock_guard<std::mutex> lock(_mutex);
          if (!_idle.empty()) {
            void *q = _idle.back();
            _idle.pop_back();
            return q;
          }
        }
        cl_int err = kCLSuccess;
        cl_command_queue q = nullptr;
        // properties 0: in order, no profiling
        driverCall([&] { q = _cl->CreateCommandQueue(static_cast<cl_context>(_context), static_cast<cl_device_id>(_device), 0, &err); });
        if (!q) {
          error = "clCreateCommandQueue: " + errorName(err);
          log(LogLevel::Error, "OpenCL: " + error);
          return nullptr;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        ++_queues;
        return q;
      }

      void Device::giveBack(void *queue)
      {
        std::lock_guard<std::mutex> lock(_mutex);
        _idle.push_back(queue);
      }

      void Device::discard(void *queue)
      {
        driverCall([&] { _cl->ReleaseCommandQueue(static_cast<cl_command_queue>(queue)); });
        std::lock_guard<std::mutex> lock(_mutex);
        --_queues;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Queue

      Queue::Queue(Device &device)
        : _device(device), _queue(device.takeQueue(_error))
      {
      }

      Queue::~Queue()
      {
        if (!_queue) {
          return;
        }
        try {
          if (finish()) {
            _device.giveBack(_queue);
          } else {
            // (e.g. CL_OUT_OF_RESOURCES after a failed kernel): not lent to another render
            _device.discard(_queue);
          }
        } catch (...) {
          // (Hooks::driverCall threw: the queue is leaked; C.36)
        }
      }

      bool Queue::finish()
      {
        if (!_queue) {
          return false;
        }
        cl_int err = kCLSuccess;
        _device.driverCall([&] { err = _device.cl().Finish(static_cast<cl_command_queue>(_queue)); });
        if (err != kCLSuccess) {
          _device.log(LogLevel::Error, "OpenCL: clFinish: " + errorName(err));
        }
        return err == kCLSuccess;
      }

    } // OpenCL
  } // Host
} // OFX
