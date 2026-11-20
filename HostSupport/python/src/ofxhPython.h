// Python bindings of HostSupport (nanobind): the adapters and the shared machinery.
//
// Lifetime model:
// - HostSupport deletes the clip, param and image objects it gets from the factory methods with
//   plain delete, so the objects it owns are C++ adapters (PyHost, PyEffectInstance,
//   PyClipInstance, PyImage), allocated with new. Each adapter holds a strong reference (_py) to
//   an instance of a user subclass of a small Python base class (ofxh.Host,
//   ofxh.ImageEffectInstance, ofxh.ClipInstance), and implements each virtual function by calling
//   the Python object.
// - The Python object holds only a Ref, shared with the adapter, which doesn't own the adapter.
//   The adapter's destructor sets Ref::p to NULL before it drops its reference to the Python
//   object: from then on, the Python object raises ReferenceError ("deleted by HostSupport").
// - An effect instance lives until ImageEffectInstance.close() or ofxh.shutdown(), which send
//   kOfxActionDestroyInstance and delete it (with its clips and params).
// - No exception may cross a HostSupport or plug-in frame: every callback is wrapped in
//   OFXH_CALL, which turns a Python exception into a status (ofxh.OfxError(status) gives that
//   status, anything else kOfxStatFailed or a safe default) and saves the first error, which the
//   Python binding that called into HostSupport re-raises as ofxh.CallbackError (ActionScope).
//   A callback that fails outside any ActionScope (e.g. called by a plug-in through ctypes)
//   reports its error with sys.unraisablehook.
// - The GIL: the bindings release it around the HostSupport calls that run plug-in code (the
//   actions, the plug-in scan, create_instance, the default RoD of the output clip), and every
//   callback (OFXH_CALL) and every adapter destructor that drops a Python object (dropLast)
//   takes it again: no adapter touches a Python object without it.
// - Threads: ofxh is single-threaded. While a binding runs on one thread (the owner thread, see
//   ActionScope), another thread may call a binding only from inside a callback (a plug-in's
//   worker thread that called the host): the bindings that read or call into HostSupport
//   without running an action (OFXH_SCOPED, the getters), with their own pending error. The
//   actions and set_param() raise RuntimeError there, and so does any binding called from a
//   thread outside a callback (checkThread()).

#ifndef OFXH_PYTHON_H
#define OFXH_PYTHON_H

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <atomic>
#include <cstdarg>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxhImageEffect.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhPluginCache.h"
#include "ofxhClip.h"
#include "ofxhParam.h"

namespace ofxh {

namespace nb = nanobind;
namespace OH = OFX::Host;
namespace OHIE = OFX::Host::ImageEffect;

// ---------------------------------------------------------------------------------------------
// References from Python objects to the C++ objects owned by HostSupport

/// What a Python base object knows of its adapter: NULL until it is bound, and again once
/// HostSupport (or close()/shutdown()) deleted the adapter.
struct Ref {
  void *p = nullptr;
};
typedef std::shared_ptr<Ref> RefPtr;

/// raise ReferenceError: the C++ object was deleted (or was never created)
[[noreturn]] void raiseDeleted(const char *what);
/// raise a Python exception (PyExc_ValueError...) with this message
[[noreturn]] void raiseError(PyObject *type, const std::string &message);

class PyHost;
class PyEffectInstance;
class PyClipInstance;
class PyParamCore;

/// the C++ part of ofxh.Host
struct HostBase {
  RefPtr ref = std::make_shared<Ref>();
  PyHost *get() const;
};

/// the C++ part of ofxh.ImageEffectInstance
struct InstanceBase {
  RefPtr ref = std::make_shared<Ref>();
  PyEffectInstance *get() const;
};

/// the C++ part of ofxh.ClipInstance
struct ClipBase {
  RefPtr ref = std::make_shared<Ref>();
  PyClipInstance *get() const;
};

/// the C++ part of ofxh.Param (Ref::p is the OH::Param::Instance of the adapter)
struct ParamBase {
  RefPtr ref = std::make_shared<Ref>();
  OH::Param::Instance *get() const;
  PyParamCore *core() const;
};

/// ofxh.PropertySet: the properties of the host, an instance, a clip or a parameter instance
struct PropertySetView {
  RefPtr ref;
  int kind;  ///< 0: host, 1: instance, 2: clip, 3: parameter
  OH::Property::Set &get() const;
};

/// ofxh.ImageData: what ClipInstance.get_image() returns
struct ImageData {
  nb::object pixels;     ///< NumPy array, (y2-y1, x2-x1, C), row 0 is y1
  nb::object bounds;     ///< (x1, y1, x2, y2), pixels at the render scale
  nb::object rod;        ///< (x1, y1, x2, y2) or None: the clip's RoD in pixels
  std::string field;     ///< "none", "both", "lower", "upper" or the kOfxImageField* value
  std::string uniqueId;  ///< "": made from the clip name, time and view
};

// ---------------------------------------------------------------------------------------------
// Errors

/// Python objects made by the module prelude (Stat, OfxError, CallbackError, ...)
struct Globals {
  nb::object statFn;             ///< ofxh._stat(int) -> Stat or int
  nb::object ofxError;           ///< ofxh.OfxError
  nb::object raiseCallbackError; ///< ofxh._raise_callback_error(where, exc, status)
  nb::object numpy;              ///< the numpy module (imported on first use)
};
Globals &globals();
nb::object numpy();

/// the status as an ofxh.Stat (or a plain int if it isn't one)
nb::object stat(OfxStatus s);

/// save the first error of the current action (see ActionScope); outside any action, report it
/// with sys.unraisablehook
void saveError(nb::handle exc);
void saveError(const char *message);

/// Saves the pending error of an enclosing action when a Python binding calls into HostSupport,
/// and re-raises the first error saved by the callbacks, as ofxh.CallbackError, in check().
/// The scopes nest: depth() > 0 inside an action or a callback of this thread. Made with the GIL.
/// The owner thread (the one of the open scopes) shares one pending error; a worker thread (a
/// plug-in's thread inside a callback) has its own. The constructor raises RuntimeError on a
/// thread other than the owner's while a scope is open there, unless fromWorker (OFXH_SCOPED: the
/// bindings that don't run an action) and that thread is inside a callback (checkThread()).
class ActionScope {
  PyObject *_saved;      ///< the pending error of the enclosing scope (a reference)
  PyObject **_slot;      ///< the pending error of this thread's scopes
  PyObject **_prevSlot;  ///< this thread's current slot before this scope (restored by discard())
  bool _owner;
  bool _done;
public:
  explicit ActionScope(bool fromWorker = false);
  ~ActionScope();
  ActionScope(const ActionScope &) = delete;
  ActionScope &operator=(const ActionScope &) = delete;
  /// raise ofxh.CallbackError("<where>: ...") from the first error, if a callback failed (with
  /// the action's status, if given)
  void check(const char *where, const OfxStatus *status = NULL);
  /// did a callback fail since the scope began?
  bool failed() const;
  /// forget the errors of the callbacks (and restore the enclosing action's)
  void discard();
  /// the number of open scopes of this thread
  static int depth();
  /// is a scope open on the owner thread (an action or a binding that calls into HostSupport)?
  static bool running();
  /// is this a worker thread (another thread than the owner's, while a scope is open there)?
  static bool onWorker();
};

/// The thread rule of the bindings, which every binding of an object checks (the get() of the
/// Python objects): while a scope is open on the owner thread, another thread raises RuntimeError,
/// unless it is inside a callback (a plug-in's worker thread) and !action. action: the bindings
/// that run an action or change a value (set_param()).
void checkThread(const char *what, bool action);

/// evaluate an expression that may call the Python callbacks in its own ActionScope, and raise
/// their first error (used by the bindings that don't run an action: a plug-in's worker thread
/// inside a callback may call them). If the expression raises
/// after a callback failed (e.g. a bad status), the callback's error is raised instead.
#define OFXH_SCOPED(where, ...)                                                        \
  ([&]() {                                                                             \
    ::ofxh::ActionScope ofxh_s_(true);                                                 \
    try {                                                                              \
      auto ofxh_r_ = (__VA_ARGS__);                                                    \
      ofxh_s_.check(where);                                                            \
      return ofxh_r_;                                                                  \
    } catch (::nanobind::python_error &) {                                             \
      if (ofxh_s_.failed()) {                                                          \
        ofxh_s_.check(where);                                                          \
      }                                                                                \
      throw;                                                                           \
    }                                                                                  \
  }())

/// counts the callbacks (OFXH_CALL) that run on this thread: a thread inside a callback of the
/// running action (e.g. a plug-in's worker thread) may call the bindings (ActionScope)
struct CallbackDepth {
  CallbackDepth() { ++depth(); }
  ~CallbackDepth() { --depth(); }
  static int &depth();
};

/// fallback tag for the functions that return an OfxStatus: ofxh.OfxError(status) gives status
struct StatusFallback {
  OfxStatus status;
};
/// fallback tag for the void functions
struct Void {};

OfxStatus onPythonError(nb::python_error &e, StatusFallback fb);
void onPythonError(nb::python_error &e, Void);
template <class T> T onPythonError(nb::python_error &e, T fb)
{
  saveError(e.value());
  return fb;
}
inline OfxStatus onCxxError(const char *what, StatusFallback fb)
{
  saveError(what);
  return fb.status;
}
inline void onCxxError(const char *what, Void) { saveError(what); }
template <class T> T onCxxError(const char *what, T fb)
{
  saveError(what);
  return fb;
}

/// Run a callback body (statements that return the result) with the GIL, and turn any exception
/// into the fallback: StatusFallback{s} for an OfxStatus, Void{} for a void function, else the
/// value to return. Nothing propagates to the caller (HostSupport or the plug-in).
#define OFXH_CALL(fallback, ...)                                                       \
  ::nanobind::gil_scoped_acquire ofxh_gil_;                                            \
  ::ofxh::CallbackDepth ofxh_depth_;                                                   \
  try {                                                                                \
    __VA_ARGS__                                                                        \
  } catch (::nanobind::python_error & ofxh_e_) {                                       \
    return ::ofxh::onPythonError(ofxh_e_, fallback);                                   \
  } catch (const std::exception &ofxh_e_) {                                            \
    return ::ofxh::onCxxError(ofxh_e_.what(), fallback);                               \
  } catch (...) {                                                                      \
    return ::ofxh::onCxxError("unknown C++ exception", fallback);                      \
  }

/// a status from what a Python callback returned: None or anything but an int is kOfxStatOK
OfxStatus statusFromResult(nb::handle r);

/// drop the last reference of an adapter to its Python object (with the GIL; leaked if the
/// interpreter is gone)
void dropLast(nb::object &py);

/// call f() without the GIL: the HostSupport calls that run plug-in code (every callback takes
/// the GIL again). The caller holds the GIL, and keeps its Python objects alive.
template <class F> auto withoutGil(F &&f) -> decltype(f())
{
  nb::gil_scoped_release release;
  return f();
}

// ---------------------------------------------------------------------------------------------
// Names: Python reads short names (depth "byte", components "rgba", context "filter"...) and
// accepts them or the OFX strings; HostSupport gets the OFX strings.
// Kinds: depth, components, premult, field, context, message, change, param_type, plane.

/// the OFX string of a short name (or of an OFX string: unchanged); unknown names are unchanged
std::string ofxName(const char *kind, const std::string &s);
/// the short name of an OFX string; unknown strings are unchanged
std::string shortName(const char *kind, const std::string &s);
inline std::string depthName(const std::string &s) { return ofxName("depth", s); }
inline std::string componentsName(const std::string &s) { return ofxName("components", s); }
inline std::string premultName(const std::string &s) { return ofxName("premult", s); }
inline std::string fieldName(const std::string &s) { return ofxName("field", s.empty() ? std::string("none") : s); }
int componentCount(const std::string &components); ///< 4, 3, 1, or 0 if unsupported
/// can ofxh make images of this (OFX) depth and these components?
bool imageFormatSupported(const std::string &depth, const std::string &components);

/// hostDemo's canonicalToPixelEnclosing (clamped to the int range)
OfxRectI canonicalToPixelEnclosing(const OfxRectD &r, const OfxPointD &renderScale, double par);
OfxRectI intersect(const OfxRectI &a, const OfxRectI &b);

// ---------------------------------------------------------------------------------------------
// Adapters

/// ofxh.Host: the image effect host
class PyHost : public OHIE::Host {
public:
  nb::object _py;
  RefPtr _ref;

  bool _ofx14 = false;  ///< the OFX 1.4 host profile (ofxh.init(profile="ofx14")), see restrictToOFX14()

  PyHost(nb::object py, RefPtr ref);
  virtual ~PyHost();

  /// the OFX 1.4 host profile, as hostDemo --ofx14 (hostDemoHostDescriptor.cpp): remove the host
  /// properties that OFX 1.4 doesn't have; fetchSuite() then gives only the OFX 1.4 suites, and
  /// OfxSetHost isn't called. Before the plug-in scan.
  void restrictToOFX14();
  virtual const void *fetchSuite(const char *suiteName, int suiteVersion);
  virtual bool supportsOfxSetHost() const { return !_ofx14; }

  virtual OHIE::Instance *newInstance(void *clientData, OHIE::ImageEffectPlugin *plugin,
                                      OHIE::Descriptor &desc, const std::string &context);
  virtual OHIE::Descriptor *makeDescriptor(OHIE::ImageEffectPlugin *plugin);
  virtual OHIE::Descriptor *makeDescriptor(const OHIE::Descriptor &rootContext,
                                           OHIE::ImageEffectPlugin *plugin);
  virtual OHIE::Descriptor *makeDescriptor(const std::string &bundlePath,
                                           OHIE::ImageEffectPlugin *plugin);
  virtual OfxStatus vmessage(const char *type, const char *id, const char *format, va_list args);
  virtual OfxStatus setPersistentMessage(const char *type, const char *id, const char *format,
                                         va_list args);
  virtual OfxStatus clearPersistentMessage();
#ifdef OFX_SUPPORTS_OPENGLRENDER
  virtual OfxStatus flushOpenGLResources() const { return kOfxStatFailed; }
#endif
};

/// a C++ image whose pixels are a NumPy array, kept alive until HostSupport deletes the image
class PyImage : public OHIE::Image {
  nb::object _pixels;
public:
  PyImage(OHIE::ClipInstance &clip, nb::object pixels, void *data, const OfxPointD &renderScale,
          const OfxRectI &bounds, const OfxRectI &rod, int rowBytes, const std::string &field,
          const std::string &uniqueId);
  virtual ~PyImage();
};

/// ofxh.ClipInstance
class PyClipInstance : public OHIE::ClipInstance {
public:
  PyEffectInstance *_effect;
  nb::object _py;
  RefPtr _ref;
  int _view;                   ///< the view of getImage() (setView)
  PyImage *_outputImage;       ///< the output image of the current render (output clip only)
  nb::object _outputData;      ///< its ImageData
  // the strings returned by reference
  mutable std::string _unmappedDepth, _unmappedComponents, _premult, _fieldOrder;

  PyClipInstance(PyEffectInstance *effect, OHIE::ClipDescriptor &desc, nb::object py, RefPtr ref);
  virtual ~PyClipInstance();

  /// release the output image of the last render (the next fetch gets a new one)
  void releaseOutputImage();
  /// call the Python get_image() and make a C++ image from what it returns (NULL if None). The
  /// output image of a render is never NULL (see fetchImage()).
  OHIE::Image *fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds);
  OHIE::Image *fetchFromPython(OfxTime time, int view, const OfxRectD *optionalBounds);
  /// the clip's RoD in canonical coordinates, from the Python region_of_definition()
  OfxRectD rod(OfxTime time, int view) const;
  /// the default RoD (hostDemo's): the effect's RoD for the output clip, the project for a
  /// connected input, empty for a disconnected one
  OfxRectD defaultRoD(OfxTime time, int view) const;
  /// make an empty image of the clip's mapped depth and components
  nb::object newImage(int x1, int y1, int x2, int y2) const;

  // overrides (pure virtuals of ClipInstance)
  virtual const std::string &getPixelDepth() const;
  virtual const std::string &getComponents() const;
  virtual const std::string &getUnmappedBitDepth() const;
  virtual const std::string &getUnmappedComponents() const;
  virtual const std::string &getPremult() const;
#ifdef OFX_EXTENSIONS_NATRON
  virtual OfxRectI getFormat() const;
#endif
  virtual double getAspectRatio() const;
  virtual double getFrameRate() const;
  virtual void getFrameRange(double &startFrame, double &endFrame) const;
  virtual const std::string &getFieldOrder() const;
  virtual bool getConnected() const;
  virtual double getUnmappedFrameRate() const;
  virtual void getUnmappedFrameRange(double &unmappedStartFrame, double &unmappedEndFrame) const;
  virtual bool getContinuousSamples() const;
  virtual OHIE::Image *getImage(OfxTime time, const OfxRectD *optionalBounds);
#ifdef OFX_EXTENSIONS_NUKE
  virtual OHIE::Image *getImagePlane(OfxTime time, int view, const std::string &plane,
                                     const OfxRectD *optionalBounds);
  virtual OfxRectD getRegionOfDefinition(OfxTime time, int view) const;
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
  virtual OHIE::Texture *loadTexture(OfxTime, const char *, const OfxRectD *) { return NULL; }
#endif
#ifdef OFX_EXTENSIONS_VEGAS
  virtual OHIE::Image *getStereoscopicImage(OfxTime time, int view, const OfxRectD *optionalBounds);
#endif
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
  virtual void setView(int view);
#endif
  virtual OfxRectD getRegionOfDefinition(OfxTime time) const;
};

/// ofxh.ImageEffectInstance
class PyEffectInstance : public OHIE::Instance {
public:
  nb::object _py;
  RefPtr _ref;
  // the render state, set by the action bindings
  bool _rendering;
  OfxPointD _renderScale;         ///< the render scale of the current action (1, 1 outside)
  OfxRectI _renderWindow;
  double _timeLineTime;           ///< the default timeline (timeline_get_time/goto_time)
  std::atomic<int> _abort;        ///< ImageEffectInstance.aborted
  bool _outputFailed;             ///< the host made the output image of the render (abort() is 1)
  int _paramChangedCount;         ///< the kOfxActionInstanceChanged sent for parameters
  mutable std::string _fielding;  ///< getDefaultOutputFielding()
  mutable std::map<int, std::string> _viewNames;  ///< getViewName()

  PyEffectInstance(OHIE::ImageEffectPlugin *plugin, OHIE::Descriptor &desc,
                   const std::string &context, nb::object py, RefPtr ref);
  virtual ~PyEffectInstance();

  const std::map<std::string, OHIE::ClipInstance *> &clips() const { return _clips; }
  /// the project rectangle in pixels at the given render scale
  OfxRectI projectPixelRect(const OfxPointD &renderScale) const;

  // ImageEffect::Instance
  virtual const std::string &getDefaultOutputFielding() const;
  /// HostSupport's, then default_output_depth (if not None) for the output when no input is
  /// connected (as hostDemo, which uses its -d depth there)
  virtual void setDefaultClipPreferences();
  virtual OHIE::ClipInstance *newClipInstance(OHIE::Instance *plugin, OHIE::ClipDescriptor *descriptor,
                                              int index);
  virtual OfxStatus vmessage(const char *type, const char *id, const char *format, va_list args);
  virtual OfxStatus setPersistentMessage(const char *type, const char *id, const char *format,
                                         va_list args);
  virtual OfxStatus clearPersistentMessage();
  virtual int abort();
  virtual void getProjectSize(double &xSize, double &ySize) const;
  virtual void getProjectOffset(double &xOffset, double &yOffset) const;
  virtual void getProjectExtent(double &xSize, double &ySize) const;
  virtual double getProjectPixelAspectRatio() const;
  virtual double getEffectDuration() const;
  virtual double getFrameRate() const;
  virtual double getFrameRecursive() const;
  virtual void getRenderScaleRecursive(double &x, double &y) const;
#ifdef OFX_EXTENSIONS_NUKE
  virtual OfxStatus getViewCount(int *nViews) const;
  virtual OfxStatus getViewName(int viewIndex, const char **name) const;
#endif
  // Param::SetInstance
  virtual OH::Param::Instance *newParam(const std::string &name, OH::Param::Descriptor &descriptor);
  virtual OfxStatus editBegin(const std::string &name);
  virtual OfxStatus editEnd();
  // (counts them: param_changed_count)
  virtual OfxStatus paramInstanceChangedAction(const std::string &paramName, const std::string &why, OfxTime time,
                                               OfxPointD renderScale);
  // Progress::ProgressI
  virtual void progressStart(const std::string &message, const std::string &messageid);
  virtual void progressEnd();
  virtual bool progressUpdate(double t);
  // TimeLine::TimeLineI
  virtual double timeLineGetTime();
  virtual void timeLineGotoTime(double t);
  virtual void timeLineGetBounds(double &t1, double &t2);
};

/// what the Python parameters (ofxh.Param subclasses, made by new_param()) share: the adapter of
/// each parameter type derives from its HostSupport instance class and from this
class PyParamCore {
public:
  PyEffectInstance *_effect;
  nb::object _py;
  RefPtr _ref;
  OH::Param::Instance *_self;

  PyParamCore(PyEffectInstance *effect, OH::Param::Instance *self, nb::object py, RefPtr ref);
  virtual ~PyParamCore();
};

/// the Python parameter (an ofxh.Param subclass) of a descriptor's type, bound to py, or NULL if
/// the type is not supported
OH::Param::Instance *newPyParam(PyEffectInstance *effect, OH::Param::Descriptor &descriptor, nb::object py,
                                RefPtr ref);

/// The default timeline at the time of an action (as hostDemo, which sets it before each
/// action): a top-level action leaves it there; an action nested in another one (called by a
/// callback, or by a worker thread) restores the time of the outer action. Made after the
/// action's ActionScope (or inside an OFXH_SCOPED).
struct TimeLineScope {
  PyEffectInstance *p;
  double saved;
  bool nested;
  TimeLineScope(PyEffectInstance *inst, double time)
    : p(inst), saved(inst->_timeLineTime), nested(ActionScope::depth() > 1 || ActionScope::onWorker())
  {
    p->_timeLineTime = time;
  }
  ~TimeLineScope()
  {
    if (nested) {
      p->_timeLineTime = saved;
    }
  }
};

/// the open instances, closed by shutdown()
std::set<PyEffectInstance *> &openInstances();

/// format a printf-style message
std::string formatMessage(const char *format, va_list args);

/// ofxh.ClipDescriptor: what new_clip_instance() is told about the clip (a copy)
nb::object makeClipDescriptor(const OHIE::ClipDescriptor &desc);

// ---------------------------------------------------------------------------------------------
// Descriptors (ofxhPythonDescribe.cpp): read-only copies, and hostDemo's --describe JSON

/// ofxh.ParamDescriptor: a copy of a parameter descriptor (effect: its effect descriptor, for the
/// page of the parameter; may be NULL)
nb::object makeParamDescriptor(const OH::Param::Descriptor &desc, const OHIE::Descriptor *effect);
/// ofxh.EffectDescriptor: a copy of the plug-in's descriptor in a context ("" : the descriptor of
/// the describe action)
nb::object makeEffectDescriptor(OHIE::ImageEffectPlugin *plugin, const std::string &context);
/// the properties of a set, as a dict {name: tuple of values} (a copy)
nb::dict propsDict(const OH::Property::Set &props);
/// the default (kOfxParamPropDefault) of a parameter (descriptor or instance) of this type, in the
/// form of getParamValue() (a tuple for several dimensions, bytes...), or None
nb::object defaultValue(const OH::Property::Set &props, const std::string &type);
/// the page parameter that lists name as a child, or ""
std::string pageOf(const OHIE::Descriptor &effect, const std::string &name);
/// ofxh.describe(): hostDemo's --describe JSON, as Python objects
nb::dict describePlugin(OHIE::ImageEffectPlugin *plugin, const std::string &context);
/// ofxh.param_template(): the defaults of the parameters with a value, {name: value}, as
/// JSON-serialisable data (normalised spatial defaults in canonical coordinates for this project)
nb::dict paramTemplate(OHIE::ImageEffectPlugin *plugin, const std::string &context, double w, double h, double ox,
                       double oy);
/// a Python value as JSON-serialisable data: tuples become lists, bytes lists of ints
nb::object jsonValue(nb::handle v);
void bindDescriptors(nb::module_ &m);

// ---------------------------------------------------------------------------------------------
// Parameters (ofxhPythonParams.cpp): the C++ parameters that store their value (with keys and
// linear interpolation, as hostDemo's MyCurve), and the Python parameters (ofxh.Param)

/// the spatial default of a double parameter in canonical coordinates (for normalised
/// defaults, kOfxParamPropDefaultCoordinateSystem), with this project size and offset
void defaultToCanonical(const OH::Param::Base &descriptor, double w, double h, double ox, double oy,
                        double *v, int n);
/// a C++ parameter instance of the descriptor's type, with the default value
/// (kOfxParamPropDefault), or NULL if the type is not supported
OH::Param::Instance *newStoredParam(PyEffectInstance *effect, OH::Param::Descriptor &descriptor);
/// the value of a parameter, as a Python object (a tuple for multi-dimensional parameters)
nb::object getParamValue(OH::Param::Instance *param, const double *time);
/// set the value of a parameter from a Python object (at a time: a key if it animates)
OfxStatus setParamValue(OH::Param::Instance *param, nb::handle value, const double *time = NULL);
/// the value a parameter starts with: its default, with the normalised spatial defaults in
/// canonical coordinates for the effect's project (as the C++ parameters)
nb::object initialValue(OH::Param::Instance *param, const PyEffectInstance *effect);
/// the key times of a parameter (empty if it has none, or no keys)
std::vector<double> paramKeyTimes(OH::Param::Instance *param);
void bindParams(nb::module_ &m);

// ---------------------------------------------------------------------------------------------
/// bind the module (ofxhPythonModule.cpp)
void bindModule(nb::module_ &m);

}  // namespace ofxh

#endif  // OFXH_PYTHON_H
