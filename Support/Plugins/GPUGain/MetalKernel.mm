// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#import <Metal/Metal.h>

// manual retain/release (newLibraryWithSource etc. return retained objects, released below)
#if __has_feature(objc_arc)
#error "MetalKernel.mm uses manual reference counting: compile it without -fobjc-arc"
#endif

#include <stdio.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

static const char* kernelSource =  \
"#include <metal_stdlib>\n" \
"using namespace metal; \n" \
"kernel void GainAdjustKernel(constant int& p_Width [[buffer (11)]], constant int& p_Height [[buffer (12)]], constant float& p_GainR [[buffer (13)]],    \n" \
"                             constant float& p_GainG [[buffer (14)]], constant float& p_GainB [[buffer (15)]], constant float& p_GainA [[buffer (16)]], \n" \
"                             const device float* p_Input [[buffer (0)]], device float* p_Output [[buffer (8)]], uint2 id [[ thread_position_in_grid ]]) \n" \
"{                                                                                                                                                       \n" \
"   if ((id.x < p_Width) && (id.y < p_Height))                                                                                                           \n" \
"   {                                                                                                                                                    \n" \
"       const int index = ((id.y * p_Width) + id.x) * 4;                                                                                                 \n" \
"       p_Output[index + 0] = p_Input[index + 0] * p_GainR;                                                                                              \n" \
"       p_Output[index + 1] = p_Input[index + 1] * p_GainG;                                                                                              \n" \
"       p_Output[index + 2] = p_Input[index + 2] * p_GainB;                                                                                              \n" \
"       p_Output[index + 3] = p_Input[index + 3] * p_GainA;                                                                                              \n" \
"   }                                                                                                                                                    \n" \
"}                                                                                                                                                       \n";

// The other formats (bytes, shorts, and floats with 1 or 3 components): packed rows of p_Width
// pixels of p_Components components, one kernel per depth. Integer components are clamped to
// [0, max] and rounded to the nearest value, floats are not clamped (as GPUGain.cpp's
// scaledComponent). Built with MTLMathModeSafe, unlike kernelSource (float RGBA, as upstream's
// sample); that mode doesn't forbid fusing the product with the rounding's + 0.5f into a
// multiply-add: if the compiler does, an integer result can be one step away from the CPU path's
// when the product isn't exact.
static const char* formatKernelSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

static float ClampTo(float v, float m) { return v > 0.f ? (v < m ? v : m) : 0.f; }
#define SCALED_UCHAR(v) ((uchar)(ClampTo((v), 255.f) + 0.5f))
#define SCALED_USHORT(v) ((ushort)(ClampTo((v), 65535.f) + 0.5f))
#define SCALED_FLOAT(v) (v)

#define GAIN_KERNEL(NAME, PIX, SCALED)                                                                     \
kernel void NAME(constant int& p_Width [[buffer (11)]], constant int& p_Height [[buffer (12)]],             \
                 constant float& p_GainR [[buffer (13)]], constant float& p_GainG [[buffer (14)]],          \
                 constant float& p_GainB [[buffer (15)]], constant float& p_GainA [[buffer (16)]],          \
                 constant int& p_Components [[buffer (17)]],                                                \
                 const device PIX* p_Input [[buffer (0)]], device PIX* p_Output [[buffer (8)]],             \
                 uint2 id [[ thread_position_in_grid ]])                                                    \
{                                                                                                          \
   if ((id.x < uint(p_Width)) && (id.y < uint(p_Height)))                                                  \
   {                                                                                                       \
       const float gain[4] = { p_GainR, p_GainG, p_GainB, p_GainA };                                       \
       const int index = ((id.y * p_Width) + id.x) * p_Components;                                         \
       for (int c = 0; c < p_Components; ++c)                                                              \
       {                                                                                                   \
           p_Output[index + c] = SCALED(p_Input[index + c] * gain[c]);                                     \
       }                                                                                                   \
   }                                                                                                       \
}

GAIN_KERNEL(GainAdjustKernelByte, uchar, SCALED_UCHAR)
GAIN_KERNEL(GainAdjustKernelShort, ushort, SCALED_USHORT)
GAIN_KERNEL(GainAdjustKernelFloat, float, SCALED_FLOAT)
)METAL";

// one compute pipeline per Metal device and kernel (a pipeline state belongs to the device that
// built it, and several queues may share a device); kept for the life of the plug-in
static std::mutex s_PipelineDeviceMutex;
typedef std::map<std::pair<id<MTLDevice>, std::string>, id<MTLComputePipelineState>> PipelineDeviceMap;
static PipelineDeviceMap s_PipelineDeviceMap;

// the pipeline of the kernel p_Name of p_Source on the device, built on first use (p_FastMath:
// kernelSource's options, else MTLMathModeSafe); nil on failure (not cached: retried next time)
static id<MTLComputePipelineState> GetPipelineState(id<MTLDevice> device, const char* p_Source, const char* p_Name,
                                                    bool p_FastMath)
{
    std::unique_lock<std::mutex> lock(s_PipelineDeviceMutex);

    const PipelineDeviceMap::key_type key(device, p_Name);
    const auto it = s_PipelineDeviceMap.find(key);
    if (it != s_PipelineDeviceMap.end())
    {
        return it->second;
    }

    NSError* err = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
#if defined(__MAC_15_0) && (__MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0)
    // fastMathEnabled is deprecated in macOS 15, which has mathMode
    if (@available(macOS 15.0, *))
    {
        options.mathMode = p_FastMath ? MTLMathModeFast : MTLMathModeSafe;
    }
    else
#endif
    {
        options.fastMathEnabled = p_FastMath ? YES : NO; // macOS 14 (and older SDKs): mathMode doesn't exist
    }
    id<MTLLibrary> metalLibrary = [device newLibraryWithSource:@(p_Source) options:options error:&err];
    [options release];
    if (!metalLibrary)
    {
        fprintf(stderr, "GPUGain: failed to load the Metal library, %s\n", err ? err.localizedDescription.UTF8String : "");
        return nil;
    }
    id<MTLFunction> kernelFunction = [metalLibrary newFunctionWithName:@(p_Name)];
    [metalLibrary release];
    if (!kernelFunction)
    {
        fprintf(stderr, "GPUGain: failed to retrieve the Metal kernel %s\n", p_Name);
        return nil;
    }
    err = nil;
    id<MTLComputePipelineState> pipelineState = [device newComputePipelineStateWithFunction:kernelFunction error:&err];
    [kernelFunction release];
    if (!pipelineState)
    {
        fprintf(stderr, "GPUGain: unable to compile the Metal kernel, %s\n", err ? err.localizedDescription.UTF8String : "");
        return nil;
    }

    s_PipelineDeviceMap[key] = pipelineState;
    return pipelineState;
}

// returns 0 (success) or 1 (the kernel couldn't be built or encoded), see GPUGain.cpp
// p_Components: 1, 3 or 4; p_ComponentBytes: 1 (bytes), 2 (shorts) or 4 (floats). Float RGBA uses
// kernelSource's GainAdjustKernel, the others formatKernelSource's kernels.
int RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, int p_Components, int p_ComponentBytes,
                   const float* p_Gain, const void* p_Input, void* p_Output)
{
    const bool floatRGBA = (p_Components == 4) && (p_ComponentBytes == 4);
    const char* name = floatRGBA ? "GainAdjustKernel" :
                       (p_ComponentBytes == 1) ? "GainAdjustKernelByte" :
                       (p_ComponentBytes == 2) ? "GainAdjustKernelShort" :
                       (p_ComponentBytes == 4) ? "GainAdjustKernelFloat" : NULL;
    if (!name || ((p_Components != 1) && (p_Components != 3) && (p_Components != 4)))
    {
        return 1;
    }
    if ((p_Width <= 0) || (p_Height <= 0))
    {
        return 0; // nothing to do
    }
    @autoreleasepool
    {
        id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue> >(p_CmdQ);
        if (!queue)
        {
            return 1;
        }
        id<MTLComputePipelineState> pipelineState = GetPipelineState(queue.device, floatRGBA ? kernelSource : formatKernelSource, name, floatRGBA);
        if (!pipelineState)
        {
            return 1;
        }

        id<MTLBuffer> srcDeviceBuf = static_cast<id<MTLBuffer> >(const_cast<void *>(p_Input));
        id<MTLBuffer> dstDeviceBuf = static_cast<id<MTLBuffer> >(p_Output);

        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        if (!commandBuffer)
        {
            return 1;
        }
        commandBuffer.label = @"GainAdjustKernel";

        id<MTLComputeCommandEncoder> computeEncoder = [commandBuffer computeCommandEncoder];
        if (!computeEncoder)
        {
            return 1;
        }
        [computeEncoder setComputePipelineState:pipelineState];

        const NSUInteger exeWidth = [pipelineState threadExecutionWidth];
        MTLSize threadGroupCount = MTLSizeMake(exeWidth, 1, 1);
        MTLSize threadGroups     = MTLSizeMake((p_Width + exeWidth - 1) / exeWidth, p_Height, 1);

        [computeEncoder setBuffer:srcDeviceBuf offset: 0 atIndex: 0];
        [computeEncoder setBuffer:dstDeviceBuf offset: 0 atIndex: 8];
        [computeEncoder setBytes:&p_Width length:sizeof(int) atIndex:11];
        [computeEncoder setBytes:&p_Height length:sizeof(int) atIndex:12];
        [computeEncoder setBytes:&p_Gain[0] length:sizeof(float) atIndex:13];
        [computeEncoder setBytes:&p_Gain[1] length:sizeof(float) atIndex:14];
        [computeEncoder setBytes:&p_Gain[2] length:sizeof(float) atIndex:15];
        [computeEncoder setBytes:&p_Gain[3] length:sizeof(float) atIndex:16];
        if (!floatRGBA)
        {
            [computeEncoder setBytes:&p_Components length:sizeof(int) atIndex:17];
        }

        [computeEncoder dispatchThreadgroups:threadGroups threadsPerThreadgroup: threadGroupCount];

        [computeEncoder endEncoding];
        [commandBuffer commit];
    }
    return 0;
}
