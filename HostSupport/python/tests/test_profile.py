"""The OFX 1.4 host profile (ofxh.init(profile="ofx14"), as hostDemo --ofx14), the one host per
process, PropertySet.__delitem__, ImageEffectInstance.param_props() and default_output_depth."""

import json
import os
import re
from pathlib import Path

import pytest

ofxh = pytest.importorskip("ofxh")
import ofxhost  # noqa: E402
from ofxhost import constants as k  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]

CHOICES = "org.openeffects.support.choiceParamsPlugin"
BASIC = "net.sf.openfx.basicPlugin"
NOISE = "net.sf.openfx.noisePlugin"
HOST_POINTER = "sf.openfx.net.OfxHostSupportHostPointer"  # kOfxHostSupportHostPointer (ofxhHost.h)


def _hostdemo_ofx14_lists():
    """kOFX14HostProps and kOFX14Suites of hostDemo (hostDemoHostDescriptor.cpp), resolved with
    ofxhost.constants: ofxh's OFX 1.4 profile must be the same."""
    src = (REPO_ROOT / "HostSupport" / "examples" / "hostDemoHostDescriptor.cpp").read_text()
    props_src = re.search(r"kOFX14HostProps\[\] = \{(.*?)\};", src, re.S).group(1)
    props = set()
    for name in re.findall(r"\b(k\w+)\b", re.sub(r"//[^\n]*", "", props_src)):
        props.add(HOST_POINTER if name == "kOfxHostSupportHostPointer" else getattr(k, name))
    suites_src = re.search(r"kOFX14Suites\[\] = \{(.*?)\};", src, re.S).group(1)
    suites = {(getattr(k, n), int(v)) for n, v in re.findall(r"\{\s*(k\w+),\s*(\d+)\s*\}", suites_src)}
    assert len(props) > 25 and len(suites) > 10
    return props, suites


def _suites():
    """Every suite name of include/ (the k...Suite constants), with versions 1 to 3."""
    names = {v for n, v in vars(k).items() if n.startswith("kOfx") and n.endswith("Suite") and isinstance(v, str)}
    return [(n, v) for n in sorted(names) for v in (1, 2, 3)]


def _host_state(plugin_dir, profile):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, profile=profile)
    try:
        assert ofxh.profile() == profile
        names = set(ofxh.host().props.names())
        suites = {s: ofxh.fetch_suite(*s) != 0 for s in _suites()}
        return names, suites
    finally:
        ofxh.shutdown()


def test_ofx14_profile_is_hostdemos(plugin_dir):
    props14, suites14 = _hostdemo_ofx14_lists()
    names, suites = _host_state(plugin_dir, "default")
    names14, s14 = _host_state(plugin_dir, "ofx14")
    assert ofxh.profile() is None  # (not initialized)
    # only the OFX 1.4 host properties remain (and those the default host has)
    assert names14 == names & props14
    assert k.kOfxParamHostPropSupportsStrChoice in names and k.kOfxParamHostPropSupportsStrChoice not in names14
    assert k.kOfxImageEffectPropSupportedContexts in names14
    # only the OFX 1.4 suites
    for s, ok in s14.items():
        assert ok == (suites[s] and s in suites14), s
    # (the DrawSuite, for one, is there with the extensions that build it, and hidden in OFX 1.4)
    assert not s14[(k.kOfxDrawSuite, 1)]
    assert s14[(k.kOfxParameterSuite, 1)] and s14[(k.kOfxMessageSuite, 2)]


def test_ofx14_describe_equals_hostdemo(plugin_dir, hostdemo):
    """The ChoiceParams sample doesn't define its StrChoice parameter in an OFX 1.4 host (paramDefine
    fails), as in hostDemo --ofx14."""
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, profile="ofx14")
    d = ofxh.describe(ofxh.get_plugin(CHOICES))
    params = [p["name"] for p in d["contexts"]["OfxImageEffectContextFilter"]["params"]]
    assert "blue_choice" not in params and "red_choice" in params

    def normalized(d):
        d = json.loads(json.dumps(d))
        d["plugin"]["filePath"] = os.path.realpath(d["plugin"]["filePath"])
        return d

    assert normalized(d) == normalized(ofxhost.describe([plugin_dir], CHOICES, host_binary=hostdemo,
                                                        extra_args=["--ofx14"], backend="subprocess"))
    ofxh.shutdown()
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    d = ofxh.describe(ofxh.get_plugin(CHOICES))
    assert "blue_choice" in [p["name"] for p in d["contexts"]["OfxImageEffectContextFilter"]["params"]]


def test_one_host_per_process(plugin_dir):
    with pytest.raises(ValueError, match="profile"):
        ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, profile="ofx15")
    assert not ofxh.initialized()
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with pytest.raises(RuntimeError, match="one host per process"):
        ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, profile="ofx14")
    assert ofxh.profile() == "default"
    ofxh.shutdown()
    # another host, another profile
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, profile="ofx14")
    assert ofxh.profile() == "ofx14" and ofxh.get_plugin(BASIC) is not None


def test_delete_host_property(plugin_dir):
    class Host(ofxh.Host):
        def init_properties(self, props):
            # (as an OFX 1.4 host: the ChoiceParams sample then has no StrChoice parameter)
            del props[k.kOfxParamHostPropSupportsStrChoice]
            with pytest.raises(KeyError):
                del props["NoSuchProperty"]
            with pytest.raises(ValueError, match="needs"):
                del props[HOST_POINTER]

    ofxh.init(Host(), [plugin_dir], use_env_path=False)
    props = ofxh.host().props
    assert k.kOfxParamHostPropSupportsStrChoice not in props and k.kOfxImageEffectPropSupportsTiles in props
    d = ofxh.describe(ofxh.get_plugin(CHOICES))["contexts"]["OfxImageEffectContextFilter"]
    assert "blue_choice" not in [p["name"] for p in d["params"]]
    inst = ofxh.create_instance(ofxh.get_plugin(BASIC), "filter", ofxh.ImageEffectInstance())
    with inst:
        with pytest.raises(TypeError, match="only the host"):
            del inst.props[k.kOfxImageEffectPropContext]
        with pytest.raises(TypeError, match="only the host"):
            del inst.clip("Source").props[k.kOfxImageClipPropOptional]


def test_param_props(plugin_dir):
    """param_props() has the instance's properties (changed in instanceChanged), param_descriptor()
    the described ones."""
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    inst = ofxh.create_instance(ofxh.get_plugin(BASIC), "filter", ofxh.ImageEffectInstance())
    with inst:
        assert inst.create_instance_action() in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
        assert inst.param_props("scaleR")[k.kOfxParamPropEnabled] == (1,)
        inst.set_param("scaleComponents", False)
        inst.begin_instance_changed_action()
        inst.param_changed_action("scaleComponents", time=0.)
        inst.end_instance_changed_action()
        props = inst.param_props("scaleR")
        assert props[k.kOfxParamPropEnabled] == (0,) and props[k.kOfxParamPropType] == (k.kOfxParamTypeDouble,)
        assert inst.param_descriptor("scaleR").enabled
        with pytest.raises(KeyError):
            inst.param_props("nope")


@pytest.mark.parametrize("depth, expected", [(None, "float"), ("byte", "byte"), ("OfxBitDepthShort", "short")])
def test_default_output_depth(plugin_dir, depth, expected):
    """Without a connected input, the output gets default_output_depth (None: HostSupport's float),
    as hostDemo's -d depth."""

    class Clip(ofxh.ClipInstance):
        connected = False

    class Instance(ofxh.ImageEffectInstance):
        default_output_depth = depth

        def new_clip_instance(self, desc, index):
            return ofxh.ClipInstance() if desc.is_output else Clip()

    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    inst = ofxh.create_instance(ofxh.get_plugin(NOISE), "general", Instance())
    with inst:
        assert inst.create_instance_action() in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
        assert inst.get_clip_preferences_action() == ofxh.Stat.OK
        assert inst.clip("Output").pixel_depth == expected
