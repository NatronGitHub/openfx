// Python bindings of HostSupport: parameters.
//
// Part 1: each parameter is a C++ instance that stores its value (no keys: paramSetValueAtTime
// sets the value, paramGetValueAtTime returns it, the derivative is 0 and the integral is
// value * (t2 - t1)). The value starts at the descriptor's default (kOfxParamPropDefault, with
// normalised spatial defaults converted to canonical coordinates, as hostDemo does). Python
// reads and writes the values with ImageEffectInstance.get_param() and set_param().

#include "ofxhPython.h"

#include <nanobind/stl/string.h>

#include <vector>

namespace ofxh {

namespace P = OFX::Host::Param;

// Spatial double params may give their default in normalised coordinates
// (kOfxParamPropDefaultCoordinateSystem, OFX 1.2): convert it to canonical coordinates, using the
// project size, plus the project offset for positions (as hostDemoParamInstance.cpp)
static void defaultToCanonical(const P::Descriptor &descriptor, const PyEffectInstance *effect, double *v, int n)
{
  if (!effect || descriptor.getDefaultCoordinateSystem() != kOfxParamCoordinatesNormalised) {
    return;
  }
  const std::string &type = descriptor.getDoubleType();
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

template <class T, int N> static void defaultValue(const P::Descriptor &d, const PyEffectInstance *e, T *v)
{
  const OH::Property::Set &props = d.getProperties();
  double dv[4];
  for (int k = 0; k < N; ++k) {
    dv[k] = props.getDoubleProperty(kOfxParamPropDefault, k);
  }
  defaultToCanonical(d, e, dv, N);
  for (int k = 0; k < N; ++k) {
    v[k] = (T)dv[k];
  }
}

static void intDefault(const P::Descriptor &d, int *v, int n)
{
  for (int k = 0; k < n; ++k) {
    v[k] = d.getProperties().getIntProperty(kOfxParamPropDefault, k);
  }
}

// the stored-value parameters: get(), set() with and without a time

class StoredInteger : public P::IntegerInstance {
  int _v;
public:
  StoredInteger(PyEffectInstance *e, P::Descriptor &d) : P::IntegerInstance(d, e) { intDefault(d, &_v, 1); }
  virtual OfxStatus get(int &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, int &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(int v) { _v = v; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, int v) { _v = v; return kOfxStatOK; }
};

class StoredDouble : public P::DoubleInstance {
  double _v;
public:
  StoredDouble(PyEffectInstance *e, P::Descriptor &d) : P::DoubleInstance(d, e) { defaultValue<double, 1>(d, e, &_v); }
  virtual OfxStatus get(double &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, double &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(double v) { _v = v; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, double v) { _v = v; return kOfxStatOK; }
  virtual OfxStatus derive(OfxTime, double &v) { v = 0.; return kOfxStatOK; }
  virtual OfxStatus integrate(OfxTime t1, OfxTime t2, double &v) { v = _v * (t2 - t1); return kOfxStatOK; }
};

class StoredBoolean : public P::BooleanInstance {
  bool _v;
public:
  StoredBoolean(PyEffectInstance *e, P::Descriptor &d) : P::BooleanInstance(d, e)
  {
    int v;
    intDefault(d, &v, 1);
    _v = v != 0;
  }
  virtual OfxStatus get(bool &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, bool &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(bool v) { _v = v; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, bool v) { _v = v; return kOfxStatOK; }
};

class StoredChoice : public P::ChoiceInstance {
  int _v;
public:
  StoredChoice(PyEffectInstance *e, P::Descriptor &d) : P::ChoiceInstance(d, e) { intDefault(d, &_v, 1); }
  virtual OfxStatus get(int &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, int &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(int v) { _v = v; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, int v) { _v = v; return kOfxStatOK; }
};

class StoredStrChoice : public P::StrChoiceInstance {
  std::string _v;
public:
  StoredStrChoice(PyEffectInstance *e, P::Descriptor &d) : P::StrChoiceInstance(d, e) { _v = getDefaultEnum(); }
  virtual OfxStatus get(std::string &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, std::string &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(const char *v) { _v = v ? v : ""; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, const char *v) { _v = v ? v : ""; return kOfxStatOK; }
};

class StoredString : public P::StringInstance {
  std::string _v;
public:
  StoredString(PyEffectInstance *e, P::Descriptor &d) : P::StringInstance(d, e)
  {
    _v = d.getProperties().getStringProperty(kOfxParamPropDefault);
  }
  virtual OfxStatus get(std::string &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, std::string &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(const char *v) { _v = v ? v : ""; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, const char *v) { _v = v ? v : ""; return kOfxStatOK; }
};

class StoredCustom : public P::CustomInstance {
  std::string _v;
public:
  StoredCustom(PyEffectInstance *e, P::Descriptor &d) : P::CustomInstance(d, e)
  {
    _v = d.getProperties().getStringProperty(kOfxParamPropDefault);
  }
  virtual OfxStatus get(std::string &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, std::string &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(const char *v) { _v = v ? v : ""; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, const char *v) { _v = v ? v : ""; return kOfxStatOK; }
};

class StoredBytes : public P::BytesInstance {
  std::vector<unsigned char> _v;
public:
  StoredBytes(PyEffectInstance *e, P::Descriptor &d) : P::BytesInstance(d, e), _v(getDefault()) {}
  virtual OfxStatus get(std::vector<unsigned char> &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, std::vector<unsigned char> &v) { v = _v; return kOfxStatOK; }
  virtual OfxStatus set(const unsigned char *data, size_t length)
  {
    _v.assign(data, data + (data ? length : 0));
    return kOfxStatOK;
  }
  virtual OfxStatus set(OfxTime, const unsigned char *data, size_t length) { return set(data, length); }
};

// (the get/set with a time forward to the ones without: no keys in part 1)
class StoredRGBA : public P::RGBAInstance {
  double _v[4];
public:
  StoredRGBA(PyEffectInstance *e, P::Descriptor &d) : P::RGBAInstance(d, e) { defaultValue<double, 4>(d, e, _v); }
  virtual OfxStatus get(double &r, double &g, double &b, double &a) { r = _v[0]; g = _v[1]; b = _v[2]; a = _v[3]; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, double &r, double &g, double &b, double &a) { return get(r, g, b, a); }
  virtual OfxStatus set(double r, double g, double b, double a) { _v[0] = r; _v[1] = g; _v[2] = b; _v[3] = a; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, double r, double g, double b, double a) { return set(r, g, b, a); }
};

class StoredRGB : public P::RGBInstance {
  double _v[3];
public:
  StoredRGB(PyEffectInstance *e, P::Descriptor &d) : P::RGBInstance(d, e) { defaultValue<double, 3>(d, e, _v); }
  virtual OfxStatus get(double &r, double &g, double &b) { r = _v[0]; g = _v[1]; b = _v[2]; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, double &r, double &g, double &b) { return get(r, g, b); }
  virtual OfxStatus set(double r, double g, double b) { _v[0] = r; _v[1] = g; _v[2] = b; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, double r, double g, double b) { return set(r, g, b); }
};

class StoredDouble2D : public P::Double2DInstance {
  double _v[2];
public:
  StoredDouble2D(PyEffectInstance *e, P::Descriptor &d) : P::Double2DInstance(d, e) { defaultValue<double, 2>(d, e, _v); }
  virtual OfxStatus get(double &x, double &y) { x = _v[0]; y = _v[1]; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, double &x, double &y) { return get(x, y); }
  virtual OfxStatus set(double x, double y) { _v[0] = x; _v[1] = y; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, double x, double y) { return set(x, y); }
};

class StoredInteger2D : public P::Integer2DInstance {
  int _v[2];
public:
  StoredInteger2D(PyEffectInstance *e, P::Descriptor &d) : P::Integer2DInstance(d, e) { intDefault(d, _v, 2); }
  virtual OfxStatus get(int &x, int &y) { x = _v[0]; y = _v[1]; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, int &x, int &y) { return get(x, y); }
  virtual OfxStatus set(int x, int y) { _v[0] = x; _v[1] = y; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, int x, int y) { return set(x, y); }
};

class StoredDouble3D : public P::Double3DInstance {
  double _v[3];
public:
  StoredDouble3D(PyEffectInstance *e, P::Descriptor &d) : P::Double3DInstance(d, e) { defaultValue<double, 3>(d, e, _v); }
  virtual OfxStatus get(double &x, double &y, double &z) { x = _v[0]; y = _v[1]; z = _v[2]; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, double &x, double &y, double &z) { return get(x, y, z); }
  virtual OfxStatus set(double x, double y, double z) { _v[0] = x; _v[1] = y; _v[2] = z; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, double x, double y, double z) { return set(x, y, z); }
};

class StoredInteger3D : public P::Integer3DInstance {
  int _v[3];
public:
  StoredInteger3D(PyEffectInstance *e, P::Descriptor &d) : P::Integer3DInstance(d, e) { intDefault(d, _v, 3); }
  virtual OfxStatus get(int &x, int &y, int &z) { x = _v[0]; y = _v[1]; z = _v[2]; return kOfxStatOK; }
  virtual OfxStatus get(OfxTime, int &x, int &y, int &z) { return get(x, y, z); }
  virtual OfxStatus set(int x, int y, int z) { _v[0] = x; _v[1] = y; _v[2] = z; return kOfxStatOK; }
  virtual OfxStatus set(OfxTime, int x, int y, int z) { return set(x, y, z); }
};

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
  return NULL;  // e.g. parametric parameters (not in part 1): the instance can't be created
}

// ---------------------------------------------------------------------------------------------
// Python access

[[noreturn]] static void raiseValue(const std::string &message)
{
  PyErr_SetString(PyExc_ValueError, message.c_str());
  throw nb::python_error();
}

static void check(OfxStatus st, P::Instance *p)
{
  if (st != kOfxStatOK) {
    raiseValue("parameter " + p->getName() + ": status " + std::to_string(st));
  }
}

template <class Inst> static Inst *as(P::Instance *p)
{
  Inst *i = dynamic_cast<Inst *>(p);
  if (!i) {
    raiseValue("parameter " + p->getName() + " of type " + p->getType() + " is not a part 1 stored parameter");
  }
  return i;
}

nb::object getParamValue(P::Instance *p, const double *time)
{
  const std::string &t = p->getType();
#define OFXH_GET(...) check(time ? i->get(*time, __VA_ARGS__) : i->get(__VA_ARGS__), p)
  if (t == kOfxParamTypeInteger) {
    P::IntegerInstance *i = as<P::IntegerInstance>(p);
    int v = 0;
    OFXH_GET(v);
    return nb::int_(v);
  }
  if (t == kOfxParamTypeChoice) {
    P::ChoiceInstance *i = as<P::ChoiceInstance>(p);
    int v = 0;
    OFXH_GET(v);
    return nb::int_(v);
  }
  if (t == kOfxParamTypeDouble) {
    P::DoubleInstance *i = as<P::DoubleInstance>(p);
    double v = 0;
    OFXH_GET(v);
    return nb::float_(v);
  }
  if (t == kOfxParamTypeBoolean) {
    P::BooleanInstance *i = as<P::BooleanInstance>(p);
    bool v = false;
    OFXH_GET(v);
    return nb::bool_(v);
  }
  if (t == kOfxParamTypeStrChoice) {
    P::StrChoiceInstance *i = as<P::StrChoiceInstance>(p);
    std::string v;
    OFXH_GET(v);
    return nb::str(v.c_str());
  }
  if (t == kOfxParamTypeString || t == kOfxParamTypeCustom) {
    P::StringInstance *i = as<P::StringInstance>(p);
    std::string v;
    OFXH_GET(v);
    return nb::str(v.c_str());
  }
  if (t == kOfxParamTypeBytes) {
    P::BytesInstance *i = as<P::BytesInstance>(p);
    std::vector<unsigned char> v;
    OFXH_GET(v);
    return nb::bytes(v.empty() ? "" : (const char *)&v[0], v.size());
  }
  if (t == kOfxParamTypeRGBA) {
    P::RGBAInstance *i = as<P::RGBAInstance>(p);
    double r = 0, g = 0, b = 0, a = 0;
    OFXH_GET(r, g, b, a);
    return nb::make_tuple(r, g, b, a);
  }
  if (t == kOfxParamTypeRGB) {
    P::RGBInstance *i = as<P::RGBInstance>(p);
    double r = 0, g = 0, b = 0;
    OFXH_GET(r, g, b);
    return nb::make_tuple(r, g, b);
  }
  if (t == kOfxParamTypeDouble2D) {
    P::Double2DInstance *i = as<P::Double2DInstance>(p);
    double x = 0, y = 0;
    OFXH_GET(x, y);
    return nb::make_tuple(x, y);
  }
  if (t == kOfxParamTypeInteger2D) {
    P::Integer2DInstance *i = as<P::Integer2DInstance>(p);
    int x = 0, y = 0;
    OFXH_GET(x, y);
    return nb::make_tuple(x, y);
  }
  if (t == kOfxParamTypeDouble3D) {
    P::Double3DInstance *i = as<P::Double3DInstance>(p);
    double x = 0, y = 0, z = 0;
    OFXH_GET(x, y, z);
    return nb::make_tuple(x, y, z);
  }
  if (t == kOfxParamTypeInteger3D) {
    P::Integer3DInstance *i = as<P::Integer3DInstance>(p);
    int x = 0, y = 0, z = 0;
    OFXH_GET(x, y, z);
    return nb::make_tuple(x, y, z);
  }
#undef OFXH_GET
  return nb::none();  // push buttons, groups, pages: no value
}

template <class T> static std::vector<T> values(nb::handle v, size_t n, P::Instance *p)
{
  std::vector<T> r;
  if (n == 1 && !nb::isinstance<nb::tuple>(v) && !nb::isinstance<nb::list>(v)) {
    r.push_back(nb::cast<T>(v));
    return r;
  }
  for (nb::handle x : v) {
    r.push_back(nb::cast<T>(x));
  }
  if (r.size() != n) {
    raiseValue("parameter " + p->getName() + " takes " + std::to_string(n) + " values");
  }
  return r;
}

OfxStatus setParamValue(P::Instance *p, nb::handle value, const double *time)
{
  const std::string &t = p->getType();
#define OFXH_SET(...) return time ? i->set(*time, __VA_ARGS__) : i->set(__VA_ARGS__)
  if (t == kOfxParamTypeInteger) {
    P::IntegerInstance *i = as<P::IntegerInstance>(p);
    OFXH_SET(nb::cast<int>(value));
  }
  if (t == kOfxParamTypeChoice) {
    P::ChoiceInstance *i = as<P::ChoiceInstance>(p);
    OFXH_SET(nb::cast<int>(value));
  }
  if (t == kOfxParamTypeDouble) {
    P::DoubleInstance *i = as<P::DoubleInstance>(p);
    OFXH_SET(nb::cast<double>(value));
  }
  if (t == kOfxParamTypeBoolean) {
    P::BooleanInstance *i = as<P::BooleanInstance>(p);
    OFXH_SET(nb::cast<bool>(value));
  }
  if (t == kOfxParamTypeStrChoice) {
    P::StrChoiceInstance *i = as<P::StrChoiceInstance>(p);
    const std::string s = nb::cast<std::string>(value);
    OFXH_SET(s.c_str());
  }
  if (t == kOfxParamTypeString || t == kOfxParamTypeCustom) {
    P::StringInstance *i = as<P::StringInstance>(p);
    const std::string s = nb::cast<std::string>(value);
    OFXH_SET(s.c_str());
  }
  if (t == kOfxParamTypeBytes) {
    P::BytesInstance *i = as<P::BytesInstance>(p);
    const nb::bytes b = nb::cast<nb::bytes>(value);
    OFXH_SET((const unsigned char *)b.c_str(), b.size());
  }
  if (t == kOfxParamTypeRGBA) {
    P::RGBAInstance *i = as<P::RGBAInstance>(p);
    const std::vector<double> v = values<double>(value, 4, p);
    OFXH_SET(v[0], v[1], v[2], v[3]);
  }
  if (t == kOfxParamTypeRGB) {
    P::RGBInstance *i = as<P::RGBInstance>(p);
    const std::vector<double> v = values<double>(value, 3, p);
    OFXH_SET(v[0], v[1], v[2]);
  }
  if (t == kOfxParamTypeDouble2D) {
    P::Double2DInstance *i = as<P::Double2DInstance>(p);
    const std::vector<double> v = values<double>(value, 2, p);
    OFXH_SET(v[0], v[1]);
  }
  if (t == kOfxParamTypeInteger2D) {
    P::Integer2DInstance *i = as<P::Integer2DInstance>(p);
    const std::vector<int> v = values<int>(value, 2, p);
    OFXH_SET(v[0], v[1]);
  }
  if (t == kOfxParamTypeDouble3D) {
    P::Double3DInstance *i = as<P::Double3DInstance>(p);
    const std::vector<double> v = values<double>(value, 3, p);
    OFXH_SET(v[0], v[1], v[2]);
  }
  if (t == kOfxParamTypeInteger3D) {
    P::Integer3DInstance *i = as<P::Integer3DInstance>(p);
    const std::vector<int> v = values<int>(value, 3, p);
    OFXH_SET(v[0], v[1], v[2]);
  }
#undef OFXH_SET
  raiseValue("parameter " + p->getName() + " of type " + t + " has no value");
}

}  // namespace ofxh
