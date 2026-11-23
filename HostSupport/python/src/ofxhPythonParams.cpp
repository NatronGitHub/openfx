// Python bindings of HostSupport: parameters.
//
// Each parameter is either
// - a C++ instance that stores its value (new_param() returned None, the default). The numeric
//   ones (integer, double, boolean, choice, colours, 2D and 3D) have keys, interpolated as
//   hostDemo's MyCurve (HostSupport/examples/hostDemoParamInstance.cpp): linearly (rounded for
//   integers), held for booleans and choices; strings, custom, string choice and bytes parameters
//   don't animate. The value starts at the descriptor's default (kOfxParamPropDefault, with
//   normalised spatial defaults converted to canonical coordinates, as hostDemo does);
// - or a Python parameter: an instance of a subclass of ofxh.Param, returned by the instance's
//   new_param(descriptor), whose get(), set(), derive(), integrate(), keyframe methods and
//   changed() the adapter of its type calls (PyParamCore).
// Python reads and sets the values of both with ImageEffectInstance.get_param() and set_param().

#include "ofxhPython.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <type_traits>
#include <vector>

#include "ofxParametricParam.h"  // kOfxParamTypeParametric
#include "ofxhUtilities.h"  // OFX::IsNaN, OFX::IsInfinite

using namespace nanobind::literals;

namespace ofxh {

namespace P = OFX::Host::Param;

// ---------------------------------------------------------------------------------------------
// Defaults

void defaultToCanonical(const P::Base &descriptor, double w, double h, double ox, double oy, double *v, int n)
{
  if (descriptor.getDefaultCoordinateSystem() != kOfxParamCoordinatesNormalised) {
    return;
  }
  const std::string &type = descriptor.getDoubleType();
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

/// the default of a numeric parameter, as doubles, with the normalised defaults of spatial double
/// parameters converted to canonical coordinates with the project of the effect (as
/// hostDemoParamInstance.cpp)
static std::vector<double> numericDefault(const P::Descriptor &d, const PyEffectInstance *e, int n, bool isDouble)
{
  const OH::Property::Set &props = d.getProperties();
  std::vector<double> v(n);
  for (int k = 0; k < n; ++k) {
    v[k] = isDouble ? props.getDoubleProperty(kOfxParamPropDefault, k)
                    : static_cast<double>(props.getIntProperty(kOfxParamPropDefault, k));
  }
  if (isDouble && e) {
    double w, h, ox, oy;
    e->getProjectSize(w, h);
    e->getProjectOffset(ox, oy);
    defaultToCanonical(d, w, h, ox, oy, &v[0], n);
  }
  return v;
}

// ---------------------------------------------------------------------------------------------
// Curve: the value of an animatable C++ parameter (hostDemo's MyCurve)
//
// A constant value, or keys, set with paramSetValueAtTime (or set_param(name, value, time)).
// Between keys, the values are interpolated linearly (eLinear), linearly then rounded (integers,
// eLinearRounded), or held until the next key (booleans and choices, eConstant); before the first
// key and after the last one, they are those of the first and last keys. paramSetValue on a
// parameter with keys sets the key at the current time (the instance's timeLineGetTime(), which by
// default is the time of the current action). A parameter that doesn't animate
// (kOfxParamPropAnimates=0) has no keys: paramSetValueAtTime sets its value.

// two key times closer than this are the same key ("some small delta", paramGetKeyIndex)
static const double kKeyTimeEpsilon = 1e-4;

// (bit tests, which -ffast-math doesn't remove, as hostDemo)
static bool isFinite(double v) { return !OFX::IsNaN(v) && !OFX::IsInfinite(v); }

class Curve {
public:
  enum Interpolation { eLinear, eLinearRounded, eConstant };
  using Keys = std::map<OfxTime, std::vector<double> >;

  Curve(PyEffectInstance *effect, const P::Descriptor &descriptor, Interpolation interpolation,
        const std::vector<double> &defaultValue)
    : _effect(effect)
    , _dimension(static_cast<int>(defaultValue.size()))
    , _interpolation(interpolation)
    , _animates(descriptor.getProperties().getIntProperty(kOfxParamPropAnimates) != 0)
    , _value(defaultValue)
  {
  }

  void getCurrent(double *v) const
  {
    if (_keys.empty()) {
      std::copy(_value.begin(), _value.end(), v);  // (without asking Python the current time)
      return;
    }
    get(currentTime(), v);
  }

  void get(OfxTime time, double *v) const
  {
    if (_keys.empty()) {
      std::copy(_value.begin(), _value.end(), v);
      return;
    }
    Keys::const_iterator hi = _keys.upper_bound(time);  // the first key after time
    if (hi == _keys.begin()) {
      std::copy(hi->second.begin(), hi->second.end(), v);  // before the first key
      return;
    }
    Keys::const_iterator lo = hi;
    --lo;
    if (hi == _keys.end() || _interpolation == eConstant) {
      std::copy(lo->second.begin(), lo->second.end(), v);  // after the last key, or held
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

  // non-finite values are refused (kOfxStatErrValue), as hostDemo
  OfxStatus set(const double *v)
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
    return set(currentTime(), v);  // a parameter with keys: the key at the current time
  }

  OfxStatus set(OfxTime time, const double *v)
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
      time = it->first;  // the same key
    }
    _keys[time].assign(v, v + _dimension);
    return kOfxStatOK;
  }

  void derive(OfxTime time, double *v) const
  {
    std::fill(v, v + _dimension, 0.);
    if (_keys.size() < 2 || _interpolation == eConstant) {
      return;
    }
    Keys::const_iterator hi = _keys.upper_bound(time);
    if (hi == _keys.begin() || hi == _keys.end()) {
      return;  // constant before the first key and after the last one
    }
    Keys::const_iterator lo = hi;
    --lo;
    for (int k = 0; k < _dimension; ++k) {
      v[k] = (hi->second[k] - lo->second[k]) / (hi->first - lo->first);
    }
  }

  // the integral of the (piecewise linear or piecewise constant) curve from time1 to time2
  OfxStatus integrate(OfxTime time1, OfxTime time2, double *v) const
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

  OfxStatus getNumKeys(unsigned int &nKeys) const
  {
    nKeys = static_cast<unsigned int>(_keys.size());
    return kOfxStatOK;
  }

  OfxStatus getKeyTime(int nth, OfxTime &time) const
  {
    if (nth < 0 || nth >= static_cast<int>(_keys.size())) {
      return kOfxStatErrBadIndex;  // "the nthKey does not exist"
    }
    Keys::const_iterator it = _keys.begin();
    std::advance(it, nth);
    time = it->first;
    return kOfxStatOK;
  }

  // direction == 0: the key at time (within kKeyTimeEpsilon); > 0: the next key after time;
  // < 0: the previous key before time
  OfxStatus getKeyIndex(OfxTime time, int direction, int &index) const
  {
    index = -1;  // "set to -1 if no key was found"
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
      return kOfxStatFailed;  // "the search failed to find a key"
    }
    index = static_cast<int>(std::distance(_keys.begin(), it));
    return kOfxStatOK;
  }

  OfxStatus deleteKey(OfxTime time)
  {
    Keys::const_iterator it = findKey(time);
    if (it == _keys.end()) {
      return kOfxStatErrBadIndex;  // "no key at the given time"
    }
    if (_keys.size() == 1) {
      _value = it->second;  // the last key: its value stays
    }
    _keys.erase(it->first);
    return kOfxStatOK;
  }

  OfxStatus deleteAllKeys()
  {
    if (!_keys.empty()) {
      getCurrent(&_value[0]);  // the value at the current time stays
      _keys.clear();
    }
    return kOfxStatOK;
  }

private:
  // the key nearest to time, if within kKeyTimeEpsilon
  Keys::const_iterator findKey(OfxTime time) const
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
  OfxTime currentTime() const { return _effect ? _effect->timeLineGetTime() : 0.; }

  PyEffectInstance *_effect;
  int _dimension;
  Interpolation _interpolation;
  bool _animates;
  std::vector<double> _value;  // without keys
  Keys _keys;
};

// ---------------------------------------------------------------------------------------------
// The stored-value C++ parameters

/// an animatable stored parameter: its value is a Curve, which gives the KeyframeParam functions
template <class Base> class CurveParam : public Base {
public:
  CurveParam(PyEffectInstance *e, P::Descriptor &d, Curve::Interpolation interpolation, int n, bool isDouble)
    : Base(d, e), _curve(e, d, interpolation, numericDefault(d, e, n, isDouble)) {}
  OfxStatus getNumKeys(unsigned int &n) const override { return _curve.getNumKeys(n); }
  OfxStatus getKeyTime(int nth, OfxTime &t) const override { return _curve.getKeyTime(nth, t); }
  OfxStatus getKeyIndex(OfxTime t, int dir, int &i) const override { return _curve.getKeyIndex(t, dir, i); }
  OfxStatus deleteKey(OfxTime t) override { return _curve.deleteKey(t); }
  OfxStatus deleteAllKeys() override { return _curve.deleteAllKeys(); }

protected:
  Curve _curve;
};

class StoredInteger : public CurveParam<P::IntegerInstance> {
public:
  StoredInteger(PyEffectInstance *e, P::Descriptor &d) : CurveParam(e, d, Curve::eLinearRounded, 1, false) {}
  OfxStatus get(int &v) override { double d; _curve.getCurrent(&d); v = static_cast<int>(d); return kOfxStatOK; }
  OfxStatus get(OfxTime t, int &v) override { double d; _curve.get(t, &d); v = static_cast<int>(d); return kOfxStatOK; }
  OfxStatus set(int v) override { const double d = v; return _curve.set(&d); }
  OfxStatus set(OfxTime t, int v) override { const double d = v; return _curve.set(t, &d); }
};

class StoredDouble : public CurveParam<P::DoubleInstance> {
public:
  StoredDouble(PyEffectInstance *e, P::Descriptor &d) : CurveParam(e, d, Curve::eLinear, 1, true) {}
  OfxStatus get(double &v) override { _curve.getCurrent(&v); return kOfxStatOK; }
  OfxStatus get(OfxTime t, double &v) override { _curve.get(t, &v); return kOfxStatOK; }
  OfxStatus set(double v) override { return _curve.set(&v); }
  OfxStatus set(OfxTime t, double v) override { return _curve.set(t, &v); }
  OfxStatus derive(OfxTime t, double &v) override { _curve.derive(t, &v); return kOfxStatOK; }
  OfxStatus integrate(OfxTime t1, OfxTime t2, double &v) override { return _curve.integrate(t1, t2, &v); }
};

class StoredBoolean : public CurveParam<P::BooleanInstance> {
public:
  StoredBoolean(PyEffectInstance *e, P::Descriptor &d) : CurveParam(e, d, Curve::eConstant, 1, false) {}
  OfxStatus get(bool &v) override { double d; _curve.getCurrent(&d); v = d != 0.; return kOfxStatOK; }
  OfxStatus get(OfxTime t, bool &v) override { double d; _curve.get(t, &d); v = d != 0.; return kOfxStatOK; }
  OfxStatus set(bool v) override { const double d = v ? 1. : 0.; return _curve.set(&d); }
  OfxStatus set(OfxTime t, bool v) override { const double d = v ? 1. : 0.; return _curve.set(t, &d); }
};

class StoredChoice : public CurveParam<P::ChoiceInstance> {
  bool valid(int v) { return v >= 0 && v < getProperties().getDimension(kOfxParamPropChoiceOption); }
public:
  StoredChoice(PyEffectInstance *e, P::Descriptor &d) : CurveParam(e, d, Curve::eConstant, 1, false) {}
  OfxStatus get(int &v) override { double d; _curve.getCurrent(&d); v = static_cast<int>(d); return kOfxStatOK; }
  OfxStatus get(OfxTime t, int &v) override { double d; _curve.get(t, &d); v = static_cast<int>(d); return kOfxStatOK; }
  OfxStatus set(int v) override
  {
    const double d = v;
    return valid(v) ? _curve.set(&d) : kOfxStatErrValue;
  }
  OfxStatus set(OfxTime t, int v) override
  {
    const double d = v;
    return valid(v) ? _curve.set(t, &d) : kOfxStatErrValue;
  }
};

// (strings, custom, string choices and bytes don't animate: the host descriptor says so)
class StoredStrChoice : public P::StrChoiceInstance {
  std::string _v;
public:
  /// one of the enums (kOfxParamPropChoiceEnum), if there are any
  bool valid(const std::string &v)
  {
    const OH::Property::Set &p = getProperties();
    const int n = p.fetchProperty(kOfxParamPropChoiceEnum) ? p.getDimension(kOfxParamPropChoiceEnum) : 0;
    for (int i = 0; i < n; ++i) {
      if (p.getStringProperty(kOfxParamPropChoiceEnum, i) == v) {
        return true;
      }
    }
    return n == 0;
  }
public:
  StoredStrChoice(PyEffectInstance *e, P::Descriptor &d) : P::StrChoiceInstance(d, e) { _v = getDefaultEnum(); }
  OfxStatus get(std::string &v) override { v = _v; return kOfxStatOK; }
  OfxStatus get(OfxTime, std::string &v) override { v = _v; return kOfxStatOK; }
  OfxStatus set(const char *v) override
  {
    const std::string s = v ? v : "";
    if (!valid(s)) {
      return kOfxStatErrValue;
    }
    _v = s;
    return kOfxStatOK;
  }
  OfxStatus set(OfxTime, const char *v) override { return set(v); }
};

class StoredString : public P::StringInstance {
  std::string _v;
public:
  StoredString(PyEffectInstance *e, P::Descriptor &d) : P::StringInstance(d, e)
  {
    _v = d.getProperties().getStringProperty(kOfxParamPropDefault);
  }
  OfxStatus get(std::string &v) override { v = _v; return kOfxStatOK; }
  OfxStatus get(OfxTime, std::string &v) override { v = _v; return kOfxStatOK; }
  OfxStatus set(const char *v) override { _v = v ? v : ""; return kOfxStatOK; }
  OfxStatus set(OfxTime, const char *v) override { _v = v ? v : ""; return kOfxStatOK; }
};

class StoredCustom : public P::CustomInstance {
  std::string _v;
public:
  StoredCustom(PyEffectInstance *e, P::Descriptor &d) : P::CustomInstance(d, e)
  {
    _v = d.getProperties().getStringProperty(kOfxParamPropDefault);
  }
  OfxStatus get(std::string &v) override { v = _v; return kOfxStatOK; }
  OfxStatus get(OfxTime, std::string &v) override { v = _v; return kOfxStatOK; }
  OfxStatus set(const char *v) override { _v = v ? v : ""; return kOfxStatOK; }
  OfxStatus set(OfxTime, const char *v) override { _v = v ? v : ""; return kOfxStatOK; }
};

class StoredBytes : public P::BytesInstance {
  std::vector<unsigned char> _v;
public:
  StoredBytes(PyEffectInstance *e, P::Descriptor &d) : P::BytesInstance(d, e), _v(getDefault()) {}
  OfxStatus get(std::vector<unsigned char> &v) override { v = _v; return kOfxStatOK; }
  OfxStatus get(OfxTime, std::vector<unsigned char> &v) override { v = _v; return kOfxStatOK; }
  OfxStatus set(const unsigned char *data, size_t length) override
  {
    _v.assign(data, data + (data ? length : 0));
    return kOfxStatOK;
  }
  OfxStatus set(OfxTime, const unsigned char *data, size_t length) override { return set(data, length); }
};

/// a multi-dimensional stored parameter: getAt() and setAt() for the overrides of its arity
template <class Base, class T, int N> class StoredN : public CurveParam<Base> {
public:
  StoredN(PyEffectInstance *e, P::Descriptor &d, Curve::Interpolation interpolation)
    : CurveParam<Base>(e, d, interpolation, N, std::is_same<T, double>::value) {}

protected:
  OfxStatus getAt(const OfxTime *t, T *v)
  {
    double d[N];
    if (t) {
      this->_curve.get(*t, d);
    } else {
      this->_curve.getCurrent(d);
    }
    for (int k = 0; k < N; ++k) {
      v[k] = static_cast<T>(d[k]);
    }
    return kOfxStatOK;
  }
  OfxStatus setAt(const OfxTime *t, const T *v)
  {
    double d[N];
    for (int k = 0; k < N; ++k) {
      d[k] = static_cast<double>(v[k]);
    }
    return t ? this->_curve.set(*t, d) : this->_curve.set(d);
  }
};

// The overrides of each arity, which call getAt() and setAt() (and derive and integrate the
// curve, for the double ones, as hostDemo; the integer ones keep HostSupport's defaults). Macros,
// for brevity: partial specializations of a template on N could declare them too, but that takes
// 3 specializations here and 3 for the Python parameters (OFXH_PY_N), plus a chain of aliases,
// against these twelve one-line uses. #undef'd after their last use.
#define OFXH_STORED_N(Name, Base, T, N, interpolation, DERIVE)                                        \
  class Name : public StoredN<Base, T, N> {                                                          \
  public:                                                                                            \
    Name(PyEffectInstance *e, P::Descriptor &d) : StoredN(e, d, interpolation) {}                    \
    OFXH_SIGNATURES_##N(T)                                                                           \
    DERIVE                                                                                           \
  };

#define OFXH_SIGNATURES_2(T)                                                                          \
  OfxStatus get(T &x, T &y) override { T v[2] = {}; OfxStatus s = getAt(nullptr, v); x = v[0]; y = v[1]; return s; }             \
  OfxStatus get(OfxTime t, T &x, T &y) override { T v[2] = {}; OfxStatus s = getAt(&t, v); x = v[0]; y = v[1]; return s; }       \
  OfxStatus set(T x, T y) override { const T v[2] = {x, y}; return setAt(nullptr, v); }                                     \
  OfxStatus set(OfxTime t, T x, T y) override { const T v[2] = {x, y}; return setAt(&t, v); }
#define OFXH_SIGNATURES_3(T)                                                                          \
  OfxStatus get(T &x, T &y, T &z) override { T v[3] = {}; OfxStatus s = getAt(nullptr, v); x = v[0]; y = v[1]; z = v[2]; return s; } \
  OfxStatus get(OfxTime t, T &x, T &y, T &z) override { T v[3] = {}; OfxStatus s = getAt(&t, v); x = v[0]; y = v[1]; z = v[2]; return s; } \
  OfxStatus set(T x, T y, T z) override { const T v[3] = {x, y, z}; return setAt(nullptr, v); }                             \
  OfxStatus set(OfxTime t, T x, T y, T z) override { const T v[3] = {x, y, z}; return setAt(&t, v); }
#define OFXH_SIGNATURES_4(T)                                                                          \
  OfxStatus get(T &x, T &y, T &z, T &w) override { T v[4] = {}; OfxStatus s = getAt(nullptr, v); x = v[0]; y = v[1]; z = v[2]; w = v[3]; return s; } \
  OfxStatus get(OfxTime t, T &x, T &y, T &z, T &w) override { T v[4] = {}; OfxStatus s = getAt(&t, v); x = v[0]; y = v[1]; z = v[2]; w = v[3]; return s; } \
  OfxStatus set(T x, T y, T z, T w) override { const T v[4] = {x, y, z, w}; return setAt(nullptr, v); }                    \
  OfxStatus set(OfxTime t, T x, T y, T z, T w) override { const T v[4] = {x, y, z, w}; return setAt(&t, v); }

#define OFXH_DERIVE_2                                                                                  \
  OfxStatus derive(OfxTime t, double &x, double &y) override { double v[2]; _curve.derive(t, v); x = v[0]; y = v[1]; return kOfxStatOK; } \
  OfxStatus integrate(OfxTime t1, OfxTime t2, double &x, double &y) override { double v[2]; OfxStatus s = _curve.integrate(t1, t2, v); x = v[0]; y = v[1]; return s; }
#define OFXH_DERIVE_3                                                                                  \
  OfxStatus derive(OfxTime t, double &x, double &y, double &z) override { double v[3]; _curve.derive(t, v); x = v[0]; y = v[1]; z = v[2]; return kOfxStatOK; } \
  OfxStatus integrate(OfxTime t1, OfxTime t2, double &x, double &y, double &z) override { double v[3]; OfxStatus s = _curve.integrate(t1, t2, v); x = v[0]; y = v[1]; z = v[2]; return s; }
#define OFXH_DERIVE_4                                                                                  \
  OfxStatus derive(OfxTime t, double &x, double &y, double &z, double &w) override { double v[4]; _curve.derive(t, v); x = v[0]; y = v[1]; z = v[2]; w = v[3]; return kOfxStatOK; } \
  OfxStatus integrate(OfxTime t1, OfxTime t2, double &x, double &y, double &z, double &w) override { double v[4]; OfxStatus s = _curve.integrate(t1, t2, v); x = v[0]; y = v[1]; z = v[2]; w = v[3]; return s; }
#define OFXH_NO_DERIVE

OFXH_STORED_N(StoredRGBA, P::RGBAInstance, double, 4, Curve::eLinear, OFXH_DERIVE_4)
OFXH_STORED_N(StoredRGB, P::RGBInstance, double, 3, Curve::eLinear, OFXH_DERIVE_3)
OFXH_STORED_N(StoredDouble2D, P::Double2DInstance, double, 2, Curve::eLinear, OFXH_DERIVE_2)
OFXH_STORED_N(StoredDouble3D, P::Double3DInstance, double, 3, Curve::eLinear, OFXH_DERIVE_3)
OFXH_STORED_N(StoredInteger2D, P::Integer2DInstance, int, 2, Curve::eLinearRounded, OFXH_NO_DERIVE)
OFXH_STORED_N(StoredInteger3D, P::Integer3DInstance, int, 3, Curve::eLinearRounded, OFXH_NO_DERIVE)
#undef OFXH_STORED_N
#undef OFXH_DERIVE_2
#undef OFXH_DERIVE_3
#undef OFXH_DERIVE_4
#undef OFXH_NO_DERIVE

P::Instance *newStoredParam(PyEffectInstance *e, P::Descriptor &d)
{
  const std::string &t = d.getType();
  if (t == kOfxParamTypeString) return new StoredString(e, d);
  if (t == kOfxParamTypeBytes) return new StoredBytes(e, d);
  if (t == kOfxParamTypeInteger) return new StoredInteger(e, d);
  if (t == kOfxParamTypeDouble) return new StoredDouble(e, d);
  if (t == kOfxParamTypeBoolean) return new StoredBoolean(e, d);
  if (t == kOfxParamTypeChoice) return new StoredChoice(e, d);
  if (t == kOfxParamTypeStrChoice) return new StoredStrChoice(e, d);
  if (t == kOfxParamTypeRGBA) return new StoredRGBA(e, d);
  if (t == kOfxParamTypeRGB) return new StoredRGB(e, d);
  if (t == kOfxParamTypeDouble2D) return new StoredDouble2D(e, d);
  if (t == kOfxParamTypeInteger2D) return new StoredInteger2D(e, d);
  if (t == kOfxParamTypeDouble3D) return new StoredDouble3D(e, d);
  if (t == kOfxParamTypeInteger3D) return new StoredInteger3D(e, d);
  if (t == kOfxParamTypeCustom) return new StoredCustom(e, d);
  if (t == kOfxParamTypePushButton) return new P::PushbuttonInstance(d, e);
  if (t == kOfxParamTypeGroup) return new P::GroupInstance(d, e);
  if (t == kOfxParamTypePage) return new P::PageInstance(d, e);
  return nullptr;  // e.g. parametric parameters: newParam() saves the error
}

// ---------------------------------------------------------------------------------------------
// The Python parameters (ofxh.Param)

P::Instance *ParamBase::get() const
{
  checkThread("ofxh", false);
  if (!ref->p) {
    raiseDeleted("ofxh.Param");
  }
  return ref->as<P::Instance>();
}

PyParamCore *ParamBase::core() const { return dynamic_cast<PyParamCore *>(get()); }

PyParamCore::PyParamCore(PyEffectInstance *effect, P::Instance *self, nb::object py, RefPtr ref)
  : _effect(effect)
  , _py(std::move(py))
  , _ref(std::move(ref))
  , _self(self)
{
  _ref->p = self;
}

PyParamCore::~PyParamCore()
{
  _ref->p = nullptr;
  dropLast(_py);
}

[[noreturn]] static void raiseValue(const std::string &message) { raiseError(PyExc_ValueError, message); }

/// raise ofxh.OfxError(status)
[[noreturn]] static void raiseOfxError(OfxStatus st)
{
  nb::object e = globals().ofxError(stat(st));
  PyErr_SetObject(globals().ofxError.ptr(), e.ptr());
  throw nb::python_error();
}

// conversions of one value
template <class T> struct Conv;
template <> struct Conv<int> {
  static int from(nb::handle h) { return nb::cast<int>(h); }
  static nb::object to(int v) { return nb::int_(v); }
};
template <> struct Conv<double> {
  static double from(nb::handle h) { return nb::cast<double>(h); }
  static nb::object to(double v) { return nb::float_(v); }
};
template <> struct Conv<bool> {
  static bool from(nb::handle h)
  {
    const int r = PyObject_IsTrue(h.ptr());
    if (r < 0) {
      throw nb::python_error();
    }
    return r != 0;
  }
  static nb::object to(bool v) { return nb::bool_(v); }
};
template <> struct Conv<std::string> {
  static std::string from(nb::handle h) { return nb::cast<std::string>(h); }
  static nb::object to(const std::string &v) { return nb::str(v.c_str(), v.size()); }
};

static nb::object timeArg(const OfxTime *t) { return t ? nb::object(nb::float_(*t)) : nb::none(); }

/// the time of get(): always a time, the current one (timeLineGetTime()) for paramGetValue
static nb::object getTime(const PyParamCore &c, const OfxTime *t)
{
  return nb::float_(t ? *t : c._effect->timeLineGetTime());
}

/// n values from what a Python method returned (a scalar for n == 1, else a sequence of n)
template <class T> static const char *expected();
template <> const char *expected<int>() { return "an int"; }
template <> const char *expected<double>() { return "a float"; }
template <> const char *expected<bool>() { return "a bool"; }
template <> const char *expected<std::string>() { return "a str"; }

/// n values from what a Python method returned (a scalar for n == 1, else a sequence of n);
/// TypeError, naming the parameter and the method, if they are not of the parameter's type
template <class T> static void fromPython(nb::handle r, T *v, int n, const P::Instance &p, const char *method)
{
  const std::string where = "parameter " + p.getName() + " (" + shortName("param_type", p.getType()) + "): " + method +
                            "() ";
  try {
    if (n == 1) {
      v[0] = Conv<T>::from(r);
      return;
    }
    // any sequence (a tuple, a list, a NumPy array...) but a str or bytes
    if (!PySequence_Check(r.ptr()) || PyUnicode_Check(r.ptr()) || PyBytes_Check(r.ptr()) ||
        PyByteArray_Check(r.ptr())) {
      raiseError(PyExc_TypeError, where + "must return a sequence of " + std::to_string(n) + " values, not " +
                                  nb::cast<std::string>(nb::str(r.type())));
    }
    const nb::tuple t = nb::tuple(r);
    if (static_cast<int>(t.size()) != n) {
      raiseValue(where + "must return " + std::to_string(n) + " values");
    }
    for (int k = 0; k < n; ++k) {
      v[k] = Conv<T>::from(t[k]);
    }
  } catch (const nb::cast_error &) {
    raiseError(PyExc_TypeError, where + "must return " + expected<T>() + (n > 1 ? " for each value" : "") + ", not " +
                                nb::cast<std::string>(nb::repr(r)));
  }
}

template <class T> static nb::object toPython(const T *v, int n)
{
  if (n == 1) {
    return Conv<T>::to(v[0]);
  }
  nb::list l;
  for (int k = 0; k < n; ++k) {
    l.append(Conv<T>::to(v[k]));
  }
  return nb::tuple(l);
}

// the calls into Python (each one a whole callPython())

template <class T> static OfxStatus pyGet(const PyParamCore &c, const OfxTime *t, T *v, int n)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    fromPython(c._py.attr("get")(getTime(c, t)), v, n, *c._self, "get");
    return kOfxStatOK;
  });
}

template <class T> static OfxStatus pySet(const PyParamCore &c, const OfxTime *t, const T *v, int n)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    return statusFromResult(c._py.attr("set")(toPython(v, n), timeArg(t)));
  });
}

template <class T> static OfxStatus pyDerive(const PyParamCore &c, OfxTime t, T *v, int n)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    fromPython(c._py.attr("derive")(t), v, n, *c._self, "derive");
    return kOfxStatOK;
  });
}

template <class T> static OfxStatus pyIntegrate(const PyParamCore &c, OfxTime t1, OfxTime t2, T *v, int n)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    fromPython(c._py.attr("integrate")(t1, t2), v, n, *c._self, "integrate");
    return kOfxStatOK;
  });
}

static OfxStatus pyNumKeys(const PyParamCore &c, unsigned int &n)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    const int k = nb::cast<int>(c._py.attr("num_keys")());
    if (k < 0) {
      raiseValue("parameter " + c._self->getName() + ": num_keys() < 0");
    }
    n = static_cast<unsigned int>(k);
    return kOfxStatOK;
  });
}

static OfxStatus pyKeyTime(const PyParamCore &c, int nth, OfxTime &t)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    t = nb::cast<double>(c._py.attr("key_time")(nth));
    return kOfxStatOK;
  });
}

static OfxStatus pyKeyIndex(const PyParamCore &c, OfxTime t, int direction, int &index)
{
  index = -1;
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    nb::object r = c._py.attr("key_index")(t, direction);
    index = r.is_none() ? -1 : nb::cast<int>(r);
    if (index < 0) {
      index = -1;
      return kOfxStatFailed;  // "the search failed to find a key"
    }
    return kOfxStatOK;
  });
}

static OfxStatus pyCall(const PyParamCore &c, const char *method, const OfxTime *t)
{
  return callPython(StatusFallback{kOfxStatFailed}, [&] {
    return statusFromResult(t ? c._py.attr(method)(*t) : c._py.attr(method)());
  });
}

static void pyChanged(const PyParamCore &c, const std::string &name)
{
  callPython(Void(), [&] { c._py.attr("changed")(name); });
}

/// what every Python parameter adapter has: its HostSupport class, and PyParamCore
template <class Base> class PyParam : public Base, public PyParamCore {
public:
  PyParam(PyEffectInstance *e, P::Descriptor &d, nb::object py, RefPtr ref)
    : Base(d, e), PyParamCore(e, this, std::move(py), std::move(ref)) {}
  /// the setEnabled/setSecret/... notifications: changed(property name), after HostSupport's
  void notify(const std::string &name, bool single, int num) OFX_EXCEPTION_SPEC override
  {
    Base::notify(name, single, num);
    pyChanged(*this, name);
  }
};

/// a Python parameter with keys: the KeyframeParam functions call its methods
template <class Base> class PyKeyedParam : public PyParam<Base> {
public:
  using PyParam<Base>::PyParam;
  OfxStatus getNumKeys(unsigned int &n) const override { return pyNumKeys(*this, n); }
  OfxStatus getKeyTime(int nth, OfxTime &t) const override { return pyKeyTime(*this, nth, t); }
  OfxStatus getKeyIndex(OfxTime t, int d, int &i) const override { return pyKeyIndex(*this, t, d, i); }
  OfxStatus deleteKey(OfxTime t) override { return pyCall(*this, "delete_key", &t); }
  OfxStatus deleteAllKeys() override { return pyCall(*this, "delete_all_keys", nullptr); }
};

/// the Python parameters of one value of type T
template <class Base, class T> class PyScalar : public PyKeyedParam<Base> {
public:
  using PyKeyedParam<Base>::PyKeyedParam;
  OfxStatus get(T &v) override { return pyGet<T>(*this, nullptr, &v, 1); }
  OfxStatus get(OfxTime t, T &v) override { return pyGet<T>(*this, &t, &v, 1); }
  OfxStatus set(T v) override { return pySet<T>(*this, nullptr, &v, 1); }
  OfxStatus set(OfxTime t, T v) override { return pySet<T>(*this, &t, &v, 1); }
};

/// the integer and double ones, which derive and integrate too
template <class Base, class T> class PyNumber : public PyScalar<Base, T> {
public:
  using PyScalar<Base, T>::PyScalar;
  OfxStatus derive(OfxTime t, T &v) override { return pyDerive<T>(*this, t, &v, 1); }
  OfxStatus integrate(OfxTime t1, OfxTime t2, T &v) override { return pyIntegrate<T>(*this, t1, t2, &v, 1); }
};

using PyInteger = PyNumber<P::IntegerInstance, int>;
using PyDouble = PyNumber<P::DoubleInstance, double>;
using PyBoolean = PyScalar<P::BooleanInstance, bool>;
using PyChoice = PyScalar<P::ChoiceInstance, int>;

/// the string, custom and string choice Python parameters
template <class Base> class PyStringParam : public PyKeyedParam<Base> {
public:
  using PyKeyedParam<Base>::PyKeyedParam;
  OfxStatus get(std::string &v) override { return pyGet<std::string>(*this, nullptr, &v, 1); }
  OfxStatus get(OfxTime t, std::string &v) override { return pyGet<std::string>(*this, &t, &v, 1); }
  OfxStatus set(const char *v) override { const std::string s(v ? v : ""); return pySet<std::string>(*this, nullptr, &s, 1); }
  OfxStatus set(OfxTime t, const char *v) override { const std::string s(v ? v : ""); return pySet<std::string>(*this, &t, &s, 1); }
};

using PyStrChoice = PyStringParam<P::StrChoiceInstance>;
using PyString = PyStringParam<P::StringInstance>;
using PyCustom = PyStringParam<P::CustomInstance>;

static std::vector<unsigned char> bytesFrom(nb::handle r)
{
  // bytes, bytearray, memoryview, a list of ints...: what bytes() takes
  nb::object b = nb::borrow(reinterpret_cast<PyObject *>(&PyBytes_Type))(r);
  char *data = nullptr;
  Py_ssize_t n = 0;
  if (PyBytes_AsStringAndSize(b.ptr(), &data, &n) != 0) {
    throw nb::python_error();
  }
  const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data);
  return std::vector<unsigned char>(bytes, bytes + n);
}

class PyBytes : public PyKeyedParam<P::BytesInstance> {
public:
  using PyKeyedParam::PyKeyedParam;
  OfxStatus get(std::vector<unsigned char> &v) override { return getAt(nullptr, v); }
  OfxStatus get(OfxTime t, std::vector<unsigned char> &v) override { return getAt(&t, v); }
  OfxStatus set(const unsigned char *data, size_t length) override { return setAt(nullptr, data, length); }
  OfxStatus set(OfxTime t, const unsigned char *data, size_t length) override { return setAt(&t, data, length); }

private:
  OfxStatus getAt(const OfxTime *t, std::vector<unsigned char> &v)
  {
    return callPython(StatusFallback{kOfxStatFailed}, [&] {
      v = bytesFrom(_py.attr("get")(getTime(*this, t)));
      return kOfxStatOK;
    });
  }
  OfxStatus setAt(const OfxTime *t, const unsigned char *data, size_t length)
  {
    return callPython(StatusFallback{kOfxStatFailed}, [&] {
      const char *chars = data ? reinterpret_cast<const char *>(data) : "";
      return statusFromResult(_py.attr("set")(nb::bytes(chars, data ? length : 0), timeArg(t)));
    });
  }
};

/// a multi-dimensional Python parameter: getAt() and setAt() for the overrides of its arity
template <class Base, class T, int N> class PyN : public PyKeyedParam<Base> {
public:
  using PyKeyedParam<Base>::PyKeyedParam;

protected:
  OfxStatus getAt(const OfxTime *t, T *v) { return pyGet<T>(*this, t, v, N); }
  OfxStatus setAt(const OfxTime *t, const T *v) { return pySet<T>(*this, t, v, N); }
};

// the overrides of each arity (macros, for the reason given at OFXH_STORED_N)
#define OFXH_PY_N(Name, Base, T, N)                                                                   \
  class Name : public PyN<Base, T, N> {                                                              \
  public:                                                                                            \
    using PyN::PyN;                                                                                  \
    OFXH_SIGNATURES_##N(T)                                                                           \
    OFXH_PY_DERIVE_##N(T)                                                                            \
  };
#define OFXH_PY_DERIVE_2(T)                                                                           \
  OfxStatus derive(OfxTime t, T &x, T &y) override { T v[2] = {}; OfxStatus s = pyDerive<T>(*this, t, v, 2); x = v[0]; y = v[1]; return s; } \
  OfxStatus integrate(OfxTime t1, OfxTime t2, T &x, T &y) override { T v[2] = {}; OfxStatus s = pyIntegrate<T>(*this, t1, t2, v, 2); x = v[0]; y = v[1]; return s; }
#define OFXH_PY_DERIVE_3(T)                                                                           \
  OfxStatus derive(OfxTime t, T &x, T &y, T &z) override { T v[3] = {}; OfxStatus s = pyDerive<T>(*this, t, v, 3); x = v[0]; y = v[1]; z = v[2]; return s; } \
  OfxStatus integrate(OfxTime t1, OfxTime t2, T &x, T &y, T &z) override { T v[3] = {}; OfxStatus s = pyIntegrate<T>(*this, t1, t2, v, 3); x = v[0]; y = v[1]; z = v[2]; return s; }
#define OFXH_PY_DERIVE_4(T)                                                                           \
  OfxStatus derive(OfxTime t, T &x, T &y, T &z, T &w) override { T v[4] = {}; OfxStatus s = pyDerive<T>(*this, t, v, 4); x = v[0]; y = v[1]; z = v[2]; w = v[3]; return s; } \
  OfxStatus integrate(OfxTime t1, OfxTime t2, T &x, T &y, T &z, T &w) override { T v[4] = {}; OfxStatus s = pyIntegrate<T>(*this, t1, t2, v, 4); x = v[0]; y = v[1]; z = v[2]; w = v[3]; return s; }

OFXH_PY_N(PyRGBA, P::RGBAInstance, double, 4)
OFXH_PY_N(PyRGB, P::RGBInstance, double, 3)
OFXH_PY_N(PyDouble2D, P::Double2DInstance, double, 2)
OFXH_PY_N(PyInteger2D, P::Integer2DInstance, int, 2)
OFXH_PY_N(PyDouble3D, P::Double3DInstance, double, 3)
OFXH_PY_N(PyInteger3D, P::Integer3DInstance, int, 3)
#undef OFXH_PY_N
#undef OFXH_PY_DERIVE_2
#undef OFXH_PY_DERIVE_3
#undef OFXH_PY_DERIVE_4
#undef OFXH_SIGNATURES_2
#undef OFXH_SIGNATURES_3
#undef OFXH_SIGNATURES_4

using PyPushButton = PyKeyedParam<P::PushbuttonInstance>;
using PyGroup = PyParam<P::GroupInstance>;
using PyPage = PyParam<P::PageInstance>;

P::Instance *newPyParam(PyEffectInstance *e, P::Descriptor &d, nb::object py, RefPtr ref)
{
  const std::string &t = d.getType();
  if (t == kOfxParamTypeInteger) return new PyInteger(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeDouble) return new PyDouble(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeBoolean) return new PyBoolean(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeChoice) return new PyChoice(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeStrChoice) return new PyStrChoice(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeString) return new PyString(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeCustom) return new PyCustom(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeBytes) return new PyBytes(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeRGBA) return new PyRGBA(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeRGB) return new PyRGB(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeDouble2D) return new PyDouble2D(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeInteger2D) return new PyInteger2D(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeDouble3D) return new PyDouble3D(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeInteger3D) return new PyInteger3D(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypePushButton) return new PyPushButton(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypeGroup) return new PyGroup(e, d, std::move(py), std::move(ref));
  if (t == kOfxParamTypePage) return new PyPage(e, d, std::move(py), std::move(ref));
  return nullptr;  // parametric parameters
}

// ---------------------------------------------------------------------------------------------
// Python access to the values (ImageEffectInstance.get_param(), set_param(), param_keys())

static void check(OfxStatus st, P::Instance *p, const char *what)
{
  if (st != kOfxStatOK) {
    raiseValue("parameter " + p->getName() + ": " + what + ": status " + nb::cast<std::string>(nb::repr(stat(st))));
  }
}

template <class Inst> static Inst *as(P::Instance *p)
{
  Inst *i = dynamic_cast<Inst *>(p);
  if (!i) {
    raiseValue("parameter " + p->getName() + " of type " + p->getType() + " is not an instance of its HostSupport class");
  }
  return i;
}

/// HostSupport's get() of a parameter, at a time (nullptr: the current time)
template <class Inst, class... V> static void getValues(Inst *i, P::Instance *p, const double *time, V &...v)
{
  check(time ? i->get(*time, v...) : i->get(v...), p, "get");
}

nb::object getParamValue(P::Instance *p, const double *time)
{
  const std::string &t = p->getType();
  if (t == kOfxParamTypeInteger) {
    P::IntegerInstance *i = as<P::IntegerInstance>(p);
    int v = 0;
    getValues(i, p, time, v);
    return nb::int_(v);
  }
  if (t == kOfxParamTypeChoice) {
    P::ChoiceInstance *i = as<P::ChoiceInstance>(p);
    int v = 0;
    getValues(i, p, time, v);
    return nb::int_(v);
  }
  if (t == kOfxParamTypeDouble) {
    P::DoubleInstance *i = as<P::DoubleInstance>(p);
    double v = 0;
    getValues(i, p, time, v);
    return nb::float_(v);
  }
  if (t == kOfxParamTypeBoolean) {
    P::BooleanInstance *i = as<P::BooleanInstance>(p);
    bool v = false;
    getValues(i, p, time, v);
    return nb::bool_(v);
  }
  if (t == kOfxParamTypeStrChoice) {
    P::StrChoiceInstance *i = as<P::StrChoiceInstance>(p);
    std::string v;
    getValues(i, p, time, v);
    return Conv<std::string>::to(v);
  }
  if (t == kOfxParamTypeString || t == kOfxParamTypeCustom) {
    P::StringInstance *i = as<P::StringInstance>(p);
    std::string v;
    getValues(i, p, time, v);
    return Conv<std::string>::to(v);
  }
  if (t == kOfxParamTypeBytes) {
    P::BytesInstance *i = as<P::BytesInstance>(p);
    std::vector<unsigned char> v;
    getValues(i, p, time, v);
    return nb::bytes(v.empty() ? "" : reinterpret_cast<const char *>(v.data()), v.size());
  }
  if (t == kOfxParamTypeRGBA) {
    P::RGBAInstance *i = as<P::RGBAInstance>(p);
    double r = 0, g = 0, b = 0, a = 0;
    getValues(i, p, time, r, g, b, a);
    return nb::make_tuple(r, g, b, a);
  }
  if (t == kOfxParamTypeRGB) {
    P::RGBInstance *i = as<P::RGBInstance>(p);
    double r = 0, g = 0, b = 0;
    getValues(i, p, time, r, g, b);
    return nb::make_tuple(r, g, b);
  }
  if (t == kOfxParamTypeDouble2D) {
    P::Double2DInstance *i = as<P::Double2DInstance>(p);
    double x = 0, y = 0;
    getValues(i, p, time, x, y);
    return nb::make_tuple(x, y);
  }
  if (t == kOfxParamTypeInteger2D) {
    P::Integer2DInstance *i = as<P::Integer2DInstance>(p);
    int x = 0, y = 0;
    getValues(i, p, time, x, y);
    return nb::make_tuple(x, y);
  }
  if (t == kOfxParamTypeDouble3D) {
    P::Double3DInstance *i = as<P::Double3DInstance>(p);
    double x = 0, y = 0, z = 0;
    getValues(i, p, time, x, y, z);
    return nb::make_tuple(x, y, z);
  }
  if (t == kOfxParamTypeInteger3D) {
    P::Integer3DInstance *i = as<P::Integer3DInstance>(p);
    int x = 0, y = 0, z = 0;
    getValues(i, p, time, x, y, z);
    return nb::make_tuple(x, y, z);
  }
  return nb::none();  // push buttons, groups, pages: no value
}

template <class T> static std::vector<T> values(nb::handle v, size_t n, P::Instance *p)
{
  std::vector<T> r;
  if (n == 1 && !nb::isinstance<nb::tuple>(v) && !nb::isinstance<nb::list>(v)) {
    r.push_back(Conv<T>::from(v));
    return r;
  }
  for (nb::handle x : v) {
    r.push_back(Conv<T>::from(x));
  }
  if (r.size() != n) {
    raiseValue("parameter " + p->getName() + " takes " + std::to_string(n) + " values");
  }
  return r;
}

/// the index of a choice: an int, or the label of one of its options (kOfxParamPropChoiceOption)
static int choiceIndex(P::Instance *p, nb::handle value)
{
  if (!nb::isinstance<nb::str>(value)) {
    return nb::cast<int>(value);
  }
  const std::string label = nb::cast<std::string>(value);
  const OH::Property::Set &props = p->getProperties();
  const int n = props.fetchProperty(kOfxParamPropChoiceOption) ? props.getDimension(kOfxParamPropChoiceOption) : 0;
  std::string options;
  for (int k = 0; k < n; ++k) {
    const std::string o = props.getStringProperty(kOfxParamPropChoiceOption, k);
    if (o == label) {
      return k;
    }
    options += (k ? ", \"" : "\"") + o + "\"";
  }
  raiseValue("parameter " + p->getName() + ": no option \"" + label + "\" (its options: " +
             (n ? options : std::string("none")) + ")");
}

/// HostSupport's set() of a parameter, at a time (nullptr: no time)
template <class Inst, class... V> static OfxStatus setValues(Inst *i, const double *time, V... v)
{
  return time ? i->set(*time, v...) : i->set(v...);
}

OfxStatus setParamValue(P::Instance *p, nb::handle value, const double *time)
{
  const std::string &t = p->getType();
  if (t == kOfxParamTypeInteger) {
    P::IntegerInstance *i = as<P::IntegerInstance>(p);
    return setValues(i, time, nb::cast<int>(value));
  }
  if (t == kOfxParamTypeChoice) {
    P::ChoiceInstance *i = as<P::ChoiceInstance>(p);
    return setValues(i, time, choiceIndex(p, value));  // (an index, or the label of an option)
  }
  if (t == kOfxParamTypeDouble) {
    P::DoubleInstance *i = as<P::DoubleInstance>(p);
    return setValues(i, time, nb::cast<double>(value));
  }
  if (t == kOfxParamTypeBoolean) {
    P::BooleanInstance *i = as<P::BooleanInstance>(p);
    return setValues(i, time, Conv<bool>::from(value));
  }
  if (t == kOfxParamTypeStrChoice) {
    P::StrChoiceInstance *i = as<P::StrChoiceInstance>(p);
    const std::string s = nb::cast<std::string>(value);
    return setValues(i, time, s.c_str());
  }
  if (t == kOfxParamTypeString || t == kOfxParamTypeCustom) {
    P::StringInstance *i = as<P::StringInstance>(p);
    const std::string s = nb::cast<std::string>(value);
    return setValues(i, time, s.c_str());
  }
  if (t == kOfxParamTypeBytes) {
    P::BytesInstance *i = as<P::BytesInstance>(p);
    const std::vector<unsigned char> b = bytesFrom(value);  // (a list of ints too: param_template())
    return setValues(i, time, b.empty() ? reinterpret_cast<const unsigned char *>("") : b.data(), b.size());
  }
  if (t == kOfxParamTypeRGBA) {
    P::RGBAInstance *i = as<P::RGBAInstance>(p);
    const std::vector<double> v = values<double>(value, 4, p);
    return setValues(i, time, v[0], v[1], v[2], v[3]);
  }
  if (t == kOfxParamTypeRGB) {
    P::RGBInstance *i = as<P::RGBInstance>(p);
    const std::vector<double> v = values<double>(value, 3, p);
    return setValues(i, time, v[0], v[1], v[2]);
  }
  if (t == kOfxParamTypeDouble2D) {
    P::Double2DInstance *i = as<P::Double2DInstance>(p);
    const std::vector<double> v = values<double>(value, 2, p);
    return setValues(i, time, v[0], v[1]);
  }
  if (t == kOfxParamTypeInteger2D) {
    P::Integer2DInstance *i = as<P::Integer2DInstance>(p);
    const std::vector<int> v = values<int>(value, 2, p);
    return setValues(i, time, v[0], v[1]);
  }
  if (t == kOfxParamTypeDouble3D) {
    P::Double3DInstance *i = as<P::Double3DInstance>(p);
    const std::vector<double> v = values<double>(value, 3, p);
    return setValues(i, time, v[0], v[1], v[2]);
  }
  if (t == kOfxParamTypeInteger3D) {
    P::Integer3DInstance *i = as<P::Integer3DInstance>(p);
    const std::vector<int> v = values<int>(value, 3, p);
    return setValues(i, time, v[0], v[1], v[2]);
  }
  raiseValue("parameter " + p->getName() + " of type " + shortName("param_type", t) + " has no value");
}

std::vector<double> paramKeyTimes(P::Instance *p)
{
  std::vector<double> r;
  P::KeyframeParam *k = dynamic_cast<P::KeyframeParam *>(p);
  if (!k) {
    return r;
  }
  unsigned int n = 0;
  check(k->getNumKeys(n), p, "getNumKeys");
  for (unsigned int i = 0; i < n; ++i) {
    OfxTime t = 0.;
    check(k->getKeyTime(static_cast<int>(i), t), p, "getKeyTime");
    r.push_back(t);
  }
  return r;
}

// ---------------------------------------------------------------------------------------------
// ofxh.Param

/// HostSupport's derive() and integrate() of a parameter type (the default of ofxh.Param): its
/// status, which the plug-in gets (they are errors: HostSupport has no approximation)
static OfxStatus hostSupportDeriveStatus(const std::string &type)
{
  if (type == kOfxParamTypeInteger) {
    return kOfxStatErrUnsupported;  // (IntegerInstance::derive, integrate)
  }
  // RGBA, RGB, 2D, 3D: kOfxStatErrMissingHostFeature; DoubleInstance has no default: the same
  return kOfxStatErrMissingHostFeature;
}

nb::object initialValue(P::Instance *p, const PyEffectInstance *effect)
{
  nb::object v = defaultValue(p->getProperties(), p->getType());
  const std::string &t = p->getType();
  if (effect && (t == kOfxParamTypeDouble || t == kOfxParamTypeDouble2D)) {
    const int n = t == kOfxParamTypeDouble ? 1 : 2;
    double dv[2];
    for (int k = 0; k < n; ++k) {
      dv[k] = p->getProperties().getDoubleProperty(kOfxParamPropDefault, k);
    }
    double w, h, ox, oy;
    effect->getProjectSize(w, h);
    effect->getProjectOffset(ox, oy);
    defaultToCanonical(*p, w, h, ox, oy, dv, n);
    v = n == 1 ? nb::object(nb::float_(dv[0])) : nb::object(nb::make_tuple(dv[0], dv[1]));
  }
  return v;
}

void bindParams(nb::module_ &m)
{
  nb::class_<ParamBase> c(m, "Param",
    "A parameter whose value Python holds. Subclass it, and return one from\n"
    "ImageEffectInstance.new_param(descriptor) (None: ofxh's C++ parameter, which stores its value\n"
    "and keys). The adapter of the descriptor's type calls:\n"
    "- get(time): the value at time, which is always a time: the current one (the instance's\n"
    "  timeline_get_time()) when the plug-in called paramGetValue without one. The value is an int\n"
    "  (integer, choice), float (double), bool, str (string, custom, string choice), bytes, or a\n"
    "  tuple for colours, 2D and 3D parameters. Required.\n"
    "- set(value, time=None): time None (paramSetValue): set the value, or the key at the current\n"
    "  time if the parameter has keys; at a time (paramSetValueAtTime): a key if the parameter\n"
    "  animates, else the value. Required. Return None or a Stat.\n"
    "- derive(time), integrate(t1, t2): the defaults raise OfxError with HostSupport's status\n"
    "  (Stat.ErrUnsupported for integers, Stat.ErrMissingHostFeature for doubles).\n"
    "- num_keys(), key_time(nth), key_index(time, direction) (-1 or None: no key), delete_key(time),\n"
    "  delete_all_keys(): the defaults have no keys.\n"
    "- changed(prop_name): a property of the parameter changed (kOfxParamPropEnabled,\n"
    "  kOfxParamPropSecret, kOfxParamPropLabel...), e.g. the plug-in's setEnabled(); default: nothing.\n"
    "Raise OfxError(status) to return a status to the plug-in. The calls happen during the actions\n"
    "(with the GIL taken again), and from get_param()/set_param(), maybe on a plug-in's worker\n"
    "thread (paramGetValue from a render thread). A Python string choice checks its values in\n"
    "set() itself (ofxh doesn't check them against the enums). Push buttons, groups and pages\n"
    "have no value: only changed() (and the keyframe methods for push buttons) are called.",
    nb::dynamic_attr());
  c.def(nb::init<>())
    .def("get", [](ParamBase &, double) -> nb::object {
      raiseError(PyExc_NotImplementedError, "ofxh.Param.get(time): subclasses implement it");
    }, "time"_a)
    .def("set", [](ParamBase &, nb::handle, nb::handle) -> nb::object {
      raiseError(PyExc_NotImplementedError, "ofxh.Param.set(value, time=None): subclasses implement it");
    }, "value"_a.none(), "time"_a.none() = nb::none())
    .def("derive", [](ParamBase &s, double) -> nb::object {
      raiseOfxError(hostSupportDeriveStatus(s.get()->getType()));
    }, "time"_a)
    .def("integrate", [](ParamBase &s, double, double) -> nb::object {
      raiseOfxError(hostSupportDeriveStatus(s.get()->getType()));
    }, "t1"_a, "t2"_a)
    .def("num_keys", [](ParamBase &) { return 0; })
    .def("key_time", [](ParamBase &, int) -> double { raiseOfxError(kOfxStatErrBadIndex); }, "nth"_a)
    .def("key_index", [](ParamBase &, double, int) { return -1; }, "time"_a, "direction"_a,
         "The index of the key at time (direction 0), the next one (> 0) or the previous one (< 0); -1: none.")
    .def("delete_key", [](ParamBase &, double) -> nb::object { raiseOfxError(kOfxStatErrBadIndex); }, "time"_a)
    .def("delete_all_keys", [](ParamBase &) { return nb::none(); })
    .def("changed", [](ParamBase &, const std::string &) {}, "prop_name"_a)
    // --- information
    .def_prop_ro("alive", [](ParamBase &s) { checkThread("ofxh", false); return s.ref->p != nullptr; })
    .def_prop_ro("name", [](ParamBase &s) { return s.get()->getName(); })
    .def_prop_ro("type", [](ParamBase &s) { return shortName("param_type", s.get()->getType()); },
                 "integer, double, boolean, choice, strchoice, string, custom, bytes, rgba, rgb, double2d...")
    .def_prop_ro("label", [](ParamBase &s) { return s.get()->getLabel(); })
    .def_prop_ro("animates", [](ParamBase &s) { return s.get()->getCanAnimate(); },
                 "kOfxParamPropAnimates: may set(value, time) make keys?")
    .def_prop_ro("enabled", [](ParamBase &s) { return s.get()->getEnabled(); })
    .def_prop_ro("secret", [](ParamBase &s) { return s.get()->getSecret(); })
    .def_prop_ro("default", [](ParamBase &s) {
      P::Instance *p = s.get();
      return defaultValue(p->getProperties(), p->getType());
    }, "The default (kOfxParamPropDefault, as the plug-in set it: normalised spatial defaults stay\n"
       "normalised), in the form of get(); None for the parameters without a value. See initial.")
    .def_prop_ro("initial", [](ParamBase &s) {
      PyParamCore *c = s.core();
      P::Instance *p = s.get();
      return scoped("Param.initial", [&] { return initialValue(p, c ? c->_effect : nullptr); });
    }, "The value a parameter starts with: the default, with the normalised spatial defaults\n"
       "(kOfxParamPropDefaultCoordinateSystem) in canonical coordinates for the instance's project, as\n"
       "ofxh's C++ parameters and param_template() have them. Return it from get() until set() (also\n"
       "from a plug-in's worker thread inside the get() callback: a non-action binding).")
    .def_prop_ro("props", [](ParamBase &s) { s.get(); return PropertySetView{s.ref, PropertySetView::Kind::param}; },
                 "The properties of the parameter instance (kOfxParamProp*), which the plug-in may change.")
    .def_prop_ro("instance", [](ParamBase &s) -> nb::object {
      PyParamCore *c = s.core();
      return c ? c->_effect->_py : nb::none();
    }, "The ofxh.ImageEffectInstance of the parameter.")
    .def("__repr__", [](ParamBase &s) {
      if (!s.ref->p) {
        return std::string("<ofxh.Param (deleted)>");
      }
      return "<ofxh.Param " + s.get()->getName() + " (" + shortName("param_type", s.get()->getType()) + ")>";
    });
}

}  // namespace ofxh
