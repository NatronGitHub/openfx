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
    after the call. __cause__ is the first exception; status is the action's status (or None)."""
    status = None


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

[[noreturn]] static void raise(PyObject *type, const std::string &message)
{
  PyErr_SetString(type, message.c_str());
  throw nb::python_error();
}

static void requireInit()
{
  if (!state().initialized) {
    raise(PyExc_RuntimeError, "ofxh is not initialized: call ofxh.init(host, plugin_dirs) first");
  }
}

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
    PyErr_SetString(PyExc_KeyError, name.c_str());
    throw nb::python_error();
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
    raise(PyExc_ValueError, "property " + name + ": status " + std::to_string(e.getStatus()));
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
    raise(PyExc_ValueError, "property " + name + ": status " + std::to_string(e.getStatus()));
  }
  nb::list l;
  for (int i = 0; i < n; ++i) {
    l.append(propValue(s, name, i));
  }
  return nb::tuple(l);
}

static void setProp(OH::Property::Set &s, const std::string &name, nb::handle value)
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
    raise(PyExc_ValueError, "property " + name + ": status " + std::to_string(e.getStatus()));
  }
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
  i.fieldExtraction = d.getFieldExtraction();
  i.optional = d.isOptional();
  i.isMask = d.isMask();
  i.isOutput = d.isOutput();
  i.supportsTiles = d.supportsTiles();
  i.temporalAccess = d.temporalAccess();
  i.supportedComponents = d.getSupportedComponents();
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

static std::string contextName(const std::string &s)
{
  if (s.compare(0, 21, "OfxImageEffectContext") == 0 || s.empty()) {
    return s;
  }
  std::string c = s;
  if (c[0] >= 'a' && c[0] <= 'z') {
    c[0] = (char)(c[0] - 'a' + 'A');
  }
  return "OfxImageEffectContext" + c;
}

#ifdef OFX_EXTENSIONS_NUKE
static std::string planeName(const std::string &s)
{
  return (s == "Colour" || s == "Color") ? std::string(kFnOfxImagePlaneColour) : s;
}
static std::string planeShortName(const std::string &s)
{
  return s == kFnOfxImagePlaneColour ? std::string("Colour") : s;
}
#endif

/// the render state, for the duration of a render action
struct RenderScope {
  PyEffectInstance *p;
  RenderScope(PyEffectInstance *inst, const OfxPointD &scale, const OfxRectI &window) : p(inst)
  {
    p->_rendering = true;
    p->_renderScale = scale;
    p->_renderWindow = window;
  }
  ~RenderScope() { p->_rendering = false; }
};

static PyClipInstance *pyClip(OHIE::ClipInstance *c) { return dynamic_cast<PyClipInstance *>(c); }

// the messages, as hostDemo's Host::vmessage prints them
static nb::object defaultMessage(const std::string &type, const std::string &text)
{
  const char *prefix = "Message : ";
  bool question = false;
  if (type == kOfxMessageLog) {
    prefix = "Log : ";
  } else if (type == kOfxMessageFatal || type == kOfxMessageError) {
    prefix = "Error : ";
  } else if (type == kOfxMessageQuestion) {
    prefix = "Question : ";
    question = true;
  }
  nb::print(nb::str((prefix + text).c_str()));
  // (no dialog to ask the question: yes)
  return stat(question ? kOfxStatReplyYes : kOfxStatOK);
}

// ---------------------------------------------------------------------------------------------
// init, shutdown

static void shutdown()
{
  State &st = state();
  if (!st.initialized) {
    return;
  }
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

static void init(nb::object host, nb::object dirs, bool useStdPaths)
{
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
  State &st = state();
  if (st.initialized) {
    raise(PyExc_RuntimeError, "ofxh is already initialized: call ofxh.shutdown() first");
  }
  if (!nb::isinstance<HostBase>(host)) {
    raise(PyExc_TypeError, "ofxh.init(): host must be an ofxh.Host");
  }
  HostBase &hb = nb::cast<HostBase &>(host);
  if (hb.ref->p) {
    raise(PyExc_ValueError, "ofxh.init(): this host is already in use");
  }
  // (before the first getPluginCache())
  OH::PluginCache::useStdOFXPluginsLocation(useStdPaths);
  OH::PluginCache *pc = OH::PluginCache::getPluginCache();
  pc->setCacheVersion("ofxhPythonV1");
  for (size_t i = 0; i < pluginDirs.size(); ++i) {
    pc->addFileToPath(pluginDirs[i]);
  }
  st.host = new PyHost(host, hb.ref);
  st.initialized = true;
  try {
    host.attr("init_properties")(PropertySetView{hb.ref, 0});
  } catch (...) {
    shutdown();
    throw;
  }
  st.pluginCache = new OHIE::PluginCache(st.host);
  st.pluginCache->registerInCache(*pc);
  ActionScope scope;
  pc->scanPluginFiles();
  scope.check("plug-in scan");
}

static nb::object createInstance(const PluginView &pv, const std::string &context, nb::object instance)
{
  requireInit();
  OHIE::ImageEffectPlugin *plugin = pv.get();
  if (!nb::isinstance<InstanceBase>(instance)) {
    raise(PyExc_TypeError, "ofxh.create_instance(): instance must be an ofxh.ImageEffectInstance");
  }
  InstanceBase &ib = nb::cast<InstanceBase &>(instance);
  if (ib.ref->p) {
    raise(PyExc_ValueError, "ofxh.create_instance(): this instance object is already in use");
  }
  ActionScope scope;
  // PyHost::newInstance() gets the Python object as the client data, and binds it
  OHIE::Instance *raw = plugin->createInstance(contextName(context), instance.ptr());
  if (raw && scope.failed()) {
    delete raw;  // (no kOfxActionCreateInstance yet)
    raw = nullptr;
  }
  scope.check("createInstance (making the clips and parameters)");
  if (!raw) {
    return nb::none();
  }
  return instance;
}

// ---------------------------------------------------------------------------------------------

void bindModule(nb::module_ &m)
{
  m.doc() = "Python bindings of the OpenFX HostSupport library: write OFX hosts in Python.";

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
                              "The properties of the host, an instance or a clip (HostSupport's Property::Set).\n"
                              "props[name] is the tuple of values; props.get(name, index) one value.")
    .def("__getitem__", [](const PropertySetView &v, const std::string &name) { return propValues(v.get(), name); })
    .def("get", [](const PropertySetView &v, const std::string &name, int index) { return propValue(v.get(), name, index); },
         "name"_a, "index"_a = 0)
    .def("__setitem__", [](const PropertySetView &v, const std::string &name, nb::handle value) { setProp(v.get(), name, value); })
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
      return std::vector<std::string>(c.begin(), c.end());
    })
    .def_prop_ro("file_path", [](const PluginView &v) { return v.get()->getBinary()->getFilePath(); })
    .def("__repr__", [](const PluginView &v) {
      return "<ofxh.Plugin " + v.get()->getIdentifier() + " " + std::to_string(v.get()->getVersionMajor()) + "." +
             std::to_string(v.get()->getVersionMinor()) + ">";
    });

  // ------------------------------------------------------------------------- ImageData
  nb::class_<ImageData>(m, "ImageData",
                        "An image returned by ClipInstance.get_image().\n\n"
                        "pixels: a NumPy array of shape (y2-y1, x2-x1, C), C = 4 (RGBA), 3 (RGB) or 1 (Alpha), of the\n"
                        "clip's (mapped) depth: uint8, uint16, float16 or float32. Row 0 is y1 (bottom-up, as OFX\n"
                        "images); the pixels of a row are contiguous; the rows may have any stride, negative too.\n"
                        "bounds: (x1, y1, x2, y2) in pixels at the render scale. rod: the clip's RoD in pixels\n"
                        "(None: from region_of_definition()). field: none, both, lower or upper. unique_id: \"\" for\n"
                        "clip:time:view (as hostDemo).")
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
                       "Callbacks: message(type, id, text), persistent_message(type, id, text),\n"
                       "clear_persistent_message(), init_properties(props). The defaults print the messages\n"
                       "as hostDemo does, and answer yes to questions.")
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
    "Callbacks (methods or properties; the defaults are hostDemo's): new_clip_instance(desc, index),\n"
    "message(type, id, text), persistent_message(...), clear_persistent_message(), the properties\n"
    "project_size, project_offset, project_extent, project_pixel_aspect_ratio, effect_duration,\n"
    "frame_rate, frame_recursive, render_scale_recursive, default_output_fielding, view_count,\n"
    "view_name(index), progress_start(message, id), progress_end(), progress_update(t),\n"
    "timeline_get_time(), timeline_goto_time(t), timeline_get_bounds(), edit_begin(name), edit_end().");
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
    .def_prop_ro("project_size", [](InstanceBase &) { return Point(768., 576.); })
    .def_prop_ro("project_offset", [](InstanceBase &) { return Point(0., 0.); })
    .def_prop_ro("project_extent", [](InstanceBase &) { return Point(768., 576.); })
    .def_prop_ro("project_pixel_aspect_ratio", [](InstanceBase &) { return 768. / 720.; })
    .def_prop_ro("effect_duration", [](InstanceBase &) { return 25.; })
    .def_prop_ro("frame_rate", [](InstanceBase &) { return 25.; })
    .def_prop_ro("frame_recursive", [](InstanceBase &s) { return s.get()->_timeLineTime; })
    .def_prop_ro("render_scale_recursive", [](InstanceBase &s) {
      return Point(s.get()->_renderScale.x, s.get()->_renderScale.y);
    })
    .def_prop_ro("default_output_fielding", [](InstanceBase &) { return std::string("none"); })
    .def_prop_ro("view_count", [](InstanceBase &) { return 1; })
    .def("view_name", [](InstanceBase &, int) { return std::string("main"); }, "index"_a)
    .def("progress_start", [](InstanceBase &, const std::string &, const std::string &) {}, "message"_a, "id"_a)
    .def("progress_end", [](InstanceBase &) {})
    .def("progress_update", [](InstanceBase &, double) { return true; }, "t"_a)
    .def("timeline_get_time", [](InstanceBase &s) { return s.get()->_timeLineTime; })
    .def("timeline_goto_time", [](InstanceBase &s, double t) { s.get()->_timeLineTime = t; }, "t"_a)
    .def("timeline_get_bounds", [](InstanceBase &s) { return Point(0., s.get()->getEffectDuration()); })
    .def("edit_begin", [](InstanceBase &, const std::string &) { return nb::none(); }, "name"_a)
    .def("edit_end", [](InstanceBase &) { return nb::none(); })
    // --- information
    .def_prop_ro("alive", [](InstanceBase &s) { return s.ref->p != nullptr; })
    .def_prop_ro("plugin", [](InstanceBase &s) { return PluginView{s.get()->getPlugin(), state().generation}; })
    .def_prop_ro("context", [](InstanceBase &s) { return s.get()->getContext(); })
    .def_prop_ro("props", [](InstanceBase &s) { s.get(); return PropertySetView{s.ref, 1}; })
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
        PyErr_SetString(PyExc_KeyError, name.c_str());
        throw nb::python_error();
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
        PyErr_SetString(PyExc_KeyError, name.c_str());
        throw nb::python_error();
      }
      return p->getType();
    }, "name"_a)
    .def("get_param", [](InstanceBase &s, const std::string &name, std::optional<double> time) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        PyErr_SetString(PyExc_KeyError, name.c_str());
        throw nb::python_error();
      }
      return getParamValue(p, time ? &*time : NULL);
    }, "name"_a, "time"_a.none() = nb::none(), "The value of a parameter (a tuple for several dimensions).")
    .def("set_param", [](InstanceBase &s, const std::string &name, nb::handle value, std::optional<double> time) {
      OH::Param::Instance *p = s.get()->getParam(name);
      if (!p) {
        PyErr_SetString(PyExc_KeyError, name.c_str());
        throw nb::python_error();
      }
      return stat(setParamValue(p, value, time ? &*time : NULL));
    }, "name"_a, "value"_a.none(), "time"_a.none() = nb::none(),
       "Set the value of a parameter (no instance changed action: call param_changed()).")
    .def_prop_ro("rendering", [](InstanceBase &s) { return s.get()->_rendering; })
    .def_prop_ro("render_window", [](InstanceBase &s) { return tuple(s.get()->_renderWindow); },
                 "The render window of the current (or last) render, in pixels.")
    .def_prop_ro("render_scale", [](InstanceBase &s) { return Point(s.get()->_renderScale.x, s.get()->_renderScale.y); })
    .def_prop_rw("aborted", [](InstanceBase &s) { return s.get()->_abort.load() != 0; },
                 [](InstanceBase &s, bool a) { s.get()->_abort.store(a ? 1 : 0); },
                 "What the plug-in's abort() gets (an atomic flag).")
    .def_prop_ro("supports_tiles", [](InstanceBase &s) { return s.get()->supportsTiles(); })
    .def_prop_ro("output_premultiplication", [](InstanceBase &s) { return s.get()->getOutputPreMultiplication(); })
    .def_prop_ro("output_fielding", [](InstanceBase &s) { return s.get()->getOutputFielding(); })
    .def_prop_ro("output_frame_rate", [](InstanceBase &s) { return s.get()->getOutputFrameRate(); })
    .def_prop_ro("continuous_samples", [](InstanceBase &s) { return s.get()->continuousSamples(); })
    .def_prop_ro("frame_varying", [](InstanceBase &s) { return s.get()->isFrameVarying(); })
    .def_prop_ro("clip_preferences_dirty", [](InstanceBase &s) { return s.get()->areClipPrefsDirty(); })
    .def("project_pixel_rect", [](InstanceBase &s, const Point &rs) { return tuple(s.get()->projectPixelRect(toPoint(rs))); },
         "render_scale"_a = Point(1., 1.), "The project, in pixels at the render scale.");

  // --- actions: each returns a Stat, or (Stat, result)
  inst.def("create", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->createInstanceAction();
      scope.check("createInstance", &st);
      return stat(st);
    }, "kOfxActionCreateInstance (set the parameter values of a saved project before).")
    .def("get_clip_preferences", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->getClipPreferences() ? kOfxStatOK : kOfxStatFailed;
      scope.check("getClipPreferences", &st);
      return stat(st);
    }, "kOfxImageEffectActionGetClipPreferences: sets the depth, components and PAR of the clips.")
    .def("get_region_of_definition", [](InstanceBase &s, double time, const Point &renderScale, int view) {
      PyEffectInstance *p = s.get();
      (void)view;
      OfxRectD rod;
      rod.x1 = rod.y1 = rod.x2 = rod.y2 = 0.;
      ActionScope scope;
      const OfxStatus st = p->getRegionOfDefinitionAction(time, toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                                          view,
#endif
                                                          rod);
      scope.check("getRegionOfDefinition", &st);
      return nb::make_tuple(stat(st), tuple(rod));
    }, "time"_a, "render_scale"_a = Point(1., 1.), "view"_a = 0, "Returns (Stat, rod) in canonical coordinates.")
    .def("get_regions_of_interest", [](InstanceBase &s, double time, const RectD &roi, const Point &renderScale, int view) {
      PyEffectInstance *p = s.get();
      (void)view;
      std::map<OHIE::ClipInstance *, OfxRectD> rois;
      ActionScope scope;
      const OfxStatus st = p->getRegionOfInterestAction(time, toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                                        view,
#endif
                                                        toRectD(roi), rois);
      scope.check("getRegionsOfInterest", &st);
      nb::dict d;
      for (std::map<OHIE::ClipInstance *, OfxRectD>::const_iterator it = rois.begin(); it != rois.end(); ++it) {
        d[it->first->getName().c_str()] = tuple(it->second);
      }
      return nb::make_tuple(stat(st), d);
    }, "time"_a, "roi"_a, "render_scale"_a = Point(1., 1.), "view"_a = 0,
       "roi: the region to render, in canonical coordinates. Returns (Stat, {clip: roi}).")
    .def("is_identity", [](InstanceBase &s, double time, const RectI &window, const Point &renderScale,
                           const std::string &field, int view, const std::string &plane) {
      PyEffectInstance *p = s.get();
      OfxTime t = time;
      std::string clip;
      (void)view;
      (void)plane;
#ifdef OFX_EXTENSIONS_NUKE
      int v = view;
      std::string pl = planeName(plane);
#endif
      ActionScope scope;
      const OfxStatus st = p->isIdentityAction(t, fieldName(field), toRectI(window), toPoint(renderScale),
#ifdef OFX_EXTENSIONS_NUKE
                                               v, pl,
#endif
                                               clip);
      scope.check("isIdentity", &st);
      nb::dict d;
      d["clip"] = st == kOfxStatOK ? nb::object(nb::str(clip.c_str())) : nb::none();
      d["time"] = t;
#ifdef OFX_EXTENSIONS_NUKE
      d["view"] = v;
      d["plane"] = planeShortName(pl);
#else
      d["view"] = view;
      d["plane"] = plane;
#endif
      return nb::make_tuple(stat(st), d);
    }, "time"_a, "render_window"_a, "render_scale"_a = Point(1., 1.), "field"_a = "none", "view"_a = 0,
       "plane"_a = "Colour", "Returns (Stat, {clip, time, view, plane}); Stat.OK: an identity.")
    .def("get_frames_needed", [](InstanceBase &s, double time) {
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
    .def("begin_sequence_render", [](InstanceBase &s, double first, double last, double step, bool interactive,
                                     const Point &renderScale, bool sequential, bool interactiveRender, bool draft, int view) {
      PyEffectInstance *p = s.get();
      (void)view;
      ActionScope scope;
      const OfxStatus st = p->beginRenderAction(first, last, step, interactive, toPoint(renderScale), sequential,
                                                interactiveRender,
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
      scope.check("beginSequenceRender", &st);
      return stat(st);
    }, "first"_a, "last"_a, "step"_a = 1., "interactive"_a = false, "render_scale"_a = Point(1., 1.),
       "sequential"_a = true, "interactive_render"_a = false, "draft"_a = false, "view"_a = 0)
    .def("render", [](InstanceBase &s, double time, const RectI &window, const Point &renderScale,
                      const std::string &field, bool sequential, bool interactiveRender, bool draft, int view,
                      int nViews, const std::vector<std::string> &planes) {
      PyEffectInstance *p = s.get();
      (void)view;
      (void)nViews;
      (void)planes;
      // each render gets new output images
      const std::map<std::string, OHIE::ClipInstance *> &clips = p->clips();
      for (std::map<std::string, OHIE::ClipInstance *>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        if (PyClipInstance *c = pyClip(it->second)) {
          c->releaseOutputImage();
          c->_outputData.reset();
        }
      }
#ifdef OFX_EXTENSIONS_NUKE
      std::list<std::string> pl;
      for (size_t i = 0; i < planes.size(); ++i) {
        pl.push_back(planeName(planes[i]));
      }
#endif
      OfxStatus st;
      ActionScope scope;
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
      nb::object out = nb::none();
      if (PyClipInstance *c = pyClip(p->getClip(kOfxImageEffectOutputClipName))) {
        if (c->_outputData.is_valid()) {
          out = c->_outputData;
        }
        c->releaseOutputImage();
      }
      scope.check("render", &st);
      return nb::make_tuple(stat(st), out);
    }, "time"_a, "render_window"_a, "render_scale"_a = Point(1., 1.), "field"_a = "none", "sequential"_a = true,
       "interactive_render"_a = false, "draft"_a = false, "view"_a = 0, "n_views"_a = 1,
       "planes"_a = std::vector<std::string>(1, "Colour"),
       "kOfxImageEffectActionRender. render_window: pixels at the render scale. Returns (Stat, the output\n"
       "ImageData the plug-in fetched, or None).")
    .def("end_sequence_render", [](InstanceBase &s, double first, double last, double step, bool interactive,
                                   const Point &renderScale, bool sequential, bool interactiveRender, bool draft, int view) {
      PyEffectInstance *p = s.get();
      (void)view;
      ActionScope scope;
      const OfxStatus st = p->endRenderAction(first, last, step, interactive, toPoint(renderScale), sequential,
                                              interactiveRender,
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
      scope.check("endSequenceRender", &st);
      return stat(st);
    }, "first"_a, "last"_a, "step"_a = 1., "interactive"_a = false, "render_scale"_a = Point(1., 1.),
       "sequential"_a = true, "interactive_render"_a = false, "draft"_a = false, "view"_a = 0)
    .def("get_time_domain", [](InstanceBase &s) {
      PyEffectInstance *p = s.get();
      OfxRangeD r;
      r.min = r.max = 0.;
      ActionScope scope;
      const OfxStatus st = p->getTimeDomainAction(r);
      scope.check("getTimeDomain", &st);
      return nb::make_tuple(stat(st), Point(r.min, r.max));
    }, "Returns (Stat, (t1, t2)) (the general context only).")
    .def("begin_instance_changed", [](InstanceBase &s, const std::string &why) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->beginInstanceChangedAction(changeReason(why));
      scope.check("beginInstanceChanged", &st);
      return stat(st);
    }, "why"_a = "user", "why: user, plugin or time (or the kOfxChange* value).")
    .def("param_changed", [](InstanceBase &s, const std::string &name, const std::string &why, double time,
                             const Point &renderScale) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->paramInstanceChangedAction(name, changeReason(why), time, toPoint(renderScale));
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "why"_a = "user", "time"_a = 0., "render_scale"_a = Point(1., 1.))
    .def("clip_changed", [](InstanceBase &s, const std::string &name, const std::string &why, double time,
                            const Point &renderScale) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->clipInstanceChangedAction(name, changeReason(why), time, toPoint(renderScale));
      scope.check("instanceChanged", &st);
      return stat(st);
    }, "name"_a, "why"_a = "user", "time"_a = 0., "render_scale"_a = Point(1., 1.))
    .def("end_instance_changed", [](InstanceBase &s, const std::string &why) {
      PyEffectInstance *p = s.get();
      ActionScope scope;
      const OfxStatus st = p->endInstanceChangedAction(changeReason(why));
      scope.check("endInstanceChanged", &st);
      return stat(st);
    }, "why"_a = "user")
    .def("close", [](InstanceBase &s) -> nb::object {
      PyEffectInstance *p = static_cast<PyEffectInstance *>(s.ref->p);
      if (!p) {
        return nb::none();  // (already closed)
      }
      ActionScope scope;
      const OfxStatus st = p->destroyInstanceAction();
      delete p;  // the clips and params too; the Python objects raise ReferenceError from now on
      scope.check("destroyInstance", &st);
      return stat(st);
    }, "kOfxActionDestroyInstance, then delete the instance (and its clips). Idempotent.")
    .def("__enter__", [](nb::object self) { return self; })
    .def("__exit__", [](nb::object self, nb::args) {
      self.attr("close")();
      return false;
    });

  // ------------------------------------------------------------------------- ClipInstance
  nb::class_<ClipBase>(m, "ClipInstance",
    "A clip of an instance. Subclass it, and return one from ImageEffectInstance.new_clip_instance().\n\n"
    "Callbacks (the defaults are hostDemo's): get_image(time, view, bounds), region_of_definition(time,\n"
    "view), and the properties connected, unmapped_bit_depth, unmapped_components, premult, format,\n"
    "aspect_ratio, frame_rate, frame_range, field_order, unmapped_frame_rate, unmapped_frame_range,\n"
    "continuous_samples (override them with a property or a class attribute). Depths, components,\n"
    "premultiplications and fields may be given by their short names (byte, rgba, premult, none...).")
    .def(nb::init<>())
    // --- callbacks (defaults)
    .def("get_image", [](ClipBase &s, double time, int view, nb::object) -> nb::object {
      PyClipInstance *c = s.get();
      if (!c->isOutput()) {
        return nb::none();
      }
      // the output: the render window (outside a render, the RoD clipped to the project)
      PyEffectInstance *e = c->_effect;
      OfxRectI b = e->_renderWindow;
      if (!e->_rendering) {
        b = intersect(canonicalToPixelEnclosing(c->rod(time, view), e->_renderScale, c->getAspectRatio()),
                      e->projectPixelRect(e->_renderScale));
      }
      ImageData d;
      d.pixels = c->newImage(b.x1, b.y1, b.x2, b.y2);
      d.bounds = tuple(b);
      d.rod = nb::none();
      d.field = "none";
      return nb::cast(std::move(d));
    }, "time"_a, "view"_a, "bounds"_a.none(),
       "Return the image (an ofxh.ImageData, or None) at this time and view. bounds: the region the\n"
       "plug-in asked for, in canonical coordinates, or None. The default makes the output image\n"
       "(filled with a sentinel) over the render window, and returns None for the inputs.")
    .def("region_of_definition", [](ClipBase &s, double time, int view) { return tuple(s.get()->defaultRoD(time, view)); },
         "time"_a, "view"_a,
         "The RoD in canonical coordinates. The default: the effect's RoD for the output, the project for\n"
         "a connected input, (0, 0, 0, 0) for a disconnected one.")
    .def_prop_ro("connected", [](ClipBase &) { return true; })
    .def_prop_ro("unmapped_bit_depth", [](ClipBase &) { return std::string(kOfxBitDepthByte); })
    .def_prop_ro("unmapped_components", [](ClipBase &) { return std::string(kOfxImageComponentRGBA); })
    .def_prop_ro("premult", [](ClipBase &s) -> std::string {
      // the output: the clip preferences; an RGB or disconnected input: opaque ("an image with no alpha
      // is opaque"); else unpremultiplied
      PyClipInstance *c = s.get();
      if (c->isOutput()) {
        return c->_effect->getOutputPreMultiplication();
      }
      if (!c->getConnected() || c->getComponents() == kOfxImageComponentRGB) {
        return kOfxImageOpaque;
      }
      return kOfxImageUnPreMultiplied;
    })
    .def_prop_ro("format", [](ClipBase &s) {
      OfxPointD one;
      one.x = one.y = 1.;
      return tuple(s.get()->_effect->projectPixelRect(one));
    }, "The format in pixels (Natron extension); default: the project.")
    .def_prop_ro("aspect_ratio", [](ClipBase &s) { return s.get()->_effect->getProjectPixelAspectRatio(); })
    .def_prop_ro("frame_rate", [](ClipBase &s) { return s.get()->_effect->getFrameRate(); })
    .def_prop_ro("frame_range", [](ClipBase &s) { return Point(0., s.get()->_effect->getEffectDuration()); })
    .def_prop_ro("field_order", [](ClipBase &) { return std::string("none"); })
    .def_prop_ro("unmapped_frame_rate", [](ClipBase &s) { return s.get()->_effect->getFrameRate(); })
    .def_prop_ro("unmapped_frame_range", [](ClipBase &s) { return Point(0., s.get()->_effect->getEffectDuration()); })
    .def_prop_ro("continuous_samples", [](ClipBase &) { return false; })
    // --- information
    .def_prop_ro("alive", [](ClipBase &s) { return s.ref->p != nullptr; })
    .def_prop_ro("name", [](ClipBase &s) { return s.get()->getName(); })
    .def_prop_ro("is_output", [](ClipBase &s) { return s.get()->isOutput(); })
    .def_prop_ro("is_optional", [](ClipBase &s) { return s.get()->isOptional(); })
    .def_prop_ro("is_mask", [](ClipBase &s) { return s.get()->isMask(); })
    .def_prop_ro("supports_tiles", [](ClipBase &s) { return s.get()->supportsTiles(); })
    .def_prop_ro("supported_components", [](ClipBase &s) { return s.get()->getSupportedComponents(); })
    .def_prop_ro("pixel_depth", [](ClipBase &s) { return s.get()->getPixelDepth(); },
                 "The depth of the images (set by the clip preferences).")
    .def_prop_ro("components", [](ClipBase &s) { return s.get()->getComponents(); },
                 "The components of the images (set by the clip preferences).")
    .def_prop_ro("dtype", [](ClipBase &s) -> nb::object {
      const std::string &d = s.get()->getPixelDepth();
      if (d == kOfxBitDepthByte) return nb::str("uint8");
      if (d == kOfxBitDepthShort) return nb::str("uint16");
      if (d == kOfxBitDepthHalf) return nb::str("float16");
      if (d == kOfxBitDepthFloat) return nb::str("float32");
      return nb::none();
    }, "The NumPy dtype name of the images (from pixel_depth), or None.")
    .def_prop_ro("view", [](ClipBase &s) { return s.get()->_view; })
    .def_prop_ro("instance", [](ClipBase &s) { return s.get()->_effect->_py; })
    .def_prop_ro("props", [](ClipBase &s) { s.get(); return PropertySetView{s.ref, 2}; })
    .def("new_image", [](ClipBase &s, const RectI &b) { return s.get()->newImage(b[0], b[1], b[2], b[3]); }, "bounds"_a,
         "An empty NumPy array for an image of these bounds (pixels), of the clip's depth and components.");

  // ------------------------------------------------------------------------- functions
  m.def("init", &init, "host"_a, "plugin_dirs"_a.none() = nb::none(), "use_std_paths"_a = false,
        "Initialize HostSupport with this host (an ofxh.Host), and scan the plug-ins of plugin_dirs (and of\n"
        "OFX_PLUGIN_PATH, and the standard locations if use_std_paths). One host per process: call\n"
        "shutdown() before another init().");
  m.def("shutdown", &shutdown,
        "Close the open instances, unload the plug-ins and delete the plug-in cache and the host. Also\n"
        "called at exit.");
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
        "the clips (instance.new_clip_instance()) and the parameters. Returns instance, or None if it failed.\n"
        "Then call instance.create() (kOfxActionCreateInstance).");
  m.def("canonical_to_pixel", [](const RectD &r, const Point &renderScale, double par) {
    return tuple(canonicalToPixelEnclosing(toRectD(r), toPoint(renderScale), par));
  }, "rect"_a, "render_scale"_a = Point(1., 1.), "par"_a = 1.,
     "The smallest pixel rectangle that encloses a canonical one (as hostDemo).");
  m.def("pixel_to_canonical", [](const RectI &r, const Point &renderScale, double par) {
    return nb::make_tuple(r[0] * par / renderScale.first, r[1] / renderScale.second, r[2] * par / renderScale.first,
                          r[3] / renderScale.second);
  }, "rect"_a, "render_scale"_a = Point(1., 1.), "par"_a = 1., "A pixel rectangle in canonical coordinates.");

  // the plug-ins are unloaded at exit, while the interpreter is alive
  nb::module_::import_("atexit").attr("register")(m.attr("shutdown"));
}

}  // namespace ofxh

NB_MODULE(ofxh, m) { ofxh::bindModule(m); }
