#ifndef OFXH_OPENCL_FUNCTIONS_H
#define OFXH_OPENCL_FUNCTIONS_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// Private to openfx::HostSupportOpenCL: the OpenCL 1.1 types, enums and functions it uses
// (CL/cl.h), loaded at run time, so that no OpenCL header or library is needed at build time.

#include "ofxhOpenCLDevice.h"

#include <cstdint>
#include <memory>
#include <string>

namespace OFX {
  namespace Host {
    namespace OpenCL {
      namespace Detail {

/// the calling convention of the OpenCL API (CL_API_CALL)
#if defined(_WIN32)
#define OFXH_CLAPI __stdcall
#else
#define OFXH_CLAPI
#endif

        typedef int32_t cl_int;
        typedef uint32_t cl_uint;
        typedef uint64_t cl_ulong;
        typedef cl_uint cl_bool;
        typedef struct _cl_platform_id *cl_platform_id;
        typedef struct _cl_device_id *cl_device_id;
        typedef struct _cl_context *cl_context;
        typedef struct _cl_command_queue *cl_command_queue;
        typedef struct _cl_mem *cl_mem;
        typedef struct _cl_event *cl_event;
        typedef intptr_t cl_context_properties;
        struct cl_image_format {
          cl_uint image_channel_order;
          cl_uint image_channel_data_type;
        };

        static const cl_int kCLSuccess = 0;
        static const cl_int kCLMemObjectAllocationFailure = -4;
        static const cl_int kCLOutOfResources = -5;
        static const cl_int kCLOutOfHostMemory = -6;
        static const cl_bool kCLTrue = 1;
        static const cl_uint kCLPlatformVersion = 0x0901;
        static const cl_uint kCLPlatformName = 0x0902;
        static const cl_ulong kCLDeviceTypeGPU = 1 << 2;
        static const cl_ulong kCLDeviceTypeAll = 0xFFFFFFFF;
        static const cl_uint kCLDeviceType = 0x1000;
        static const cl_uint kCLDeviceMaxMemAllocSize = 0x1010;
        static const cl_uint kCLDeviceImageSupport = 0x1016;
        static const cl_uint kCLDeviceGlobalMemSize = 0x101F;
        static const cl_uint kCLDeviceName = 0x102B;
        static const cl_uint kCLDeviceVendor = 0x102C;
        static const cl_uint kCLDriverVersion = 0x102D;
        static const cl_uint kCLDeviceVersion = 0x102F;
        static const cl_context_properties kCLContextPlatform = 0x1084;
        static const cl_ulong kCLMemReadWrite = 1 << 0;
        static const cl_uint kCLRGBA = 0x10B5;
        static const cl_uint kCLUnormInt8 = 0x10D2;
        static const cl_uint kCLUnormInt16 = 0x10D3;
        static const cl_uint kCLHalfFloat = 0x10DD;
        static const cl_uint kCLFloat = 0x10DE;

        /// the OpenCL library and the functions used here; it stays loaded while a CLFunctions
        /// refers to it
        struct CLFunctions {
          std::shared_ptr<void> library;
          cl_int (OFXH_CLAPI *GetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *) = nullptr;
          cl_int (OFXH_CLAPI *GetPlatformInfo)(cl_platform_id, cl_uint, size_t, void *, size_t *) = nullptr;
          cl_int (OFXH_CLAPI *GetDeviceIDs)(cl_platform_id, cl_ulong, cl_uint, cl_device_id *, cl_uint *) = nullptr;
          cl_int (OFXH_CLAPI *GetDeviceInfo)(cl_device_id, cl_uint, size_t, void *, size_t *) = nullptr;
          cl_context (OFXH_CLAPI *CreateContext)(const cl_context_properties *, cl_uint, const cl_device_id *,
                                                 void (OFXH_CLAPI *)(const char *, const void *, size_t, void *),
                                                 void *, cl_int *) = nullptr;
          cl_int (OFXH_CLAPI *ReleaseContext)(cl_context) = nullptr;
          cl_command_queue (OFXH_CLAPI *CreateCommandQueue)(cl_context, cl_device_id, cl_ulong, cl_int *) = nullptr;
          cl_int (OFXH_CLAPI *ReleaseCommandQueue)(cl_command_queue) = nullptr;
          cl_mem (OFXH_CLAPI *CreateBuffer)(cl_context, cl_ulong, size_t, void *, cl_int *) = nullptr;
          cl_mem (OFXH_CLAPI *CreateImage2D)(cl_context, cl_ulong, const cl_image_format *, size_t, size_t, size_t,
                                             void *, cl_int *) = nullptr;
          cl_int (OFXH_CLAPI *ReleaseMemObject)(cl_mem) = nullptr;
          cl_int (OFXH_CLAPI *EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void *,
                                                  cl_uint, const cl_event *, cl_event *) = nullptr;
          cl_int (OFXH_CLAPI *EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *,
                                                 cl_uint, const cl_event *, cl_event *) = nullptr;
          cl_int (OFXH_CLAPI *EnqueueWriteBufferRect)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *,
                                                      const size_t *, size_t, size_t, size_t, size_t, const void *,
                                                      cl_uint, const cl_event *, cl_event *) = nullptr;
          cl_int (OFXH_CLAPI *EnqueueReadBufferRect)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *,
                                                     const size_t *, size_t, size_t, size_t, size_t, void *, cl_uint,
                                                     const cl_event *, cl_event *) = nullptr;
          cl_int (OFXH_CLAPI *EnqueueWriteImage)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *,
                                                 size_t, size_t, const void *, cl_uint, const cl_event *,
                                                 cl_event *) = nullptr;
          cl_int (OFXH_CLAPI *EnqueueReadImage)(cl_command_queue, cl_mem, cl_bool, const size_t *, const size_t *,
                                                size_t, size_t, void *, cl_uint, const cl_event *, cl_event *) = nullptr;
          cl_int (OFXH_CLAPI *Finish)(cl_command_queue) = nullptr;
        };

        /// the functions, from hooks.getProcAddress or the ICD loader; nullptr and error if one is
        /// missing
        std::shared_ptr<const CLFunctions> loadCLFunctions(const Hooks &hooks, std::string &error);

        /// "CL_OUT_OF_RESOURCES (-5)"... for the messages
        std::string errorName(cl_int err);

      } // Detail
    } // OpenCL
  } // Host
} // OFX

#endif // OFXH_OPENCL_FUNCTIONS_H
