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
#ifndef HOST_DEMO_PARAM_INSTANCE_H
#define HOST_DEMO_PARAM_INSTANCE_H

#include <map>
#include <vector>

namespace MyHost {

  // All the param instances below store their value, initialised from the
  // default value of the descriptor (kOfxParamPropDefault). The numeric ones (MyCurve) may have
  // keys; the string, custom, string choice and bytes ones don't animate: get(time, ...) returns
  // the stored value, and set(time, ...) sets it.

  class MyPushbuttonInstance : public OFX::Host::Param::PushbuttonInstance {
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyPushbuttonInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
  };

  class MyStringInstance : public OFX::Host::Param::StringInstance {
    std::string _s;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyStringInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(std::string&);
    OfxStatus get(OfxTime time, std::string&);
    OfxStatus set(const char*);
    OfxStatus set(OfxTime time, const char*);
  };

  class MyCustomInstance : public OFX::Host::Param::CustomInstance {
    std::string _s;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyCustomInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(std::string&);
    OfxStatus get(OfxTime time, std::string&);
    OfxStatus set(const char*);
    OfxStatus set(OfxTime time, const char*);
  };

  class MyBytesInstance : public OFX::Host::Param::BytesInstance {
    std::vector<unsigned char> _value;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyBytesInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(std::vector<unsigned char>&);
    OfxStatus get(OfxTime time, std::vector<unsigned char>&);
    OfxStatus set(const unsigned char* data, size_t length);
    OfxStatus set(OfxTime time, const unsigned char* data, size_t length);
  };

  /// The value of an animatable parameter (integer, double, boolean, choice, colour, 2D and 3D):
  /// a constant value, or keys, set with paramSetValueAtTime (or hostDemo's -s name@t=value).
  /// Between keys, values are interpolated linearly (eLinear), linearly then rounded (integers,
  /// eLinearRounded), or held until the next key (booleans and choices, eConstant); before the
  /// first key and after the last one, they are those of the first and last keys.
  /// paramSetValue on a parameter with keys sets the key at the current time (the time of the
  /// action, see MyEffectInstance::timeLineGetTime()). A parameter that doesn't animate
  /// (kOfxParamPropAnimates=0) has no keys: paramSetValueAtTime sets its value.
  class MyCurve {
  public:
    enum InterpolationEnum { eLinear, eLinearRounded, eConstant };
    typedef std::map<OfxTime, std::vector<double> > Keys;
    MyCurve(MyEffectInstance* effect, const OFX::Host::Param::Descriptor& descriptor, int dimension,
            InterpolationEnum interpolation, const double* defaultValue);
    void getCurrent(double* v) const;
    void get(OfxTime time, double* v) const;
    void set(const double* v);
    void set(OfxTime time, const double* v);
    void derive(OfxTime time, double* v) const;
    void integrate(OfxTime time1, OfxTime time2, double* v) const;
    OfxStatus getNumKeys(unsigned int& nKeys) const;
    OfxStatus getKeyTime(int nth, OfxTime& time) const;
    OfxStatus getKeyIndex(OfxTime time, int direction, int& index) const;
    OfxStatus deleteKey(OfxTime time);
    OfxStatus deleteAllKeys();
    const Keys& getKeys() const { return _keys; }
    int getDimension() const { return _dimension; }
    bool getAnimates() const { return _animates; }
  private:
    Keys::const_iterator findKey(OfxTime time) const;
    OfxTime currentTime() const;
    MyEffectInstance* _effect;
    int _dimension;
    InterpolationEnum _interpolation;
    bool _animates;
    std::vector<double> _value; // without keys
    Keys _keys;
  };

  // the KeyframeParam functions of the animatable parameters, and access to their curve
#define MYHOST_CURVE_METHODS \
    OfxStatus getNumKeys(unsigned int& nKeys) const { return _curve.getNumKeys(nKeys); } \
    OfxStatus getKeyTime(int nth, OfxTime& time) const { return _curve.getKeyTime(nth, time); } \
    OfxStatus getKeyIndex(OfxTime time, int direction, int& index) const { return _curve.getKeyIndex(time, direction, index); } \
    OfxStatus deleteKey(OfxTime time) { return _curve.deleteKey(time); } \
    OfxStatus deleteAllKeys() { return _curve.deleteAllKeys(); } \
    const MyCurve& getCurve() const { return _curve; }

  class MyIntegerInstance : public OFX::Host::Param::IntegerInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyIntegerInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&);
    OfxStatus get(OfxTime time, int&);
    OfxStatus set(int);
    OfxStatus set(OfxTime time, int);
    MYHOST_CURVE_METHODS
  };

  class MyDoubleInstance : public OFX::Host::Param::DoubleInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyDoubleInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&);
    OfxStatus get(OfxTime time, double&);
    OfxStatus set(double);
    OfxStatus set(OfxTime time, double);
    OfxStatus derive(OfxTime time, double&);
    OfxStatus integrate(OfxTime time1, OfxTime time2, double&);
    MYHOST_CURVE_METHODS
  };

  class MyBooleanInstance : public OFX::Host::Param::BooleanInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyBooleanInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(bool&);
    OfxStatus get(OfxTime time, bool&);
    OfxStatus set(bool);
    OfxStatus set(OfxTime time, bool);
    MYHOST_CURVE_METHODS
  };

  class MyChoiceInstance : public OFX::Host::Param::ChoiceInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyChoiceInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&);
    OfxStatus get(OfxTime time, int&);
    OfxStatus set(int);
    OfxStatus set(OfxTime time, int);
    MYHOST_CURVE_METHODS
  };

  class MyStrChoiceInstance : public OFX::Host::Param::StrChoiceInstance {
    std::string _choice;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyStrChoiceInstance(MyEffectInstance* effect,  const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(std::string&);
    OfxStatus get(OfxTime time, std::string&);
    OfxStatus set(const char*);
    OfxStatus set(OfxTime time, const char*);
  };

  class MyRGBAInstance : public OFX::Host::Param::RGBAInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyRGBAInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&,double&,double&);
    OfxStatus get(OfxTime time, double&,double&,double&,double&);
    OfxStatus set(double,double,double,double);
    OfxStatus set(OfxTime time, double,double,double,double);
    OfxStatus derive(OfxTime time, double&,double&,double&,double&);
    OfxStatus integrate(OfxTime time1, OfxTime time2, double&,double&,double&,double&);
    MYHOST_CURVE_METHODS
  };

  class MyRGBInstance : public OFX::Host::Param::RGBInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyRGBInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&,double&);
    OfxStatus get(OfxTime time, double&,double&,double&);
    OfxStatus set(double,double,double);
    OfxStatus set(OfxTime time, double,double,double);
    OfxStatus derive(OfxTime time, double&,double&,double&);
    OfxStatus integrate(OfxTime time1, OfxTime time2, double&,double&,double&);
    MYHOST_CURVE_METHODS
  };

  class MyDouble2DInstance : public OFX::Host::Param::Double2DInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyDouble2DInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&);
    OfxStatus get(OfxTime time, double&,double&);
    OfxStatus set(double,double);
    OfxStatus set(OfxTime time, double,double);
    OfxStatus derive(OfxTime time, double&,double&);
    OfxStatus integrate(OfxTime time1, OfxTime time2, double&,double&);
    MYHOST_CURVE_METHODS
  };

  class MyInteger2DInstance : public OFX::Host::Param::Integer2DInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyInteger2DInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&,int&);
    OfxStatus get(OfxTime time, int&,int&);
    OfxStatus set(int,int);
    OfxStatus set(OfxTime time, int,int);
    MYHOST_CURVE_METHODS
  };

  class MyDouble3DInstance : public OFX::Host::Param::Double3DInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyDouble3DInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&,double&);
    OfxStatus get(OfxTime time, double&,double&,double&);
    OfxStatus set(double,double,double);
    OfxStatus set(OfxTime time, double,double,double);
    OfxStatus derive(OfxTime time, double&,double&,double&);
    OfxStatus integrate(OfxTime time1, OfxTime time2, double&,double&,double&);
    MYHOST_CURVE_METHODS
  };

  class MyInteger3DInstance : public OFX::Host::Param::Integer3DInstance {
    MyCurve _curve;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyInteger3DInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&,int&,int&);
    OfxStatus get(OfxTime time, int&,int&,int&);
    OfxStatus set(int,int,int);
    OfxStatus set(OfxTime time, int,int,int);
    MYHOST_CURVE_METHODS
  };

}

#endif // HOST_DEMO_PARAM_INSTANCE_H
