"""What the plug-ins describe: ofxh.describe() gives hostDemo's --describe JSON (compared with
hostDemo's, through ofxhost.describe()), Plugin.descriptor() the read-only descriptor views, and
ofxh.param_template() the defaults of the parameters."""

import json
import os

import pytest

ofxh = pytest.importorskip("ofxh")
import ofxhost  # noqa: E402

BASIC = "net.sf.openfx.basicPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"  # Choice params and a StrChoice param
PLUGINS = [BASIC, TESTER, CHOICES, "net.sf.openfx.invertPlugin", "uk.co.thefoundry.GeneratorExample"]


def _init(plugin_dir):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)


def _normalized(d):
    """A describe JSON through json (lists, not tuples), with a real file path."""
    d = json.loads(json.dumps(d))
    d["plugin"]["filePath"] = os.path.realpath(d["plugin"]["filePath"])
    return d


@pytest.mark.parametrize("plugin_id", PLUGINS)
def test_describe_matches_hostdemo(plugin_dir, hostdemo, plugin_id):
    expected = _normalized(ofxhost.describe(plugin_dir, plugin_id, host_binary=hostdemo))
    _init(plugin_dir)
    got = _normalized(ofxh.describe(ofxh.get_plugin(plugin_id)))
    assert got == expected


def test_describe_one_context(plugin_dir):
    _init(plugin_dir)
    plugin = ofxh.get_plugin(BASIC)
    d = ofxh.describe(plugin, "general")
    assert list(d["contexts"]) == ["OfxImageEffectContextGeneral"] and d["ok"]
    with pytest.raises(ValueError, match="context"):
        ofxh.describe(ofxh.get_plugin("uk.co.thefoundry.GeneratorExample"), "transition")


def test_descriptor_views(plugin_dir):
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
    """The template is what a new instance starts with; JSON-serialisable; set_param() takes it."""
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
    """The normalised spatial defaults (Tester's widgetPos, at the middle) depend on the project."""
    _init(plugin_dir)
    plugin = ofxh.get_plugin(TESTER)
    assert ofxh.param_template(plugin, "filter")["widgetPos"] == [384., 288.]
    assert ofxh.param_template(plugin, "filter", project_size=(1920, 1080))["widgetPos"] == [960., 540.]
