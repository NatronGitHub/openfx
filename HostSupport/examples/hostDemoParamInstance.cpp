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
#include <fstream>

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

namespace MyHost {

  // All the param instances store their value, initialised from the default
  // value of the descriptor (kOfxParamPropDefault). Parameters are not animated:
  // get(time, ...) returns the stored value and set(time, ...) sets it.

  //
  // MyStringInstance
  //

  MyStringInstance::MyStringInstance(MyEffectInstance* effect,
                                     const std::string& /*name*/,
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::StringInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    _s = _descriptor.getProperties().getStringProperty(kOfxParamPropDefault);
  }

  OfxStatus MyStringInstance::get(std::string& s)
  {
    s = _s;
    return kOfxStatOK;
  }

  OfxStatus MyStringInstance::get(OfxTime, std::string& s)
  {
    return get(s);
  }

  OfxStatus MyStringInstance::set(const char* s)
  {
    _s = s ? s : "";
    return kOfxStatOK;
  }

  OfxStatus MyStringInstance::set(OfxTime, const char* s)
  {
    return set(s);
  }

  //
  // MyCustomInstance
  //

  MyCustomInstance::MyCustomInstance(MyEffectInstance* effect,
                                     const std::string& /*name*/,
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::CustomInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    _s = _descriptor.getProperties().getStringProperty(kOfxParamPropDefault);
  }

  OfxStatus MyCustomInstance::get(std::string& s)
  {
    s = _s;
    return kOfxStatOK;
  }

  OfxStatus MyCustomInstance::get(OfxTime, std::string& s)
  {
    return get(s);
  }

  OfxStatus MyCustomInstance::set(const char* s)
  {
    _s = s ? s : "";
    return kOfxStatOK;
  }

  OfxStatus MyCustomInstance::set(OfxTime, const char* s)
  {
    return set(s);
  }

  //
  // MyBytesInstance (OFX 1.5 kOfxParamTypeBytes)
  //

  MyBytesInstance::MyBytesInstance(MyEffectInstance* effect,
                                   const std::string& /*name*/,
                                   OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::BytesInstance(descriptor), _value(getDefault()), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyBytesInstance::get(std::vector<unsigned char>& v)
  {
    v = _value;
    return kOfxStatOK;
  }

  OfxStatus MyBytesInstance::get(OfxTime, std::vector<unsigned char>& v)
  {
    v = _value;
    return kOfxStatOK;
  }

  OfxStatus MyBytesInstance::set(const unsigned char* data, size_t length)
  {
    if (data && length > 0) {
      _value.assign(data, data + length); // copy: data is only valid during the call
    } else {
      _value.clear();
    }
    return kOfxStatOK;
  }

  OfxStatus MyBytesInstance::set(OfxTime, const unsigned char* data, size_t length)
  {
    return set(data, length);
  }

  //
  // MyIntegerInstance
  //

  MyIntegerInstance::MyIntegerInstance(MyEffectInstance* effect, 
                                       const std::string& /*name*/, 
                                       OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::IntegerInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    _i = _descriptor.getProperties().getIntProperty(kOfxParamPropDefault);
  }

  OfxStatus MyIntegerInstance::get(int& i)
  {
    i = _i;
    return kOfxStatOK;
  }

  OfxStatus MyIntegerInstance::get(OfxTime, int& i)
  {
    return get(i);
  }

  OfxStatus MyIntegerInstance::set(int i)
  {
    _i = i;
    return kOfxStatOK;
  }

  OfxStatus MyIntegerInstance::set(OfxTime, int i)
  {
    return set(i);
  }

  //
  // MyDoubleInstance
  //

  // Spatial double params may give their default in normalised coordinates
  // (kOfxParamPropDefaultCoordinateSystem, OFX 1.2): convert it to canonical
  // coordinates, using the project size, plus the project offset for positions.
  static void
  defaultToCanonical(const OFX::Host::Param::Descriptor& descriptor,
                     const MyEffectInstance* effect,
                     double* v, int n)
  {
    if (!effect || descriptor.getDefaultCoordinateSystem() != kOfxParamCoordinatesNormalised) {
      return;
    }
    const std::string& type = descriptor.getDoubleType();
    double w, h, ox, oy;
    effect->getProjectSize(w, h);
    effect->getProjectOffset(ox, oy);
    if (n == 1) {
      if (type == kOfxParamDoubleTypeX) {
        v[0] *= w;
      } else if (type == kOfxParamDoubleTypeXAbsolute) {
        v[0] = v[0] * w + ox;
      } else if (type == kOfxParamDoubleTypeY) {
        v[0] *= h;
      } else if (type == kOfxParamDoubleTypeYAbsolute) {
        v[0] = v[0] * h + oy;
      }
    } else if (n == 2) {
      if (type == kOfxParamDoubleTypeXY) {
        v[0] *= w;
        v[1] *= h;
      } else if (type == kOfxParamDoubleTypeXYAbsolute) {
        v[0] = v[0] * w + ox;
        v[1] = v[1] * h + oy;
      }
    }
  }

  MyDoubleInstance::MyDoubleInstance(MyEffectInstance* effect, 
                                     const std::string& /*name*/, 
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::DoubleInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    _d = _descriptor.getProperties().getDoubleProperty(kOfxParamPropDefault);
    defaultToCanonical(_descriptor, _effect, &_d, 1);
  }

  OfxStatus MyDoubleInstance::get(double& d)
  {
    d = _d;
    return kOfxStatOK;
  }

  OfxStatus MyDoubleInstance::get(OfxTime, double& d)
  {
    return get(d);
  }

  OfxStatus MyDoubleInstance::set(double d)
  {
    _d = d;
    return kOfxStatOK;
  }

  OfxStatus MyDoubleInstance::set(OfxTime, double d)
  {
    return set(d);
  }

  // parameters don't animate in hostDemo: the derivative is 0, and the integral is
  // value * (time2 - time1)
  OfxStatus MyDoubleInstance::derive(OfxTime, double& d)
  {
    d = 0.;
    return kOfxStatOK;
  }

  OfxStatus MyDoubleInstance::integrate(OfxTime time1, OfxTime time2, double& d)
  {
    d = _d * (time2 - time1);
    return kOfxStatOK;
  }

  //
  // MyBooleanInstance
  //

  MyBooleanInstance::MyBooleanInstance(MyEffectInstance* effect, 
                                       const std::string& /*name*/, 
                                       OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::BooleanInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    _b = _descriptor.getProperties().getIntProperty(kOfxParamPropDefault) != 0;
  }

  OfxStatus MyBooleanInstance::get(bool& b)
  {
    b = _b;
    return kOfxStatOK;
  }

  OfxStatus MyBooleanInstance::get(OfxTime, bool& b)
  {
    return get(b);
  }

  OfxStatus MyBooleanInstance::set(bool b)
  {
    _b = b;
    return kOfxStatOK;
  }

  OfxStatus MyBooleanInstance::set(OfxTime, bool b)
  {
    return set(b);
  }

  //
  // MyStrChoiceInstance
  //

  MyStrChoiceInstance::MyStrChoiceInstance(MyEffectInstance* effect,
                                     const std::string& /*name*/,
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::StrChoiceInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
      _choice = getDefaultEnum();
  }

  OfxStatus MyStrChoiceInstance::get(std::string& choice)
  {
    choice = _choice;
    return kOfxStatOK;
  }

  OfxStatus MyStrChoiceInstance::get(OfxTime, std::string& choice)
  {
    return get(choice);
  }

  OfxStatus MyStrChoiceInstance::set(const char* choice)
  {
    _choice = choice ? choice : "";
    return kOfxStatOK;
  }

  OfxStatus MyStrChoiceInstance::set(OfxTime, const char* choice)
  {
    return set(choice);
  }

  //
  // MyChoiceInstance
  //

  MyChoiceInstance::MyChoiceInstance(MyEffectInstance* effect, 
                                     const std::string& /*name*/, 
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::ChoiceInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    _i = _descriptor.getProperties().getIntProperty(kOfxParamPropDefault);
  }

  OfxStatus MyChoiceInstance::get(int& i)
  {
    i = _i;
    return kOfxStatOK;
  }

  OfxStatus MyChoiceInstance::get(OfxTime, int& i)
  {
    return get(i);
  }

  OfxStatus MyChoiceInstance::set(int i)
  {
    if ( i < 0 || i >= (int)getProperties().getDimension(kOfxParamPropChoiceOption) ) {
      return kOfxStatErrValue;
    }
    _i = i;
    return kOfxStatOK;
  }

  OfxStatus MyChoiceInstance::set(OfxTime, int i)
  {
    return set(i);
  }

  //
  // MyRGBAInstance
  //

  MyRGBAInstance::MyRGBAInstance(MyEffectInstance* effect, 
                                 const std::string& /*name*/, 
                                 OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::RGBAInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    const OFX::Host::Property::Set& props = _descriptor.getProperties();
    _r = props.getDoubleProperty(kOfxParamPropDefault, 0);
    _g = props.getDoubleProperty(kOfxParamPropDefault, 1);
    _b = props.getDoubleProperty(kOfxParamPropDefault, 2);
    _a = props.getDoubleProperty(kOfxParamPropDefault, 3);
  }

  OfxStatus MyRGBAInstance::get(double& r, double& g, double& b, double& a)
  {
    r = _r;
    g = _g;
    b = _b;
    a = _a;
    return kOfxStatOK;
  }

  OfxStatus MyRGBAInstance::get(OfxTime, double& r, double& g, double& b, double& a)
  {
    return get(r, g, b, a);
  }

  OfxStatus MyRGBAInstance::set(double r, double g, double b, double a)
  {
    _r = r;
    _g = g;
    _b = b;
    _a = a;
    return kOfxStatOK;
  }

  OfxStatus MyRGBAInstance::set(OfxTime, double r, double g, double b, double a)
  {
    return set(r, g, b, a);
  }

  //
  // MyRGBInstance
  //

  MyRGBInstance::MyRGBInstance(MyEffectInstance* effect, 
                               const std::string& /*name*/, 
                               OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::RGBInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    const OFX::Host::Property::Set& props = _descriptor.getProperties();
    _r = props.getDoubleProperty(kOfxParamPropDefault, 0);
    _g = props.getDoubleProperty(kOfxParamPropDefault, 1);
    _b = props.getDoubleProperty(kOfxParamPropDefault, 2);
  }

  OfxStatus MyRGBInstance::get(double& r, double& g, double& b)
  {
    r = _r;
    g = _g;
    b = _b;
    return kOfxStatOK;
  }

  OfxStatus MyRGBInstance::get(OfxTime, double& r, double& g, double& b)
  {
    return get(r, g, b);
  }

  OfxStatus MyRGBInstance::set(double r, double g, double b)
  {
    _r = r;
    _g = g;
    _b = b;
    return kOfxStatOK;
  }

  OfxStatus MyRGBInstance::set(OfxTime, double r, double g, double b)
  {
    return set(r, g, b);
  }

  //
  // MyDouble2DInstance
  //

  MyDouble2DInstance::MyDouble2DInstance(MyEffectInstance* effect, 
                                         const std::string& /*name*/, 
                                         OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Double2DInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    const OFX::Host::Property::Set& props = _descriptor.getProperties();
    double v[2] = { props.getDoubleProperty(kOfxParamPropDefault, 0),
                    props.getDoubleProperty(kOfxParamPropDefault, 1) };
    defaultToCanonical(_descriptor, _effect, v, 2);
    _x = v[0];
    _y = v[1];
  }

  OfxStatus MyDouble2DInstance::get(double& x, double& y)
  {
    x = _x;
    y = _y;
    return kOfxStatOK;
  }

  OfxStatus MyDouble2DInstance::get(OfxTime, double& x, double& y)
  {
    return get(x, y);
  }

  OfxStatus MyDouble2DInstance::set(double x, double y)
  {
    _x = x;
    _y = y;
    return kOfxStatOK;
  }

  OfxStatus MyDouble2DInstance::set(OfxTime, double x, double y)
  {
    return set(x, y);
  }

  //
  // MyInteger2DInstance
  //

  MyInteger2DInstance::MyInteger2DInstance(MyEffectInstance* effect, 
                                           const std::string& /*name*/, 
                                           OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Integer2DInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    const OFX::Host::Property::Set& props = _descriptor.getProperties();
    _x = props.getIntProperty(kOfxParamPropDefault, 0);
    _y = props.getIntProperty(kOfxParamPropDefault, 1);
  }

  OfxStatus MyInteger2DInstance::get(int& x, int& y)
  {
    x = _x;
    y = _y;
    return kOfxStatOK;
  }

  OfxStatus MyInteger2DInstance::get(OfxTime, int& x, int& y)
  {
    return get(x, y);
  }

  OfxStatus MyInteger2DInstance::set(int x, int y)
  {
    _x = x;
    _y = y;
    return kOfxStatOK;
  }

  OfxStatus MyInteger2DInstance::set(OfxTime, int x, int y)
  {
    return set(x, y);
  }

  //
  // MyDouble3DInstance
  //

  MyDouble3DInstance::MyDouble3DInstance(MyEffectInstance* effect, 
                                         const std::string& /*name*/, 
                                         OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Double3DInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    const OFX::Host::Property::Set& props = _descriptor.getProperties();
    _x = props.getDoubleProperty(kOfxParamPropDefault, 0);
    _y = props.getDoubleProperty(kOfxParamPropDefault, 1);
    _z = props.getDoubleProperty(kOfxParamPropDefault, 2);
  }

  OfxStatus MyDouble3DInstance::get(double& x, double& y, double& z)
  {
    x = _x;
    y = _y;
    z = _z;
    return kOfxStatOK;
  }

  OfxStatus MyDouble3DInstance::get(OfxTime, double& x, double& y, double& z)
  {
    return get(x, y, z);
  }

  OfxStatus MyDouble3DInstance::set(double x, double y, double z)
  {
    _x = x;
    _y = y;
    _z = z;
    return kOfxStatOK;
  }

  OfxStatus MyDouble3DInstance::set(OfxTime, double x, double y, double z)
  {
    return set(x, y, z);
  }

  //
  // MyInteger3DInstance
  //

  MyInteger3DInstance::MyInteger3DInstance(MyEffectInstance* effect, 
                                           const std::string& /*name*/, 
                                           OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Integer3DInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
    const OFX::Host::Property::Set& props = _descriptor.getProperties();
    _x = props.getIntProperty(kOfxParamPropDefault, 0);
    _y = props.getIntProperty(kOfxParamPropDefault, 1);
    _z = props.getIntProperty(kOfxParamPropDefault, 2);
  }

  OfxStatus MyInteger3DInstance::get(int& x, int& y, int& z)
  {
    x = _x;
    y = _y;
    z = _z;
    return kOfxStatOK;
  }

  OfxStatus MyInteger3DInstance::get(OfxTime, int& x, int& y, int& z)
  {
    return get(x, y, z);
  }

  OfxStatus MyInteger3DInstance::set(int x, int y, int z)
  {
    _x = x;
    _y = y;
    _z = z;
    return kOfxStatOK;
  }

  OfxStatus MyInteger3DInstance::set(OfxTime, int x, int y, int z)
  {
    return set(x, y, z);
  }

  //
  // MyPushbuttonInstance
  //

  MyPushbuttonInstance::MyPushbuttonInstance(MyEffectInstance* effect, 
                                             const std::string& /*name*/, 
                                             OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::PushbuttonInstance(descriptor), _effect(effect), _descriptor(descriptor)
  {
  }

}
