// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <mutex>
#include <map>
#include <string>
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

// The other formats (bytes, shorts, and floats with 1 or 3 components): packed rows of p_Width
// pixels of p_Components components, one program per depth (one kernel each, so that the cache
// below builds each program once). Integer components are clamped to [0, max] and rounded to the
// nearest value, floats are not clamped (as GPUGain.cpp's scaledComponent). FP_CONTRACT OFF: no
// contraction of the product and the rounding's + 0.5f into a fused multiply-add, which would round
// differently from the CPU path. OpenCL 1.1 (as ofxGPURender.h asks) has the byte-addressable
// stores of the uchar and ushort kernels.
// (KernelSourceFormat, after the definitions of NAME, PIX and SCALED)
static const char *KernelSourceFormat = R"CL(
#pragma OPENCL FP_CONTRACT OFF
float ClampTo(float v, float m) { return v > 0.f ? (v < m ? v : m) : 0.f; }
#define SCALED_UCHAR(v) ((uchar)(ClampTo((v), 255.f) + 0.5f))
#define SCALED_USHORT(v) ((ushort)(ClampTo((v), 65535.f) + 0.5f))
#define SCALED_FLOAT(v) (v)

__kernel void NAME(
   int p_Width,
   int p_Height,
   int p_Components,
   float p_GainR,
   float p_GainG,
   float p_GainB,
   float p_GainA,
   __global const PIX* p_Input,
   __global PIX* p_Output)
{
   const int x = get_global_id(0);
   const int y = get_global_id(1);

   if ((x < p_Width) && (y < p_Height))
   {
       const float gain[4] = { p_GainR, p_GainG, p_GainB, p_GainA };
       const int index = ((y * p_Width) + x) * p_Components;
       for (int c = 0; c < p_Components; ++c)
       {
           p_Output[index + c] = SCALED(p_Input[index + c] * gain[c]);
       }
   }
}
)CL";

static const std::string KernelSourceBuffersByte = std::string(
    "#define NAME GainAdjustKernelBuffersByte\n#define PIX uchar\n#define SCALED SCALED_UCHAR\n") + KernelSourceFormat;
static const std::string KernelSourceBuffersShort = std::string(
    "#define NAME GainAdjustKernelBuffersShort\n#define PIX ushort\n#define SCALED SCALED_USHORT\n") + KernelSourceFormat;
static const std::string KernelSourceBuffersFloat = std::string(
    "#define NAME GainAdjustKernelBuffersFloat\n#define PIX float\n#define SCALED SCALED_FLOAT\n") + KernelSourceFormat;

// Images: read_imagef and write_imagef convert the image's channel type: "normalized integer"
// channels are read as [0, 1] and written with saturation, so that integer depths are clamped, as in
// the buffer kernels. Their rounding is the implementation's: OpenCL 1.1, section 8.3.1.2,
// "Converting floating-point values to normalized integer channel data types", gives as the
// preferred method convert_uchar_sat_rte(f * 255.0f) for CL_UNORM_INT8 and
// convert_ushort_sat_rte(f * 65535.0f) for CL_UNORM_INT16 (round to nearest even), but "OpenCL
// implementations may choose to approximate the rounding mode", with "the absolute error" of the
// conversion "<= 0.6". The buffer kernels round halves up, so an Images result can differ from
// theirs (and the CPU path's) by one step. The gains are channelGains', so that any channel order
// works (e.g. CL_A or CL_R for Alpha).
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

typedef std::mutex Locker;
typedef std::lock_guard<std::mutex> AutoLock;

// One kernel per OpenCL context and kernel source. ofxGPURender.h: "the plug-in should keep a
// separate set of kernels per OpenCL context (e.g., using a map). The OpenCL context can be found
// from the command queue using clGetCommandQueueInfo with CL_QUEUE_CONTEXT."
// The spec also says "Do not retain OpenCL objects without a matching release within the render
// action": the cached kernels are created by the plug-in (not retained host objects) and kept for
// the life of the plug-in, as in upstream's sample; the context is only used as a key (not
// retained), and the program is released once the kernel is created.
// A cl_kernel isn't thread-safe between clSetKernelArg and clEnqueueNDRangeKernel, so RunKernel
// holds the lock through both. Failed builds are not cached (they are retried at the next render).
typedef std::map<std::pair<cl_context, const char*>, cl_kernel> KernelMap;

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
cl_kernel GetKernel(cl_context p_Context, const char* p_Source, const char* p_Name)
{
    KernelMap& kernels = GetKernelMap();
    const KernelMap::key_type key(p_Context, p_Source);
    KernelMap::const_iterator it = kernels.find(key);
    if (it != kernels.end())
    {
        return it->second;
    }

    cl_int error = CL_SUCCESS;
    cl_program program = clCreateProgramWithSource(p_Context, 1, &p_Source, NULL, &error);
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

// the status of an OpenCL error: 0 (success), 1 (failed), 2 (out of memory), see GPUGain.cpp
int KernelStatus(cl_int p_Error)
{
    switch (p_Error)
    {
    case CL_SUCCESS:
        return 0;
    case CL_OUT_OF_RESOURCES:
    case CL_OUT_OF_HOST_MEMORY:
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
        return 2;
    default:
        return 1;
    }
}

// returns 0, 1 or 2 (see KernelStatus). p_Input and p_Output are cl_mem (buffers or images).
// p_Components: the components argument of the format kernels (0 for the float RGBA and images
// kernels, which have none)
int RunKernel(const char* p_Source, const char* p_Name, void* p_CmdQ, int p_Width, int p_Height, int p_Components,
              const float* p_Gain, const void* p_Input, void* p_Output)
{
    if ((p_Width <= 0) || (p_Height <= 0))
    {
        return 0; // nothing to do (and a zero global work size is an error)
    }
    cl_command_queue cmdQ = static_cast<cl_command_queue>(p_CmdQ);
    if (!cmdQ)
    {
        return 1;
    }
    cl_context context = NULL;
    if (CheckError(clGetCommandQueueInfo(cmdQ, CL_QUEUE_CONTEXT, sizeof(cl_context), &context, NULL),
                   "Unable to get the context") != CL_SUCCESS || !context)
    {
        return 1;
    }
    cl_mem input = static_cast<cl_mem>(const_cast<void*>(p_Input));
    cl_mem output = static_cast<cl_mem>(p_Output);

    AutoLock lock(GetLocker());

    cl_kernel kernel = GetKernel(context, p_Source, p_Name);
    if (!kernel)
    {
        return 1;
    }

    struct Arg { size_t size; const void* value; };
    const Arg args[] = {
        { sizeof(int), &p_Width }, { sizeof(int), &p_Height }, { sizeof(int), &p_Components },
        { sizeof(float), &p_Gain[0] }, { sizeof(float), &p_Gain[1] },
        { sizeof(float), &p_Gain[2] }, { sizeof(float), &p_Gain[3] },
        { sizeof(cl_mem), &input }, { sizeof(cl_mem), &output },
    };
    cl_uint index = 0;
    for (const Arg& arg : args)
    {
        if ((&arg == &args[2]) && !p_Components)
        {
            continue; // no components argument
        }
        const cl_int error = CheckError(clSetKernelArg(kernel, index++, arg.size, arg.value),
                                        "Unable to set a kernel argument");
        if (error != CL_SUCCESS)
        {
            return KernelStatus(error);
        }
    }

    // one work item per pixel; the implementation chooses the work-group size
    const size_t globalWorkSize[2] = { (size_t)p_Width, (size_t)p_Height };
    return KernelStatus(CheckError(clEnqueueNDRangeKernel(cmdQ, kernel, 2, NULL, globalWorkSize, NULL, 0, NULL, NULL),
                                   "Unable to enqueue the kernel"));
}

} // namespace

// p_Components: 1, 3 or 4; p_ComponentBytes: 1 (bytes), 2 (shorts) or 4 (floats). Float RGBA uses
// KernelSourceBuffers, the others the format kernels.
int RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, int p_Components, int p_ComponentBytes,
                           const float* p_Gain, const void* p_Input, void* p_Output)
{
    if ((p_Components != 1) && (p_Components != 3) && (p_Components != 4))
    {
        return 1;
    }
    switch (p_ComponentBytes)
    {
    case 1:
        return RunKernel(KernelSourceBuffersByte.c_str(), "GainAdjustKernelBuffersByte", p_CmdQ, p_Width, p_Height, p_Components,
                         p_Gain, p_Input, p_Output);
    case 2:
        return RunKernel(KernelSourceBuffersShort.c_str(), "GainAdjustKernelBuffersShort", p_CmdQ, p_Width, p_Height, p_Components,
                         p_Gain, p_Input, p_Output);
    case 4:
        if (p_Components == 4)
        {
            return RunKernel(KernelSourceBuffers, "GainAdjustKernelBuffers", p_CmdQ, p_Width, p_Height, 0, p_Gain, p_Input, p_Output);
        }
        return RunKernel(KernelSourceBuffersFloat.c_str(), "GainAdjustKernelBuffersFloat", p_CmdQ, p_Width, p_Height, p_Components,
                         p_Gain, p_Input, p_Output);
    default:
        return 1;
    }
}

int RunOpenCLKernelImages(void* p_CmdQ, int p_Width, int p_Height, const float* p_Gain, const void* p_Input, void* p_Output)
{
    return RunKernel(KernelSourceImages, "GainAdjustKernelImages", p_CmdQ, p_Width, p_Height, 0, p_Gain, p_Input, p_Output);
}
