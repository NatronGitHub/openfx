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

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxPixels.h"

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
#include "hostDemoParamInstance.h"

// my host support code
namespace MyHost {

  MyEffectInstance::MyEffectInstance(OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                                     OFX::Host::ImageEffect::Descriptor& desc,
                                     const std::string& context) 
                                     : OFX::Host::ImageEffect::Instance(plugin,desc,context,false)
                                     , _rendering(false)
                                     , _defaultDepth(kOfxBitDepthByte)
                                     , _timeLineTime(0.)
  {
    _renderScale.x = _renderScale.y = 1.;
    _renderWindow.x1 = _renderWindow.y1 = _renderWindow.x2 = _renderWindow.y2 = 0;
  }

  void MyEffectInstance::setRenderState(const OfxPointD& renderScale, const OfxRectI& renderWindow,
                                        const std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD>& rois)
  {
    _rendering = true;
    _renderScale = renderScale;
    _renderWindow = renderWindow;
    _rois = rois;
  }

  void MyEffectInstance::clearRenderState()
  {
    _rendering = false;
    _rois.clear();
  }

  bool MyEffectInstance::getRegionOfInterest(const OFX::Host::ImageEffect::ClipInstance* clip, OfxRectD& roi) const
  {
    if (!_rendering) {
      return false;
    }
    std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD>::const_iterator it =
      _rois.find(const_cast<OFX::Host::ImageEffect::ClipInstance*>(clip));
    if (it == _rois.end()) {
      return false;
    }
    roi = it->second;
    return true;
  }

  OfxRectI MyEffectInstance::getProjectPixelRect(const OfxPointD& renderScale) const
  {
    double x, y, w, h;
    getProjectOffset(x, y);
    getProjectSize(w, h);
    OfxRectD r;
    r.x1 = x;
    r.y1 = y;
    r.x2 = x + w;
    r.y2 = y + h;
    return canonicalToPixelEnclosing(r, renderScale, getProjectPixelAspectRatio());
  }

  void MyEffectInstance::setDefaultClipPreferences()
  {
    OFX::Host::ImageEffect::Instance::setDefaultClipPreferences();
    // HostSupport maps the output to float when no input is connected; hostDemo uses
    // the -d depth, as for the inputs
    for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = _clips.begin(); it != _clips.end(); ++it) {
      if (!it->second->isOutput() && it->second->getConnected()) {
        return;
      }
    }
    OFX::Host::ImageEffect::ClipInstance* output = getClip(kOfxImageEffectOutputClipName);
    if (output && isChromaticComponent(output->getComponents())) {
      output->setPixelDepth(bestSupportedDepth(_defaultDepth));
    }
  }

  // class member function implementation

  // get a new clip instance
  OFX::Host::ImageEffect::ClipInstance* MyEffectInstance::newClipInstance(OFX::Host::ImageEffect::Instance* plugin,
                                                                          OFX::Host::ImageEffect::ClipDescriptor* descriptor,
                                                                          int index)
  {
    return new MyClipInstance(this,descriptor);
  }

    
  /// get default output fielding. This is passed into the clip prefs action
  /// and  might be mapped (if the host allows such a thing)
  const std::string &MyEffectInstance::getDefaultOutputFielding() const
  {
    /// our clip is pretending to be progressive PAL SD, so return kOfxImageFieldNone
    static const std::string v(kOfxImageFieldNone);
    return v;    
  }

  OfxStatus MyEffectInstance::recordMessage(const char* kind, const char* type, const char* id,
                                            const char* format, va_list args)
  {
    // vsnprintf is C99 (and C++11), but every compiler hostDemo is built with has it
    std::vector<char> buf(65536);
    const int n = format ? vsnprintf(&buf[0], buf.size(), format, args) : 0;
    std::string text(n > 0 ? &buf[0] : "");
    if (n < 0 || n >= (int)buf.size()) {
      text += "...";
    }
    printf("%s %s %s", type ? type : "", id ? id : "", text.c_str());
    MessageRecord m;
    m.kind = kind;
    m.type = type ? type : "";
    m.id = id ? id : "";
    m.text = text;
    _messages.push_back(m);
    return kOfxStatOK;
  }

  // vmessage
  OfxStatus MyEffectInstance::vmessage(const char* type,
                                       const char* id,
                                       const char* format,
                                       va_list args)
  {
    return recordMessage("message", type, id, format, args);
  }

  OfxStatus MyEffectInstance::setPersistentMessage(const char* type,
                                                   const char* id,
                                                   const char* format,
                                                   va_list args)
  {
    return recordMessage("persistentMessage", type, id, format, args);
  }

  OfxStatus MyEffectInstance::clearPersistentMessage()
  {
    MessageRecord m;
    m.kind = "clearPersistentMessage";
    _messages.push_back(m);
    return kOfxStatOK;
  }

  // get the project size in CANONICAL pixels, so PAL SD return 768, 576
  void MyEffectInstance::getProjectSize(double& xSize, double& ySize) const
  {
    xSize = 768; 
    ySize = 576;
  }

  // get the project offset in CANONICAL pixels, we are at 0,0
  void MyEffectInstance::getProjectOffset(double& xOffset, double& yOffset) const
  {
    xOffset = 0;
    yOffset = 0;
  }

  // get the project extent in CANONICAL pixels, so PAL SD return 768, 576
  void MyEffectInstance::getProjectExtent(double& xSize, double& ySize) const
  {
    xSize = 768; 
    ySize = 576;
  }

  // get the PAR, SD PAL is 1.0666
  double MyEffectInstance::getProjectPixelAspectRatio() const
  {
    return double(768)/double(720);
  }

  // we are only 25 frames
  double MyEffectInstance::getEffectDuration() const
  {
    return 25.0;
  }

  // get frame rate, so progressive PAL SD return 25
  double MyEffectInstance::getFrameRate() const
  {
    return 25.0;
  }

  /// This is called whenever a param is changed by the plugin so that
  /// the recursive instanceChangedAction will be fed the correct frame 
  double MyEffectInstance::getFrameRecursive() const
  {
    return _timeLineTime; // (the time of the current action)
  }

  /// This is called whenever a param is changed by the plugin so that
  /// the recursive instanceChangedAction will be fed the correct
  /// renderScale
  void MyEffectInstance::getRenderScaleRecursive(double &x, double &y) const
  {
    x = _renderScale.x;
    y = _renderScale.y;
  }

  // make a parameter instance
  OFX::Host::Param::Instance* MyEffectInstance::newParam(const std::string& name, OFX::Host::Param::Descriptor& descriptor)
  {
    if(descriptor.getType()==kOfxParamTypeString)
      return new MyStringInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeBytes)
      return new MyBytesInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeInteger)
      return new MyIntegerInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeDouble)
      return new MyDoubleInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeBoolean)
      return new MyBooleanInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeChoice)
      return new MyChoiceInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeStrChoice)
      return new MyStrChoiceInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeRGBA)
      return new MyRGBAInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeRGB)
      return new MyRGBInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeDouble2D)
      return new MyDouble2DInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeInteger2D)
      return new MyInteger2DInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeDouble3D)
      return new MyDouble3DInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeInteger3D)
      return new MyInteger3DInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeCustom)
      return new MyCustomInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypePushButton)
      return new MyPushbuttonInstance(this,name,descriptor);
    else if(descriptor.getType()==kOfxParamTypeGroup)
      return new OFX::Host::Param::GroupInstance(descriptor,this);
    else if(descriptor.getType()==kOfxParamTypePage)
      return new OFX::Host::Param::PageInstance(descriptor,this);
    else
      return 0;
  }

  // undo/redo blocks: hostDemo has no undo, so they only succeed ("kOfxStatOK - all was OK")
  OfxStatus MyEffectInstance::editBegin(const std::string& /*name*/)
  {
    return kOfxStatOK;
  }

  OfxStatus MyEffectInstance::editEnd(){
    return kOfxStatOK;
  }

  /// Start doing progress. 
  void  MyEffectInstance::progressStart(const std::string &message, const std::string &messageid)
  {
    ProgressRecord p;
    p.message = message;
    p.id = messageid;
    p.updates = 0;
    p.lastValue = 0.;
    p.ended = false;
    _progress.push_back(p);
  }
  
  /// finish yer progress
  void  MyEffectInstance::progressEnd()
  {
    if (!_progress.empty()) {
      _progress.back().ended = true;
    }
  }
  
  /// set the progress to some level of completion, returns
  /// false if you should abandon processing, true to continue
  bool  MyEffectInstance::progressUpdate(double t)
  {
    if (!_progress.empty()) {
      ++_progress.back().updates;
      _progress.back().lastValue = t;
    }
    return true;
  }


  /// get the current time on the timeline. This is not necessarily the same
  /// time as being passed to an action (eg render)
  // the timeline is at the time of the current action (hostDemo sets it before each one), which is
  // also the time of paramSetValue on a parameter with keys
  double  MyEffectInstance::timeLineGetTime()
  {
    return _timeLineTime;
  }
  
  /// set the timeline to a specific time
  void  MyEffectInstance::timeLineGotoTime(double t)
  {
    _timeLineTime = t;
  }
  
  /// get the first and last times available on the effect's timeline
  void  MyEffectInstance::timeLineGetBounds(double &t1, double &t2)
  {
    t1 = 0;
    t2 = 25;
  }

#ifdef OFX_EXTENSIONS_NUKE
  /// Returns the number of views
  OfxStatus MyEffectInstance::getViewCount(int *nViews) const {
    *nViews = 1;
    return kOfxStatOK;
  }
          
  /// Returns the view textual representation
  /// The string is owned by the host and must be valid throughout the calling action
  OfxStatus MyEffectInstance::getViewName(int viewIndex, const char** name) const {
    *name = "main";
    return kOfxStatOK;
  }
#endif

}
