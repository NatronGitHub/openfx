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

namespace MyHost {

  // All the param instances below store their value, initialised from the
  // default value of the descriptor (kOfxParamPropDefault). The demo host
  // doesn't animate parameters: get(time, ...) returns the stored value,
  // and set(time, ...) sets it.

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

  class MyIntegerInstance : public OFX::Host::Param::IntegerInstance {
    int _i;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyIntegerInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&);
    OfxStatus get(OfxTime time, int&);
    OfxStatus set(int);
    OfxStatus set(OfxTime time, int);
  };

  class MyDoubleInstance : public OFX::Host::Param::DoubleInstance {
    double _d;
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
  };

  class MyBooleanInstance : public OFX::Host::Param::BooleanInstance {
    bool _b;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyBooleanInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(bool&);
    OfxStatus get(OfxTime time, bool&);
    OfxStatus set(bool);
    OfxStatus set(OfxTime time, bool);
  };

  class MyChoiceInstance : public OFX::Host::Param::ChoiceInstance {
    int _i;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyChoiceInstance(MyEffectInstance* effect,  const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&);
    OfxStatus get(OfxTime time, int&);
    OfxStatus set(int);
    OfxStatus set(OfxTime time, int);
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
    double _r, _g, _b, _a;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyRGBAInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&,double&,double&);
    OfxStatus get(OfxTime time, double&,double&,double&,double&);
    OfxStatus set(double,double,double,double);
    OfxStatus set(OfxTime time, double,double,double,double);
  };


  class MyRGBInstance : public OFX::Host::Param::RGBInstance {
    double _r, _g, _b;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyRGBInstance(MyEffectInstance* effect,  const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&,double&);
    OfxStatus get(OfxTime time, double&,double&,double&);
    OfxStatus set(double,double,double);
    OfxStatus set(OfxTime time, double,double,double);
  };

  class MyDouble2DInstance : public OFX::Host::Param::Double2DInstance {
    double _x, _y;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyDouble2DInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&);
    OfxStatus get(OfxTime time,double&,double&);
    OfxStatus set(double,double);
    OfxStatus set(OfxTime time,double,double);
  };

  class MyInteger2DInstance : public OFX::Host::Param::Integer2DInstance {
    int _x, _y;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyInteger2DInstance(MyEffectInstance* effect,  const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&,int&);
    OfxStatus get(OfxTime time,int&,int&);
    OfxStatus set(int,int);
    OfxStatus set(OfxTime time,int,int);
  };

  class MyDouble3DInstance : public OFX::Host::Param::Double3DInstance {
    double _x, _y, _z;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyDouble3DInstance(MyEffectInstance* effect, const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(double&,double&,double&);
    OfxStatus get(OfxTime time,double&,double&,double&);
    OfxStatus set(double,double,double);
    OfxStatus set(OfxTime time,double,double,double);
  };

  class MyInteger3DInstance : public OFX::Host::Param::Integer3DInstance {
    int _x, _y, _z;
  protected:
    MyEffectInstance*   _effect;
    OFX::Host::Param::Descriptor& _descriptor;
  public:
    MyInteger3DInstance(MyEffectInstance* effect,  const std::string& name, OFX::Host::Param::Descriptor& descriptor);
    OfxStatus get(int&,int&,int&);
    OfxStatus get(OfxTime time,int&,int&,int&);
    OfxStatus set(int,int,int);
    OfxStatus set(OfxTime time,int,int,int);
  };

}

#endif // HOST_DEMO_PARAM_INSTANCE_H
