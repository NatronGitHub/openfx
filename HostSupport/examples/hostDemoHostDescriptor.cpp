/*
Software License :

Copyright (c) 2007, The Open Effects Association Ltd. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name The Open Effects Association Ltd, nor the names of its 
      contributors may be used to endorse or promote products derived from this
      software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

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
    , _keepLoaded(getenv("OFX_HOSTDEMO_KEEP_LOADED") != NULL && strcmp(getenv("OFX_HOSTDEMO_KEEP_LOADED"), "1") == 0)
  {
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
  static const struct { const char* name; int version; } kOFX14Suites[] = {
    { kOfxPropertySuite, 1 }, { kOfxMemorySuite, 1 }, { kOfxImageEffectSuite, 1 },
    { kOfxParameterSuite, 1 }, { kOfxParametricParameterSuite, 1 }, { kOfxInteractSuite, 1 },
    { kOfxMessageSuite, 1 }, { kOfxMessageSuite, 2 }, { kOfxProgressSuite, 1 }, { kOfxProgressSuite, 2 },
    { kOfxTimeLineSuite, 1 }, { kOfxMultiThreadSuite, 1 }, { kOfxOpenGLRenderSuite, 1 },
    { kOfxDialogSuite, 1 },
    { NULL, 0 }
  };

  void Host::restrictToOFX14()
  {
    _ofx14 = true;
    std::set<std::string> keep;
    for (int i = 0; kOFX14HostProps[i]; ++i) {
      keep.insert(kOFX14HostProps[i]);
    }
    std::vector<std::string> extra;
    const OFX::Host::Property::PropertyMap& props = _properties.getProperties();
    for (OFX::Host::Property::PropertyMap::const_iterator it = props.begin(); it != props.end(); ++it) {
      if (!keep.count(it->first)) {
        extra.push_back(it->first);
      }
    }
    // a plug-in reading one of them gets kOfxStatErrUnknown, and paramDefine() of a StrChoice
    // or Bytes param (no kOfxParamHostPropSupportsStrChoice or kOfxHostSupportHostPropSupportsBytes)
    // returns kOfxStatErrUnknown, as for an unknown type
    for (size_t i = 0; i < extra.size(); ++i) {
      _properties.removeProperty(extra[i]);
    }
  }

  const void *Host::fetchSuite(const char *suiteName, int suiteVersion)
  {
    if (_ofx14) {
      int i = 0;
      while (kOFX14Suites[i].name &&
             !(strcmp(suiteName, kOFX14Suites[i].name) == 0 && suiteVersion == kOFX14Suites[i].version)) {
        ++i;
      }
      if (!kOFX14Suites[i].name) {
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
    return new MyEffectInstance(plugin, desc, context);
  }
  
  /// Override this to create a descriptor, this makes the 'root' descriptor
  OFX::Host::ImageEffect::Descriptor *Host::makeDescriptor(OFX::Host::ImageEffect::ImageEffectPlugin* plugin)
  {
    OFX::Host::ImageEffect::Descriptor *desc = new OFX::Host::ImageEffect::Descriptor(plugin);
    return desc;
  }
  
  /// used to construct a context description, rootContext is the main context
  OFX::Host::ImageEffect::Descriptor *Host::makeDescriptor(const OFX::Host::ImageEffect::Descriptor &rootContext, 
                                                           OFX::Host::ImageEffect::ImageEffectPlugin *plugin)
  {
    OFX::Host::ImageEffect::Descriptor *desc = new OFX::Host::ImageEffect::Descriptor(rootContext, plugin);
    return desc;
  }
  
  /// used to construct populate the cache
  OFX::Host::ImageEffect::Descriptor *Host::makeDescriptor(const std::string &bundlePath, 
                                                           OFX::Host::ImageEffect::ImageEffectPlugin *plugin)
  {
    OFX::Host::ImageEffect::Descriptor *desc = new OFX::Host::ImageEffect::Descriptor(bundlePath, plugin);
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
      /// cant do this properly inour example, as we need to raise a dialogue to ask a question, so just return yes
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
