// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#import <Metal/Metal.h>

// manual retain/release (newLibraryWithSource etc. return retained objects, released below)
#if __has_feature(objc_arc)
#error "MetalKernel.mm uses manual reference counting: compile it without -fobjc-arc"
#endif

#include <stdio.h>

#include <unordered_map>
#include <mutex>

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

// one compute pipeline per Metal device (a pipeline state belongs to the device that built it,
// and several queues may share a device); kept for the life of the plug-in
static std::mutex s_PipelineDeviceMutex;
typedef std::unordered_map<id<MTLDevice>, id<MTLComputePipelineState>> PipelineDeviceMap;
static PipelineDeviceMap s_PipelineDeviceMap;

// the pipeline of the device, built on first use; nil on failure (not cached: retried next time)
static id<MTLComputePipelineState> GetPipelineState(id<MTLDevice> device)
{
    std::unique_lock<std::mutex> lock(s_PipelineDeviceMutex);

    const auto it = s_PipelineDeviceMap.find(device);
    if (it != s_PipelineDeviceMap.end())
    {
        return it->second;
    }

    NSError* err = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.fastMathEnabled = YES;
    id<MTLLibrary> metalLibrary = [device newLibraryWithSource:@(kernelSource) options:options error:&err];
    [options release];
    if (!metalLibrary)
    {
        fprintf(stderr, "GPUGain: failed to load the Metal library, %s\n", err ? err.localizedDescription.UTF8String : "");
        return nil;
    }
    id<MTLFunction> kernelFunction = [metalLibrary newFunctionWithName:@"GainAdjustKernel"];
    [metalLibrary release];
    if (!kernelFunction)
    {
        fprintf(stderr, "GPUGain: failed to retrieve the Metal kernel\n");
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

    s_PipelineDeviceMap[device] = pipelineState;
    return pipelineState;
}

// returns 0, or non-zero if the kernel couldn't be built or encoded
int RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, const float* p_Gain, const float* p_Input, float* p_Output)
{
    @autoreleasepool
    {
        id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue> >(p_CmdQ);
        if (!queue)
        {
            return 1;
        }
        id<MTLComputePipelineState> pipelineState = GetPipelineState(queue.device);
        if (!pipelineState)
        {
            return 1;
        }

        id<MTLBuffer> srcDeviceBuf = reinterpret_cast<id<MTLBuffer> >(const_cast<float *>(p_Input));
        id<MTLBuffer> dstDeviceBuf = reinterpret_cast<id<MTLBuffer> >(p_Output);

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

        [computeEncoder dispatchThreadgroups:threadGroups threadsPerThreadgroup: threadGroupCount];

        [computeEncoder endEncoding];
        [commandBuffer commit];
    }
    return 0;
}
