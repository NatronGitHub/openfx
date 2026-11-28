#ifndef GPU_RENDER_CHECKS_H
#define GPU_RENDER_CHECKS_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// What the checks of the GPU components share (openclRenderChecks, metalRenderChecks,
// cudaRenderChecks): the results, the CPU test images, and the render of
// Support/Plugins/GPUGain compared with the expected pixels.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include "ofxhImageEffect.h"
#include "ofxhUtilities.h"
#include "hostDemoClipInstance.h"
#include "hostDemoEffectInstance.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

namespace GPUChecks {

  using namespace OFX::Host;

  /// the failed checks
  inline int gFailures = 0;

  inline void check(const std::string &what, bool ok)
  {
    std::printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
    gFailures += ok ? 0 : 1;
  }

  /// adds the error of a pixel component to maxError: a NaN (a pixel the plug-in didn't write,
  /// which keeps hostDemo's sentinel) makes it infinite (std::max would drop it)
  inline void addError(float &maxError, float value, float expected)
  {
    const float e = std::fabs(value - expected);
    if (OFX::IsNaN(e) || e > maxError) {
      maxError = OFX::IsNaN(e) ? INFINITY : e;
    }
  }

  /// the environment variable var is "1": the checks that need what's missing fail instead of being
  /// skipped
  inline bool required(const char *var = "OFXH_REQUIRE")
  {
    const char *r = std::getenv(var);
    return r && std::string(r) == "1";
  }

  inline void skip(const std::string &what, const char *var = "OFXH_REQUIRE")
  {
    if (required(var)) {
      check(what + " (" + var + "=1)", false);
    } else {
      std::printf("skipped: %s\n", what.c_str());
    }
  }

  /// a CPU image with its own pixels: rowBytes packed (0), padded (1) or negative (-1)
  class TestImage : public ImageEffect::Image {
  public:
    TestImage(ImageEffect::ClipInstance &clip, const std::string &depth, const std::string &components,
              const OfxRectI &bounds, int rows)
      : ImageEffect::Image(clip)
    {
      const int bytes = depth == kOfxBitDepthByte ? 1 : depth == kOfxBitDepthFloat ? 4 : 2;
      const int n = components == kOfxImageComponentRGBA ? 4 : components == kOfxImageComponentRGB ? 3 : 1;
      const int w = bounds.x2 - bounds.x1, h = bounds.y2 - bounds.y1;
      packed = w * n * bytes;
      const int stride = packed + (rows > 0 ? 12 : 0);
      // (at least a byte: an empty image has pixel data too)
      storage.assign(std::max<size_t>(1, static_cast<size_t>(stride) * h), 0);
      // negative rowBytes: the rows top down in memory, the data pointer at the bottom row
      char *data = rows < 0 ? &storage[static_cast<size_t>(stride) * (h - 1)] : storage.data();
      rowBytes = rows < 0 ? -stride : stride;
      setStringProperty(kOfxImageEffectPropPixelDepth, depth);
      setStringProperty(kOfxImageEffectPropComponents, components);
      setPointerProperty(kOfxImagePropData, data);
      setIntPropertyN(kOfxImagePropBounds, &bounds.x1, 4);
      setIntPropertyN(kOfxImagePropRegionOfDefinition, &bounds.x1, 4);
      setIntProperty(kOfxImagePropRowBytes, rowBytes);
      this->h = h;
      for (int y = 0; y < h; ++y) {
        for (int i = 0; i < packed; ++i) {
          row(y)[i] = static_cast<char>((y * 31 + i * 7 + 1) & 0x7F);
        }
      }
    }
    char *row(int y) { return static_cast<char *>(getPointerProperty(kOfxImagePropData)) + static_cast<std::ptrdiff_t>(y) * rowBytes; }
    std::vector<char> pixels()
    {
      std::vector<char> p;
      for (int y = 0; y < h; ++y) {
        p.insert(p.end(), row(y), row(y) + packed);
      }
      return p;
    }
    std::vector<char> storage;
    int packed = 0, rowBytes = 0, h = 0;
  };

  struct RenderResult {
    OfxStatus status = kOfxStatFailed;
    bool readBack = false;
    bool failed = true;
    bool cpuRetry = false;
    int liveImages = -1;      ///< after the actions
    float maxError = INFINITY; ///< vs the expected pixels
  };

  /// runs actions() (Begin/Render/EndSequenceRender: the first failure), with output's image
  /// outputImage over window, in a GPU session or not, and fills result's status, readBack,
  /// failed, cpuRetry and liveImages
  typedef std::function<void(MyHost::MyClipInstance &output, ImageEffect::Image &outputImage, const OfxRectI &window,
                             const std::function<OfxStatus()> &actions, RenderResult &result)>
    RunActions;

  /// Begin/Render/EndSequenceRender of the whole RoD at time 0 of instance, GPUGain with scale 0.5
  /// (r, g and b times 0.5), with source input (a hostDemo input spec; "none": not connected),
  /// sent by run; then result.maxError compares the output with the expected pixels.
  /// halfWindow: the render action gets the left half of the output's window only, so that the
  /// other half stays unwritten (hostDemo's NaN sentinel), which the comparison must catch
  inline RenderResult renderGPUGain(MyHost::MyEffectInstance &instance, const std::string &input, bool halfWindow,
                                    const RunActions &run)
  {
    RenderResult result;
    MyHost::MyClipInstance *output = dynamic_cast<MyHost::MyClipInstance *>(instance.getClip(kOfxImageEffectOutputClipName));
    MyHost::MyClipInstance *source = dynamic_cast<MyHost::MyClipInstance *>(instance.getClip(kOfxImageEffectSimpleSourceClipName));
    MyHost::InputSpec spec;
    std::string error;
    MyHost::parseInputSpec(input, spec, error);
    source->setInput(spec);
    source->setUnmappedBitDepth(kOfxBitDepthFloat);
    instance.getClipPreferences();
    const OfxPointD scale = { 1., 1. };
    OfxRectD rod = { 0., 0., 0., 0. };
    instance.getRegionOfDefinitionAction(0., scale,
#ifdef OFX_EXTENSIONS_NUKE
                                         0,
#endif
                                         rod);
    const OfxRectI window = MyHost::canonicalToPixelEnclosing(rod, scale, instance.getProjectPixelAspectRatio());
    std::map<ImageEffect::ClipInstance *, OfxRectD> rois;
    instance.getRegionOfInterestAction(0., scale,
#ifdef OFX_EXTENSIONS_NUKE
                                       0,
#endif
                                       rod, rois);
    instance.setRenderState(scale, window, rois);
    output->releaseOutputImage();
    ImageEffect::Image *outputImage = output->MyHost::MyClipInstance::getImage(0., nullptr);
#ifdef OFX_EXTENSIONS_NUKE
    std::list<std::string> planes(1, kFnOfxImagePlaneColour);
#endif
    auto actions = [&]() {
      OfxStatus st = instance.beginRenderAction(0., 0., 1., false, scale, false, false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                                false,
#ifdef OFX_EXTENSIONS_NATRON
                                                nullptr,
#endif
#endif
                                                false
#ifdef OFX_EXTENSIONS_NUKE
                                                , 0
#endif
                                                );
      if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        return st;
      }
      OfxRectI actionWindow = window;
      if (halfWindow) {
        actionWindow.x2 = window.x1 + (window.x2 - window.x1) / 2;
      }
      st = instance.renderAction(0., kOfxImageFieldNone, actionWindow, scale, false, false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                 false,
#ifdef OFX_EXTENSIONS_NATRON
                                 nullptr,
#endif
#endif
                                 false
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
                                 , 0
#endif
#ifdef OFX_EXTENSIONS_VEGAS
                                 , 1
#endif
#ifdef OFX_EXTENSIONS_NUKE
                                 , planes
#endif
                                 );
      const OfxStatus end = instance.endRenderAction(0., 0., 1., false, scale, false, false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                                     false,
#ifdef OFX_EXTENSIONS_NATRON
                                                     nullptr,
#endif
#endif
                                                     false
#ifdef OFX_EXTENSIONS_NUKE
                                                     , 0
#endif
                                                     );
      return st == kOfxStatOK && end != kOfxStatOK && end != kOfxStatReplyDefault ? end : st;
    };
    run(*output, *outputImage, window, actions, result);
    MyHost::MyImage *out = dynamic_cast<MyHost::MyImage *>(outputImage);
    if (result.readBack && out) {
      ImageEffect::Image *in = source->getConnected() ? source->MyHost::MyClipInstance::getImage(0., nullptr) : nullptr;
      MyHost::MyImage *src = dynamic_cast<MyHost::MyImage *>(in);
      if (src) {
        result.maxError = 0.f;
        for (int y = window.y1; y < window.y2; ++y) {
          for (int x = window.x1; x < window.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
              // (GPUGain's scale is on r, g and b; its alpha is scaled by scaleA, 1)
              const float expected = (c < 3 ? 0.5f : 1.f) * src->getComponentFloat(x, y, c);
              addError(result.maxError, out->getComponentFloat(x, y, c), expected);
            }
          }
        }
      }
      if (in) {
        in->releaseReference();
      }
    }
    instance.clearRenderState();
    outputImage->releaseReference();
    return result;
  }

  /// runs actions() without a GPU session (a CPU render)
  inline void runOnCPU(MyHost::MyClipInstance &, ImageEffect::Image &, const OfxRectI &,
                       const std::function<OfxStatus()> &actions, RenderResult &result)
  {
    result.status = actions();
    result.readBack = result.status == kOfxStatOK;
    result.failed = false;
  }

  /// runs actions() in session (of a GPU component: setOutput(), readOutput(), liveImages()), which
  /// slot points to meanwhile (the test clips' getImage() returns its images); failed and retry:
  /// the component's failure and CPU retry predicates
  template <class Session>
  void runInSession(Session &session, Session *&slot, MyHost::MyEffectInstance &instance, MyHost::MyClipInstance &output,
                    ImageEffect::Image &outputImage, const OfxRectI &window, const std::function<OfxStatus()> &actions,
                    RenderResult &result, bool (*failed)(const Session &, OfxStatus),
                    bool (*retry)(const ImageEffect::Instance &, const Session &, OfxStatus))
  {
    session.setOutput(output, outputImage, window);
    slot = &session;
    result.status = actions();
    slot = nullptr;
    result.liveImages = session.liveImages();
    result.readBack = result.status == kOfxStatOK && session.readOutput();
    result.failed = failed(session, result.status);
    result.cpuRetry = retry(instance, session, result.status);
  }

} // GPUChecks

#endif // GPU_RENDER_CHECKS_H
