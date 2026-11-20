// Python bindings of HostSupport (nanobind): the adapters and the shared machinery.
//
// Lifetime model:
// - HostSupport deletes the clip, param and image objects it gets from the factory methods with
//   plain delete, so the objects it owns are C++ adapters (PyHost, PyEffectInstance,
//   PyClipInstance, PyImage), allocated with new. Each adapter holds a strong reference (_py) to
//   an instance of a user subclass of a small Python base class (ofxh.Host,
//   ofxh.ImageEffectInstance, ofxh.ClipInstance), and implements each virtual function by calling
//   the Python object.
// - The Python base class holds a Ref, shared with the adapter. The adapter's destructor sets
//   Ref::p to NULL: from then on, the Python object raises ReferenceError ("deleted by
//   HostSupport").
// - No exception may cross a HostSupport or plug-in frame: every callback is wrapped in
//   OFXH_CALL, which turns a Python exception into a status (ofxh.OfxError(status) gives that
//   status, anything else kOfxStatFailed or a safe default) and saves the first error, which the
//   Python binding that called into HostSupport re-raises as ofxh.CallbackError (ActionScope).
// - The GIL: the Python side calls HostSupport with the GIL held, and every callback acquires it
//   (part 1 renders on the calling thread only).

#ifndef OFXH_PYTHON_H
#define OFXH_PYTHON_H

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <atomic>
#include <cstdarg>
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

class PyHost;
class PyEffectInstance;
class PyClipInstance;

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
  nb::object statFn;           ///< ofxh._stat(int) -> Stat or int
  nb::object ofxError;         ///< ofxh.OfxError
  nb::object raiseCallbackError; ///< ofxh._raise_callback_error(where, exc)
  nb::object numpy;            ///< the numpy module (imported on first use)
};
Globals &globals();

/// the status as an ofxh.Stat (or a plain int if it isn't one)
nb::object stat(OfxStatus s);

/// save the first error of the current action (see ActionScope)
void saveError(nb::handle exc);
void saveError(const char *message);

/// Saves the pending error of an enclosing action when a Python binding calls into HostSupport,
/// and re-raises the first error saved by the callbacks, as ofxh.CallbackError, in check().
class ActionScope {
  nb::object _saved;
  bool _done;
public:
  ActionScope();
  ~ActionScope();
  /// raise ofxh.CallbackError("<where>: ...") from the first error, if a callback failed (with
  /// the action's status, if given)
  void check(const char *where, const OfxStatus *status = NULL);
  /// did a callback fail since the scope began?
  bool failed() const;
  /// forget the errors of the callbacks (and restore the enclosing action's)
  void discard();
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

// ---------------------------------------------------------------------------------------------
// Names (Python accepts the short names, HostSupport gets the OFX strings)

std::string depthName(const std::string &s);       ///< "byte" -> kOfxBitDepthByte...
std::string componentsName(const std::string &s);  ///< "rgba" -> kOfxImageComponentRGBA...
std::string premultName(const std::string &s);     ///< "premult" -> kOfxImagePreMultiplied...
std::string fieldName(const std::string &s);       ///< "none" -> kOfxImageFieldNone...
std::string changeReason(const std::string &s);    ///< "user" -> kOfxChangeUserEdited...
int componentCount(const std::string &components); ///< 4, 3, 1, or 0 if unsupported

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

  PyHost(nb::object py, RefPtr ref);
  virtual ~PyHost();

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
  mutable bool _inRoDAction;   ///< the output clip is asking for the effect's RoD
  // the strings returned by reference
  mutable std::string _unmappedDepth, _unmappedComponents, _premult, _fieldOrder;

  PyClipInstance(PyEffectInstance *effect, OHIE::ClipDescriptor &desc, nb::object py, RefPtr ref);
  virtual ~PyClipInstance();

  /// release the output image of the last render (the next fetch gets a new one)
  void releaseOutputImage();
  /// call the Python get_image() and make a C++ image from what it returns (NULL if None)
  OHIE::Image *fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds);
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
  // the render state, set by the render binding
  bool _rendering;
  OfxPointD _renderScale;
  OfxRectI _renderWindow;
  double _timeLineTime;           ///< the default timeline (timeline_get_time/goto_time)
  std::atomic<int> _abort;        ///< ImageEffectInstance.aborted
  mutable std::string _fielding;  ///< getDefaultOutputFielding()
  mutable std::string _viewName;  ///< getViewName()

  PyEffectInstance(OHIE::ImageEffectPlugin *plugin, OHIE::Descriptor &desc,
                   const std::string &context, nb::object py, RefPtr ref);
  virtual ~PyEffectInstance();

  const std::map<std::string, OHIE::ClipInstance *> &clips() const { return _clips; }
  /// the project rectangle in pixels at the given render scale
  OfxRectI projectPixelRect(const OfxPointD &renderScale) const;

  // ImageEffect::Instance
  virtual const std::string &getDefaultOutputFielding() const;
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
  // Progress::ProgressI
  virtual void progressStart(const std::string &message, const std::string &messageid);
  virtual void progressEnd();
  virtual bool progressUpdate(double t);
  // TimeLine::TimeLineI
  virtual double timeLineGetTime();
  virtual void timeLineGotoTime(double t);
  virtual void timeLineGetBounds(double &t1, double &t2);
};

/// the open instances, closed by shutdown()
std::set<PyEffectInstance *> &openInstances();

/// format a printf-style message (hostDemo's buffer size; "..." if truncated)
std::string formatMessage(const char *format, va_list args);

// ---------------------------------------------------------------------------------------------
// Parameters (part 1: C++ parameters that store their value; Python sees get/set)

/// a C++ parameter instance of the descriptor's type, with the default value
/// (kOfxParamPropDefault), or NULL if the type is unknown
OH::Param::Instance *newStoredParam(PyEffectInstance *effect, OH::Param::Descriptor &descriptor);
/// the value of a parameter, as a Python object (a tuple for multi-dimensional parameters)
nb::object getParamValue(OH::Param::Instance *param, const double *time);
/// set the value of a parameter from a Python object
OfxStatus setParamValue(OH::Param::Instance *param, nb::handle value, const double *time);

// ---------------------------------------------------------------------------------------------
/// bind the module (ofxhPythonModule.cpp)
void bindModule(nb::module_ &m);

}  // namespace ofxh

#endif  // OFXH_PYTHON_H
