// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// metalRenderChecks' plug-in-like Metal work (Objective-C++, ARC): kernels on Metal buffers and
// textures, committed in command buffers with unretained references and not waited for, as a
// plug-in may do; and what the checks read from Metal objects.

#import <Metal/Metal.h>

#include <map>
#include <mutex>
#include <string>

#if !__has_feature(objc_arc)
#error "metalRenderChecksKernel.mm needs ARC (-fobjc-arc)"
#endif

// (|a| <= spin: "+ (a > 1e30f ? 1 : 0)" adds 0, but needs the loop, which safe math keeps)
static const char *kSource =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "kernel void slowScale(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],\n"
  "                      constant uint &n [[buffer(2)]], constant float &f [[buffer(3)]],\n"
  "                      constant uint &spin [[buffer(4)]], uint id [[thread_position_in_grid]])\n"
  "{\n"
  "  if (id >= n) return;\n"
  "  float a = 0.0f;\n"
  "  for (uint i = 0; i < spin; ++i) { a += sin(float(i) + in[id]); }\n"
  "  out[id] = in[id] * f + (a > 1e30f ? 1.0f : 0.0f);\n"
  "}\n"
  "kernel void slowTextureCopy(texture2d<float, access::read> in [[texture(0)]],\n"
  "                            texture2d<float, access::write> out [[texture(1)]],\n"
  "                            constant uint &spin [[buffer(0)]], uint2 id [[thread_position_in_grid]])\n"
  "{\n"
  "  if (id.x >= out.get_width() || id.y >= out.get_height()) return;\n"
  "  const float4 v = in.read(id);\n"
  "  float a = 0.0f;\n"
  "  for (uint i = 0; i < spin; ++i) { a += sin(float(i) + v.x); }\n"
  "  out.write(v + (a > 1e30f ? 1.0f : 0.0f), id);\n"
  "}\n"
  "kernel void textureCoordinates(texture2d<float, access::write> out [[texture(0)]],\n"
  "                               uint2 id [[thread_position_in_grid]])\n"
  "{\n"
  "  if (id.x >= out.get_width() || id.y >= out.get_height()) return;\n"
  "  out.write(float4(float(id.x), float(id.y), 0.0f, 1.0f), id);\n"
  "}\n";

/// the pipeline of function, compiled without fast math (it may fold the spin loops away:
/// fastMathEnabled is on by default), and kept for the life of the program, as a plug-in keeps its
/// own: a command buffer with unretained references doesn't keep its pipeline alive either; nil and
/// *why if it fails
static id<MTLComputePipelineState> pipeline(id<MTLDevice> device, NSString *function, std::string *why)
{
  static std::mutex mutex;
  // (strong; never destroyed: a static map would release the pipelines at exit, with no autorelease
  // pool in place, and a Metal object's dealloc may autorelease: most likely the three
  // AppleParavirtAllocator objects reported with no pool by the GitHub macOS runners' paravirtualized
  // device, one per pipeline, which Apple silicon's driver doesn't autorelease)
  static auto *pipelines = new std::map<std::string, id<MTLComputePipelineState>>;
  std::lock_guard<std::mutex> lock(mutex);
  const std::string key = function.UTF8String;
  const auto it = pipelines->find(key);
  if (it != pipelines->end()) {
    return it->second;
  }
  NSError *err = nil;
  MTLCompileOptions *options = [MTLCompileOptions new];
#if defined(__MAC_15_0) && (__MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0)
  if (@available(macOS 15.0, *)) {
    options.mathMode = MTLMathModeSafe;
  } else
#endif
  {
    // (macOS 14, or an older SDK: mathMode doesn't exist)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
  }
  id<MTLLibrary> library = [device newLibraryWithSource:@(kSource) options:options error:&err];
  id<MTLFunction> f = [library newFunctionWithName:function];
  id<MTLComputePipelineState> p = f ? [device newComputePipelineStateWithFunction:f error:&err] : nil;
  if (!p) {
    *why = err ? err.localizedDescription.UTF8String : "no pipeline";
    return nil;
  }
  (*pipelines)[key] = p;
  return p;
}

/// commits the dispatch of function (its arguments set by encode) over width x height items, in a
/// command buffer with unretained references, without waiting; *commandBuffer: that command
/// buffer (+1); "" or why it failed
static std::string commit(void *queue, NSString *function, unsigned width, unsigned height, void **commandBuffer,
                          void (^encode)(id<MTLComputeCommandEncoder>))
{
  *commandBuffer = nullptr;
  std::string why;
  @autoreleasepool {
    id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)queue;
    id<MTLComputePipelineState> p = pipeline(q.device, function, &why);
    if (!p) {
      return why;
    }
    id<MTLCommandBuffer> cb = [q commandBufferWithUnretainedReferences];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:p];
    encode(enc);
    const NSUInteger w = p.threadExecutionWidth;
    [enc dispatchThreadgroups:MTLSizeMake((width + w - 1) / w, height, 1) threadsPerThreadgroup:MTLSizeMake(w, 1, 1)];
    [enc endEncoding];
    [cb commit];
    *commandBuffer = (__bridge_retained void *)cb;
  }
  return why;
}

/// commits, on queue, out[i] = in[i] * factor for i < count (floats of Metal buffers), after spin
/// iterations per item. *commandBuffer: for commandBufferCompleted() and releaseCommandBuffer()
std::string commitSlowScale(void *queue, void *input, void *output, unsigned count, float factor, unsigned spin,
                            void **commandBuffer)
{
  return commit(queue, @"slowScale", count, 1, commandBuffer, ^(id<MTLComputeCommandEncoder> enc) {
    [enc setBuffer:(__bridge id<MTLBuffer>)input offset:0 atIndex:0];
    [enc setBuffer:(__bridge id<MTLBuffer>)output offset:0 atIndex:1];
    [enc setBytes:&count length:sizeof(count) atIndex:2];
    [enc setBytes:&factor length:sizeof(factor) atIndex:3];
    [enc setBytes:&spin length:sizeof(spin) atIndex:4];
  });
}

/// commits, on queue, output = input (Metal textures of width x height), after spin iterations per
/// texel
std::string commitSlowTextureCopy(void *queue, void *input, void *output, unsigned width, unsigned height,
                                  unsigned spin, void **commandBuffer)
{
  return commit(queue, @"slowTextureCopy", width, height, commandBuffer, ^(id<MTLComputeCommandEncoder> enc) {
    [enc setTexture:(__bridge id<MTLTexture>)input atIndex:0];
    [enc setTexture:(__bridge id<MTLTexture>)output atIndex:1];
    [enc setBytes:&spin length:sizeof(spin) atIndex:0];
  });
}

/// commits, on queue, texel (i, j) of output (a float RGBA Metal texture) = (i, j, 0, 1)
std::string commitTextureCoordinates(void *queue, void *output, unsigned width, unsigned height, void **commandBuffer)
{
  return commit(queue, @"textureCoordinates", width, height, commandBuffer, ^(id<MTLComputeCommandEncoder> enc) {
    [enc setTexture:(__bridge id<MTLTexture>)output atIndex:0];
  });
}

/// whether commandBuffer (of commit*()) has completed
bool commandBufferCompleted(void *commandBuffer)
{
  @autoreleasepool {
    return ((__bridge id<MTLCommandBuffer>)commandBuffer).status == MTLCommandBufferStatusCompleted;
  }
}

void releaseCommandBuffer(void *commandBuffer)
{
  @autoreleasepool { // (its dealloc may autorelease: this thread has no pool)
    (void)(__bridge_transfer id<MTLCommandBuffer>)commandBuffer;
  }
}

/// what a Metal texture says: whether it is one, its MTLPixelFormat, MTLTextureUsage and size
bool textureInfo(void *texture, unsigned long *format, unsigned long *usage, unsigned long *width, unsigned long *height)
{
  @autoreleasepool {
    id object = (__bridge id)texture;
    if (![object conformsToProtocol:@protocol(MTLTexture)]) {
      return false;
    }
    id<MTLTexture> t = object;
    *format = t.pixelFormat;
    *usage = t.usage;
    *width = t.width;
    *height = t.height;
    return true;
  }
}

/// the MTLPixelFormat of name ("RGBA8Unorm"...)
unsigned long pixelFormat(const char *name)
{
  const std::string n(name);
  return n == "RGBA8Unorm" ? MTLPixelFormatRGBA8Unorm : n == "RGBA16Unorm" ? MTLPixelFormatRGBA16Unorm
       : n == "RGBA16Float" ? MTLPixelFormatRGBA16Float : n == "RGBA32Float" ? MTLPixelFormatRGBA32Float
       : n == "R8Unorm" ? MTLPixelFormatR8Unorm : n == "R16Unorm" ? MTLPixelFormatR16Unorm
       : n == "R16Float" ? MTLPixelFormatR16Float : n == "R32Float" ? MTLPixelFormatR32Float : MTLPixelFormatInvalid;
}

unsigned long usageRead() { return MTLTextureUsageShaderRead; }
unsigned long usageWrite() { return MTLTextureUsageShaderWrite; }

/// a weak reference to a Metal object: whether something still keeps it alive
struct WeakObject {
  __weak id object;
};

void *makeWeakBuffer(void *object)
{
  WeakObject *w = new WeakObject;
  @autoreleasepool {
    w->object = (__bridge id)object;
  }
  return w;
}

/// a new object, autoreleased into the innermost autorelease pool of the thread (which then has
/// its only reference): a weak reference to it, for weakBufferAlive() and deleteWeakBuffer()
void *makeAutoreleasedObject()
{
  WeakObject *w = new WeakObject;
  NSObject *object = [[NSObject alloc] init];
  w->object = object;
  CFAutorelease(CFBridgingRetain(object));
  return w;
}

bool weakBufferAlive(void *weak)
{
  // (in a pool: reading a weak reference may autorelease what it loads, which would keep it alive)
  @autoreleasepool {
    id object = static_cast<WeakObject *>(weak)->object;
    return object != nil;
  }
}

void deleteWeakBuffer(void *weak)
{
  @autoreleasepool {
    delete static_cast<WeakObject *>(weak);
  }
}
