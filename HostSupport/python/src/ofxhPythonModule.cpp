// Python bindings of HostSupport: the ofxh module.

#include "ofxhPython.h"

#include <nanobind/eval.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/list.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "ofxGPURender.h"  // kOfxStatGLRenderFailed, kOfxImageEffectPropOpenGLRenderSupported
#include "ofxMessage.h"
#include "ofxParametricParam.h"  // kOfxParamTypeParametric
#include "ofxhHost.h"  // kOfxHostSupportHostPointer
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"  // kFnOfxImagePlaneColour
#endif

#if defined(OFX_SUPPORTS_MULTITHREAD) || defined(OFX_SUPPORTS_DIALOG)
#error "the ofxh module doesn't implement the OFX_SUPPORTS_MULTITHREAD and OFX_SUPPORTS_DIALOG host functions"
#endif

using namespace nanobind::literals;

namespace ofxh {

static const char *kModuleDoc =
  "Python bindings of the OpenFX HostSupport library: write OFX hosts in Python, in-process.\n\n"
  "Subclass ofxh.Host, ofxh.ImageEffectInstance and ofxh.ClipInstance (and ofxh.Param for the\n"
  "parameters that Python holds), override their callbacks, then ofxh.init(host, plugin_dirs),\n"
  "ofxh.create_instance(plugin, context, instance), and call the instance's *_action() methods.\n"
  "Names are short (depth \"byte\", components \"rgba\", context \"filter\"...; ofx_name() and\n"
  "short_name() convert them). What the plug-ins describe: Plugin.descriptor(context),\n"
  "ofxh.describe() (hostDemo's --describe JSON) and ofxh.param_template(). See\n"
  "HostSupport/examples/python/minimal_host.py.\n\n"
  "The GIL is released while HostSupport and the plug-in run (the actions, the plug-in scan,\n"
  "create_instance()), and taken again by every callback, so that other Python threads run; but\n"
  "ofxh is single-threaded: call it from one thread. While a binding runs, another thread gets\n"
  "RuntimeError, except a plug-in's worker thread inside a callback (it called the host), which\n"
  "may call the bindings that run no action and change no value (get_param(), the getters, the\n"
  "defaults...), but not the actions, press() or set_param().\n\n"
  "One host per process: HostSupport's plug-in cache and image effect host are process-wide, so\n"
  "ofxh.init() raises RuntimeError while a host is initialized. shutdown() then init() again works\n"
  "(another host, other plug-in directories, another profile); the plug-ins are unloaded in between,\n"
  "but a binary the system doesn't unload keeps its static state. init(profile=\"ofx14\") is an OFX\n"
  "1.4 host (hostDemo's --ofx14).\n\n"
  "HostSupport/examples/python/ofxhost has hostDemo's test host on ofxh (ofxhost.render(...,\n"
  "backend=\"inprocess\"), with hostDemo's JSON report).";

// What Python defines (exec'd in the module namespace)
static const char *kPrelude = R"PY(
import enum as _enum


class Stat(_enum.IntEnum):
    """OFX status codes (kOfxStat*)."""
    OK = 0
    Failed = 1
    ErrFatal = 2
    ErrUnknown = 3
    ErrMissingHostFeature = 4
    ErrUnsupported = 5
    ErrExists = 6
    ErrFormat = 7
    ErrMemory = 8
    ErrBadHandle = 9
    ErrBadIndex = 10
    ErrValue = 11
    ReplyYes = 12
    ReplyNo = 13
    ReplyDefault = 14
    Unlicensed = 15
    ErrImageFormat = 1000
    GPUOutOfMemory = 1001
    GPURenderFailed = 1002


def _stat(value):
    try:
        return Stat(value)
    except ValueError:
        return value


class OfxError(Exception):
    """Raise it in a host callback to give an OFX status to HostSupport and the plug-in.

    OfxError(status) in a callback that returns a status (message, edit_begin, ...) returns that
    status, and is not an error. Anywhere else, or any other exception, is an error: the callback
    returns kOfxStatFailed (or a safe default), and the action raises CallbackError."""

    def __init__(self, status=Stat.Failed, message=None):
        self.status = int(status)
        super().__init__(message if message is not None else f"OFX status {_stat(self.status)!r}")


class CallbackError(RuntimeError):
    """A host callback raised an exception while HostSupport or the plug-in called it.

    Raised by the function that called into HostSupport (an action, create_instance, init...),
    after the call. __cause__ is the first exception; status is the action's status (or None).
    When create_instance() raises it but the instance was made, instance is that instance (still
    bound: close() it); else None."""
    status = None
    instance = None


def _raise_callback_error(where, exc, status=None):
    e = CallbackError(f"{where}: a host callback raised {type(exc).__name__}: {exc}")
    e.status = status
    raise e from exc
)PY";

// ---------------------------------------------------------------------------------------------
// Module state: one host per process

struct State {
  bool initialized = false;
  std::string profile;  ///< ofxh.init()'s "default" or "ofx14", set before init_properties(); "" when not initialized
  std::unique_ptr<PyHost> host;  ///< (HostSupport keeps a raw pointer to it: gImageEffectHost)
  std::unique_ptr<OHIE::PluginCache> pluginCache;  ///< (registered in HostSupport's plug-in cache)
  unsigned generation = 0;  ///< incremented by shutdown(): Plugin views become invalid
};

static State &state()
{
  // never deleted, as globals(): a host that shutdown() didn't delete (an exit from inside an
  // action) must not be deleted after the interpreter is finalized
  static State *s = new State;
  return *s;
}

#ifdef OFXH_OPENGL
namespace GL = OFX::Host::OpenGL;

/// the OpenGL render mode of the instance (chooseRenderMode()): with the host's context and the
/// instance's attachment if opengl, else as without a context (the CPU retry)
static GL::RenderMode renderMode(PyEffectInstance *p, bool opengl)
{
  PyHost *h = state().host.get();
  const bool hasGL = opengl && h && h->_glContext && !(p->_glAttachment && p->_glAttachment->failed());
  return GL::chooseRenderMode(*p, hasGL);
}

/// An action of the instance with OpenGL (action(openGLRender, contextData)) in a RenderSession, with
/// output (the output clip, for the render action) as the render target over window, as hostDemo's
/// withOpenGL(). Sets p->_glResult: "not_sent" (not attached: the action wasn't called); "retry"
/// (GL::cpuRetryAllowed(): OpenGL failed, and the plug-in can render on the CPU); "failed" (OpenGL
/// failed, GL::openGLFailed(), or threw); else "ok". Without the GIL.
static OfxStatus withOpenGL(PyEffectInstance *p, OHIE::ClipInstance *output, OfxTime t, const OfxRectI &window,
                            const std::function<OfxStatus(bool, void *)> &action)
{
  PyHost *h = state().host.get();
  p->_glResult = "not_sent";
  p->_glError.clear();
  if (!p->_glAttachment) {
    p->_glAttachment.reset(new GL::Attachment(*p));
  }
  GL::CurrentContext current(*h->_glContext);
  GL::RenderSession session(*p->_glAttachment, current);
  if (!session.ok()) {
    return session.status();
  }
  OHIE::Image *out = nullptr;
  OfxStatus st = kOfxStatFailed;
  // (if it throws: OpenGL failed, retried on the CPU if the plug-in can, as hostDemo)
  p->_glResult = renderMode(p, false) == GL::RenderMode::CPU ? "retry" : "failed";
  try {
    if (output) {
      out = output->getImage(t, nullptr); // (the output image of this render)
      if (!out) {
        return kOfxStatFailed;
      }
      session.setOutput(*out, window);
    }
    // the session of the action, also when it throws (E.6)
    struct SessionScope {
      PyEffectInstance *p;
      ~SessionScope() { p->_glSession = nullptr; }
    } scope = { p };
    p->_glSession = &session;
    st = action(true, session.contextData());
    const bool failed = GL::openGLFailed(session, st);
    if (out && st == kOfxStatOK && !failed) {
      session.readOutput();
    }
    p->_glResult = !failed ? "ok" : GL::cpuRetryAllowed(*p, session, st) ? "retry" : "failed";
  } catch (const std::exception &e) {
    p->_glError = e.what(); // (opengl_error: the in-process backend prints it, as hostDemo)
  }
  if (out) {
    out->releaseReference();
  }
  return st;
}

#endif

#ifdef OFXH_OPENCL
namespace CL = OFX::Host::OpenCL;

/// the OpenCL render mode of the instance (CL::chooseRenderMode()) with the host's device (Buffers,
/// and Images if the device has them), preferring Images if preferImages; without a device, as
/// without OpenCL. (The host descriptor's properties, set by init(), say the same; RenderSession
/// checks them.)
static CL::RenderMode renderModeCL(PyEffectInstance *p, bool preferImages)
{
  PyHost *h = state().host.get();
  const CL::Device *d = h ? h->_clDevice.get() : nullptr;
  return CL::chooseRenderMode(*p, d != nullptr, d && d->info().imageSupport, preferImages);
}

static const char *modeName(CL::RenderMode m)
{
  switch (m) {
  case CL::RenderMode::Buffers: return "opencl-buffers";
  case CL::RenderMode::Images: return "opencl-images";
  case CL::RenderMode::CPU: return "cpu";
  case CL::RenderMode::None: break;
  }
  return "none";
}

/// (withGPU())
static bool gpuFailed(const CL::RenderSession &session, OfxStatus st) { return CL::openCLFailed(session, st); }
#endif

#ifdef OFXH_METAL
namespace MT = OFX::Host::Metal;

/// the Metal render mode of the instance (MT::chooseRenderMode()) with the host's device (buffers and
/// textures), preferring textures if preferTextures; without a device, as without Metal
static MT::RenderMode renderModeMT(PyEffectInstance *p, bool preferTextures)
{
  PyHost *h = state().host.get();
  const bool device = h && h->_mtDevice;
  return MT::chooseRenderMode(*p, device, device, preferTextures);
}

static const char *modeName(MT::RenderMode m)
{
  switch (m) {
  case MT::RenderMode::Buffers: return "metal-buffers";
  case MT::RenderMode::Textures: return "metal-textures";
  case MT::RenderMode::CPU: return "cpu";
  case MT::RenderMode::None: break;
  }
  return "none";
}

/// (withGPU())
static bool gpuFailed(const MT::RenderSession &session, OfxStatus st) { return MT::metalFailed(session, st); }
#endif

#ifdef OFXH_CUDA
namespace CU = OFX::Host::CUDA;

/// the CUDA render mode of the instance (CU::chooseRenderMode()) with the host's device; without a
/// device, or after a sticky CUDA error (Device::usable()), as without CUDA
static CU::RenderMode renderModeCU(PyEffectInstance *p)
{
  PyHost *h = state().host.get();
  return CU::chooseRenderMode(*p, h && h->_cuDevice && h->_cuDevice->usable());
}

static const char *modeName(CU::RenderMode m)
{
  switch (m) {
  case CU::RenderMode::CUDA: return "cuda";
  case CU::RenderMode::CPU: return "cpu";
  case CU::RenderMode::None: break;
  }
  return "none";
}

/// (withGPU())
static bool gpuFailed(const CU::RenderSession &session, OfxStatus st) { return CU::cudaFailed(session, st); }
#endif

#if defined(OFXH_OPENCL) || defined(OFXH_METAL) || defined(OFXH_CUDA)
/// An action of the instance on a Queue of device (a CL:: or MT::Queue, a CU::Stream), in a Session
/// (CL::, MT:: or CU::RenderSession, made with sessionArgs: the OpenCL mode): act(false, nullptr), with
/// output's image (the render action) as the session's output over window, read back after. slot,
/// result and error are the instance's for the API (_clSession, _clResult, _clError, or the _mt or _cu
/// ones). Sets result: "not_sent" (the session refused it: the action wasn't called); "retry" (the
/// API's cpuRetryAllowed()); "failed" (its openCLFailed(), metalFailed() or cudaFailed(), or it threw);
/// else "ok". Without the GIL.
template <class Queue, class Session, class Device, class... SessionArgs>
static OfxStatus withGPU(PyEffectInstance *p, Device &device, std::atomic<Session *> &slot, std::string &result,
                         std::string &error, OHIE::ClipInstance *output, OfxTime t, const OfxRectI &window,
                         const std::function<OfxStatus(bool, void *)> &act, SessionArgs... sessionArgs)
{
  result = "not_sent";
  error.clear();
  OHIE::Image *out = nullptr;
  OfxStatus st = kOfxStatFailed;
  {
    Queue queue(device);
    Session session(*p, queue, sessionArgs...);
    if (!session.ok()) {
      return kOfxStatFailed;
    }
    result = cpuRetryAllowed(*p, session, kOfxStatGPURenderFailed) ? "retry" : "failed"; // (the API's, by ADL)
    try {
      if (output) {
        out = output->getImage(t, nullptr); // (the output image of this render, before the session)
        if (!out || !session.setOutput(*output, *out, window)) {
          if (out) {
            out->releaseReference();
          }
          return kOfxStatFailed;
        }
      }
      // the session of the action, also when it throws (E.6)
      struct SessionScope {
        std::atomic<Session *> &slot;
        ~SessionScope() { slot = nullptr; }
      } scope = { slot };
      slot = &session;
      st = act(false, nullptr);
      slot = nullptr;
      if (out && st == kOfxStatOK && !session.failed()) {
        session.readOutput();
      }
      const bool failed = gpuFailed(session, st);
      result = !failed ? "ok" : cpuRetryAllowed(*p, session, st) ? "retry" : "failed";
    } catch (const std::exception &e) {
      error = e.what();
    }
  }
  if (out) {
    out->releaseReference();
  }
  return st;
}
#endif

/// The ValueError of an action's GPU option (checkOpenCL(), checkMetal()): option ("opencl",
/// "metal=True") needs device ("an OpenCL device"...), which hint gives, and an instance that
/// renders with it (modeOk; else notMode says why)
static void requireGPU(const std::string &what, const std::string &option, const std::string &device,
                       const std::string &hint, bool hasDevice, bool modeOk, const std::function<std::string()> &notMode)
{
  if (!hasDevice) {
    raiseError(PyExc_ValueError, what + ": " + option + " needs " + device + " (" + hint + ")");
  }
  if (!modeOk) {
    raiseError(PyExc_ValueError, what + ": " + notMode());
  }
}

/// opencl ("", "buffers" or "images") must be "" or the instance's OpenCL mode with the host's
/// device: else ValueError (called before the action's scopes)
static void checkOpenCL(PyEffectInstance *p, bool opengl, const std::string &opencl, const std::string &what)
{
  if (opencl.empty()) {
    return;
  }
  if (opencl != "buffers" && opencl != "images") {
    raiseError(PyExc_ValueError, what + ": opencl=\"" + opencl + "\": expected \"\", \"buffers\" or \"images\"");
  }
  if (opengl) {
    raiseError(PyExc_ValueError, what + ": opengl=True and opencl can't be used together");
  }
#ifdef OFXH_OPENCL
  PyHost *h = state().host.get();
  const bool images = opencl == "images";
  const CL::RenderMode mode = renderModeCL(p, images);
  requireGPU(what, "opencl", "an OpenCL device", "ofxh.init(opencl=True), ofxh.opencl_device()", h && h->_clDevice,
             mode == (images ? CL::RenderMode::Images : CL::RenderMode::Buffers), [&] {
               return "opencl=\"" + opencl + "\": the instance doesn't render with OpenCL " +
                 (images ? "Images" : "Buffers") + " (render_mode(opencl=True" + (images ? ", prefer_images=True" : "") +
                 ") is \"" + modeName(mode) + "\")";
             });
#else
  (void)p;
  raiseError(PyExc_ValueError, what + ": opencl: ofxh was built without OFX_HOSTSUPPORT_OPENCL");
#endif
}

/// metal ("", "buffers" or "textures") must be "" or the instance's Metal mode with the host's
/// device: else ValueError (called before the action's scopes)
static void checkMetal(PyEffectInstance *p, bool opengl, const std::string &opencl, const std::string &metal,
                       const std::string &what)
{
  if (metal.empty()) {
    return;
  }
  if (metal != "buffers" && metal != "textures") {
    raiseError(PyExc_ValueError, what + ": metal=\"" + metal + "\": expected \"\", \"buffers\" or \"textures\"");
  }
  if (opengl || !opencl.empty()) {
    raiseError(PyExc_ValueError, what + ": metal can't be used with opengl or opencl");
  }
#ifdef OFXH_METAL
  PyHost *h = state().host.get();
  const bool textures = metal == "textures";
  const MT::RenderMode mode = renderModeMT(p, textures);
  requireGPU(what, "metal", "a Metal device", "ofxh.init(metal=True), ofxh.metal_device()", h && h->_mtDevice,
             mode == (textures ? MT::RenderMode::Textures : MT::RenderMode::Buffers), [&] {
               return "metal=\"" + metal + "\": the instance doesn't render with Metal " + (textures ? "textures" : "buffers") +
                 " (render_mode(metal=True" + (textures ? ", prefer_textures=True" : "") + ") is \"" + modeName(mode) + "\")";
             });
#else
  (void)p;
  raiseError(PyExc_ValueError, what + ": metal: ofxh was built without OFX_HOSTSUPPORT_METAL");
#endif
}

/// cuda=True needs a CUDA device and an instance that renders with CUDA: else ValueError (called
/// before the action's scopes)
static void checkCUDA(PyEffectInstance *p, bool opengl, const std::string &opencl, const std::string &metal, bool cuda,
                      const std::string &what)
{
  if (!cuda) {
    return;
  }
  if (opengl || !opencl.empty() || !metal.empty()) {
    raiseError(PyExc_ValueError, what + ": cuda=True can't be used with opengl, opencl or metal");
  }
#ifdef OFXH_CUDA
  PyHost *h = state().host.get();
  const CU::RenderMode mode = renderModeCU(p);
  requireGPU(what, "cuda=True", "a CUDA device", "ofxh.init(cuda=True), ofxh.cuda_device()", h && h->_cuDevice,
             mode == CU::RenderMode::CUDA, [&] {
               return std::string("cuda=True: the instance doesn't render with CUDA (render_mode(cuda=True) is \"") +
                 modeName(mode) + "\")";
             });
#else
  (void)p;
  raiseError(PyExc_ValueError, what + ": cuda=True: ofxh was built without OFX_HOSTSUPPORT_CUDA");
#endif
}

/// opengl=True needs an OpenGL context and an instance that renders with OpenGL: else ValueError
/// (called before the action's scopes; runAction() checks again)
static void checkOpenGL(PyEffectInstance *p, bool opengl, const std::string &what)
{
  if (!opengl) {
    return;
  }
#ifdef OFXH_OPENGL
  PyHost *h = state().host.get();
  if (!h || !h->_glContext) {
    raiseError(PyExc_ValueError, what + ": opengl=True needs an OpenGL context (ofxh.init(opengl=True), "
               "ofxh.opengl_context())");
  }
  const GL::RenderMode mode = renderMode(p, true);
  if (mode != GL::RenderMode::OpenGL) {
    raiseError(PyExc_ValueError, what + ": opengl=True: the instance doesn't render with OpenGL (render_mode() is \"" +
               (mode == GL::RenderMode::CPU ? "cpu" : "none") + "\")");
  }
#else
  (void)p;
  raiseError(PyExc_ValueError, what + ": opengl=True: ofxh was built without OFX_HOSTSUPPORT_OPENGL");
#endif
}

/// An action, with OpenGL if opengl (withOpenGL(), with output as the render target), else without:
/// act(openGLRender, contextData). kOfxImageEffectPropOpenGLEnabled means something only "When a
/// plug-in and host have established they can both use OpenGL renders" (ofxGPURender.h): opengl=True
/// raises ValueError unless the instance renders with OpenGL (render_mode()). Takes the GIL off.
/// With opencl ("buffers" or "images"), the same with OpenCL (withGPU(); checkOpenCL()); with metal,
/// with Metal (withGPU(); checkMetal()); with cuda, with CUDA (withGPU(); checkCUDA()).
static OfxStatus runAction(PyEffectInstance *p, bool opengl, const std::string &opencl, const std::string &metal, bool cuda,
                           const std::string &what,
                           OHIE::ClipInstance *output, OfxTime t, const OfxRectI &window, int view,
                           const std::function<OfxStatus(bool, void *)> &act)
{
  (void)output;
  (void)t;
  (void)window;
  (void)view;
  if (cuda) {
    checkCUDA(p, opengl, opencl, metal, cuda, what);
#ifdef OFXH_CUDA
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    if (output) {
      output->setView(view); // (as for OpenGL below)
    }
#endif
    PyHost *h = state().host.get();
    return withoutGil([&] {
      return withGPU<CU::Stream, CU::RenderSession>(p, *h->_cuDevice, p->_cuSession, p->_cuResult, p->_cuError, output, t,
                                                    window, act);
    });
#else
    return kOfxStatFailed;
#endif
  }
  if (!metal.empty()) {
    checkMetal(p, opengl, opencl, metal, what);
#ifdef OFXH_METAL
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    if (output) {
      output->setView(view); // (as for OpenGL below)
    }
#endif
    PyHost *h = state().host.get();
    const MT::RenderMode mode = metal == "textures" ? MT::RenderMode::Textures : MT::RenderMode::Buffers;
    return withoutGil([&] {
      MT::AutoreleasePool pool; // (the plug-in's autoreleased Metal objects)
      return withGPU<MT::Queue, MT::RenderSession>(p, *h->_mtDevice, p->_mtSession, p->_mtResult, p->_mtError, output, t,
                                                   window, act, mode);
    });
#else
    return kOfxStatFailed;
#endif
  }
  if (!opencl.empty()) {
    checkOpenCL(p, opengl, opencl, what);
#ifdef OFXH_OPENCL
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    if (output) {
      output->setView(view); // (as for OpenGL below)
    }
#endif
    const CL::RenderMode mode = opencl == "images" ? CL::RenderMode::Images : CL::RenderMode::Buffers;
    PyHost *h = state().host.get();
    return withoutGil([&] {
      return withGPU<CL::Queue, CL::RenderSession>(p, *h->_clDevice, p->_clSession, p->_clResult, p->_clError, output, t,
                                                   window, act, mode);
    });
#else
    return kOfxStatFailed;
#endif
  }
  if (!opengl) {
    return withoutGil([&] { return act(false, nullptr); });
  }
#ifdef OFXH_OPENGL
  checkOpenGL(p, opengl, what);
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
  if (output) {
    // the view of this render first, as hostDemo: renderAction() sets it, which releases the output
    // image of another view, and the render target is made of this one before
    output->setView(view);
  }
#endif
  return withoutGil([&] { return withOpenGL(p, output, t, window, act); });
#else
  checkOpenGL(p, opengl, what);
  return kOfxStatFailed;
#endif
}

static void requireInit()
{
  if (!state().initialized) {
    raiseError(PyExc_RuntimeError, "ofxh is not initialized: call ofxh.init(host, plugin_dirs) first");
  }
}

static void requireNoAction(const char *what)
{
  if (ActionScope::depth() > 0 || ActionScope::running()) {
    raiseError(PyExc_RuntimeError, std::string(what) + " can't be called in an action or a host callback (or while "
                                   "one runs in another thread)");
  }
}

/// the render actions don't nest: a nested render would release the outer render's output image
/// and leave the instance not rendering
static void requireNotRendering(PyEffectInstance *p, const char *what)
{
  if (p->rendering()) {
    raiseError(PyExc_RuntimeError, std::string(what) + " can't be called during a render action (from one of its "
                                   "callbacks)");
  }
}

[[noreturn]] static void raiseKey(const std::string &name) { raiseError(PyExc_KeyError, name); }

// ---------------------------------------------------------------------------------------------
// Views

OH::Property::Set &PropertySetView::get() const
{
  checkThread("ofxh.PropertySet", false);
  if (!ref->p) {
    raiseDeleted("ofxh.PropertySet");
  }
  switch (kind) {
    case Kind::host: return ref->as<PyHost>()->getProperties();
    case Kind::instance: return ref->as<PyEffectInstance>()->getProps();
    case Kind::clip: return ref->as<PyClipInstance>()->getProps();
    case Kind::param: break;
  }
  return ref->as<OH::Param::Instance>()->getProperties();
}

static OH::Property::Property *fetchProp(const OH::Property::Set &s, const std::string &name)
{
  OH::Property::Property *p = s.fetchProperty(name, true);
  if (!p) {
    raiseKey(name);
  }
  return p;
}

static nb::object propValue(const OH::Property::Set &s, const std::string &name, int index)
{
  OH::Property::Property *p = fetchProp(s, name);
  try {
    switch (p->getType()) {
      case OH::Property::eInt: return nb::int_(s.getIntProperty(name, index));
      case OH::Property::eDouble: return nb::float_(s.getDoubleProperty(name, index));
      case OH::Property::eString: return nb::str(s.getStringProperty(name, index).c_str());
      case OH::Property::ePointer: return nb::int_(reinterpret_cast<uintptr_t>(s.getPointerProperty(name, index)));
      default: break;
    }
  } catch (const OH::Property::Exception &e) {
    raiseError(PyExc_ValueError, "property " + name + ": status " + std::to_string(e.getStatus()));
  }
  return nb::none();
}

static nb::tuple propValues(const OH::Property::Set &s, const std::string &name)
{
  fetchProp(s, name);
  int n = 0;
  try {
    n = s.getDimension(name);
  } catch (const OH::Property::Exception &e) {
    raiseError(PyExc_ValueError, "property " + name + ": status " + std::to_string(e.getStatus()));
  }
  nb::list l;
  for (int i = 0; i < n; ++i) {
    l.append(propValue(s, name, i));
  }
  return nb::tuple(l);
}

static bool setProp(OH::Property::Set &s, const std::string &name, nb::handle value)
{
  OH::Property::Property *p = fetchProp(s, name);
  std::vector<nb::handle> v;
  if (nb::isinstance<nb::tuple>(value) || nb::isinstance<nb::list>(value)) {
    for (nb::handle x : value) {
      v.push_back(x);
    }
  } else {
    v.push_back(value);
  }
  try {
    // a variable-size property gets exactly the values given (fewer values than it has drop the
    // others: props[kOfxImageEffectPropSupportedPixelDepths] = (byte, short, float))
    if (!p->isFixedSize() && static_cast<int>(v.size()) < p->getDimension()) {
      p->reset();
    }
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
      switch (p->getType()) {
        case OH::Property::eInt: s.setIntProperty(name, nb::cast<int>(v[i]), i); break;
        case OH::Property::eDouble: s.setDoubleProperty(name, nb::cast<double>(v[i]), i); break;
        case OH::Property::eString: s.setStringProperty(name, nb::cast<std::string>(v[i]), i); break;
        case OH::Property::ePointer: s.setPointerProperty(name, reinterpret_cast<void *>(nb::cast<uintptr_t>(v[i])), i); break;
        default: break;
      }
    }
  } catch (const OH::Property::Exception &e) {
    raiseError(PyExc_ValueError, "property " + name + ": status " + std::to_string(e.getStatus()));
  }
  return true;
}

static std::vector<std::string> shortNames(const char *kind, const std::vector<std::string> &v)
{
  std::vector<std::string> r;
  for (size_t i = 0; i < v.size(); ++i) {
    r.push_back(shortName(kind, v[i]));
  }
  return r;
}

/// ofxh.ClipDescriptor: a copy of what the plug-in described for a clip
struct ClipDescriptorInfo {
  std::string name, label, fieldExtraction;
  bool optional, isMask, isOutput, supportsTiles, temporalAccess;
  std::vector<std::string> supportedComponents;
};

nb::object makeClipDescriptor(const OHIE::ClipDescriptor &d)
{
  ClipDescriptorInfo i;
  i.name = d.getName();
  i.label = d.getLabel();
  i.fieldExtraction = shortName("field", d.getFieldExtraction());
  i.optional = d.isOptional();
  i.isMask = d.isMask();
  i.isOutput = d.isOutput();
  i.supportsTiles = d.supportsTiles();
  i.temporalAccess = d.temporalAccess();
  i.supportedComponents = shortNames("components", d.getSupportedComponents());
  return nb::cast(std::move(i));
}

/// ofxh.Plugin: a plug-in of the plug-in cache (valid until shutdown())
struct PluginView {
  OHIE::ImageEffectPlugin *p;
  unsigned generation;
  OHIE::ImageEffectPlugin *get() const
  {
    checkThread("ofxh.Plugin", false);
    if (!state().initialized || generation != state().generation) {
      raiseDeleted("ofxh.Plugin");
    }
    return p;
  }
};

// ---------------------------------------------------------------------------------------------
// Conversions

using Point = std::pair<double, double>;
using RectD = std::array<double, 4>;
using RectI = std::array<int, 4>;

static OfxPointD toPoint(const Point &p)
{
  OfxPointD r;
  r.x = p.first;
  r.y = p.second;
  return r;
}

static OfxRectD toRectD(const RectD &a)
{
  OfxRectD r;
  r.x1 = a[0];
  r.y1 = a[1];
  r.x2 = a[2];
  r.y2 = a[3];
  return r;
}

static OfxRectI toRectI(const RectI &a)
{
  OfxRectI r;
  r.x1 = a[0];
  r.y1 = a[1];
  r.x2 = a[2];
  r.y2 = a[3];
  return r;
}

static nb::tuple tuple(const OfxRectD &r) { return nb::make_tuple(r.x1, r.y1, r.x2, r.y2); }
static nb::tuple tuple(const OfxRectI &r) { return nb::make_tuple(r.x1, r.y1, r.x2, r.y2); }

/// the views this build can pass: with the Nuke extension, to all the actions that take one;
/// with the Vegas extension, to the render (and several views)
static void checkView(int view, [[maybe_unused]] bool render, int nViews = 1)
{
  bool views = false, severalViews = false;
#ifdef OFX_EXTENSIONS_NUKE
  views = true;
#endif
#ifdef OFX_EXTENSIONS_VEGAS
  views = views || render;
  severalViews = true;
#endif
  if (view < 0) {
    raiseError(PyExc_ValueError, "view must be >= 0");
  }
  if (view != 0 && !views) {
    raiseError(PyExc_ValueError, "view " + std::to_string(view) + ": this build of ofxh can't pass a view to this "
                                 "action (it needs OFX_EXTENSIONS_NUKE, or OFX_EXTENSIONS_VEGAS for the render; see "
                                 "ofxh.build_config)");
  }
  if (nViews < 1 || (nViews > 1 && !severalViews)) {
    raiseError(PyExc_ValueError, "n_views " + std::to_string(nViews) + ": this build of ofxh renders one view "
                                 "(several need OFX_EXTENSIONS_VEGAS; see ofxh.build_config)");
  }
}

/// the render scale, for the duration of an action
struct RenderScaleScope {
  PyEffectInstance *p;
  OfxPointD saved;
  RenderScaleScope(PyEffectInstance *inst, const OfxPointD &scale) : p(inst), saved(inst->_renderScale)
  {
    p->_renderScale = scale;
  }
  ~RenderScaleScope() { p->_renderScale = saved; }
  RenderScaleScope(const RenderScaleScope &) = delete;
  RenderScaleScope &operator=(const RenderScaleScope &) = delete;
};

/// the render state, for the duration of a render action (and no output image made by the host
/// yet, see PyClipInstance::fetchImage())
struct RenderScope : RenderScaleScope {
  RenderScope(PyEffectInstance *inst, const OfxPointD &scale, const OfxRectI &window) : RenderScaleScope(inst, scale)
  {
    p->_rendering = true;
    p->_renderWindow = window;
    p->_outputFailed.store(false);
  }
  ~RenderScope() { p->_rendering = false; }
};

static PyClipInstance *pyClip(OHIE::ClipInstance *c) { return dynamic_cast<PyClipInstance *>(c); }

// the messages, as hostDemo's Host::vmessage prints them (type: a short name)
static nb::object defaultMessage(const std::string &type, const std::string &text)
{
  const std::string t = shortName("message", ofxName("message", type));
  const char *prefix = "Message : ";
  bool question = false;
  if (t == "log") {
    prefix = "Log : ";
  } else if (t == "fatal" || t == "error") {
    prefix = "Error : ";
  } else if (t == "question") {
    prefix = "Question : ";
    question = true;
  }
  nb::print(nb::str((prefix + text).c_str()));
  // (no dialog to ask the question: yes)
  return stat(question ? kOfxStatReplyYes : kOfxStatOK);
}

/// A callback without arguments that the user may override with a property, a class attribute,
/// or an instance attribute (self.frame_rate = 30): a read-write property whose value is the
/// instance attribute if set, else the default.
template <class Base, class F>
static void overridable(nb::class_<Base> &c, const char *name, F dflt, const char *doc = "")
{
  const std::string key(name);
  c.def_prop_rw(name,
    [key, dflt](nb::handle self) -> nb::object {
      nb::object d = nb::getattr(self, "__dict__", nb::none());
      if (!d.is_none()) {
        if (PyObject *v = PyDict_GetItemString(d.ptr(), key.c_str())) {
          return nb::borrow(v);
        }
      }
      return dflt(nb::cast<Base &>(self));
    },
    [key](nb::handle self, nb::handle value) {
      nb::object d = nb::getattr(self, "__dict__", nb::none());
      if (d.is_none()) {
        raiseError(PyExc_AttributeError, key + ": this object has no __dict__");
      }
      d[key.c_str()] = value;
    },
    doc);
}

// ---------------------------------------------------------------------------------------------
// init, shutdown

static void shutdown()
{
  State &st = state();
  if (!st.initialized) {
    return;
  }
  requireNoAction("ofxh.shutdown()");
  ActionScope scope;
  // close the open instances: kOfxActionDestroyInstance while they are whole, then delete
  std::vector<PyEffectInstance *> open(openInstances().begin(), openInstances().end());
  for (size_t i = 0; i < open.size(); ++i) {
    if (openInstances().count(open[i])) {
      PyEffectInstance *inst = open[i];
      withoutGil([&] { return inst->destroyInstanceAction(); });
      delete open[i];
    }
  }
  // the plug-ins get kOfxActionUnload and the binaries are unloaded: this needs the image effect
  // plug-in cache (the API handler) and the host, which are deleted after (as in hostDemo)
  withoutGil([] { OH::PluginCache::clearPluginCache(); });
  st.pluginCache.reset();
  OHIE::gImageEffectHost = nullptr;
  std::unique_ptr<PyHost> host = std::move(st.host);
  st.initialized = false;
  st.profile.clear();
  ++st.generation;
  host.reset();
  scope.check("shutdown");
}

/// HostSupport's plug-in cache, made without OFX_PLUGIN_PATH unless useEnvPath (the cache reads
/// it when it is made)
static OH::PluginCache *makePluginCache(bool useEnvPath)
{
  if (useEnvPath) {
    return OH::PluginCache::getPluginCache();
  }
  const char *e = std::getenv("OFX_PLUGIN_PATH");
  const bool restore = e != nullptr;
  const std::string saved = e ? e : "";
#ifdef _WIN32
  _putenv_s("OFX_PLUGIN_PATH", "");
#else
  unsetenv("OFX_PLUGIN_PATH");
#endif
  OH::PluginCache *pc = OH::PluginCache::getPluginCache();
  if (restore) {
#ifdef _WIN32
    _putenv_s("OFX_PLUGIN_PATH", saved.c_str());
#else
    setenv("OFX_PLUGIN_PATH", saved.c_str(), 1);
#endif
  }
  return pc;
}

#ifdef OFXH_OPENGL
/// the backends named in names, an iterable of str (None: defaultBackends()); TypeError for a str
/// (not a list of names) or a non-str item, ValueError for an unknown name
static std::vector<GL::Backend> parseBackends(const nb::object &names, const std::string &what)
{
  if (names.is_none()) {
    return GL::defaultBackends();
  }
  if (nb::isinstance<nb::str>(names)) {
    raiseError(PyExc_TypeError, what + ": the OpenGL backends are a list of names, not a str");
  }
  std::vector<GL::Backend> backends;
  for (nb::handle n : names) {
    if (!nb::isinstance<nb::str>(n)) {
      raiseError(PyExc_TypeError, what + ": an OpenGL backend name must be a str");
    }
    const std::string name = nb::cast<std::string>(n);
    GL::Backend b;
    if (!GL::backendFromName(name, b)) {
      raiseError(PyExc_ValueError, what + ": unknown OpenGL backend \"" + name + "\"");
    }
    backends.push_back(b);
  }
  return backends;
}

/// a renderer as a dict (opengl_renderers(), opengl_context())
static nb::dict rendererDict(const GL::RendererInfo &info)
{
  nb::dict d;
  d["backend"] = GL::backendName(info.backend);
  d["id"] = info.id;
  d["renderer"] = info.renderer;
  d["vendor"] = info.vendor;
  d["version"] = info.version;
  d["glsl"] = info.shadingLanguageVersion;
  d["video_memory"] = info.videoMemoryBytes;
  d["software"] = info.software;
  d["max_texture_size"] = info.maxTextureSize;
  return d;
}
#endif

static void init(nb::object host, nb::object dirs, bool useStdPaths, bool useEnvPath, const std::string &profile,
                 bool keepLoaded, bool normalisedDefaults, bool opengl, nb::object openglBackends, int openglRenderer,
                 bool opencl, nb::object openclDevice, bool metal, int metalDevice, bool cuda, int cudaDevice,
                 const std::string &cudaUUID)
{
  State &st = state();
  if (st.initialized) {
    raiseError(PyExc_RuntimeError, "ofxh is already initialized: one host per process (HostSupport's plug-in cache "
                                   "and image effect host are process-wide); call ofxh.shutdown() first");
  }
  requireNoAction("ofxh.init()");
  if (profile != "default" && profile != "ofx14") {
    raiseError(PyExc_ValueError, "ofxh.init(): profile \"" + profile + "\": expected \"default\" or \"ofx14\"");
  }
#ifndef OFXH_OPENGL
  if (opengl) {
    raiseError(PyExc_ValueError, "ofxh.init(): opengl=True: ofxh was built without OFX_HOSTSUPPORT_OPENGL");
  }
#endif
  if (!opengl && (!openglBackends.is_none() || openglRenderer != -1)) {
    raiseError(PyExc_ValueError, "ofxh.init(): opengl_backends and opengl_renderer need opengl=True");
  }
#ifdef OFXH_OPENGL
  const std::vector<GL::Backend> glBackends = opengl ? parseBackends(openglBackends, "ofxh.init()")
                                                     : std::vector<GL::Backend>();
#endif
#ifndef OFXH_OPENCL
  if (opencl) {
    raiseError(PyExc_ValueError, "ofxh.init(): opencl=True: ofxh was built without OFX_HOSTSUPPORT_OPENCL");
  }
#endif
#ifndef OFXH_METAL
  if (metal) {
    raiseError(PyExc_ValueError, "ofxh.init(): metal=True: ofxh was built without OFX_HOSTSUPPORT_METAL");
  }
#endif
  if (metalDevice < -1) {
    raiseError(PyExc_ValueError, "ofxh.init(): metal_device must be -1 (the default device) or an index of metal_devices()");
  }
#ifndef OFXH_CUDA
  if (cuda) {
    raiseError(PyExc_ValueError, "ofxh.init(): cuda=True: ofxh was built without OFX_HOSTSUPPORT_CUDA");
  }
#endif
  if (cudaDevice < -1) {
    raiseError(PyExc_ValueError, "ofxh.init(): cuda_device must be -1 (the first device) or an index of cuda_devices()");
  }
  if (!cudaUUID.empty() && (!cuda || cudaDevice != -1)) {
    raiseError(PyExc_ValueError, "ofxh.init(): cuda_uuid needs cuda=True, and not cuda_device");
  }
  // opencl_device: (platform, device), indices of opencl_devices(); None: the default device
  int clPlatform = -1, clDevice = -1;
  if (!openclDevice.is_none()) {
    std::pair<int, int> pd;
    if (!nb::try_cast(openclDevice, pd) || pd.first < 0 || pd.second < 0) {
      raiseError(PyExc_ValueError, "ofxh.init(): opencl_device must be None or (platform, device), indices >= 0");
    }
    clPlatform = pd.first;
    clDevice = pd.second;
  }
  if (!nb::isinstance<HostBase>(host)) {
    raiseError(PyExc_TypeError, "ofxh.init(): host must be an ofxh.Host");
  }
  HostBase &hb = nb::cast<HostBase &>(host);
  if (hb.ref->p) {
    raiseError(PyExc_ValueError, "ofxh.init(): this host is already in use");
  }
  // plugin_dirs: a path (str or os.PathLike), or an iterable of paths
  std::vector<std::string> pluginDirs;
  nb::object fspath = nb::module_::import_("os").attr("fspath");
  if (nb::isinstance<nb::str>(dirs) || nb::hasattr(dirs, "__fspath__")) {
    pluginDirs.push_back(nb::cast<std::string>(fspath(dirs)));
  } else if (!dirs.is_none()) {
    for (nb::handle d : dirs) {
      pluginDirs.push_back(nb::cast<std::string>(fspath(d)));
    }
  }
#ifdef OFXH_OPENGL
  // the context first (before the scan: without one, the host doesn't say it supports OpenGL). A
  // backend or renderer the caller chose that can't be used is an error: no fallback to the CPU
  std::unique_ptr<GL::Context> glContext;
  std::string glError;
  if (opengl) {
    withoutGil([&] {
      glContext = GL::createContext(GL::Hooks(), glBackends, openglRenderer, &glError);
      return 0;
    });
    if (!glContext && (openglRenderer != -1 || !openglBackends.is_none())) {
      // a ValueError whose opengl_error attribute is the reason (what opengl_context() would say)
      nb::object e = nb::borrow(PyExc_ValueError)(
        nb::str(("ofxh.init(): no OpenGL context on the chosen backends or renderer (" + glError + ")").c_str()));
      nb::setattr(e, "opengl_error", nb::str(glError.c_str()));
      PyErr_SetObject(PyExc_ValueError, e.ptr());
      throw nb::python_error();
    }
  }
#endif
  // (before the first getPluginCache())
  OH::PluginCache::useStdOFXPluginsLocation(useStdPaths);
  OH::PluginCache *pc = makePluginCache(useEnvPath);
  pc->setCacheVersion("ofxhPythonV1");
  for (size_t i = 0; i < pluginDirs.size(); ++i) {
    pc->addFileToPath(pluginDirs[i]);
  }
  st.host = std::make_unique<PyHost>(host, hb.ref);
  st.host->_keepLoaded = keepLoaded;
  st.host->_normalisedDefaults = normalisedDefaults;
#ifdef OFXH_OPENGL
  st.host->_glContext = std::move(glContext);
  st.host->_glError = glError;
#endif
#ifdef OFXH_OPENCL
  if (opencl) {
    // before the scan: without a device, the host doesn't say it supports OpenCL
    PyHost *h = st.host.get();
    withoutGil([&] {
      h->_clDevice = CL::createDevice(CL::Hooks(), clPlatform, clDevice, &h->_clError);
      return 0;
    });
  }
#else
  (void)clPlatform;
  (void)clDevice;
#endif
#ifdef OFXH_METAL
  if (metal) {
    // before the scan: without a device, the host doesn't say it supports Metal
    PyHost *h = st.host.get();
    withoutGil([&] {
      h->_mtDevice = MT::createDevice(MT::Hooks(), metalDevice, &h->_mtError);
      return 0;
    });
  }
#else
  (void)metalDevice;
#endif
#ifdef OFXH_CUDA
  if (cuda) {
    // before the scan: without a device, the host doesn't say it supports CUDA
    PyHost *h = st.host.get();
    withoutGil([&] {
      h->_cuDevice = cudaUUID.empty() ? CU::createDevice(CU::Hooks(), cudaDevice, &h->_cuError)
                                      : CU::createDeviceWithUUID(CU::Hooks(), cudaUUID, &h->_cuError);
      return 0;
    });
  }
#else
  (void)cudaDevice;
  (void)cudaUUID;
#endif
  st.initialized = true;
  st.profile = profile;
  try {
    {
      ActionScope scope;
      host.attr("init_properties")(PropertySetView{hb.ref, PropertySetView::Kind::host});
      scope.check("init_properties");
    }
    if (!st.initialized || st.host == nullptr) {
      raiseError(PyExc_RuntimeError, "ofxh.init(): the host was shut down during init_properties()");
    }
#ifdef OFXH_OPENGL
    if (st.host->_glContext) {
      // (an OFX 1.4 host property)
      st.host->getProperties().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
    }
#endif
#ifdef OFXH_OPENCL
    if (st.host->_clDevice) {
      // "a host indicates it supports OpenCL acceleration by setting kOfxImageEffectPropOpenCLSupported
      // (Images) and/or kOfxImageEffectPropOpenCLRenderSupported (Buffers) on it's descriptor"
      // (ofxGPURender.h; OFX 1.5 properties, which the ofx14 profile below removes)
      st.host->getProperties().setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "true");
      st.host->getProperties().setStringProperty(kOfxImageEffectPropOpenCLSupported,
                                                 st.host->_clDevice->info().imageSupport ? "true" : "false");
    }
#endif
#ifdef OFXH_METAL
    if (st.host->_mtDevice) {
      // kOfxImageEffectPropMetalRenderSupported, "Indicates whether a host or plug-in can support Metal
      // render" (ofxGPURender.h, host descriptor; an OFX 1.5 property, which the ofx14 profile removes)
      st.host->getProperties().setStringProperty(kOfxImageEffectPropMetalRenderSupported, "true");
      // "The host sets it on the host descriptor" (kOfxImageEffectPropMetalTextureSupported, upstream main)
      st.host->getProperties().setStringProperty(kOfxImageEffectPropMetalTextureSupported, "true");
    }
#endif
#ifdef OFXH_CUDA
    if (st.host->_cuDevice) {
      // kOfxImageEffectPropCudaRenderSupported and kOfxImageEffectPropCudaStreamSupported, "Indicates
      // whether a host or plug-in can support CUDA render" and "CUDA streams" (ofxGPURender.h, host
      // descriptor; OFX 1.5 properties, which the ofx14 profile removes): CUDA memory, and a stream for
      // the plug-ins that support streams too
      st.host->getProperties().setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
      st.host->getProperties().setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
    }
#endif
    if (profile == "ofx14") {
      // (after init_properties(): as hostDemo, only the OFX 1.4 properties remain)
      st.host->restrictToOFX14();
    }
    st.pluginCache = std::make_unique<OHIE::PluginCache>(st.host.get());
    st.pluginCache->registerInCache(*pc);
    ActionScope scope;
    withoutGil([&] { pc->scanPluginFiles(); });
    scope.check("plug-in scan");
  } catch (...) {
    // leave no half-initialized host
    try {
      shutdown();
    } catch (nb::python_error &) {
    }
    throw;
  }
}

static nb::object createInstance(const PluginView &pv, const std::string &context, nb::object instance)
{
  requireInit();
  OHIE::ImageEffectPlugin *plugin = pv.get();
  if (!nb::isinstance<InstanceBase>(instance)) {
    raiseError(PyExc_TypeError, "ofxh.create_instance(): instance must be an ofxh.ImageEffectInstance");
  }
  InstanceBase &ib = nb::cast<InstanceBase &>(instance);
  if (ib.ref->p) {
    raiseError(PyExc_ValueError, "ofxh.create_instance(): this instance object is already in use");
  }
  ActionScope scope;
  // PyHost::newInstance() gets the Python object as the client data, and binds it. If
  // newInstance(), newClipInstance() or newParam() fail, populate() fails and HostSupport deletes
  // the instance: raw is nullptr.
  // (the describe in context action, the clips and the parameters: without the GIL)
  const std::string ctx = ofxName("context", context);
  void *clientData = instance.ptr();
  OHIE::Instance *raw = withoutGil([&] { return plugin->createInstance(ctx, clientData); });
  // (another callback failed, e.g. a message: the instance was made and stays bound, and the
  // CallbackError carries it, as e.instance, so that the caller can close it)
  try {
    scope.check("createInstance (making the clips and parameters)");
  } catch (nb::python_error &e) {
    if (raw) {
      nb::setattr(e.value(), "instance", instance);
    }
    throw;
  }
  if (!raw) {
    return nb::none();
  }
  return instance;
}

// ---------------------------------------------------------------------------------------------
// The bindings, by area (bindModule() calls them in this order)

/// build configuration: the OFX_* definitions the module (and HostSupport) were compiled with
static void bindBuildConfig(nb::module_ &m)
{
  nb::dict d;
#define OFXH_DEF(name, on) d[#name] = nb::bool_(on);
#ifdef OFX_EXTENSIONS_NATRON
  OFXH_DEF(OFX_EXTENSIONS_NATRON, true)
#else
  OFXH_DEF(OFX_EXTENSIONS_NATRON, false)
#endif
#ifdef OFX_EXTENSIONS_NUKE
  OFXH_DEF(OFX_EXTENSIONS_NUKE, true)
#else
  OFXH_DEF(OFX_EXTENSIONS_NUKE, false)
#endif
#ifdef OFX_EXTENSIONS_VEGAS
  OFXH_DEF(OFX_EXTENSIONS_VEGAS, true)
#else
  OFXH_DEF(OFX_EXTENSIONS_VEGAS, false)
#endif
#ifdef OFX_EXTENSIONS_TUTTLE
  OFXH_DEF(OFX_EXTENSIONS_TUTTLE, true)
#else
  OFXH_DEF(OFX_EXTENSIONS_TUTTLE, false)
#endif
#ifdef OFX_EXTENSIONS_RESOLVE
  OFXH_DEF(OFX_EXTENSIONS_RESOLVE, true)
#else
  OFXH_DEF(OFX_EXTENSIONS_RESOLVE, false)
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
  OFXH_DEF(OFX_SUPPORTS_OPENGLRENDER, true)
#else
  OFXH_DEF(OFX_SUPPORTS_OPENGLRENDER, false)
#endif
#ifdef OFX_SUPPORTS_PARAMETRIC
  OFXH_DEF(OFX_SUPPORTS_PARAMETRIC, true)
#else
  OFXH_DEF(OFX_SUPPORTS_PARAMETRIC, false)
#endif
#ifdef OFXH_OPENGL
  OFXH_DEF(OFX_HOSTSUPPORT_OPENGL, true)
#else
  OFXH_DEF(OFX_HOSTSUPPORT_OPENGL, false)
#endif
#ifdef OFXH_OPENCL
  OFXH_DEF(OFX_HOSTSUPPORT_OPENCL, true)
#else
  OFXH_DEF(OFX_HOSTSUPPORT_OPENCL, false)
#endif
#ifdef OFXH_METAL
  OFXH_DEF(OFX_HOSTSUPPORT_METAL, true)
#else
  OFXH_DEF(OFX_HOSTSUPPORT_METAL, false)
#endif
#ifdef OFXH_CUDA
  OFXH_DEF(OFX_HOSTSUPPORT_CUDA, true)
#else
  OFXH_DEF(OFX_HOSTSUPPORT_CUDA, false)
#endif
#undef OFXH_DEF
  m.attr("build_config") = d;
}

static void bindPropertySet(nb::module_ &m)
{
  nb::class_<PropertySetView>(m, "PropertySet",
                              "The properties of the host, an instance or a clip (HostSupport's Property::Set, with\n"
                              "the OFX names and values). props[name] is the tuple of values; props.get(name, index)\n"
                              "one value. props[name] = values sets them (all the values of a variable-size property).")
    .def("__getitem__", [](const PropertySetView &v, const std::string &name) {
      return scoped("PropertySet[]", [&] { return propValues(v.get(), name); });
    })
    .def("get", [](const PropertySetView &v, const std::string &name, int index) {
      return scoped("PropertySet.get()", [&] { return propValue(v.get(), name, index); });
    }, "name"_a, "index"_a = 0)
    .def("__setitem__", [](const PropertySetView &v, const std::string &name, nb::handle value) {
      checkThread("PropertySet[]=", true);
      scoped("PropertySet[]=", [&] { return setProp(v.get(), name, value); });
    })
    .def("__delitem__", [](const PropertySetView &v, const std::string &name) {
      checkThread("del PropertySet[]", true);
      OH::Property::Set &s = v.get();
      if (v.kind != PropertySetView::Kind::host) {
        // (HostSupport reads the properties of instances, clips and parameters itself, and a
        // missing one throws where it doesn't expect it)
        raiseError(PyExc_TypeError, "del PropertySet[" + name + "]: only the host's properties can be deleted");
      }
      if (name == kOfxHostSupportHostPointer) {
        raiseError(PyExc_ValueError, "del PropertySet[" + name + "]: HostSupport needs this property (fetchSuite)");
      }
      if (!s.fetchProperty(name, false)) {
        raiseKey(name);
      }
      s.removeProperty(name);
    }, "name"_a,
       "Delete a host property (e.g. in Host.init_properties(), before the plug-in scan): a plug-in that\n"
       "reads it then gets kOfxStatErrUnknown. Only the host's properties (TypeError for the others, which\n"
       "HostSupport reads itself), not kOfxHostSupportHostPointer (ValueError). KeyError if it doesn't exist.")
    .def("__contains__", [](const PropertySetView &v, const std::string &name) { return v.get().fetchProperty(name, true) != nullptr; })
    .def("names", [](const PropertySetView &v) {
      std::vector<std::string> r;
      const OH::Property::PropertyMap &pm = v.get().getProperties();
      for (OH::Property::PropertyMap::const_iterator it = pm.begin(); it != pm.end(); ++it) {
        r.push_back(it->first);
      }
      return r;
    }, "The names of the properties (not those of a chained set).");
}

static void bindClipDescriptor(nb::module_ &m)
{
  nb::class_<ClipDescriptorInfo>(m, "ClipDescriptor", "What the plug-in described for a clip (a copy).")
    .def_ro("name", &ClipDescriptorInfo::name)
    .def_ro("label", &ClipDescriptorInfo::label)
    .def_ro("field_extraction", &ClipDescriptorInfo::fieldExtraction)
    .def_ro("is_optional", &ClipDescriptorInfo::optional)
    .def_ro("is_mask", &ClipDescriptorInfo::isMask)
    .def_ro("is_output", &ClipDescriptorInfo::isOutput)
    .def_ro("supports_tiles", &ClipDescriptorInfo::supportsTiles)
    .def_ro("temporal_access", &ClipDescriptorInfo::temporalAccess)
    .def_ro("supported_components", &ClipDescriptorInfo::supportedComponents)
    .def("__repr__", [](const ClipDescriptorInfo &i) { return "<ofxh.ClipDescriptor " + i.name + ">"; });
}

static void bindPlugin(nb::module_ &m)
{
  nb::class_<PluginView>(m, "Plugin", "An image effect plug-in found by ofxh.init() (valid until ofxh.shutdown()).")
    .def_prop_ro("id", [](const PluginView &v) { return v.get()->getIdentifier(); })
    .def_prop_ro("version", [](const PluginView &v) {
      return std::make_pair(v.get()->getVersionMajor(), v.get()->getVersionMinor());
    })
    .def_prop_ro("label", [](const PluginView &v) { return v.get()->getDescriptor().getLabel(); })
    .def_prop_ro("grouping", [](const PluginView &v) { return v.get()->getDescriptor().getPluginGrouping(); })
    .def_prop_ro("contexts", [](const PluginView &v) {
      const std::set<std::string> &c = v.get()->getContexts();
      return shortNames("context", std::vector<std::string>(c.begin(), c.end()));
    })
    .def_prop_ro("file_path", [](const PluginView &v) { return v.get()->getBinary()->getFilePath(); })
    .def("descriptor", [](const PluginView &v, std::optional<std::string> context) {
      OHIE::ImageEffectPlugin *p = v.get();
      return makeEffectDescriptor(p, context ? ofxName("context", *context) : std::string());
    }, "context"_a.none() = nb::none(),
       "What the plug-in described (an ofxh.EffectDescriptor, a copy) in a context (filter, general...;\n"
       "runs the describe in context action the first time). context None: the descriptor of the\n"
       "describe action, before any context (no clips, no parameters); not all the contexts, as in\n"
       "ofxh.describe(plugin, None).")
    .def("__repr__", [](const PluginView &v) {
      return "<ofxh.Plugin " + v.get()->getIdentifier() + " " + std::to_string(v.get()->getVersionMajor()) + "." +
             std::to_string(v.get()->getVersionMinor()) + ">";
    });
}

static void bindImageData(nb::module_ &m)
{
  nb::class_<ImageData>(m, "ImageData",
                        "An image returned by ClipInstance.get_image().\n\n"
                        "pixels: a writable NumPy array of shape (y2-y1, x2-x1, C), C = 4 (RGBA), 3 (RGB) or 1\n"
                        "(Alpha), of the clip's (mapped) depth: uint8, uint16, float16 or float32. Row 0 is y1\n"
                        "(bottom-up, as OFX images); the pixels of a row are contiguous; the rows may have any\n"
                        "stride, negative too. Read-only arrays are refused (the plug-in gets a non-const pointer):\n"
                        "pass a copy.\n"
                        "bounds: (x1, y1, x2, y2) in PIXELS at the render scale (get_image()'s bounds argument is\n"
                        "in canonical coordinates). An output image of a render must contain the render window.\n"
                        "rod: the clip's RoD in pixels (None: from region_of_definition()). field: none, both,\n"
                        "lower or upper. unique_id: \"\" for clip:time:view (as hostDemo).\n"
                        "For a plug-in without multiple resolutions (or in a host that reports\n"
                        "kOfxImageEffectPropSupportsMultiResolution 0), ofxh crops and pads an input image to the\n"
                        "project extent from the origin: the plug-in gets other bounds and RoD than these.")
    .def("__init__", [](ImageData *d, nb::object pixels, const RectI &bounds, std::optional<RectI> rod,
                        const std::string &field, const std::string &uniqueId) {
      new (d) ImageData();
      d->pixels = std::move(pixels);
      d->bounds = tuple(toRectI(bounds));
      d->rod = rod ? nb::object(tuple(toRectI(*rod))) : nb::none();
      d->field = field;
      d->uniqueId = uniqueId;
    }, "pixels"_a, "bounds"_a, "rod"_a.none() = nb::none(), "field"_a = "none", "unique_id"_a = "")
    .def_rw("pixels", &ImageData::pixels)
    .def_prop_rw("bounds", [](const ImageData &d) { return d.bounds; },
                 [](ImageData &d, const RectI &b) { d.bounds = tuple(toRectI(b)); })
    .def_prop_rw("rod", [](const ImageData &d) { return d.rod; },
                 [](ImageData &d, std::optional<RectI> r) { d.rod = r ? nb::object(tuple(toRectI(*r))) : nb::none(); },
                 nb::arg("rod").none())
    .def_rw("field", &ImageData::field)
    .def_rw("unique_id", &ImageData::uniqueId)
    .def("__repr__", [](const ImageData &d) {
      return "ofxh.ImageData(bounds=" + nb::cast<std::string>(nb::repr(d.bounds)) + ")";
    });
}

static void bindHost(nb::module_ &m)
{
  nb::class_<HostBase>(m, "Host",
                       "The host. Subclass it and override the callbacks; pass one to ofxh.init().\n\n"
                       "Callbacks: message(message_type, message_id, text) (message_type: fatal, error, warning,\n"
                       "message, log or question), persistent_message(message_type, message_id, text),\n"
                       "clear_persistent_message(), init_properties(props). The defaults print the messages as\n"
                       "hostDemo does, and answer yes to questions.",
                       nb::dynamic_attr())
    .def(nb::init<>())
    .def("message", [](HostBase &, const std::string &type, const std::string &, const std::string &text) {
      return defaultMessage(type, text);
    }, "type"_a, "id"_a, "text"_a, "A message of the message suite (formatted). Return None or a Stat.")
    .def("persistent_message", [](nb::object self, const std::string &type, const std::string &id, const std::string &text) {
      return self.attr("message")(type, id, text);
    }, "type"_a, "id"_a, "text"_a, "A persistent message (default: message()).")
    .def("clear_persistent_message", [](HostBase &) { return nb::none(); })
    .def("init_properties", [](HostBase &, nb::object) {}, "props"_a,
         "Called by ofxh.init() before the plug-in scan: set the host properties here (props: ofxh.PropertySet).")
    .def_prop_ro("props", [](HostBase &h) { h.get(); return PropertySetView{h.ref, PropertySetView::Kind::host}; },
                 "The host properties (after ofxh.init()).")
    .def_prop_ro("alive", [](HostBase &h) { checkThread("ofxh", false); return h.ref->p != nullptr; });
}

static void bindInstanceActions(nb::class_<InstanceBase> &inst);

static void bindImageEffectInstance(nb::module_ &m)
{
  nb::class_<InstanceBase> inst(m, "ImageEffectInstance",
    "An image effect instance. Subclass it, override the callbacks, and create it with\n"
    "ofxh.create_instance(plugin, context, MyInstance()). close() it (or use `with`).\n\n"
    "Callbacks (the defaults are hostDemo's): the methods new_clip_instance(desc, index),\n"
    "message(message_type, message_id, text), persistent_message(...), clear_persistent_message(),\n"
    "view_name(index), progress_start(message, id), progress_end(), progress_update(t), timeline_get_time(),\n"
    "timeline_goto_time(t), timeline_get_bounds(), edit_begin(name), edit_end(); and the values\n"
    "project_size, project_offset, project_extent, project_pixel_aspect_ratio, effect_duration,\n"
    "frame_rate, frame_recursive, render_scale_recursive, default_output_fielding, view_count, which\n"
    "may be overridden with a property, a class attribute or an instance attribute\n"
    "(self.frame_rate = 30). new_param(desc) makes the parameters: an ofxh.Param, or None (the\n"
    "default) for ofxh's C++ parameter, which stores its value and keys. view_count and view_name\n"
    "(the Nuke extension's plane suite) default to one view, \"main\": a host that renders more views\n"
    "per frame (hostDemo in Nuke or Vegas builds: two) overrides both. view_name is called for the\n"
    "views only: another index is kOfxStatErrBadIndex, and \"\", for the plug-in.\n\n"
    "Actions: the *_action() methods, named after HostSupport's. They return an ofxh.Stat, or\n"
    "(Stat, result). Those with a time move the default timeline there first (as hostDemo), so that\n"
    "paramGetValue and paramSetValue, which use the current time, use the action's. A top-level action\n"
    "leaves it there; an action called by a callback of another one restores the outer action's time.\n"
    "A timeline_get_time() override must follow them: return the action's time during an action\n"
    "(e.g. keep the default, or move with timeline_goto_time()), or paramGetValue (and Param.get()\n"
    "for it) doesn't give the action's value.",
    nb::dynamic_attr());
  inst.def(nb::init<>())
    // --- callbacks (defaults)
    .def("new_clip_instance", [](InstanceBase &, nb::object, int) { return nb::type<ClipBase>()(); }, "desc"_a, "index"_a,
         "Make the clip instance (an ofxh.ClipInstance) of a clip descriptor (an ofxh.ClipDescriptor).")
    .def("new_param", [](InstanceBase &, nb::object) { return nb::none(); }, "desc"_a,
         "Make the parameter of a parameter descriptor (an ofxh.ParamDescriptor): an ofxh.Param subclass\n"
         "instance, whose get()/set()... hold the value, or None (the default) for ofxh's C++ parameter,\n"
         "which stores its value and keys (linear interpolation; held for booleans and choices).")
    .def("message", [](InstanceBase &, const std::string &type, const std::string &id, const std::string &text) -> nb::object {
      State &st = state();
      if (st.host) {
        return st.host->_py.attr("message")(type, id, text);
      }
      return defaultMessage(type, text);
    }, "type"_a, "id"_a, "text"_a, "A message from the plug-in (default: the host's message()).")
    .def("persistent_message", [](nb::object self, const std::string &type, const std::string &id, const std::string &text) {
      return self.attr("message")(type, id, text);
    }, "type"_a, "id"_a, "text"_a)
    .def("clear_persistent_message", [](InstanceBase &) { return nb::none(); })
    .def("view_name", [](InstanceBase &, int) { return std::string("main"); }, "index"_a)
    .def("progress_start", [](InstanceBase &, const std::string &, const std::string &) {}, "message"_a, "id"_a)
    .def("progress_end", [](InstanceBase &) {})
    .def("progress_update", [](InstanceBase &, double) { return true; }, "t"_a)
    .def("timeline_get_time", [](InstanceBase &s) { return s.get()->timeLineTime(); })
    .def("timeline_goto_time", [](InstanceBase &s, double t) { s.get()->setTimeLineTime(t); }, "t"_a,
         "The default stores the time (no callback: nothing to raise). The actions with a time also\n"
         "move it there (see the class docstring).")
    .def("timeline_get_bounds", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      return scoped("timeline_get_bounds()", [&] { return Point(0., p->getEffectDuration()); });
    })
    .def("edit_begin", [](InstanceBase &, const std::string &) { return nb::none(); }, "name"_a)
    .def("edit_end", [](InstanceBase &) { return nb::none(); });
  overridable(inst, "project_size", [](InstanceBase &) { return nb::cast(Point(768., 576.)); });
  overridable(inst, "project_offset", [](InstanceBase &) { return nb::cast(Point(0., 0.)); });
  overridable(inst, "project_extent", [](InstanceBase &) { return nb::cast(Point(768., 576.)); });
  overridable(inst, "project_pixel_aspect_ratio", [](InstanceBase &) { return nb::cast(768. / 720.); });
  overridable(inst, "effect_duration", [](InstanceBase &) { return nb::cast(25.); });
  overridable(inst, "frame_rate", [](InstanceBase &) { return nb::cast(25.); });
  overridable(inst, "frame_recursive", [](InstanceBase &s) { return nb::cast(s.get()->timeLineTime()); });
  overridable(inst, "render_scale_recursive", [](InstanceBase &s) {
    return nb::cast(Point(s.get()->renderScale().x, s.get()->renderScale().y));
  });
  overridable(inst, "default_output_fielding", [](InstanceBase &) { return nb::cast(std::string("none")); });
  overridable(inst, "view_count", [](InstanceBase &) { return nb::cast(1); });
  overridable(inst, "default_output_depth", [](InstanceBase &) { return nb::none(); },
              "The output clip's depth (byte, short, half, float, or the OFX string; the best one the plug-in\n"
              "supports) in the default clip preferences when no input is connected (e.g. a generator), or\n"
              "None (the default): HostSupport's, float. hostDemo uses its -d depth. Another value is a ValueError,\n"
              "raised by the action that sets the default clip preferences (as ofxh.CallbackError).");

  // --- information
  inst.def_prop_ro("alive", [](InstanceBase &s) { checkThread("ofxh", false); return s.ref->p != nullptr; })
    .def_prop_ro("plugin", [](InstanceBase &s) { return PluginView{s.get()->getPlugin(), state().generation}; })
    .def_prop_ro("context", [](InstanceBase &s) { return shortName("context", s.get()->getContext()); })
    .def_prop_ro("props", [](InstanceBase &s) { s.get(); return PropertySetView{s.ref, PropertySetView::Kind::instance}; })
    .def_prop_ro("handle", [](InstanceBase &s) { return reinterpret_cast<uintptr_t>(s.get()->getHandle()); },
                 "The OfxImageEffectHandle, as an int (e.g. for ctypes, with ofxh.fetch_suite()).")
    .def_prop_ro("clips", [](InstanceBase &s) {
      nb::dict d;
      const std::map<std::string, OHIE::ClipInstance *> &c = s.get()->clips();
      for (std::map<std::string, OHIE::ClipInstance *>::const_iterator it = c.begin(); it != c.end(); ++it) {
        if (PyClipInstance *pc = pyClip(it->second)) {
          d[it->first.c_str()] = pc->_py;
        }
      }
      return d;
    }, "The clips, by name.")
    .def("clip", [](InstanceBase &s, const std::string &name) {
      PyClipInstance *c = pyClip(s.get()->getClip(name));
      if (!c) {
        raiseKey(name);
      }
      return c->_py;
    }, "name"_a)
    .def_prop_ro("param_names", [](InstanceBase &s) {
      std::vector<std::string> r;
      const std::list<OH::Param::Instance *> &l = s.get()->getParamList();
      for (std::list<OH::Param::Instance *>::const_iterator it = l.begin(); it != l.end(); ++it) {
        r.push_back((*it)->getName());
      }
      return r;
    })
    .def("param_type", [](InstanceBase &s, const std::string &name) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      return shortName("param_type", p->getType());
    }, "name"_a, "The type of a parameter: integer, double, boolean, choice, string, rgba, double2d...")
    .def("get_param", [](InstanceBase &s, const std::string &name, std::optional<double> time) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      return scoped("get_param()", [&] { return getParamValue(p, time ? &*time : nullptr); });
    }, "name"_a, "time"_a.none() = nb::none(),
       "The value of a parameter at a time (None: the current time, timeline_get_time()), as the plug-in\n"
       "gets it: int, float, bool, str, bytes, or a tuple for several dimensions (None for push buttons,\n"
       "groups and pages). A Python parameter (ofxh.Param) gives what its get() returns.")
    .def("set_param", [](InstanceBase &s, const std::string &name, nb::handle value, std::optional<double> time) {
      checkThread("set_param()", true);
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      return stat(scoped("set_param()", [&] { return setParamValue(p, value, time ? &*time : nullptr); }));
    }, "name"_a, "value"_a.none(), "time"_a.none() = nb::none(),
       "Set the value of a parameter; at a time, the key at that time if it animates (else the value).\n"
       "Without a time, a parameter with keys gets the key at the current time (as paramSetValue).\n"
       "Sends no instance changed action: call param_changed_action() for a user edit, or not at all to\n"
       "load a project before create_instance_action() (\"if the values are being loaded from an old\n"
       "setup, that load should have taken place before the create instance action is called\").\n"
       "A choice takes its index or the label of one of its options (kOfxParamPropChoiceOption; another\n"
       "string is a ValueError that lists them), for C++ and Python parameters alike.\n"
       "Bytes take bytes (or a list of ints, as param_template() gives them). The C++ string choice\n"
       "parameters check the value against the enums (Stat.ErrValue); a Python one (ofxh.Param) checks\n"
       "what it wants in its set(). Not from a plug-in's worker thread (RuntimeError).")
    .def("param", [](InstanceBase &s, const std::string &name) -> nb::object {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      PyParamCore *c = dynamic_cast<PyParamCore *>(p);
      return c ? c->_py : nb::none();
    }, "name"_a, "The ofxh.Param that new_param() made for this parameter, or None (a C++ parameter).")
    .def("param_props", [](InstanceBase &s, const std::string &name) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      return propsDict(p->getProperties());
    }, "name"_a,
       "The properties of a parameter instance, {name: tuple of values} (a copy): what the plug-in may change\n"
       "after describe, e.g. kOfxParamPropEnabled and kOfxParamPropSecret (param_descriptor() has the\n"
       "described values).")
    .def("param_initial", [](InstanceBase &s, const std::string &name) {
      PyEffectInstance *p = s.get();
      OH::Param::Instance *param = p->getParam(name);
      if (!param) {
        raiseKey(name);
      }
      return scoped("param_initial()", [&] { return initialValue(param, p); });
    }, "name"_a,
       "The value a parameter starts with: its default, with the normalised spatial defaults in\n"
       "canonical coordinates for the project (as the C++ parameters and param_template()).")
    .def_prop_ro("param_changed_count", [](InstanceBase &s) { return s.get()->paramChangedCount(); },
                 "The number of kOfxActionInstanceChanged sent for parameters so far (the plug-in's own\n"
                 "changes too): a diagnostic, e.g. to check that a project load sends none.")
    .def("param_descriptor", [](InstanceBase &s, const std::string &name) {
      PyEffectInstance *p = s.get();
      const std::map<std::string, OH::Param::Descriptor *> &d = p->getDescriptor().getParams();
      std::map<std::string, OH::Param::Descriptor *>::const_iterator it = d.find(name);
      if (it == d.end()) {
        raiseKey(name);
      }
      return makeParamDescriptor(*it->second, &p->getDescriptor());
    }, "name"_a, "The ofxh.ParamDescriptor of a parameter (a copy).")
    .def("param_keys", [](InstanceBase &s, const std::string &name) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      return scoped("param_keys()", [&] { return paramKeyTimes(p); });
    }, "name"_a, "The times of the keys of a parameter, in order ([] if it has none).")
    .def("param_curve", [](InstanceBase &s, const std::string &name) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      const std::vector<double> times = scoped("param_curve()", [&] { return paramKeyTimes(p); });
      nb::list r;
      for (size_t k = 0; k < times.size(); ++k) {
        const double t = times[k];
        r.append(nb::make_tuple(t, scoped("param_curve()", [&] { return getParamValue(p, &t); })));
      }
      return r;
    }, "name"_a,
       "The keys of a parameter, [(time, value)...] in time order, with the values of get_param() ([] if it\n"
       "has none). Between the keys, ofxh's C++ parameters interpolate linearly (rounded for integers), and\n"
       "hold booleans and choices; set_param(name, value, time) for each key gives the same curve back.")
    .def("param_values", [](InstanceBase &s, std::optional<double> time) {
      PyEffectInstance *p = s.get();
      nb::dict d;
      const std::list<OH::Param::Instance *> &l = p->getParamList();
      for (std::list<OH::Param::Instance *>::const_iterator it = l.begin(); it != l.end(); ++it) {
        const std::string &t = (*it)->getType();
        if (t == kOfxParamTypeGroup || t == kOfxParamTypePage || t == kOfxParamTypePushButton ||
            t == kOfxParamTypeParametric) {
          continue;
        }
        d[(*it)->getName().c_str()] =
          jsonValue(scoped("param_values()", [&] { return getParamValue(*it, time ? &*time : nullptr); }));
      }
      return d;
    }, "time"_a.none() = nb::none(),
       "The values of the parameters that have one, {name: value}, as JSON-serialisable data (lists, not\n"
       "tuples; bytes as lists of ints), in the form of param_template(): set_param() takes them back.")
    .def("press", [](InstanceBase &s, const std::string &name, std::optional<double> time, const Point &renderScale) {
      PyEffectInstance *p = s.get();
      OH::Param::Instance *param = p->getParam(name);
      if (!param) {
        raiseKey(name);
      }
      if (param->getType() != kOfxParamTypePushButton) {
        raiseError(PyExc_ValueError, "press(): parameter " + name + " is a " + shortName("param_type", param->getType()) +
                                     ", not a push button");
      }
      const double t = time ? *time : scoped("press()", [&] { return p->timeLineGetTime(); });
      const std::string why(kOfxChangeUserEdited);
      ActionScope scope;
      OfxStatus st;
      TimeLineScope tl(p, t);
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = withoutGil([&] {
          p->beginInstanceChangedAction(why);
          const OfxStatus r = p->paramInstanceChangedAction(name, why, t, toPoint(renderScale));
          p->endInstanceChangedAction(why);
          return r;
        });
      }
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "time"_a.none() = nb::none(), "render_scale"_a = Point(1., 1.),
       "Press a push button. This runs actions (despite its name without _action):\n"
       "kOfxActionInstanceChanged (reason \"user edited\", between the begin and end instance changed\n"
       "actions) at time (None: timeline_get_time()). Returns its Stat.")
    .def_prop_ro("rendering", [](InstanceBase &s) { return s.get()->rendering(); })
    .def_prop_ro("render_window", [](InstanceBase &s) { return tuple(s.get()->renderWindow()); },
                 "The render window of the current (or last) render action, in pixels.")
    .def_prop_ro("render_scale", [](InstanceBase &s) { return Point(s.get()->renderScale().x, s.get()->renderScale().y); },
                 "The render scale of the current action ((1, 1) outside the actions).")
    .def_prop_rw("aborted", [](InstanceBase &s) { return s.get()->aborted(); },
                 [](InstanceBase &s, bool a) { s.get()->setAborted(a); },
                 "What the plug-in's abort() gets (an atomic flag).")
    .def_prop_ro("supports_tiles", [](InstanceBase &s) { return s.get()->supportsTiles(); })
    .def_prop_ro("output_premultiplication", [](InstanceBase &s) {
      return shortName("premult", s.get()->getOutputPreMultiplication());
    }, "Set by the clip preferences.")
    .def_prop_ro("output_fielding", [](InstanceBase &s) { return shortName("field", s.get()->getOutputFielding()); })
    .def_prop_ro("output_frame_rate", [](InstanceBase &s) { return s.get()->getOutputFrameRate(); })
    .def_prop_ro("output_pixel_aspect_ratio", [](InstanceBase &s) { return s.get()->getOutputPixelAspectRatio(); },
                 "The output PAR after the clip preferences (Instance::getOutputPixelAspectRatio()).")
    .def_prop_ro("output_continuous_samples", [](InstanceBase &s) { return s.get()->continuousSamples(); })
    .def_prop_ro("output_frame_varying", [](InstanceBase &s) { return s.get()->isFrameVarying(); })
    .def_prop_ro("clip_preferences_dirty", [](InstanceBase &s) { return s.get()->areClipPrefsDirty(); })
    .def("project_pixel_rect", [](InstanceBase &s, const Point &rs) {
      PyEffectInstance *p = s.get();
      return scoped("project_pixel_rect()", [&] { return tuple(p->projectPixelRect(toPoint(rs))); });
    }, "render_scale"_a = Point(1., 1.), "The project, in pixels at the render scale.");

  bindInstanceActions(inst);
}

/// the actions: each returns a Stat, or (Stat, result)
static void bindInstanceActions(nb::class_<InstanceBase> &inst)
{
  inst.def("create_instance_action", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = withoutGil([&] { return p->createInstanceAction(); });
      scope.check("createInstance", &st);
      return stat(st);
    }, "kOfxActionCreateInstance (set the parameter values of a saved project before).")
    .def("get_clip_preferences_action", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = withoutGil([&] { return p->getClipPreferences(); }) ? kOfxStatOK : kOfxStatFailed;
      scope.check("getClipPreferences", &st);
      return stat(st);
    }, "kOfxImageEffectActionGetClipPreferences: sets the depth, components and PAR of the clips.")
    .def("get_region_of_definition_action", [](InstanceBase &s, double time, const Point &renderScale, int view) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      OfxRectD rod;
      rod.x1 = rod.y1 = rod.x2 = rod.y2 = 0.;
      ActionScope scope;
      OfxStatus st;
      TimeLineScope tl(p, time);
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = withoutGil([&] { return p->getRegionOfDefinitionAction(time, toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                            view,
#endif
                                            rod); });
      }
      scope.check("getRegionOfDefinition", &st);
      return nb::make_tuple(stat(st), tuple(rod));
    }, "time"_a, "render_scale"_a = Point(1., 1.), "view"_a = 0, "Returns (Stat, rod) in canonical coordinates.")
    .def("get_regions_of_interest_action", [](InstanceBase &s, double time, const RectD &roi, const Point &renderScale, int view) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      std::map<OHIE::ClipInstance *, OfxRectD> rois;
      ActionScope scope;
      OfxStatus st;
      TimeLineScope tl(p, time);
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = withoutGil([&] { return p->getRegionOfInterestAction(time, toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                          view,
#endif
                                          toRectD(roi), rois); });
      }
      scope.check("getRegionsOfInterest", &st);
      nb::dict d;
      for (std::map<OHIE::ClipInstance *, OfxRectD>::const_iterator it = rois.begin(); it != rois.end(); ++it) {
        d[it->first->getName().c_str()] = tuple(it->second);
      }
      return nb::make_tuple(stat(st), d);
    }, "time"_a, "roi"_a, "render_scale"_a = Point(1., 1.), "view"_a = 0,
       "roi: the region to render, in canonical coordinates. Returns (Stat, {clip: roi}).")
    .def("is_identity_action", [](InstanceBase &s, double time, const RectI &window, const Point &renderScale,
                                  const std::string &field, int view, const std::string &plane) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      OfxTime t = time;
      std::string clip;
#ifdef OFX_EXTENSIONS_NUKE
      int v = view;
      std::string pl = ofxName("plane", plane);
#else
      if (shortName("plane", ofxName("plane", plane)) != "Colour") {
        raiseError(PyExc_ValueError, "plane " + plane + ": this build of ofxh has only the colour plane");
      }
#endif
      ActionScope scope;
      OfxStatus st;
      TimeLineScope tl(p, time);
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = withoutGil([&] { return p->isIdentityAction(t, fieldName(field), toRectI(window), toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                 v, pl,
#endif
                                 clip); });
      }
      scope.check("isIdentity", &st);
      nb::dict d;
      d["clip"] = st == kOfxStatOK ? nb::object(nb::str(clip.c_str())) : nb::none();
      d["time"] = t;
#ifdef OFX_EXTENSIONS_NUKE
      d["view"] = v;
      d["plane"] = shortName("plane", pl);
#else
      d["view"] = view;
      d["plane"] = std::string("Colour");
#endif
      return nb::make_tuple(stat(st), d);
    }, "time"_a, "render_window"_a, "render_scale"_a = Point(1., 1.), "field"_a = "none", "view"_a = 0,
       "plane"_a = "Colour", "Returns (Stat, {clip, time, view, plane}); Stat.OK: an identity.")
    .def("get_frames_needed_action", [](InstanceBase &s, double time) {
      PyEffectInstance *p = s.get();
      OHIE::RangeMap ranges;
      ActionScope scope;
      TimeLineScope tl(p, time);
      const OfxStatus st = withoutGil([&] { return p->getFrameNeededAction(time, ranges); });
      scope.check("getFramesNeeded", &st);
      nb::dict d;
      for (OHIE::RangeMap::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        nb::list l;
        for (size_t k = 0; k < it->second.size(); ++k) {
          l.append(Point(it->second[k].min, it->second[k].max));
        }
        d[it->first->getName().c_str()] = l;
      }
      return nb::make_tuple(stat(st), d);
    }, "time"_a, "Returns (Stat, {clip: [(t1, t2), ...]}).")
    .def("begin_sequence_render_action", [](InstanceBase &s, double first, double last, double step, bool interactive,
                                            const Point &renderScale, bool sequential, bool interactiveRender, bool draft,
                                            int view, bool opengl, const std::string &opencl, const std::string &metal, bool cuda) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      requireNotRendering(p, "begin_sequence_render_action()");
      checkOpenGL(p, opengl && opencl.empty() && metal.empty() && !cuda, "begin_sequence_render_action()");
      checkCUDA(p, opengl, opencl, metal, cuda, "begin_sequence_render_action()");
      checkOpenCL(p, opengl, opencl, "begin_sequence_render_action()");
      checkMetal(p, opengl, opencl, metal, "begin_sequence_render_action()");
      ActionScope scope;
      OfxStatus st;
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        auto act = [&](bool openGLRender, void *contextData) {
          (void)openGLRender;
          (void)contextData;
          return p->beginRenderAction(first, last, step, interactive, toPoint(renderScale), sequential, interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                  openGLRender,
#ifdef OFX_EXTENSIONS_NATRON
                                  contextData,
#endif
#endif
                                  draft
#ifdef OFX_EXTENSIONS_NUKE
                                  , view
#endif
                                  );
        };
        const OfxRectI none = { 0, 0, 0, 0 };
        st = runAction(p, opengl, opencl, metal, cuda, "begin_sequence_render_action()", nullptr, first, none, view, act);
      }
      scope.check("beginSequenceRender", &st);
      return stat(st);
    }, "first"_a, "last"_a, "step"_a = 1., "interactive"_a = false, "render_scale"_a = Point(1., 1.),
       "sequential"_a = true, "interactive_render"_a = false, "draft"_a = false, "view"_a = 0, "opengl"_a = false,
       "opencl"_a = "", "metal"_a = "", "cuda"_a = false)
    .def("render_action", [](InstanceBase &s, double time, const RectI &window, const Point &renderScale,
                             const std::string &field, bool sequential, bool interactiveRender, bool draft, int view,
                             int nViews, const std::vector<std::string> &planes, bool opengl,
                             const std::string &opencl, const std::string &metal, bool cuda) {
      PyEffectInstance *p = s.get();
      checkView(view, true, nViews);
      requireNotRendering(p, "render_action()");
      checkOpenGL(p, opengl && opencl.empty() && metal.empty() && !cuda, "render_action()");
      checkCUDA(p, opengl, opencl, metal, cuda, "render_action()");
      checkOpenCL(p, opengl, opencl, "render_action()");
      checkMetal(p, opengl, opencl, metal, "render_action()");
      if (planes.size() != 1) {
        raiseError(PyExc_ValueError, "planes: ofxh renders one plane per render action");
      }
      if (shortName("plane", ofxName("plane", planes[0])) != "Colour") {
        raiseError(PyExc_ValueError, "plane " + planes[0] + ": ofxh renders the colour plane only");
      }
      // each render gets new output images
      const std::map<std::string, OHIE::ClipInstance *> &clips = p->clips();
      for (std::map<std::string, OHIE::ClipInstance *>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        if (PyClipInstance *c = pyClip(it->second)) {
          c->resetOutput();
        }
      }
      // the output image can be made (the clip preferences set its depth and components): else the
      // output fallback of PyClipInstance::fetchImage() couldn't make one either
      {
        PyClipInstance *out = pyClip(p->getClip(kOfxImageEffectOutputClipName));
        if (!out) {
          raiseError(PyExc_ValueError, "render_action(): the plug-in has no output clip");
        }
        const std::string depth = scoped("render_action()", [&] { return out->getPixelDepth(); });
        const std::string comps = scoped("render_action()", [&] { return out->getComponents(); });
        if (!imageFormatSupported(depth, comps)) {
          raiseError(PyExc_ValueError, "render_action(): the output clip's depth \"" + shortName("depth", depth) +
                                       "\" and components \"" + shortName("components", comps) +
                                       "\" can't be rendered: call create_instance_action() and "
                                       "get_clip_preferences_action() first");
        }
      }
#ifdef OFX_EXTENSIONS_NUKE
      std::list<std::string> pl(1, ofxName("plane", planes[0]));
#endif
      OfxStatus st;
      ActionScope scope;
      TimeLineScope tl(p, time);
      {
        RenderScope rs(p, toPoint(renderScale), toRectI(window));
        auto act = [&](bool openGLRender, void *contextData) {
          (void)openGLRender;
          (void)contextData;
          return p->renderAction(time, fieldName(field), toRectI(window), toPoint(renderScale), sequential, interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                             openGLRender,
#ifdef OFX_EXTENSIONS_NATRON
                             contextData,
#endif
#endif
                             draft
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
                             , view
#endif
#ifdef OFX_EXTENSIONS_VEGAS
                             , nViews
#endif
#ifdef OFX_EXTENSIONS_NUKE
                             , pl
#endif
                             );
        };
        st = runAction(p, opengl, opencl, metal, cuda, "render_action()", p->getClip(kOfxImageEffectOutputClipName), time, toRectI(window),
                       view, act);
      }
      // the output image of this render (the clip keeps the array, not the C++ image)
      nb::dict out;
      if (PyClipInstance *c = pyClip(p->getClip(kOfxImageEffectOutputClipName))) {
        if (c->outputData().is_valid()) {
          out["Colour"] = c->outputData();
        }
        c->releaseOutputImage();
      }
      if (p->takeOutputFailed()) {
        // (the plug-in rendered into an image that ofxh made, see PyClipInstance::fetchImage())
        st = kOfxStatFailed;
      }
      scope.check("render", &st);
      return nb::make_tuple(stat(st), out);
    }, "time"_a, "render_window"_a, "render_scale"_a = Point(1., 1.), "field"_a = "none", "sequential"_a = true,
       "interactive_render"_a = false, "draft"_a = false, "view"_a = 0, "n_views"_a = 1,
       "planes"_a = std::vector<std::string>(1, "Colour"), "opengl"_a = false, "opencl"_a = "", "metal"_a = "",
       "cuda"_a = false,
       "kOfxImageEffectActionRender. render_window: pixels at the render scale. Returns (Stat, {plane:\n"
       "the output ImageData the plug-in fetched}) (empty if it fetched none). ofxh renders one plane,\n"
       "the colour plane; view and n_views need the Nuke or Vegas extensions (see build_config).\n"
       "The output clip must have a depth and components that ofxh can make (ValueError otherwise):\n"
       "call create_instance_action() (which sets the default clip preferences) and\n"
       "get_clip_preferences_action() before: ofxh doesn't run them for you. If\n"
       "get_image() gives no valid output image, ofxh gives the plug-in one over the render window (a\n"
       "plug-in may not check), asks it to abort, and the action returns Stat.Failed and raises\n"
       "CallbackError.\n"
       "opengl: render with OpenGL (ofxh.init(opengl=True); kOfxImageEffectPropOpenGLEnabled 1) in a\n"
       "RenderSession: the plug-in loads its textures with clipLoadTexture, the output texture is read\n"
       "back into the output image; opengl_result says how it went. Also for the sequence actions.\n"
       "opencl: \"buffers\" or \"images\": render with OpenCL (ofxh.init(opencl=True);\n"
       "kOfxImageEffectPropOpenCLEnabled 1 and kOfxImageEffectPropOpenCLCommandQueue) in a RenderSession:\n"
       "get_image()'s images are given to the plug-in as OpenCL buffers or images, and the output is read\n"
       "back into the output image; refused (ValueError) unless render_mode(opencl=True, prefer_images=...)\n"
       "is that mode; opencl_result says how it went. Also for the sequence actions. Not with opengl.\n"
       "metal: \"buffers\" or \"textures\": render with Metal (ofxh.init(metal=True);\n"
       "kOfxImageEffectPropMetalEnabled 1, or kOfxImageEffectPropMetalTextureEnabled 1, and\n"
       "kOfxImageEffectPropMetalCommandQueue) in a RenderSession, likewise; refused (ValueError) unless\n"
       "render_mode(metal=True, prefer_textures=...) is that mode; metal_result says how it went. Not with\n"
       "opengl or opencl.\n"
       "cuda: render with CUDA device memory (ofxh.init(cuda=True); kOfxImageEffectPropCudaEnabled 1, and\n"
       "kOfxImageEffectPropCudaStream if the plug-in supports CUDA streams) in a RenderSession, likewise;\n"
       "refused (ValueError) unless render_mode(cuda=True) is \"cuda\"; cuda_result says how it went. Not\n"
       "with opengl, opencl or metal.")
    .def("render_mode", [](InstanceBase &s, bool opengl, bool opencl, bool preferImages, bool metal,
                           bool preferTextures, bool cuda) -> std::string {
      PyEffectInstance *p = s.get();
      (void)preferImages;
      (void)preferTextures;
      if (cuda) {
#ifdef OFXH_CUDA
        return modeName(renderModeCU(p));
#else
        raiseError(PyExc_ValueError, "render_mode(cuda=True): ofxh was built without OFX_HOSTSUPPORT_CUDA");
#endif
      }
      if (metal) {
#ifdef OFXH_METAL
        return modeName(renderModeMT(p, preferTextures));
#else
        raiseError(PyExc_ValueError, "render_mode(metal=True): ofxh was built without OFX_HOSTSUPPORT_METAL");
#endif
      }
      if (opencl) {
#ifdef OFXH_OPENCL
        return modeName(renderModeCL(p, preferImages));
#else
        raiseError(PyExc_ValueError, "render_mode(opencl=True): ofxh was built without OFX_HOSTSUPPORT_OPENCL");
#endif
      }
#ifdef OFXH_OPENGL
      const GL::RenderMode m = renderMode(p, opengl);
      return m == GL::RenderMode::OpenGL ? "opengl" : m == GL::RenderMode::CPU ? "cpu" : "none";
#else
      (void)opengl;
      return OHIE::canRenderOnCPU(*p) ? "cpu" : "none";
#endif
    }, "opengl"_a = true, "opencl"_a = false, "prefer_images"_a = false, "metal"_a = false, "prefer_textures"_a = false,
       "cuda"_a = false,
       "How the instance renders. With cuda (CU::chooseRenderMode(), from\n"
       "kOfxImageEffectPropCudaRenderSupported and kOfxImageEffectPropCPURenderSupported, with the device of\n"
       "ofxh.init(cuda=True)): \"cuda\", \"cpu\" or \"none\". With metal (MT::chooseRenderMode(), from\n"
       "kOfxImageEffectPropMetalRenderSupported, kOfxImageEffectPropMetalTextureSupported and\n"
       "kOfxImageEffectPropCPURenderSupported, with the device of ofxh.init(metal=True)): \"metal-buffers\",\n"
       "\"metal-textures\" (if both, textures only if prefer_textures; RGBA and Alpha clips only), \"cpu\" or\n"
       "\"none\". With opencl (CL::chooseRenderMode(), from\n"
       "kOfxImageEffectPropOpenCLRenderSupported (Buffers), kOfxImageEffectPropOpenCLSupported (Images)\n"
       "and kOfxImageEffectPropCPURenderSupported, with the device of ofxh.init(opencl=True)):\n"
       "\"opencl-buffers\", \"opencl-images\" (if both, Images only if prefer_images), \"cpu\" or \"none\".\n"
       "Else OpenGL (chooseRenderMode(), from kOfxImageEffectPropOpenGLRenderSupported and\n"
       "kOfxImageEffectPropCPURenderSupported): \"opengl\", \"cpu\" or \"none\" (it needs OpenGL); with\n"
       "the host's OpenGL context if opengl (unless Attached or Detached failed), else as without one.")
    .def("can_render_on_cpu", [](InstanceBase &s) { return OHIE::canRenderOnCPU(*s.get()); },
       "Whether the plug-in can render on the CPU (HostSupport's ImageEffect::canRenderOnCPU(): its\n"
       "kOfxImageEffectPropCPURenderSupported isn't \"false\"), the rule of the CPU retry after a GPU\n"
       "render that wasn't sent or failed.")
#ifdef OFXH_CUDA
    .def_prop_ro("cuda_error", [](InstanceBase &s) { return s.get()->_cuError; },
       "The error that stopped the last action with cuda (an exception, as hostDemo's \"Error: CUDA\n"
       "render: ...\"), or \"\".")
    .def_prop_ro("cuda_result", [](InstanceBase &s) { return s.get()->_cuResult; },
       "The last action with cuda, as opencl_result: \"ok\", \"not_sent\", \"retry\", \"failed\", or \"\" if none.")
#endif
#ifdef OFXH_METAL
    .def_prop_ro("metal_error", [](InstanceBase &s) { return s.get()->_mtError; },
       "The error that stopped the last action with metal (an exception, as hostDemo's \"Error: Metal\n"
       "render: ...\"), or \"\".")
    .def_prop_ro("metal_result", [](InstanceBase &s) { return s.get()->_mtResult; },
       "The last action with metal, as opencl_result: \"ok\", \"not_sent\", \"retry\", \"failed\", or \"\" if none.")
#endif
#ifdef OFXH_OPENCL
    .def_prop_ro("opencl_error", [](InstanceBase &s) { return s.get()->_clError; },
       "The error that stopped the last action with opencl (an exception, as hostDemo's \"Error: OpenCL\n"
       "render: ...\"), or \"\".")
    .def_prop_ro("opencl_result", [](InstanceBase &s) { return s.get()->_clResult; },
       "The last action with opencl: \"ok\"; \"not_sent\" (the RenderSession refused it: the plug-in's or\n"
       "the host descriptor's property doesn't allow the mode, or no command queue; the action wasn't\n"
       "sent); \"retry\" (an image couldn't be copied, or kOfxStatGPURenderFailed, kOfxStatGPUOutOfMemory or\n"
       "kOfxStatErrMemory, and the plug-in can render on the CPU: render it again there); \"failed\" (the\n"
       "same, or an error, for a plug-in that can't); \"\" if none.")
#endif
#ifdef OFXH_OPENGL
    .def_prop_ro("opengl_error", [](InstanceBase &s) { return s.get()->_glError; },
       "The error that stopped the last action with opengl=True (an OpenGL error, as hostDemo's \"Error:\n"
       "OpenGL render: ...\"), or \"\".")
    .def_prop_ro("opengl_result", [](InstanceBase &s) { return s.get()->_glResult; },
       "The last action with opengl=True: \"ok\"; \"not_sent\" (the instance couldn't be attached: the\n"
       "action wasn't sent); \"retry\" (an input texture failed, or kOfxStatGPURenderFailed,\n"
       "kOfxStatGPUOutOfMemory or kOfxStatErrMemory, and the plug-in can render on the CPU: render it\n"
       "again there); \"failed\" (the same, or an error, for a plug-in that can't); \"\" if none.")
#endif
    .def("end_sequence_render_action", [](InstanceBase &s, double first, double last, double step, bool interactive,
                                          const Point &renderScale, bool sequential, bool interactiveRender, bool draft,
                                          int view, bool opengl, const std::string &opencl, const std::string &metal, bool cuda) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      requireNotRendering(p, "end_sequence_render_action()");
      checkOpenGL(p, opengl && opencl.empty() && metal.empty() && !cuda, "end_sequence_render_action()");
      checkCUDA(p, opengl, opencl, metal, cuda, "end_sequence_render_action()");
      checkOpenCL(p, opengl, opencl, "end_sequence_render_action()");
      checkMetal(p, opengl, opencl, metal, "end_sequence_render_action()");
      ActionScope scope;
      OfxStatus st;
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        auto act = [&](bool openGLRender, void *contextData) {
          (void)openGLRender;
          (void)contextData;
          return p->endRenderAction(first, last, step, interactive, toPoint(renderScale), sequential, interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                  openGLRender,
#ifdef OFX_EXTENSIONS_NATRON
                                  contextData,
#endif
#endif
                                  draft
#ifdef OFX_EXTENSIONS_NUKE
                                  , view
#endif
                                  );
        };
        const OfxRectI none = { 0, 0, 0, 0 };
        st = runAction(p, opengl, opencl, metal, cuda, "end_sequence_render_action()", nullptr, first, none, view, act);
      }
      scope.check("endSequenceRender", &st);
      return stat(st);
    }, "first"_a, "last"_a, "step"_a = 1., "interactive"_a = false, "render_scale"_a = Point(1., 1.),
       "sequential"_a = true, "interactive_render"_a = false, "draft"_a = false, "view"_a = 0, "opengl"_a = false,
       "opencl"_a = "", "metal"_a = "", "cuda"_a = false)
    .def("get_time_domain_action", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      OfxRangeD r;
      r.min = r.max = 0.;
      ActionScope scope;
      const OfxStatus st = withoutGil([&] { return p->getTimeDomainAction(r); });
      scope.check("getTimeDomain", &st);
      return nb::make_tuple(stat(st), Point(r.min, r.max));
    }, "Returns (Stat, (t1, t2)) (the general context only).")
#ifdef OFX_EXTENSIONS_NUKE
    .def("get_clip_components_action", [](InstanceBase &s, double time, int view) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      OHIE::ComponentsMap components;
      OHIE::ClipInstance *passThroughClip = nullptr;
      OfxTime passThroughTime = time;
      int passThroughView = view;
      ActionScope scope;
      OfxStatus st;
      {
        TimeLineScope tl(p, time);
        st = withoutGil([&] {
          return p->getClipComponentsAction(time, view, components, passThroughClip, passThroughTime, passThroughView);
        });
      }
      scope.check("getClipComponents", &st);
      // (by clip name: the map is ordered by the clips' addresses)
      nb::dict clips;
      for (const auto &[name, clip] : p->clips()) {
        const OHIE::ComponentsMap::const_iterator found = components.find(clip);
        if (found != components.end()) {
          nb::list l;
          for (const std::string &c : found->second) {
            l.append(nb::str(c.c_str()));
          }
          clips[name.c_str()] = l;
        }
      }
      nb::dict d;
      d["clips"] = clips;
      d["pass_through_clip"] = passThroughClip ? nb::object(nb::str(passThroughClip->getName().c_str())) : nb::none();
      d["pass_through_time"] = passThroughTime;
      d["pass_through_view"] = passThroughView;
      return nb::make_tuple(stat(st), d);
    }, "time"_a, "view"_a = 0,
       "kFnOfxImageEffectActionGetClipComponents (Nuke extension builds): the planes the plug-in needs on each\n"
       "clip and produces on the output, and the clip, time and view it passes the other planes through\n"
       "from. Returns (Stat, {clips: {clip: [plane, ...]} (the plug-in's strings, empty unless Stat.OK),\n"
       "pass_through_clip (None if none), pass_through_time, pass_through_view}).")
#endif
    .def("begin_instance_changed_action", [](InstanceBase &s, const std::string &why) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = withoutGil([&] { return p->beginInstanceChangedAction(ofxName("change", why)); });
      scope.check("beginInstanceChanged", &st);
      return stat(st);
    }, "why"_a = "user", "why: user, plugin or time (or the kOfxChange* value).")
    .def("param_changed_action", [](InstanceBase &s, const std::string &name, const std::string &why, std::optional<double> at,
                                    const Point &renderScale) {
      PyEffectInstance *p = s.get();
      // (None: the current time, as press())
      const double time = at ? *at : scoped("param_changed_action()", [&] { return p->timeLineGetTime(); });
      ActionScope scope;
      OfxStatus st;
      TimeLineScope tl(p, time);
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = withoutGil([&] { return p->paramInstanceChangedAction(name, ofxName("change", why), time, toPoint(renderScale)); });
      }
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "why"_a = "user", "time"_a.none() = nb::none(), "render_scale"_a = Point(1., 1.),
       "kOfxActionInstanceChanged for a parameter, at time (None: timeline_get_time()). Call it between\n"
       "begin_instance_changed_action() and end_instance_changed_action().")
    .def("clip_changed_action", [](InstanceBase &s, const std::string &name, const std::string &why, std::optional<double> at,
                                   const Point &renderScale) {
      PyEffectInstance *p = s.get();
      // (None: the current time, as press())
      const double time = at ? *at : scoped("clip_changed_action()", [&] { return p->timeLineGetTime(); });
      ActionScope scope;
      OfxStatus st;
      TimeLineScope tl(p, time);
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = withoutGil([&] { return p->clipInstanceChangedAction(name, ofxName("change", why), time, toPoint(renderScale)); });
      }
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "why"_a = "user", "time"_a.none() = nb::none(), "render_scale"_a = Point(1., 1.),
       "kOfxActionInstanceChanged for a clip, at time (None: timeline_get_time()).")
    .def("end_instance_changed_action", [](InstanceBase &s, const std::string &why) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = withoutGil([&] { return p->endInstanceChangedAction(ofxName("change", why)); });
      scope.check("endInstanceChanged", &st);
      return stat(st);
    }, "why"_a = "user")
    .def("close", [](InstanceBase &s) {
      PyEffectInstance *p = s.ref->as<PyEffectInstance>();
      if (!p) {
        return;  // (already closed: a no-op)
      }
      requireNoAction("ImageEffectInstance.close()");
      ActionScope scope;
      // (the status of kOfxActionDestroyInstance doesn't matter: the instance is deleted anyway)
      withoutGil([&] { return p->destroyInstanceAction(); });
      delete p;  // the clips and params too; the Python objects raise ReferenceError from now on
      scope.check("destroyInstance");
    }, "kOfxActionDestroyInstance, then delete the instance (and its clips). Returns None; a second\n"
       "close() does nothing. Not in an action or a callback.")
    .def("__enter__", [](nb::object self) { return self; })
    .def("__exit__", [](nb::object self, nb::args) {
      self.attr("close")();
      return false;
    });
}

static void bindClipInstance(nb::module_ &m)
{
  nb::class_<ClipBase> clip(m, "ClipInstance",
    "A clip of an instance. Subclass it, and return one from ImageEffectInstance.new_clip_instance().\n\n"
    "Callbacks (the defaults are hostDemo's): the methods get_image(time, view, bounds, plane=\"Colour\")\n"
    "(ofxh renders the colour plane only, one plane per render action; with the Nuke extension, a\n"
    "plug-in may fetch another plane of an input, see components_present) and\n"
    "region_of_definition(time, view); and the values connected, unmapped_bit_depth,\n"
    "unmapped_components, premult, format, aspect_ratio, frame_rate, frame_range, field_order,\n"
    "unmapped_frame_rate, unmapped_frame_range, continuous_samples, components_present (Nuke extension\n"
    "builds), which may be overridden with a\n"
    "property, a class attribute or an instance attribute. Depths, components, premultiplications and\n"
    "fields are short names (byte, rgba, premult, none...; the OFX strings are accepted too).\n\n"
    "The output clip gives one image per render action: the first one get_image() returns, to every\n"
    "fetch of the render, whatever its view (e.g. a Vegas stereoscopic fetch of another view).",
    nb::dynamic_attr());
  clip.def(nb::init<>())
    // --- callbacks (defaults)
    .def("get_image", [](ClipBase &s, double time, int view, nb::object, const std::string &) -> nb::object {
      PyClipInstance *c = s.get();
      if (!c->isOutput()) {
        return nb::none();
      }
      // the output: the render window (outside a render, the RoD clipped to the project)
      PyEffectInstance *e = c->_effect;
      OfxRectI b = e->renderWindow();
      if (!e->rendering()) {
        b = scoped("get_image()", [&] {
          return intersect(canonicalToPixelEnclosing(c->rod(time, view), e->renderScale(), c->getAspectRatio()),
                           e->projectPixelRect(e->renderScale()));
        });
      }
      ImageData d;
      d.pixels = scoped("get_image()", [&] { return c->newImage(b.x1, b.y1, b.x2, b.y2); });
      d.bounds = tuple(b);
      d.rod = nb::none();
      d.field = "none";
      return nb::cast(std::move(d));
    }, "time"_a, "view"_a, "bounds"_a.none(), "plane"_a = "Colour",
       "Return the image (an ofxh.ImageData, or None) at this time and view. bounds: the region the\n"
       "plug-in asked for, in canonical coordinates, or None. plane: \"Colour\" (the only one ofxh\n"
       "renders), or for an input a plane of its components_present with two components (Nuke\n"
       "extension builds): ForwardMotionVector or BackMotionVector (components motionvectors),\n"
       "StereoDisparityLeft or StereoDisparityRight (disparity); its image is (H, W, 2), and ofxh gives\n"
       "it the plane's components and the premultiplication opaque. The default makes the output image\n"
       "(filled with a sentinel) over the render window, and returns None for the inputs.")
    .def("region_of_definition", [](ClipBase &s, double time, int view) {
      PyClipInstance *c = s.get();
      return scoped("region_of_definition()", [&] { return tuple(c->defaultRoD(time, view)); });
    }, "time"_a, "view"_a,
       "The RoD in canonical coordinates. The default: the effect's RoD for the output, the project for\n"
       "a connected input, (0, 0, 0, 0) for a disconnected one. The output's default runs the plug-in's\n"
       "RoD action: a plug-in's worker thread that asks for it is refused (RuntimeError, so its call\n"
       "fails), as it would run the action concurrently with the one in progress.");
  overridable(clip, "connected", [](ClipBase &) { return nb::cast(true); });
  overridable(clip, "unmapped_bit_depth", [](ClipBase &) { return nb::cast(std::string("byte")); });
  overridable(clip, "unmapped_components", [](ClipBase &) { return nb::cast(std::string("rgba")); });
  overridable(clip, "premult", [](ClipBase &s) {
    // the output: the clip preferences; an RGB or disconnected input: opaque ("an image with no alpha
    // is opaque"); else unpremultiplied
    PyClipInstance *c = s.get();
    std::string r;
    if (c->isOutput()) {
      r = shortName("premult", c->_effect->getOutputPreMultiplication());
    } else if (!scoped("premult", [&] { return c->getConnected(); }) ||
               scoped("premult", [&] { return c->getComponents(); }) == kOfxImageComponentRGB) {
      r = "opaque";
    } else {
      r = "unpremult";
    }
    return nb::cast(r);
  });
  overridable(clip, "format", [](ClipBase &s) {
    OfxPointD one;
    one.x = one.y = 1.;
    PyEffectInstance *e = s.get()->_effect;
    return nb::object(scoped("format", [&] { return tuple(e->projectPixelRect(one)); }));
  }, "The format in pixels (Natron extension); default: the project.");
  overridable(clip, "aspect_ratio", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(scoped("aspect_ratio", [&] { return e->getProjectPixelAspectRatio(); }));
  });
  overridable(clip, "frame_rate", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(scoped("frame_rate", [&] { return e->getFrameRate(); }));
  });
  overridable(clip, "frame_range", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(scoped("frame_range", [&] { return Point(0., e->getEffectDuration()); }));
  });
  overridable(clip, "field_order", [](ClipBase &) { return nb::cast(std::string("none")); });
  overridable(clip, "unmapped_frame_rate", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(scoped("unmapped_frame_rate", [&] { return e->getFrameRate(); }));
  });
  overridable(clip, "unmapped_frame_range", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(scoped("unmapped_frame_range", [&] { return Point(0., e->getEffectDuration()); }));
  });
  overridable(clip, "continuous_samples", [](ClipBase &) { return nb::cast(false); });
#ifdef OFX_EXTENSIONS_NUKE
  overridable(clip, "components_present", [](ClipBase &s) {
    PyClipInstance *c = s.get();
    nb::list l;
    const std::vector<std::string> present =
      scoped("components_present", [&] { return c->OHIE::ClipInstance::getComponentsPresent(); });
    for (const std::string &p : present) {
      l.append(nb::str(shortName("plane", p).c_str()));
    }
    return nb::object(l);
  }, "The planes of the clip (kFnOfxImageEffectPropComponentsPresent, Nuke extension builds), short names\n"
     "or OFX names. The default, HostSupport's: Colour, none on a disconnected input. A multi-plane host\n"
     "(kFnOfxImageEffectPropMultiPlanar) lists the other planes of an input, which get_image() gives.");
#endif
  // --- information
  clip.def_prop_ro("alive", [](ClipBase &s) { checkThread("ofxh", false); return s.ref->p != nullptr; })
    .def_prop_ro("name", [](ClipBase &s) { return s.get()->getName(); })
    .def_prop_ro("is_output", [](ClipBase &s) { return s.get()->isOutput(); })
    .def_prop_ro("is_optional", [](ClipBase &s) { return s.get()->isOptional(); })
    .def_prop_ro("is_mask", [](ClipBase &s) { return s.get()->isMask(); })
    .def_prop_ro("supports_tiles", [](ClipBase &s) { return s.get()->supportsTiles(); })
    .def_prop_ro("supported_components", [](ClipBase &s) {
      return shortNames("components", s.get()->getSupportedComponents());
    })
    .def_prop_ro("pixel_depth", [](ClipBase &s) {
      PyClipInstance *c = s.get();
      return shortName("depth", scoped("pixel_depth", [&] { return c->getPixelDepth(); }));
    }, "The depth of the images (set by the clip preferences): byte, short, half, float or none.")
    .def_prop_ro("components", [](ClipBase &s) {
      PyClipInstance *c = s.get();
      return shortName("components", scoped("components", [&] { return c->getComponents(); }));
    }, "The components of the images (set by the clip preferences): rgba, rgb, alpha or none.")
    .def_prop_ro("dtype", [](ClipBase &s) -> nb::object {
      PyClipInstance *c = s.get();
      const std::string d = scoped("dtype", [&] { return c->getPixelDepth(); });
      if (d == kOfxBitDepthByte) return nb::str("uint8");
      if (d == kOfxBitDepthShort) return nb::str("uint16");
      if (d == kOfxBitDepthHalf) return nb::str("float16");
      if (d == kOfxBitDepthFloat) return nb::str("float32");
      return nb::none();
    }, "The NumPy dtype name of the images (from pixel_depth), or None.")
    .def_prop_ro("view", [](ClipBase &s) { return s.get()->view(); })
    .def_prop_ro("instance", [](ClipBase &s) { return s.get()->_effect->_py; })
    .def_prop_ro("props", [](ClipBase &s) { s.get(); return PropertySetView{s.ref, PropertySetView::Kind::clip}; })
    .def("new_image", [](ClipBase &s, const RectI &b) {
      PyClipInstance *c = s.get();
      return scoped("new_image()", [&] { return c->newImage(b[0], b[1], b[2], b[3]); });
    }, "bounds"_a, "An empty NumPy array for an image of these bounds (pixels), of the clip's depth and components.");
}

#ifdef OFXH_METAL
/// a DeviceInfo for metal_devices() and metal_device()
static nb::dict deviceDict(const MT::DeviceInfo &info)
{
  nb::dict d;
  d["index"] = info.index;
  d["name"] = info.name;
  d["registry_id"] = info.registryID;
  d["system_default"] = info.systemDefault;
  d["unified_memory"] = info.unifiedMemory;
  d["recommended_max_working_set_size"] = info.recommendedMaxWorkingSetSize;
  d["max_buffer_length"] = info.maxBufferLength;
  return d;
}
#endif

#ifdef OFXH_CUDA
/// a DeviceInfo for cuda_devices() and cuda_device()
static nb::dict deviceDict(const CU::DeviceInfo &info)
{
  nb::dict d;
  d["index"] = info.index;
  d["name"] = info.name;
  d["compute_capability"] = nb::make_tuple(info.computeMajor, info.computeMinor);
  d["pci_bus_id"] = info.pciBusId;
  d["uuid"] = info.uuid;
  d["total_mem"] = info.totalMem;
  d["driver_version"] = info.driverVersion;
  return d;
}
#endif

#ifdef OFXH_OPENCL
/// a DeviceInfo for opencl_devices() and opencl_device()
static nb::dict deviceDict(const CL::DeviceInfo &info)
{
  nb::dict d;
  d["platform"] = info.platform;
  d["device"] = info.device;
  d["platform_name"] = info.platformName;
  d["platform_version"] = info.platformVersion;
  d["name"] = info.name;
  d["vendor"] = info.vendor;
  d["version"] = info.version;
  d["driver_version"] = info.driverVersion;
  d["gpu"] = info.gpu;
  d["images"] = info.imageSupport;
  d["global_mem_size"] = info.globalMemSize;
  return d;
}
#endif

static void bindFunctions(nb::module_ &m)
{
  m.def("init", &init, "host"_a, "plugin_dirs"_a.none() = nb::none(), "use_std_paths"_a = false,
        "use_env_path"_a = true, "profile"_a = "default", "keep_loaded"_a = false, "normalised_defaults"_a = true,
        "opengl"_a = false, "opengl_backends"_a.none() = nb::none(), "opengl_renderer"_a = -1,
        "opencl"_a = false, "opencl_device"_a.none() = nb::none(), "metal"_a = false, "metal_device"_a = -1,
        "cuda"_a = false, "cuda_device"_a = -1, "cuda_uuid"_a = "",
        "Initialize HostSupport with this host (an ofxh.Host), and scan the plug-ins of plugin_dirs, of\n"
        "OFX_PLUGIN_PATH if use_env_path, and of the standard locations if use_std_paths. One host per\n"
        "process: RuntimeError if one is initialized; call shutdown() before another init(). If it raises,\n"
        "it shuts down first.\n"
        "profile: \"default\", or \"ofx14\" for an OFX 1.4 host, as hostDemo --ofx14: after\n"
        "Host.init_properties(), only the OFX 1.4 host properties remain (a plug-in reading another one, e.g.\n"
        "kOfxParamHostPropSupportsStrChoice, gets kOfxStatErrUnknown), fetch_suite() gives only the OFX 1.4\n"
        "suites, OfxSetHost isn't called, and paramDefine of a StrChoice or Bytes parameter fails. One\n"
        "view: the ofxhost in-process backend renders one view in this profile, whatever the build. There is\n"
        "no OFX 1.1 profile: hostDemo --ofx11 is a hostDemo test hook.\n"
        "keep_loaded: keep the scanned plug-ins loaded (Host::keepPluginsLoadedAfterScan(), as hostDemo with\n"
        "OFX_HOSTDEMO_KEEP_LOADED=1): one kOfxActionLoad and Describe per plug-in, and Unload at shutdown().\n"
        "By default the scan sends Load, Describe and Unload and unloads the binary, and the first use of a\n"
        "plug-in loads it again.\n"
        "normalised_defaults: False for a host without normalised parameter defaults, as hostDemo\n"
        "--no-normalised-defaults: the double param descriptors have no kOfxParamPropDefaultCoordinateSystem\n"
        "(a plug-in's set fails with kOfxStatErrUnknown), so their defaults are canonical.\n"
        "opengl: create an offscreen OpenGL context (openfx::HostSupportOpenGL, builds with\n"
        "OFX_HOSTSUPPORT_OPENGL) before the scan; if it is made, the host's\n"
        "kOfxImageEffectPropOpenGLRenderSupported is \"true\", and the actions may take opengl=True. See\n"
        "opengl_context().\n"
        "opengl_backends: the backends to try, by name (\"egl-surfaceless\", \"egl-device\", \"glx\", \"cgl\",\n"
        "\"wgl\"; default: the platform's); opengl_renderer: the renderer id of opengl_renderers() (default -1,\n"
        "the backend's default). Both need opengl=True: ValueError otherwise, or for an unknown backend.\n"
        "If either is given and no context can be made, init() raises ValueError, whose opengl_error\n"
        "attribute says why, before the scan; with neither, it goes on without a context, and\n"
        "opengl_context() says why.\n"
        "opencl: create an OpenCL context (openfx::HostSupportOpenCL, builds with OFX_HOSTSUPPORT_OPENCL)\n"
        "before the scan, on opencl_device, (platform, device) of opencl_devices(), or by default the first\n"
        "GPU, else the first device; if it is made, the host's kOfxImageEffectPropOpenCLRenderSupported\n"
        "(Buffers) is \"true\", and kOfxImageEffectPropOpenCLSupported (Images) too if the device has\n"
        "images, the host provides the OpenCL program suite (OfxOpenCLProgramSuiteV1, built for the\n"
        "context of the render's queue), and the actions may take opencl=\"buffers\" or \"images\". See\n"
        "opencl_device().\n"
        "metal: likewise with a Metal device (openfx::HostSupportMetal, macOS builds with OFX_HOSTSUPPORT_METAL),\n"
        "metal_device, an index of metal_devices(), or by default (-1) the system default device; if it is made,\n"
        "the host's kOfxImageEffectPropMetalRenderSupported (buffers) and kOfxImageEffectPropMetalTextureSupported\n"
        "(textures) are \"true\", and the actions may take metal=\"buffers\" or \"textures\".\n"
        "See metal_device().\n"
        "cuda: likewise with a CUDA device (openfx::HostSupportCUDA, builds with OFX_HOSTSUPPORT_CUDA; the CUDA\n"
        "driver is loaded at run time), cuda_device, an index of cuda_devices(), or cuda_uuid, the \"uuid\" of\n"
        "one (the identity to save: the indices change with CUDA_VISIBLE_DEVICES), or by default the first\n"
        "device; if it is made, the host's kOfxImageEffectPropCudaRenderSupported and\n"
        "kOfxImageEffectPropCudaStreamSupported are \"true\", and the actions may take cuda=True. See\n"
        "cuda_device().");

  m.def("opengl_renderers", [](nb::object backends) {
    nb::list r;
#ifdef OFXH_OPENGL
    const std::vector<GL::Backend> b = parseBackends(backends, "ofxh.opengl_renderers()");
    std::vector<GL::RendererInfo> infos;
    withoutGil([&] {
      infos = GL::listRenderers(GL::Hooks(), b);
      return 0;
    });
    for (const GL::RendererInfo &info : infos) {
      r.append(rendererDict(info));
    }
#else
    (void)backends;
#endif
    return r;
  }, "backends"_a.none() = nb::none(),
     "The OpenGL renderers of these backends (by name; default: the platform's), each a dict: {\"backend\",\n"
     "\"id\" (for init(opengl_renderer=...)), \"renderer\", \"vendor\", \"version\", \"glsl\", \"video_memory\"\n"
     "(bytes, 0 if unknown), \"software\", \"max_texture_size\"}. A context is made on each to read them. []\n"
     "without OFX_HOSTSUPPORT_OPENGL. Needs no init().");
  m.def("opengl_context", []() -> nb::object {
    nb::dict d;
#ifdef OFXH_OPENGL
    const PyHost *h = state().host.get();
    if (!h || (!h->_glContext && h->_glError.empty())) {
      return nb::none();
    }
    if (!h->_glContext) {
      d["error"] = h->_glError;
      return d;
    }
    return rendererDict(h->_glContext->info());
#else
    return nb::none();
#endif
  }, "The OpenGL context of ofxh.init(opengl=True), as a renderer of opengl_renderers() (\"backend\", \"id\",\n"
     "\"renderer\"...), or {\"error\": why} if none could be made; None without opengl=True (or\n"
     "OFX_HOSTSUPPORT_OPENGL).");
  m.def("opencl_devices", []() {
    nb::list l;
#ifdef OFXH_OPENCL
    std::vector<CL::DeviceInfo> devices;
    withoutGil([&] {
      devices = CL::listDevices(CL::Hooks());
      return 0;
    });
    for (const CL::DeviceInfo &info : devices) {
      l.append(deviceDict(info));
    }
#endif
    return l;
  }, "The OpenCL devices of all the platforms (openfx::HostSupportOpenCL's listDevices()), as dicts\n"
     "{\"platform\", \"device\" (the indices for init(opencl_device=...)), \"platform_name\",\n"
     "\"platform_version\", \"name\", \"vendor\", \"version\", \"driver_version\", \"gpu\", \"images\",\n"
     "\"global_mem_size\"}; [] without OpenCL (or OFX_HOSTSUPPORT_OPENCL). Needs no init().");
  m.def("opencl_device", []() -> nb::object {
#ifdef OFXH_OPENCL
    const PyHost *h = state().host.get();
    if (!h || (!h->_clDevice && h->_clError.empty())) {
      return nb::none();
    }
    if (!h->_clDevice) {
      nb::dict d;
      d["error"] = h->_clError;
      return d;
    }
    return deviceDict(h->_clDevice->info());
#else
    return nb::none();
#endif
  }, "The OpenCL device of ofxh.init(opencl=True), as opencl_devices() gives it, or {\"error\": why} if\n"
     "none could be made; None without opencl=True (or OFX_HOSTSUPPORT_OPENCL).");
  m.def("metal_devices", []() {
    nb::list l;
#ifdef OFXH_METAL
    std::vector<MT::DeviceInfo> devices;
    withoutGil([&] {
      devices = MT::listDevices(MT::Hooks());
      return 0;
    });
    for (const MT::DeviceInfo &info : devices) {
      l.append(deviceDict(info));
    }
#endif
    return l;
  }, "The Metal devices (openfx::HostSupportMetal's listDevices()), as dicts {\"index\" (for\n"
     "init(metal_device=...)), \"name\", \"registry_id\", \"system_default\",\n"
     "\"unified_memory\", \"recommended_max_working_set_size\", \"max_buffer_length\"}; [] without\n"
     "Metal (or OFX_HOSTSUPPORT_METAL). Needs no init().");
  m.def("metal_device", []() -> nb::object {
#ifdef OFXH_METAL
    const PyHost *h = state().host.get();
    if (!h || (!h->_mtDevice && h->_mtError.empty())) {
      return nb::none();
    }
    if (!h->_mtDevice) {
      nb::dict d;
      d["error"] = h->_mtError;
      return d;
    }
    return deviceDict(h->_mtDevice->info());
#else
    return nb::none();
#endif
  }, "The Metal device of ofxh.init(metal=True), as metal_devices() gives it, or {\"error\": why} if none\n"
     "could be made; None without metal=True (or OFX_HOSTSUPPORT_METAL).");
  m.def("cuda_devices", []() {
    nb::list l;
#ifdef OFXH_CUDA
    std::vector<CU::DeviceInfo> devices;
    withoutGil([&] {
      devices = CU::listDevices(CU::Hooks());
      return 0;
    });
    for (const CU::DeviceInfo &info : devices) {
      l.append(deviceDict(info));
    }
#endif
    return l;
  }, "The CUDA devices (openfx::HostSupportCUDA's listDevices()), as dicts {\"index\" (for\n"
     "init(cuda_device=...)), \"name\", \"uuid\" (for init(cuda_uuid=...)), \"compute_capability\" (major, minor),\n"
     "\"pci_bus_id\", \"total_mem\",\n"
     "\"driver_version\"}; [] without a CUDA driver (or OFX_HOSTSUPPORT_CUDA). Needs no init().");
  m.def("cuda_device", []() -> nb::object {
#ifdef OFXH_CUDA
    const PyHost *h = state().host.get();
    if (!h || (!h->_cuDevice && h->_cuError.empty())) {
      return nb::none();
    }
    if (!h->_cuDevice) {
      nb::dict d;
      d["error"] = h->_cuError;
      return d;
    }
    return deviceDict(h->_cuDevice->info());
#else
    return nb::none();
#endif
  }, "The CUDA device of ofxh.init(cuda=True), as cuda_devices() gives it, or {\"error\": why} if none\n"
     "could be made; None without cuda=True (or OFX_HOSTSUPPORT_CUDA).");
  m.def("shutdown", &shutdown,
        "Close the open instances, unload the plug-ins and delete the plug-in cache and the host. Also\n"
        "called at exit. Not in an action or a callback.");
  m.def("initialized", []() { return state().initialized; });
  m.def("profile", []() -> nb::object {
    return state().initialized ? nb::object(nb::str(state().profile.c_str())) : nb::none();
  }, "The profile of the initialized host (\"default\" or \"ofx14\", see init()), or None.");
  m.def("keep_loaded", []() -> nb::object {
    const State &st = state();
    return st.host ? nb::object(nb::bool_(st.host->_keepLoaded)) : nb::none();
  }, "Whether the initialized host keeps the scanned plug-ins loaded (see init()), or None.");
  m.def("host", []() -> nb::object { return state().host ? state().host->_py : nb::none(); });
  m.def("plugins", []() {
    requireInit();
    std::vector<PluginView> r;
    const std::vector<OHIE::ImageEffectPlugin *> &p = state().pluginCache->getPlugins();
    for (size_t i = 0; i < p.size(); ++i) {
      r.push_back(PluginView{p[i], state().generation});
    }
    return r;
  }, "The image effect plug-ins found.");
  m.def("get_plugin", [](const std::string &id, int major, int minor) -> nb::object {
    requireInit();
    OHIE::ImageEffectPlugin *p = state().pluginCache->getPluginById(id, major, minor);
    if (!p) {
      return nb::none();
    }
    return nb::cast(PluginView{p, state().generation});
  }, "id"_a, "major"_a = -1, "minor"_a = -1, "The plug-in with this identifier (the latest version), or None.");
  m.def("create_instance", &createInstance, "plugin"_a, "context"_a, "instance"_a,
        "Make an instance of the plug-in in the context (filter, general, ... or the kOfxImageEffectContext*\n"
        "value), bound to instance (a new ofxh.ImageEffectInstance): describes the context if needed, makes\n"
        "the clips (instance.new_clip_instance()) and the parameters. Returns instance, or None if it failed\n"
        "(a CallbackError if a callback raised, e.g. new_clip_instance()). If a callback raised but the\n"
        "instance was made, CallbackError is raised with the instance as e.instance, still bound:\n"
        "close() it.\n"
        "Then call instance.create_instance_action() (kOfxActionCreateInstance).");
  m.def("describe", [](const PluginView &v, std::optional<std::string> context) {
    requireInit();
    return describePlugin(v.get(), context ? ofxName("context", *context) : std::string());
  }, "plugin"_a, "context"_a.none() = nb::none(),
     "hostDemo's --describe JSON of a plug-in, as Python objects (what ofxhost.describe() reads):\n"
     "{schema, schemaVersion, ok, plugin, contexts: {context: {ok, flags, clips, params}}}, with the OFX\n"
     "names (OfxParamTypeDouble...). context None: all the contexts (as hostDemo); else only this one\n"
     "(filter, general, ...). (Plugin.descriptor(None) is the describe action's descriptor instead.)");
  m.def("param_template", [](const PluginView &v, const std::string &context, const Point &projectSize,
                             const Point &projectOffset) {
    requireInit();
    return paramTemplate(v.get(), ofxName("context", context), projectSize.first, projectSize.second,
                         projectOffset.first, projectOffset.second);
  }, "plugin"_a, "context"_a, "project_size"_a = Point(768., 576.), "project_offset"_a = Point(0., 0.),
     "The defaults of the parameters that have a value, {name: value}, as JSON-serialisable data (lists,\n"
     "not tuples; bytes as lists of ints), the values a new instance starts with in this project (the\n"
     "normalised spatial defaults are converted with it): a template to edit, and pass back to\n"
     "set_param(). The default project is ofxh's (and hostDemo's).");
  m.def("fetch_suite", [](const std::string &name, int version) {
    requireInit();
    return reinterpret_cast<uintptr_t>(state().host->fetchSuite(name.c_str(), version));
  }, "name"_a, "version"_a,
     "The address of a suite of the host (0 if it has none), e.g. for ctypes calls in tests.");
  m.def("canonical_to_pixel", [](const RectD &r, const Point &renderScale, double par) {
    return tuple(canonicalToPixelEnclosing(toRectD(r), toPoint(renderScale), par));
  }, "rect"_a, "render_scale"_a = Point(1., 1.), "par"_a = 1.,
     "The smallest pixel rectangle that encloses a canonical one (as hostDemo).");
  m.def("pixel_to_canonical", [](const RectI &r, const Point &renderScale, double par) {
    return nb::make_tuple(r[0] * par / renderScale.first, r[1] / renderScale.second, r[2] * par / renderScale.first,
                          r[3] / renderScale.second);
  }, "rect"_a, "render_scale"_a = Point(1., 1.), "par"_a = 1., "A pixel rectangle in canonical coordinates.");
  m.def("ofx_name", [](const std::string &kind, const std::string &name) { return ofxName(kind.c_str(), name); },
        "kind"_a, "name"_a,
        "The OFX string of a short name: kind is depth, components, premult, field, context, message,\n"
        "change, param_type or plane (e.g. ofx_name(\"depth\", \"byte\") is \"OfxBitDepthByte\").");
  m.def("short_name", [](const std::string &kind, const std::string &value) { return shortName(kind.c_str(), value); },
        "kind"_a, "value"_a, "The short name of an OFX string (unknown strings are unchanged).");
}

void bindModule(nb::module_ &m)
{
  m.doc() = kModuleDoc;

  m.attr("__builtins__") = nb::handle(PyEval_GetBuiltins());
  nb::exec(nb::str(kPrelude), m.attr("__dict__"));
  Globals &g = globals();
  g.statFn = m.attr("_stat");
  g.ofxError = m.attr("OfxError");
  g.raiseCallbackError = m.attr("_raise_callback_error");

  bindBuildConfig(m);
  bindPropertySet(m);
  bindDescriptors(m);  // ParamDescriptor, EffectDescriptor
  bindParams(m);
  bindClipDescriptor(m);
  bindPlugin(m);
  bindImageData(m);
  bindHost(m);
  bindImageEffectInstance(m);
  bindClipInstance(m);
  bindFunctions(m);

  // the plug-ins are unloaded at exit, while the interpreter is alive
  nb::module_::import_("atexit").attr("register")(m.attr("shutdown"));
}

}  // namespace ofxh

NB_MODULE(ofxh, m) { ofxh::bindModule(m); }
