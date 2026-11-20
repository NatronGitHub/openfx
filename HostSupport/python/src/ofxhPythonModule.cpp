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
#include <cstdlib>
#include <list>
#include <map>
#include <optional>
#include <utility>

#include "ofxMessage.h"
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
  "Subclass ofxh.Host, ofxh.ImageEffectInstance and ofxh.ClipInstance, override their callbacks,\n"
  "then ofxh.init(host, plugin_dirs), ofxh.create_instance(plugin, context, instance), and call the\n"
  "instance's *_action() methods. Names are short (depth \"byte\", components \"rgba\", context\n"
  "\"filter\"...; ofx_name() and short_name() convert them). See\n"
  "HostSupport/examples/python/minimal_host.py.\n\n"
  "Part 1 of NatronGitHub/openfx#136. Planned for part 3: the OFX 1.4 host profile (hostDemo's\n"
  "--ofx14), the JSON report of hostDemo, and lifting the one-host-per-process limit where\n"
  "HostSupport allows it.";

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
  PyHost *host = nullptr;
  OHIE::PluginCache *pluginCache = nullptr;
  unsigned generation = 0;  ///< incremented by shutdown(): Plugin views become invalid
};

static State &state()
{
  static State s;
  return s;
}

static void requireInit()
{
  if (!state().initialized) {
    raiseError(PyExc_RuntimeError, "ofxh is not initialized: call ofxh.init(host, plugin_dirs) first");
  }
}

static void requireNoAction(const char *what)
{
  if (ActionScope::depth() > 0) {
    raiseError(PyExc_RuntimeError, std::string(what) + " can't be called in an action or a host callback");
  }
}

[[noreturn]] static void raiseKey(const std::string &name) { raiseError(PyExc_KeyError, name); }

// ---------------------------------------------------------------------------------------------
// Views

/// ofxh.PropertySet: the properties of the host, an instance or a clip
struct PropertySetView {
  RefPtr ref;
  int kind;  ///< 0: host, 1: instance, 2: clip
  OH::Property::Set &get() const
  {
    if (!ref->p) {
      raiseDeleted("ofxh.PropertySet");
    }
    switch (kind) {
      case 0: return static_cast<PyHost *>(ref->p)->getProperties();
      case 1: return static_cast<PyEffectInstance *>(ref->p)->getProps();
      default: return static_cast<PyClipInstance *>(ref->p)->getProps();
    }
  }
};

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
      case OH::Property::ePointer: return nb::int_((uintptr_t)s.getPointerProperty(name, index));
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
    for (int i = 0; i < (int)v.size(); ++i) {
      switch (p->getType()) {
        case OH::Property::eInt: s.setIntProperty(name, nb::cast<int>(v[i]), i); break;
        case OH::Property::eDouble: s.setDoubleProperty(name, nb::cast<double>(v[i]), i); break;
        case OH::Property::eString: s.setStringProperty(name, nb::cast<std::string>(v[i]), i); break;
        case OH::Property::ePointer: s.setPointerProperty(name, (void *)nb::cast<uintptr_t>(v[i]), i); break;
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
    if (!state().initialized || generation != state().generation) {
      raiseDeleted("ofxh.Plugin");
    }
    return p;
  }
};

// ---------------------------------------------------------------------------------------------
// Conversions

typedef std::pair<double, double> Point;
typedef std::array<double, 4> RectD;
typedef std::array<int, 4> RectI;

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
static void checkView(int view, bool render, int nViews = 1)
{
  bool views = false, severalViews = false;
#ifdef OFX_EXTENSIONS_NUKE
  views = true;
#endif
#ifdef OFX_EXTENSIONS_VEGAS
  views = views || render;
  severalViews = true;
#endif
  (void)render;
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
};

/// the render state, for the duration of a render action
struct RenderScope : RenderScaleScope {
  RenderScope(PyEffectInstance *inst, const OfxPointD &scale, const OfxRectI &window) : RenderScaleScope(inst, scale)
  {
    p->_rendering = true;
    p->_renderWindow = window;
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
      open[i]->destroyInstanceAction();
      delete open[i];
    }
  }
  // the plug-ins get kOfxActionUnload and the binaries are unloaded: this needs the image effect
  // plug-in cache (the API handler) and the host, which are deleted after (as in hostDemo)
  OH::PluginCache::clearPluginCache();
  delete st.pluginCache;
  st.pluginCache = nullptr;
  OHIE::gImageEffectHost = nullptr;
  PyHost *host = st.host;
  st.host = nullptr;
  st.initialized = false;
  ++st.generation;
  delete host;
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
  const bool restore = e != NULL;
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

static void init(nb::object host, nb::object dirs, bool useStdPaths, bool useEnvPath)
{
  State &st = state();
  if (st.initialized) {
    raiseError(PyExc_RuntimeError, "ofxh is already initialized: call ofxh.shutdown() first");
  }
  requireNoAction("ofxh.init()");
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
  // (before the first getPluginCache())
  OH::PluginCache::useStdOFXPluginsLocation(useStdPaths);
  OH::PluginCache *pc = makePluginCache(useEnvPath);
  pc->setCacheVersion("ofxhPythonV1");
  for (size_t i = 0; i < pluginDirs.size(); ++i) {
    pc->addFileToPath(pluginDirs[i]);
  }
  st.host = new PyHost(host, hb.ref);
  st.initialized = true;
  try {
    {
      ActionScope scope;
      host.attr("init_properties")(PropertySetView{hb.ref, 0});
      scope.check("init_properties");
    }
    if (!st.initialized || st.host == nullptr) {
      raiseError(PyExc_RuntimeError, "ofxh.init(): the host was shut down during init_properties()");
    }
    st.pluginCache = new OHIE::PluginCache(st.host);
    st.pluginCache->registerInCache(*pc);
    ActionScope scope;
    pc->scanPluginFiles();
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
  // the instance: raw is NULL.
  OHIE::Instance *raw = plugin->createInstance(ofxName("context", context), instance.ptr());
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

void bindModule(nb::module_ &m)
{
  m.doc() = kModuleDoc;

  m.attr("__builtins__") = nb::handle(PyEval_GetBuiltins());
  nb::exec(nb::str(kPrelude), m.attr("__dict__"));
  Globals &g = globals();
  g.statFn = m.attr("_stat");
  g.ofxError = m.attr("OfxError");
  g.raiseCallbackError = m.attr("_raise_callback_error");

  // build configuration: the OFX_* definitions the module (and HostSupport) were compiled with
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
#undef OFXH_DEF
    m.attr("build_config") = d;
  }

  // ------------------------------------------------------------------------- PropertySet
  nb::class_<PropertySetView>(m, "PropertySet",
                              "The properties of the host, an instance or a clip (HostSupport's Property::Set, with\n"
                              "the OFX names and values). props[name] is the tuple of values; props.get(name, index)\n"
                              "one value.")
    .def("__getitem__", [](const PropertySetView &v, const std::string &name) {
      return OFXH_SCOPED("PropertySet[]", propValues(v.get(), name));
    })
    .def("get", [](const PropertySetView &v, const std::string &name, int index) {
      return OFXH_SCOPED("PropertySet.get()", propValue(v.get(), name, index));
    }, "name"_a, "index"_a = 0)
    .def("__setitem__", [](const PropertySetView &v, const std::string &name, nb::handle value) {
      OFXH_SCOPED("PropertySet[]=", setProp(v.get(), name, value));
    })
    .def("__contains__", [](const PropertySetView &v, const std::string &name) { return v.get().fetchProperty(name, true) != NULL; })
    .def("names", [](const PropertySetView &v) {
      std::vector<std::string> r;
      const OH::Property::PropertyMap &pm = v.get().getProperties();
      for (OH::Property::PropertyMap::const_iterator it = pm.begin(); it != pm.end(); ++it) {
        r.push_back(it->first);
      }
      return r;
    }, "The names of the properties (not those of a chained set).");

  // ------------------------------------------------------------------------- ClipDescriptor
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

  // ------------------------------------------------------------------------- Plugin
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
    .def("__repr__", [](const PluginView &v) {
      return "<ofxh.Plugin " + v.get()->getIdentifier() + " " + std::to_string(v.get()->getVersionMajor()) + "." +
             std::to_string(v.get()->getVersionMinor()) + ">";
    });

  // ------------------------------------------------------------------------- ImageData
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
                        "lower or upper. unique_id: \"\" for clip:time:view (as hostDemo).")
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

  // ------------------------------------------------------------------------- Host
  nb::class_<HostBase>(m, "Host",
                       "The host. Subclass it and override the callbacks; pass one to ofxh.init().\n\n"
                       "Callbacks: message(type, id, text) (type: fatal, error, warning, message, log or\n"
                       "question), persistent_message(type, id, text), clear_persistent_message(),\n"
                       "init_properties(props). The defaults print the messages as hostDemo does, and answer\n"
                       "yes to questions.",
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
    .def_prop_ro("props", [](HostBase &h) { h.get(); return PropertySetView{h.ref, 0}; },
                 "The host properties (after ofxh.init()).")
    .def_prop_ro("alive", [](HostBase &h) { return h.ref->p != nullptr; });

  // ------------------------------------------------------------------------- ImageEffectInstance
  nb::class_<InstanceBase> inst(m, "ImageEffectInstance",
    "An image effect instance. Subclass it, override the callbacks, and create it with\n"
    "ofxh.create_instance(plugin, context, MyInstance()). close() it (or use `with`).\n\n"
    "Callbacks (the defaults are hostDemo's): the methods new_clip_instance(desc, index),\n"
    "message(type, id, text), persistent_message(...), clear_persistent_message(), view_name(index),\n"
    "progress_start(message, id), progress_end(), progress_update(t), timeline_get_time(),\n"
    "timeline_goto_time(t), timeline_get_bounds(), edit_begin(name), edit_end(); and the values\n"
    "project_size, project_offset, project_extent, project_pixel_aspect_ratio, effect_duration,\n"
    "frame_rate, frame_recursive, render_scale_recursive, default_output_fielding, view_count, which\n"
    "may be overridden with a property, a class attribute or an instance attribute\n"
    "(self.frame_rate = 30).\n\n"
    "Actions: the *_action() methods, named after HostSupport's. They return an ofxh.Stat, or\n"
    "(Stat, result).",
    nb::dynamic_attr());
  inst.def(nb::init<>())
    // --- callbacks (defaults)
    .def("new_clip_instance", [](InstanceBase &, nb::object, int) { return nb::type<ClipBase>()(); }, "desc"_a, "index"_a,
         "Make the clip instance (an ofxh.ClipInstance) of a clip descriptor (an ofxh.ClipDescriptor).")
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
    .def("timeline_get_time", [](InstanceBase &s) { return s.get()->_timeLineTime; })
    .def("timeline_goto_time", [](InstanceBase &s, double t) { s.get()->_timeLineTime = t; }, "t"_a)
    .def("timeline_get_bounds", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      return OFXH_SCOPED("timeline_get_bounds()", Point(0., p->getEffectDuration()));
    })
    .def("edit_begin", [](InstanceBase &, const std::string &) { return nb::none(); }, "name"_a)
    .def("edit_end", [](InstanceBase &) { return nb::none(); });
  overridable(inst, "project_size", [](InstanceBase &) { return nb::cast(Point(768., 576.)); });
  overridable(inst, "project_offset", [](InstanceBase &) { return nb::cast(Point(0., 0.)); });
  overridable(inst, "project_extent", [](InstanceBase &) { return nb::cast(Point(768., 576.)); });
  overridable(inst, "project_pixel_aspect_ratio", [](InstanceBase &) { return nb::cast(768. / 720.); });
  overridable(inst, "effect_duration", [](InstanceBase &) { return nb::cast(25.); });
  overridable(inst, "frame_rate", [](InstanceBase &) { return nb::cast(25.); });
  overridable(inst, "frame_recursive", [](InstanceBase &s) { return nb::cast(s.get()->_timeLineTime); });
  overridable(inst, "render_scale_recursive", [](InstanceBase &s) {
    return nb::cast(Point(s.get()->_renderScale.x, s.get()->_renderScale.y));
  });
  overridable(inst, "default_output_fielding", [](InstanceBase &) { return nb::cast(std::string("none")); });
  overridable(inst, "view_count", [](InstanceBase &) { return nb::cast(1); });

  // --- information
  inst.def_prop_ro("alive", [](InstanceBase &s) { return s.ref->p != nullptr; })
    .def_prop_ro("plugin", [](InstanceBase &s) { return PluginView{s.get()->getPlugin(), state().generation}; })
    .def_prop_ro("context", [](InstanceBase &s) { return shortName("context", s.get()->getContext()); })
    .def_prop_ro("props", [](InstanceBase &s) { s.get(); return PropertySetView{s.ref, 1}; })
    .def_prop_ro("handle", [](InstanceBase &s) { return (uintptr_t)s.get()->getHandle(); },
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
      return getParamValue(p, time ? &*time : NULL);
    }, "name"_a, "time"_a.none() = nb::none(),
       "The value of a parameter (a tuple for several dimensions). Part 1 has no keys: the value is the\n"
       "same at all times.")
    .def("set_param", [](InstanceBase &s, const std::string &name, nb::handle value) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        raiseKey(name);
      }
      return stat(setParamValue(p, value));
    }, "name"_a, "value"_a.none(),
       "Set the value of a parameter (no instance changed action: call param_changed_action()). Part 1\n"
       "has no keys.")
    .def_prop_ro("rendering", [](InstanceBase &s) { return s.get()->_rendering; })
    .def_prop_ro("render_window", [](InstanceBase &s) { return tuple(s.get()->_renderWindow); },
                 "The render window of the current (or last) render action, in pixels.")
    .def_prop_ro("render_scale", [](InstanceBase &s) { return Point(s.get()->_renderScale.x, s.get()->_renderScale.y); },
                 "The render scale of the current action ((1, 1) outside the actions).")
    .def_prop_rw("aborted", [](InstanceBase &s) { return s.get()->_abort.load() != 0; },
                 [](InstanceBase &s, bool a) { s.get()->_abort.store(a ? 1 : 0); },
                 "What the plug-in's abort() gets (an atomic flag).")
    .def_prop_ro("supports_tiles", [](InstanceBase &s) { return s.get()->supportsTiles(); })
    .def_prop_ro("output_premultiplication", [](InstanceBase &s) {
      return shortName("premult", s.get()->getOutputPreMultiplication());
    }, "Set by the clip preferences.")
    .def_prop_ro("output_fielding", [](InstanceBase &s) { return shortName("field", s.get()->getOutputFielding()); })
    .def_prop_ro("output_frame_rate", [](InstanceBase &s) { return s.get()->getOutputFrameRate(); })
    .def_prop_ro("output_continuous_samples", [](InstanceBase &s) { return s.get()->continuousSamples(); })
    .def_prop_ro("output_frame_varying", [](InstanceBase &s) { return s.get()->isFrameVarying(); })
    .def_prop_ro("clip_preferences_dirty", [](InstanceBase &s) { return s.get()->areClipPrefsDirty(); })
    .def("project_pixel_rect", [](InstanceBase &s, const Point &rs) {
      PyEffectInstance *p = s.get();
      return OFXH_SCOPED("project_pixel_rect()", tuple(p->projectPixelRect(toPoint(rs))));
    }, "render_scale"_a = Point(1., 1.), "The project, in pixels at the render scale.");

  // --- actions: each returns a Stat, or (Stat, result)
  inst.def("create_instance_action", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->createInstanceAction();
      scope.check("createInstance", &st);
      return stat(st);
    }, "kOfxActionCreateInstance (set the parameter values of a saved project before).")
    .def("get_clip_preferences_action", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->getClipPreferences() ? kOfxStatOK : kOfxStatFailed;
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
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->getRegionOfDefinitionAction(time, toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                            view,
#endif
                                            rod);
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
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->getRegionOfInterestAction(time, toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                          view,
#endif
                                          toRectD(roi), rois);
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
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->isIdentityAction(t, fieldName(field), toRectI(window), toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                 v, pl,
#endif
                                 clip);
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
      const OfxStatus st = p->getFrameNeededAction(time, ranges);
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
                                            int view) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      ActionScope scope;
      OfxStatus st;
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->beginRenderAction(first, last, step, interactive, toPoint(renderScale), sequential, interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                  false,
#ifdef OFX_EXTENSIONS_NATRON
                                  NULL,
#endif
#endif
                                  draft
#ifdef OFX_EXTENSIONS_NUKE
                                  , view
#endif
                                  );
      }
      scope.check("beginSequenceRender", &st);
      return stat(st);
    }, "first"_a, "last"_a, "step"_a = 1., "interactive"_a = false, "render_scale"_a = Point(1., 1.),
       "sequential"_a = true, "interactive_render"_a = false, "draft"_a = false, "view"_a = 0)
    .def("render_action", [](InstanceBase &s, double time, const RectI &window, const Point &renderScale,
                             const std::string &field, bool sequential, bool interactiveRender, bool draft, int view,
                             int nViews, const std::vector<std::string> &planes) {
      PyEffectInstance *p = s.get();
      checkView(view, true, nViews);
      if (planes.size() != 1) {
        raiseError(PyExc_ValueError, "planes: part 1 renders one plane per render action");
      }
      if (shortName("plane", ofxName("plane", planes[0])) != "Colour") {
        raiseError(PyExc_ValueError, "plane " + planes[0] + ": part 1 renders the colour plane only");
      }
      // each render gets new output images
      const std::map<std::string, OHIE::ClipInstance *> &clips = p->clips();
      for (std::map<std::string, OHIE::ClipInstance *>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        if (PyClipInstance *c = pyClip(it->second)) {
          c->releaseOutputImage();
          c->_outputData.reset();
        }
      }
      // the output image can be made (the clip preferences set its depth and components): else the
      // output fallback of PyClipInstance::fetchImage() couldn't make one either
      {
        PyClipInstance *out = pyClip(p->getClip(kOfxImageEffectOutputClipName));
        if (!out) {
          raiseError(PyExc_ValueError, "render_action(): the plug-in has no output clip");
        }
        const std::string depth = OFXH_SCOPED("render_action()", out->getPixelDepth());
        const std::string comps = OFXH_SCOPED("render_action()", out->getComponents());
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
      p->_outputFailed = false;
      {
        RenderScope rs(p, toPoint(renderScale), toRectI(window));
        st = p->renderAction(time, fieldName(field), toRectI(window), toPoint(renderScale), sequential, interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                             false,
#ifdef OFX_EXTENSIONS_NATRON
                             NULL,
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
      }
      // the output image of this render (the clip keeps the array, not the C++ image)
      nb::dict out;
      if (PyClipInstance *c = pyClip(p->getClip(kOfxImageEffectOutputClipName))) {
        if (c->_outputData.is_valid()) {
          out["Colour"] = c->_outputData;
        }
        c->releaseOutputImage();
      }
      if (p->_outputFailed) {
        // (the plug-in rendered into an image that ofxh made, see PyClipInstance::fetchImage())
        st = kOfxStatFailed;
        p->_outputFailed = false;
      }
      scope.check("render", &st);
      return nb::make_tuple(stat(st), out);
    }, "time"_a, "render_window"_a, "render_scale"_a = Point(1., 1.), "field"_a = "none", "sequential"_a = true,
       "interactive_render"_a = false, "draft"_a = false, "view"_a = 0, "n_views"_a = 1,
       "planes"_a = std::vector<std::string>(1, "Colour"),
       "kOfxImageEffectActionRender. render_window: pixels at the render scale. Returns (Stat, {plane:\n"
       "the output ImageData the plug-in fetched}) (empty if it fetched none). Part 1 renders one plane,\n"
       "the colour plane; view and n_views need the Nuke or Vegas extensions (see build_config). If\n"
       "The output clip must have a depth and components that ofxh can make (ValueError otherwise):\n"
       "call create_instance_action() (which sets the default clip preferences) and\n"
       "get_clip_preferences_action() before: ofxh doesn't run them for you. If\n"
       "get_image() gives no valid output image, ofxh gives the plug-in one over the render window (a\n"
       "plug-in may not check), asks it to abort, and the action returns Stat.Failed and raises\n"
       "CallbackError.")
    .def("end_sequence_render_action", [](InstanceBase &s, double first, double last, double step, bool interactive,
                                          const Point &renderScale, bool sequential, bool interactiveRender, bool draft,
                                          int view) {
      PyEffectInstance *p = s.get();
      checkView(view, false);
      ActionScope scope;
      OfxStatus st;
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->endRenderAction(first, last, step, interactive, toPoint(renderScale), sequential, interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                false,
#ifdef OFX_EXTENSIONS_NATRON
                                NULL,
#endif
#endif
                                draft
#ifdef OFX_EXTENSIONS_NUKE
                                , view
#endif
                                );
      }
      scope.check("endSequenceRender", &st);
      return stat(st);
    }, "first"_a, "last"_a, "step"_a = 1., "interactive"_a = false, "render_scale"_a = Point(1., 1.),
       "sequential"_a = true, "interactive_render"_a = false, "draft"_a = false, "view"_a = 0)
    .def("get_time_domain_action", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      OfxRangeD r;
      r.min = r.max = 0.;
      ActionScope scope;
      const OfxStatus st = p->getTimeDomainAction(r);
      scope.check("getTimeDomain", &st);
      return nb::make_tuple(stat(st), Point(r.min, r.max));
    }, "Returns (Stat, (t1, t2)) (the general context only).")
    .def("begin_instance_changed_action", [](InstanceBase &s, const std::string &why) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->beginInstanceChangedAction(ofxName("change", why));
      scope.check("beginInstanceChanged", &st);
      return stat(st);
    }, "why"_a = "user", "why: user, plugin or time (or the kOfxChange* value).")
    .def("param_changed_action", [](InstanceBase &s, const std::string &name, const std::string &why, double time,
                                    const Point &renderScale) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      OfxStatus st;
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->paramInstanceChangedAction(name, ofxName("change", why), time, toPoint(renderScale));
      }
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "why"_a = "user", "time"_a = 0., "render_scale"_a = Point(1., 1.))
    .def("clip_changed_action", [](InstanceBase &s, const std::string &name, const std::string &why, double time,
                                   const Point &renderScale) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      OfxStatus st;
      {
        RenderScaleScope rs(p, toPoint(renderScale));
        st = p->clipInstanceChangedAction(name, ofxName("change", why), time, toPoint(renderScale));
      }
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "why"_a = "user", "time"_a = 0., "render_scale"_a = Point(1., 1.))
    .def("end_instance_changed_action", [](InstanceBase &s, const std::string &why) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->endInstanceChangedAction(ofxName("change", why));
      scope.check("endInstanceChanged", &st);
      return stat(st);
    }, "why"_a = "user")
    .def("close", [](InstanceBase &s) {
      PyEffectInstance *p = static_cast<PyEffectInstance *>(s.ref->p);
      if (!p) {
        return;  // (already closed: a no-op)
      }
      requireNoAction("ImageEffectInstance.close()");
      ActionScope scope;
      // (the status of kOfxActionDestroyInstance doesn't matter: the instance is deleted anyway)
      p->destroyInstanceAction();
      delete p;  // the clips and params too; the Python objects raise ReferenceError from now on
      scope.check("destroyInstance");
    }, "kOfxActionDestroyInstance, then delete the instance (and its clips). Returns None; a second\n"
       "close() does nothing. Not in an action or a callback.")
    .def("__enter__", [](nb::object self) { return self; })
    .def("__exit__", [](nb::object self, nb::args) {
      self.attr("close")();
      return false;
    });

  // ------------------------------------------------------------------------- ClipInstance
  nb::class_<ClipBase> clip(m, "ClipInstance",
    "A clip of an instance. Subclass it, and return one from ImageEffectInstance.new_clip_instance().\n\n"
    "Callbacks (the defaults are hostDemo's): the methods get_image(time, view, bounds, plane=\"Colour\")\n"
    "(accept the plane keyword: part 1 passes \"Colour\", later parts other planes) and\n"
    "region_of_definition(time, view); and the values connected, unmapped_bit_depth,\n"
    "unmapped_components, premult, format, aspect_ratio, frame_rate, frame_range, field_order,\n"
    "unmapped_frame_rate, unmapped_frame_range, continuous_samples, which may be overridden with a\n"
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
      OfxRectI b = e->_renderWindow;
      if (!e->_rendering) {
        b = OFXH_SCOPED("get_image()", intersect(canonicalToPixelEnclosing(c->rod(time, view), e->_renderScale,
                                                                           c->getAspectRatio()),
                                                 e->projectPixelRect(e->_renderScale)));
      }
      ImageData d;
      d.pixels = OFXH_SCOPED("get_image()", c->newImage(b.x1, b.y1, b.x2, b.y2));
      d.bounds = tuple(b);
      d.rod = nb::none();
      d.field = "none";
      return nb::cast(std::move(d));
    }, "time"_a, "view"_a, "bounds"_a.none(), "plane"_a = "Colour",
       "Return the image (an ofxh.ImageData, or None) at this time and view. bounds: the region the\n"
       "plug-in asked for, in canonical coordinates, or None. plane: \"Colour\" (part 1). The default\n"
       "makes the output image\n"
       "(filled with a sentinel) over the render window, and returns None for the inputs.")
    .def("region_of_definition", [](ClipBase &s, double time, int view) {
      PyClipInstance *c = s.get();
      return OFXH_SCOPED("region_of_definition()", tuple(c->defaultRoD(time, view)));
    }, "time"_a, "view"_a,
       "The RoD in canonical coordinates. The default: the effect's RoD for the output, the project for\n"
       "a connected input, (0, 0, 0, 0) for a disconnected one.");
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
    } else if (!OFXH_SCOPED("premult", c->getConnected()) ||
               OFXH_SCOPED("premult", c->getComponents()) == kOfxImageComponentRGB) {
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
    return nb::object(OFXH_SCOPED("format", tuple(e->projectPixelRect(one))));
  }, "The format in pixels (Natron extension); default: the project.");
  overridable(clip, "aspect_ratio", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(OFXH_SCOPED("aspect_ratio", e->getProjectPixelAspectRatio()));
  });
  overridable(clip, "frame_rate", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(OFXH_SCOPED("frame_rate", e->getFrameRate()));
  });
  overridable(clip, "frame_range", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(OFXH_SCOPED("frame_range", Point(0., e->getEffectDuration())));
  });
  overridable(clip, "field_order", [](ClipBase &) { return nb::cast(std::string("none")); });
  overridable(clip, "unmapped_frame_rate", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(OFXH_SCOPED("unmapped_frame_rate", e->getFrameRate()));
  });
  overridable(clip, "unmapped_frame_range", [](ClipBase &s) {
    PyEffectInstance *e = s.get()->_effect;
    return nb::cast(OFXH_SCOPED("unmapped_frame_range", Point(0., e->getEffectDuration())));
  });
  overridable(clip, "continuous_samples", [](ClipBase &) { return nb::cast(false); });
  // --- information
  clip.def_prop_ro("alive", [](ClipBase &s) { return s.ref->p != nullptr; })
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
      return shortName("depth", OFXH_SCOPED("pixel_depth", c->getPixelDepth()));
    }, "The depth of the images (set by the clip preferences): byte, short, half, float or none.")
    .def_prop_ro("components", [](ClipBase &s) {
      PyClipInstance *c = s.get();
      return shortName("components", OFXH_SCOPED("components", c->getComponents()));
    }, "The components of the images (set by the clip preferences): rgba, rgb, alpha or none.")
    .def_prop_ro("dtype", [](ClipBase &s) -> nb::object {
      PyClipInstance *c = s.get();
      const std::string d = OFXH_SCOPED("dtype", c->getPixelDepth());
      if (d == kOfxBitDepthByte) return nb::str("uint8");
      if (d == kOfxBitDepthShort) return nb::str("uint16");
      if (d == kOfxBitDepthHalf) return nb::str("float16");
      if (d == kOfxBitDepthFloat) return nb::str("float32");
      return nb::none();
    }, "The NumPy dtype name of the images (from pixel_depth), or None.")
    .def_prop_ro("view", [](ClipBase &s) { return s.get()->_view; })
    .def_prop_ro("instance", [](ClipBase &s) { return s.get()->_effect->_py; })
    .def_prop_ro("props", [](ClipBase &s) { s.get(); return PropertySetView{s.ref, 2}; })
    .def("new_image", [](ClipBase &s, const RectI &b) {
      PyClipInstance *c = s.get();
      return OFXH_SCOPED("new_image()", c->newImage(b[0], b[1], b[2], b[3]));
    }, "bounds"_a, "An empty NumPy array for an image of these bounds (pixels), of the clip's depth and components.");

  // ------------------------------------------------------------------------- functions
  m.def("init", &init, "host"_a, "plugin_dirs"_a.none() = nb::none(), "use_std_paths"_a = false,
        "use_env_path"_a = true,
        "Initialize HostSupport with this host (an ofxh.Host), and scan the plug-ins of plugin_dirs, of\n"
        "OFX_PLUGIN_PATH if use_env_path, and of the standard locations if use_std_paths. One host per\n"
        "process: call shutdown() before another init(). If it raises, it shuts down first.");
  m.def("shutdown", &shutdown,
        "Close the open instances, unload the plug-ins and delete the plug-in cache and the host. Also\n"
        "called at exit. Not in an action or a callback.");
  m.def("initialized", []() { return state().initialized; });
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
  m.def("fetch_suite", [](const std::string &name, int version) {
    requireInit();
    return (uintptr_t)state().host->fetchSuite(name.c_str(), version);
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

  // the plug-ins are unloaded at exit, while the interpreter is alive
  nb::module_::import_("atexit").attr("register")(m.attr("shutdown"));
}

}  // namespace ofxh

NB_MODULE(ofxh, m) { ofxh::bindModule(m); }
