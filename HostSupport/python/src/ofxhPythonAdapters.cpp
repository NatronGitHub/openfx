// Python bindings of HostSupport: the adapters (see ofxhPython.h)

#include "ofxhPython.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/pair.h>

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "ofxMessage.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"  // kFnOfxImagePlaneColour
#endif

namespace ofxh {

// ---------------------------------------------------------------------------------------------
// References and errors

void raiseDeleted(const char *what)
{
  PyErr_Format(PyExc_ReferenceError, "%s: deleted by HostSupport (or not created yet)", what);
  throw nb::python_error();
}

[[noreturn]] static void raise(PyObject *type, const std::string &message)
{
  PyErr_SetString(type, message.c_str());
  throw nb::python_error();
}

PyHost *HostBase::get() const
{
  if (!ref->p) {
    raiseDeleted("ofxh.Host");
  }
  return static_cast<PyHost *>(ref->p);
}

PyEffectInstance *InstanceBase::get() const
{
  if (!ref->p) {
    raiseDeleted("ofxh.ImageEffectInstance");
  }
  return static_cast<PyEffectInstance *>(ref->p);
}

PyClipInstance *ClipBase::get() const
{
  if (!ref->p) {
    raiseDeleted("ofxh.ClipInstance");
  }
  return static_cast<PyClipInstance *>(ref->p);
}

Globals &globals()
{
  // never deleted: Python objects must not be released after the interpreter is finalized
  static Globals *g = new Globals;
  return *g;
}

nb::object stat(OfxStatus s) { return globals().statFn(s); }

/// the first error saved by the callbacks since the innermost ActionScope (heap-allocated, see
/// globals())
static nb::object &pendingError()
{
  static nb::object *e = new nb::object;
  return *e;
}

void saveError(nb::handle exc)
{
  if (!pendingError().is_valid()) {
    pendingError() = nb::borrow(exc);
  }
}

void saveError(const char *message)
{
  if (!pendingError().is_valid()) {
    pendingError() = nb::steal(PyObject_CallFunction(PyExc_RuntimeError, "s", message));
    if (!pendingError().is_valid()) {
      PyErr_Clear();
    }
  }
}

ActionScope::ActionScope()
  : _saved(std::move(pendingError()))
  , _done(false)
{
  pendingError() = nb::object();
}

ActionScope::~ActionScope() { discard(); }

void ActionScope::discard()
{
  if (!_done) {
    _done = true;
    pendingError() = std::move(_saved);
  }
}

bool ActionScope::failed() const { return !_done && pendingError().is_valid(); }

void ActionScope::check(const char *where, const OfxStatus *status)
{
  nb::object e = std::move(pendingError());
  discard();
  if (e.is_valid()) {
    // raises (python_error)
    globals().raiseCallbackError(where, e, status ? stat(*status) : nb::none());
  }
}

OfxStatus onPythonError(nb::python_error &e, StatusFallback fb)
{
  if (globals().ofxError.is_valid() && e.matches(globals().ofxError)) {
    // ofxh.OfxError(status): a deliberate status, not an error
    int st = fb.status;
    if (!nb::try_cast(e.value().attr("status"), st)) {
      saveError(e.value());
      return fb.status;
    }
    return st;
  }
  saveError(e.value());
  return fb.status;
}

void onPythonError(nb::python_error &e, Void) { saveError(e.value()); }

OfxStatus statusFromResult(nb::handle r)
{
  if (r.is_none() || PyBool_Check(r.ptr()) || !PyLong_Check(r.ptr())) {
    return kOfxStatOK;
  }
  return nb::cast<int>(r);
}

std::string formatMessage(const char *format, va_list args)
{
  if (!format) {
    return std::string();
  }
  std::vector<char> buf(65536);
  va_list copy;
  va_copy(copy, args);
  const int n = vsnprintf(&buf[0], buf.size(), format, copy);
  va_end(copy);
  std::string text(n > 0 ? &buf[0] : "");
  if (n < 0 || n >= (int)buf.size()) {
    text += "...";
  }
  return text;
}

// ---------------------------------------------------------------------------------------------
// Names

static std::string lower(const std::string &s)
{
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i] >= 'A' && r[i] <= 'Z') {
      r[i] = (char)(r[i] - 'A' + 'a');
    }
  }
  return r;
}

std::string depthName(const std::string &s)
{
  const std::string l = lower(s);
  if (l == "byte" || l == "uint8") return kOfxBitDepthByte;
  if (l == "short" || l == "uint16") return kOfxBitDepthShort;
  if (l == "half" || l == "float16") return kOfxBitDepthHalf;
  if (l == "float" || l == "float32") return kOfxBitDepthFloat;
  if (l == "none") return kOfxBitDepthNone;
  return s;
}

std::string componentsName(const std::string &s)
{
  const std::string l = lower(s);
  if (l == "rgba") return kOfxImageComponentRGBA;
  if (l == "rgb") return kOfxImageComponentRGB;
  if (l == "alpha" || l == "a") return kOfxImageComponentAlpha;
  if (l == "none") return kOfxImageComponentNone;
  return s;
}

std::string premultName(const std::string &s)
{
  const std::string l = lower(s);
  if (l == "premult" || l == "premultiplied") return kOfxImagePreMultiplied;
  if (l == "unpremult" || l == "unpremultiplied") return kOfxImageUnPreMultiplied;
  if (l == "opaque") return kOfxImageOpaque;
  return s;
}

std::string fieldName(const std::string &s)
{
  const std::string l = lower(s);
  if (l == "none" || l.empty()) return kOfxImageFieldNone;
  if (l == "both") return kOfxImageFieldBoth;
  if (l == "lower") return kOfxImageFieldLower;
  if (l == "upper") return kOfxImageFieldUpper;
  return s;
}

std::string changeReason(const std::string &s)
{
  const std::string l = lower(s);
  if (l == "user" || l == "user_edited") return kOfxChangeUserEdited;
  if (l == "plugin" || l == "plugin_edited") return kOfxChangePluginEdited;
  if (l == "time") return kOfxChangeTime;
  return s;
}

int componentCount(const std::string &components)
{
  if (components == kOfxImageComponentRGBA) return 4;
  if (components == kOfxImageComponentRGB) return 3;
  if (components == kOfxImageComponentAlpha) return 1;
  return 0;
}

static int clampToInt(double v)
{
  if (std::isnan(v)) return 0;
  if (v <= (double)INT_MIN) return INT_MIN;
  if (v >= (double)INT_MAX) return INT_MAX;
  return (int)v;
}

OfxRectI canonicalToPixelEnclosing(const OfxRectD &r, const OfxPointD &renderScale, double par)
{
  // as hostDemo: the epsilon absorbs rounding errors, e.g. 768 / (768/720) = 720.0000000001
  const double eps = 1e-6;
  OfxRectI p;
  p.x1 = clampToInt(std::floor(r.x1 * renderScale.x / par + eps));
  p.y1 = clampToInt(std::floor(r.y1 * renderScale.y + eps));
  p.x2 = clampToInt(std::ceil(r.x2 * renderScale.x / par - eps));
  p.y2 = clampToInt(std::ceil(r.y2 * renderScale.y - eps));
  return p;
}

OfxRectI intersect(const OfxRectI &a, const OfxRectI &b)
{
  OfxRectI r;
  r.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
  r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
  r.x2 = a.x2 < b.x2 ? a.x2 : b.x2;
  r.y2 = a.y2 < b.y2 ? a.y2 : b.y2;
  return r;
}

// ---------------------------------------------------------------------------------------------
// Calls into Python (each one is a whole OFXH_CALL: they are used from const functions)

static OfxRectD emptyRectD()
{
  OfxRectD r;
  r.x1 = r.y1 = r.x2 = r.y2 = 0.;
  return r;
}

static OfxRectD toRectD(nb::handle h)
{
  const nb::tuple t = nb::tuple(h);
  if (t.size() != 4) {
    raise(PyExc_ValueError, "a rectangle is (x1, y1, x2, y2)");
  }
  OfxRectD r;
  r.x1 = nb::cast<double>(t[0]);
  r.y1 = nb::cast<double>(t[1]);
  r.x2 = nb::cast<double>(t[2]);
  r.y2 = nb::cast<double>(t[3]);
  return r;
}

static OfxRectI toRectI(nb::handle h, const char *what)
{
  const nb::tuple t = nb::tuple(h);
  if (t.size() != 4) {
    raise(PyExc_ValueError, std::string(what) + " is (x1, y1, x2, y2)");
  }
  OfxRectI r;
  r.x1 = nb::cast<int>(t[0]);
  r.y1 = nb::cast<int>(t[1]);
  r.x2 = nb::cast<int>(t[2]);
  r.y2 = nb::cast<int>(t[3]);
  return r;
}

static std::string attrString(const nb::object &o, const char *name, const char *fallback)
{
  OFXH_CALL(std::string(fallback), return nb::cast<std::string>(o.attr(name));)
}

static double attrDouble(const nb::object &o, const char *name, double fallback)
{
  OFXH_CALL(fallback, return nb::cast<double>(o.attr(name));)
}

static bool attrBool(const nb::object &o, const char *name, bool fallback)
{
  OFXH_CALL(fallback, return nb::cast<bool>(o.attr(name));)
}

static std::pair<double, double> attrPair(const nb::object &o, const char *name, std::pair<double, double> fallback)
{
  OFXH_CALL(fallback, return nb::cast<std::pair<double, double> >(o.attr(name));)
}

// ---------------------------------------------------------------------------------------------
// PyHost

PyHost::PyHost(nb::object py, RefPtr ref)
  : _py(std::move(py))
  , _ref(std::move(ref))
{
  // the host description of hostDemo (hostDemoHostDescriptor.cpp), with its own name
  _properties.setIntProperty(kOfxPropAPIVersion, 1, 0);
  _properties.setIntProperty(kOfxPropAPIVersion, 4, 1);
  _properties.setStringProperty(kOfxPropName, "net.sf.openfx.ofxh");
  _properties.setStringProperty(kOfxPropLabel, "OFX HostSupport Python host");
  _properties.setIntProperty(kOfxPropVersion, 1, 0);
  _properties.setIntProperty(kOfxPropVersion, 0, 1);
  _properties.setStringProperty(kOfxPropVersionLabel, "1.0");
  _properties.setIntProperty(kOfxImageEffectHostPropIsBackground, 0);
  _properties.setIntProperty(kOfxImageEffectPropSupportsOverlays, 0);
  _properties.setIntProperty(kOfxImageEffectPropSupportsMultiResolution, 0);
  _properties.setIntProperty(kOfxImageEffectPropSupportsTiles, true);
  _properties.setIntProperty(kOfxImageEffectPropTemporalClipAccess, true);
  _properties.setStringProperty(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
  _properties.setStringProperty(kOfxImageEffectPropSupportedComponents, kOfxImageComponentAlpha, 1);
  _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGenerator, 0);
  _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 1);
  _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 2);
  _properties.setStringProperty(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextTransition, 3);
  _properties.setIntProperty(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
  _properties.setIntProperty(kOfxParamHostPropSupportsStrChoice, 1);       // newStoredParam() makes them
  _properties.setIntProperty(kOfxHostSupportHostPropSupportsBytes, 1);     // newStoredParam() makes them
  _properties.setIntProperty(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
  _properties.setIntProperty(kOfxImageEffectInstancePropSequentialRender, 2);
  _properties.setIntProperty(kOfxImageEffectPropSetableFrameRate, 0);
  _properties.setIntProperty(kOfxImageEffectPropSetableFielding, 0);
  _properties.setIntProperty(kOfxParamHostPropSupportsCustomInteract, 0);
  _properties.setIntProperty(kOfxParamHostPropSupportsStringAnimation, 0);
  _properties.setIntProperty(kOfxParamHostPropSupportsChoiceAnimation, 0);   // part 1: no keys
  _properties.setIntProperty(kOfxParamHostPropSupportsBooleanAnimation, 0);  // part 1: no keys
  _properties.setIntProperty(kOfxParamHostPropSupportsCustomAnimation, 0);
  _properties.setIntProperty(kOfxParamHostPropMaxParameters, -1);
  _properties.setIntProperty(kOfxParamHostPropMaxPages, 0);
  _properties.setIntProperty(kOfxParamHostPropPageRowColumnCount, 0, 0);
  _properties.setIntProperty(kOfxParamHostPropPageRowColumnCount, 0, 1);
  _ref->p = this;
}

PyHost::~PyHost()
{
  if (nb::is_alive()) {
    nb::gil_scoped_acquire gil;
    _ref->p = nullptr;
    _py.reset();
  } else {
    _ref->p = nullptr;
    _py.release();  // (leaked: the interpreter is gone)
  }
}

OHIE::Instance *PyHost::newInstance(void *clientData, OHIE::ImageEffectPlugin *plugin,
                                    OHIE::Descriptor &desc, const std::string &context)
{
  // clientData is the Python instance passed to ofxh.create_instance()
  OFXH_CALL((OHIE::Instance *)NULL,
    if (!clientData) {
      raise(PyExc_RuntimeError, "use ofxh.create_instance() to create an instance");
    }
    nb::object py = nb::borrow((PyObject *)clientData);
    InstanceBase &ib = nb::cast<InstanceBase &>(py);
    return new PyEffectInstance(plugin, desc, context, py, ib.ref);
  )
}

OHIE::Descriptor *PyHost::makeDescriptor(OHIE::ImageEffectPlugin *plugin)
{
  return new OHIE::Descriptor(plugin);
}

OHIE::Descriptor *PyHost::makeDescriptor(const OHIE::Descriptor &rootContext, OHIE::ImageEffectPlugin *plugin)
{
  return new OHIE::Descriptor(rootContext, plugin);
}

OHIE::Descriptor *PyHost::makeDescriptor(const std::string &bundlePath, OHIE::ImageEffectPlugin *plugin)
{
  return new OHIE::Descriptor(bundlePath, plugin);
}

OfxStatus PyHost::vmessage(const char *type, const char *id, const char *format, va_list args)
{
  const std::string text = formatMessage(format, args);
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    return statusFromResult(_py.attr("message")(type ? type : "", id ? id : "", text));
  )
}

OfxStatus PyHost::setPersistentMessage(const char *type, const char *id, const char *format, va_list args)
{
  const std::string text = formatMessage(format, args);
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    return statusFromResult(_py.attr("persistent_message")(type ? type : "", id ? id : "", text));
  )
}

OfxStatus PyHost::clearPersistentMessage()
{
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    return statusFromResult(_py.attr("clear_persistent_message")());
  )
}

// ---------------------------------------------------------------------------------------------
// PyImage

PyImage::PyImage(OHIE::ClipInstance &clip, nb::object pixels, void *data, const OfxPointD &renderScale,
                 const OfxRectI &bounds, const OfxRectI &rod, int rowBytes, const std::string &field,
                 const std::string &uniqueId)
  : OHIE::Image(clip, renderScale.x, renderScale.y, data, bounds, rod, rowBytes, field, uniqueId)
  , _pixels(std::move(pixels))
{
}

PyImage::~PyImage()
{
  // HostSupport deletes the image when its last reference is released, usually in a plug-in's
  // clipReleaseImage (with the GIL, part 1 renders on the Python thread)
  if (nb::is_alive()) {
    nb::gil_scoped_acquire gil;
    _pixels.reset();
  } else {
    _pixels.release();  // (leaked: the interpreter is gone)
  }
}

// ---------------------------------------------------------------------------------------------
// PyClipInstance

PyClipInstance::PyClipInstance(PyEffectInstance *effect, OHIE::ClipDescriptor &desc, nb::object py, RefPtr ref)
  : OHIE::ClipInstance(effect, desc)
  , _effect(effect)
  , _py(std::move(py))
  , _ref(std::move(ref))
  , _view(0)
  , _outputImage(NULL)
  , _inRoDAction(false)
{
  _ref->p = this;
}

PyClipInstance::~PyClipInstance()
{
  releaseOutputImage();
  if (nb::is_alive()) {
    nb::gil_scoped_acquire gil;
    _ref->p = nullptr;
    _outputData.reset();
    _py.reset();
  } else {
    _ref->p = nullptr;
    _outputData.release();
    _py.release();
  }
}

void PyClipInstance::releaseOutputImage()
{
  if (_outputImage) {
    PyImage *img = _outputImage;
    _outputImage = NULL;
    img->releaseReference();
  }
}

static bool dtypeOf(const std::string &depth, nb::dlpack::dtype &dt, const char *&name)
{
  if (depth == kOfxBitDepthByte) {
    dt = nb::dtype<uint8_t>();
    name = "uint8";
  } else if (depth == kOfxBitDepthShort) {
    dt = nb::dtype<uint16_t>();
    name = "uint16";
  } else if (depth == kOfxBitDepthHalf) {
    dt = nb::dlpack::dtype{(uint8_t)nb::dlpack::dtype_code::Float, 16, 1};
    name = "float16";
  } else if (depth == kOfxBitDepthFloat) {
    dt = nb::dtype<float>();
    name = "float32";
  } else {
    return false;
  }
  return true;
}

nb::object PyClipInstance::newImage(int x1, int y1, int x2, int y2) const
{
  const std::string &depth = getPixelDepth();
  const std::string &comps = getComponents();
  nb::dlpack::dtype dt;
  const char *dtname = NULL;
  const int c = componentCount(comps);
  if (!dtypeOf(depth, dt, dtname) || c == 0) {
    raise(PyExc_ValueError, "clip " + getName() + ": can't make images of depth \"" + depth +
                            "\" and components \"" + comps + "\"");
  }
  if (x2 < x1) x2 = x1;
  if (y2 < y1) y2 = y1;
  Globals &g = globals();
  if (!g.numpy.is_valid()) {
    g.numpy = nb::module_::import_("numpy");
  }
  return g.numpy.attr("empty")(nb::make_tuple(y2 - y1, x2 - x1, c), dtname);
}

// fill the output image with hostDemo's sentinel (hostDemoClipInstance.cpp, MyImage): a quiet NaN
// for float (and half) images, 0xCD bytes for byte and short images, so that unwritten pixels show
static void fillSentinel(unsigned char *data, long long rowBytes, int width, int height, int nComps,
                         const std::string &depth)
{
  for (int y = 0; y < height; ++y) {
    unsigned char *row = data + (long long)y * rowBytes;
    const size_t n = (size_t)width * (size_t)nComps;
    if (depth == kOfxBitDepthFloat) {
      const unsigned int nanBits = 0x7fc00000U;
      for (size_t i = 0; i < n; ++i) {
        std::memcpy(row + i * 4, &nanBits, 4);
      }
    } else if (depth == kOfxBitDepthHalf) {
      const unsigned short nanBits = 0x7e00U;
      for (size_t i = 0; i < n; ++i) {
        std::memcpy(row + i * 2, &nanBits, 2);
      }
    } else {
      const size_t bytes = n * (depth == kOfxBitDepthShort ? 2 : 1);
      std::memset(row, 0xCD, bytes);
    }
  }
}

OHIE::Image *PyClipInstance::fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds)
{
  OFXH_CALL((OHIE::Image *)NULL,
    if (isOutput() && _outputImage) {
      // one output image per render, shared by all its fetches
      _outputImage->addReference();
      return (OHIE::Image *)_outputImage;
    }
    nb::object b = optionalBounds ? nb::object(nb::make_tuple(optionalBounds->x1, optionalBounds->y1,
                                                              optionalBounds->x2, optionalBounds->y2))
                                  : nb::none();
    nb::object r = _py.attr("get_image")(time, view, b);
    if (r.is_none()) {
      return (OHIE::Image *)NULL;
    }
    if (!nb::isinstance<ImageData>(r)) {
      raise(PyExc_TypeError, "clip " + getName() + ": get_image() must return an ofxh.ImageData or None, not " +
                             nb::cast<std::string>(nb::str(r.type())));
    }
    ImageData &d = nb::cast<ImageData &>(r);
    const std::string where = "clip " + getName() + ": get_image(): ";
    const OfxRectI bounds = toRectI(d.bounds, "bounds");
    const std::string &depth = getPixelDepth();
    const std::string &comps = getComponents();
    const int nComps = componentCount(comps);
    nb::dlpack::dtype dt;
    const char *dtname = NULL;
    if (!dtypeOf(depth, dt, dtname) || nComps == 0) {
      raise(PyExc_ValueError, where + "the clip's depth \"" + depth + "\" and components \"" + comps +
                              "\" are not supported");
    }
    Globals &g = globals();
    if (!g.numpy.is_valid()) {
      g.numpy = nb::module_::import_("numpy");
    }
    if (!nb::isinstance(d.pixels, g.numpy.attr("ndarray"))) {
      raise(PyExc_TypeError, where + "pixels must be a NumPy array");
    }
    nb::ndarray<nb::ro> a;
    if (!nb::try_cast(d.pixels, a, false)) {
      raise(PyExc_TypeError, where + "pixels: unsupported NumPy array");
    }
    const int width = bounds.x2 - bounds.x1, height = bounds.y2 - bounds.y1;
    if (a.ndim() != 3 || width < 0 || height < 0 || (long long)a.shape(0) != height ||
        (long long)a.shape(1) != width || (int)a.shape(2) != nComps) {
      std::ostringstream ss;
      ss << where << "pixels must have the shape (y2-y1, x2-x1, " << nComps << ") = (" << height << ", "
         << width << ", " << nComps << ") of the bounds and the clip's components (" << comps << "), not (";
      for (size_t i = 0; i < a.ndim(); ++i) {
        ss << (i ? ", " : "") << a.shape(i);
      }
      ss << ")";
      raise(PyExc_ValueError, ss.str());
    }
    if (a.dtype() != dt) {
      raise(PyExc_ValueError, where + "pixels must be " + dtname + ", the clip's depth (" + depth + "), not " +
                              nb::cast<std::string>(nb::str(nb::handle(nb::object(d.pixels.attr("dtype"))))));
    }
    if ((nComps > 1 && a.stride(2) != 1) || (width > 1 && a.stride(1) != nComps)) {
      raise(PyExc_ValueError, where + "the pixels of each row must be contiguous (strides[1] == C*itemsize, "
                              "strides[2] == itemsize)");
    }
    if (isOutput() && !nb::cast<bool>(d.pixels.attr("flags").attr("writeable"))) {
      raise(PyExc_ValueError, where + "the pixels of an output image must be writable");
    }
    const long long rowBytes = (long long)a.stride(0) * (long long)a.itemsize();
    if (rowBytes > INT_MAX || rowBytes < -INT_MAX) {
      raise(PyExc_ValueError, where + "the rows are too large");
    }
    void *data = const_cast<void *>(static_cast<const void *>(a.data()));
    const double par = getAspectRatio();
    const OfxRectI rodPixels = d.rod.is_none() ? canonicalToPixelEnclosing(rod(time, view), _effect->_renderScale, par)
                                               : toRectI(d.rod, "rod");
    std::string uid = d.uniqueId;
    if (uid.empty()) {
      std::ostringstream ss;  // as hostDemo
      ss << getName() << ":" << time << ":" << view << (isOutput() ? ":output" : "");
      uid = ss.str();
    }
    if (isOutput()) {
      fillSentinel(static_cast<unsigned char *>(data), rowBytes, width, height, nComps, depth);
    }
    PyImage *img = new PyImage(*this, nb::borrow(d.pixels), data, _effect->_renderScale, bounds, rodPixels,
                               (int)rowBytes, fieldName(d.field), uid);
    if (isOutput()) {
      // the clip keeps a reference until the end of the render (releaseOutputImage)
      _outputImage = img;
      _outputData = r;
      img->addReference();
    }
    return (OHIE::Image *)img;
  )
}

OfxRectD PyClipInstance::rod(OfxTime time, int view) const
{
  OFXH_CALL(emptyRectD(), return toRectD(_py.attr("region_of_definition")(time, view));)
}

OfxRectD PyClipInstance::defaultRoD(OfxTime time, int view) const
{
  OfxRectD v = emptyRectD();
  if (isOutput()) {
    // the output RoD is the effect's RoD (the guard protects against a plug-in that asks for
    // the output RoD in its own RoD action)
    if (_inRoDAction) {
      return v;
    }
    _inRoDAction = true;
    OfxRectD r = v;
    const OfxStatus st = _effect->getRegionOfDefinitionAction(time, _effect->_renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                              view,
#endif
                                                              r);
    _inRoDAction = false;
    if (st == kOfxStatOK || st == kOfxStatReplyDefault) {
      v = r;
    }
    return v;
  }
  (void)view;
  if (!getConnected()) {
    return v;  // a disconnected clip has an empty RoD
  }
  // the project
  double x, y, w, h;
  _effect->getProjectOffset(x, y);
  _effect->getProjectSize(w, h);
  v.x1 = x;
  v.y1 = y;
  v.x2 = x + w;
  v.y2 = y + h;
  return v;
}

const std::string &PyClipInstance::getPixelDepth() const
{
  static const std::string none(kOfxBitDepthNone);
  return getConnected() ? OHIE::ClipInstance::getPixelDepth() : none;
}

const std::string &PyClipInstance::getComponents() const
{
  static const std::string none(kOfxImageComponentNone);
  return getConnected() ? OHIE::ClipInstance::getComponents() : none;
}

const std::string &PyClipInstance::getUnmappedBitDepth() const
{
  // "kOfxBitDepthNone (implying a clip is unconnected image)"
  static const std::string none(kOfxBitDepthNone);
  if (!getConnected()) {
    return none;
  }
  _unmappedDepth = depthName(attrString(_py, "unmapped_bit_depth", kOfxBitDepthByte));
  return _unmappedDepth;
}

const std::string &PyClipInstance::getUnmappedComponents() const
{
  // "kOfxImageComponentNone (implying a clip is unconnected, not valid for an image)"
  static const std::string none(kOfxImageComponentNone);
  if (!getConnected()) {
    return none;
  }
  _unmappedComponents = componentsName(attrString(_py, "unmapped_components", kOfxImageComponentRGBA));
  return _unmappedComponents;
}

const std::string &PyClipInstance::getPremult() const
{
  _premult = premultName(attrString(_py, "premult", kOfxImageUnPreMultiplied));
  return _premult;
}

#ifdef OFX_EXTENSIONS_NATRON
OfxRectI PyClipInstance::getFormat() const
{
  OfxPointD one;
  one.x = one.y = 1.;
  const OfxRectI fallback = _effect->projectPixelRect(one);
  OFXH_CALL(fallback, return toRectI(_py.attr("format"), "format");)
}
#endif

double PyClipInstance::getAspectRatio() const { return attrDouble(_py, "aspect_ratio", 1.); }

double PyClipInstance::getFrameRate() const { return attrDouble(_py, "frame_rate", 25.); }

void PyClipInstance::getFrameRange(double &startFrame, double &endFrame) const
{
  const std::pair<double, double> r = attrPair(_py, "frame_range", std::make_pair(0., 25.));
  startFrame = r.first;
  endFrame = r.second;
}

const std::string &PyClipInstance::getFieldOrder() const
{
  _fieldOrder = fieldName(attrString(_py, "field_order", kOfxImageFieldNone));
  return _fieldOrder;
}

bool PyClipInstance::getConnected() const
{
  // the output clip is always connected
  return isOutput() || attrBool(_py, "connected", true);
}

double PyClipInstance::getUnmappedFrameRate() const { return attrDouble(_py, "unmapped_frame_rate", 25.); }

void PyClipInstance::getUnmappedFrameRange(double &unmappedStartFrame, double &unmappedEndFrame) const
{
  const std::pair<double, double> r = attrPair(_py, "unmapped_frame_range", std::make_pair(0., 25.));
  unmappedStartFrame = r.first;
  unmappedEndFrame = r.second;
}

bool PyClipInstance::getContinuousSamples() const { return attrBool(_py, "continuous_samples", false); }

OHIE::Image *PyClipInstance::getImage(OfxTime time, const OfxRectD *optionalBounds)
{
  return fetchImage(time, _view, optionalBounds);
}

#ifdef OFX_EXTENSIONS_NUKE
OHIE::Image *PyClipInstance::getImagePlane(OfxTime time, int view, const std::string &plane,
                                           const OfxRectD *optionalBounds)
{
  // part 1: the colour plane only
  if (plane != kFnOfxImagePlaneColour) {
    return NULL;
  }
  return fetchImage(time, view < 0 ? _view : view, optionalBounds);
}

OfxRectD PyClipInstance::getRegionOfDefinition(OfxTime time, int view) const
{
  return rod(time, view < 0 ? _view : view);
}
#endif

#ifdef OFX_EXTENSIONS_VEGAS
OHIE::Image *PyClipInstance::getStereoscopicImage(OfxTime time, int view, const OfxRectD *optionalBounds)
{
  return fetchImage(time, view, optionalBounds);
}
#endif

#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
void PyClipInstance::setView(int view)
{
  if (view != _view) {
    releaseOutputImage();
    _view = view;
  }
}
#endif

OfxRectD PyClipInstance::getRegionOfDefinition(OfxTime time) const { return rod(time, _view); }

// ---------------------------------------------------------------------------------------------
// PyEffectInstance

std::set<PyEffectInstance *> &openInstances()
{
  static std::set<PyEffectInstance *> s;
  return s;
}

PyEffectInstance::PyEffectInstance(OHIE::ImageEffectPlugin *plugin, OHIE::Descriptor &desc,
                                   const std::string &context, nb::object py, RefPtr ref)
  : OHIE::Instance(plugin, desc, context, false)
  , _py(std::move(py))
  , _ref(std::move(ref))
  , _rendering(false)
  , _timeLineTime(0.)
  , _abort(0)
{
  _renderScale.x = _renderScale.y = 1.;
  _renderWindow.x1 = _renderWindow.y1 = _renderWindow.x2 = _renderWindow.y2 = 0;
  _ref->p = this;
  openInstances().insert(this);
}

PyEffectInstance::~PyEffectInstance()
{
  openInstances().erase(this);
  // (~Instance() deletes the clips and params next)
  if (nb::is_alive()) {
    nb::gil_scoped_acquire gil;
    _ref->p = nullptr;
    _py.reset();
  } else {
    _ref->p = nullptr;
    _py.release();
  }
}

OfxRectI PyEffectInstance::projectPixelRect(const OfxPointD &renderScale) const
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

const std::string &PyEffectInstance::getDefaultOutputFielding() const
{
  _fielding = fieldName(attrString(_py, "default_output_fielding", kOfxImageFieldNone));
  return _fielding;
}

/// ofxh.ClipDescriptor: what new_clip_instance() is told about the clip (a copy)
nb::object makeClipDescriptor(const OHIE::ClipDescriptor &desc);

OHIE::ClipInstance *PyEffectInstance::newClipInstance(OHIE::Instance * /*plugin*/, OHIE::ClipDescriptor *descriptor,
                                                      int index)
{
  OFXH_CALL((OHIE::ClipInstance *)NULL,
    nb::object py = _py.attr("new_clip_instance")(makeClipDescriptor(*descriptor), index);
    if (!nb::isinstance<ClipBase>(py)) {
      raise(PyExc_TypeError, "new_clip_instance() must return an ofxh.ClipInstance, not " +
                             nb::cast<std::string>(nb::str(py.type())));
    }
    ClipBase &cb = nb::cast<ClipBase &>(py);
    if (cb.ref->p) {
      raise(PyExc_ValueError, "new_clip_instance() returned a clip that already belongs to an instance");
    }
    return (OHIE::ClipInstance *)new PyClipInstance(this, *descriptor, py, cb.ref);
  )
}

OfxStatus PyEffectInstance::vmessage(const char *type, const char *id, const char *format, va_list args)
{
  const std::string text = formatMessage(format, args);
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    return statusFromResult(_py.attr("message")(type ? type : "", id ? id : "", text));
  )
}

OfxStatus PyEffectInstance::setPersistentMessage(const char *type, const char *id, const char *format, va_list args)
{
  const std::string text = formatMessage(format, args);
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    return statusFromResult(_py.attr("persistent_message")(type ? type : "", id ? id : "", text));
  )
}

OfxStatus PyEffectInstance::clearPersistentMessage()
{
  OFXH_CALL(StatusFallback{kOfxStatFailed}, return statusFromResult(_py.attr("clear_persistent_message")());)
}

int PyEffectInstance::abort() { return _abort.load() ? 1 : 0; }

void PyEffectInstance::getProjectSize(double &xSize, double &ySize) const
{
  const std::pair<double, double> r = attrPair(_py, "project_size", std::make_pair(768., 576.));
  xSize = r.first;
  ySize = r.second;
}

void PyEffectInstance::getProjectOffset(double &xOffset, double &yOffset) const
{
  const std::pair<double, double> r = attrPair(_py, "project_offset", std::make_pair(0., 0.));
  xOffset = r.first;
  yOffset = r.second;
}

void PyEffectInstance::getProjectExtent(double &xSize, double &ySize) const
{
  const std::pair<double, double> r = attrPair(_py, "project_extent", std::make_pair(768., 576.));
  xSize = r.first;
  ySize = r.second;
}

double PyEffectInstance::getProjectPixelAspectRatio() const
{
  return attrDouble(_py, "project_pixel_aspect_ratio", 768. / 720.);
}

double PyEffectInstance::getEffectDuration() const { return attrDouble(_py, "effect_duration", 25.); }

double PyEffectInstance::getFrameRate() const { return attrDouble(_py, "frame_rate", 25.); }

double PyEffectInstance::getFrameRecursive() const { return attrDouble(_py, "frame_recursive", _timeLineTime); }

void PyEffectInstance::getRenderScaleRecursive(double &x, double &y) const
{
  const std::pair<double, double> r =
    attrPair(_py, "render_scale_recursive", std::make_pair(_renderScale.x, _renderScale.y));
  x = r.first;
  y = r.second;
}

#ifdef OFX_EXTENSIONS_NUKE
OfxStatus PyEffectInstance::getViewCount(int *nViews) const
{
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    *nViews = nb::cast<int>(_py.attr("view_count"));
    return kOfxStatOK;
  )
}

OfxStatus PyEffectInstance::getViewName(int viewIndex, const char **name) const
{
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    _viewName = nb::cast<std::string>(_py.attr("view_name")(viewIndex));
    *name = _viewName.c_str();  // "valid throughout the calling action"
    return kOfxStatOK;
  )
}
#endif

OH::Param::Instance *PyEffectInstance::newParam(const std::string & /*name*/, OH::Param::Descriptor &descriptor)
{
  try {
    return newStoredParam(this, descriptor);
  } catch (...) {
    return NULL;
  }
}

OfxStatus PyEffectInstance::editBegin(const std::string &name)
{
  OFXH_CALL(StatusFallback{kOfxStatFailed}, return statusFromResult(_py.attr("edit_begin")(name));)
}

OfxStatus PyEffectInstance::editEnd()
{
  OFXH_CALL(StatusFallback{kOfxStatFailed}, return statusFromResult(_py.attr("edit_end")());)
}

void PyEffectInstance::progressStart(const std::string &message, const std::string &messageid)
{
  OFXH_CALL(Void(), _py.attr("progress_start")(message, messageid);)
}

void PyEffectInstance::progressEnd()
{
  OFXH_CALL(Void(), _py.attr("progress_end")();)
}

bool PyEffectInstance::progressUpdate(double t)
{
  OFXH_CALL(true,
    nb::object r = _py.attr("progress_update")(t);
    return r.is_none() ? true : nb::cast<bool>(r);
  )
}

double PyEffectInstance::timeLineGetTime()
{
  OFXH_CALL(_timeLineTime, return nb::cast<double>(_py.attr("timeline_get_time")());)
}

void PyEffectInstance::timeLineGotoTime(double t)
{
  OFXH_CALL(Void(), _py.attr("timeline_goto_time")(t);)
}

void PyEffectInstance::timeLineGetBounds(double &t1, double &t2)
{
  std::pair<double, double> r = std::make_pair(0., 25.);
  {
    nb::gil_scoped_acquire gil;
    try {
      r = nb::cast<std::pair<double, double> >(_py.attr("timeline_get_bounds")());
    } catch (nb::python_error &e) {
      saveError(e.value());
    } catch (const std::exception &e) {
      saveError(e.what());
    }
  }
  t1 = r.first;
  t2 = r.second;
}

}  // namespace ofxh
