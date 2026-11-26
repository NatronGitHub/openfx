// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * propsetDump: dump the property sets that HostSupport actually creates, as JSON.
 *
 * Used by scripts/audit-propsets.py (NatronGitHub/openfx#79) to compare HostSupport
 * against the OFX property-set metadata (@propset / @actiondef in include/).
 *
 * Usage: OFX_PLUGIN_PATH=<dir> propsetDump <pluginIdentifier> [output.json]
 * The plug-in should support the filter context (e.g. the Invert sample from
 * Support/Plugins, "net.sf.openfx.invertPlugin").
 *
 * It dumps the host descriptor, the effect descriptor and instance, the clip
 * descriptors and instances, the images, every parameter type (descriptor and
 * instance), an interact descriptor and instance, and the inArgs/outArgs of every
 * action HostSupport sends to the plug-in during a typical run.
 *
 * This file is distributed under the same license as the rest of HostSupport.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxInteract.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

#include "ofxhBinary.h"
#include "ofxhPropertySuite.h"
#include "ofxhClip.h"
#include "ofxhParam.h"
#include "ofxhMemory.h"
#include "ofxhImageEffect.h"
#include "ofxhPluginAPICache.h"
#include "ofxhPluginCache.h"
#include "ofxhHost.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhInteract.h"

#include "hostDemoHostDescriptor.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"

namespace {

  std::string jsonEscape(const std::string &s)
  {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (c == '"' || c == '\\') { r += '\\'; r += c; }
      else if ((unsigned char)c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); r += buf; }
      else { r += c; }
    }
    return r;
  }

  const char *typeName(OFX::Host::Property::TypeEnum t)
  {
    switch (t) {
    case OFX::Host::Property::eInt: return "int";
    case OFX::Host::Property::eDouble: return "double";
    case OFX::Host::Property::eString: return "string";
    case OFX::Host::Property::ePointer: return "pointer";
    default: return "unknown";
    }
  }

  // set name -> JSON object text (first occurrence wins)
  std::map<std::string, std::string> gSets;
  std::vector<std::string> gOrder;

  void dumpSet(const std::string &setName, const OFX::Host::Property::Set *set)
  {
    if (!set || gSets.count(setName)) {
      return;
    }
    std::ostringstream os;
    os << "{";
    const OFX::Host::Property::PropertyMap &props = set->getProperties();
    bool first = true;
    for (OFX::Host::Property::PropertyMap::const_iterator it = props.begin(); it != props.end(); ++it) {
      OFX::Host::Property::Property *p = it->second;
      if (!first) os << ",";
      first = false;
      os << "\n    \"" << jsonEscape(it->first) << "\": {\"type\": \"" << typeName(p->getType())
         << "\", \"dimension\": " << p->getFixedDimension()
         << ", \"pluginReadOnly\": " << (p->getPluginReadOnly() ? "true" : "false") << "}";
    }
    os << "\n  }";
    gSets[setName] = os.str();
    gOrder.push_back(setName);
  }

  /// effect instance that records the arguments of every action sent to the plug-in
  class DumpEffectInstance : public MyHost::MyEffectInstance {
  public:
    DumpEffectInstance(OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                       OFX::Host::ImageEffect::Descriptor& desc,
                       const std::string& context)
    : MyHost::MyEffectInstance(plugin, desc, context)
    {}

    virtual OfxStatus mainEntry(const char *action, const void *handle,
                                OFX::Host::Property::Set *inArgs, OFX::Host::Property::Set *outArgs)
    {
      std::string a = action;
      if (a == kOfxActionInstanceChanged && inArgs) {
        // distinguish param and clip changes, which have different property values but the same set
        const std::string &type = inArgs->getStringProperty(kOfxPropType);
        a += std::string(":") + type;
      }
      dumpSet("action:" + a + ":inArgs", inArgs);
      dumpSet("action:" + a + ":outArgs", outArgs);
      return MyHost::MyEffectInstance::mainEntry(action, handle, inArgs, outArgs);
    }
  };

  class DumpHost : public MyHost::Host {
  public:
    virtual OFX::Host::ImageEffect::Instance* newInstance(void* /*clientData*/,
                                                          OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                                                          OFX::Host::ImageEffect::Descriptor& desc,
                                                          const std::string& context)
    {
      return new DumpEffectInstance(plugin, desc, context);
    }
  };

  /// minimal interact instance, only to see the properties HostSupport declares
  class DumpInteract : public OFX::Host::Interact::Instance {
  public:
    DumpInteract(OFX::Host::Interact::Descriptor &desc, void *effectInstance)
    : OFX::Host::Interact::Instance(desc, effectInstance) {}
    virtual void getViewportSize(double &width, double &height) const { width = 720; height = 576; }
    virtual void getPixelScale(double& xScale, double& yScale) const { xScale = yScale = 1.; }
    virtual double getScreenPixelRatio() const { return 1.; }
    virtual void getBackgroundColour(double &r, double &g, double &b) const { r = g = b = 0.; }
    virtual bool getSuggestedColour(double &r, double &g, double &b) const { r = g = b = 1.; return false; }
    virtual OfxStatus swapBuffers() { return kOfxStatOK; }
    virtual OfxStatus redraw() { return kOfxStatOK; }
  };

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2) {
    std::cerr << "usage: OFX_PLUGIN_PATH=<dir> " << argv[0] << " <pluginIdentifier> [output.json]" << std::endl;
    return 2;
  }
  const std::string pluginId = argv[1];

  OFX::Host::PluginCache::getPluginCache()->setCacheVersion("propsetDumpV1");
  DumpHost myHost;
  OFX::Host::ImageEffect::PluginCache imageEffectPluginCache(&myHost);
  imageEffectPluginCache.registerInCache(*OFX::Host::PluginCache::getPluginCache());
  OFX::Host::PluginCache::getPluginCache()->scanPluginFiles();

  // host descriptor
  dumpSet("ImageEffectHost", &myHost.getProperties());

  OFX::Host::ImageEffect::ImageEffectPlugin* plugin = imageEffectPluginCache.getPluginById(pluginId);
  if (!plugin) {
    std::cerr << "plug-in " << pluginId << " not found (check OFX_PLUGIN_PATH)" << std::endl;
    return 1;
  }

  // parameter types, descriptors and instances (created through the demo host's factory)
  const char *paramTypes[] = {
    kOfxParamTypeInteger, kOfxParamTypeDouble, kOfxParamTypeBoolean, kOfxParamTypeChoice,
    kOfxParamTypeStrChoice, kOfxParamTypeRGBA, kOfxParamTypeRGB, kOfxParamTypeDouble2D,
    kOfxParamTypeInteger2D, kOfxParamTypeDouble3D, kOfxParamTypeInteger3D, kOfxParamTypeString,
    kOfxParamTypeCustom, kOfxParamTypeBytes, kOfxParamTypeGroup, kOfxParamTypePage,
    kOfxParamTypePushButton,
#ifdef OFX_SUPPORTS_PARAMETRIC
    kOfxParamTypeParametric,
#endif
    NULL
  };

  OFX::Host::auto_ptr<OFX::Host::ImageEffect::Instance> instance(plugin->createInstance(kOfxImageEffectContextFilter, NULL));
  if (!instance.get()) {
    std::cerr << "could not create a filter instance of " << pluginId << std::endl;
    return 1;
  }

  // effect and clip descriptors (the context descriptor the instance was made from)
  OFX::Host::ImageEffect::Descriptor &desc = instance->getDescriptor();
  dumpSet("EffectDescriptor", &desc.getProps());
  {
    const std::map<std::string, OFX::Host::ImageEffect::ClipDescriptor*> &clips = desc.getClips();
    for (std::map<std::string, OFX::Host::ImageEffect::ClipDescriptor*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
      dumpSet("ClipDescriptor", &it->second->getProps());
    }
  }

  for (int i = 0; paramTypes[i]; ++i) {
    const std::string type = paramTypes[i];
    OFX::Host::Param::Descriptor *pd = new OFX::Host::Param::Descriptor(type, "dump_" + type);
    pd->addStandardParamProps(type);
    dumpSet("ParamDescriptor:" + type, &pd->getProperties());
    OFX::Host::Param::Instance *pi = NULL;
    try {
      pi = static_cast<MyHost::MyEffectInstance*>(instance.get())->newParam("dump_" + type, *pd);
    } catch (...) {
      pi = NULL;
    }
    if (pi) {
      dumpSet("ParamInstance:" + type, &pi->getProperties());
      delete pi;
    }
    delete pd;
  }

  OfxStatus stat = instance->createInstanceAction();
  dumpSet("EffectInstance", &instance->getProps());
  OFX::Host::ImageEffect::ClipInstance *srcClip = instance->getClip(kOfxImageEffectSimpleSourceClipName);
  OFX::Host::ImageEffect::ClipInstance *dstClip = instance->getClip(kOfxImageEffectOutputClipName);
  if (srcClip) dumpSet("ClipInstance", &srcClip->getProps());

  instance->getClipPreferences();

  OfxPointD renderScale; renderScale.x = renderScale.y = 1.;
  OfxRectI renderWindow; renderWindow.x1 = renderWindow.y1 = 0; renderWindow.x2 = 720; renderWindow.y2 = 576;
  OfxRectD roi; roi.x1 = roi.y1 = 0; roi.x2 = 720; roi.y2 = 576;
  const OfxTime t = 0;

  OfxRectD rod;
  instance->getRegionOfDefinitionAction(t, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                        0,
#endif
                                        rod);
  std::map<OFX::Host::ImageEffect::ClipInstance *, OfxRectD> rois;
  instance->getRegionOfInterestAction(t, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                      0,
#endif
                                      roi, rois);
  OFX::Host::ImageEffect::RangeMap rangeMap;
  instance->getFrameNeededAction(t, rangeMap);
  {
    OfxTime identityTime = t;
    std::string identityClip;
#ifdef OFX_EXTENSIONS_NUKE
    int identityView = 0;
    std::string identityPlane;
#endif
    instance->isIdentityAction(identityTime, kOfxImageFieldNone, renderWindow, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                               identityView, identityPlane,
#endif
                               identityClip);
  }
  OfxRangeD range;
  instance->getTimeDomainAction(range);

  instance->beginRenderAction(0, 1, 1.0, false, renderScale, /*sequential=*/true, /*interactive=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                              /*openGLRender=*/false,
#  ifdef OFX_EXTENSIONS_NATRON
                              /*contextData=*/NULL,
#  endif
#endif
                              /*draftRender=*/false
#ifdef OFX_EXTENSIONS_NUKE
                              , /*view=*/0
#endif
                              );
#ifdef OFX_EXTENSIONS_NUKE
  std::list<std::string> planes;
  planes.push_back(kFnOfxImagePlaneColour);
#endif
  instance->renderAction(t, kOfxImageFieldNone, renderWindow, renderScale, /*sequential=*/true, /*interactive=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                         /*openGLRender=*/false,
#  ifdef OFX_EXTENSIONS_NATRON
                         /*contextData=*/NULL,
#  endif
#endif
                         /*draftRender=*/false
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
                         , /*view=*/0
#endif
#ifdef OFX_EXTENSIONS_VEGAS
                         , /*nViews=*/1
#endif
#ifdef OFX_EXTENSIONS_NUKE
                         , planes
#endif
                         );
  instance->endRenderAction(0, 1, 1.0, false, renderScale, /*sequential=*/true, /*interactive=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                            /*openGLRender=*/false,
#  ifdef OFX_EXTENSIONS_NATRON
                            /*contextData=*/NULL,
#  endif
#endif
                            /*draftRender=*/false
#ifdef OFX_EXTENSIONS_NUKE
                            , /*view=*/0
#endif
                            );

  // images: the source image (fetched as a plug-in would) and the output image
  if (srcClip) {
    OFX::Host::ImageEffect::Image *img = srcClip->getImage(t, NULL);
    if (img) {
      dumpSet("Image", img); // ImageBase is a Property::Set
      img->releaseReference();
    }
  }
  (void)dstClip;

  // instance changed (param and clip), purge caches, sync private data, instance edit
  instance->beginInstanceChangedAction(kOfxChangeUserEdited);
  if (srcClip) {
    instance->clipInstanceChangedAction(kOfxImageEffectSimpleSourceClipName, kOfxChangeUserEdited, t, renderScale);
  }
  {
    // a param instance change needs a param of the instance: use any
    const std::map<std::string, OFX::Host::Param::Instance*> &params = instance->getParams();
    if (!params.empty()) {
      instance->paramInstanceChangedAction(params.begin()->first, kOfxChangeUserEdited, t, renderScale);
    }
  }
  instance->endInstanceChangedAction(kOfxChangeUserEdited);
  instance->purgeCachesAction();
  instance->syncPrivateDataAction();
  instance->beginInstanceEditAction();
  instance->endInstanceEditAction();
  {
    std::vector<std::string> preferred;
    std::string colourspace;
    instance->getOutputColourspaceAction(preferred, colourspace);
  }

  // interact descriptor and instance (standalone; the plug-in needs no overlay for this)
  {
    OFX::Host::Interact::Descriptor idesc;
    dumpSet("InteractDescriptor", &idesc.getProperties());
    DumpInteract iinst(idesc, instance.get());
    dumpSet("InteractInstance", &iinst.getProperties());
  }

  stat = instance->destroyInstanceAction();
  (void)stat;

  // output
  std::ostringstream os;
  os << "{\n";
  os << "  \"defines\": [";
  const char *defines[] = {
#ifdef OFX_EXTENSIONS_NATRON
    "OFX_EXTENSIONS_NATRON",
#endif
#ifdef OFX_EXTENSIONS_NUKE
    "OFX_EXTENSIONS_NUKE",
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    "OFX_EXTENSIONS_VEGAS",
#endif
#ifdef OFX_EXTENSIONS_TUTTLE
    "OFX_EXTENSIONS_TUTTLE",
#endif
#ifdef OFX_EXTENSIONS_RESOLVE
    "OFX_EXTENSIONS_RESOLVE",
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
    "OFX_SUPPORTS_OPENGLRENDER",
#endif
#ifdef OFX_SUPPORTS_PARAMETRIC
    "OFX_SUPPORTS_PARAMETRIC",
#endif
#ifdef OFX_SUPPORTS_DIALOG
    "OFX_SUPPORTS_DIALOG",
#endif
    NULL
  };
  for (int i = 0; defines[i]; ++i) {
    os << (i ? ", " : "") << "\"" << defines[i] << "\"";
  }
  os << "],\n  \"sets\": {";
  for (size_t i = 0; i < gOrder.size(); ++i) {
    os << (i ? "," : "") << "\n  \"" << jsonEscape(gOrder[i]) << "\": " << gSets[gOrder[i]];
  }
  os << "\n  }\n}\n";

  if (argc >= 3) {
    std::ofstream f(argv[2]);
    f << os.str();
  } else {
    std::cout << os.str();
  }
  return 0;
}
