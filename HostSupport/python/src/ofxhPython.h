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
//   The adapter's destructor sets Ref::p to nullptr before it drops its reference to the Python
//   object: from then on, the Python object raises ReferenceError ("deleted by HostSupport").
// - An effect instance lives until ImageEffectInstance.close() or ofxh.shutdown(), which send
//   kOfxActionDestroyInstance and delete it (with its clips and params).
// - No exception may cross a HostSupport or plug-in frame: every callback is wrapped in
//   callPython(), which turns a Python exception into a status (ofxh.OfxError(status) gives that
//   status, anything else kOfxStatFailed or a safe default) and saves the first error, which the
//   Python binding that called into HostSupport re-raises as ofxh.CallbackError (ActionScope).
//   A callback that fails outside any ActionScope (e.g. called by a plug-in through ctypes)
//   reports its error with sys.unraisablehook.
// - The GIL: the bindings release it around the HostSupport calls that run plug-in code (the
//   actions, the plug-in scan, create_instance, the default RoD of the output clip), and every
//   callback (callPython()) and every adapter destructor that drops a Python object (dropLast)
//   takes it again: no adapter touches a Python object without it.
// - Threads: ofxh is single-threaded. While a binding runs on one thread (the owner thread, see
//   ActionScope), another thread may call a binding only from inside a callback (a plug-in's
//   worker thread that called the host): the bindings that read or call into HostSupport
//   without running an action (scoped(), the getters), with their own pending error. The
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
#include <type_traits>
#include <vector>

#ifdef OFXH_OPENGL
#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLRender.h"
#endif
#ifdef OFXH_OPENCL
#include "ofxhOpenCLDevice.h"
#include "ofxhOpenCLRender.h"
#endif
#ifdef OFXH_METAL
#include "ofxhMetalDevice.h"
#include "ofxhMetalRender.h"
#endif
#ifdef OFXH_CUDA
#include "ofxhCUDADevice.h"
#include "ofxhCUDARender.h"
#endif

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

/// What a Python base object knows of its adapter: nullptr until it is bound, and again once
/// HostSupport (or close()/shutdown()) deleted the adapter. Untyped, because one Ref type serves
/// the adapters of all the Python base classes (HostBase, InstanceBase, ClipBase, ParamBase): each
/// reads it with as<its adapter type>().
struct Ref {
  void *p = nullptr;
  template <class T> T *as() const { return static_cast<T *>(p); }
};
using RefPtr = std::shared_ptr<Ref>;

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
  /// whose properties they are: the adapter type of ref
  enum class Kind { host, instance, clip, param };
  RefPtr ref;
  Kind kind;
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
/// thread other than the owner's while a scope is open there, unless fromWorker (scoped(): the
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
  void check(const char *where, const OfxStatus *status = nullptr);
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

/// return f(), which may call the Python callbacks, in its own ActionScope, and raise their first
/// error (used by the bindings that don't run an action: a plug-in's worker thread inside a
/// callback may call them). If f() raises after a callback failed (e.g. a bad status), the
/// callback's error is raised instead. Returns a value (decayed), never a reference into f's frame.
template <class F> auto scoped(const char *where, F &&f) -> std::decay_t<decltype(f())>
{
  ActionScope scope(true);
  try {
    auto r = f();
    scope.check(where);
    return r;
  } catch (nb::python_error &) {
    if (scope.failed()) {
      scope.check(where);
    }
    throw;
  }
}

/// counts the callbacks (callPython()) that run on this thread: a thread inside a callback of the
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

/// Run a callback body with the GIL and return what it returns, and turn any exception into
/// the fallback: StatusFallback{s} for an OfxStatus, Void{} for a void body, else the value to
/// return. Nothing propagates to the caller (HostSupport or the plug-in).
template <class Fallback, class F> auto callPython(Fallback fallback, F &&body) -> std::decay_t<decltype(body())>
{
  nb::gil_scoped_acquire gil;
  CallbackDepth depth;
  try {
    return body();
  } catch (nb::python_error &e) {
    return onPythonError(e, fallback);
  } catch (const std::exception &e) {
    return onCxxError(e.what(), fallback);
  } catch (...) {
    return onCxxError("unknown C++ exception", fallback);
  }
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
int componentCount(const std::string &components); ///< 4, 3, 2 (Nuke planes), 1, or 0 if unsupported
/// the components of the images of a plane other than the colour plane (an OFX name): those of the
/// Nuke extension's motion vector and disparity planes, "" for the others (ofxh makes no image of them)
std::string planeComponents(const std::string &plane);
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
  bool _keepLoaded = false;  ///< ofxh.init(keep_loaded=True): keepPluginsLoadedAfterScan()
  /// ofxh.init(normalised_defaults=False), as hostDemo --no-normalised-defaults: the double param
  /// descriptors have no kOfxParamPropDefaultCoordinateSystem (makeDescriptor())
  bool _normalisedDefaults = true;
#ifdef OFXH_OPENGL
  /// ofxh.init(opengl=True): the OpenGL context (one render at a time), or the reason there is none
  std::unique_ptr<OFX::Host::OpenGL::Context> _glContext;
  std::string _glError;
#endif
#ifdef OFXH_OPENCL
  /// ofxh.init(opencl=True): the OpenCL device (a context, and the queues lent to renders), or the
  /// reason there is none
  std::unique_ptr<OFX::Host::OpenCL::Device> _clDevice;
  std::string _clError;
#endif
#ifdef OFXH_METAL
  /// ofxh.init(metal=True): the Metal device (and the queues lent to renders), or the reason there is
  /// none
  std::unique_ptr<OFX::Host::Metal::Device> _mtDevice;
  std::string _mtError;
#endif
#ifdef OFXH_CUDA
  /// ofxh.init(cuda=True): the CUDA device (and the streams lent to renders), or the reason there is
  /// none
  std::unique_ptr<OFX::Host::CUDA::Device> _cuDevice;
  std::string _cuError;
#endif

  PyHost(nb::object py, RefPtr ref);
  ~PyHost() override;

  /// the OFX 1.4 host profile, as hostDemo --ofx14 (hostDemoHostDescriptor.cpp): remove the host
  /// properties that OFX 1.4 doesn't have; fetchSuite() then gives only the OFX 1.4 suites, and
  /// OfxSetHost isn't called. Before the plug-in scan.
  void restrictToOFX14();
  const void *fetchSuite(const char *suiteName, int suiteVersion) override;
  bool supportsOfxSetHost() const override { return !_ofx14; }
  /// kOfxParamPropColourManagement on RGB and RGBA params (so that the tests exercise it); false
  /// with the OFX 1.4 profile
  bool supportsParamColourManagement() const override { return !_ofx14; }
  /// ofxh.init(keep_loaded=True), as hostDemo with OFX_HOSTDEMO_KEEP_LOADED=1: the scanned
  /// plug-ins stay loaded (one Load and Describe), instead of being unloaded after the scan and
  /// loaded again at their first use
  bool keepPluginsLoadedAfterScan() const override { return _keepLoaded; }

  OHIE::Instance *newInstance(void *clientData, OHIE::ImageEffectPlugin *plugin,
                              OHIE::Descriptor &desc, const std::string &context) override;
  OHIE::Descriptor *makeDescriptor(OHIE::ImageEffectPlugin *plugin) override;
  OHIE::Descriptor *makeDescriptor(const OHIE::Descriptor &rootContext,
                                   OHIE::ImageEffectPlugin *plugin) override;
  OHIE::Descriptor *makeDescriptor(const std::string &bundlePath,
                                   OHIE::ImageEffectPlugin *plugin) override;
  OfxStatus vmessage(const char *type, const char *id, const char *format, va_list args) override;
  OfxStatus setPersistentMessage(const char *type, const char *id, const char *format,
                                 va_list args) override;
  OfxStatus clearPersistentMessage() override;
#ifdef OFX_SUPPORTS_OPENGLRENDER
  /// "kOfxStatReplyDefault - nothing the host could do.." (flushResources): one OpenGL context,
  /// which the calling render uses, and no other GPU resource kept (as hostDemo)
  OfxStatus flushOpenGLResources() const override { return kOfxStatReplyDefault; }
#endif
  /// the OpenCL program suite (OfxOpenCLProgramSuiteV1), with the device of ofxh.init(opencl=True):
  /// built for the context of the render's queue (OpenCL::Device::compileProgram()), as hostDemo
  bool supportsOpenCLProgramSuite() const override
  {
#ifdef OFXH_OPENCL
    return _clDevice != nullptr;
#else
    return false;
#endif
  }
  OfxStatus compileOpenCLProgram(const char *pszProgramSource, int fOptional, void *pResult) override
  {
#ifdef OFXH_OPENCL
    if (_clDevice) {
      return _clDevice->compileProgram(OFX::Host::OpenCL::currentQueue(), pszProgramSource, fOptional, pResult);
    }
#endif
    (void)pszProgramSource;
    (void)fOptional;
    (void)pResult;
    return kOfxStatErrUnsupported;
  }
};

/// a C++ image whose pixels are a NumPy array, kept alive until HostSupport deletes the image
class PyImage : public OHIE::Image {
  nb::object _pixels;
public:
  PyImage(OHIE::ClipInstance &clip, nb::object pixels, void *data, const OfxPointD &renderScale,
          const OfxRectI &bounds, const OfxRectI &rod, int rowBytes, const std::string &field,
          const std::string &uniqueId);
  ~PyImage() override;
};

/// ofxh.ClipInstance
class PyClipInstance : public OHIE::ClipInstance {
public:
  PyEffectInstance *_effect;
  nb::object _py;
  RefPtr _ref;

  PyClipInstance(PyEffectInstance *effect, OHIE::ClipDescriptor &desc, nb::object py, RefPtr ref);
  ~PyClipInstance() override;

  /// the view of getImage() (setView)
  int view() const { return _view; }
  /// the ImageData of the output image of the current (or last) render (output clip only; may be
  /// invalid)
  const nb::object &outputData() const { return _outputData; }
  /// release the output image of the last render (the next fetch gets a new one)
  void releaseOutputImage();
  /// releaseOutputImage(), and drop its ImageData: before a render
  void resetOutput();
  /// call the Python get_image() and make a C++ image from what it returns (nullptr if None). The
  /// output image of a render is never nullptr (see fetchImage()). plane: "" for the colour plane,
  /// else the OFX name of an input's plane that planeComponents() knows
  OHIE::Image *fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds,
                          const std::string &plane = std::string());
  OHIE::Image *fetchFromPython(OfxTime time, int view, const OfxRectD *optionalBounds, const std::string &plane);
  /// in an OpenCL, Metal or CUDA render action of the instance, its copy in that API's memory
  /// (RenderSession::getImage(), which gives the output for the output clip); else image. Takes
  /// image's reference.
  OHIE::Image *forGPU(OHIE::Image *image);
  /// the clip's RoD in canonical coordinates, from the Python region_of_definition()
  OfxRectD rod(OfxTime time, int view) const;
  /// the RoD that the plug-in gets (getRegionOfDefinition()): rod(), or for a connected input the
  /// effect's single-resolution frame, if it has one
  OfxRectD pluginRoD(OfxTime time, int view) const;
  /// the default RoD (hostDemo's): the effect's RoD for the output clip, the project for a
  /// connected input, empty for a disconnected one
  OfxRectD defaultRoD(OfxTime time, int view) const;
  /// make an empty image of the clip's mapped depth and components
  nb::object newImage(int x1, int y1, int x2, int y2) const;

  // overrides (pure virtuals of ClipInstance)
  const std::string &getPixelDepth() const override;
  const std::string &getComponents() const override;
  const std::string &getUnmappedBitDepth() const override;
  const std::string &getUnmappedComponents() const override;
  const std::string &getPremult() const override;
#ifdef OFX_EXTENSIONS_NATRON
  OfxRectI getFormat() const override;
#endif
  double getAspectRatio() const override;
  double getFrameRate() const override;
  void getFrameRange(double &startFrame, double &endFrame) const override;
  const std::string &getFieldOrder() const override;
  bool getConnected() const override;
  double getUnmappedFrameRate() const override;
  void getUnmappedFrameRange(double &unmappedStartFrame, double &unmappedEndFrame) const override;
  bool getContinuousSamples() const override;
  OHIE::Image *getImage(OfxTime time, const OfxRectD *optionalBounds) override;
#ifdef OFX_EXTENSIONS_NUKE
  OHIE::Image *getImagePlane(OfxTime time, int view, const std::string &plane,
                             const OfxRectD *optionalBounds) override;
  OfxRectD getRegionOfDefinition(OfxTime time, int view) const override;
  /// ClipInstance.components_present (kFnOfxImageEffectPropComponentsPresent)
  const std::vector<std::string> &getComponentsPresent() const OFX_EXCEPTION_SPEC override;
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
  /// in an OpenGL action of the instance: RenderSession::loadTexture(); else nullptr (kOfxStatFailed)
  OHIE::Texture *loadTexture(OfxTime time, const char *format, const OfxRectD *region) override;
#endif
#ifdef OFX_EXTENSIONS_VEGAS
  OHIE::Image *getStereoscopicImage(OfxTime time, int view, const OfxRectD *optionalBounds) override;
#endif
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
  void setView(int view) override;
#endif
  OfxRectD getRegionOfDefinition(OfxTime time) const override;

private:
  /// make the C++ image of what get_image() returned (raises if it isn't a valid image). frame:
  /// the single-resolution frame (pixels) of an input to crop and pad the image to, and its bounds
  /// there (nullptr: none)
  PyImage *makeImage(nb::object r, OfxTime time, int view, const std::string &plane,
                     const OfxRectI *frame = nullptr, const OfxRectI *frameBounds = nullptr);

  int _view;                   ///< the view of getImage() (setView)
  PyImage *_outputImage;       ///< the output image of the current render (output clip only)
  nb::object _outputData;      ///< its ImageData
  // the strings returned by reference
  mutable std::string _unmappedDepth, _unmappedComponents, _premult, _fieldOrder;
#ifdef OFX_EXTENSIONS_NUKE
  /// storage for getComponentsPresent(), written under the GIL (a plug-in keeps the const char* of a value)
  mutable std::vector<std::string> _componentsPresent;
#endif
};

/// ofxh.ImageEffectInstance
class PyEffectInstance : public OHIE::Instance {
public:
  nb::object _py;
  RefPtr _ref;

  PyEffectInstance(OHIE::ImageEffectPlugin *plugin, OHIE::Descriptor &desc,
                   const std::string &context, nb::object py, RefPtr ref);
  ~PyEffectInstance() override;

  // rendering, renderScale, renderWindow: written only by RenderScaleScope and RenderScope
  // (ofxhPythonModule.cpp)
  bool rendering() const { return _rendering; }
  /// the render scale of the current action ((1, 1) outside)
  const OfxPointD &renderScale() const { return _renderScale; }
  /// the render window of the current (or last) render action
  const OfxRectI &renderWindow() const { return _renderWindow; }
  /// the default timeline (written by TimeLineScope and timeline_goto_time)
  double timeLineTime() const { return _timeLineTime; }
  void setTimeLineTime(double t) { _timeLineTime = t; }
  /// ImageEffectInstance.aborted (written by that Python property)
  bool aborted() const { return _abort.load() != 0; }
  void setAborted(bool a) { _abort.store(a ? 1 : 0); }
  /// the host made the output image of the render (written by PyClipInstance::fetchImage(), from
  /// any worker thread; abort() returns 1 until takeOutputFailed())
  void setOutputFailed() { _outputFailed.store(true); }
  /// whether the host made the output image of the render, and clear it
  bool takeOutputFailed() { return _outputFailed.exchange(false); }
  /// the kOfxActionInstanceChanged sent for parameters (counted by paramInstanceChangedAction())
  int paramChangedCount() const { return _paramChangedCount; }
#ifdef OFXH_OPENGL
  /// the OpenGL attachment (kOfxActionOpenGLContextAttached), made by the first OpenGL action
  std::unique_ptr<OFX::Host::OpenGL::Attachment> _glAttachment;
  /// the session of the OpenGL action in progress, which PyClipInstance::loadTexture() forwards to
  OFX::Host::OpenGL::RenderSession *_glSession = nullptr;
  /// the outcome of the last OpenGL action: "ok", "not_sent" (not attached) or "failed" (an input
  /// texture failed, or the plug-in reported an OpenGL failure)
  std::string _glResult;
  /// the exception that stopped the last OpenGL action, "" if none
  std::string _glError;
  /// kOfxActionOpenGLContextDetached before kOfxActionDestroyInstance
  OfxStatus destroyInstanceAction() override
  {
    _glAttachment.reset();
    return OHIE::Instance::destroyInstanceAction();
  }
#endif
#ifdef OFXH_OPENCL
  /// the session of the OpenCL action in progress, whose images PyClipInstance's getImage() gives
  /// (read on the plug-in's threads too: the GPURenderScope isn't seen there)
  std::atomic<OFX::Host::OpenCL::RenderSession *> _clSession{ nullptr };
  /// the outcome of the last OpenCL action: "ok", "not_sent" (the session refused it: see its log),
  /// "retry" or "failed" (an image copy failed, or the plug-in reported a GPU failure)
  std::string _clResult;
  /// the exception that stopped the last OpenCL action, "" if none
  std::string _clError;
#endif
#ifdef OFXH_METAL
  /// the same for Metal (withMetal())
  std::atomic<OFX::Host::Metal::RenderSession *> _mtSession{ nullptr };
  std::string _mtResult;
  std::string _mtError;
#endif
#ifdef OFXH_CUDA
  /// the same for CUDA (withGPU())
  std::atomic<OFX::Host::CUDA::RenderSession *> _cuSession{ nullptr };
  std::string _cuResult;
  std::string _cuError;
#endif

  const std::map<std::string, OHIE::ClipInstance *> &clips() const { return _clips; }
  /// the project rectangle in pixels at the given render scale
  OfxRectI projectPixelRect(const OfxPointD &renderScale) const;
  /// ofxh crops and pads the inputs of a plug-in without multiple resolutions, as hostDemo
  /// (PyClipInstance::fetchFromPython())
  bool cropsAndPadsImages() const override { return true; }

  // ImageEffect::Instance
  const std::string &getDefaultOutputFielding() const override;
  /// HostSupport's, then default_output_depth (if not None) for the output when no input is
  /// connected (as hostDemo, which uses its -d depth there)
  void setDefaultClipPreferences() override;
  OHIE::ClipInstance *newClipInstance(OHIE::Instance *plugin, OHIE::ClipDescriptor *descriptor,
                                      int index) override;
  OfxStatus vmessage(const char *type, const char *id, const char *format, va_list args) override;
  OfxStatus setPersistentMessage(const char *type, const char *id, const char *format,
                                 va_list args) override;
  OfxStatus clearPersistentMessage() override;
  int abort() override;
  void getProjectSize(double &xSize, double &ySize) const override;
  void getProjectOffset(double &xOffset, double &yOffset) const override;
  void getProjectExtent(double &xSize, double &ySize) const override;
  double getProjectPixelAspectRatio() const override;
  double getEffectDuration() const override;
  double getFrameRate() const override;
  double getFrameRecursive() const override;
  void getRenderScaleRecursive(double &x, double &y) const override;
#ifdef OFX_EXTENSIONS_NUKE
  OfxStatus getViewCount(int *nViews) const override;
  OfxStatus getViewName(int viewIndex, const char **name) const override;
#endif
  // Param::SetInstance
  OH::Param::Instance *newParam(const std::string &name, OH::Param::Descriptor &descriptor) override;
  OfxStatus editBegin(const std::string &name) override;
  OfxStatus editEnd() override;
  // (counts them: param_changed_count)
  OfxStatus paramInstanceChangedAction(const std::string &paramName, const std::string &why, OfxTime time,
                                       OfxPointD renderScale) override;
  // Progress::ProgressI
  void progressStart(const std::string &message, const std::string &messageid) override;
  void progressEnd() override;
  bool progressUpdate(double t) override;
  // TimeLine::TimeLineI
  double timeLineGetTime() override;
  void timeLineGotoTime(double t) override;
  void timeLineGetBounds(double &t1, double &t2) override;

private:
  friend struct RenderScaleScope;
  friend struct RenderScope;

  bool _rendering;
  OfxPointD _renderScale;
  OfxRectI _renderWindow;
  double _timeLineTime;
  std::atomic<int> _abort;
  std::atomic<bool> _outputFailed;  ///< atomic: abort() reads it from the plug-in's threads
  int _paramChangedCount;
  mutable std::string _fielding;  ///< getDefaultOutputFielding()
  mutable std::map<int, std::string> _viewNames;  ///< getViewName()
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

/// the Python parameter (an ofxh.Param subclass) of a descriptor's type, bound to py, or nullptr if
/// the type is not supported
OH::Param::Instance *newPyParam(PyEffectInstance *effect, OH::Param::Descriptor &descriptor, nb::object py,
                                RefPtr ref);

/// The default timeline at the time of an action (as hostDemo, which sets it before each
/// action): a top-level action leaves it there; an action nested in another one (called by a
/// callback, or by a worker thread) restores the time of the outer action. Made after the
/// action's ActionScope (or inside a scoped()).
struct TimeLineScope {
  PyEffectInstance *p;
  double saved;
  bool nested;
  TimeLineScope(PyEffectInstance *inst, double time)
    : p(inst), saved(inst->timeLineTime()), nested(ActionScope::depth() > 1 || ActionScope::onWorker())
  {
    p->setTimeLineTime(time);
  }
  ~TimeLineScope()
  {
    if (nested) {
      p->setTimeLineTime(saved);
    }
  }
  TimeLineScope(const TimeLineScope &) = delete;
  TimeLineScope &operator=(const TimeLineScope &) = delete;
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
/// page of the parameter; may be nullptr)
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
/// (kOfxParamPropDefault), or nullptr if the type is not supported
OH::Param::Instance *newStoredParam(PyEffectInstance *effect, OH::Param::Descriptor &descriptor);
/// the value of a parameter, as a Python object (a tuple for multi-dimensional parameters)
nb::object getParamValue(OH::Param::Instance *param, const double *time);
/// set the value of a parameter from a Python object (at a time: a key if it animates)
OfxStatus setParamValue(OH::Param::Instance *param, nb::handle value, const double *time = nullptr);
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
