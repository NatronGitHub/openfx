// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// cudaRenderChecks: checks of openfx::HostSupportCUDA (ofxhCUDADevice.h, ofxhCUDARender.h) and of
/// ImageEffect::GPURenderScope's CUDA arguments, on hostDemo's host classes.
/// - The default loader: whatever this machine has (a CUDA driver, or the reason there is none).
/// - A fake CUDA driver (a function table given by Hooks::getProcAddress, so these checks run on
///   any machine): its device memory is CPU memory, it checks that each call has the context it
///   needs and that each copy stays in its allocation, and its streams run the work enqueued on
///   them only when they are synchronized. On it: the loader (a missing function, cuInit failing),
///   the device list, createDevice() by index, ContextScope, the streams lent, reused, and
///   destroyed when they fail; the copies to device memory and back (all depths and components,
///   packed, padded and negative rowBytes; the render window only; an empty image; cuMemAlloc
///   out of memory); and, with Support/Plugins/GPUGain from OFX_PLUGIN_PATH (any build: its CUDA
///   support is set on the instance, and a "kernel" of these checks stands for the plug-in's on
///   the fake device memory): chooseRenderMode(), the inArgs of Begin/Render/EndSequenceRender
///   (kOfxImageEffectPropCudaEnabled 1, kOfxImageEffectPropCudaStream only when the plug-in and
///   the host both support streams, the primary context current), the host's waits for the
///   stream (before reading back and before freeing the memory of images the plug-in released),
///   the precondition (the plug-in's or the host descriptor's "false"), "needed", and the CPU
///   retry after kOfxStatGPURenderFailed, a failed stream or an allocation failure.
///   Without the plug-in, the render checks are skipped, unless OFXH_REQUIRE=1 (CI).
/// - A real CUDA device, if there is one: the copies, and GPUGain's CUDA render if it was built
///   with CUDA (OFX_SUPPORTS_CUDARENDER), compared with the expected pixels. Without a device or
///   that build, skipped, unless OFXH_REQUIRE_CUDA=1.
/// Exits with 1 if a check fails.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"

#include "ofxhCUDADevice.h"
#include "ofxhCUDARender.h"
#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhParam.h"
#include "ofxhPluginCache.h"
#include "ofxhUtilities.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
#include "hostDemoHostDescriptor.h"
#include "gpuRenderChecks.h"

using namespace OFX::Host;
namespace CU = OFX::Host::CUDA;
using GPUChecks::check;
using GPUChecks::skip;
using GPUChecks::RenderResult;
using GPUChecks::TestImage;

namespace {

  ////////////////////////////////////////////////////////////////////////////////
  // the fake CUDA driver (the CUDA driver API of cuda.h, 64-bit)

  namespace Fake {

#if defined(_WIN32)
#define FAKE_CUDAAPI __stdcall
#else
#define FAKE_CUDAAPI
#endif
    typedef unsigned long long Ptr;  ///< CUdeviceptr
    const int kSuccess = 0, kInvalidValue = 1, kOutOfMemory = 2, kNotInitialized = 3, kNoDevice = 100,
              kInvalidDevice = 101, kInvalidContext = 201, kLaunchOutOfResources = 701;
    const int kHost = 1, kDevice = 2;  ///< CUmemorytype

    /// CUDA_MEMCPY2D (cuda.h)
    struct Copy2D {
      size_t srcXInBytes, srcY;
      int srcMemoryType;
      const void *srcHost;
      Ptr srcDevice;
      void *srcArray;
      size_t srcPitch;
      size_t dstXInBytes, dstY;
      int dstMemoryType;
      void *dstHost;
      Ptr dstDevice;
      void *dstArray;
      size_t dstPitch;
      size_t WidthInBytes, Height;
    };
    static_assert(sizeof(Copy2D) == 128, "CUDA_MEMCPY2D is 128 bytes on 64-bit platforms");

    struct Stream {
      std::vector<std::function<void()>> pending;  ///< the work enqueued, run by cuStreamSynchronize
    };

    struct State {
      std::mutex mutex;
      int deviceCount = 2;
      int initResult = kSuccess;
      bool failAlloc = false;  ///< cuMemAlloc: CUDA_ERROR_OUT_OF_MEMORY
      int failSync = kSuccess;  ///< what cuStreamSynchronize returns (and then the work is dropped)
      bool failAfterWork = false;  ///< failSync only for a stream with work pending (a failed kernel)
      int failFree = kSuccess;     ///< what cuMemFree returns (the memory is freed anyway)
      int nullSyncs = 0;        ///< cuStreamSynchronize of the legacy default stream
      std::map<Ptr, std::vector<char>> memory;  ///< the live allocations, by address
      std::set<Stream *> streams;               ///< the live streams
      int allocs = 0, frees = 0, streamsCreated = 0, streamsDestroyed = 0, syncs = 0, retains = 0, releases = 0;
      int freedWhileBusy = 0;  ///< cuMemFree while a stream had work pending
      int badCalls = 0;        ///< calls without the context they need, copies out of their allocation
      std::string missing;     ///< a function the loader doesn't find
      bool uuidV2 = false;     ///< offer cuDeviceGetUuid_v2 (driver 11.4+)
    };
    State g;
    char gContexts[4];  ///< the primary context of device d: &gContexts[d]
    thread_local std::vector<void *> tCurrent;  ///< the context stack of the thread

    void *current() { return tCurrent.empty() ? nullptr : tCurrent.back(); }
    bool hasContext()
    {
      if (current()) {
        return true;
      }
      ++g.badCalls;
      return false;
    }

    /// the live allocation that has [p, p + n), else nullptr
    char *at(Ptr p, size_t n)
    {
      auto it = g.memory.upper_bound(p);
      if (it == g.memory.begin()) {
        return nullptr;
      }
      --it;
      if (p + n > it->first + it->second.size()) {
        return nullptr;
      }
      return it->second.data() + (p - it->first);
    }

    void reset()
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      g.deviceCount = 2;
      g.initResult = kSuccess;
      g.failAlloc = false;
      g.failSync = kSuccess;
      g.failAfterWork = false;
      g.failFree = kSuccess;
      g.uuidV2 = false;
      g.missing.clear();
    }

    /// the work pending on the streams
    size_t pending()
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      size_t n = 0;
      for (Stream *s : g.streams) {
        n += s->pending.size();
      }
      return n;
    }

    void enqueue(void *stream, std::function<void()> work)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      static_cast<Stream *>(stream)->pending.push_back(std::move(work));
    }

    int FAKE_CUDAAPI init(unsigned) { return g.initResult; }
    int FAKE_CUDAAPI driverGetVersion(int *v) { *v = 12040; return kSuccess; }
    int FAKE_CUDAAPI deviceGetCount(int *n) { *n = g.deviceCount; return g.initResult; }
    int FAKE_CUDAAPI deviceGet(int *d, int ordinal)
    {
      if (ordinal < 0 || ordinal >= g.deviceCount) {
        return kInvalidDevice;
      }
      *d = ordinal;
      return kSuccess;
    }
    int FAKE_CUDAAPI deviceGetName(char *name, int len, int d)
    {
      std::snprintf(name, static_cast<size_t>(len), "Fake CUDA device %d", d);
      return kSuccess;
    }
    int FAKE_CUDAAPI deviceTotalMem(size_t *bytes, int d) { *bytes = (size_t(1) << 30) * static_cast<size_t>(d + 1); return kSuccess; }
    int FAKE_CUDAAPI deviceGetAttribute(int *v, int attribute, int d)
    {
      switch (attribute) {
      case 75: *v = 8; break;      // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
      case 76: *v = 6; break;      // _MINOR
      case 33: *v = d + 1; break;  // CU_DEVICE_ATTRIBUTE_PCI_BUS_ID
      case 34: *v = 0; break;      // _PCI_DEVICE_ID
      case 50: *v = 0; break;      // _PCI_DOMAIN_ID
      default: return kInvalidValue;
      }
      return kSuccess;
    }
    int FAKE_CUDAAPI deviceGetUuid(unsigned char *uuid, int d)
    {
      for (int i = 0; i < 16; ++i) {
        uuid[i] = static_cast<unsigned char>(i == 0 ? 0xA0 + d : i * 0x11);
      }
      return kSuccess;
    }
    /// (the MIG-aware v2: another UUID, to see which one the library takes)
    int FAKE_CUDAAPI deviceGetUuidV2(unsigned char *uuid, int d)
    {
      deviceGetUuid(uuid, d);
      uuid[0] = static_cast<unsigned char>(0xB0 + d);
      return kSuccess;
    }
    int FAKE_CUDAAPI primaryCtxRetain(void **ctx, int d)
    {
      *ctx = &gContexts[d];
      ++g.retains;
      return kSuccess;
    }
    int FAKE_CUDAAPI primaryCtxRelease(int) { ++g.releases; return kSuccess; }
    int FAKE_CUDAAPI ctxPushCurrent(void *ctx)
    {
      if (!ctx) {
        return kInvalidContext;
      }
      tCurrent.push_back(ctx);
      return kSuccess;
    }
    int FAKE_CUDAAPI ctxPopCurrent(void **ctx)
    {
      if (tCurrent.empty()) {
        return kInvalidContext;
      }
      if (ctx) {
        *ctx = tCurrent.back();
      }
      tCurrent.pop_back();
      return kSuccess;
    }
    int FAKE_CUDAAPI streamCreate(void **stream, unsigned flags)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      if (!hasContext() || flags != 1) {  // (CU_STREAM_NON_BLOCKING)
        ++g.badCalls;
        return kInvalidValue;
      }
      Stream *s = new Stream;
      g.streams.insert(s);
      ++g.streamsCreated;
      *stream = s;
      return kSuccess;
    }
    int FAKE_CUDAAPI streamSynchronize(void *stream)
    {
      std::vector<std::function<void()>> work;
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        Stream *s = static_cast<Stream *>(stream);
        if (!hasContext()) {
          return kInvalidContext;
        }
        if (!s) {
          ++g.nullSyncs;  // (the legacy default stream: its copies ran at once)
          return kSuccess;
        }
        if (!g.streams.count(s)) {
          ++g.badCalls;
          return kInvalidValue;
        }
        ++g.syncs;
        const bool busy = !s->pending.empty();
        work.swap(s->pending);
        if (g.failSync != kSuccess && (busy || !g.failAfterWork)) {
          return g.failSync;  // (the work is dropped, as after a failed launch)
        }
      }
      for (const auto &w : work) {
        w();
      }
      return kSuccess;
    }
    int FAKE_CUDAAPI streamDestroy(void *stream)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      Stream *s = static_cast<Stream *>(stream);
      if (!hasContext() || !g.streams.erase(s)) {
        return kInvalidValue;
      }
      delete s;
      ++g.streamsDestroyed;
      return kSuccess;
    }
    int FAKE_CUDAAPI memAlloc(Ptr *p, size_t bytes)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      if (!hasContext()) {
        return kInvalidContext;
      }
      if (bytes == 0) {
        return kInvalidValue;
      }
      if (g.failAlloc) {
        return kOutOfMemory;
      }
      std::vector<char> m(bytes, static_cast<char>(0xEE));
      const Ptr a = static_cast<Ptr>(reinterpret_cast<uintptr_t>(m.data()));
      g.memory[a].swap(m);
      ++g.allocs;
      *p = a;
      return kSuccess;
    }
    int FAKE_CUDAAPI memFree(Ptr p)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      if (!hasContext() || !g.memory.erase(p)) {
        return kInvalidValue;
      }
      for (Stream *s : g.streams) {
        g.freedWhileBusy += s->pending.empty() ? 0 : 1;
      }
      ++g.frees;
      return g.failFree;
    }
    int copy2D(const Copy2D *c);
    /// on stream, after the work pending there (a copy of the descriptor); at once on the legacy
    /// default stream
    int FAKE_CUDAAPI memcpy2DAsync(const Copy2D *c, void *stream)
    {
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!hasContext()) {
          return kInvalidContext;
        }
        Stream *s = static_cast<Stream *>(stream);
        if (s && !g.streams.count(s)) {
          ++g.badCalls;
          return kInvalidValue;
        }
        if (s && !s->pending.empty()) {
          const Copy2D copy = *c;
          s->pending.push_back([copy] { copy2D(&copy); });
          return kSuccess;
        }
      }
      return copy2D(c);
    }
    int copy2D(const Copy2D *c)
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      for (size_t y = 0; y < c->Height; ++y) {
        const char *src = c->srcMemoryType == kHost
          ? static_cast<const char *>(c->srcHost) + (c->srcY + y) * c->srcPitch + c->srcXInBytes
          : c->srcMemoryType == kDevice ? at(c->srcDevice + (c->srcY + y) * c->srcPitch + c->srcXInBytes, c->WidthInBytes)
                                        : nullptr;
        char *dst = c->dstMemoryType == kHost
          ? static_cast<char *>(c->dstHost) + (c->dstY + y) * c->dstPitch + c->dstXInBytes
          : c->dstMemoryType == kDevice ? at(c->dstDevice + (c->dstY + y) * c->dstPitch + c->dstXInBytes, c->WidthInBytes)
                                        : nullptr;
        if (!src || !dst || c->srcMemoryType == c->dstMemoryType) {
          ++g.badCalls;
          return kInvalidValue;
        }
        std::memcpy(dst, src, c->WidthInBytes);
      }
      return kSuccess;
    }

    /// Hooks::getProcAddress: the exported names of the driver (cuda.h maps cuMemAlloc to
    /// cuMemAlloc_v2...); nothing for the legacy ones
    void *getProcAddress(const char *name)
    {
      static const std::map<std::string, void *> functions = {
        { "cuInit", reinterpret_cast<void *>(&init) },
        { "cuDriverGetVersion", reinterpret_cast<void *>(&driverGetVersion) },
        { "cuDeviceGetCount", reinterpret_cast<void *>(&deviceGetCount) },
        { "cuDeviceGet", reinterpret_cast<void *>(&deviceGet) },
        { "cuDeviceGetName", reinterpret_cast<void *>(&deviceGetName) },
        { "cuDeviceTotalMem_v2", reinterpret_cast<void *>(&deviceTotalMem) },
        { "cuDeviceGetAttribute", reinterpret_cast<void *>(&deviceGetAttribute) },
        { "cuDeviceGetUuid", reinterpret_cast<void *>(&deviceGetUuid) },
        { "cuDevicePrimaryCtxRetain", reinterpret_cast<void *>(&primaryCtxRetain) },
        { "cuDevicePrimaryCtxRelease_v2", reinterpret_cast<void *>(&primaryCtxRelease) },
        { "cuCtxPushCurrent_v2", reinterpret_cast<void *>(&ctxPushCurrent) },
        { "cuCtxPopCurrent_v2", reinterpret_cast<void *>(&ctxPopCurrent) },
        { "cuStreamCreate", reinterpret_cast<void *>(&streamCreate) },
        { "cuStreamSynchronize", reinterpret_cast<void *>(&streamSynchronize) },
        { "cuStreamDestroy_v2", reinterpret_cast<void *>(&streamDestroy) },
        { "cuMemAlloc_v2", reinterpret_cast<void *>(&memAlloc) },
        { "cuMemFree_v2", reinterpret_cast<void *>(&memFree) },
        { "cuMemcpy2DAsync_v2", reinterpret_cast<void *>(&memcpy2DAsync) },
      };
      if (g.missing == name) {
        return nullptr;
      }
      if (std::strcmp(name, "cuDeviceGetUuid_v2") == 0) {
        return g.uuidV2 ? reinterpret_cast<void *>(&deviceGetUuidV2) : nullptr;
      }
      auto it = functions.find(name);
      return it == functions.end() ? nullptr : it->second;
    }

    CU::Hooks hooks()
    {
      CU::Hooks h;
      h.getProcAddress = &getProcAddress;
      h.log = [](LogLevel, const std::string &message) { std::printf("  %s\n", message.c_str()); };
      return h;
    }

  } // Fake

  ////////////////////////////////////////////////////////////////////////////////
  // the host

  class CUDATestInstance;

  /// hostDemo's clip; during a CUDA render, getImage() returns the session's images
  class CUDATestClip : public MyHost::MyClipInstance {
  public:
    CUDATestClip(CUDATestInstance *effect, ImageEffect::ClipDescriptor *desc);
    ImageEffect::Image *getImage(OfxTime time, const OfxRectD *optionalBounds) override;
  private:
    CUDATestInstance *_cuEffect;
  };

  /// the CUDA inArgs of an action
  struct GPUArgs {
    std::string action;
    int enabled = -1;
    bool hasStream = false;  ///< kOfxImageEffectPropCudaStream is set
    void *stream = nullptr;
    void *context = nullptr; ///< the fake driver's current context during the action
    bool operator==(const GPUArgs &o) const
    {
      return action == o.action && enabled == o.enabled && hasStream == o.hasStream && stream == o.stream &&
        context == o.context;
    }
  };

  /// the expected inArgs of Begin, Render and EndSequenceRender
  std::vector<GPUArgs> expectedArgs(int enabled, bool hasStream, void *stream, void *context)
  {
    std::vector<GPUArgs> v(3);
    const char *actions[] = { kOfxImageEffectActionBeginSequenceRender, kOfxImageEffectActionRender,
                              kOfxImageEffectActionEndSequenceRender };
    for (int i = 0; i < 3; ++i) {
      v[i].action = actions[i];
      v[i].enabled = enabled;
      v[i].hasStream = hasStream;
      v[i].stream = stream;
      v[i].context = context;
    }
    return v;
  }

  /// what the actions with kOfxImageEffectPropCudaEnabled 1 do
  enum class Actions {
    Plugin,      ///< the plug-in's (a real device)
    FakeKernel,  ///< the checks' "kernel" on the fake device memory, as a CUDA plug-in would
    RecordOnly   ///< nothing, whatever the inArgs (kOfxStatOK): the plug-in isn't called
  };

  /// hostDemo's instance: records the CUDA inArgs of Begin/Render/EndSequenceRender
  class CUDATestInstance : public MyHost::MyEffectInstance {
  public:
    CU::RenderSession *session = nullptr;
    std::vector<GPUArgs> args;
    Actions cudaActions = Actions::Plugin;

    CUDATestInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context) {}

    ImageEffect::ClipInstance *newClipInstance(ImageEffect::Instance *, ImageEffect::ClipDescriptor *descriptor, int) override
    {
      return new CUDATestClip(this, descriptor);
    }

    OfxStatus mainEntry(const char *action, const void *handle, Property::Set *inArgs, Property::Set *outArgs) override
    {
      if (inArgs && (std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0 ||
                     std::strcmp(action, kOfxImageEffectActionRender) == 0 ||
                     std::strcmp(action, kOfxImageEffectActionEndSequenceRender) == 0)) {
        GPUArgs a;
        a.action = action;
        a.enabled = inArgs->getIntProperty(kOfxImageEffectPropCudaEnabled);
        // "This property will only be set if the host and plug-in both support CUDA streams"
        a.hasStream = inArgs->fetchProperty(kOfxImageEffectPropCudaStream) != nullptr;
        a.stream = a.hasStream ? inArgs->getPointerProperty(kOfxImageEffectPropCudaStream) : nullptr;
        a.context = Fake::current();
        args.push_back(a);
        if (cudaActions == Actions::RecordOnly) {
          return kOfxStatOK;
        }
        if (a.enabled == 1 && cudaActions == Actions::FakeKernel) {
          return std::strcmp(action, kOfxImageEffectActionRender) == 0 ? fakeKernel(*inArgs, a.stream) : kOfxStatOK;
        }
      }
      return MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
    }

  private:
    /// GPUGain's render with scale 0.5, as its CUDA kernel: output = source times (0.5, 0.5, 0.5,
    /// 1) over the render window, float RGBA, on the device memory of the images (CPU memory of
    /// the fake driver). With a stream, the work is enqueued on it, and the action returns
    /// without waiting ("the plug-in SHOULD NOT wait for final asynchronous operations to
    /// complete before returning from the render action"); without one, it runs now ("the
    /// plug-in SHOULD ensure that any asynchronous operations it enqueues have completed before
    /// returning"). Either way the images are released before returning. Without a source:
    /// kOfxStatGPURenderFailed, as GPUGain's GPU paths.
    OfxStatus fakeKernel(Property::Set &inArgs, void *stream)
    {
      OfxRectI w;
      inArgs.getIntPropertyN(kOfxImageEffectPropRenderWindow, &w.x1, 4);
      ImageEffect::ClipInstance *sourceClip = getClip(kOfxImageEffectSimpleSourceClipName);
      ImageEffect::Image *out = getClip(kOfxImageEffectOutputClipName)->getImage(0., nullptr);
      ImageEffect::Image *in = sourceClip->getConnected() ? sourceClip->getImage(0., nullptr) : nullptr;
      OfxStatus st = kOfxStatOK;
      if (!out || !in) {
        st = out ? kOfxStatGPURenderFailed : kOfxStatFailed;
      } else if (out->getStringProperty(kOfxImageEffectPropPixelDepth) != kOfxBitDepthFloat ||
                 out->getStringProperty(kOfxImageEffectPropComponents) != kOfxImageComponentRGBA) {
        st = kOfxStatGPURenderFailed;
      } else {
        float *o = static_cast<float *>(out->getPointerProperty(kOfxImagePropData));
        const float *s = static_cast<const float *>(in->getPointerProperty(kOfxImagePropData));
        const OfxRectI ob = out->getBounds(), ib = in->getBounds();
        const int orow = out->getIntProperty(kOfxImagePropRowBytes) / 4, irow = in->getIntProperty(kOfxImagePropRowBytes) / 4;
        auto work = [o, s, ob, ib, orow, irow, w] {
          for (int y = w.y1; y < w.y2; ++y) {
            for (int x = w.x1; x < w.x2; ++x) {
              for (int c = 0; c < 4; ++c) {
                o[(y - ob.y1) * orow + (x - ob.x1) * 4 + c] = (c < 3 ? 0.5f : 1.f) * s[(y - ib.y1) * irow + (x - ib.x1) * 4 + c];
              }
            }
          }
        };
        if (stream) {
          Fake::enqueue(stream, work);
        } else {
          work();
        }
      }
      if (in) {
        in->releaseReference();
      }
      if (out) {
        out->releaseReference();
      }
      return st;
    }
  };

  CUDATestClip::CUDATestClip(CUDATestInstance *effect, ImageEffect::ClipDescriptor *desc)
    : MyHost::MyClipInstance(effect, desc), _cuEffect(effect) {}

  ImageEffect::Image *CUDATestClip::getImage(OfxTime time, const OfxRectD *optionalBounds)
  {
    ImageEffect::Image *image = MyHost::MyClipInstance::getImage(time, optionalBounds);
    if (!_cuEffect->session) {
      return image;
    }
    ImageEffect::Image *copy = _cuEffect->session->getImage(*this, image);
    if (image) {
      image->releaseReference();
    }
    return copy;
  }

  class CUDATestHost : public MyHost::Host {
  public:
    ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc,
                                       const std::string &context) override
    {
      return new CUDATestInstance(plugin, desc, context);
    }
  };

  /// a render of instance (GPUGain with scale 0.5, GPUChecks::renderGPUGain()) with source input:
  /// with CUDA on device if cuda, else on the CPU
  RenderResult render(CUDATestInstance &instance, CU::Device &device, bool cuda, const std::string &input,
                      bool halfWindow = false)
  {
    instance.args.clear();
    return GPUChecks::renderGPUGain(instance, input, halfWindow,
                                    [&](MyHost::MyClipInstance &output, ImageEffect::Image &outputImage, const OfxRectI &window,
                                        const std::function<OfxStatus()> &actions, RenderResult &result) {
      if (!cuda) {
        GPUChecks::runOnCPU(output, outputImage, window, actions, result);
        return;
      }
      CU::Stream stream(device);
      CU::RenderSession session(instance, stream);
      GPUChecks::runInSession(session, instance.session, instance, output, outputImage, window, actions, result,
                              &CU::cudaFailed, &CU::cpuRetryAllowed);
    });
  }

  ////////////////////////////////////////////////////////////////////////////////
  // the checks

  /// the loader and the device list, on the fake driver
  void checkLoader()
  {
    {
      CU::Hooks none;
      none.getProcAddress = [](const char *) -> void * { return nullptr; };
      std::string error;
      const bool noDevice = !CU::createDevice(none, -1, &error);
      check("a loader that finds nothing: no device (" + error + ")", noDevice && error == "cuInit not found");
      check("a loader that finds nothing: no device listed", CU::listDevices(none).empty());
    }
    {
      Fake::g.missing = "cuMemcpy2DAsync_v2";
      std::string error;
      check("a driver without cuMemcpy2DAsync_v2: no device",
            !CU::createDevice(Fake::hooks(), -1, &error) && error == "cuMemcpy2DAsync_v2 not found");
      Fake::reset();
    }
    {
      Fake::g.initResult = Fake::kNoDevice;
      std::string error;
      const std::vector<CU::DeviceInfo> none = CU::listDevices(Fake::hooks(), &error);
      check("cuInit fails: no device listed, and why (" + error + ")",
            none.empty() && error == "cuInit: CUDA_ERROR_NO_DEVICE (100)");
      Fake::g.initResult = Fake::kNotInitialized;
      check("cuInit fails: no device created", !CU::createDevice(Fake::hooks()));
      Fake::reset();
      Fake::g.deviceCount = 0;
      check("no device: \"no CUDA device\"", [] {
        std::string e;
        return CU::listDevices(Fake::hooks(), &e).empty() && e == "no CUDA device";
      }());
      Fake::reset();
    }
    std::string error;
    const std::vector<CU::DeviceInfo> devices = CU::listDevices(Fake::hooks(), &error);
    check("listDevices: the fake driver's two devices", devices.size() == 2 && error.empty());
    if (devices.size() == 2) {
      const CU::DeviceInfo &d = devices[1];
      check("listDevices: device 1's info (" + d.name + ", " + d.pciBusId + ")",
            d.index == 1 && d.name == "Fake CUDA device 1" && d.computeMajor == 8 && d.computeMinor == 6 &&
            d.pciBusId == "0000:02:00" && d.totalMem == (2ull << 30) && d.driverVersion == 12040 &&
            d.uuid == "GPU-a1112233-4455-6677-8899-aabbccddeeff");
      std::unique_ptr<CU::Device> byUUID = CU::createDeviceWithUUID(Fake::hooks(), d.uuid);
      check("createDeviceWithUUID(" + d.uuid + "): device 1", byUUID && byUUID->info().index == 1);
      std::string why;
      check("createDeviceWithUUID() of an unknown UUID: nullptr, and why",
            !CU::createDeviceWithUUID(Fake::hooks(), "GPU-0", &why) && why == "no CUDA device GPU-0");
      Fake::g.uuidV2 = true;
      const std::vector<CU::DeviceInfo> v2 = CU::listDevices(Fake::hooks());
      Fake::reset();
      check("with cuDeviceGetUuid_v2 (MIG): its UUID (" + (v2.size() == 2 ? v2[1].uuid : std::string()) + ")",
            v2.size() == 2 && v2[1].uuid == "GPU-b1112233-4455-6677-8899-aabbccddeeff");
      Fake::g.uuidV2 = true;
      std::unique_ptr<CU::Device> mig = CU::createDeviceWithUUID(Fake::hooks(), "MIG-b1112233-4455-6677-8899-aabbccddeeff");
      Fake::reset();
      check("createDeviceWithUUID() of a \"MIG-\" UUID, as nvidia-smi -L prints it: that device", mig && mig->info().index == 1);
    }
    {
      const int retains = Fake::g.retains, releases = Fake::g.releases;
      std::unique_ptr<CU::Device> one = CU::createDevice(Fake::hooks(), 1);
      check("createDevice(1): device 1, its primary context retained",
            one && one->info().index == 1 && one->nativeDevice() == 1 && one->nativeContext() == &Fake::gContexts[1] &&
            Fake::g.retains == retains + 1);
      one.reset();
      check("~Device: the primary context released", Fake::g.releases == releases + 1);
      std::unique_ptr<CU::Device> first = CU::createDevice(Fake::hooks());
      check("createDevice(): the first device", first && first->info().index == 0);
      std::string why;
      check("createDevice(5): nullptr, and why", !CU::createDevice(Fake::hooks(), 5, &why) && why == "no CUDA device 5");
    }
  }

  /// the context and the streams, on the fake driver
  void checkStreams(CU::Device &device)
  {
    check("no context current outside a ContextScope", Fake::current() == nullptr);
    {
      CU::ContextScope outer(device);
      CU::ContextScope inner(device);
      check("ContextScope: the primary context current, scopes nest",
            outer.ok() && inner.ok() && Fake::current() == device.nativeContext() && Fake::tCurrent.size() == 2);
    }
    check("ContextScope: popped at the end of the scope", Fake::tCurrent.empty());
    {
      // a host's multi-thread-suite thread: no context until the host opens a ContextScope there
      void *before = &before, *during = nullptr, *after = &after;
      std::thread t([&] {
        before = Fake::current();
        {
          CU::ContextScope scope(device);
          during = Fake::current();
        }
        after = Fake::current();
      });
      t.join();
      check("another thread: no context, the device's in a ContextScope there, none after",
            before == nullptr && during == device.nativeContext() && after == nullptr);
    }
    {
      // withContext() pops also when its call throws
      bool thrown = false;
      try {
        device.withContext([]() -> int { throw std::runtime_error("the host's call"); });
      } catch (const std::runtime_error &) {
        thrown = true;
      }
      check("withContext(): popped when the call throws", thrown && Fake::tCurrent.empty());
    }

    const int created = Fake::g.streamsCreated;
    {
      CU::Stream s(device);
      check("a Stream has no stream until open()", s.native() == nullptr && s.synchronize() && device.streamCount() == 0 &&
            Fake::g.streamsCreated == created);
    }
    {
      CU::Stream a(device);
      CU::Stream b(device);
      check("two streams opened at once: two CUDA streams", a.open() && b.open() && a.native() != b.native() &&
            device.streamCount() == 2 && a.open());
    }
    {
      CU::Stream c(device);
      check("a stream after they were given back: an idle one", c.open() && device.streamCount() == 2 &&
            Fake::g.streamsCreated == created + 2 && c.synchronize());
    }
    {
      const int destroyed = Fake::g.streamsDestroyed;
      {
        CU::Stream f(device);
        f.open();
        Fake::g.failSync = Fake::kLaunchOutOfResources;
      }
      Fake::g.failSync = Fake::kSuccess;
      check("a stream whose cuStreamSynchronize failed: destroyed, not given back",
            device.streamCount() == 1 && Fake::g.streamsDestroyed == destroyed + 1);
    }
    check("a non-sticky error leaves the device usable", device.usable());
    check("the driver calls had the context they need", Fake::g.badCalls == 0 && Fake::tCurrent.empty());
  }

  /// setOutput() copies image to device memory, readOutput() copies it back over garbage
  void checkCopies(CU::Device &device, ImageEffect::Instance &instance, ImageEffect::ClipInstance &clip, bool fake)
  {
    const char *depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthHalf, kOfxBitDepthFloat };
    const char *components[] = { kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha };
    const OfxRectI bounds = { -3, 5, 14, 12 };
    const std::string on = fake ? " (fake driver)" : " (CUDA device)";
    for (const char *d : depths) {
      for (const char *c : components) {
        for (int rows : { 0, 1, -1 }) {
          const std::string what = std::string(d) + " " + c + (rows == 0 ? " packed" : rows > 0 ? " padded" : " negative rowBytes") + on;
          CU::Stream stream(device);
          TestImage image(clip, d, c, bounds, rows);
          const std::vector<char> before = image.pixels();
          CU::RenderSession session(instance, stream);
          ImageEffect::Image *out = session.setOutput(clip, image, bounds);
          check(what + ": copied, with packed rowBytes and the CPU image's depth, components and bounds",
                out && !session.failed() && out->getPointerProperty(kOfxImagePropData) != nullptr &&
                out->getIntProperty(kOfxImagePropRowBytes) == image.packed &&
                out->getStringProperty(kOfxImageEffectPropPixelDepth) == d &&
                out->getStringProperty(kOfxImageEffectPropComponents) == c &&
                out->getBounds().x1 == bounds.x1 && out->getBounds().y2 == bounds.y2);
          if (fake && out) {
            // the device memory is the CPU's here: its rows are bottom up and packed
            const char *mem = static_cast<const char *>(out->getPointerProperty(kOfxImagePropData));
            check(what + ": the device rows are bottom up and packed",
                  std::vector<char>(mem, mem + before.size()) == before);
          }
          for (char &b : image.storage) {
            b = 0x55;
          }
          check(what + ": read back", session.readOutput() && image.pixels() == before);
          if (out) {
            ImageEffect::Image *again = session.getImage(clip, nullptr);
            check(what + ": getImage() of the output clip is the output, another reference",
                  again == out && session.liveImages() == 1);
            if (again) {
              again->releaseReference();
            }
          }
        }
      }
    }
    for (int rows : { 0, 1, -1 }) {
      // a render window inside the bounds: only its pixels go to the device and come back; a host
      // pixel outside it, changed after setOutput(), keeps its new value
      const std::string what = std::string(rows == 0 ? "packed" : rows > 0 ? "padded" : "negative rowBytes") + on;
      CU::Stream stream(device);
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, rows);
      const OfxRectI inner = { 0, 7, 9, 11 };
      CU::RenderSession session(instance, stream);
      const std::vector<char> before = image.pixels();
      const bool set = session.setOutput(clip, image, inner) != nullptr;
      for (char &b : image.storage) {
        b = 0x55;
      }
      bool ok = set && session.readOutput();
      const int px = 4 * 4; // float RGBA
      for (int y = bounds.y1; ok && y < bounds.y2; ++y) {
        for (int x = bounds.x1; ok && x < bounds.x2; ++x) {
          const bool in = x >= inner.x1 && x < inner.x2 && y >= inner.y1 && y < inner.y2;
          const char *p = image.row(y - bounds.y1) + (x - bounds.x1) * px;
          const char *b = &before[(static_cast<size_t>(y - bounds.y1) * (bounds.x2 - bounds.x1) + (x - bounds.x1)) * px];
          for (int i = 0; ok && i < px; ++i) {
            ok = in ? p[i] == b[i] : p[i] == 0x55;
          }
        }
      }
      check(what + ": readOutput() copies the render window back only; a pixel outside it keeps the host's value", ok);
    }
    {
      // an empty image: no allocation of size 0
      CU::Stream stream(device);
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, { 0, 0, 0, 4 }, 0);
      CU::RenderSession session(instance, stream);
      check("an empty image: not copied, failed()" + on, !session.getImage(clip, &image) && session.failed());
    }
    if (fake) {
      // cuMemAlloc out of memory: the session failed, the CPU retry is allowed
      CU::Stream stream(device);
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
      CU::RenderSession session(instance, stream);
      Fake::g.failAlloc = true;
      const bool none = !session.setOutput(clip, image, bounds);
      Fake::g.failAlloc = false;
      check("cuMemAlloc out of memory: no output, failed(), status() kOfxStatGPUOutOfMemory, the CPU retry allowed",
            none && session.failed() && session.status() == kOfxStatGPUOutOfMemory && CU::cudaFailed(session, kOfxStatOK) &&
            CU::cpuRetryAllowed(instance, session, kOfxStatOK));
    }
  }

  /// the renders of GPUGain on the fake driver (the checks' "kernel" for its CUDA one)
  void checkFakeRenders(MyHost::Host &host, CUDATestInstance &instance, CU::Device &device)
  {
    Property::Set &props = instance.getProps();
    Property::Set &hostProps = host.getProperties();
    void *ctx = device.nativeContext();
    // GPUGain's CUDA support, whatever its build: its "kernel" is the checks'
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
    props.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "true");

    check("chooseRenderMode(CUDA) = CUDA", CU::chooseRenderMode(instance, true) == CU::RenderMode::CUDA);
    check("chooseRenderMode(no CUDA) = CPU", CU::chooseRenderMode(instance, false) == CU::RenderMode::CPU);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "false");
    check("chooseRenderMode(no CUDA, plug-in CPU \"false\") = None", CU::chooseRenderMode(instance, false) == CU::RenderMode::None);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "true");
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "false");
    check("chooseRenderMode(CUDA, plug-in \"false\") = CPU", CU::chooseRenderMode(instance, true) == CU::RenderMode::CPU);
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
    check("cudaStreamsSupported(): the plug-in and the host say \"true\"", ImageEffect::cudaStreamsSupported(instance));

    checkCopies(device, instance, *instance.getClip(kOfxImageEffectOutputClipName), true);

    instance.cudaActions = Actions::FakeKernel;
    RenderResult r = render(instance, device, false, "ramp");
    check("CPU render: kOfxStatOK, the source's r, g and b times 0.5 (max error " + std::to_string(r.maxError) + ")",
          r.status == kOfxStatOK && r.maxError <= 1e-6f);
    check("CPU render: kOfxImageEffectPropCudaEnabled 0 and no kOfxImageEffectPropCudaStream in the inArgs",
          instance.args == expectedArgs(0, false, nullptr, nullptr));

    // with a stream: the "kernel" runs when the host synchronizes the stream, after the plug-in
    // released its images
    int syncs = Fake::g.syncs, freedWhileBusy = Fake::g.freedWhileBusy;
    r = render(instance, device, true, "ramp");
    check("CUDA render with a stream: kOfxStatOK, read back after the stream's work, the expected pixels (max error " +
          std::to_string(r.maxError) + ")", r.status == kOfxStatOK && r.readBack && !r.failed && r.maxError <= 1e-6f);
    check("CUDA render with a stream: kOfxImageEffectPropCudaEnabled 1, the same stream in the inArgs of the 3 actions, "
          "the primary context current",
          instance.args.size() == 3 && instance.args[0].stream != nullptr &&
          instance.args == expectedArgs(1, true, instance.args[0].stream, ctx));
    check("CUDA render with a stream: the host waited for it (cuStreamSynchronize), and freed no memory before",
          Fake::g.syncs > syncs && Fake::g.freedWhileBusy == freedWhileBusy);
    check("CUDA render: the plug-in released its images (the output is left)", r.liveImages == 1);
    check("CUDA render: the session freed all its device memory, the stream given back",
          Fake::g.memory.empty() && device.streamCount() >= 1 && Fake::tCurrent.empty());
    r = render(instance, device, true, "ramp", /*halfWindow=*/true);
    check("a CUDA render that leaves half the output unwritten (NaN): an infinite error", r.status == kOfxStatOK && r.maxError == INFINITY);

    // no stream unless both support streams: "This property will only be set if the host and
    // plug-in both support CUDA streams"; then the plug-in waits for its work itself
    const int streams = Fake::g.streamsCreated + device.streamCount();
    props.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "false");
    r = render(instance, device, true, "ramp");
    check("the plug-in's kOfxImageEffectPropCudaStreamSupported \"false\": CUDA, no kOfxImageEffectPropCudaStream, the "
          "expected pixels", r.status == kOfxStatOK && r.readBack && r.maxError <= 1e-6f &&
          instance.args == expectedArgs(1, false, nullptr, ctx));
    check("... and no stream taken", Fake::g.streamsCreated + device.streamCount() == streams);
    props.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
    hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "false");
    r = render(instance, device, true, "ramp");
    check("the host's kOfxImageEffectPropCudaStreamSupported \"false\": CUDA, no kOfxImageEffectPropCudaStream",
          r.status == kOfxStatOK && r.maxError <= 1e-6f && instance.args == expectedArgs(1, false, nullptr, ctx) &&
          !ImageEffect::cudaStreamsSupported(instance));
    hostProps.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
    props.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "needed");
    r = render(instance, device, true, "ramp");
    check("the plug-in's kOfxImageEffectPropCudaStreamSupported \"needed\": a stream",
          r.status == kOfxStatOK && r.maxError <= 1e-6f && instance.args.size() == 3 && instance.args[0].hasStream);
    props.setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");

    // the precondition: "If a plug-in and host have both set kOfxImageEffectPropCudaRenderSupported="true""
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "false");
    r = render(instance, device, true, "ramp");
    check("a session for a plug-in without CUDA: not CUDA (0 and no stream in the inArgs), nothing copied",
          instance.args == expectedArgs(0, false, nullptr, ctx) && r.failed);
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
    hostProps.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "false");
    r = render(instance, device, true, "ramp");
    check("a session while the host's kOfxImageEffectPropCudaRenderSupported is \"false\": not CUDA",
          instance.args == expectedArgs(0, false, nullptr, ctx));
    hostProps.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
    // "needed" (upstream main's @propdef of kOfxImageEffectPropCudaRenderSupported): CUDA, and
    // nothing inferred about the CPU
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "needed");
    check("chooseRenderMode(plug-in \"needed\") = CUDA", CU::chooseRenderMode(instance, true) == CU::RenderMode::CUDA);
    check("chooseRenderMode(plug-in \"needed\", no CUDA) = CPU (kOfxImageEffectPropCPURenderSupported decides)",
          CU::chooseRenderMode(instance, false) == CU::RenderMode::CPU);
    r = render(instance, device, true, "ramp");
    check("a CUDA render of a plug-in that says \"needed\": CUDA, the expected pixels",
          r.status == kOfxStatOK && r.readBack && instance.args.size() == 3 && instance.args[0].enabled == 1 &&
          r.maxError <= 1e-6f);
    props.setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");

    // the CPU retry: kOfxStatGPURenderFailed (no source), a failed stream, an allocation failure
    r = render(instance, device, true, "none");
    check("CUDA render without a source: kOfxStatGPURenderFailed, the CPU retry allowed",
          r.status == kOfxStatGPURenderFailed && r.failed && r.cpuRetry);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "false");
    r = render(instance, device, true, "none");
    check("the same, with kOfxImageEffectPropCPURenderSupported \"false\": no CPU retry", r.failed && !r.cpuRetry);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "true");
    {
      // the plug-in's kernel fails: the next cuStreamSynchronize reports it (a non-sticky error, once)
      const int frees = Fake::g.frees, allocs = Fake::g.allocs;
      Fake::g.failSync = Fake::kLaunchOutOfResources;
      Fake::g.failAfterWork = true;
      r = render(instance, device, true, "ramp");
      Fake::reset();
      check("the plug-in's work fails (CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES on the stream): not read back, the CPU retry "
            "allowed", r.status == kOfxStatOK && !r.readBack && r.failed && r.cpuRetry);
      check("... the memory freed anyway, the device still usable",
            Fake::g.frees - frees == Fake::g.allocs - allocs && Fake::g.memory.empty() && device.usable());
    }
    {
      Fake::g.failAlloc = true;
      r = render(instance, device, true, "ramp");
      Fake::g.failAlloc = false;
      check("cuMemAlloc out of memory: failed, the CPU retry allowed", r.failed && r.cpuRetry && !r.readBack);
    }

    // GPURenderScope itself: kOfxImageEffectPropCudaStream only with a stream, nothing for another
    // instance or in a nested scope of API CPU
    instance.cudaActions = Actions::RecordOnly;
    int tag = 0;
    {
      ImageEffect::GPURenderScope scope(instance, ImageEffect::GPURenderScope::API::CUDA, nullptr);
      render(instance, device, false, "ramp");
      check("a GPURenderScope of API CUDA without a stream: kOfxImageEffectPropCudaEnabled 1, no kOfxImageEffectPropCudaStream",
            instance.args == expectedArgs(1, false, nullptr, nullptr));
    }
    {
      ImageEffect::GPURenderScope scope(instance, ImageEffect::GPURenderScope::API::CUDA, &tag);
      render(instance, device, false, "ramp");
      check("a GPURenderScope of API CUDA with a stream: kOfxImageEffectPropCudaStream is it",
            instance.args == expectedArgs(1, true, &tag, nullptr));
      instance.getProps().setStringProperty(kOfxImageEffectPropCudaStreamSupported, "false");
      render(instance, device, false, "ramp");
      check("... but not if the plug-in's kOfxImageEffectPropCudaStreamSupported is \"false\" (HostSupport checks it too)",
            instance.args == expectedArgs(1, false, nullptr, nullptr));
      instance.getProps().setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
      {
        ImageEffect::GPURenderScope inner(instance, ImageEffect::GPURenderScope::API::CPU, &tag);
        render(instance, device, false, "ramp");
        check("a nested GPURenderScope of API CPU: no CUDA in the inArgs", instance.args == expectedArgs(0, false, nullptr, nullptr));
      }
    }
    {
      ImageEffect::GPURenderScope metal(instance, ImageEffect::GPURenderScope::API::Metal, &tag);
      render(instance, device, false, "ramp");
      check("a GPURenderScope of another API: kOfxImageEffectPropCudaEnabled 0, no kOfxImageEffectPropCudaStream",
            instance.args == expectedArgs(0, false, nullptr, nullptr));
    }
    instance.cudaActions = Actions::Plugin;
    check("no scope left", ImageEffect::GPURenderScope::find(instance) == nullptr);

    {
      // a session that ends with the plug-in's work still pending on its stream (no readOutput():
      // e.g. a failed or aborted render): ~RenderSession waits for the stream before freeing
      instance.cudaActions = Actions::FakeKernel;
      const int freedWhileBusy = Fake::g.freedWhileBusy;
      size_t pendingAtEnd = 0;
      instance.args.clear();
      GPUChecks::renderGPUGain(instance, "ramp", false,
                               [&](MyHost::MyClipInstance &output, ImageEffect::Image &outputImage, const OfxRectI &window,
                                   const std::function<OfxStatus()> &actions, RenderResult &result) {
        CU::Stream stream(device);
        CU::RenderSession session(instance, stream);
        session.setOutput(output, outputImage, window);
        instance.session = &session;
        result.status = actions();
        instance.session = nullptr;
        pendingAtEnd = Fake::pending();
      });
      check("a session ending with the plug-in's work pending (no readOutput()): the work was pending, no memory "
            "freed while it was, all of it freed",
            pendingAtEnd > 0 && Fake::g.freedWhileBusy == freedWhileBusy && Fake::g.memory.empty() && Fake::pending() == 0);
    }

    {
      // one Stream for a frame's tree: two sessions on it, nested (a node's input rendered from its
      // clipGetImage): the same stream for both, and the work ordered on it
      instance.cudaActions = Actions::FakeKernel;
      CU::Stream frame(device);
      {
        CU::RenderSession outer(instance, frame);
        CU::RenderSession inner(instance, frame);
        check("two sessions on one Stream: the same stream", outer.ok() && inner.ok() && frame.native() != nullptr);
      }
      const int created = Fake::g.streamsCreated, nulls = Fake::g.nullSyncs;
      instance.args.clear();
      RenderResult a = GPUChecks::renderGPUGain(instance, "ramp", false,
                                                [&](MyHost::MyClipInstance &output, ImageEffect::Image &outputImage,
                                                    const OfxRectI &window, const std::function<OfxStatus()> &actions,
                                                    RenderResult &result) {
        CU::RenderSession session(instance, frame);
        GPUChecks::runInSession(session, instance.session, instance, output, outputImage, window, actions, result,
                                &CU::cudaFailed, &CU::cpuRetryAllowed);
      });
      check("a render on the frame's Stream: the expected pixels, its stream in the inArgs, no other stream taken, "
            "the copies on it (none on the legacy default stream)",
            a.status == kOfxStatOK && a.readBack && a.maxError <= 1e-6f && instance.args.size() == 3 &&
            instance.args[1].stream == frame.native() && Fake::g.streamsCreated == created && Fake::g.nullSyncs == nulls);
    }

    // the sticky errors, after which "any further CUDA work will return the same error" (or "The
    // context cannot be used anymore"), each on a device of its own: the device becomes unusable,
    // its sessions refuse CUDA, and the host's chooseRenderMode(effect, device.usable()) gives the
    // CPU. A non-sticky error leaves it usable.
    instance.cudaActions = Actions::FakeKernel;
    const int sticky[] = { 214, 700, 702, 710, 714, 715, 716, 717, 718, 719, 810, 911 };
    for (int code : sticky) {
      std::unique_ptr<CU::Device> d = CU::createDevice(Fake::hooks(), 0);
      if (!d) {
        check("a device for the sticky error " + std::to_string(code), false);
        continue;
      }
      Fake::g.failSync = code;
      Fake::g.failAfterWork = true;
      r = render(instance, *d, true, "ramp");
      Fake::reset();
      const bool unusable = !d->usable() && CU::chooseRenderMode(instance, d->usable()) == CU::RenderMode::CPU;
      RenderResult again = render(instance, *d, true, "ramp");
      check("sticky error " + std::to_string(code) + ": failed, the CPU retry allowed, the device unusable, the CPU "
            "chosen next, and a session on it refuses CUDA",
            r.failed && r.cpuRetry && !r.readBack && unusable && again.failed &&
            instance.args == expectedArgs(0, false, nullptr, d->nativeContext()));
    }
    {
      // a sticky error seen only when ~RenderSession frees its memory (e.g. a plug-in without
      // streams: no stream to synchronize): the device becomes unusable too
      std::unique_ptr<CU::Device> d = CU::createDevice(Fake::hooks(), 0);
      {
        TestImage image(*instance.getClip(kOfxImageEffectOutputClipName), kOfxBitDepthFloat, kOfxImageComponentRGBA,
                        { 0, 0, 4, 4 }, 0);  // (outlives the session)
        CU::Stream s(*d);
        CU::RenderSession session(instance, s);
        session.setOutput(*instance.getClip(kOfxImageEffectOutputClipName), image, { 0, 0, 4, 4 });
        Fake::g.failFree = 700;
      }
      Fake::reset();
      check("a sticky error from cuMemFree in ~RenderSession: the device unusable, the memory freed",
            !d->usable() && Fake::g.memory.empty());
    }
    {
      std::unique_ptr<CU::Device> d = CU::createDevice(Fake::hooks(), 0);
      CU::Stream s(*d);
      CU::RenderSession session(instance, s);
      check("status() of a session that failed without a CUDA error: kOfxStatGPURenderFailed", [&] {
        TestImage image(*instance.getClip(kOfxImageEffectOutputClipName), kOfxBitDepthFloat, kOfxImageComponentRGBA,
                        { 0, 0, 0, 2 }, 0);
        return session.ok() && !session.getImage(*instance.getClip(kOfxImageEffectOutputClipName), &image) &&
          session.status() == kOfxStatGPURenderFailed;
      }());
    }
    instance.cudaActions = Actions::Plugin;
    check("the driver calls had the context they need, no context left current", Fake::g.badCalls == 0 && Fake::tCurrent.empty());
    check("all the device memory freed", Fake::g.memory.empty() && Fake::g.allocs == Fake::g.frees);
  }

  /// the renders of GPUGain built with CUDA on a real device (never run in CI)
  void checkDeviceRenders(CUDATestInstance &instance, CU::Device &device)
  {
    checkCopies(device, instance, *instance.getClip(kOfxImageEffectOutputClipName), false);
    if (instance.getProps().getStringProperty(kOfxImageEffectPropCudaRenderSupported) != "true") {
      skip("GPUGain was built without CUDA (OFX_SUPPORTS_CUDARENDER): its CUDA render", "OFXH_REQUIRE_CUDA");
      return;
    }
    RenderResult cpu = render(instance, device, false, "ramp");
    check("CPU render on the CUDA device's host: the expected pixels", cpu.status == kOfxStatOK && cpu.maxError <= 1e-6f);
    RenderResult r = render(instance, device, true, "ramp");
    check("GPUGain's CUDA render: kOfxStatOK, read back, the expected pixels (max error " + std::to_string(r.maxError) + ")",
          r.status == kOfxStatOK && r.readBack && !r.failed && r.maxError <= 1e-6f);
    check("GPUGain's CUDA render: kOfxImageEffectPropCudaEnabled 1 and a stream in the inArgs",
          instance.args.size() == 3 && instance.args[1].enabled == 1 && instance.args[1].hasStream && instance.args[1].stream);
    r = render(instance, device, true, "none");
    check("GPUGain's CUDA render without a source: kOfxStatGPURenderFailed, the CPU retry allowed",
          r.status == kOfxStatGPURenderFailed && r.cpuRetry);
  }

  std::unique_ptr<CUDATestInstance> createGPUGain(ImageEffect::ImageEffectPlugin &plugin)
  {
    std::unique_ptr<CUDATestInstance> instance(dynamic_cast<CUDATestInstance *>(plugin.createInstance(kOfxImageEffectContextFilter, NULL)));
    check("createInstance", instance != nullptr);
    if (!instance) {
      return instance;
    }
    const OfxStatus created = instance->createInstanceAction();
    check("createInstanceAction", created == kOfxStatOK || created == kOfxStatReplyDefault);
    Param::DoubleInstance *scale = dynamic_cast<Param::DoubleInstance *>(instance->getParam("scale"));
    check("GPUGain's scale param set to 0.5", scale != nullptr);
    if (scale) {
      scale->set(0.5);
    }
    return instance;
  }

} // namespace

int main()
{
  {
    // the default loader: whatever this machine has
    std::string error;
    const std::vector<CU::DeviceInfo> devices = CU::listDevices(CU::Hooks(), &error);
    for (const CU::DeviceInfo &d : devices) {
      std::printf("  CUDA device %d: %s (compute capability %d.%d, PCI %s, %llu MB, driver %d)\n", d.index, d.name.c_str(),
                  d.computeMajor, d.computeMinor, d.pciBusId.c_str(), d.totalMem >> 20, d.driverVersion);
    }
    check("the default loader: CUDA devices, or the reason there are none (" + error + ")",
          !devices.empty() == error.empty());
  }

  checkLoader();
  std::unique_ptr<CU::Device> fake = CU::createDevice(Fake::hooks(), 0);
  check("a device of the fake driver", fake != nullptr);
  if (!fake) {
    std::printf("cudaRenderChecks: %d failure(s)\n", GPUChecks::gFailures);
    return 1;
  }
  checkStreams(*fake);

  PluginCache::useStdOFXPluginsLocation(false); // only OFX_PLUGIN_PATH
  CUDATestHost host;
  ImageEffect::gImageEffectHost = &host;
  // the host can pass CUDA memory and streams (a plug-in reads it in kOfxActionDescribe)
  host.getProperties().setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
  host.getProperties().setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
  const char *path = std::getenv("OFX_PLUGIN_PATH");
  const char *pluginId = "com.OpenFXSample.GPUGain"; // Support/Plugins/GPUGain
  ImageEffect::PluginCache imageEffectPluginCache(&host);
  ImageEffect::ImageEffectPlugin *plugin = nullptr;
  if (!path || !*path) {
    skip("OFX_PLUGIN_PATH is not set: the render checks");
  } else {
    imageEffectPluginCache.registerInCache(*PluginCache::getPluginCache());
    PluginCache::getPluginCache()->scanPluginFiles();
    plugin = imageEffectPluginCache.getPluginById(pluginId);
    if (!plugin) {
      skip(std::string("plug-in ") + pluginId + " in OFX_PLUGIN_PATH: the render checks");
    }
  }
  if (plugin) {
    std::unique_ptr<CUDATestInstance> instance = createGPUGain(*plugin);
    if (instance) {
      checkFakeRenders(host, *instance, *fake);
      instance->destroyInstanceAction();
    }
  }
  fake.reset();
  check("the fake device's streams destroyed, its context released",
        Fake::g.streams.empty() && Fake::g.retains == Fake::g.releases);

  // a real CUDA device, if any (none in CI)
  std::string error;
  CU::Hooks hooks;
  hooks.log = [](LogLevel, const std::string &message) { std::printf("  %s\n", message.c_str()); };
  std::unique_ptr<CU::Device> device = CU::createDevice(hooks, -1, &error);
  if (!device) {
    skip("no CUDA device (" + error + "): the checks on a CUDA device", "OFXH_REQUIRE_CUDA");
  } else if (plugin) {
    std::unique_ptr<CUDATestInstance> instance = createGPUGain(*plugin);
    if (instance) {
      checkDeviceRenders(*instance, *device);
      instance->destroyInstanceAction();
    }
  } else {
    skip("the checks on a CUDA device need GPUGain", "OFXH_REQUIRE_CUDA");
  }
  std::printf("cudaRenderChecks: %d failure(s)\n", GPUChecks::gFailures);
  return GPUChecks::gFailures ? 1 : 0;
}
