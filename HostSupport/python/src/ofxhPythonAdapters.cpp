// Python bindings of HostSupport: the adapters (see ofxhPython.h)

#include "ofxhPython.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/pair.h>

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include "ofxMessage.h"
#include "ofxParametricParam.h"  // kOfxParamTypeParametric
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"  // kFnOfxImagePlaneColour
#endif

namespace ofxh {

// ---------------------------------------------------------------------------------------------
// References and errors

void raiseError(PyObject *type, const std::string &message)
{
  PyErr_SetString(type, message.c_str());
  throw nb::python_error();
}

void raiseDeleted(const char *what)
{
  raiseError(PyExc_ReferenceError, std::string(what) + ": deleted by HostSupport (or not created yet)");
}

PyHost *HostBase::get() const
{
  checkThread("ofxh", false);
  if (!ref->p) {
    raiseDeleted("ofxh.Host");
  }
  return static_cast<PyHost *>(ref->p);
}

PyEffectInstance *InstanceBase::get() const
{
  checkThread("ofxh", false);
  if (!ref->p) {
    raiseDeleted("ofxh.ImageEffectInstance");
  }
  return static_cast<PyEffectInstance *>(ref->p);
}

PyClipInstance *ClipBase::get() const
{
  checkThread("ofxh", false);
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

nb::object numpy()
{
  Globals &g = globals();
  if (!g.numpy.is_valid()) {
    g.numpy = nb::module_::import_("numpy");
  }
  return g.numpy;
}

nb::object stat(OfxStatus s) { return globals().statFn(s); }

// The scopes and their errors.
// - The owner thread: the thread of the open scopes of the bindings called from Python (the
//   actions run there, with the GIL released). Its scopes share one pending error (ownerPending).
// - A worker thread: another thread, inside a callback (a plug-in's thread that called the host
//   during an action). Its scopes (the non-action bindings, OFXH_SCOPED) have their own pending
//   error and depth (thread_local), so that the scopes of two workers don't interleave. An error
//   of a worker's callback outside its own scopes goes to the owner's pending error: the action
//   raises it.
// All of this is touched with the GIL only. The pending errors are raw references (PyObject *):
// a thread_local nb::object would be destroyed at the thread's exit, without the GIL.

/// the pending error of the owner thread's innermost scope (a new reference, or NULL)
static PyObject *&ownerPending()
{
  static PyObject *e = NULL;
  return e;
}

/// the pending error of this worker thread's innermost scope
static PyObject *&workerPending()
{
  static thread_local PyObject *e = NULL;
  return e;
}

/// the open scopes of the owner thread
static int &ownerDepth()
{
  static int d = 0;
  return d;
}

/// the open scopes of this thread (owner or worker)
static int &threadDepth()
{
  static thread_local int d = 0;
  return d;
}

static std::thread::id &ownerThread()
{
  static std::thread::id t;
  return t;
}

/// is a scope open on the owner thread, and is this another thread?
static bool foreignThread() { return ownerDepth() > 0 && ownerThread() != std::this_thread::get_id(); }

int ActionScope::depth() { return threadDepth(); }

bool ActionScope::running() { return ownerDepth() > 0; }

bool ActionScope::onWorker() { return foreignThread(); }

int &CallbackDepth::depth()
{
  static thread_local int d = 0;
  return d;
}

void checkThread(const char *what, bool action)
{
  if (!foreignThread()) {
    return;
  }
  if (CallbackDepth::depth() == 0) {
    raiseError(PyExc_RuntimeError, std::string(what) + ": an action is running in another thread (ofxh is not "
                                   "thread-safe: call it from one thread)");
  }
  if (action) {
    raiseError(PyExc_RuntimeError, std::string(what) + ": a plug-in's worker thread may only call the bindings that "
                                   "don't run an action or change a value (an action is running in another thread)");
  }
}

/// a callback failed outside any action: nobody can raise its error, report it
static void reportUnraisable(nb::handle exc)
{
  PyErr_SetObject((PyObject *)Py_TYPE(exc.ptr()), exc.ptr());
  PyErr_WriteUnraisable(nb::str("ofxh: a host callback called outside an action").ptr());
}

void saveError(nb::handle exc)
{
  PyObject **slot = NULL;
  if (threadDepth() > 0) {
    slot = foreignThread() ? &workerPending() : &ownerPending();
  } else if (ownerDepth() > 0) {
    slot = &ownerPending();  // (a worker's callback outside its own scopes: the action raises it)
  } else {
    reportUnraisable(exc);
    return;
  }
  if (!*slot) {
    Py_INCREF(exc.ptr());
    *slot = exc.ptr();
  }
}

void saveError(const char *message)
{
  nb::object e = nb::steal(PyObject_CallFunction(PyExc_RuntimeError, "s", message));
  if (!e.is_valid()) {
    PyErr_Clear();
    return;
  }
  saveError(e);
}

ActionScope::ActionScope(bool fromWorker)
  : _saved(NULL)
  , _slot(NULL)
  , _owner(true)
  , _done(false)
{
  if (foreignThread()) {
    // (raises unless fromWorker and inside a callback)
    checkThread(fromWorker ? "ofxh" : "ofxh action", !fromWorker);
    _owner = false;
    _slot = &workerPending();
  } else {
    if (ownerDepth() == 0) {
      ownerThread() = std::this_thread::get_id();
    }
    ++ownerDepth();
    _slot = &ownerPending();
  }
  _saved = *_slot;
  *_slot = NULL;
  ++threadDepth();
}

ActionScope::~ActionScope() { discard(); }

void ActionScope::discard()
{
  if (!_done) {
    _done = true;
    --threadDepth();
    if (_owner) {
      --ownerDepth();
    }
    PyObject *cur = *_slot;
    *_slot = _saved;
    Py_XDECREF(cur);
  }
}

bool ActionScope::failed() const { return !_done && *_slot != NULL; }

void ActionScope::check(const char *where, const OfxStatus *status)
{
  nb::object e = nb::steal(*_slot);
  *_slot = NULL;
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
  char buf[1024];
  va_list copy;
  va_copy(copy, args);
  const int n = vsnprintf(buf, sizeof(buf), format, copy);
  va_end(copy);
  if (n < 0) {
    return std::string("...");  // (an encoding error)
  }
  if ((size_t)n < sizeof(buf)) {
    return std::string(buf, (size_t)n);
  }
  std::vector<char> big((size_t)n + 1);
  va_copy(copy, args);
  vsnprintf(&big[0], big.size(), format, copy);
  va_end(copy);
  return std::string(&big[0], (size_t)n);
}

// ---------------------------------------------------------------------------------------------
// Names

struct NameEntry {
  const char *kind;
  const char *shortName;
  const char *ofx;
};

static const NameEntry kNames[] = {
  {"depth", "none", kOfxBitDepthNone},
  {"depth", "byte", kOfxBitDepthByte},
  {"depth", "short", kOfxBitDepthShort},
  {"depth", "half", kOfxBitDepthHalf},
  {"depth", "float", kOfxBitDepthFloat},
  {"components", "none", kOfxImageComponentNone},
  {"components", "rgba", kOfxImageComponentRGBA},
  {"components", "rgb", kOfxImageComponentRGB},
  {"components", "alpha", kOfxImageComponentAlpha},
  {"premult", "opaque", kOfxImageOpaque},
  {"premult", "premult", kOfxImagePreMultiplied},
  {"premult", "unpremult", kOfxImageUnPreMultiplied},
  {"field", "none", kOfxImageFieldNone},
  {"field", "both", kOfxImageFieldBoth},
  {"field", "lower", kOfxImageFieldLower},
  {"field", "upper", kOfxImageFieldUpper},
  {"field", "single", kOfxImageFieldSingle},
  {"field", "doubled", kOfxImageFieldDoubled},
  {"context", "generator", kOfxImageEffectContextGenerator},
  {"context", "filter", kOfxImageEffectContextFilter},
  {"context", "transition", kOfxImageEffectContextTransition},
  {"context", "paint", kOfxImageEffectContextPaint},
  {"context", "general", kOfxImageEffectContextGeneral},
  {"context", "retimer", kOfxImageEffectContextRetimer},
  {"context", "reader", "OfxImageEffectContextReader"},  // (the macros need OFX_EXTENSIONS_TUTTLE)
  {"context", "writer", "OfxImageEffectContextWriter"},
  {"message", "fatal", kOfxMessageFatal},
  {"message", "error", kOfxMessageError},
  {"message", "warning", kOfxMessageWarning},
  {"message", "message", kOfxMessageMessage},
  {"message", "log", kOfxMessageLog},
  {"message", "question", kOfxMessageQuestion},
  {"change", "user", kOfxChangeUserEdited},
  {"change", "plugin", kOfxChangePluginEdited},
  {"change", "time", kOfxChangeTime},
  {"param_type", "integer", kOfxParamTypeInteger},
  {"param_type", "double", kOfxParamTypeDouble},
  {"param_type", "boolean", kOfxParamTypeBoolean},
  {"param_type", "choice", kOfxParamTypeChoice},
  {"param_type", "strchoice", kOfxParamTypeStrChoice},
  {"param_type", "rgba", kOfxParamTypeRGBA},
  {"param_type", "rgb", kOfxParamTypeRGB},
  {"param_type", "double2d", kOfxParamTypeDouble2D},
  {"param_type", "integer2d", kOfxParamTypeInteger2D},
  {"param_type", "double3d", kOfxParamTypeDouble3D},
  {"param_type", "integer3d", kOfxParamTypeInteger3D},
  {"param_type", "string", kOfxParamTypeString},
  {"param_type", "custom", kOfxParamTypeCustom},
  {"param_type", "bytes", kOfxParamTypeBytes},
  {"param_type", "pushbutton", kOfxParamTypePushButton},
  {"param_type", "group", kOfxParamTypeGroup},
  {"param_type", "page", kOfxParamTypePage},
  {"param_type", "parametric", kOfxParamTypeParametric},
  // (the Nuke extension's colour plane; the other planes keep their names)
  {"plane", "Colour", "uk.co.thefoundry.OfxImagePlaneColour"},
  {NULL, NULL, NULL}};

/// other accepted spellings
static const NameEntry kAliases[] = {
  {"depth", "uint8", kOfxBitDepthByte},
  {"depth", "uint16", kOfxBitDepthShort},
  {"depth", "float16", kOfxBitDepthHalf},
  {"depth", "float32", kOfxBitDepthFloat},
  {"components", "a", kOfxImageComponentAlpha},
  {"premult", "premultiplied", kOfxImagePreMultiplied},
  {"premult", "unpremultiplied", kOfxImageUnPreMultiplied},
  {"plane", "Color", "uk.co.thefoundry.OfxImagePlaneColour"},
  {NULL, NULL, NULL}};

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

std::string ofxName(const char *kind, const std::string &s)
{
  const std::string l = lower(s);
  for (const NameEntry *t : {kNames, kAliases}) {
    for (const NameEntry *e = t; e->kind; ++e) {
      if (std::strcmp(e->kind, kind) == 0 && lower(e->shortName) == l) {
        return e->ofx;
      }
    }
  }
  return s;
}

std::string shortName(const char *kind, const std::string &s)
{
  for (const NameEntry *e = kNames; e->kind; ++e) {
    if (std::strcmp(e->kind, kind) == 0 && s == e->ofx) {
      return e->shortName;
    }
  }
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
    raiseError(PyExc_ValueError, "a rectangle is (x1, y1, x2, y2)");
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
    raiseError(PyExc_ValueError, std::string(what) + " is (x1, y1, x2, y2)");
  }
  OfxRectI r;
  r.x1 = nb::cast<int>(t[0]);
  r.y1 = nb::cast<int>(t[1]);
  r.x2 = nb::cast<int>(t[2]);
  r.y2 = nb::cast<int>(t[3]);
  return r;
}

static std::string typeName(nb::handle o) { return nb::cast<std::string>(nb::str(o.type())); }

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

static std::pair<double, double> callPair(const nb::object &o, const char *name, std::pair<double, double> fallback)
{
  OFXH_CALL(fallback, return nb::cast<std::pair<double, double> >(o.attr(name)());)
}

/// the cached string for a const std::string& return: assigned only when the value changes, so
/// that what a caller got (e.g. its c_str()) stays valid while the value doesn't change
static const std::string &cached(std::string &cache, const std::string &value)
{
  if (cache != value) {
    cache = value;
  }
  return cache;
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
  // as hostDemo: the C++ parameters have keys (Curve), held for choices and booleans
  _properties.setIntProperty(kOfxParamHostPropSupportsChoiceAnimation, 1);
  _properties.setIntProperty(kOfxParamHostPropSupportsBooleanAnimation, 1);
  _properties.setIntProperty(kOfxParamHostPropSupportsCustomAnimation, 0);
  _properties.setIntProperty(kOfxParamHostPropMaxParameters, -1);
  _properties.setIntProperty(kOfxParamHostPropMaxPages, 0);
  _properties.setIntProperty(kOfxParamHostPropPageRowColumnCount, 0, 0);
  _properties.setIntProperty(kOfxParamHostPropPageRowColumnCount, 0, 1);
  _ref->p = this;
}

/// drop the last reference of an adapter to its Python object: after its Refs are cleared, so
/// that what the Python object's deletion runs (e.g. a __del__) sees a deleted object
void dropLast(nb::object &py)
{
  nb::object last = std::move(py);
  if (!nb::is_alive()) {
    last.release();  // (leaked: the interpreter is gone)
    return;
  }
  nb::gil_scoped_acquire gil;
  last.reset();
}

PyHost::~PyHost()
{
  _ref->p = nullptr;
  dropLast(_py);
}

OHIE::Instance *PyHost::newInstance(void *clientData, OHIE::ImageEffectPlugin *plugin,
                                    OHIE::Descriptor &desc, const std::string &context)
{
  // clientData is the Python instance passed to ofxh.create_instance()
  OFXH_CALL((OHIE::Instance *)NULL,
    if (!clientData) {
      raiseError(PyExc_RuntimeError, "use ofxh.create_instance() to create an instance");
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

/// call a message callback (message or persistent_message) with the short message type
static OfxStatus callMessage(const nb::object &py, const char *method, const char *type, const char *id,
                             const char *format, va_list args)
{
  const std::string text = formatMessage(format, args);
  OFXH_CALL(StatusFallback{kOfxStatFailed},
    return statusFromResult(py.attr(method)(shortName("message", type ? type : ""), id ? id : "", text));
  )
}

OfxStatus PyHost::vmessage(const char *type, const char *id, const char *format, va_list args)
{
  return callMessage(_py, "message", type, id, format, args);
}

OfxStatus PyHost::setPersistentMessage(const char *type, const char *id, const char *format, va_list args)
{
  return callMessage(_py, "persistent_message", type, id, format, args);
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
  // clipReleaseImage, during an action, without the GIL: dropLast() takes it
  dropLast(_pixels);
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
{
  _ref->p = this;
}

PyClipInstance::~PyClipInstance()
{
  _ref->p = nullptr;
  releaseOutputImage();
  dropLast(_outputData);
  dropLast(_py);
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

bool imageFormatSupported(const std::string &depth, const std::string &components)
{
  nb::dlpack::dtype dt;
  const char *name = NULL;
  return dtypeOf(depth, dt, name) && componentCount(components) > 0;
}

nb::object PyClipInstance::newImage(int x1, int y1, int x2, int y2) const
{
  const std::string &depth = getPixelDepth();
  const std::string &comps = getComponents();
  nb::dlpack::dtype dt;
  const char *dtname = NULL;
  const int c = componentCount(comps);
  if (!dtypeOf(depth, dt, dtname) || c == 0) {
    raiseError(PyExc_ValueError, "clip " + getName() + ": can't make images of depth \"" + shortName("depth", depth) +
                                 "\" and components \"" + shortName("components", comps) + "\"");
  }
  if (x2 < x1) x2 = x1;
  if (y2 < y1) y2 = y1;
  return numpy().attr("empty")(nb::make_tuple(y2 - y1, x2 - x1, c), dtname);
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

/// make the C++ image of what get_image() returned (raises if it isn't a valid image)
static PyImage *makeImage(PyClipInstance &clip, nb::object r, OfxTime time, int view)
{
  PyEffectInstance *const effect = clip._effect;
  if (!nb::isinstance<ImageData>(r)) {
    raiseError(PyExc_TypeError, "clip " + clip.getName() + ": get_image() must return an ofxh.ImageData or None, not " +
                                typeName(r));
  }
  ImageData &d = nb::cast<ImageData &>(r);
  const std::string where = "clip " + clip.getName() + ": get_image(): ";
  const OfxRectI bounds = toRectI(d.bounds, "bounds");
  const std::string &depth = clip.getPixelDepth();
  const std::string &comps = clip.getComponents();
  const int nComps = componentCount(comps);
  nb::dlpack::dtype dt;
  const char *dtname = NULL;
  if (!dtypeOf(depth, dt, dtname) || nComps == 0) {
    raiseError(PyExc_ValueError, where + "the clip's depth \"" + shortName("depth", depth) + "\" and components \"" +
                                 shortName("components", comps) + "\" are not supported");
  }
  if (!nb::isinstance(d.pixels, numpy().attr("ndarray"))) {
    raiseError(PyExc_TypeError, where + "pixels must be a NumPy array");
  }
  nb::ndarray<nb::ro> a;
  if (!nb::try_cast(d.pixels, a, false)) {
    raiseError(PyExc_TypeError, where + "pixels: unsupported NumPy array");
  }
  const int width = bounds.x2 - bounds.x1, height = bounds.y2 - bounds.y1;
  if (a.ndim() != 3 || width < 0 || height < 0 || (long long)a.shape(0) != height ||
      (long long)a.shape(1) != width || (int)a.shape(2) != nComps) {
    std::ostringstream ss;
    ss << where << "pixels must have the shape (y2-y1, x2-x1, " << nComps << ") = (" << height << ", "
       << width << ", " << nComps << ") of the bounds and the clip's components ("
       << shortName("components", comps) << "), not (";
    for (size_t i = 0; i < a.ndim(); ++i) {
      ss << (i ? ", " : "") << a.shape(i);
    }
    ss << ")";
    raiseError(PyExc_ValueError, ss.str());
  }
  if (a.dtype() != dt) {
    raiseError(PyExc_ValueError, where + "pixels must be " + dtname + ", the clip's depth (" +
                                 shortName("depth", depth) + "), not " +
                                 nb::cast<std::string>(nb::str(nb::handle(nb::object(d.pixels.attr("dtype"))))));
  }
  if ((nComps > 1 && a.stride(2) != 1) || (width > 1 && a.stride(1) != nComps)) {
    raiseError(PyExc_ValueError, where + "the pixels of each row must be contiguous (strides[1] == C*itemsize, "
                                 "strides[2] == itemsize)");
  }
  // the plug-in gets a pointer to non-const data, for the inputs too: no read-only arrays
  if (!nb::cast<bool>(d.pixels.attr("flags").attr("writeable"))) {
    raiseError(PyExc_ValueError, where + "the pixels must be writable (the plug-in gets a non-const pointer): "
                                 "pass a copy of a read-only array");
  }
  if (clip.isOutput() && effect->_rendering) {
    const OfxRectI &w = effect->_renderWindow;
    if (bounds.x1 > w.x1 || bounds.y1 > w.y1 || bounds.x2 < w.x2 || bounds.y2 < w.y2) {
      std::ostringstream ss;
      ss << where << "the bounds (" << bounds.x1 << ", " << bounds.y1 << ", " << bounds.x2 << ", " << bounds.y2
         << ") of the output image must contain the render window (" << w.x1 << ", " << w.y1 << ", " << w.x2
         << ", " << w.y2 << ")";
      raiseError(PyExc_ValueError, ss.str());
    }
  }
  const long long rowBytes = (long long)a.stride(0) * (long long)a.itemsize();
  if (rowBytes > INT_MAX || rowBytes < -INT_MAX) {
    raiseError(PyExc_ValueError, where + "the rows are too large");
  }
  void *data = const_cast<void *>(static_cast<const void *>(a.data()));
  const double par = clip.getAspectRatio();
  const OfxRectI rodPixels = d.rod.is_none() ? canonicalToPixelEnclosing(clip.rod(time, view), effect->_renderScale, par)
                                             : toRectI(d.rod, "rod");
  std::string uid = d.uniqueId;
  if (uid.empty()) {
    std::ostringstream ss;  // as hostDemo
    ss << clip.getName() << ":" << time << ":" << view << (clip.isOutput() ? ":output" : "");
    uid = ss.str();
  }
  if (clip.isOutput()) {
    fillSentinel(static_cast<unsigned char *>(data), rowBytes, width, height, nComps, depth);
  }
  PyImage *img = new PyImage(clip, nb::borrow(d.pixels), data, effect->_renderScale, bounds, rodPixels,
                             (int)rowBytes, fieldName(d.field), uid);
  if (clip.isOutput()) {
    // the clip keeps a reference until the end of the render (releaseOutputImage)
    clip._outputImage = img;
    clip._outputData = r;
    img->addReference();
  }
  return img;
}

OHIE::Image *PyClipInstance::fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds)
{
  OHIE::Image *img = fetchFromPython(time, view, optionalBounds);
  if (img || !isOutput() || !_effect->_rendering) {
    return img;  // (an input may have no image: the plug-in checks)
  }
  // During a render, the output image is never NULL (plug-ins don't check it): the host makes one
  // over the render window, the plug-in is asked to abort, and the render action fails.
  OFXH_CALL((OHIE::Image *)NULL,
    saveError(("clip " + getName() + ": get_image() gave no valid output image during the render: ofxh made one "
               "over the render window, and the render fails").c_str());
    _effect->_outputFailed = true;
    const OfxRectI &w = _effect->_renderWindow;
    ImageData d;
    d.pixels = newImage(w.x1, w.y1, w.x2, w.y2);
    d.bounds = nb::make_tuple(w.x1, w.y1, w.x2, w.y2);
    d.rod = nb::none();
    d.field = "none";
    return (OHIE::Image *)makeImage(*this, nb::cast(std::move(d)), time, view);
  )
}

OHIE::Image *PyClipInstance::fetchFromPython(OfxTime time, int view, const OfxRectD *optionalBounds)
{
  OFXH_CALL((OHIE::Image *)NULL,
    if (isOutput() && _outputImage) {
      // one output image per render, shared by all its fetches (whatever their view: see the
      // ClipInstance docstring)
      _outputImage->addReference();
      return (OHIE::Image *)_outputImage;
    }
    nb::object b = optionalBounds ? nb::object(nb::make_tuple(optionalBounds->x1, optionalBounds->y1,
                                                              optionalBounds->x2, optionalBounds->y2))
                                  : nb::none();
    // (part 1: the colour plane only; the keyword is there for the planes of a later part)
    nb::object r = _py.attr("get_image")(time, view, b, nb::arg("plane") = "Colour");
    if (r.is_none()) {
      return (OHIE::Image *)NULL;
    }
    return (OHIE::Image *)makeImage(*this, r, time, view);
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
    // the output RoD is the effect's RoD: an action, which a worker thread may not run
    checkThread("region_of_definition() of the output clip (the plug-in's RoD action)", true);
    // (the guard protects against a plug-in that asks for the output RoD in its own RoD action;
    // per thread)
    static thread_local std::set<const PyClipInstance *> inRoDAction;
    if (inRoDAction.count(this)) {
      return v;
    }
    inRoDAction.insert(this);
    OfxRectD r = v;
    {
      // (called by the default region_of_definition(), with the GIL: the plug-in runs without it;
      // the timeline at the RoD's time, restored when nested, as the RoD action binding)
      TimeLineScope tl(_effect, time);
      const OfxStatus st = withoutGil([&] {
        return _effect->getRegionOfDefinitionAction(time, _effect->_renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                    view,
#endif
                                                    r);
      });
      if (st == kOfxStatOK || st == kOfxStatReplyDefault) {
        v = r;
      }
    }
    inRoDAction.erase(this);
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
  return cached(_unmappedDepth, depthName(attrString(_py, "unmapped_bit_depth", kOfxBitDepthByte)));
}

const std::string &PyClipInstance::getUnmappedComponents() const
{
  // "kOfxImageComponentNone (implying a clip is unconnected, not valid for an image)"
  static const std::string none(kOfxImageComponentNone);
  if (!getConnected()) {
    return none;
  }
  return cached(_unmappedComponents, componentsName(attrString(_py, "unmapped_components", kOfxImageComponentRGBA)));
}

const std::string &PyClipInstance::getPremult() const
{
  return cached(_premult, premultName(attrString(_py, "premult", kOfxImageUnPreMultiplied)));
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
  return cached(_fieldOrder, fieldName(attrString(_py, "field_order", kOfxImageFieldNone)));
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
  // (the output clip gives its image of the current render, whatever the view)
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
  , _outputFailed(false)
  , _paramChangedCount(0)
{
  _renderScale.x = _renderScale.y = 1.;
  _renderWindow.x1 = _renderWindow.y1 = _renderWindow.x2 = _renderWindow.y2 = 0;
  _ref->p = this;
  openInstances().insert(this);
}

PyEffectInstance::~PyEffectInstance()
{
  // A safety net, which ofxh doesn't reach: close() and shutdown() send kOfxActionDestroyInstance
  // before they delete the instance, and HostSupport deletes an instance itself only when
  // populate() fails, before kOfxActionCreateInstance. Without it, ~Instance() would send the
  // action after the clips and parameters are deleted below. (Without the GIL if the caller holds
  // it, as the other actions; HostSupport's own deletes run without it.)
  if (_created) {
    if (nb::is_alive() && PyGILState_Check()) {
      withoutGil([&] { return destroyInstanceAction(); });
    } else {
      destroyInstanceAction();
    }
  }
  openInstances().erase(this);
  // first, the Python objects of the instance, of its clips and of its parameters become deleted
  _ref->p = nullptr;
  for (std::map<std::string, OHIE::ClipInstance *>::iterator it = _clips.begin(); it != _clips.end(); ++it) {
    if (PyClipInstance *c = dynamic_cast<PyClipInstance *>(it->second)) {
      c->_ref->p = nullptr;
    }
  }
  for (std::list<OH::Param::Instance *>::iterator it = _paramList.begin(); it != _paramList.end(); ++it) {
    if (PyParamCore *c = dynamic_cast<PyParamCore *>(*it)) {
      c->_ref->p = nullptr;
    }
  }
  // then the clips and parameters are deleted (here, not by ~Instance() and ~SetInstance(), so
  // that the instance's Python object goes last)
  if (_ownsData) {
    for (std::map<std::string, OHIE::ClipInstance *>::iterator it = _clips.begin(); it != _clips.end(); ++it) {
      delete it->second;
      it->second = NULL;
    }
  }
  if (_ownsParams) {
    for (std::list<OH::Param::Instance *>::iterator it = _paramList.begin(); it != _paramList.end(); ++it) {
      delete *it;
    }
    _paramList.clear();
    _params.clear();
  }
  dropLast(_py);
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
  return cached(_fielding, fieldName(attrString(_py, "default_output_fielding", kOfxImageFieldNone)));
}

OHIE::ClipInstance *PyEffectInstance::newClipInstance(OHIE::Instance * /*plugin*/, OHIE::ClipDescriptor *descriptor,
                                                      int index)
{
  OFXH_CALL((OHIE::ClipInstance *)NULL,
    nb::object py = _py.attr("new_clip_instance")(makeClipDescriptor(*descriptor), index);
    if (!nb::isinstance<ClipBase>(py)) {
      raiseError(PyExc_TypeError, "new_clip_instance() must return an ofxh.ClipInstance, not " + typeName(py));
    }
    ClipBase &cb = nb::cast<ClipBase &>(py);
    if (cb.ref->p) {
      raiseError(PyExc_ValueError, "new_clip_instance() returned a clip that already belongs to an instance");
    }
    return (OHIE::ClipInstance *)new PyClipInstance(this, *descriptor, py, cb.ref);
  )
}

OfxStatus PyEffectInstance::vmessage(const char *type, const char *id, const char *format, va_list args)
{
  return callMessage(_py, "message", type, id, format, args);
}

OfxStatus PyEffectInstance::setPersistentMessage(const char *type, const char *id, const char *format, va_list args)
{
  return callMessage(_py, "persistent_message", type, id, format, args);
}

OfxStatus PyEffectInstance::clearPersistentMessage()
{
  OFXH_CALL(StatusFallback{kOfxStatFailed}, return statusFromResult(_py.attr("clear_persistent_message")());)
}

int PyEffectInstance::abort() { return (_abort.load() || _outputFailed) ? 1 : 0; }

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
    const std::string &n = cached(_viewNames[viewIndex], nb::cast<std::string>(_py.attr("view_name")(viewIndex)));
    *name = n.c_str();  // "valid throughout the calling action"
    return kOfxStatOK;
  )
}
#endif

OH::Param::Instance *PyEffectInstance::newParam(const std::string &name, OH::Param::Descriptor &descriptor)
{
  OFXH_CALL((OH::Param::Instance *)NULL,
    // new_param(descriptor): an ofxh.Param, or None for a C++ parameter that stores its value
    nb::object py = _py.attr("new_param")(makeParamDescriptor(descriptor, &getDescriptor()));
    OH::Param::Instance *p = NULL;
    if (py.is_none()) {
      p = newStoredParam(this, descriptor);
    } else {
      if (!nb::isinstance<ParamBase>(py)) {
        raiseError(PyExc_TypeError, "parameter " + name + ": new_param() must return an ofxh.Param or None, not " +
                                    typeName(py));
      }
      ParamBase &pb = nb::cast<ParamBase &>(py);
      if (pb.ref->p) {
        raiseError(PyExc_ValueError, "parameter " + name + ": new_param() returned a parameter that is already bound");
      }
      p = newPyParam(this, descriptor, py, pb.ref);
    }
    if (!p) {
      raiseError(PyExc_NotImplementedError, "parameter " + name + ": type " + shortName("param_type", descriptor.getType()) +
                                            " is not supported (parametric parameters need OFX_SUPPORTS_PARAMETRIC "
                                            "and a host implementation)");
    }
    return p;
  )
}

OfxStatus PyEffectInstance::paramInstanceChangedAction(const std::string &paramName, const std::string &why,
                                                       OfxTime time, OfxPointD renderScale)
{
  ++_paramChangedCount;
  return OHIE::Instance::paramInstanceChangedAction(paramName, why, time, renderScale);
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
  const std::pair<double, double> r = callPair(_py, "timeline_get_bounds", std::make_pair(0., 25.));
  t1 = r.first;
  t2 = r.second;
}

}  // namespace ofxh
