// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <iterator>

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
#include "ofxhUtilities.h" // IsNaN, IsInfinite

// my host
#include "hostDemoHostDescriptor.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
#include "hostDemoParamInstance.h"

namespace MyHost {

  // All the param instances belong to the effect's parameter set (so that HostSupport sends
  // kOfxActionInstanceChanged, kOfxChangePluginEdited, when the plug-in sets a value), and store
  // their value, initialised from the default
  // value of the descriptor (kOfxParamPropDefault). The numeric ones may have keys (MyCurve);
  // the string, custom, string choice and bytes ones don't animate: get(time, ...) returns the
  // stored value and set(time, ...) sets it.

  //
  // MyStringInstance
  //

  MyStringInstance::MyStringInstance(MyEffectInstance* effect,
                                     const std::string& /*name*/,
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::StringInstance(descriptor, effect), _effect(effect), _descriptor(descriptor)
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
    : OFX::Host::Param::CustomInstance(descriptor, effect), _effect(effect), _descriptor(descriptor)
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
    : OFX::Host::Param::BytesInstance(descriptor, effect), _value(getDefault()), _effect(effect), _descriptor(descriptor)
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
  // MyCurve
  //

  // two key times closer than this are the same key ("some small delta", paramGetKeyIndex)
  static const double kKeyTimeEpsilon = 1e-4;

  MyCurve::MyCurve(MyEffectInstance* effect, const OFX::Host::Param::Descriptor& descriptor, int dimension,
                   InterpolationEnum interpolation, const double* defaultValue)
    : _effect(effect)
    , _dimension(dimension)
    , _interpolation(interpolation)
    , _animates(descriptor.getProperties().getIntProperty(kOfxParamPropAnimates) != 0)
    , _value(defaultValue, defaultValue + dimension)
  {
  }

  OfxTime MyCurve::currentTime() const
  {
    return _effect ? _effect->timeLineGetTime() : 0.;
  }

  // the key nearest to time, if within kKeyTimeEpsilon
  MyCurve::Keys::const_iterator MyCurve::findKey(OfxTime time) const
  {
    Keys::const_iterator best = _keys.end();
    for (Keys::const_iterator it = _keys.lower_bound(time - kKeyTimeEpsilon);
         it != _keys.end() && it->first <= time + kKeyTimeEpsilon; ++it) {
      if (best == _keys.end() || std::fabs(it->first - time) < std::fabs(best->first - time)) {
        best = it;
      }
    }
    return best;
  }

  void MyCurve::getCurrent(double* v) const
  {
    get(currentTime(), v);
  }

  void MyCurve::get(OfxTime time, double* v) const
  {
    if (_keys.empty()) {
      std::copy(_value.begin(), _value.end(), v);
      return;
    }
    Keys::const_iterator hi = _keys.upper_bound(time); // the first key after time
    if (hi == _keys.begin()) {
      std::copy(hi->second.begin(), hi->second.end(), v); // before the first key
      return;
    }
    Keys::const_iterator lo = hi;
    --lo;
    if (hi == _keys.end() || _interpolation == eConstant) {
      std::copy(lo->second.begin(), lo->second.end(), v); // after the last key, or held
      return;
    }
    const double a = (time - lo->first) / (hi->first - lo->first);
    for (int k = 0; k < _dimension; ++k) {
      v[k] = lo->second[k] + a * (hi->second[k] - lo->second[k]);
      if (_interpolation == eLinearRounded) {
        v[k] = std::floor(v[k] + 0.5);
      }
    }
  }

  // is v finite? (bit tests, which -ffast-math doesn't remove)
  static bool isFinite(double v)
  {
    return !OFX::IsNaN(v) && !OFX::IsInfinite(v);
  }

  // non-finite values are refused (kOfxStatErrValue): an infinite key time or value would give
  // inf/inf, inf-inf or 0*inf (NaN) in the interpolation, the derivative and the integral
  OfxStatus MyCurve::set(const double* v)
  {
    for (int k = 0; k < _dimension; ++k) {
      if (!isFinite(v[k])) {
        return kOfxStatErrValue;
      }
    }
    if (_keys.empty()) {
      _value.assign(v, v + _dimension);
      return kOfxStatOK;
    }
    return set(currentTime(), v); // a parameter with keys: the key at the current time
  }

  OfxStatus MyCurve::set(OfxTime time, const double* v)
  {
    if (!isFinite(time)) {
      return kOfxStatErrValue;
    }
    for (int k = 0; k < _dimension; ++k) {
      if (!isFinite(v[k])) {
        return kOfxStatErrValue;
      }
    }
    if (!_animates) {
      _value.assign(v, v + _dimension);
      return kOfxStatOK;
    }
    Keys::const_iterator it = findKey(time);
    if (it != _keys.end()) {
      time = it->first; // the same key
    }
    _keys[time].assign(v, v + _dimension);
    return kOfxStatOK;
  }

  void MyCurve::derive(OfxTime time, double* v) const
  {
    std::fill(v, v + _dimension, 0.);
    if (_keys.size() < 2 || _interpolation == eConstant) {
      return;
    }
    Keys::const_iterator hi = _keys.upper_bound(time);
    if (hi == _keys.begin() || hi == _keys.end()) {
      return; // constant before the first key and after the last one
    }
    Keys::const_iterator lo = hi;
    --lo;
    for (int k = 0; k < _dimension; ++k) {
      v[k] = (hi->second[k] - lo->second[k]) / (hi->first - lo->first);
    }
  }

  // the integral of the (piecewise linear or piecewise constant) curve from time1 to time2
  // (non-finite bounds are refused: an infinite interval gives inf-inf or 0*inf)
  OfxStatus MyCurve::integrate(OfxTime time1, OfxTime time2, double* v) const
  {
    std::fill(v, v + _dimension, 0.);
    if (!isFinite(time1) || !isFinite(time2)) {
      return kOfxStatErrValue;
    }
    const double sign = time2 < time1 ? -1. : 1.;
    if (time2 < time1) {
      std::swap(time1, time2);
    }
    // the breakpoints: the key times between time1 and time2
    std::vector<OfxTime> t;
    t.push_back(time1);
    for (Keys::const_iterator it = _keys.upper_bound(time1); it != _keys.end() && it->first < time2; ++it) {
      t.push_back(it->first);
    }
    t.push_back(time2);
    std::vector<double> a(_dimension), b(_dimension);
    for (size_t i = 0; i + 1 < t.size(); ++i) {
      get(t[i], &a[0]);
      // the curve is linear (or constant, with the value at its start) on [t[i], t[i+1]]
      if (_interpolation == eConstant) {
        b = a;
      } else {
        get(t[i + 1], &b[0]);
      }
      for (int k = 0; k < _dimension; ++k) {
        v[k] += 0.5 * (a[k] + b[k]) * (t[i + 1] - t[i]);
      }
    }
    for (int k = 0; k < _dimension; ++k) {
      v[k] *= sign;
    }
    return kOfxStatOK;
  }

  OfxStatus MyCurve::getNumKeys(unsigned int& nKeys) const
  {
    nKeys = (unsigned int)_keys.size();
    return kOfxStatOK;
  }

  OfxStatus MyCurve::getKeyTime(int nth, OfxTime& time) const
  {
    if (nth < 0 || nth >= (int)_keys.size()) {
      return kOfxStatErrBadIndex; // "the nthKey does not exist"
    }
    Keys::const_iterator it = _keys.begin();
    std::advance(it, nth);
    time = it->first;
    return kOfxStatOK;
  }

  // direction == 0: the key at time (within kKeyTimeEpsilon); > 0: the next key after time;
  // < 0: the previous key before time
  OfxStatus MyCurve::getKeyIndex(OfxTime time, int direction, int& index) const
  {
    index = -1; // "set to -1 if no key was found"
    Keys::const_iterator it;
    if (direction == 0) {
      it = findKey(time);
    } else if (direction > 0) {
      it = _keys.upper_bound(time + kKeyTimeEpsilon);
    } else {
      it = _keys.lower_bound(time - kKeyTimeEpsilon);
      it = it == _keys.begin() ? _keys.end() : --it;
    }
    if (it == _keys.end()) {
      return kOfxStatFailed; // "the search failed to find a key"
    }
    index = (int)std::distance(_keys.begin(), it);
    return kOfxStatOK;
  }

  OfxStatus MyCurve::deleteKey(OfxTime time)
  {
    Keys::const_iterator it = findKey(time);
    if (it == _keys.end()) {
      return kOfxStatErrBadIndex; // "no key at the given time"
    }
    if (_keys.size() == 1) {
      _value = it->second; // the last key: its value stays
    }
    _keys.erase(it->first);
    return kOfxStatOK;
  }

  OfxStatus MyCurve::deleteAllKeys()
  {
    if (!_keys.empty()) {
      getCurrent(&_value[0]); // the value at the current time stays
      _keys.clear();
    }
    return kOfxStatOK;
  }

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

  // the default value of a numeric parameter (kOfxParamPropDefault), as doubles, with the
  // normalised defaults of spatial double parameters converted to canonical coordinates
  static const double*
  defaultValue(const OFX::Host::Param::Descriptor& descriptor, const MyEffectInstance* effect, int n, bool isDouble)
  {
    static double v[4];
    const OFX::Host::Property::Set& props = descriptor.getProperties();
    for (int k = 0; k < n; ++k) {
      v[k] = isDouble ? props.getDoubleProperty(kOfxParamPropDefault, k) : (double)props.getIntProperty(kOfxParamPropDefault, k);
    }
    if (isDouble) {
      defaultToCanonical(descriptor, effect, v, n);
    }
    return v;
  }

  //
  // MyIntegerInstance
  //

  MyIntegerInstance::MyIntegerInstance(MyEffectInstance* effect,
                                       const std::string& /*name*/,
                                       OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::IntegerInstance(descriptor, effect), _curve(effect, descriptor, 1, MyCurve::eLinearRounded, defaultValue(descriptor, effect, 1, false)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyIntegerInstance::get(int& v)
  {
    double d;
    _curve.getCurrent(&d);
    v = (int)d;
    return kOfxStatOK;
  }

  OfxStatus MyIntegerInstance::get(OfxTime time, int& v)
  {
    double d;
    _curve.get(time, &d);
    v = (int)d;
    return kOfxStatOK;
  }

  OfxStatus MyIntegerInstance::set(int v)
  {
    const double d = (double)v;
    return _curve.set(&d);
  }

  OfxStatus MyIntegerInstance::set(OfxTime time, int v)
  {
    const double d = (double)v;
    return _curve.set(time, &d);
  }

  //
  // MyDoubleInstance
  //

  MyDoubleInstance::MyDoubleInstance(MyEffectInstance* effect,
                                     const std::string& /*name*/,
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::DoubleInstance(descriptor, effect), _curve(effect, descriptor, 1, MyCurve::eLinear, defaultValue(descriptor, effect, 1, true)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyDoubleInstance::get(double& v)
  {
    double d;
    _curve.getCurrent(&d);
    v = d;
    return kOfxStatOK;
  }

  OfxStatus MyDoubleInstance::get(OfxTime time, double& v)
  {
    double d;
    _curve.get(time, &d);
    v = d;
    return kOfxStatOK;
  }

  OfxStatus MyDoubleInstance::set(double v)
  {
    const double d = v;
    return _curve.set(&d);
  }

  OfxStatus MyDoubleInstance::set(OfxTime time, double v)
  {
    const double d = v;
    return _curve.set(time, &d);
  }

  OfxStatus MyDoubleInstance::derive(OfxTime time, double& d)
  {
    _curve.derive(time, &d);
    return kOfxStatOK;
  }

  OfxStatus MyDoubleInstance::integrate(OfxTime time1, OfxTime time2, double& d)
  {
    return _curve.integrate(time1, time2, &d);
  }

  //
  // MyBooleanInstance
  //

  MyBooleanInstance::MyBooleanInstance(MyEffectInstance* effect,
                                       const std::string& /*name*/,
                                       OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::BooleanInstance(descriptor, effect), _curve(effect, descriptor, 1, MyCurve::eConstant, defaultValue(descriptor, effect, 1, false)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyBooleanInstance::get(bool& v)
  {
    double d;
    _curve.getCurrent(&d);
    v = d != 0.;
    return kOfxStatOK;
  }

  OfxStatus MyBooleanInstance::get(OfxTime time, bool& v)
  {
    double d;
    _curve.get(time, &d);
    v = d != 0.;
    return kOfxStatOK;
  }

  OfxStatus MyBooleanInstance::set(bool v)
  {
    const double d = v ? 1. : 0.;
    return _curve.set(&d);
  }

  OfxStatus MyBooleanInstance::set(OfxTime time, bool v)
  {
    const double d = v ? 1. : 0.;
    return _curve.set(time, &d);
  }

  //
  // MyStrChoiceInstance
  //

  MyStrChoiceInstance::MyStrChoiceInstance(MyEffectInstance* effect,
                                     const std::string& /*name*/,
                                     OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::StrChoiceInstance(descriptor, effect), _effect(effect), _descriptor(descriptor)
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
    : OFX::Host::Param::ChoiceInstance(descriptor, effect), _curve(effect, descriptor, 1, MyCurve::eConstant, defaultValue(descriptor, effect, 1, false)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyChoiceInstance::get(int& v)
  {
    double d;
    _curve.getCurrent(&d);
    v = (int)d;
    return kOfxStatOK;
  }

  OfxStatus MyChoiceInstance::get(OfxTime time, int& v)
  {
    double d;
    _curve.get(time, &d);
    v = (int)d;
    return kOfxStatOK;
  }

  OfxStatus MyChoiceInstance::set(int v)
  {
    if (v < 0 || v >= (int)getProperties().getDimension(kOfxParamPropChoiceOption)) {
      return kOfxStatErrValue;
    }
    const double d = (double)v;
    return _curve.set(&d);
  }

  OfxStatus MyChoiceInstance::set(OfxTime time, int v)
  {
    if (v < 0 || v >= (int)getProperties().getDimension(kOfxParamPropChoiceOption)) {
      return kOfxStatErrValue;
    }
    const double d = (double)v;
    return _curve.set(time, &d);
  }

  //
  // MyRGBAInstance
  //

  MyRGBAInstance::MyRGBAInstance(MyEffectInstance* effect,
                                 const std::string& /*name*/,
                                 OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::RGBAInstance(descriptor, effect), _curve(effect, descriptor, 4, MyCurve::eLinear, defaultValue(descriptor, effect, 4, true)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyRGBAInstance::get(double& r,double& g,double& b,double& a)
  {
    double d[4];
    _curve.getCurrent(d);
    r = d[0];
    g = d[1];
    b = d[2];
    a = d[3];
    return kOfxStatOK;
  }

  OfxStatus MyRGBAInstance::get(OfxTime time, double& r,double& g,double& b,double& a)
  {
    double d[4];
    _curve.get(time, d);
    r = d[0];
    g = d[1];
    b = d[2];
    a = d[3];
    return kOfxStatOK;
  }

  OfxStatus MyRGBAInstance::set(double r,double g,double b,double a)
  {
    const double d[4] = { (double)r, (double)g, (double)b, (double)a };
    return _curve.set(d);
  }

  OfxStatus MyRGBAInstance::set(OfxTime time, double r,double g,double b,double a)
  {
    const double d[4] = { (double)r, (double)g, (double)b, (double)a };
    return _curve.set(time, d);
  }

  // ("Only double and colour params can have their derivatives found", "can be integrated")
  OfxStatus MyRGBAInstance::derive(OfxTime time, double& r, double& g, double& b, double& a)
  {
    double d[4];
    _curve.derive(time, d);
    r = d[0];
    g = d[1];
    b = d[2];
    a = d[3];
    return kOfxStatOK;
  }

  OfxStatus MyRGBAInstance::integrate(OfxTime time1, OfxTime time2, double& r, double& g, double& b, double& a)
  {
    double d[4];
    const OfxStatus st = _curve.integrate(time1, time2, d);
    if (st != kOfxStatOK) {
      return st;
    }
    r = d[0];
    g = d[1];
    b = d[2];
    a = d[3];
    return kOfxStatOK;
  }

  //
  // MyRGBInstance
  //

  MyRGBInstance::MyRGBInstance(MyEffectInstance* effect,
                               const std::string& /*name*/,
                               OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::RGBInstance(descriptor, effect), _curve(effect, descriptor, 3, MyCurve::eLinear, defaultValue(descriptor, effect, 3, true)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyRGBInstance::get(double& r,double& g,double& b)
  {
    double d[3];
    _curve.getCurrent(d);
    r = d[0];
    g = d[1];
    b = d[2];
    return kOfxStatOK;
  }

  OfxStatus MyRGBInstance::get(OfxTime time, double& r,double& g,double& b)
  {
    double d[3];
    _curve.get(time, d);
    r = d[0];
    g = d[1];
    b = d[2];
    return kOfxStatOK;
  }

  OfxStatus MyRGBInstance::set(double r,double g,double b)
  {
    const double d[3] = { (double)r, (double)g, (double)b };
    return _curve.set(d);
  }

  OfxStatus MyRGBInstance::set(OfxTime time, double r,double g,double b)
  {
    const double d[3] = { (double)r, (double)g, (double)b };
    return _curve.set(time, d);
  }

  // ("Only double and colour params can have their derivatives found", "can be integrated")
  OfxStatus MyRGBInstance::derive(OfxTime time, double& r, double& g, double& b)
  {
    double d[3];
    _curve.derive(time, d);
    r = d[0];
    g = d[1];
    b = d[2];
    return kOfxStatOK;
  }

  OfxStatus MyRGBInstance::integrate(OfxTime time1, OfxTime time2, double& r, double& g, double& b)
  {
    double d[3];
    const OfxStatus st = _curve.integrate(time1, time2, d);
    if (st != kOfxStatOK) {
      return st;
    }
    r = d[0];
    g = d[1];
    b = d[2];
    return kOfxStatOK;
  }

  //
  // MyDouble2DInstance
  //

  MyDouble2DInstance::MyDouble2DInstance(MyEffectInstance* effect,
                                         const std::string& /*name*/,
                                         OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Double2DInstance(descriptor, effect), _curve(effect, descriptor, 2, MyCurve::eLinear, defaultValue(descriptor, effect, 2, true)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyDouble2DInstance::get(double& x,double& y)
  {
    double d[2];
    _curve.getCurrent(d);
    x = d[0];
    y = d[1];
    return kOfxStatOK;
  }

  OfxStatus MyDouble2DInstance::get(OfxTime time, double& x,double& y)
  {
    double d[2];
    _curve.get(time, d);
    x = d[0];
    y = d[1];
    return kOfxStatOK;
  }

  OfxStatus MyDouble2DInstance::set(double x,double y)
  {
    const double d[2] = { (double)x, (double)y };
    return _curve.set(d);
  }

  OfxStatus MyDouble2DInstance::set(OfxTime time, double x,double y)
  {
    const double d[2] = { (double)x, (double)y };
    return _curve.set(time, d);
  }

  // ("Only double and colour params can have their derivatives found", "can be integrated")
  OfxStatus MyDouble2DInstance::derive(OfxTime time, double& x, double& y)
  {
    double d[2];
    _curve.derive(time, d);
    x = d[0];
    y = d[1];
    return kOfxStatOK;
  }

  OfxStatus MyDouble2DInstance::integrate(OfxTime time1, OfxTime time2, double& x, double& y)
  {
    double d[2];
    const OfxStatus st = _curve.integrate(time1, time2, d);
    if (st != kOfxStatOK) {
      return st;
    }
    x = d[0];
    y = d[1];
    return kOfxStatOK;
  }

  //
  // MyInteger2DInstance
  //

  MyInteger2DInstance::MyInteger2DInstance(MyEffectInstance* effect,
                                           const std::string& /*name*/,
                                           OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Integer2DInstance(descriptor, effect), _curve(effect, descriptor, 2, MyCurve::eLinearRounded, defaultValue(descriptor, effect, 2, false)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyInteger2DInstance::get(int& x,int& y)
  {
    double d[2];
    _curve.getCurrent(d);
    x = (int)d[0];
    y = (int)d[1];
    return kOfxStatOK;
  }

  OfxStatus MyInteger2DInstance::get(OfxTime time, int& x,int& y)
  {
    double d[2];
    _curve.get(time, d);
    x = (int)d[0];
    y = (int)d[1];
    return kOfxStatOK;
  }

  OfxStatus MyInteger2DInstance::set(int x,int y)
  {
    const double d[2] = { (double)x, (double)y };
    return _curve.set(d);
  }

  OfxStatus MyInteger2DInstance::set(OfxTime time, int x,int y)
  {
    const double d[2] = { (double)x, (double)y };
    return _curve.set(time, d);
  }

  //
  // MyDouble3DInstance
  //

  MyDouble3DInstance::MyDouble3DInstance(MyEffectInstance* effect,
                                         const std::string& /*name*/,
                                         OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Double3DInstance(descriptor, effect), _curve(effect, descriptor, 3, MyCurve::eLinear, defaultValue(descriptor, effect, 3, true)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyDouble3DInstance::get(double& x,double& y,double& z)
  {
    double d[3];
    _curve.getCurrent(d);
    x = d[0];
    y = d[1];
    z = d[2];
    return kOfxStatOK;
  }

  OfxStatus MyDouble3DInstance::get(OfxTime time, double& x,double& y,double& z)
  {
    double d[3];
    _curve.get(time, d);
    x = d[0];
    y = d[1];
    z = d[2];
    return kOfxStatOK;
  }

  OfxStatus MyDouble3DInstance::set(double x,double y,double z)
  {
    const double d[3] = { (double)x, (double)y, (double)z };
    return _curve.set(d);
  }

  OfxStatus MyDouble3DInstance::set(OfxTime time, double x,double y,double z)
  {
    const double d[3] = { (double)x, (double)y, (double)z };
    return _curve.set(time, d);
  }

  // ("Only double and colour params can have their derivatives found", "can be integrated")
  OfxStatus MyDouble3DInstance::derive(OfxTime time, double& x, double& y, double& z)
  {
    double d[3];
    _curve.derive(time, d);
    x = d[0];
    y = d[1];
    z = d[2];
    return kOfxStatOK;
  }

  OfxStatus MyDouble3DInstance::integrate(OfxTime time1, OfxTime time2, double& x, double& y, double& z)
  {
    double d[3];
    const OfxStatus st = _curve.integrate(time1, time2, d);
    if (st != kOfxStatOK) {
      return st;
    }
    x = d[0];
    y = d[1];
    z = d[2];
    return kOfxStatOK;
  }

  //
  // MyInteger3DInstance
  //

  MyInteger3DInstance::MyInteger3DInstance(MyEffectInstance* effect,
                                           const std::string& /*name*/,
                                           OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::Integer3DInstance(descriptor, effect), _curve(effect, descriptor, 3, MyCurve::eLinearRounded, defaultValue(descriptor, effect, 3, false)), _effect(effect), _descriptor(descriptor)
  {
  }

  OfxStatus MyInteger3DInstance::get(int& x,int& y,int& z)
  {
    double d[3];
    _curve.getCurrent(d);
    x = (int)d[0];
    y = (int)d[1];
    z = (int)d[2];
    return kOfxStatOK;
  }

  OfxStatus MyInteger3DInstance::get(OfxTime time, int& x,int& y,int& z)
  {
    double d[3];
    _curve.get(time, d);
    x = (int)d[0];
    y = (int)d[1];
    z = (int)d[2];
    return kOfxStatOK;
  }

  OfxStatus MyInteger3DInstance::set(int x,int y,int z)
  {
    const double d[3] = { (double)x, (double)y, (double)z };
    return _curve.set(d);
  }

  OfxStatus MyInteger3DInstance::set(OfxTime time, int x,int y,int z)
  {
    const double d[3] = { (double)x, (double)y, (double)z };
    return _curve.set(time, d);
  }

  //

  MyPushbuttonInstance::MyPushbuttonInstance(MyEffectInstance* effect, 
                                             const std::string& /*name*/, 
                                             OFX::Host::Param::Descriptor& descriptor)
    : OFX::Host::Param::PushbuttonInstance(descriptor, effect), _effect(effect), _descriptor(descriptor)
  {
  }

}
