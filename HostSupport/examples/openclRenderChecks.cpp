// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// openclRenderChecks: checks of openfx::HostSupportOpenCL (ofxhOpenCLDevice.h,
/// ofxhOpenCLRender.h) and of ImageEffect::GPURenderScope, on hostDemo's host classes:
/// - without OpenCL: a loader that finds nothing;
/// - with an OpenCL device: the device list, the queues lent and given back, and released when
///   their clFinish fails (a loader with a failing clFinish);
/// - with Support/Plugins/GPUGain built with OpenCL (com.OpenFXSample.GPUGain, from
///   OFX_PLUGIN_PATH): chooseRenderMode(); the copies to OpenCL buffers and images and back (all
///   depths and components, packed, padded and negative rowBytes; OpenCL Images: RGBA only);
///   GPUGain renders with OpenCL Buffers and Images compared with the expected pixels, with
///   kOfxImageEffectPropOpenCLEnabled 1 and the queue in the inArgs of BeginSequenceRender, Render
///   and EndSequenceRender (and 0 and NULL on the CPU, for another instance, in a nested scope of
///   API CPU, and when the plug-in or the host descriptor doesn't support the session's mode);
///   "needed" for Buffers; the images the plug-in released; unwritten (NaN) output pixels caught
///   by the comparison; and the CPU retry after kOfxStatGPURenderFailed (GPUGain without a source).
/// Without an OpenCL device or the plug-in, those checks are skipped, unless OFXH_REQUIRE=1 (CI).
/// Exits with 1 if a check fails.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxGPURender.h"

#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhOpenCLDevice.h"
#include "ofxhOpenCLRender.h"
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
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace OFX::Host;
namespace CL = OFX::Host::OpenCL;
using GPUChecks::check;
using GPUChecks::skip;
using GPUChecks::RenderResult;
using GPUChecks::TestImage;

#if defined(_WIN32)
#define CHECKS_CLAPI __stdcall
#else
#define CHECKS_CLAPI
#endif

namespace {

  /// the OpenCL library's function name, as the library's default loader finds it
  void *openCLSymbol(const char *name);

  const char *modeName(CL::RenderMode m)
  {
    switch (m) {
    case CL::RenderMode::CPU: return "CPU";
    case CL::RenderMode::Buffers: return "Buffers";
    case CL::RenderMode::Images: return "Images";
    case CL::RenderMode::None: return "None";
    }
    return "?";
  }

  class CLTestInstance;

  /// hostDemo's clip; during an OpenCL render, getImage() returns the session's images
  class CLTestClip : public MyHost::MyClipInstance {
  public:
    CLTestClip(CLTestInstance *effect, ImageEffect::ClipDescriptor *desc);
    ImageEffect::Image *getImage(OfxTime time, const OfxRectD *optionalBounds) override;
  private:
    CLTestInstance *_clEffect;
  };

  /// the OpenCL inArgs of an action
  struct GPUArgs {
    std::string action;
    int enabled = -1;
    void *queue = nullptr;
    bool operator==(const GPUArgs &o) const { return action == o.action && enabled == o.enabled && queue == o.queue; }
  };

  /// hostDemo's instance: records the OpenCL inArgs of Begin/Render/EndSequenceRender
  class CLTestInstance : public MyHost::MyEffectInstance {
  public:
    CL::RenderSession *session = nullptr;
    std::vector<GPUArgs> args;

    CLTestInstance(ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc, const std::string &context)
      : MyHost::MyEffectInstance(plugin, desc, context) {}

    ImageEffect::ClipInstance *newClipInstance(ImageEffect::Instance *, ImageEffect::ClipDescriptor *descriptor, int) override
    {
      return new CLTestClip(this, descriptor);
    }

    OfxStatus mainEntry(const char *action, const void *handle, Property::Set *inArgs, Property::Set *outArgs) override
    {
      if (inArgs && (std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0 ||
                     std::strcmp(action, kOfxImageEffectActionRender) == 0 ||
                     std::strcmp(action, kOfxImageEffectActionEndSequenceRender) == 0)) {
        GPUArgs a;
        a.action = action;
        a.enabled = inArgs->getIntProperty(kOfxImageEffectPropOpenCLEnabled);
        a.queue = inArgs->getPointerProperty(kOfxImageEffectPropOpenCLCommandQueue);
        args.push_back(a);
      }
      return MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
    }
  };

  CLTestClip::CLTestClip(CLTestInstance *effect, ImageEffect::ClipDescriptor *desc)
    : MyHost::MyClipInstance(effect, desc), _clEffect(effect) {}

  ImageEffect::Image *CLTestClip::getImage(OfxTime time, const OfxRectD *optionalBounds)
  {
    ImageEffect::Image *image = MyHost::MyClipInstance::getImage(time, optionalBounds);
    if (!_clEffect->session) {
      return image;
    }
    ImageEffect::Image *copy = _clEffect->session->getImage(*this, image);
    if (image) {
      image->releaseReference();
    }
    return copy;
  }

  class CLTestHost : public MyHost::Host {
  public:
    ImageEffect::Instance *newInstance(void *, ImageEffect::ImageEffectPlugin *plugin, ImageEffect::Descriptor &desc,
                                       const std::string &context) override
    {
      return new CLTestInstance(plugin, desc, context);
    }
  };

  ////////////////////////////////////////////////////////////////////////////////
  // copies


  /// setOutput() copies image to OpenCL, readOutput() copies it back over garbage
  void checkCopies(CL::Device &device, ImageEffect::Instance &instance, ImageEffect::ClipInstance &clip)
  {
    const char *depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthHalf, kOfxBitDepthFloat };
    const char *components[] = { kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha };
    const OfxRectI bounds = { -3, 5, 14, 12 };
    for (CL::RenderMode mode : { CL::RenderMode::Buffers, CL::RenderMode::Images }) {
      if (mode == CL::RenderMode::Images && !device.info().imageSupport) {
        skip("OpenCL Images: the device has no image support (CL_DEVICE_IMAGE_SUPPORT)");
        continue;
      }
      for (const char *d : depths) {
        for (const char *c : components) {
          for (int rows : { 0, 1, -1 }) {
            const std::string what = std::string(modeName(mode)) + " " + d + " " + c +
              (rows == 0 ? " packed" : rows > 0 ? " padded" : " negative rowBytes");
            CL::Queue queue(device);
            TestImage image(clip, d, c, bounds, rows);
            const std::vector<char> before = image.pixels();
            CL::RenderSession session(instance, queue, mode);
            ImageEffect::Image *out = session.setOutput(clip, image, bounds);
            if (mode == CL::RenderMode::Images && std::strcmp(c, kOfxImageComponentRGBA) != 0) {
              check(what + ": not an OpenCL Image, failed()", !out && session.failed());
              continue;
            }
            const bool images = mode == CL::RenderMode::Images;
            check(what + ": copied, with kOfxImageEffectPropOpenCLImage " + (images ? "set" : "NULL") + " and rowBytes " +
                  (images ? "0" : "packed"),
                  out && !session.failed() && out->getPointerProperty(kOfxImagePropData) != nullptr &&
                  (out->getPointerProperty(kOfxImageEffectPropOpenCLImage) != nullptr) == images &&
                  out->getIntProperty(kOfxImagePropRowBytes) == (images ? 0 : image.packed) &&
                  out->getStringProperty(kOfxImageEffectPropPixelDepth) == d &&
                  out->getStringProperty(kOfxImageEffectPropComponents) == c &&
                  out->getBounds().x1 == bounds.x1 && out->getBounds().y2 == bounds.y2);
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
    }
    for (CL::RenderMode mode : { CL::RenderMode::Buffers, CL::RenderMode::Images }) {
      if (mode == CL::RenderMode::Images && !device.info().imageSupport) {
        continue;
      }
      for (int rows : { 0, 1, -1 }) {
        // a render window inside the bounds: only its pixels go to OpenCL and come back; a host pixel
        // outside it, changed after setOutput(), keeps its new value
        const std::string what = std::string(modeName(mode)) + (rows == 0 ? " packed" : rows > 0 ? " padded" : " negative rowBytes");
        CL::Queue queue(device);
        TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, rows);
        const OfxRectI inner = { 0, 7, 9, 11 };
        CL::RenderSession session(instance, queue, mode);
        const std::vector<char> before = image.pixels();
        const bool set = session.setOutput(clip, image, inner) != nullptr;
        for (char &b : image.storage) {
          b = 0x55;
        }
        const bool read = set && session.readOutput();
        bool ok = read;
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
    }
    {
      // an empty image: no OpenCL buffer of size 0
      CL::Queue queue(device);
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, { 0, 0, 0, 4 }, 0);
      CL::RenderSession session(instance, queue, CL::RenderMode::Buffers);
      check("an empty image: not copied, failed()", !session.getImage(clip, &image) && session.failed());
    }
    {
      CL::Queue queue(device);
      CL::RenderSession session(instance, queue, CL::RenderMode::CPU);
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, 0);
      check("a session in RenderMode::CPU: not ok(), copies nothing", !session.ok() && !session.getImage(clip, &image));
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // renders

  /// a render of instance (GPUGain with scale 0.5, GPUChecks::renderGPUGain()) with source input, in
  /// mode (with OpenCL in a RenderSession on device, if Buffers or Images)
  RenderResult render(CLTestInstance &instance, CL::Device &device, CL::RenderMode mode, const std::string &input,
                      bool halfWindow = false)
  {
    instance.args.clear();
    return GPUChecks::renderGPUGain(instance, input, halfWindow,
                                    [&](MyHost::MyClipInstance &output, ImageEffect::Image &outputImage, const OfxRectI &window,
                                        const std::function<OfxStatus()> &actions, RenderResult &result) {
      if (mode != CL::RenderMode::Buffers && mode != CL::RenderMode::Images) {
        GPUChecks::runOnCPU(output, outputImage, window, actions, result);
        return;
      }
      CL::Queue queue(device);
      CL::RenderSession session(instance, queue, mode);
      GPUChecks::runInSession(session, instance.session, instance, output, outputImage, window, actions, result,
                              &CL::openCLFailed, &CL::cpuRetryAllowed);
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
    }
    return v;
  }

  ////////////////////////////////////////////////////////////////////////////////
  // the OpenCL program suite

  typedef void *(CHECKS_CLAPI *CreateKernelFn)(void *, const char *, int32_t *);
  typedef int32_t (CHECKS_CLAPI *SetKernelArgFn)(void *, uint32_t, size_t, const void *);
  typedef int32_t (CHECKS_CLAPI *EnqueueNDRangeKernelFn)(void *, void *, uint32_t, const size_t *, const size_t *,
                                                         const size_t *, uint32_t, const void *, void *);
  typedef void *(CHECKS_CLAPI *CreateBufferFn)(void *, uint64_t, size_t, void *, int32_t *);
  typedef int32_t (CHECKS_CLAPI *EnqueueReadBufferFn)(void *, void *, uint32_t, size_t, size_t, void *, uint32_t,
                                                      const void *, void *);
  typedef int32_t (CHECKS_CLAPI *ReleaseFn)(void *);

  /// the test kernel: out[i] = v * OFXH_GAIN, a macro of the host's build options
  const char kFillSource[] =
    "__kernel void fill(__global float *out, float v) { out[get_global_id(0)] = v * OFXH_GAIN; }\n";

  /// runs kernel "fill" of program on queue (of device) over 4 items with v, and releases program:
  /// out[3], NaN if something failed
  float runFill(CL::Device &device, void *queue, void *program, float v)
  {
    const CreateKernelFn createKernel = reinterpret_cast<CreateKernelFn>(openCLSymbol("clCreateKernel"));
    const SetKernelArgFn setArg = reinterpret_cast<SetKernelArgFn>(openCLSymbol("clSetKernelArg"));
    const EnqueueNDRangeKernelFn enqueue = reinterpret_cast<EnqueueNDRangeKernelFn>(openCLSymbol("clEnqueueNDRangeKernel"));
    const CreateBufferFn createBuffer = reinterpret_cast<CreateBufferFn>(openCLSymbol("clCreateBuffer"));
    const EnqueueReadBufferFn readBuffer = reinterpret_cast<EnqueueReadBufferFn>(openCLSymbol("clEnqueueReadBuffer"));
    const ReleaseFn releaseKernel = reinterpret_cast<ReleaseFn>(openCLSymbol("clReleaseKernel"));
    const ReleaseFn releaseMem = reinterpret_cast<ReleaseFn>(openCLSymbol("clReleaseMemObject"));
    const ReleaseFn releaseProgram = reinterpret_cast<ReleaseFn>(openCLSymbol("clReleaseProgram"));
    if (!createKernel || !setArg || !enqueue || !createBuffer || !readBuffer || !releaseKernel || !releaseMem ||
        !releaseProgram || !program || !queue) {
      return NAN;
    }
    float out[4] = { NAN, NAN, NAN, NAN };
    int32_t err = 0;
    void *kernel = createKernel(program, "fill", &err);
    void *buffer = createBuffer(device.nativeContext(), 1 /* CL_MEM_READ_WRITE */, sizeof(out), nullptr, &err);
    if (kernel && buffer && setArg(kernel, 0, sizeof(buffer), &buffer) == 0 && setArg(kernel, 1, sizeof(v), &v) == 0) {
      const size_t global = 4;
      if (enqueue(queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr, nullptr) == 0) {
        readBuffer(queue, buffer, 1 /* blocking */, 0, sizeof(out), out, 0, nullptr, nullptr);
      }
    }
    if (buffer) {
      releaseMem(buffer);
    }
    if (kernel) {
      releaseKernel(kernel);
    }
    releaseProgram(program);
    return out[3];
  }

  /// OfxOpenCLProgramSuiteV1 through hostDemo's host (setOpenCLProgramCompiler()) and
  /// Device::compileProgram(): the context of the render's queue, the host's build options, the
  /// binary cache (in memory, in a directory, keyed on the source, options and environment), and
  /// fOptional
  void checkPrograms(CLTestHost &host, ImageEffect::Instance &instance, const CL::Hooks &baseHooks)
  {
    typedef ImageEffect::GPURenderScope Scope;
    check("no compiler: no OpenCL program suite", host.fetchSuite(kOfxOpenCLProgramSuite, 1) == nullptr);
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
      ("openclRenderChecks-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    // private to the user, whatever the umask (Hooks::programCacheDirectory)
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    check("a cache directory " + dir.string(), !ec);
    CL::Hooks hooks = baseHooks;
    // "The host can add flags (such as -cl-denorms-are-zero) to the build call"
    hooks.buildOptions = "-cl-denorms-are-zero -DOFXH_GAIN=3";
    hooks.programCacheDirectory = dir.string();
    std::unique_ptr<CL::Device> device = CL::createDevice(hooks);
    check("a device with build options and a cache directory", device != nullptr);
    if (!device) {
      return;
    }
    CL::Device *dev = device.get();
    host.setOpenCLProgramCompiler([&dev](const char *source, int fOptional, void *result) {
      return dev->compileProgram(CL::currentQueue(), source, fOptional, result);
    });
    const OfxOpenCLProgramSuiteV1 *suite = static_cast<const OfxOpenCLProgramSuiteV1 *>(host.fetchSuite(kOfxOpenCLProgramSuite, 1));
    check("the OpenCL program suite", suite && suite->compileProgram);
    if (!suite || !suite->compileProgram) {
      return;
    }
    int notNull = 0;
    void *program = &notNull;
    check("compileProgram outside an OpenCL render, fOptional 1: kOfxStatReplyDefault (\"host may skip compiling on "
          "this call\"), no program", suite->compileProgram(kFillSource, 1, &program) == kOfxStatReplyDefault && !program);
    program = &notNull;
    check("compileProgram outside an OpenCL render, fOptional 0: kOfxStatFailed (no context to build for), no program",
          suite->compileProgram(kFillSource, 0, &program) == kOfxStatFailed && !program);
    check("compileProgram(NULL source): kOfxStatErrBadHandle", suite->compileProgram(nullptr, 0, &program) == kOfxStatErrBadHandle);
    {
      CL::Queue queue(*dev);
      Scope scope(instance, Scope::API::OpenCL, queue.native());
      OfxStatus st = suite->compileProgram(kFillSource, 0, &program);
      check("compileProgram in an OpenCL render: kOfxStatOK, built from the source with the host's options (3 v)",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f && dev->programCacheStats().builds == 1);
      st = suite->compileProgram(kFillSource, 0, &program);
      check("the same source again: the binary kept in memory",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f &&
            dev->programCacheStats().builds == 1 && dev->programCacheStats().memoryHits == 1);
      st = suite->compileProgram(kFillSource, 1, &program);
      check("fOptional 1 in a render: compiled", st == kOfxStatOK && runFill(*dev, queue.native(), program, 1.f) == 3.f);
      const std::string changed = std::string(kFillSource) + "// changed\n";
      st = suite->compileProgram(changed.c_str(), 0, &program);
      check("a changed source: built again (\"if the source of the program [...] changes, the host must recompile\")",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f && dev->programCacheStats().builds == 2);
      program = &notNull;
      st = suite->compileProgram("__kernel void broken(", 0, &program);
      check("a source that doesn't build: kOfxStatFailed, no program", st == kOfxStatFailed && !program);

      // the plug-in's threads (multi-thread suite): the scope is per thread, the host passes it on
      OfxStatus other = kOfxStatOK, passed = kOfxStatFailed;
      void *otherProgram = &notNull, *passedProgram = nullptr;
      std::thread([&] { other = suite->compileProgram(kFillSource, 1, &otherProgram); }).join();
      check("another thread without the render's scope, fOptional 1: kOfxStatReplyDefault", other == kOfxStatReplyDefault && !otherProgram);
      const Scope *s = Scope::current();
      check("GPURenderScope::current(): the render's scope", s == &scope && &s->effect() == &instance);
      std::thread([&] {
        Scope passedOn(s->effect(), s->api(), s->queue());
        passed = suite->compileProgram(kFillSource, 0, &passedProgram);
      }).join();
      check("another thread with the render's scope passed on: kOfxStatOK",
            passed == kOfxStatOK && runFill(*dev, queue.native(), passedProgram, 2.f) == 6.f);

      // the program is built for the context of the render's queue: a queue of another context
      std::unique_ptr<CL::Device> otherDevice = CL::createDevice(baseHooks);
      if (otherDevice) {
        CL::Queue otherQueue(*otherDevice);
        Scope inner(instance, Scope::API::OpenCL, otherQueue.native());
        program = &notNull;
        check("a render on a queue of another context: kOfxStatFailed (not this device's context)",
              suite->compileProgram(kFillSource, 0, &program) == kOfxStatFailed && !program);
      }
    }
    // the binaries in the directory: another device (another context) with the same environment
    {
      std::unique_ptr<CL::Device> again = CL::createDevice(hooks);
      dev = again.get();
      CL::Queue queue(*dev);
      Scope scope(instance, Scope::API::OpenCL, queue.native());
      const OfxStatus st = suite->compileProgram(kFillSource, 0, &program);
      check("a new device, the same directory: built from the cached binary",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f &&
            dev->programCacheStats().fileHits == 1 && dev->programCacheStats().builds == 0);
    }
    // the options are in the key
    {
      CL::Hooks five = hooks;
      five.buildOptions = "-DOFXH_GAIN=5";
      std::unique_ptr<CL::Device> optioned = CL::createDevice(five);
      dev = optioned.get();
      CL::Queue queue(*dev);
      Scope scope(instance, Scope::API::OpenCL, queue.native());
      const OfxStatus st = suite->compileProgram(kFillSource, 0, &program);
      check("other build options: built again with them (5 v)",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 10.f && dev->programCacheStats().builds == 1 &&
            dev->programCacheStats().fileHits == 0);
    }
#if !defined(_WIN32)
    // the files are the user's only (mode 0600)
    {
      bool private600 = false;
      for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto perms = std::filesystem::status(entry.path(), ec).permissions();
        private600 = perms == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        if (!private600) {
          break;
        }
      }
      check("the cache files have mode 0600", private600);
    }
#endif
    // damaged cache files: ignored, built again. Never given to the driver: a binary cut inside, or
    // with a changed byte (a torn write), may crash a driver rather than give CL_INVALID_BINARY
    // each cache file as written, restored before each damage
    std::map<std::filesystem::path, std::vector<char>> originals;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.path().extension() == ".clbin") {
        std::vector<char> &bytes = originals[entry.path()];
        if (FILE *fp = std::fopen(entry.path().string().c_str(), "rb")) {
          int ch = 0;
          while ((ch = std::fgetc(fp)) != EOF) {
            bytes.push_back(static_cast<char>(ch));
          }
          std::fclose(fp);
        }
      }
    }
    check("the cache files (3 sources and options)", originals.size() == 3);
    struct Damage { const char *what; std::function<void(const std::filesystem::path &, size_t)> apply; };
    const Damage damages[] = {
      { "a cache file cut inside its header", [](const std::filesystem::path &f, size_t) {
          std::error_code e;
          std::filesystem::resize_file(f, 10, e);
        } },
      { "a cache file cut inside its binary", [](const std::filesystem::path &f, size_t size) {
          std::error_code e;
          std::filesystem::resize_file(f, size - 16, e);
        } },
      { "a cache file with a changed byte in its binary", [](const std::filesystem::path &f, size_t size) {
          if (FILE *fp = std::fopen(f.string().c_str(), "r+b")) {
            std::fseek(fp, static_cast<long>(size - 1), SEEK_SET);
            const int ch = std::fgetc(fp);
            std::fseek(fp, static_cast<long>(size - 1), SEEK_SET);
            std::fputc(ch ^ 0x5A, fp);
            std::fclose(fp);
          }
        } },
    };
    for (const Damage &d : damages) {
      for (const auto &o : originals) {
        if (FILE *fp = std::fopen(o.first.string().c_str(), "wb")) {
          std::fwrite(o.second.data(), 1, o.second.size(), fp);
          std::fclose(fp);
        }
        d.apply(o.first, o.second.size());
      }
      // the damage is caught before the driver: no binary the driver refused (a driver may also
      // crash on one, or accept it)
      CL::Hooks damageHooks = hooks;
      bool refused = false;
      damageHooks.log = [&refused, log = hooks.log](OFX::Host::LogLevel level, const std::string &message) {
        refused = refused || message.find("a cached binary was refused") != std::string::npos;
        if (log) {
          log(level, message);
        }
      };
      std::unique_ptr<CL::Device> fresh = CL::createDevice(damageHooks);
      dev = fresh.get();
      CL::Queue queue(*dev);
      Scope scope(instance, Scope::API::OpenCL, queue.native());
      const OfxStatus st = suite->compileProgram(kFillSource, 0, &program);
      check(std::string(d.what) + ": ignored before the driver sees it, built from the source",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f && !refused &&
            dev->programCacheStats().builds == 1 && dev->programCacheStats().fileHits == 0);
    }
#if !defined(_WIN32)
    // a directory other users can write to: not used ("a binary can carry native code")
    {
      const std::filesystem::path shared = dir / "shared";
      std::filesystem::create_directories(shared, ec);
      std::filesystem::permissions(shared, std::filesystem::perms::owner_all | std::filesystem::perms::group_all,
                                   std::filesystem::perm_options::replace, ec);
      CL::Hooks sharedHooks = hooks;
      sharedHooks.programCacheDirectory = shared.string();
      bool warned = false;
      const auto log = hooks.log;
      sharedHooks.log = [&warned, log](OFX::Host::LogLevel level, const std::string &message) {
        warned = warned || (level == OFX::Host::LogLevel::Warning && message.find("writable by other users") != std::string::npos);
        if (log) {
          log(level, message);
        }
      };
      std::unique_ptr<CL::Device> unsafe = CL::createDevice(sharedHooks);
      dev = unsafe.get();
      CL::Queue queue(*dev);
      Scope scope(instance, Scope::API::OpenCL, queue.native());
      const OfxStatus first = suite->compileProgram(kFillSource, 0, &program);
      const bool ran = first == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f;
      check("a group-writable cache directory: a Warning, the memory cache only (nothing written there)",
            ran && warned && std::filesystem::is_empty(shared, ec));
    }
    // a symbolic link to a private directory: refused (it could be repointed after the check),
    // also written "link/" or "link/." (lstat() would follow the link then)
    {
      const std::filesystem::path target = dir / "target", link = dir / "link";
      std::filesystem::create_directories(target, ec);
      std::filesystem::permissions(target, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
      std::filesystem::create_directory_symlink(target, link, ec);
      check("a symbolic link to a private directory", !ec);
      for (const char *suffix : { "", "/", "//", "/." }) {
        CL::Hooks linkHooks = hooks;
        linkHooks.programCacheDirectory = link.string() + suffix;
        bool warned = false;
        linkHooks.log = [&warned, log = hooks.log](OFX::Host::LogLevel level, const std::string &message) {
          warned = warned || (level == OFX::Host::LogLevel::Warning && message.find("program cache directory") != std::string::npos);
          if (log) {
            log(level, message);
          }
        };
        std::unique_ptr<CL::Device> linked = CL::createDevice(linkHooks);
        dev = linked.get();
        CL::Queue queue(*dev);
        Scope scope(instance, Scope::API::OpenCL, queue.native());
        const OfxStatus first = suite->compileProgram(kFillSource, 0, &program);
        const bool ran = first == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f;
        check(std::string("a cache directory \"link") + suffix + "\" (a symbolic link): a Warning, the memory cache only "
              "(nothing written there)", ran && warned && std::filesystem::is_empty(target, ec));
      }
      // the same directory, without the link and with a trailing "/": used
      CL::Hooks slashHooks = hooks;
      slashHooks.programCacheDirectory = target.string() + "/";
      std::unique_ptr<CL::Device> direct = CL::createDevice(slashHooks);
      dev = direct.get();
      CL::Queue queue(*dev);
      Scope scope(instance, Scope::API::OpenCL, queue.native());
      const OfxStatus st = suite->compileProgram(kFillSource, 0, &program);
      check("a private cache directory written with a trailing \"/\": used",
            st == kOfxStatOK && runFill(*dev, queue.native(), program, 2.f) == 6.f && !std::filesystem::is_empty(target, ec));
    }
#endif
    dev = nullptr;
    host.setOpenCLProgramCompiler(MyHost::Host::OpenCLProgramCompiler());
    check("the compiler removed: no OpenCL program suite", host.fetchSuite(kOfxOpenCLProgramSuite, 1) == nullptr);
    std::filesystem::remove_all(dir, ec);
  }

  void checkRenders(CLTestHost &host, CL::Device &device, const CL::Hooks &hooks)
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
    std::unique_ptr<CLTestInstance> instance(dynamic_cast<CLTestInstance *>(plugin->createInstance(kOfxImageEffectContextFilter, NULL)));
    check("createInstance", instance != nullptr);
    if (!instance) {
      return;
    }
    const OfxStatus created = instance->createInstanceAction();
    check("createInstanceAction", created == kOfxStatOK || created == kOfxStatReplyDefault);
    // (with any build of the plug-in: only its instance is needed)
    checkPrograms(host, *instance, hooks);
    Property::Set &props = instance->getProps();
    if (props.getStringProperty(kOfxImageEffectPropOpenCLRenderSupported) != "true" ||
        props.getStringProperty(kOfxImageEffectPropOpenCLSupported) != "true") {
      skip(std::string(pluginId) + " was built without OpenCL (OFX_SUPPORTS_OPENCLRENDER): the render checks");
      instance->destroyInstanceAction();
      return;
    }

    // GPUGain: Buffers and Images "true", the CPU "true"
    check("chooseRenderMode(Buffers and Images) = Buffers", CL::chooseRenderMode(*instance, true, true) == CL::RenderMode::Buffers);
    check("chooseRenderMode(Buffers and Images, preferImages) = Images", CL::chooseRenderMode(*instance, true, true, true) == CL::RenderMode::Images);
    check("chooseRenderMode(Images only) = Images", CL::chooseRenderMode(*instance, false, true) == CL::RenderMode::Images);
    check("chooseRenderMode(no OpenCL) = CPU", CL::chooseRenderMode(*instance, false, false) == CL::RenderMode::CPU);
    props.setStringProperty(kOfxImageEffectPropOpenCLSupported, "false");
    check("chooseRenderMode(plug-in Buffers only, preferImages) = Buffers", CL::chooseRenderMode(*instance, true, true, true) == CL::RenderMode::Buffers);
    check("chooseRenderMode(plug-in Buffers only, host Images only) = CPU", CL::chooseRenderMode(*instance, false, true) == CL::RenderMode::CPU);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "false");
    check("chooseRenderMode(plug-in Buffers only and no CPU, host Images only) = None", CL::chooseRenderMode(*instance, false, true) == CL::RenderMode::None);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "true");
    props.setStringProperty(kOfxImageEffectPropOpenCLSupported, "true");
    {
      // OpenCL Images only for RGBA clips (the session's are CL_RGBA): a connected Alpha or RGB
      // input, or output, gets Buffers, or the CPU without them
      MyHost::InputSpec spec;
      std::string error;
      MyHost::parseInputSpec("ramp", spec, error);
      ImageEffect::ClipInstance *source = instance->getClip(kOfxImageEffectSimpleSourceClipName);
      ImageEffect::ClipInstance *output = instance->getClip(kOfxImageEffectOutputClipName);
      dynamic_cast<MyHost::MyClipInstance *>(source)->setInput(spec);
      instance->getClipPreferences();
      check("RGBA clips: chooseRenderMode(Buffers and Images, preferImages) = Images",
            CL::chooseRenderMode(*instance, true, true, true) == CL::RenderMode::Images);
      for (const char *c : { kOfxImageComponentAlpha, kOfxImageComponentRGB }) {
        for (ImageEffect::ClipInstance *clip : { source, output }) {
          const std::string what = std::string(clip == source ? "an input" : "the output") + " of " + c;
          clip->setComponents(c);
          check(what + ": chooseRenderMode(Buffers and Images, preferImages) = Buffers",
                CL::chooseRenderMode(*instance, true, true, true) == CL::RenderMode::Buffers);
          check(what + ": chooseRenderMode(Images only) = CPU", CL::chooseRenderMode(*instance, false, true) == CL::RenderMode::CPU);
          clip->setComponents(kOfxImageComponentRGBA);
        }
      }
      instance->getClipPreferences();
    }

    checkCopies(device, *instance, *instance->getClip(kOfxImageEffectOutputClipName));

    if (Param::DoubleInstance *scale = dynamic_cast<Param::DoubleInstance *>(instance->getParam("scale"))) {
      scale->set(0.5);
    }
    check("GPUGain's scale param set to 0.5", dynamic_cast<Param::DoubleInstance *>(instance->getParam("scale")) != nullptr);

    RenderResult r = render(*instance, device, CL::RenderMode::CPU, "ramp");
    check("CPU render: kOfxStatOK, the source's r, g and b times 0.5 (max error " + std::to_string(r.maxError) + ")",
          r.status == kOfxStatOK && r.maxError <= 1e-6f);
    check("CPU render: kOfxImageEffectPropOpenCLEnabled 0 and no queue in the inArgs", instance->args == expectedArgs(0, nullptr));
    r = render(*instance, device, CL::RenderMode::CPU, "ramp", /*halfWindow=*/true);
    check("a render that leaves half the output unwritten (NaN): an infinite error", r.status == kOfxStatOK && r.maxError == INFINITY);

    for (CL::RenderMode mode : { CL::RenderMode::Buffers, CL::RenderMode::Images }) {
      const std::string name = modeName(mode);
      if (mode == CL::RenderMode::Images && !device.info().imageSupport) {
        skip("OpenCL Images render: the device has no image support");
        continue;
      }
      r = render(*instance, device, mode, "ramp");
      check(name + " render: kOfxStatOK, read back, the source's r, g and b times 0.5 (max error " + std::to_string(r.maxError) + ")",
            r.status == kOfxStatOK && r.readBack && !r.failed && r.maxError <= 1e-6f);
      check(name + " render: kOfxImageEffectPropOpenCLEnabled 1 and the queue in the inArgs",
            instance->args.size() == 3 && instance->args[0].queue != nullptr &&
            instance->args == expectedArgs(1, instance->args[0].queue));
      check(name + " render: the plug-in released its images (the output is left)", r.liveImages == 1);
    }

    // kOfxImageEffectPropOpenCLEnabled only "If a plug-in and host have both set
    // kOfxImageEffectPropOpenCLRenderSupported="true" or have both set
    // kOfxImageEffectPropOpenCLSupported="true"": a session for a mode one of them doesn't support
    // isn't ok(), and the actions get 0 and no queue
    props.setStringProperty(kOfxImageEffectPropOpenCLSupported, "false");
    r = render(*instance, device, CL::RenderMode::Images, "ramp");
    check("an Images session for a plug-in without Images: not OpenCL (0 and no queue in the inArgs)",
          instance->args == expectedArgs(0, nullptr));
    props.setStringProperty(kOfxImageEffectPropOpenCLSupported, "true");
    Property::Set &hostProps = host.getProperties();
    hostProps.setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "false");
    r = render(*instance, device, CL::RenderMode::Buffers, "ramp");
    check("a Buffers session while the host's kOfxImageEffectPropOpenCLRenderSupported is \"false\": not OpenCL",
          instance->args == expectedArgs(0, nullptr));
    hostProps.setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "true");
    // "needed" (upstream main's @propdef of kOfxImageEffectPropOpenCLRenderSupported): Buffers,
    // and nothing inferred about the CPU
    props.setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "needed");
    check("chooseRenderMode(plug-in Buffers \"needed\") = Buffers", CL::chooseRenderMode(*instance, true, false) == CL::RenderMode::Buffers);
    check("chooseRenderMode(plug-in Buffers \"needed\", no OpenCL) = CPU (kOfxImageEffectPropCPURenderSupported decides)",
          CL::chooseRenderMode(*instance, false, false) == CL::RenderMode::CPU);
    r = render(*instance, device, CL::RenderMode::Buffers, "ramp");
    check("a Buffers render of a plug-in that says \"needed\": OpenCL, the expected pixels",
          r.status == kOfxStatOK && r.readBack && instance->args.size() == 3 && instance->args[0].enabled == 1 &&
          r.maxError <= 1e-6f);
    props.setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "true");

    // GPUGain without a source: kOfxStatGPURenderFailed with OpenCL ("the GPU kernels need a
    // source image: let the host render on the CPU"), then the CPU retry
    r = render(*instance, device, CL::RenderMode::Buffers, "none");
    check("Buffers render without a source: kOfxStatGPURenderFailed, the CPU retry allowed",
          r.status == kOfxStatGPURenderFailed && r.failed && r.cpuRetry);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "false");
    r = render(*instance, device, CL::RenderMode::Buffers, "none");
    check("the same, with kOfxImageEffectPropCPURenderSupported \"false\": no CPU retry", r.failed && !r.cpuRetry);
    props.setStringProperty(kOfxImageEffectPropCPURenderSupported, "true");

    {
      // the scope applies to its instance only, and a nested scope of API None to none
      std::unique_ptr<ImageEffect::Instance> other(plugin->createInstance(kOfxImageEffectContextFilter, NULL));
      int queueTag = 0;
      ImageEffect::GPURenderScope scope(*other, ImageEffect::GPURenderScope::API::OpenCL, &queueTag);
      r = render(*instance, device, CL::RenderMode::CPU, "ramp");
      check("a GPURenderScope of another instance: no OpenCL in the inArgs", instance->args == expectedArgs(0, nullptr));
      ImageEffect::GPURenderScope outer(*instance, ImageEffect::GPURenderScope::API::OpenCL, &queueTag);
      {
        ImageEffect::GPURenderScope inner(*instance, ImageEffect::GPURenderScope::API::CPU, &queueTag);
        r = render(*instance, device, CL::RenderMode::CPU, "ramp");
        check("a nested GPURenderScope of API CPU: no OpenCL in the inArgs", instance->args == expectedArgs(0, nullptr));
      }
      check("the outer scope again", ImageEffect::GPURenderScope::find(*instance) == &outer &&
            outer.api() == ImageEffect::GPURenderScope::API::OpenCL && outer.queue() == &queueTag);
    }
    check("no scope left", ImageEffect::GPURenderScope::find(*instance) == nullptr);
    instance->destroyInstanceAction();
  }

  ////////////////////////////////////////////////////////////////////////////////
  // a queue whose clFinish fails

  typedef int32_t (CHECKS_CLAPI *FinishFn)(void *);
  FinishFn gRealFinish = nullptr;
  bool gFailFinish = false;
  int32_t CHECKS_CLAPI failingFinish(void *queue)
  {
    return gFailFinish ? -5 /* CL_OUT_OF_RESOURCES */ : gRealFinish(queue);
  }

  void *openCLSymbol(const char *name)
  {
#if defined(_WIN32)
    HMODULE h = LoadLibraryA("OpenCL.dll");
    return h ? reinterpret_cast<void *>(GetProcAddress(h, name)) : nullptr;
#else
#if defined(__APPLE__)
    void *h = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_NOW | RTLD_LOCAL);
#else
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      h = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
    }
#endif
    return h ? dlsym(h, name) : nullptr; // (left loaded)
#endif
  }

  /// ~Queue releases a queue whose clFinish failed instead of lending it again
  void checkFailedQueue()
  {
    CL::Hooks hooks;
    hooks.getProcAddress = [](const char *name) -> void * {
      if (std::strcmp(name, "clFinish") == 0) {
        gRealFinish = reinterpret_cast<FinishFn>(openCLSymbol(name));
        return gRealFinish ? reinterpret_cast<void *>(&failingFinish) : nullptr;
      }
      return openCLSymbol(name);
    };
    std::unique_ptr<CL::Device> device = CL::createDevice(hooks);
    check("a device with a clFinish that can fail", device != nullptr);
    if (!device) {
      return;
    }
    void *failed = nullptr;
    {
      CL::Queue q(*device);
      failed = q.native();
      gFailFinish = true;
    }
    gFailFinish = false;
    check("a queue whose clFinish failed: released, not given back", failed && device->queueCount() == 0);
    {
      CL::Queue q(*device);
      check("the next queue: a new one", q.native() && device->queueCount() == 1);
    }
    check("a queue whose clFinish worked: given back", device->queueCount() == 1);
  }

} // namespace

int main()
{
  {
    CL::Hooks none;
    none.getProcAddress = [](const char *) -> void * { return nullptr; };
    std::string error;
    const bool noDevice = !CL::createDevice(none, -1, -1, &error);
    check("a loader that finds nothing: no device (" + error + ")", noDevice && !error.empty());
    check("a loader that finds nothing: no device listed", CL::listDevices(none).empty());
  }

  CL::Hooks hooks;
  hooks.log = [](OFX::Host::LogLevel, const std::string &message) { std::printf("  %s\n", message.c_str()); };
  std::string error;
  const std::vector<CL::DeviceInfo> devices = CL::listDevices(hooks, &error);
  for (const CL::DeviceInfo &d : devices) {
    std::printf("  device %d.%d: %s (%s, %s, driver %s)%s%s, on %s (%s)\n", d.platform, d.device, d.name.c_str(),
                d.vendor.c_str(), d.version.c_str(), d.driverVersion.c_str(), d.gpu ? ", GPU" : "",
                d.imageSupport ? ", images" : "", d.platformName.c_str(), d.platformVersion.c_str());
  }
  std::unique_ptr<CL::Device> device = CL::createDevice(hooks, -1, -1, &error);
  if (!device) {
    skip("no OpenCL device (" + error + "): the device, copy and render checks");
  } else {
    check("listDevices: the device is listed", !devices.empty());
    check("createDevice(" + std::to_string(devices.back().platform) + ", " + std::to_string(devices.back().device) +
          "): that device", [&] {
            std::unique_ptr<CL::Device> last = CL::createDevice(hooks, devices.back().platform, devices.back().device);
            return last && last->info().name == devices.back().name;
          }());
    check("createDevice() of a device that doesn't exist: nullptr", !CL::createDevice(hooks, 99, 0));
    {
      CL::Queue a(*device);
      CL::Queue b(*device);
      check("two queues at once: two command queues", a.native() && b.native() && a.native() != b.native() &&
            device->queueCount() == 2);
    }
    {
      CL::Queue c(*device);
      check("a queue after they were given back: an idle one", c.native() && device->queueCount() == 2 && c.finish());
    }
    checkFailedQueue();
    PluginCache::useStdOFXPluginsLocation(false); // only OFX_PLUGIN_PATH
    CLTestHost host;
    ImageEffect::gImageEffectHost = &host;
    // the host can pass OpenCL Buffers and, if the device can, Images (a plug-in reads it in
    // kOfxActionDescribe)
    host.getProperties().setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "true");
    host.getProperties().setStringProperty(kOfxImageEffectPropOpenCLSupported, device->info().imageSupport ? "true" : "false");
    checkRenders(host, *device, hooks);
  }
  std::printf("openclRenderChecks: %d failure(s)\n", GPUChecks::gFailures);
  return GPUChecks::gFailures ? 1 : 0;
}
