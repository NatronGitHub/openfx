// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// std::mutex with C++11, else a platform mutex
#if __cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L)
#define GPUGAIN_STD_MUTEX 1
#include <mutex>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <map>
#include <stdio.h>

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

static const char *KernelSourceBuffers = "\n" \
"__kernel void GainAdjustKernelBuffers(                                 \n" \
"   int p_Width,                                                        \n" \
"   int p_Height,                                                       \n" \
"   float p_GainR,                                                      \n" \
"   float p_GainG,                                                      \n" \
"   float p_GainB,                                                      \n" \
"   float p_GainA,                                                      \n" \
"   __global const float* p_Input,                                      \n" \
"   __global float* p_Output)                                           \n" \
"{                                                                      \n" \
"   const int x = get_global_id(0);                                     \n" \
"   const int y = get_global_id(1);                                     \n" \
"                                                                       \n" \
"   if ((x < p_Width) && (y < p_Height))                                \n" \
"   {                                                                   \n" \
"       const int index = ((y * p_Width) + x) * 4;                      \n" \
"                                                                       \n" \
"       p_Output[index + 0] = p_Input[index + 0] * p_GainR;             \n" \
"       p_Output[index + 1] = p_Input[index + 1] * p_GainG;             \n" \
"       p_Output[index + 2] = p_Input[index + 2] * p_GainB;             \n" \
"       p_Output[index + 3] = p_Input[index + 3] * p_GainA;             \n" \
"   }                                                                   \n" \
"}                                                                      \n" \
"\n";

static const char *KernelSourceImages = "\n" \
"__constant sampler_t imageSampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;  \n" \
"                                                                                                           \n" \
"__kernel void GainAdjustKernelImages(                                                                      \n" \
"   int p_Width,                                                                                            \n" \
"   int p_Height,                                                                                           \n" \
"   float p_GainR,                                                                                          \n" \
"   float p_GainG,                                                                                          \n" \
"   float p_GainB,                                                                                          \n" \
"   float p_GainA,                                                                                          \n" \
"   __read_only  image2d_t p_Input,                                                                         \n" \
"   __write_only image2d_t p_Output)                                                                        \n" \
"{                                                                                                          \n" \
"   const int x = get_global_id(0);                                                                         \n" \
"   const int y = get_global_id(1);                                                                         \n" \
"                                                                                                           \n" \
"   if ((x < p_Width) && (y < p_Height))                                                                    \n" \
"   {                                                                                                       \n" \
"       int2 coord = (int2)(x, y);                                                                          \n" \
"       float4 out = read_imagef(p_Input, imageSampler, coord);                                             \n" \
"       out *= (float4)(p_GainR, p_GainG, p_GainB, p_GainA);                                                \n" \
"       write_imagef(p_Output, coord, out);                                                                 \n" \
"   }                                                                                                       \n" \
"}                                                                                                          \n" \
"\n";

namespace {

cl_int CheckError(cl_int p_Error, const char* p_Msg)
{
    if (p_Error != CL_SUCCESS)
    {
        fprintf(stderr, "GPUGain: %s [%d]\n", p_Msg, p_Error);
    }
    return p_Error;
}

#ifdef GPUGAIN_STD_MUTEX
typedef std::mutex Locker;
typedef std::lock_guard<std::mutex> AutoLock;
#else
class Locker
{
public:
    Locker()
    {
#ifdef _WIN32
        InitializeCriticalSection(&mutex);
#else
        pthread_mutex_init(&mutex, NULL);
#endif
    }

    ~Locker()
    {
#ifdef _WIN32
        DeleteCriticalSection(&mutex);
#else
        pthread_mutex_destroy(&mutex);
#endif
    }

    void lock()
    {
#ifdef _WIN32
        EnterCriticalSection(&mutex);
#else
        pthread_mutex_lock(&mutex);
#endif
    }

    void unlock()
    {
#ifdef _WIN32
        LeaveCriticalSection(&mutex);
#else
        pthread_mutex_unlock(&mutex);
#endif
    }

private:
    Locker(const Locker&);
    Locker& operator=(const Locker&);
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
};

class AutoLock
{
public:
    explicit AutoLock(Locker& p_Locker) : _locker(p_Locker) { _locker.lock(); }
    ~AutoLock() { _locker.unlock(); }
private:
    AutoLock(const AutoLock&);
    AutoLock& operator=(const AutoLock&);
    Locker& _locker;
};
#endif

// One kernel per command queue and kernel source (required for multi-GPU systems). A cl_kernel
// isn't thread-safe between clSetKernelArg and clEnqueueNDRangeKernel, so RunKernel holds the
// lock through both. Failed builds are not cached (they are retried at the next render).
typedef std::map<std::pair<cl_command_queue, const char*>, cl_kernel> KernelMap;

Locker& GetLocker()
{
    static Locker locker;
    return locker;
}

KernelMap& GetKernelMap()
{
    static KernelMap kernels;
    return kernels;
}

// call with the lock held; returns NULL on failure
cl_kernel GetKernel(cl_command_queue p_CmdQ, const char* p_Source, const char* p_Name)
{
    KernelMap& kernels = GetKernelMap();
    const KernelMap::key_type key(p_CmdQ, p_Source);
    KernelMap::const_iterator it = kernels.find(key);
    if (it != kernels.end())
    {
        return it->second;
    }

    cl_context clContext = NULL;
    if (CheckError(clGetCommandQueueInfo(p_CmdQ, CL_QUEUE_CONTEXT, sizeof(cl_context), &clContext, NULL),
                   "Unable to get the context") != CL_SUCCESS)
    {
        return NULL;
    }
    cl_int error = CL_SUCCESS;
    cl_program program = clCreateProgramWithSource(clContext, 1, &p_Source, NULL, &error);
    if (CheckError(error, "Unable to create program") != CL_SUCCESS)
    {
        return NULL;
    }
    error = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    cl_kernel kernel = NULL;
    if (CheckError(error, "Unable to build program") == CL_SUCCESS)
    {
        kernel = clCreateKernel(program, p_Name, &error);
        if (CheckError(error, "Unable to create kernel") != CL_SUCCESS)
        {
            kernel = NULL;
        }
    }
    // the kernel keeps a reference to its program
    clReleaseProgram(program);
    if (kernel)
    {
        kernels[key] = kernel;
    }
    return kernel;
}

// returns CL_SUCCESS or an OpenCL error. p_Input and p_Output are cl_mem (buffers or images)
cl_int RunKernel(const char* p_Source, const char* p_Name,
                 void* p_CmdQ, int p_Width, int p_Height, const float* p_Gain, const void* p_Input, void* p_Output)
{
    cl_command_queue cmdQ = static_cast<cl_command_queue>(p_CmdQ);
    if (!cmdQ)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    cl_mem input = static_cast<cl_mem>(const_cast<void*>(p_Input));
    cl_mem output = static_cast<cl_mem>(p_Output);

    AutoLock lock(GetLocker());

    cl_kernel kernel = GetKernel(cmdQ, p_Source, p_Name);
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }

    cl_uint count = 0;
    cl_int error = clSetKernelArg(kernel, count++, sizeof(int), &p_Width);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Height);
    error |= clSetKernelArg(kernel, count++, sizeof(float), &p_Gain[0]);
    error |= clSetKernelArg(kernel, count++, sizeof(float), &p_Gain[1]);
    error |= clSetKernelArg(kernel, count++, sizeof(float), &p_Gain[2]);
    error |= clSetKernelArg(kernel, count++, sizeof(float), &p_Gain[3]);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &input);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &output);
    if (CheckError(error, "Unable to set kernel arguments") != CL_SUCCESS)
    {
        return error;
    }

    // one work item per pixel; the implementation chooses the work-group size
    const size_t globalWorkSize[2] = { (size_t)p_Width, (size_t)p_Height };
    return CheckError(clEnqueueNDRangeKernel(cmdQ, kernel, 2, NULL, globalWorkSize, NULL, 0, NULL, NULL),
                      "Unable to enqueue the kernel");
}

} // namespace

int RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, const float* p_Gain, const void* p_Input, void* p_Output)
{
    return RunKernel(KernelSourceBuffers, "GainAdjustKernelBuffers", p_CmdQ, p_Width, p_Height, p_Gain, p_Input, p_Output);
}

int RunOpenCLKernelImages(void* p_CmdQ, int p_Width, int p_Height, const float* p_Gain, const void* p_Input, void* p_Output)
{
    return RunKernel(KernelSourceImages, "GainAdjustKernelImages", p_CmdQ, p_Width, p_Height, p_Gain, p_Input, p_Output);
}
