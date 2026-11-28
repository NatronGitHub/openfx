// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// metalRenderChecks: checks of openfx::HostSupportMetal (ofxhMetalDevice.h, ofxhMetalRender.h)
/// and of ImageEffect::GPURenderScope's Metal API, on hostDemo's host classes (openclRenderChecks
/// does the same for OpenCL):
/// - with a Metal device: the device list, createDevice() by index, the queues lent and given back;
/// - with Support/Plugins/GPUGain built with Metal (com.OpenFXSample.GPUGain, from OFX_PLUGIN_PATH):
///   chooseRenderMode(); the copies to Metal buffers and back (all depths and components, packed,
///   padded and negative rowBytes), and the render window only for the output; GPUGain rendered
///   with Metal compared with the expected pixels, with kOfxImageEffectPropMetalEnabled 1 and the
///   queue in the inArgs of BeginSequenceRender, Render and EndSequenceRender, and
///   kOfxImageEffectPropOpenCLEnabled and kOfxImageEffectPropMetalTextureEnabled 0 (and all 0 and NULL
///   on the CPU, and when the plug-in or the host descriptor doesn't support Metal); "needed"; the
///   images the plug-in released; and the CPU retry after kOfxStatGPURenderFailed (GPUGain without
///   a source).
/// GPURenderScope itself (another instance, nested scopes) is checked for every API by
/// suiteHandleChecks (checkGPURenderScope()), and the comparison's catch of unwritten (NaN)
/// output pixels by cudaRenderChecks (in CI on Linux and Windows). All these checks are functional
/// (#379, part 3 of #361).
/// Without a Metal device or the plug-in, those checks are skipped, unless OFXH_REQUIRE=1.
/// Exits with 1 if a check fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"

#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhMetalDevice.h"
#include "ofxhMetalRender.h"
#include "ofxhParam.h"
#include "ofxhPluginCache.h"
#include "ofxhUtilities.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
#include "hostDemoHostDescriptor.h"
#include "gpuRenderChecks.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

using namespace OFX::Host;
namespace MTL = OFX::Host::Metal;
using GPUChecks::check;
using GPUChecks::skip;
using GPUChecks::RenderResult;
using GPUChecks::TestImage;

// metalRenderChecksKernel.mm
std::string commitSlowScale(void *queue, void *input, void *output, unsigned count, float factor, unsigned spin,
                            void **commandBuffer);
std::string commitSlowTextureCopy(void *queue, void *input, void *output, unsigned width, unsigned height,
                                  unsigned spin, void **commandBuffer);
std::string commitTextureCoordinates(void *queue, void *output, unsigned width, unsigned height, void **commandBuffer);
bool textureInfo(void *texture, unsigned long *format, unsigned long *usage, unsigned long *width, unsigned long *height);
unsigned long pixelFormat(const char *name);
unsigned long usageRead();
unsigned long usageWrite();
bool commandBufferCompleted(void *commandBuffer);
void releaseCommandBuffer(void *commandBuffer);
void *makeWeakBuffer(void *buffer);
void *makeAutoreleasedObject();
bool weakBufferAlive(void *weak);
void deleteWeakBuffer(void *weak);

namespace {


  class MetalTestInstance;

  /// hostDemo's clip; during a Metal render, getImage() returns the session's images
  class MetalTestClip : public MyHost::MyClipInstance {
  public:
    MetalTestClip(MetalTestInstance *effect, ImageEffect::ClipDescriptor *desc);
    ImageEffect::Image *getImage(OfxTime time, const OfxRectD *optionalBounds) override;
  private:
    MetalTestInstance *_metalEffect;
  };

  /// the GPU inArgs of an action
  struct GPUArgs {
    std::string action;
    int enabled = -1;        ///< kOfxImageEffectPropMetalEnabled
    void *queue = nullptr;   ///< kOfxImageEffectPropMetalCommandQueue
    int openCL = -1;         ///< kOfxImageEffectPropOpenCLEnabled
    int textures = -1;       ///< kOfxImageEffectPropMetalTextureEnabled
    bool operator==(const GPUArgs &o) const
    {
      return action == o.action && enabled == o.enabled && queue == o.queue && openCL == o.openCL && textures == o.textures;
    }
  };

  /// hostDemo's instance: records the GPU inArgs of Begin/Render/EndSequenceRender
  class MetalTestInstance : public MyHost::MyEffectInstance {
  public:
    MTL::RenderSession *session = nullptr;
    std::vector<GPUArgs> args;

    MetalTestInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context) {}

    ImageEffect::ClipInstance *newClipInstance(ImageEffect::Instance *, ImageEffect::ClipDescriptor *descriptor, int) override
    {
      return new MetalTestClip(this, descriptor);
    }

    OfxStatus mainEntry(const char *action, const void *handle, Property::Set *inArgs, Property::Set *outArgs) override
    {
      if (inArgs && (std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0 ||
                     std::strcmp(action, kOfxImageEffectActionRender) == 0 ||
                     std::strcmp(action, kOfxImageEffectActionEndSequenceRender) == 0)) {
        GPUArgs a;
        a.action = action;
        a.enabled = inArgs->getIntProperty(kOfxImageEffectPropMetalEnabled);
        a.queue = inArgs->getPointerProperty(kOfxImageEffectPropMetalCommandQueue);
        a.openCL = inArgs->getIntProperty(kOfxImageEffectPropOpenCLEnabled);
        a.textures = inArgs->getIntProperty(kOfxImageEffectPropMetalTextureEnabled);
        args.push_back(a);
      }
      return MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
    }
  };

  MetalTestClip::MetalTestClip(MetalTestInstance *effect, ImageEffect::ClipDescriptor *desc)
    : MyHost::MyClipInstance(effect, desc), _metalEffect(effect) {}

  ImageEffect::Image *MetalTestClip::getImage(OfxTime time, const OfxRectD *optionalBounds)
  {
    ImageEffect::Image *image = MyHost::MyClipInstance::getImage(time, optionalBounds);
    if (!_metalEffect->session) {
      return image;
    }
    ImageEffect::Image *copy = _metalEffect->session->getImage(*this, image);
    if (image) {
      image->releaseReference();
    }
    return copy;
  }

  class MetalTestHost : public MyHost::Host {
  public:
    ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc,
                                       const std::string &context) override
    {
      return new MetalTestInstance(plugin, desc, context);
    }
  };

  ////////////////////////////////////////////////////////////////////////////////
  // copies


  /// a queue of device and a Metal session on it (GPUChecks::checkCopies())
  struct MetalSession {
    MTL::Queue queue;
    MTL::RenderSession session;
    MetalSession(MTL::Device &device, ImageEffect::Instance &instance) : queue(device), session(instance, queue) {}
  };

  /// the copies to Metal buffers and back (GPUChecks::checkCopies()): an id<MTLBuffer> of its
  /// own, with packed rowBytes; an image of an unknown depth isn't copied
  /// #379 (part 3 of #361).
  void checkCopies(MTL::Device &device, ImageEffect::Instance &instance, ImageEffect::ClipInstance &clip)
  {
    GPUChecks::CopyChecks<MetalSession> c;
    c.prefix = "Metal buffer ";
    c.make = [&] { return std::make_unique<MetalSession>(device, instance); };
    c.copiedWhat = "an id<MTLBuffer> with rowBytes packed";
    c.copied = [](ImageEffect::Image &out, const TestImage &image) {
      return out.getPointerProperty(kOfxImagePropData) != image.getPointerProperty(kOfxImagePropData) &&
        out.getIntProperty(kOfxImagePropRowBytes) == image.packed;
    };
    GPUChecks::checkCopies(c, clip);
    {
      // an unknown depth: not copied
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, { -3, 5, 14, 12 }, 0);
      image.setStringProperty(kOfxImageEffectPropPixelDepth, "OfxBitDepthUnknown");
      MetalSession f(device, instance);
      check("an image of an unknown depth: not copied, failed()", !f.session.getImage(clip, &image) && f.session.failed());
    }
  }

  /// The session's waits: a plug-in-like slow kernel (out = 2 * in), in a command buffer with
  /// unretained references, committed and not waited for, after which the "plug-in" releases its
  /// input image at once, while the kernel still runs: the session still keeps the input buffer
  /// (a weak reference to it isn't nil), readOutput() reads the kernel's results (it waited for
  /// the writes to the output buffer), and once the session ended the command buffer is complete.
  /// #379.
  void checkWaits(MTL::Device &device, ImageEffect::Instance &instance)
  {
    ImageEffect::ClipInstance &output = *instance.getClip(kOfxImageEffectOutputClipName);
    ImageEffect::ClipInstance &source = *instance.getClip(kOfxImageEffectSimpleSourceClipName);
    const OfxRectI bounds = { 0, 0, 64, 48 };
    TestImage in(source, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
    TestImage out(output, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
    const unsigned count = 64 * 48 * 4;
    float *pin = reinterpret_cast<float *>(in.getPointerProperty(kOfxImagePropData));
    for (unsigned i = 0; i < count; ++i) {
      pin[i] = static_cast<float>(i % 97) * 0.25f;
    }
    bool running = false, alive = false, read = false;
    void *commandBuffer = nullptr;
    void *weak = nullptr;
    std::string why = "no session images";
    {
      MTL::Queue queue(device);
      MTL::RenderSession session(instance, queue);
      ImageEffect::Image *o = session.setOutput(output, out, bounds);
      ImageEffect::Image *i = session.getImage(source, &in);
      if (o && i) {
        weak = makeWeakBuffer(i->getPointerProperty(kOfxImagePropData));
        why = commitSlowScale(queue.native(), i->getPointerProperty(kOfxImagePropData),
                              o->getPointerProperty(kOfxImagePropData), count, 2.f, 200000, &commandBuffer);
        i->releaseReference(); // (as the plug-in does, before its work is done)
        running = commandBuffer && !commandBufferCompleted(commandBuffer);
        alive = weakBufferAlive(weak);
        read = why.empty() && session.readOutput();
      }
    }
    const bool completed = commandBuffer && commandBufferCompleted(commandBuffer);
    // (this thread has no autorelease pool: an autoreleased buffer would stay alive)
    const bool released = weak && !weakBufferAlive(weak);
    const float *pout = reinterpret_cast<const float *>(out.getPointerProperty(kOfxImagePropData));
    bool equal = read;
    for (unsigned k = 0; equal && k < count; ++k) {
      equal = pout[k] == 2.f * pin[k];
    }
    if (commandBuffer) {
      releaseCommandBuffer(commandBuffer);
    }
    if (weak) {
      deleteWeakBuffer(weak);
    }
    const std::string note = why.empty() ? std::string() : " (" + why + ")";
    check("a slow kernel in an unretained command buffer: still running when the plug-in releases its input" + note,
          running);
    check("the released input's MTLBuffer: kept by the session while the kernel runs", alive);
    check("readOutput() reads the kernel's results", equal);
    check("after the session: the kernel's command buffer is complete (the session waited for its reads)", completed);
    check("after the session, on a thread without an autorelease pool: the input MTLBuffer is released", released);
  }

  /// a plug-in-like texture kernel's command buffer, released at the end of the scope
  struct CommandBuffer {
    void *cb = nullptr;
    ~CommandBuffer() { if (cb) { releaseCommandBuffer(cb); } }
    /// waits for it (a test's own wait, as the session's readOutput() would)
    bool completed() const { return cb && commandBufferCompleted(cb); }
  };

  /// Metal textures (upstream main's kOfxImageEffectPropMetalTextureEnabled), with the plug-in's and
  /// the host's kOfxImageEffectPropMetalTextureSupported "true":
  /// - "for RGBA images ::kOfxBitDepthByte is MTLPixelFormatRGBA8Unorm, ::kOfxBitDepthShort is
  ///   MTLPixelFormatRGBA16Unorm, ::kOfxBitDepthHalf is MTLPixelFormatRGBA16Float and
  ///   ::kOfxBitDepthFloat is MTLPixelFormatRGBA32Float; alpha-only images use the single-channel R
  ///   formats", with "::kOfxImagePropRowBytes ... 0", the sources readable and the output writable
  ///   from shaders; a copy kernel gives the source's pixels back, for every rowBytes;
  /// - "a host MUST NOT pass ::kOfxImageComponentRGB images as textures";
  /// - "their origin is the bottom left with y increasing upwards": texel (i, j) is pixel (x1 + i, y1 + j);
  /// - the render window only is read back;
  /// - lifetime: "The host MUST keep them alive until the work the plug-in enqueued on the command
  ///   queue has completed", with a released input texture, a slow kernel and unretained references.
  /// #379.
  void checkTextures(MTL::Device &device, ImageEffect::Instance &instance)
  {
    ImageEffect::ClipInstance &output = *instance.getClip(kOfxImageEffectOutputClipName);
    ImageEffect::ClipInstance &source = *instance.getClip(kOfxImageEffectSimpleSourceClipName);
    const OfxRectI bounds = { -3, 5, 14, 12 };
    const unsigned w = 17, h = 7;
    struct Format { const char *depth, *components, *format; };
    const Format formats[] = {
      { kOfxBitDepthByte, kOfxImageComponentRGBA, "RGBA8Unorm" }, { kOfxBitDepthShort, kOfxImageComponentRGBA, "RGBA16Unorm" },
      { kOfxBitDepthHalf, kOfxImageComponentRGBA, "RGBA16Float" }, { kOfxBitDepthFloat, kOfxImageComponentRGBA, "RGBA32Float" },
      { kOfxBitDepthByte, kOfxImageComponentAlpha, "R8Unorm" }, { kOfxBitDepthShort, kOfxImageComponentAlpha, "R16Unorm" },
      { kOfxBitDepthHalf, kOfxImageComponentAlpha, "R16Float" }, { kOfxBitDepthFloat, kOfxImageComponentAlpha, "R32Float" },
    };
    for (const Format &f : formats) {
      for (int rows : { 0, 1, -1 }) {
        const std::string what = std::string("Metal texture ") + f.depth + " " + f.components +
          (rows == 0 ? " packed" : rows > 0 ? " padded" : " negative rowBytes");
        TestImage in(source, f.depth, f.components, bounds, rows);
        if (std::strcmp(f.depth, kOfxBitDepthHalf) == 0 || std::strcmp(f.depth, kOfxBitDepthFloat) == 0) {
          // (finite values: the test pattern's bytes may be NaNs, which a kernel needn't keep)
          const int n = std::strcmp(f.components, kOfxImageComponentRGBA) == 0 ? 4 : 1;
          for (int y = 0; y < static_cast<int>(h); ++y) {
            for (int i = 0; i < static_cast<int>(w) * n; ++i) {
              if (std::strcmp(f.depth, kOfxBitDepthFloat) == 0) {
                reinterpret_cast<float *>(in.row(y))[i] = static_cast<float>((y * 31 + i * 7) % 101) / 8.f;
              } else {
                reinterpret_cast<unsigned short *>(in.row(y))[i] = static_cast<unsigned short>(0x3C00 + (y * 31 + i * 7) % 900);
              }
            }
          }
        }
        TestImage out(output, f.depth, f.components, bounds, 0);
        bool ok = false;
        unsigned long format = 0, usageIn = 0, usageOut = 0, tw = 0, th = 0;
        std::string why;
        {
          MTL::Queue queue(device);
          MTL::RenderSession session(instance, queue, MTL::RenderMode::Textures);
          ImageEffect::Image *o = session.setOutput(output, out, bounds);
          ImageEffect::Image *i = session.getImage(source, &in);
          if (o && i && textureInfo(i->getPointerProperty(kOfxImagePropData), &format, &usageIn, &tw, &th) &&
              textureInfo(o->getPointerProperty(kOfxImagePropData), &format, &usageOut, &tw, &th)) {
            ok = i->getIntProperty(kOfxImagePropRowBytes) == 0 && o->getIntProperty(kOfxImagePropRowBytes) == 0 &&
              format == pixelFormat(f.format) && tw == w && th == h && (usageIn & usageRead()) && !(usageIn & usageWrite()) &&
              (usageOut & usageWrite());
            CommandBuffer cb;
            why = commitSlowTextureCopy(queue.native(), i->getPointerProperty(kOfxImagePropData),
                                        o->getPointerProperty(kOfxImagePropData), w, h, 0, &cb.cb);
            i->releaseReference();
            ok = ok && why.empty() && session.readOutput();
          }
        }
        check(what + ": " + f.format + ", rowBytes 0, sources ShaderRead, the output ShaderWrite; a copy kernel gives the "
              "source back" + (why.empty() ? std::string() : " (" + why + ")"), ok && out.pixels() == in.pixels());
      }
    }
    {
      // RGB: not a texture
      MTL::Queue queue(device);
      MTL::RenderSession session(instance, queue, MTL::RenderMode::Textures);
      TestImage in(source, kOfxBitDepthFloat, kOfxImageComponentRGB, bounds, 0);
      check("an RGB image: not a Metal texture, failed()", !session.getImage(source, &in) && session.failed());
    }
    {
      // larger than the Macs' 16384 x 16384 textures: refused before Metal sees it
      MTL::Queue queue(device);
      MTL::RenderSession session(instance, queue, MTL::RenderMode::Textures);
      TestImage in(source, kOfxBitDepthByte, kOfxImageComponentAlpha, { 0, 0, 16385, 1 }, 0);
      check("an image wider than 16384: not a Metal texture, failed()", !session.getImage(source, &in) && session.failed());
    }
    {
      // the orientation, and the render window only read back
      TestImage out(output, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
      const OfxRectI inner = { 0, 7, 9, 11 };
      bool read = false;
      {
        MTL::Queue queue(device);
        MTL::RenderSession session(instance, queue, MTL::RenderMode::Textures);
        ImageEffect::Image *o = session.setOutput(output, out, inner);
        for (char &b : out.storage) {
          b = 0x55;
        }
        CommandBuffer cb;
        read = o && commitTextureCoordinates(queue.native(), o->getPointerProperty(kOfxImagePropData), w, h, &cb.cb).empty() &&
          session.readOutput();
      }
      bool coords = read, outside = read;
      for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
          const float *p = reinterpret_cast<const float *>(out.row(y - bounds.y1)) + (x - bounds.x1) * 4;
          if (x >= inner.x1 && x < inner.x2 && y >= inner.y1 && y < inner.y2) {
            coords = coords && p[0] == static_cast<float>(x - bounds.x1) && p[1] == static_cast<float>(y - bounds.y1) &&
              p[2] == 0.f && p[3] == 1.f;
          } else {
            const char *c = reinterpret_cast<const char *>(p);
            for (int k = 0; k < 16; ++k) {
              outside = outside && c[k] == 0x55;
            }
          }
        }
      }
      check("a Metal texture's texel (i, j) is the pixel (x1 + i, y1 + j): the bottom left origin, y up", coords);
      check("a Metal texture: readOutput() copies the render window back only", outside);
    }
    {
      // lifetime: a slow kernel, the input released at once
      TestImage in(source, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
      for (int y = 0; y < static_cast<int>(h); ++y) {
        for (int i = 0; i < static_cast<int>(w) * 4; ++i) {
          reinterpret_cast<float *>(in.row(y))[i] = static_cast<float>(i % 13) + static_cast<float>(y);
        }
      }
      TestImage out(output, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
      bool running = false, alive = false, read = false;
      void *weak = nullptr;
      CommandBuffer cb;
      std::string why = "no session images";
      {
        MTL::Queue queue(device);
        MTL::RenderSession session(instance, queue, MTL::RenderMode::Textures);
        ImageEffect::Image *o = session.setOutput(output, out, bounds);
        ImageEffect::Image *i = session.getImage(source, &in);
        if (o && i) {
          weak = makeWeakBuffer(i->getPointerProperty(kOfxImagePropData));
          why = commitSlowTextureCopy(queue.native(), i->getPointerProperty(kOfxImagePropData),
                                      o->getPointerProperty(kOfxImagePropData), w, h, 2000000, &cb.cb);
          i->releaseReference(); // (as the plug-in does, before its work is done)
          running = cb.cb && !cb.completed();
          alive = weakBufferAlive(weak);
          read = why.empty() && session.readOutput();
        }
      }
      const bool released = weak && !weakBufferAlive(weak);
      if (weak) {
        deleteWeakBuffer(weak);
      }
      const std::string note = why.empty() ? std::string() : " (" + why + ")";
      check("after the session, on a thread without an autorelease pool: the input MTLTexture is released", released);
      check("a slow texture kernel in an unretained command buffer: still running when the plug-in releases its input" + note,
            running);
      check("the released input's MTLTexture: kept by the session while the kernel runs", alive);
      check("readOutput() reads the texture kernel's results", read && out.pixels() == in.pixels());
      check("after the session: the texture kernel's command buffer is complete (the session waited for its reads)",
            cb.completed());
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // renders

  /// a render of instance (GPUGain with scale 0.5, GPUChecks::renderGPUGain()) with source input, in
  /// mode (with Metal in a RenderSession on device, if Buffers or Textures)
  RenderResult render(MetalTestInstance &instance, MTL::Device &device, MTL::RenderMode mode, const std::string &input,
                      bool halfWindow = false)
  {
    instance.args.clear();
    return GPUChecks::renderGPUGain(instance, input, halfWindow,
                                    [&](MyHost::MyClipInstance &output, ImageEffect::Image &outputImage, const OfxRectI &window,
                                        const std::function<OfxStatus()> &actions, RenderResult &result) {
      if (mode != MTL::RenderMode::Buffers && mode != MTL::RenderMode::Textures) {
        GPUChecks::runOnCPU(output, outputImage, window, actions, result);
        return;
      }
      MTL::Queue queue(device);
      MTL::RenderSession session(instance, queue, mode);
      GPUChecks::runInSession(session, instance.session, instance, output, outputImage, window, actions, result,
                              &MTL::metalFailed, &MTL::cpuRetryAllowed);
    });
  }

  std::vector<GPUArgs> expectedArgs(int enabled, void *queue)
  {
    std::vector<GPUArgs> v(3);
    const char *actions[] = { kOfxImageEffectActionBeginSequenceRender, kOfxImageEffectActionRender,
                              kOfxImageEffectActionEndSequenceRender };
    for (int i = 0; i < 3; ++i) {
      v[i].action = actions[i];
      v[i].enabled = enabled;
      v[i].queue = queue;
      v[i].openCL = 0;
      v[i].textures = 0;
    }
    return v;
  }

  /// the inArgs of a Metal texture render: kOfxImageEffectPropMetalTextureEnabled 1, and "a host
  /// MUST NOT set both properties to 1": kOfxImageEffectPropMetalEnabled 0; the queue
  std::vector<GPUArgs> expectedTextureArgs(void *queue)
  {
    std::vector<GPUArgs> v = expectedArgs(0, queue);
    for (GPUArgs &a : v) {
      a.textures = 1;
    }
    return v;
  }

  /// #379.
  void checkRenders(MyHost::Host &host, MTL::Device &device)
  {
    const char *path = std::getenv("OFX_PLUGIN_PATH");
    if (!path || !*path) {
      skip("OFX_PLUGIN_PATH is not set: the copy and render checks");
      return;
    }
    const char *pluginId = "com.OpenFXSample.GPUGain"; // Support/Plugins/GPUGain
    ImageEffect::PluginCache imageEffectPluginCache(&host);
    imageEffectPluginCache.registerInCache(*PluginCache::getPluginCache());
    PluginCache::getPluginCache()->scanPluginFiles();
    ImageEffect::ImageEffectPlugin *plugin = imageEffectPluginCache.getPluginById(pluginId);
    if (!plugin) {
      skip(std::string("plug-in ") + pluginId + " in OFX_PLUGIN_PATH: the copy and render checks");
      return;
    }
    std::unique_ptr<MetalTestInstance> instance(dynamic_cast<MetalTestInstance *>(plugin->createInstance(kOfxImageEffectContextFilter, NULL)));
    check("createInstance", instance != nullptr);
    if (!instance) {
      return;
    }
    const OfxStatus created = instance->createInstanceAction();
    check("createInstanceAction", created == kOfxStatOK || created == kOfxStatReplyDefault);
    Property::Set &props = instance->getProps();
    if (props.getStringProperty(kOfxImageEffectPropMetalRenderSupported) != "true") {
      skip(std::string(pluginId) + " was built without Metal (OFX_EXAMPLE_GPUGAIN_METAL): the render checks");
      instance->destroyInstanceAction();
      return;
    }

    // chooseRenderMode() (GPUGain: Metal "true", the CPU "true"): Metal buffers if the plug-in and
    // the host have them, else the CPU, or None if the plug-in can't render on the CPU either
    const char *const metal = kOfxImageEffectPropMetalRenderSupported, *const textures = kOfxImageEffectPropMetalTextureSupported;
    const char *const cpu = kOfxImageEffectPropCPURenderSupported;
    MetalTestInstance &in = *instance;
    GPUChecks::checkRules({
      { "chooseRenderMode(Metal) = Buffers", {}, [&] { return MTL::chooseRenderMode(in, true) == MTL::RenderMode::Buffers; } },
      { "chooseRenderMode(no Metal) = CPU", {}, [&] { return MTL::chooseRenderMode(in, false) == MTL::RenderMode::CPU; } },
      { "chooseRenderMode(Metal, plug-in \"false\") = CPU", { { &props, metal, "false" } },
        [&] { return MTL::chooseRenderMode(in, true) == MTL::RenderMode::CPU; } },
      { "chooseRenderMode(Metal, plug-in without Metal or CPU) = None", { { &props, metal, "false" }, { &props, cpu, "false" } },
        [&] { return MTL::chooseRenderMode(in, true) == MTL::RenderMode::None; } },
    });

    checkCopies(device, *instance, *instance->getClip(kOfxImageEffectOutputClipName));
    checkWaits(device, *instance);

    // Metal textures: GPUGain has none, so its instance declares them here for the session checks
    // (no texture render is sent to GPUGain: it would read the textures as CPU memory)
    props.setStringProperty(textures, "true");
    GPUChecks::checkRules({
      { "chooseRenderMode(buffers and textures) = Buffers (the host's default)", {},
        [&] { return MTL::chooseRenderMode(in, true, true) == MTL::RenderMode::Buffers; } },
      { "chooseRenderMode(buffers and textures, preferTextures) = Textures", {},
        [&] { return MTL::chooseRenderMode(in, true, true, true) == MTL::RenderMode::Textures; } },
      { "chooseRenderMode(textures only) = Textures", {},
        [&] { return MTL::chooseRenderMode(in, false, true) == MTL::RenderMode::Textures; } },
    });
    {
      // "a host MUST NOT pass ::kOfxImageComponentRGB images as textures": an RGB input or output
      // gets buffers, or the CPU without them; Alpha is a texture
      ImageEffect::ClipInstance *source = instance->getClip(kOfxImageEffectSimpleSourceClipName);
      ImageEffect::ClipInstance *output = instance->getClip(kOfxImageEffectOutputClipName);
      MyHost::InputSpec spec;
      std::string error;
      MyHost::parseInputSpec("ramp", spec, error);
      dynamic_cast<MyHost::MyClipInstance *>(source)->setInput(spec);
      instance->getClipPreferences();
      for (ImageEffect::ClipInstance *clip : { source, output }) {
        const std::string what = clip == source ? "an input" : "the output";
        clip->setComponents(kOfxImageComponentRGB);
        check(what + " of RGB: chooseRenderMode(buffers and textures, preferTextures) = Buffers",
              MTL::chooseRenderMode(*instance, true, true, true) == MTL::RenderMode::Buffers);
        check(what + " of RGB: chooseRenderMode(textures only) = CPU", MTL::chooseRenderMode(*instance, false, true) == MTL::RenderMode::CPU);
        clip->setComponents(kOfxImageComponentAlpha);
        check(what + " of Alpha: chooseRenderMode(textures only) = Textures",
              MTL::chooseRenderMode(*instance, false, true) == MTL::RenderMode::Textures);
        clip->setComponents(kOfxImageComponentRGBA);
      }
      instance->getClipPreferences();
    }
    GPUChecks::checkRules({
      { "chooseRenderMode(plug-in textures \"needed\", not one of its values) = CPU", { { &props, textures, "needed" } },
        [&] { return MTL::chooseRenderMode(in, false, true) == MTL::RenderMode::CPU; } },
    });
    Property::Set &hostProperties = host.getProperties();
    checkTextures(device, *instance);
    {
      // the inArgs of a texture render, through the core's scope (on the CPU: GPUGain has no texture path)
      int queueTag = 0;
      ImageEffect::GPURenderScope scope(*instance, ImageEffect::GPURenderScope::API::MetalTextures, &queueTag);
      render(*instance, device, MTL::RenderMode::CPU, "ramp");
      check("GPURenderScope API MetalTextures: kOfxImageEffectPropMetalTextureEnabled 1, kOfxImageEffectPropMetalEnabled 0, "
            "the queue", instance->args == expectedTextureArgs(&queueTag));
    }
    // "only for a path that both host and plug-in declared as supported": a texture session isn't
    // ok() when the plug-in or the host descriptor says "false" (the actions get all 0 and no queue)
    GPUChecks::checkRules({
      { "a texture session for a plug-in without textures: not Metal (0 and no queue in the inArgs)",
        { { &props, textures, "false" } },
        [&] { render(in, device, MTL::RenderMode::Textures, "ramp"); return in.args == expectedArgs(0, nullptr); } },
      { "a texture session while the host's kOfxImageEffectPropMetalTextureSupported is \"false\": not Metal",
        { { &hostProperties, textures, "false" } },
        [&] { render(in, device, MTL::RenderMode::Textures, "ramp"); return in.args == expectedArgs(0, nullptr); } },
    });
    props.setStringProperty(textures, "false");

    if (Param::DoubleInstance *scale = dynamic_cast<Param::DoubleInstance *>(instance->getParam("scale"))) {
      scale->set(0.5);
    }
    check("GPUGain's scale param set to 0.5", dynamic_cast<Param::DoubleInstance *>(instance->getParam("scale")) != nullptr);

    RenderResult r = render(*instance, device, MTL::RenderMode::CPU, "ramp");
    check("CPU render: kOfxStatOK, the source's r, g and b times 0.5 (max error " + std::to_string(r.maxError) + ")",
          r.status == kOfxStatOK && r.maxError <= 1e-6f);
    check("CPU render: kOfxImageEffectPropMetalEnabled 0 and no queue in the inArgs", instance->args == expectedArgs(0, nullptr));

    r = render(*instance, device, MTL::RenderMode::Buffers, "ramp");
    check("Metal render: kOfxStatOK, read back, the source's r, g and b times 0.5 (max error " + std::to_string(r.maxError) + ")",
          r.status == kOfxStatOK && r.readBack && !r.failed && r.maxError <= 1e-6f);
    check("Metal render: kOfxImageEffectPropMetalEnabled 1 and the queue in the inArgs, OpenCL and Metal textures 0",
          instance->args.size() == 3 && instance->args[0].queue != nullptr &&
          instance->args == expectedArgs(1, instance->args[0].queue));
    check("Metal render: the plug-in released its images (the output is left)", r.liveImages == 1);

    // the session's precondition, kOfxImageEffectPropMetalEnabled only "If a plug-in and host have
    // both set kOfxImageEffectPropMetalRenderSupported="true"": a session that one of them doesn't
    // support isn't ok(), and the actions get 0 and no queue; "needed" (upstream main's @propdef of
    // kOfxImageEffectPropMetalRenderSupported): Metal, and nothing inferred about the CPU; and
    // GPUGain without a source: kOfxStatGPURenderFailed with Metal ("the GPU kernels need a source
    // image: let the host render on the CPU"), then the CPU retry, unless the plug-in can't render
    // on the CPU
    Property::Set &hostProps = host.getProperties();
    GPUChecks::checkRules({
      { "a session for a plug-in without Metal: not Metal (0 and no queue in the inArgs)", { { &props, metal, "false" } },
        [&] { render(in, device, MTL::RenderMode::Buffers, "ramp"); return in.args == expectedArgs(0, nullptr); } },
      { "a session while the host's kOfxImageEffectPropMetalRenderSupported is \"false\": not Metal",
        { { &hostProps, metal, "false" } },
        [&] { render(in, device, MTL::RenderMode::Buffers, "ramp"); return in.args == expectedArgs(0, nullptr); } },
      { "chooseRenderMode(plug-in \"needed\") = Buffers", { { &props, metal, "needed" } },
        [&] { return MTL::chooseRenderMode(in, true) == MTL::RenderMode::Buffers; } },
      { "chooseRenderMode(plug-in \"needed\", no Metal) = CPU (kOfxImageEffectPropCPURenderSupported decides)",
        { { &props, metal, "needed" } },
        [&] { return MTL::chooseRenderMode(in, false) == MTL::RenderMode::CPU; } },
      { "a Metal render of a plug-in that says \"needed\": Metal, the expected pixels", { { &props, metal, "needed" } },
        [&] {
          const RenderResult n = render(in, device, MTL::RenderMode::Buffers, "ramp");
          return n.status == kOfxStatOK && n.readBack && in.args.size() == 3 && in.args[0].enabled == 1 && n.maxError <= 1e-6f;
        } },
      { "Metal render without a source: kOfxStatGPURenderFailed, the CPU retry allowed", {},
        [&] {
          const RenderResult n = render(in, device, MTL::RenderMode::Buffers, "none");
          return n.status == kOfxStatGPURenderFailed && n.failed && n.cpuRetry;
        } },
      { "the same, with kOfxImageEffectPropCPURenderSupported \"false\": no CPU retry", { { &props, cpu, "false" } },
        [&] {
          const RenderResult n = render(in, device, MTL::RenderMode::Buffers, "none");
          return n.failed && !n.cpuRetry;
        } },
    });
    instance->destroyInstanceAction();
  }

} // namespace

int main()
{
  MTL::Hooks hooks;
  hooks.log = [](OFX::Host::LogLevel, const std::string &message) { std::printf("  %s\n", message.c_str()); };
  std::string error;
  const std::vector<MTL::DeviceInfo> devices = MTL::listDevices(hooks, &error);
  for (const MTL::DeviceInfo &d : devices) {
    std::printf("  device %d: %s%s%s, max buffer %llu bytes\n", d.index, d.name.c_str(),
                d.systemDefault ? ", system default" : "", d.unifiedMemory ? ", unified memory" : "", d.maxBufferLength);
  }
  std::unique_ptr<MTL::Device> device = MTL::createDevice(hooks, -1, &error);
  if (!device) {
    skip("no Metal device (" + error + "): the device, copy and render checks");
  } else {
    check("listDevices: the device is listed, as the system default", !devices.empty() &&
          devices[static_cast<size_t>(device->info().index)].registryID == device->info().registryID &&
          (device->info().systemDefault || device->info().index == 0));
    check("createDevice(" + std::to_string(devices.back().index) + "): that device", [&] {
            std::unique_ptr<MTL::Device> last = MTL::createDevice(hooks, devices.back().index);
            return last && last->info().registryID == devices.back().registryID && last->nativeDevice() != nullptr;
          }());
    check("createDeviceWithRegistryID() of the default device: that device", [&] {
            std::unique_ptr<MTL::Device> d = MTL::createDeviceWithRegistryID(hooks, device->info().registryID);
            return d && d->info().index == device->info().index && d->info().registryID == device->info().registryID;
          }());
    check("createDeviceWithRegistryID() of an unknown registry ID: nullptr, with the reason", [&] {
            std::string why;
            return !MTL::createDeviceWithRegistryID(hooks, 0, &why) && !why.empty();
          }());
    {
      // MTL::AutoreleasePool (ofxhMetalDevice.h, for threads without a pool): an object autoreleased
      // in the inner of two nested pools lives until that pool ends, and is released then, before
      // the outer one ends. (The macOS CI step also fails if the checks autorelease with no pool
      // in place: Objective-C's OBJC_DEBUG_MISSING_POOLS message.)
      void *weak = nullptr;
      bool aliveInInner = false, aliveAfterInner = true;
      {
        MTL::AutoreleasePool outer;
        {
          MTL::AutoreleasePool inner;
          weak = makeAutoreleasedObject();
          aliveInInner = weakBufferAlive(weak);
        }
        aliveAfterInner = weakBufferAlive(weak);
      }
      deleteWeakBuffer(weak);
      check("AutoreleasePool: nested pools; an object autoreleased in the inner one lives until it ends, and is released then",
            aliveInInner && !aliveAfterInner);
    }
    check("createDevice() of a device that doesn't exist: nullptr, with the reason", [&] {
            std::string why;
            return !MTL::createDevice(hooks, 99, &why) && !why.empty();
          }());
    {
      MTL::Queue a(*device);
      MTL::Queue b(*device);
      check("two queues at once: two command queues", a.native() && b.native() && a.native() != b.native() &&
            device->queueCount() == 2);
    }
    {
      MTL::Queue c(*device);
      check("a queue after they were given back: an idle one", c.native() && device->queueCount() == 2);
    }
    PluginCache::useStdOFXPluginsLocation(false); // only OFX_PLUGIN_PATH
    MetalTestHost host;
    ImageEffect::gImageEffectHost = &host;
    // the host can pass Metal buffers (a plug-in reads it in kOfxActionDescribe)
    host.getProperties().setStringProperty(kOfxImageEffectPropMetalRenderSupported, "true");
    host.getProperties().setStringProperty(kOfxImageEffectPropMetalTextureSupported, "true");
    checkRenders(host, *device);
  }
  std::printf("metalRenderChecks: %d failure(s)\n", GPUChecks::gFailures);
  return GPUChecks::gFailures ? 1 : 0;
}
