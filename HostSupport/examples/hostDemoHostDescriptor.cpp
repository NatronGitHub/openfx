// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <iostream>
#include <cstdlib> // getenv
#include <cstring> // strcmp
#include <fstream>
#include <set>
#include <string>
#include <vector>

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxPixels.h"
#include "ofxDialog.h"
#include "ofxInteract.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxOpenGLRender.h"
#include "ofxParametricParam.h"
#include "ofxProgress.h"
#include "ofxTimeLine.h"

// ofx host
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

// my host
#include "hostDemoHostDescriptor.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"

namespace MyHost
{ 
  Host::Host()
    : _ofx14(false)
    , _ofx11(false)
    , _keepLoaded(false)
    , _normalisedDefaults(true)
  {
    const char *keepLoaded = getenv("OFX_HOSTDEMO_KEEP_LOADED");
    _keepLoaded = keepLoaded != NULL && strcmp(keepLoaded, "1") == 0;
    // set host properties... TODO
    _properties.setIntProperty(kOfxPropAPIVersion, 1, 0);
    _properties.setIntProperty(kOfxPropAPIVersion, 4, 1);
    _properties.setStringProperty(kOfxPropName, "OFXDemoHost");
    _properties.setStringProperty(kOfxPropLabel, "OFX Demo Host");
    _properties.setIntProperty(kOfxPropVersion, 1, 0);
    _properties.setIntProperty(kOfxPropVersion, 0, 1);
    _properties.setStringProperty(kOfxPropVersionLabel, "1.0");
    _properties.setIntProperty(kOfxImageEffectHostPropIsBackground, 0);
    _properties.setIntProperty(kOfxImageEffectPropSupportsOverlays, 0);
    _properties.setIntProperty(kOfxImageEffectPropSupportsMultiResolution, 0);
    _properties.setIntProperty(kOfxImageEffectPropSupportsTiles, true);
    _properties.setIntProperty(kOfxImageEffectPropTemporalClipAccess, true);
    _properties.setStringProperty(kOfxImageEffectPropSupportedComponents,  kOfxImageComponentRGBA, 0);
    _properties.setStringProperty(kOfxImageEffectPropSupportedComponents,  kOfxImageComponentAlpha, 1);
    // the depths of MyImage (hostDemoClipInstance.cpp) and of -d: no half. HostSupport leaves the
    // property empty ("the host must set it"), and a plug-in reading it would find no depth
    _properties.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte, 0);
    _properties.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthShort, 1);
    _properties.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 2);
    _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGenerator, 0 );
    _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 1);
    _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 2 );
    _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextTransition, 3 );
    _properties.setIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
    // MyEffectInstance::newParam() implements MyStrChoiceInstance
    _properties.setIntProperty(kOfxParamHostPropSupportsStrChoice, 1);
    // MyEffectInstance::newParam() implements MyBytesInstance
    _properties.setIntProperty(kOfxHostSupportHostPropSupportsBytes, 1);
    _properties.setIntProperty(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
    // 2: "the host will have set kOfxImageEffectPropSequentialRenderStatus on the relevant
    // actions" (hostDemo does, from --frames)
    _properties.setIntProperty(kOfxImageEffectInstancePropSequentialRender, 2);
    _properties.setIntProperty(kOfxImageEffectPropSetableFrameRate, 0);
    _properties.setIntProperty(kOfxImageEffectPropSetableFielding, 0);
    _properties.setIntProperty(kOfxParamHostPropSupportsCustomInteract, 0 );
    _properties.setIntProperty(kOfxParamHostPropSupportsStringAnimation, 0 );
    // (keys held until the next key: MyCurve)
    _properties.setIntProperty(kOfxParamHostPropSupportsChoiceAnimation, 1 );
    _properties.setIntProperty(kOfxParamHostPropSupportsBooleanAnimation, 1 );
    _properties.setIntProperty(kOfxParamHostPropSupportsCustomAnimation, 0 );
    _properties.setIntProperty(kOfxParamHostPropMaxParameters, -1);
    _properties.setIntProperty(kOfxParamHostPropMaxPages, 0);
    _properties.setIntProperty(kOfxParamHostPropPageRowColumnCount, 0, 0 );
    _properties.setIntProperty(kOfxParamHostPropPageRowColumnCount, 0, 1 );
  }
  
  // --ofx14: the host descriptor properties of OFX 1.4. These are the properties whose
  // documentation says "Property Set - host descriptor" in the OFX 1.4 headers (tag
  // OFX_Release_1_4_TAG), and the list of properties on the image effect host of the OFX 1.4
  // reference (Documentation/ofxPropertiesByObject.xml), which adds kOfxPropType, kOfxPropName,
  // kOfxPropLabel, kOfxImageEffectPropSupportedContexts, kOfxImageEffectPropRenderQualityDraft and
  // kOfxImageEffectHostPropNativeOrigin. Everything else is an OFX 1.5+ or extension property
  // (upstream's headers mark the OFX 1.5 ones as introduced or added in version 1.5): StrChoice,
  // Bytes, CPU, OpenCL, CUDA and Metal render support, colour management, the Natron and Nuke
  // host extensions.
  static const char* const kOFX14HostProps[] = {
    kOfxPropAPIVersion, kOfxPropType, kOfxPropName, kOfxPropLabel, kOfxPropVersion,
    kOfxPropVersionLabel, kOfxPropHostOSHandle,
    kOfxImageEffectHostPropIsBackground, kOfxImageEffectPropSupportsOverlays,
    kOfxImageEffectPropSupportsMultiResolution, kOfxImageEffectPropSupportsTiles,
    kOfxImageEffectPropTemporalClipAccess, kOfxImageEffectPropSupportedComponents,
    kOfxImageEffectPropSupportedPixelDepths, kOfxImageEffectPropSupportedContexts,
    kOfxImageEffectPropSupportsMultipleClipDepths, kOfxImageEffectPropSupportsMultipleClipPARs,
    kOfxImageEffectPropSetableFrameRate, kOfxImageEffectPropSetableFielding,
    kOfxImageEffectInstancePropSequentialRender, kOfxImageEffectPropOpenGLRenderSupported,
    kOfxImageEffectPropRenderQualityDraft, kOfxImageEffectHostPropNativeOrigin,
    kOfxParamHostPropSupportsCustomInteract, kOfxParamHostPropSupportsStringAnimation,
    kOfxParamHostPropSupportsChoiceAnimation, kOfxParamHostPropSupportsBooleanAnimation,
    kOfxParamHostPropSupportsCustomAnimation, kOfxParamHostPropSupportsParametricAnimation,
    kOfxParamHostPropMaxParameters, kOfxParamHostPropMaxPages, kOfxParamHostPropPageRowColumnCount,
    kOfxHostSupportHostPointer, // not an OFX property: HostSupport's fetchSuite needs it
    NULL
  };

  // --ofx14: the suites of OFX 1.4 (the suite structs of the OFX 1.4 headers). Not the DrawSuite,
  // the OpenCL program suite (OFX 1.5), or the Natron, Nuke and Vegas extension suites.
  struct SuiteVersion { const char* name; int version; };
  static const SuiteVersion kOFX14Suites[] = {
    { kOfxPropertySuite, 1 }, { kOfxMemorySuite, 1 }, { kOfxImageEffectSuite, 1 },
    { kOfxParameterSuite, 1 }, { kOfxParametricParameterSuite, 1 }, { kOfxInteractSuite, 1 },
    { kOfxMessageSuite, 1 }, { kOfxMessageSuite, 2 }, { kOfxProgressSuite, 1 }, { kOfxProgressSuite, 2 },
    { kOfxTimeLineSuite, 1 }, { kOfxMultiThreadSuite, 1 }, { kOfxOpenGLRenderSuite, 1 },
    { kOfxDialogSuite, 1 },
    { NULL, 0 }
  };

  // --ofx11: the host descriptor properties of OFX 1.1, as kOFX14HostProps: those whose
  // documentation says "Property Set - host descriptor" in the OFX 1.1 headers (tag
  // OFX_Release_1_1_TAG), with kOfxPropType, kOfxPropName, kOfxPropLabel and
  // kOfxImageEffectPropSupportedContexts, and kOfxPropAPIVersion: an OFX 1.2 property, but VEGAS
  // Pro, the OFX 1.1 host this profile is modelled on, reports API version 1.1 with it
  // (openfx-misc's README-hosts.txt). No kOfxPropVersion, kOfxPropVersionLabel, kOfxPropHostOSHandle,
  // kOfxImageEffectInstancePropSequentialRender, kOfxParamHostPropSupportsParametricAnimation (OFX
  // 1.2), kOfxImageEffectPropOpenGLRenderSupported (1.3), kOfxImageEffectPropRenderQualityDraft and
  // kOfxImageEffectHostPropNativeOrigin (1.4).
  static const char* const kOFX11HostProps[] = {
    kOfxPropAPIVersion, kOfxPropType, kOfxPropName, kOfxPropLabel,
    kOfxImageEffectHostPropIsBackground, kOfxImageEffectPropSupportsOverlays,
    kOfxImageEffectPropSupportsMultiResolution, kOfxImageEffectPropSupportsTiles,
    kOfxImageEffectPropTemporalClipAccess, kOfxImageEffectPropSupportedComponents,
    kOfxImageEffectPropSupportedPixelDepths, kOfxImageEffectPropSupportedContexts,
    kOfxImageEffectPropSupportsMultipleClipDepths, kOfxImageEffectPropSupportsMultipleClipPARs,
    kOfxImageEffectPropSetableFrameRate, kOfxImageEffectPropSetableFielding,
    kOfxParamHostPropSupportsCustomInteract, kOfxParamHostPropSupportsStringAnimation,
    kOfxParamHostPropSupportsChoiceAnimation, kOfxParamHostPropSupportsBooleanAnimation,
    kOfxParamHostPropSupportsCustomAnimation,
    kOfxParamHostPropMaxParameters, kOfxParamHostPropMaxPages, kOfxParamHostPropPageRowColumnCount,
    kOfxHostSupportHostPointer, // not an OFX property: HostSupport's fetchSuite needs it
    NULL
  };

  // --ofx11: the suites of OFX 1.1, but the interact suite, which VEGAS doesn't have (it has no
  // overlays and no custom interacts: README-hosts.txt), and with the message suite V2 (OFX 1.2),
  // which VEGAS Pro 12 lists; its persistent messages are refused, as in DaVinci Resolve and
  // Fusion 2026 (MyEffectInstance::setRefusePersistentMessages())
  static const SuiteVersion kOFX11Suites[] = {
    { kOfxPropertySuite, 1 }, { kOfxMemorySuite, 1 }, { kOfxImageEffectSuite, 1 },
    { kOfxParameterSuite, 1 }, { kOfxMessageSuite, 1 }, { kOfxMessageSuite, 2 }, { kOfxProgressSuite, 1 },
    { kOfxTimeLineSuite, 1 }, { kOfxMultiThreadSuite, 1 },
    { NULL, 0 }
  };

  /// keep only the host properties of keepList
  static void keepHostProperties(OFX::Host::Property::Set& properties, const char* const* keepList)
  {
    std::set<std::string> keep;
    for (int i = 0; keepList[i]; ++i) {
      keep.insert(keepList[i]);
    }
    std::vector<std::string> extra;
    const OFX::Host::Property::PropertyMap& props = properties.getProperties();
    for (OFX::Host::Property::PropertyMap::const_iterator it = props.begin(); it != props.end(); ++it) {
      if (!keep.count(it->first)) {
        extra.push_back(it->first);
      }
    }
    // a plug-in reading one of them gets kOfxStatErrUnknown, and paramDefine() of a StrChoice
    // or Bytes param (no kOfxParamHostPropSupportsStrChoice or kOfxHostSupportHostPropSupportsBytes)
    // returns kOfxStatErrUnknown, as for an unknown type
    for (size_t i = 0; i < extra.size(); ++i) {
      properties.removeProperty(extra[i]);
    }
  }

  void Host::restrictToOFX11()
  {
    _ofx11 = true;
    keepHostProperties(_properties, kOFX11HostProps);
    // VEGAS Pro 12's host description (README-hosts.txt): API version 1.1, no tiles, no
    // multi-resolution, no overlays, 8-bit and float depths only, a single clip PAR
    _properties.setIntProperty(kOfxPropAPIVersion, 1, 0);
    _properties.setIntProperty(kOfxPropAPIVersion, 1, 1);
    _properties.setIntProperty(kOfxImageEffectPropSupportsTiles, 0);
    _properties.setIntProperty(kOfxImageEffectPropSupportsMultiResolution, 0);
    _properties.setIntProperty(kOfxImageEffectPropSupportsOverlays, 0);
    _properties.setIntProperty(kOfxParamHostPropSupportsCustomInteract, 0);
    _properties.setIntProperty(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
    _properties.removeProperty(kOfxImageEffectPropSupportedPixelDepths);
    const OFX::Host::Property::PropSpec depths = { kOfxImageEffectPropSupportedPixelDepths, OFX::Host::Property::eString, 0, true, "" };
    _properties.createProperty(depths);
    static const char* const depths11[] = { kOfxBitDepthByte, kOfxBitDepthFloat };
    for (int i = 0; i < 2; ++i) {
      _properties.setStringProperty(kOfxImageEffectPropSupportedPixelDepths, depths11[i], i);
    }
    // kOfxParamPropDefaultCoordinateSystem is OFX 1.2: as --no-normalised-defaults
    _normalisedDefaults = false;
  }

  void Host::removeSequentialRender(OFX::Host::Property::Set& props) const
  {
    // --ofx11: kOfxImageEffectInstancePropSequentialRender is on the plug-in descriptor and
    // instance in OFX 1.1, but Resolve and Nuke lack it on the descriptor (README-hosts.txt): a
    // host without it, to test the Support library's fallbacks (the instance too)
    if (_ofx11) {
      props.removeProperty(kOfxImageEffectInstancePropSequentialRender);
    }
  }

  void Host::restrictToOFX14()
  {
    _ofx14 = true;
    keepHostProperties(_properties, kOFX14HostProps);
  }

  const void *Host::fetchSuite(const char *suiteName, int suiteVersion)
  {
    if (_ofx14 || _ofx11) {
      const SuiteVersion* suites = _ofx14 ? kOFX14Suites : kOFX11Suites;
      int i = 0;
      while (suites[i].name &&
             !(strcmp(suiteName, suites[i].name) == 0 && suiteVersion == suites[i].version)) {
        ++i;
      }
      if (!suites[i].name) {
        return NULL;
      }
    }
    return OFX::Host::ImageEffect::Host::fetchSuite(suiteName, suiteVersion);
  }

  OFX::Host::ImageEffect::Instance* Host::newInstance(void* /*clientData*/,
                                                      OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                                                      OFX::Host::ImageEffect::Descriptor& desc,
                                                      const std::string& context)
  {
    MyEffectInstance* instance = new MyEffectInstance(plugin, desc, context);
    removeSequentialRender(instance->getProps());
    if (_ofx11) {
      instance->setRefusePersistentMessages();
    }
    return instance;
  }
  
  /// Override this to create a descriptor, this makes the 'root' descriptor
  OFX::Host::ImageEffect::Descriptor *Host::makeDescriptor(OFX::Host::ImageEffect::ImageEffectPlugin* plugin)
  {
    OFX::Host::ImageEffect::Descriptor *desc = new OFX::Host::ImageEffect::Descriptor(plugin);
    removeSequentialRender(desc->getProps());
    return desc;
  }

  namespace {
    /// --no-normalised-defaults: a context descriptor (where the plug-in defines its params) whose
    /// params have no kOfxParamPropDefaultCoordinateSystem, as in a host without normalised defaults
    class NoNormalisedDefaultsDescriptor : public OFX::Host::ImageEffect::Descriptor
    {
    public:
      NoNormalisedDefaultsDescriptor(const OFX::Host::ImageEffect::Descriptor& rootContext,
                                     OFX::Host::ImageEffect::ImageEffectPlugin* plugin)
        : OFX::Host::ImageEffect::Descriptor(rootContext, plugin)
      {
      }

      virtual OFX::Host::Param::Descriptor* paramDefine(const char* paramType, const char* name)
      {
        OFX::Host::Param::Descriptor* desc = OFX::Host::ImageEffect::Descriptor::paramDefine(paramType, name);
        if (desc) {
          desc->getProperties().removeProperty(kOfxParamPropDefaultCoordinateSystem);
        }
        return desc;
      }
    };
  }

  /// used to construct a context description, rootContext is the main context
  OFX::Host::ImageEffect::Descriptor *Host::makeDescriptor(const OFX::Host::ImageEffect::Descriptor &rootContext, 
                                                           OFX::Host::ImageEffect::ImageEffectPlugin *plugin)
  {
    OFX::Host::ImageEffect::Descriptor *desc = _normalisedDefaults ?
      new OFX::Host::ImageEffect::Descriptor(rootContext, plugin) : new NoNormalisedDefaultsDescriptor(rootContext, plugin);
    removeSequentialRender(desc->getProps());
    return desc;
  }
  
  /// used to construct populate the cache
  OFX::Host::ImageEffect::Descriptor *Host::makeDescriptor(const std::string &bundlePath, 
                                                           OFX::Host::ImageEffect::ImageEffectPlugin *plugin)
  {
    OFX::Host::ImageEffect::Descriptor *desc = new OFX::Host::ImageEffect::Descriptor(bundlePath, plugin);
    removeSequentialRender(desc->getProps());
    return desc;
  }
  
  /// message
  OfxStatus Host::vmessage(const char* type,
                           const char* /*id*/,
                           const char* format,
                           va_list args)
  {
    bool isQuestion = false;
    const char *prefix = "Message : ";
    if (strcmp(type, kOfxMessageLog) == 0) {
      prefix = "Log : ";
    }
    else if(strcmp(type, kOfxMessageFatal) == 0 ||
            strcmp(type, kOfxMessageError) == 0) {
      prefix = "Error : ";
    }
    else if(strcmp(type, kOfxMessageQuestion) == 0)  {
      prefix = "Question : ";
      isQuestion = true;
    }
    
    // Just dump our message to stdout, should be done with a proper
    // UI in a full ap, and post a dialogue for yes/no questions.
    fputs(prefix, stdout);
    vprintf(format, args);
    printf("\n");

    if(isQuestion) {
      /// can't do this properly in our example, as we need to raise a dialogue to ask a question, so just return yes
      return kOfxStatReplyYes;      
    }
    else {
      return kOfxStatOK;
    }
  }

  OfxStatus Host::setPersistentMessage(const char* type,
                                       const char* id,
                                       const char* format,
                                       va_list args)
  {
    return vmessage(type, id, format, args);
  }

  OfxStatus Host::clearPersistentMessage()
  {
    return kOfxStatOK;
  }

#ifdef OFX_SUPPORTS_DIALOG
  // dialog
  /// @see OfxDialogSuite.requestDialog()
  OfxStatus Host::requestDialog(OfxImageEffectHandle instance, OfxPropertySetHandle inArgs, void *instanceData)
  {
    return kOfxStatFailed;
  }

  /// @see OfxDialogSuite.notifyRedrawPending()
  OfxStatus Host::notifyRedrawPending(OfxImageEffectHandle instance, OfxPropertySetHandle inArgs)
  {
    return kOfxStatReplyDefault;
  }
#endif
}
