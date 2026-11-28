// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhMetalDevice.h: the Metal devices and their command queues. Objective-C++ with ARC: the
// Metal objects are strong references in Device::Impl; a Queue holds its command queue as a +1
// reference (__bridge_retained) until it gives it back (__bridge_transfer).

#import <Metal/Metal.h>

#include "ofxhMetalDevice.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// the ARC runtime's autorelease pool functions (clang's "Objective-C Automatic Reference Counting"
// documentation, "Runtime support"), for AutoreleasePool
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

#if !__has_feature(objc_arc)
#error "ofxhMetalDevice.mm needs ARC (-fobjc-arc)"
#endif

namespace OFX {
  namespace Host {
    namespace Metal {

      struct Device::Impl {
        id<MTLDevice> device;                    ///< (strong)
        /// device, as nativeDevice() gives it: a plain pointer, so that returning it involves no ARC
        /// (returning a bridged object may hand it to the caller autoreleased, which leaks on a host
        /// thread without an autorelease pool)
        void *native = nullptr;
        mutable std::mutex mutex;
        std::vector<id<MTLCommandQueue>> idle;   ///< idle queues (strong)
        int queues = 0;                          ///< queues created
      };

      /// info of device, the index-th of MTLCopyAllDevices()
      static DeviceInfo deviceInfo(id<MTLDevice> device, int index, id<MTLDevice> systemDefault)
      {
        DeviceInfo info;
        info.index = index;
        info.name = device.name ? device.name.UTF8String : "";
        info.registryID = device.registryID;
        info.systemDefault = systemDefault && device.registryID == systemDefault.registryID;
        info.unifiedMemory = device.hasUnifiedMemory;
        info.recommendedMaxWorkingSetSize = device.recommendedMaxWorkingSetSize;
        info.maxBufferLength = device.maxBufferLength;
        return info;
      }

      std::vector<DeviceInfo> listDevices(const Hooks &, std::string *error)
      {
        std::vector<DeviceInfo> infos;
        @autoreleasepool {
          NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
          id<MTLDevice> systemDefault = MTLCreateSystemDefaultDevice();
          for (NSUInteger i = 0; i < devices.count; ++i) {
            infos.push_back(deviceInfo(devices[i], static_cast<int>(i), systemDefault));
          }
        }
        if (error) {
          *error = infos.empty() ? std::string("no Metal device") : std::string();
        }
        return infos;
      }

      /// creates the Devices (a friend of Device)
      struct DeviceFactory {
        /// the device that pick(devices, systemDefault) chooses in MTLCopyAllDevices(), its index or
        /// devices.count if none (then notFound() says why)
        static std::unique_ptr<Device> create(const Hooks &hooks, std::string *error,
                                              const std::function<NSUInteger(NSArray<id<MTLDevice>> *, id<MTLDevice>)> &pick,
                                              const std::function<std::string()> &notFound)
        {
          std::string why;
          std::unique_ptr<Device> result;
          @autoreleasepool {
            NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
            id<MTLDevice> systemDefault = MTLCreateSystemDefaultDevice();
            const NSUInteger chosen = devices.count > 0 ? pick(devices, systemDefault) : devices.count;
            if (devices.count == 0) {
              why = "no Metal device";
            } else if (chosen >= devices.count) {
              why = notFound();
            } else {
              std::unique_ptr<Device::Impl> impl(new Device::Impl);
              impl->device = devices[chosen];
              impl->native = (__bridge void *)impl->device;
              const DeviceInfo info = deviceInfo(impl->device, static_cast<int>(chosen), systemDefault);
              result.reset(new Device(hooks, info, std::move(impl)));
              if (hooks.log) {
                hooks.log(LogLevel::Info, "Metal: " + info.name);
              }
            }
          }
          if (!result && hooks.log) {
            hooks.log(LogLevel::Error, "Metal: " + why);
          }
          if (error) {
            *error = result ? std::string() : why;
          }
          return result;
        }
      };

      std::unique_ptr<Device> createDevice(const Hooks &hooks, int index, std::string *error)
      {
        return DeviceFactory::create(hooks, error, [index](NSArray<id<MTLDevice>> *devices, id<MTLDevice> systemDefault) {
          if (index >= 0) {
            return static_cast<NSUInteger>(index);
          }
          // by default the system default device, else the first device
          for (NSUInteger i = 0; i < devices.count; ++i) {
            if (systemDefault && devices[i].registryID == systemDefault.registryID) {
              return i;
            }
          }
          return NSUInteger(0);
        }, [index] { return "no Metal device " + std::to_string(index); });
      }

      std::unique_ptr<Device> createDeviceWithRegistryID(const Hooks &hooks, unsigned long long registryID,
                                                         std::string *error)
      {
        return DeviceFactory::create(hooks, error, [registryID](NSArray<id<MTLDevice>> *devices, id<MTLDevice>) {
          NSUInteger i = 0;
          while (i < devices.count && devices[i].registryID != registryID) {
            ++i;
          }
          return i;
        }, [registryID] { return "no Metal device of registry ID " + std::to_string(registryID); });
      }

      ////////////////////////////////////////////////////////////////////////////////
      // AutoreleasePool

      AutoreleasePool::AutoreleasePool() : _pool(objc_autoreleasePoolPush())
      {
      }

      AutoreleasePool::~AutoreleasePool()
      {
        objc_autoreleasePoolPop(_pool);
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Device

      Device::Device(const Hooks &hooks, const DeviceInfo &info, std::unique_ptr<Impl> impl)
        : _hooks(hooks), _info(info), _impl(std::move(impl))
      {
      }

      Device::~Device()
      {
        // (ARC releases the device and the idle queues, in a pool: a Metal object's dealloc may
        // autorelease, and the host's thread may have none)
        @autoreleasepool {
          _impl.reset();
        }
      }

      void *Device::nativeDevice() const
      {
        return _impl->native;
      }

      void Device::log(LogLevel level, const std::string &message) const
      {
        if (_hooks.log) {
          _hooks.log(level, message);
        }
      }

      int Device::queueCount() const
      {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        return _impl->queues;
      }

      void *Device::takeQueue(std::string &error)
      {
        {
          std::lock_guard<std::mutex> lock(_impl->mutex);
          if (!_impl->idle.empty()) {
            id<MTLCommandQueue> q = _impl->idle.back();
            _impl->idle.pop_back();
            return (__bridge_retained void *)q;
          }
        }
        @autoreleasepool { // (a host's render thread may have no pool)
          id<MTLCommandQueue> q = [_impl->device newCommandQueue];
          if (!q) {
            error = "newCommandQueue failed";
            log(LogLevel::Error, "Metal: " + error);
            return nullptr;
          }
          q.label = @"OFX HostSupport render";
          std::lock_guard<std::mutex> lock(_impl->mutex);
          ++_impl->queues;
          return (__bridge_retained void *)q;
        }
      }

      void Device::giveBack(void *queue)
      {
        id<MTLCommandQueue> q = (__bridge_transfer id<MTLCommandQueue>)queue;
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->idle.push_back(q);
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Queue

      Queue::Queue(Device &device)
        : _device(device), _queue(device.takeQueue(_error))
      {
      }

      Queue::~Queue()
      {
        if (_queue) {
          // (no wait: the RenderSession waited for the plug-in's work on its buffers)
          _device.giveBack(_queue);
        }
      }

    } // Metal
  } // Host
} // OFX
