// The Metal kernel of the Metal example (metal.cpp): Objective-C++, compiled by the C++ compiler.
// Software License: see metal.cpp (The Open Effects Association Ltd, BSD-3-Clause).

#import <Metal/Metal.h>

// manual retain/release: the new... methods return retained objects, released below
#if __has_feature(objc_arc)
#error "kernel.mm uses manual reference counting: compile it without -fobjc-arc"
#endif

#include <stdio.h>
#include <map>

// the kernel parameters, the same layout as GainParams in kKernelSource (int and float only)
struct GainParams {
    int width, height;     // the render window
    float rGain, gGain, bGain;
    int inOffset, inRow;   // in floats: the first pixel of the window, and the row length
    int outOffset, outRow;
};

// One thread per pixel of the render window: float RGBA pixels, rows of increasing y (see
// deviceLayout in metal.cpp).
static const char *kKernelSource =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct GainParams {\n"
"    int width, height;\n"
"    float rGain, gGain, bGain;\n"
"    int inOffset, inRow;\n"
"    int outOffset, outRow;\n"
"};\n"
"kernel void GainKernel(const device float *input [[buffer(0)]],\n"
"                       device float *output [[buffer(1)]],\n"
"                       constant GainParams &p [[buffer(2)]],\n"
"                       uint2 id [[thread_position_in_grid]])\n"
"{\n"
"    const int x = int(id.x);\n"
"    const int y = int(id.y);\n"
"    if ((x < p.width) && (y < p.height)) {\n"
"        const int i = p.inOffset + y * p.inRow + x * 4;\n"
"        const int o = p.outOffset + y * p.outRow + x * 4;\n"
"        output[o + 0] = input[i + 0] * p.rGain;\n"
"        output[o + 1] = input[i + 1] * p.gGain;\n"
"        output[o + 2] = input[i + 2] * p.bGain;\n"
"        output[o + 3] = input[i + 3];\n"
"    }\n"
"}\n";

// One compute pipeline per Metal device: a pipeline state belongs to the device that built it,
// and the host may use several devices (or queues of a device). Kept until ReleaseMetalKernels,
// with the device (the key), which is retained so that its address can't be reused meanwhile.
// The caller holds a lock (gKernelMutex in metal.cpp).
typedef std::map<id<MTLDevice>, id<MTLComputePipelineState> > PipelineMap;
static PipelineMap gPipelines;

// the pipeline of a device, built on first use; nil on failure (not cached: retried next time)
static id<MTLComputePipelineState> GetPipeline(id<MTLDevice> device)
{
    PipelineMap::const_iterator it = gPipelines.find(device);
    if (it != gPipelines.end())
    {
        return it->second;
    }

    NSError *err = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kKernelSource]
                                                  options:nil
                                                    error:&err];
    if (!library)
    {
        fprintf(stderr, "metal: failed to compile the Metal library, %s\n", err ? err.localizedDescription.UTF8String : "");
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"GainKernel"];
    [library release];
    if (!function)
    {
        fprintf(stderr, "metal: failed to find the Metal kernel\n");
        return nil;
    }
    err = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&err];
    [function release];
    if (!pipeline)
    {
        fprintf(stderr, "metal: failed to make the Metal pipeline, %s\n", err ? err.localizedDescription.UTF8String : "");
        return nil;
    }
    // both retained (pipeline: new...), released by ReleaseMetalKernels
    gPipelines[[device retain]] = pipeline;
    return pipeline;
}

void ReleaseMetalKernels(void)
{
    for (PipelineMap::iterator it = gPipelines.begin(); it != gPipelines.end(); ++it)
    {
        [it->second release];
        [it->first release];
    }
    gPipelines.clear();
}

// Encode the kernel on the host's queue and commit it. Returns 0 (success) or 1 (failed).
int RunMetalKernel(void *p_CmdQ, int p_Width, int p_Height, const float *p_Gain,
                   const void *p_Input, int p_InOffset, int p_InRow,
                   void *p_Output, int p_OutOffset, int p_OutRow)
{
    if ((p_Width <= 0) || (p_Height <= 0))
    {
        return 0; // nothing to do
    }
    // the objects that Metal returns autoreleased (command buffer, encoder) are released here
    @autoreleasepool
    {
        id<MTLCommandQueue> queue = (id<MTLCommandQueue>) p_CmdQ;
        id<MTLBuffer> input = (id<MTLBuffer>) p_Input;
        id<MTLBuffer> output = (id<MTLBuffer>) p_Output;
        if (!queue || !input || !output)
        {
            return 1;
        }
        // the buffers must hold the window (the host owns them: no retain or release)
        const NSUInteger inEnd = ((NSUInteger)p_InOffset + (NSUInteger)(p_Height - 1) * (NSUInteger)p_InRow + (NSUInteger)p_Width * 4) * sizeof(float);
        const NSUInteger outEnd = ((NSUInteger)p_OutOffset + (NSUInteger)(p_Height - 1) * (NSUInteger)p_OutRow + (NSUInteger)p_Width * 4) * sizeof(float);
        if ((input.length < inEnd) || (output.length < outEnd))
        {
            fprintf(stderr, "metal: a Metal buffer is smaller than its image\n");
            return 1;
        }
        id<MTLComputePipelineState> pipeline = GetPipeline(queue.device);
        if (!pipeline)
        {
            return 1;
        }

        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        if (!commandBuffer)
        {
            return 1;
        }
        commandBuffer.label = @"GainKernel";
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (!encoder)
        {
            return 1;
        }

        GainParams params;
        params.width = p_Width;
        params.height = p_Height;
        params.rGain = p_Gain[0];
        params.gGain = p_Gain[1];
        params.bGain = p_Gain[2];
        params.inOffset = p_InOffset;
        params.inRow = p_InRow;
        params.outOffset = p_OutOffset;
        params.outRow = p_OutRow;

        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBytes:&params length:sizeof(params) atIndex:2];

        const NSUInteger w = pipeline.threadExecutionWidth;
        NSUInteger h = pipeline.maxTotalThreadsPerThreadgroup / w;
        if (h < 1)
        {
            h = 1;
        }
        const MTLSize threadsPerGroup = MTLSizeMake(w, h, 1);
        const MTLSize groups = MTLSizeMake((p_Width + w - 1) / w, (p_Height + h - 1) / h, 1);
        [encoder dispatchThreadgroups:groups threadsPerThreadgroup:threadsPerGroup];
        [encoder endEncoding];

        // ofxGPURender.h, kOfxImageEffectPropMetalCommandQueue: "The plug-in SHOULD NOT wait
        // for final asynchronous operations to complete before returning from the render
        // action." (no waitUntilCompleted: the queue keeps the command buffer until it is done)
        [commandBuffer commit];
    }
    return 0;
}
