// Python bindings of HostSupport: what the plug-ins describe, read-only.
//
// - ofxh.ParamDescriptor and ofxh.EffectDescriptor: copies of a parameter descriptor and of the
//   plug-in's descriptor in a context (with its clips and parameters). new_param() gets a
//   ParamDescriptor; Plugin.descriptor(context) returns an EffectDescriptor.
// - ofxh.describe(plugin, context=None): hostDemo's --describe JSON (hostDemo.cpp, describePlugin()),
//   as Python objects, which ofxhost reads too.
// - ofxh.param_template(plugin, context): the defaults of the parameters that have a value, as
//   JSON-serialisable data that set_param() takes back.

#include "ofxhPython.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <map>

#include "ofxGPURender.h"  // kOfxImageEffectPropCudaRenderSupported...
#include "ofxParametricParam.h"  // kOfxParamTypeParametric

using namespace nanobind::literals;

namespace ofxh {

namespace P = OFX::Host::Param;
namespace Prop = OFX::Host::Property;

// ---------------------------------------------------------------------------------------------
// Properties

static nb::object propValue(const Prop::Set &s, Prop::Property *p, const std::string &name, int i)
{
  switch (p->getType()) {
    case Prop::eInt: return nb::int_(s.getIntProperty(name, i));
    case Prop::eDouble: return nb::float_(s.getDoubleProperty(name, i));
    case Prop::eString: {
      const std::string &v = s.getStringProperty(name, i);
      return nb::str(v.c_str(), v.size());
    }
    case Prop::ePointer: return nb::int_((uintptr_t)s.getPointerProperty(name, i));
    default: return nb::none();
  }
}

nb::dict propsDict(const Prop::Set &props)
{
  nb::dict d;
  const Prop::PropertyMap &pm = props.getProperties();
  for (Prop::PropertyMap::const_iterator it = pm.begin(); it != pm.end(); ++it) {
    nb::list l;
    const int n = it->second->getDimension();
    for (int i = 0; i < n; ++i) {
      l.append(propValue(props, it->second, it->first, i));
    }
    d[it->first.c_str()] = nb::tuple(l);
  }
  return d;
}

/// hostDemo's propJson(): a scalar if the property has one value and !forceArray, else a list;
/// None if it doesn't exist (or is a pointer)
static nb::object propJson(const Prop::Set &props, const char *name, bool forceArray = false)
{
  Prop::Property *p = props.fetchProperty(name);
  if (!p || p->getType() == Prop::ePointer) {
    return nb::none();
  }
  const int n = p->getDimension();
  nb::list a;
  for (int i = 0; i < n; ++i) {
    a.append(propValue(props, p, name, i));
  }
  if (n == 1 && !forceArray) {
    return a[0];
  }
  return std::move(a);
}

/// hostDemo's propBoolJson(): an int property as a boolean (None if it doesn't exist)
static nb::object propBoolJson(const Prop::Set &props, const char *name)
{
  Prop::Property *p = props.fetchProperty(name);
  if (!p || p->getType() != Prop::eInt || p->getDimension() < 1) {
    return nb::none();
  }
  return nb::bool_(props.getIntProperty(name) != 0);
}

nb::object defaultValue(const Prop::Set &props, const std::string &type)
{
  Prop::Property *p = props.fetchProperty(kOfxParamPropDefault);
  if (!p || type == kOfxParamTypeParametric || type == kOfxParamTypeGroup || type == kOfxParamTypePage ||
      type == kOfxParamTypePushButton) {
    return nb::none();
  }
  if (type == kOfxParamTypeBytes) {
    // a pointer to an OfxBytes, or NULL
    const OfxBytes *b = p->getType() == Prop::ePointer
                          ? static_cast<const OfxBytes *>(props.getPointerProperty(kOfxParamPropDefault))
                          : NULL;
    return nb::bytes(b && b->data ? (const char *)b->data : "", b && b->data ? b->length : 0);
  }
  if (type == kOfxParamTypeStrChoice) {
    // as StrChoiceInstance::getDefaultEnum(): an empty default is the first option
    std::string v = props.getStringProperty(kOfxParamPropDefault);
    if (v.empty() && props.fetchProperty(kOfxParamPropChoiceEnum) && props.getDimension(kOfxParamPropChoiceEnum) > 0) {
      v = props.getStringProperty(kOfxParamPropChoiceEnum, 0);
    }
    return nb::str(v.c_str(), v.size());
  }
  const int n = p->getDimension();
  nb::list l;
  for (int i = 0; i < n; ++i) {
    if (type == kOfxParamTypeBoolean) {
      l.append(nb::bool_(props.getIntProperty(kOfxParamPropDefault, i) != 0));
    } else {
      l.append(propValue(props, p, kOfxParamPropDefault, i));
    }
  }
  if (n == 1) {
    return l[0];
  }
  return nb::tuple(l);
}

std::string pageOf(const OHIE::Descriptor &effect, const std::string &name)
{
  const std::list<P::Descriptor *> &params = effect.getParamList();
  for (std::list<P::Descriptor *>::const_iterator p = params.begin(); p != params.end(); ++p) {
    if ((*p)->getType() != kOfxParamTypePage) {
      continue;
    }
    const Prop::Set &props = (*p)->getProperties();
    const int n = props.getDimension(kOfxParamPropPageChild);
    for (int i = 0; i < n; ++i) {
      if (props.getStringProperty(kOfxParamPropPageChild, i) == name) {
        return (*p)->getName();
      }
    }
  }
  return std::string();
}

// ---------------------------------------------------------------------------------------------
// ofxh.ParamDescriptor, ofxh.EffectDescriptor (copies)

struct ParamDescriptorInfo {
  std::string name, type, label, hint, scriptName, parent, page, doubleType, defaultCoordinateSystem;
  int dimension;
  bool secret, enabled, animates, evaluateOnChange, persistent;
  nb::object defaultValue, min, max, displayMin, displayMax, choices, choiceEnums, props;
};

nb::object makeParamDescriptor(const P::Descriptor &d, const OHIE::Descriptor *effect)
{
  const Prop::Set &p = d.getProperties();
  ParamDescriptorInfo i;
  i.name = d.getName();
  i.type = shortName("param_type", d.getType());
  i.label = d.getLabel();
  i.hint = d.getHint();
  i.scriptName = d.getScriptName();
  i.parent = d.getParentName();
  i.page = effect ? pageOf(*effect, d.getName()) : std::string();
  i.doubleType = p.fetchProperty(kOfxParamPropDoubleType) ? d.getDoubleType() : std::string();
  i.defaultCoordinateSystem = p.fetchProperty(kOfxParamPropDefaultCoordinateSystem) ? d.getDefaultCoordinateSystem() : std::string();
  Prop::Property *def = p.fetchProperty(kOfxParamPropDefault);
  i.dimension = def ? def->getDimension() : 0;
  i.secret = d.getSecret();
  i.enabled = d.getEnabled();
  i.animates = p.fetchProperty(kOfxParamPropAnimates) ? d.getCanAnimate() : false;
  i.evaluateOnChange = p.fetchProperty(kOfxParamPropEvaluateOnChange) ? d.getEvaluateOnChange() : false;
  i.persistent = p.fetchProperty(kOfxParamPropPersistant) ? d.getIsPersistent() : false;
  i.defaultValue = defaultValue(p, d.getType());
  i.min = propJson(p, kOfxParamPropMin);
  i.max = propJson(p, kOfxParamPropMax);
  i.displayMin = propJson(p, kOfxParamPropDisplayMin);
  i.displayMax = propJson(p, kOfxParamPropDisplayMax);
  i.choices = p.fetchProperty(kOfxParamPropChoiceOption) ? nb::object(nb::tuple(propJson(p, kOfxParamPropChoiceOption, true)))
                                                          : nb::none();
  i.choiceEnums = p.fetchProperty(kOfxParamPropChoiceEnum) ? nb::object(nb::tuple(propJson(p, kOfxParamPropChoiceEnum, true)))
                                                            : nb::none();
  i.props = propsDict(p);
  return nb::cast(std::move(i));
}

struct EffectDescriptorInfo {
  std::string context, label, grouping;
  nb::object props, clips, params;
};

nb::object makeEffectDescriptor(OHIE::ImageEffectPlugin *plugin, const std::string &context)
{
  OHIE::Descriptor *d = NULL;
  if (context.empty()) {
    d = &plugin->getDescriptor();
  } else {
    if (plugin->getContexts().count(context) == 0) {
      raiseError(PyExc_ValueError, "plug-in " + plugin->getIdentifier() + " has no " + shortName("context", context) +
                                   " context (see Plugin.contexts)");
    }
    {
      ActionScope scope;
      d = withoutGil([&] { return plugin->getContext(context); });  // (the describe in context action)
      scope.check("describeInContext");
    }
    if (!d) {
      raiseError(PyExc_RuntimeError, "plug-in " + plugin->getIdentifier() + ": the describe in context action failed "
                                     "for the " + shortName("context", context) + " context");
    }
  }
  EffectDescriptorInfo i;
  i.context = shortName("context", context);
  i.label = d->getLabel();
  i.grouping = d->getPluginGrouping();
  i.props = propsDict(d->getProps());
  nb::list clips;
  const std::vector<OHIE::ClipDescriptor *> &cl = d->getClipsByOrder();
  for (size_t k = 0; k < cl.size(); ++k) {
    clips.append(makeClipDescriptor(*cl[k]));
  }
  i.clips = std::move(clips);
  nb::list params;
  const std::list<P::Descriptor *> &pl = d->getParamList();
  for (std::list<P::Descriptor *>::const_iterator p = pl.begin(); p != pl.end(); ++p) {
    params.append(makeParamDescriptor(**p, d));
  }
  i.params = std::move(params);
  return nb::cast(std::move(i));
}

// ---------------------------------------------------------------------------------------------
// describe(): hostDemo's --describe JSON (hostDemo.cpp: pluginJson, effectFlagsJson, clipJson,
// paramJson, describePlugin)

static nb::dict pluginJson(OHIE::ImageEffectPlugin *plugin)
{
  nb::dict j;
  j["id"] = plugin->getIdentifier();
  j["versionMajor"] = plugin->getVersionMajor();
  j["versionMinor"] = plugin->getVersionMinor();
  const OHIE::Descriptor &d = plugin->getDescriptor();
  j["label"] = d.getLabel();
  j["grouping"] = d.getPluginGrouping();
  j["filePath"] = propJson(d.getProps(), kOfxPluginPropFilePath);
  return j;
}

static nb::dict effectFlagsJson(const Prop::Set &p)
{
  nb::dict j;
  j["supportedPixelDepths"] = propJson(p, kOfxImageEffectPropSupportedPixelDepths, true);
  j["supportsTiles"] = propBoolJson(p, kOfxImageEffectPropSupportsTiles);
  j["supportsMultiResolution"] = propBoolJson(p, kOfxImageEffectPropSupportsMultiResolution);
  j["temporalClipAccess"] = propBoolJson(p, kOfxImageEffectPropTemporalClipAccess);
  j["supportsMultipleClipDepths"] = propBoolJson(p, kOfxImageEffectPropSupportsMultipleClipDepths);
  j["supportsMultipleClipPARs"] = propBoolJson(p, kOfxImageEffectPropSupportsMultipleClipPARs);
  j["fieldRenderTwiceAlways"] = propBoolJson(p, kOfxImageEffectPluginPropFieldRenderTwiceAlways);
  j["singleInstance"] = propBoolJson(p, kOfxImageEffectPluginPropSingleInstance);
  j["hostFrameThreading"] = propBoolJson(p, kOfxImageEffectPluginPropHostFrameThreading);
  j["renderThreadSafety"] = propJson(p, kOfxImageEffectPluginRenderThreadSafety);
  j["sequentialRender"] = propJson(p, kOfxImageEffectInstancePropSequentialRender);
  j["clipPreferencesSlaveParams"] = propJson(p, kOfxImageEffectPropClipPreferencesSlaveParam, true);
  j["openGLRenderSupported"] = propJson(p, kOfxImageEffectPropOpenGLRenderSupported);
  j["openCLRenderSupported"] = propJson(p, kOfxImageEffectPropOpenCLRenderSupported);
  j["cudaRenderSupported"] = propJson(p, kOfxImageEffectPropCudaRenderSupported);
  j["cudaStreamSupported"] = propJson(p, kOfxImageEffectPropCudaStreamSupported);
  j["metalRenderSupported"] = propJson(p, kOfxImageEffectPropMetalRenderSupported);
  j["cpuRenderSupported"] = propJson(p, kOfxImageEffectPropCPURenderSupported);
  return j;
}

static nb::dict clipJson(const OHIE::ClipDescriptor &c)
{
  const Prop::Set &p = c.getProps();
  nb::dict j;
  j["name"] = c.getName();
  j["label"] = c.getLabel();
  j["hint"] = propJson(p, kOfxParamPropHint);
  j["optional"] = c.isOptional();
  j["isMask"] = c.isMask();
  j["supportedComponents"] = propJson(p, kOfxImageEffectPropSupportedComponents, true);
  j["temporalClipAccess"] = c.temporalAccess();
  j["supportsTiles"] = c.supportsTiles();
  j["fieldExtraction"] = c.getFieldExtraction();
  j["secret"] = propBoolJson(p, kOfxParamPropSecret);
  return j;
}

static nb::dict paramJson(const P::Descriptor &d, const std::string &page)
{
  const Prop::Set &p = d.getProperties();
  nb::dict j;
  j["name"] = d.getName();
  j["type"] = d.getType();
  j["label"] = d.getLabel();
  j["hint"] = d.getHint();
  j["scriptName"] = d.getScriptName();
  j["secret"] = d.getSecret();
  j["enabled"] = d.getEnabled();
  j["parent"] = d.getParentName();
  j["page"] = page;
  Prop::Property *def = p.fetchProperty(kOfxParamPropDefault);
  j["dimension"] = def ? def->getDimension() : 0;
  j["default"] = propJson(p, kOfxParamPropDefault);
  j["min"] = propJson(p, kOfxParamPropMin);
  j["max"] = propJson(p, kOfxParamPropMax);
  j["displayMin"] = propJson(p, kOfxParamPropDisplayMin);
  j["displayMax"] = propJson(p, kOfxParamPropDisplayMax);
  j["doubleType"] = propJson(p, kOfxParamPropDoubleType);
  j["choiceOptions"] = propJson(p, kOfxParamPropChoiceOption, true);
  j["animates"] = propBoolJson(p, kOfxParamPropAnimates);
  j["evaluateOnChange"] = propBoolJson(p, kOfxParamPropEvaluateOnChange);
  j["children"] = propJson(p, kOfxParamPropPageChild, true);
  return j;
}

nb::dict describePlugin(OHIE::ImageEffectPlugin *plugin, const std::string &context)
{
  bool ok = true;
  nb::dict out;
  out["schema"] = "hostDemo.describe";
  out["schemaVersion"] = 1;
  out["plugin"] = pluginJson(plugin);
  nb::dict contexts;
  const std::set<std::string> &cs = plugin->getContexts();
  if (!context.empty() && cs.count(context) == 0) {
    raiseError(PyExc_ValueError, "plug-in " + plugin->getIdentifier() + " has no " + shortName("context", context) +
                                 " context (see Plugin.contexts)");
  }
  for (std::set<std::string>::const_iterator it = cs.begin(); it != cs.end(); ++it) {
    if (!context.empty() && *it != context) {
      continue;
    }
    nb::dict c;
    OHIE::Descriptor *d = NULL;
    {
      ActionScope scope;
      const std::string ctx = *it;
      d = withoutGil([&] { return plugin->getContext(ctx); });
      scope.check("describeInContext");
    }
    contexts[it->c_str()] = c;
    if (!d) {
      c["ok"] = false;
      ok = false;
      continue;
    }
    c["ok"] = true;
    c["flags"] = effectFlagsJson(d->getProps());
    nb::list clips;
    const std::vector<OHIE::ClipDescriptor *> &cl = d->getClipsByOrder();
    for (size_t k = 0; k < cl.size(); ++k) {
      clips.append(clipJson(*cl[k]));
    }
    c["clips"] = clips;
    // the page of each parameter (the first page that lists it, as hostDemo)
    const std::list<P::Descriptor *> &params = d->getParamList();
    std::map<std::string, std::string> page;
    for (std::list<P::Descriptor *>::const_iterator p = params.begin(); p != params.end(); ++p) {
      if ((*p)->getType() == kOfxParamTypePage) {
        const Prop::Set &props = (*p)->getProperties();
        const int n = props.getDimension(kOfxParamPropPageChild);
        for (int i = 0; i < n; ++i) {
          page.insert(std::make_pair(props.getStringProperty(kOfxParamPropPageChild, i), (*p)->getName()));
        }
      }
    }
    nb::list ps;
    for (std::list<P::Descriptor *>::const_iterator p = params.begin(); p != params.end(); ++p) {
      std::map<std::string, std::string>::const_iterator pg = page.find((*p)->getName());
      ps.append(paramJson(**p, pg == page.end() ? std::string() : pg->second));
    }
    c["params"] = ps;
  }
  out["contexts"] = contexts;
  out["ok"] = ok;
  return out;
}

nb::object jsonValue(nb::handle v)
{
  if (nb::isinstance<nb::bytes>(v) || PyByteArray_Check(v.ptr())) {
    nb::list l;
    for (nb::handle b : v) {
      l.append(b);
    }
    return std::move(l);
  }
  if (nb::isinstance<nb::tuple>(v) || nb::isinstance<nb::list>(v)) {
    nb::list l;
    for (nb::handle x : v) {
      l.append(jsonValue(x));
    }
    return std::move(l);
  }
  return nb::borrow(v);
}

/// the defaults of the parameters that have a value (not groups, pages, push buttons or
/// parametric parameters), with the normalised spatial defaults in canonical coordinates for this
/// project, as the parameters of a new instance start
nb::dict paramTemplate(OHIE::ImageEffectPlugin *plugin, const std::string &context, double w, double h,
                              double ox, double oy)
{
  if (plugin->getContexts().count(context) == 0) {
    raiseError(PyExc_ValueError, "plug-in " + plugin->getIdentifier() + " has no " + shortName("context", context) +
                                 " context (see Plugin.contexts)");
  }
  OHIE::Descriptor *d = NULL;
  {
    ActionScope scope;
    d = withoutGil([&] { return plugin->getContext(context); });
    scope.check("describeInContext");
  }
  if (!d) {
    raiseError(PyExc_RuntimeError, "plug-in " + plugin->getIdentifier() + ": the describe in context action failed");
  }
  nb::dict r;
  const std::list<P::Descriptor *> &pl = d->getParamList();
  for (std::list<P::Descriptor *>::const_iterator it = pl.begin(); it != pl.end(); ++it) {
    const P::Descriptor &p = **it;
    nb::object v = defaultValue(p.getProperties(), p.getType());
    if (v.is_none()) {
      continue;
    }
    const std::string &t = p.getType();
    if (t == kOfxParamTypeDouble || t == kOfxParamTypeDouble2D) {
      const int n = t == kOfxParamTypeDouble ? 1 : 2;
      double dv[2];
      for (int k = 0; k < n; ++k) {
        dv[k] = p.getProperties().getDoubleProperty(kOfxParamPropDefault, k);
      }
      defaultToCanonical(p, w, h, ox, oy, dv, n);
      v = n == 1 ? nb::object(nb::float_(dv[0])) : nb::object(nb::make_tuple(dv[0], dv[1]));
    }
    r[p.getName().c_str()] = jsonValue(v);
  }
  return r;
}

// ---------------------------------------------------------------------------------------------

void bindDescriptors(nb::module_ &m)
{
  nb::class_<ParamDescriptorInfo>(m, "ParamDescriptor",
    "What the plug-in described for a parameter (a copy): what new_param() gets, and\n"
    "EffectDescriptor.params. type is a short name (integer, double, choice...); default is in the\n"
    "form of get_param() (None for groups, pages, push buttons); min, max, display_min and\n"
    "display_max are a number, a list (several dimensions) or None; choices the options of a\n"
    "choice (None otherwise); page the page parameter that lists it (\"\" if none); props all the\n"
    "properties, {name: tuple of values}.")
    .def_ro("name", &ParamDescriptorInfo::name)
    .def_ro("type", &ParamDescriptorInfo::type)
    .def_ro("label", &ParamDescriptorInfo::label)
    .def_ro("hint", &ParamDescriptorInfo::hint)
    .def_ro("script_name", &ParamDescriptorInfo::scriptName)
    .def_ro("parent", &ParamDescriptorInfo::parent)
    .def_ro("page", &ParamDescriptorInfo::page)
    .def_ro("double_type", &ParamDescriptorInfo::doubleType)
    .def_ro("default_coordinate_system", &ParamDescriptorInfo::defaultCoordinateSystem)
    .def_ro("dimension", &ParamDescriptorInfo::dimension)
    .def_ro("secret", &ParamDescriptorInfo::secret)
    .def_ro("enabled", &ParamDescriptorInfo::enabled)
    .def_ro("animates", &ParamDescriptorInfo::animates)
    .def_ro("evaluate_on_change", &ParamDescriptorInfo::evaluateOnChange)
    .def_ro("persistent", &ParamDescriptorInfo::persistent)
    .def_ro("default", &ParamDescriptorInfo::defaultValue)
    .def_ro("min", &ParamDescriptorInfo::min)
    .def_ro("max", &ParamDescriptorInfo::max)
    .def_ro("display_min", &ParamDescriptorInfo::displayMin)
    .def_ro("display_max", &ParamDescriptorInfo::displayMax)
    .def_ro("choices", &ParamDescriptorInfo::choices)
    .def_ro("choice_enums", &ParamDescriptorInfo::choiceEnums, "The enums of a string choice (None otherwise).")
    .def_ro("props", &ParamDescriptorInfo::props)
    .def("__repr__", [](const ParamDescriptorInfo &i) { return "<ofxh.ParamDescriptor " + i.name + " (" + i.type + ")>"; });

  nb::class_<EffectDescriptorInfo>(m, "EffectDescriptor",
    "What the plug-in described in a context (a copy; Plugin.descriptor(context)): props, {name:\n"
    "tuple of values}; clips, a list of ClipDescriptor; params, a list of ParamDescriptor, in the\n"
    "order of definition.")
    .def_ro("context", &EffectDescriptorInfo::context)
    .def_ro("label", &EffectDescriptorInfo::label)
    .def_ro("grouping", &EffectDescriptorInfo::grouping)
    .def_ro("props", &EffectDescriptorInfo::props)
    .def_ro("clips", &EffectDescriptorInfo::clips)
    .def_ro("params", &EffectDescriptorInfo::params)
    .def("param", [](const EffectDescriptorInfo &i, const std::string &name) -> nb::object {
      for (nb::handle p : i.params) {
        if (nb::cast<const ParamDescriptorInfo &>(p).name == name) {
          return nb::borrow(p);
        }
      }
      raiseError(PyExc_KeyError, name);
    }, "name"_a)
    .def("__repr__", [](const EffectDescriptorInfo &i) { return "<ofxh.EffectDescriptor " + i.label + " (" + i.context + ")>"; });
}

}  // namespace ofxh

