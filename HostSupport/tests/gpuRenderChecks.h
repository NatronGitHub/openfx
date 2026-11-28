#ifndef GPU_RENDER_CHECKS_H
#define GPU_RENDER_CHECKS_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// What the checks of the GPU components share (openglRenderChecks, openclRenderChecks,
// metalRenderChecks, cudaRenderChecks): the results, the rule tables (checkRules()), the CPU test
// images, the copies to a GPU session and back (checkCopies()), and the render of
// Support/Plugins/GPUGain compared with the expected pixels. They are functional checks (#361 and
// its parts: #362, #370, #379, #388); the GPURenderScope checks of every API are in
// suiteHandleChecks (checkGPURenderScope()), which runs without a device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include "ofxhImageEffect.h"
#include "ofxhUtilities.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
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

  /// a string property value of a rule table's row: set while the row is checked, then put back
  struct Setting {
    Property::Set *set;
    const char *name;
    std::string value;
  };

  /// a row of a rule table: its settings, and what must then hold
  struct Rule {
    std::string what;
    std::vector<Setting> settings;
    std::function<bool()> ok;
  };

  /// the rows of a rule table (the GPU components' chooseRenderMode(), their session
  /// preconditions, "needed", and the CPU retry), each with its settings applied, and then the
  /// previous values put back
  inline void checkRules(const std::vector<Rule> &rules)
  {
    /// applies a row's settings, and puts the previous values back when it ends, also if ok()
    /// throws
    class Applied {
    public:
      explicit Applied(const std::vector<Setting> &settings) : _settings(settings)
      {
        for (const Setting &s : _settings) {
          _saved.push_back(s.set->getStringProperty(s.name));
          s.set->setStringProperty(s.name, s.value);
        }
      }
      ~Applied()
      {
        for (size_t i = _saved.size(); i-- > 0;) {
          _settings[i].set->setStringProperty(_settings[i].name, _saved[i]);
        }
      }
      Applied(const Applied &) = delete;
      Applied &operator=(const Applied &) = delete;
    private:
      const std::vector<Setting> &_settings;
      std::vector<std::string> _saved;
    };
    for (const Rule &rule : rules) {
      Applied applied(rule.settings);
      check(rule.what, rule.ok());
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

  /// what checkCopies() needs of a GPU component. Fixture: a session of the component (and what
  /// it needs, e.g. its queue), as its member session, made by make().
  template <class Fixture>
  struct CopyChecks {
    std::string prefix;  ///< of the case names, e.g. "Buffers " (the mode)
    std::string suffix;  ///< e.g. " (fake driver)"
    std::function<std::unique_ptr<Fixture>()> make;
    std::string copiedWhat;  ///< what copied() checks, after ": copied, "
    /// the component's own properties of the device image out of image
    std::function<bool(ImageEffect::Image &out, const TestImage &image)> copied;
    /// the components the session doesn't copy (setOutput() gives NULL, and failed()), if any
    std::function<bool(const char *components)> refuses;
    std::string refusedWhat;
    /// more checks of a copied image (before: the CPU image's packed pixels), if any
    std::function<void(ImageEffect::Image &out, const std::vector<char> &before, const std::string &what)> afterCopy;
  };

  /// the copies of a GPU session (OpenCL, Metal, CUDA: ofxhGPUImages.h's copyRect() and the
  /// component's device calls): setOutput() copies a CPU image to the device, for every depth,
  /// components and rowBytes (packed, padded, negative), with the image's depth, components and
  /// bounds; readOutput() copies it back over garbage; getImage() of the output clip is the
  /// output, another reference. A render window inside the bounds: only its pixels go to the
  /// device and come back, and a host pixel outside it, changed after setOutput(), keeps its new
  /// value. An empty image isn't copied (no device allocation of size 0).
  template <class Fixture>
  void checkCopies(const CopyChecks<Fixture> &c, ImageEffect::ClipInstance &clip)
  {
    const char *depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthHalf, kOfxBitDepthFloat };
    const char *components[] = { kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha };
    const OfxRectI bounds = { -3, 5, 14, 12 };
    auto rowsWord = [](int rows) { return rows == 0 ? "packed" : rows > 0 ? "padded" : "negative rowBytes"; };
    for (const char *d : depths) {
      for (const char *comps : components) {
        for (int rows : { 0, 1, -1 }) {
          const std::string what = c.prefix + d + " " + comps + " " + rowsWord(rows) + c.suffix;
          TestImage image(clip, d, comps, bounds, rows);
          const std::vector<char> before = image.pixels();
          std::unique_ptr<Fixture> f = c.make();
          ImageEffect::Image *out = f->session.setOutput(clip, image, bounds);
          if (c.refuses && c.refuses(comps)) {
            check(what + ": " + c.refusedWhat, !out && f->session.failed());
            continue;
          }
          check(what + ": copied, " + c.copiedWhat,
                out && !f->session.failed() && out->getPointerProperty(kOfxImagePropData) != nullptr &&
                out->getStringProperty(kOfxImageEffectPropPixelDepth) == d &&
                out->getStringProperty(kOfxImageEffectPropComponents) == comps &&
                out->getBounds().x1 == bounds.x1 && out->getBounds().y2 == bounds.y2 && c.copied(*out, image));
          if (out && c.afterCopy) {
            c.afterCopy(*out, before, what);
          }
          for (char &b : image.storage) {
            b = 0x55;
          }
          check(what + ": read back", f->session.readOutput() && image.pixels() == before);
          if (out) {
            ImageEffect::Image *again = f->session.getImage(clip, nullptr);
            check(what + ": getImage() of the output clip is the output, another reference",
                  again == out && f->session.liveImages() == 1);
            if (again) {
              again->releaseReference();
            }
          }
        }
      }
    }
    for (int rows : { 0, 1, -1 }) {
      const std::string what = c.prefix + rowsWord(rows) + c.suffix;
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, bounds, rows);
      const OfxRectI inner = { 0, 7, 9, 11 };
      std::unique_ptr<Fixture> f = c.make();
      const std::vector<char> before = image.pixels();
      const bool set = f->session.setOutput(clip, image, inner) != nullptr;
      for (char &b : image.storage) {
        b = 0x55;
      }
      bool ok = set && f->session.readOutput();
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
      // an empty image: no device allocation of size 0
      TestImage image(clip, kOfxBitDepthFloat, kOfxImageComponentRGBA, { 0, 0, 0, 4 }, 0);
      std::unique_ptr<Fixture> f = c.make();
      const std::string mode = c.prefix.empty() ? std::string() : " (" + c.prefix.substr(0, c.prefix.size() - 1) + ")";
      check("an empty image" + mode + ": not copied, failed()" + c.suffix, !f->session.getImage(clip, &image) && f->session.failed());
    }
  }

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
