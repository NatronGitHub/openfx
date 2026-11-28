#ifndef OFXH_CUDA_FUNCTIONS_H
#define OFXH_CUDA_FUNCTIONS_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// Private to openfx::HostSupportCUDA: the CUDA driver API types, enums and functions it uses
// (cuda.h, CUDA 11 and later, 64-bit), loaded at run time, so that no CUDA toolkit is needed at
// build time. These are this library's own declarations of the documented driver API (types,
// values, exported names), not NVIDIA's headers.

#include "ofxhCUDADevice.h"

#include <cstddef>
#include <memory>
#include <string>

namespace OFX {
  namespace Host {
    namespace CUDA {
      namespace Detail {

/// the calling convention of the CUDA driver API (CUDAAPI)
#if defined(_WIN32)
#define OFXH_CUDAAPI __stdcall
#else
#define OFXH_CUDAAPI
#endif

        static_assert(sizeof(void *) == 8, "openfx::HostSupportCUDA: the CUDA driver API is 64-bit only");

        typedef int CUresult;
        typedef int CUdevice;
        typedef struct CUctx_st *CUcontext;
        typedef struct CUstream_st *CUstream;
        typedef unsigned long long CUdeviceptr;  ///< (CUdeviceptr_v2, 64-bit)
        typedef struct CUarray_st *CUarray;

        static const CUresult kCudaSuccess = 0;
        static const CUresult kCudaErrorInvalidValue = 1;
        static const CUresult kCudaErrorOutOfMemory = 2;
        static const CUresult kCudaErrorNotInitialized = 3;
        static const CUresult kCudaErrorDeinitialized = 4;
        static const CUresult kCudaErrorStubLibrary = 34;
        static const CUresult kCudaErrorEccUncorrectable = 214;
        static const CUresult kCudaErrorNoDevice = 100;
        static const CUresult kCudaErrorInvalidDevice = 101;
        static const CUresult kCudaErrorInvalidContext = 201;
        static const CUresult kCudaErrorIllegalAddress = 700;
        static const CUresult kCudaErrorLaunchOutOfResources = 701;
        static const CUresult kCudaErrorLaunchTimeout = 702;
        static const CUresult kCudaErrorAssert = 710;
        static const CUresult kCudaErrorLaunchFailed = 719;
        static const CUresult kCudaErrorExternalDevice = 911;
        static const CUresult kCudaErrorUnknown = 999;

        /// CUdevice_attribute
        static const int kCuDeviceAttributePciBusId = 33;
        static const int kCuDeviceAttributePciDeviceId = 34;
        static const int kCuDeviceAttributePciDomainId = 50;
        static const int kCuDeviceAttributeComputeCapabilityMajor = 75;
        static const int kCuDeviceAttributeComputeCapabilityMinor = 76;

        /// CUmemorytype
        static const int kCuMemoryTypeHost = 1;
        static const int kCuMemoryTypeDevice = 2;

        /// cuStreamCreate's flags: CU_STREAM_NON_BLOCKING, a stream that doesn't synchronize with the
        /// legacy default stream (other renders' work there doesn't hold it up)
        static const unsigned kCuStreamNonBlocking = 1;

        /// CUuuid
        struct CUuuid {
          char bytes[16];
        };

        /// the errors after which the context is unusable (kStickyErrors):
        /// - those cuda.h documents with "This leaves the process in an inconsistent state and any
        ///   further CUDA work will return the same error": CUDA_ERROR_ILLEGAL_ADDRESS (700),
        ///   _LAUNCH_TIMEOUT (702), _HARDWARE_STACK_ERROR (714), _ILLEGAL_INSTRUCTION (715),
        ///   _MISALIGNED_ADDRESS (716), _INVALID_ADDRESS_SPACE (717), _INVALID_PC (718),
        ///   _LAUNCH_FAILED (719), _EXTERNAL_DEVICE (911);
        /// - CUDA_ERROR_MPS_CLIENT_TERMINATED (810): "To continue using CUDA, the process must be
        ///   terminated and relaunched";
        /// - CUDA_ERROR_ASSERT (710): "The context cannot be used anymore, and must be destroyed";
        /// - CUDA_ERROR_ECC_UNCORRECTABLE (214), "an uncorrectable ECC error was detected during
        ///   execution": not documented as sticky, but the device memory can't be trusted after it
        ///   (this library's choice)
        static const CUresult kStickyErrors[] = { 214, 700, 702, 710, 714, 715, 716, 717, 718, 719, 810, 911 };

        inline bool stickyError(CUresult err)
        {
          for (CUresult e : kStickyErrors) {
            if (err == e) {
              return true;
            }
          }
          return false;
        }

        /// CUDA_MEMCPY2D (CUDA_MEMCPY2D_v2), the argument of cuMemcpy2DAsync_v2
        struct CUDA_MEMCPY2D {
          size_t srcXInBytes;
          size_t srcY;
          int srcMemoryType;  ///< (CUmemorytype)
          const void *srcHost;
          CUdeviceptr srcDevice;
          CUarray srcArray;
          size_t srcPitch;
          size_t dstXInBytes;
          size_t dstY;
          int dstMemoryType;
          void *dstHost;
          CUdeviceptr dstDevice;
          CUarray dstArray;
          size_t dstPitch;
          size_t WidthInBytes;
          size_t Height;
        };
        static_assert(sizeof(CUDA_MEMCPY2D) == 128, "CUDA_MEMCPY2D: cuda.h's layout on 64-bit platforms");

        /// the CUDA driver library and the functions used here, by their exported names (cuda.h
        /// maps cuMemAlloc to cuMemAlloc_v2...); it stays loaded while a CUFunctions refers to it
        struct CUFunctions {
          std::shared_ptr<void> library;
          CUresult (OFXH_CUDAAPI *Init)(unsigned) = nullptr;
          CUresult (OFXH_CUDAAPI *DriverGetVersion)(int *) = nullptr;
          CUresult (OFXH_CUDAAPI *DeviceGetCount)(int *) = nullptr;
          CUresult (OFXH_CUDAAPI *DeviceGet)(CUdevice *, int) = nullptr;
          CUresult (OFXH_CUDAAPI *DeviceGetName)(char *, int, CUdevice) = nullptr;
          CUresult (OFXH_CUDAAPI *DeviceTotalMem)(size_t *, CUdevice) = nullptr;           ///< cuDeviceTotalMem_v2
          CUresult (OFXH_CUDAAPI *DeviceGetAttribute)(int *, int, CUdevice) = nullptr;
          CUresult (OFXH_CUDAAPI *DeviceGetUuid)(CUuuid *, CUdevice) = nullptr;   ///< _v2 if there (MIG), else v1
          CUresult (OFXH_CUDAAPI *DevicePrimaryCtxRetain)(CUcontext *, CUdevice) = nullptr;
          CUresult (OFXH_CUDAAPI *DevicePrimaryCtxRelease)(CUdevice) = nullptr;            ///< _v2
          CUresult (OFXH_CUDAAPI *CtxPushCurrent)(CUcontext) = nullptr;                    ///< _v2
          CUresult (OFXH_CUDAAPI *CtxPopCurrent)(CUcontext *) = nullptr;                   ///< _v2
          CUresult (OFXH_CUDAAPI *StreamCreate)(CUstream *, unsigned) = nullptr;
          CUresult (OFXH_CUDAAPI *StreamSynchronize)(CUstream) = nullptr;
          CUresult (OFXH_CUDAAPI *StreamDestroy)(CUstream) = nullptr;                      ///< _v2
          CUresult (OFXH_CUDAAPI *MemAlloc)(CUdeviceptr *, size_t) = nullptr;              ///< _v2
          CUresult (OFXH_CUDAAPI *MemFree)(CUdeviceptr) = nullptr;                         ///< _v2
          CUresult (OFXH_CUDAAPI *Memcpy2DAsync)(const CUDA_MEMCPY2D *, CUstream) = nullptr; ///< _v2
        };

        /// the functions, from hooks.getProcAddress or the driver library; nullptr and error if one
        /// is missing
        std::shared_ptr<const CUFunctions> loadCUFunctions(const Hooks &hooks, std::string &error);

        /// "CUDA_ERROR_OUT_OF_MEMORY (2)"... for the messages
        std::string errorName(CUresult err);

      } // Detail
    } // CUDA
  } // Host
} // OFX

#endif // OFXH_CUDA_FUNCTIONS_H
