"""What the plug-ins describe: ofxh.describe() gives hostDemo's --describe JSON (compared with
hostDemo's, through ofxhost.describe()), Plugin.descriptor() the read-only descriptor views, and
ofxh.param_template() the defaults of the parameters; also the short names of the OFX strings."""

import json
import os

import pytest

ofxh = pytest.importorskip("ofxh")
import ofxhost  # noqa: E402

BASIC = "net.sf.openfx.basicPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"  # Choice params and a StrChoice param
INVERT = "net.sf.openfx.invertPlugin"
PLUGINS = [BASIC, TESTER, CHOICES, INVERT, "uk.co.thefoundry.GeneratorExample"]


def _init(plugin_dir):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)


def _normalized(d):
    """A describe JSON through json (lists, not tuples), with a real file path."""
    d = json.loads(json.dumps(d))
    d["plugin"]["filePath"] = os.path.realpath(d["plugin"]["filePath"])
    return d


@pytest.mark.parametrize("plugin_id", PLUGINS)
def test_describe_matches_hostdemo(plugin_dir, hostdemo, plugin_id):
    """ofxh.describe() gives exactly hostDemo's --describe JSON (ofxhost.describe() on the subprocess backend) for five
    plug-ins: the C++ and the Python hosts describe the same plug-ins the same way, key for key (the strictest
    describe parity check; the ofxhost backends' own comparison is HostSupport/examples/python/tests). (#197)"""
    expected = _normalized(ofxhost.describe(plugin_dir, plugin_id, host_binary=hostdemo))
    _init(plugin_dir)
    got = _normalized(ofxh.describe(ofxh.get_plugin(plugin_id)))
    assert got == expected


def test_describe_one_context(plugin_dir):
    """describe(plugin, context) describes that context only; a context the plug-in doesn't support is a ValueError.
    (#197)"""
    _init(plugin_dir)
    plugin = ofxh.get_plugin(BASIC)
    d = ofxh.describe(plugin, "general")
    assert list(d["contexts"]) == ["OfxImageEffectContextGeneral"] and d["ok"]
    with pytest.raises(ValueError, match="context"):
        ofxh.describe(ofxh.get_plugin("uk.co.thefoundry.GeneratorExample"), "transition")


def test_descriptor_views(plugin_dir):
    """Plugin.descriptor(context) and descriptor() (the describe action's, no context): read-only views of the clips,
    the parameters (type, default, min, max, parent, animates, dimension, props, secret, choices, the default
    coordinate system, StrChoice enums) and the properties, in describe()'s order; an unknown parameter is a
    KeyError. (#197)"""
    _init(plugin_dir)
    plugin = ofxh.get_plugin(BASIC)
    d = plugin.descriptor("filter")
    assert d.context == "filter" and d.label == "Gain"
    assert [c.name for c in d.clips] == ["Source", "Output"]
    names = [p["name"] for p in ofxh.describe(plugin, "filter")["contexts"]["OfxImageEffectContextFilter"]["params"]]
    assert [p.name for p in d.params] == names
    scale_r = d.param("scaleR")
    assert (scale_r.type, scale_r.default, scale_r.min, scale_r.max) == ("double", 1., 0., 10.)
    assert scale_r.parent == "componentScales" and scale_r.animates and scale_r.dimension == 1
    assert scale_r.props["OfxParamPropDefault"] == (1.,)
    assert d.param("componentScales").type == "group" and d.param("componentScales").default is None
    assert "OfxImageEffectPropSupportedPixelDepths" in d.props
    with pytest.raises(KeyError):
        d.param("noSuchParam")
    # the describe action's descriptor: no context
    assert plugin.descriptor().label == "Gain"
    tester = ofxh.get_plugin(TESTER).descriptor("filter")
    assert tester.param("secretTest").secret and not tester.param("Insignificant").secret
    assert len(tester.param("enableTest").choices) == 2 and tester.param("MinMaxTest").choices is None
    assert tester.param("widgetPos").default_coordinate_system == "OfxParamCoordinatesNormalised"
    strchoice = [p for p in ofxh.get_plugin(CHOICES).descriptor("filter").params if p.type == "strchoice"]
    assert strchoice and strchoice[0].choice_enums and strchoice[0].default in strchoice[0].choice_enums


@pytest.mark.parametrize("plugin_id", [BASIC, TESTER, CHOICES])
def test_param_template(plugin_dir, plugin_id):
    """The template is what a new instance starts with; JSON-serialisable; set_param() takes it. (#197)"""
    _init(plugin_dir)
    plugin = ofxh.get_plugin(plugin_id)
    template = ofxh.param_template(plugin, "filter")
    assert json.loads(json.dumps(template)) == template
    with ofxh.create_instance(plugin, "filter", ofxh.ImageEffectInstance()) as inst:
        assert template == inst.param_values()
        for name, value in template.items():
            assert inst.set_param(name, value) == ofxh.Stat.OK
        assert inst.param_values() == template


def test_param_template_project(plugin_dir):
    """The normalised spatial defaults (Tester's widgetPos, at the middle) depend on the project. (#197)"""
    _init(plugin_dir)
    plugin = ofxh.get_plugin(TESTER)
    assert ofxh.param_template(plugin, "filter")["widgetPos"] == [384., 288.]
    assert ofxh.param_template(plugin, "filter", project_size=(1920, 1080))["widgetPos"] == [960., 540.]


def test_short_names(plugin_dir):
    """The short names that ofxh uses for the OFX strings (contexts, parameter types, components, depths, the
    premultiplication) on plug-ins, instances and clips, and ofx_name()/short_name() between the two (an OFX name
    stays as it is). (#194)"""
    _init(plugin_dir)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", ofxh.ImageEffectInstance()) as inst:
        plugin = inst.plugin
        assert plugin.contexts == ["filter"] and inst.context == "filter"
        assert inst.param_type("supportsTiles") == "boolean"
        clip = inst.clip("Source")
        assert clip.supported_components == ["rgba", "alpha"]
        assert inst.create_instance_action() == ofxh.Stat.OK
        assert inst.get_clip_preferences_action() == ofxh.Stat.OK
        assert (clip.pixel_depth, clip.components, clip.dtype) == ("byte", "rgba", "uint8")
        assert inst.output_premultiplication in ("premult", "unpremult", "opaque")
    assert ofxh.ofx_name("depth", "byte") == "OfxBitDepthByte"
    assert ofxh.short_name("context", "OfxImageEffectContextGeneral") == "general"
    assert ofxh.ofx_name("depth", "OfxBitDepthFloat") == "OfxBitDepthFloat"

