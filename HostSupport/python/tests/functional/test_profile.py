"""The OFX 1.4 host profile (ofxh.init(profile="ofx14"), as hostDemo --ofx14), the one host per process,
PropertySet.__delitem__ on the host descriptor, default_output_depth, keep_loaded and a host without normalised
defaults (ofxh.init(normalised_defaults=False)). (The host's supported lists and a generator's output PAR are
regression tests: regression/test_host_descriptor.py, regression/test_output_par.py.)"""

import json
import os
import re

import pytest

ofxh = pytest.importorskip("ofxh")
import ofxhost  # noqa: E402
from ofxh_helpers import REPO_ROOT  # noqa: E402
from ofxhost import constants as k  # noqa: E402

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
    """ofxh's OFX 1.4 profile has exactly hostDemo --ofx14's host properties and suites (kOFX14HostProps and
    kOFX14Suites, parsed from hostDemoHostDescriptor.cpp), among those of the default profile: no StrChoice support
    property, no DrawSuite; the C++ and Python hosts must offer plug-ins the same OFX 1.4 host. (#207)"""
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
    fails), as in hostDemo --ofx14. (#207)"""
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
    """init() refuses an unknown profile (ValueError, no host left), and a second host while one is initialized
    (RuntimeError "one host per process", the first host and its profile stay); after shutdown() another host may
    have another profile. (#207)"""
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
    """del on the host descriptor's properties in init_properties() (as an OFX 1.4 host without StrChoice: ChoiceParams
    then doesn't define blue_choice): an unknown property is a KeyError, the host pointer (which HostSupport needs) a
    ValueError; the instance's and clip's properties refuse del (TypeError: only the host's). (#207)"""
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


def _noise_without_input(plugin_dir, depth):
    """Noise in the general context, its optional Source disconnected, in an instance whose default_output_depth is
    depth."""

    class Clip(ofxh.ClipInstance):
        connected = False

    class Instance(ofxh.ImageEffectInstance):
        default_output_depth = depth

        def new_clip_instance(self, desc, index):
            return ofxh.ClipInstance() if desc.is_output else Clip()

    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    return ofxh.create_instance(ofxh.get_plugin(NOISE), "general", Instance())


@pytest.mark.parametrize("depth, expected", [(None, "float"), ("byte", "byte"), ("OfxBitDepthShort", "short")],
                         ids=["default", "byte", "ofx-name"])
def test_default_output_depth(plugin_dir, depth, expected):
    """Without a connected input, the output gets default_output_depth (None: HostSupport's float; a short name or
    an OFX name), as hostDemo's -d depth. (#207)"""
    with _noise_without_input(plugin_dir, depth) as inst:
        assert inst.create_instance_action() in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
        assert inst.get_clip_preferences_action() == ofxh.Stat.OK
        assert inst.clip("Output").pixel_depth == expected


def test_default_output_depth_invalid(plugin_dir):
    """A default_output_depth that isn't a depth ("double") makes the create instance action fail (before the clip
    preferences): a CallbackError whose cause is a ValueError naming default_output_depth. (#207)"""
    with _noise_without_input(plugin_dir, "double") as inst:
        with pytest.raises(ofxh.CallbackError) as e:
            inst.create_instance_action()
        assert isinstance(e.value.__cause__, ValueError) and "default_output_depth" in str(e.value.__cause__)


def test_keep_loaded(plugin_dir):
    """ofxh.init(keep_loaded=True): Host::keepPluginsLoadedAfterScan() (hostDemo's
    OFX_HOSTDEMO_KEEP_LOADED=1); ofxh.keep_loaded() says which mode the host is in. (The actions it
    sends are checked against hostDemo's in HostSupport/examples/python/tests/functional/test_hostdemo_cli.py.)
    (#212)"""
    assert ofxh.keep_loaded() is None  # (not initialized)
    for keep in (True, False):
        ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, keep_loaded=keep)
        try:
            assert ofxh.keep_loaded() is keep
            assert ofxh.describe(ofxh.get_plugin(BASIC))["ok"]  # (its first use)
        finally:
            ofxh.shutdown()
        assert ofxh.keep_loaded() is None


def test_normalised_defaults(plugin_dir):
    """ofxh.init(normalised_defaults=False), as hostDemo --no-normalised-defaults: the double param
    descriptors have no kOfxParamPropDefaultCoordinateSystem, so the Tester's widgetPos (normalised
    0.5, 0.5) has none. (The values it gives are checked against hostDemo's in
    HostSupport/examples/python/tests/functional/test_hostdemo_cli.py.) (#330)"""
    tester = "net.sf.openfx.GenericTestPlugin"
    for normalised, system in ((True, k.kOfxParamCoordinatesNormalised), (False, "")):
        ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, normalised_defaults=normalised)
        try:
            p = ofxh.get_plugin(tester).descriptor("filter").param("widgetPos")
            assert p.default_coordinate_system == system
            assert (k.kOfxParamPropDefaultCoordinateSystem in p.props) is normalised
        finally:
            ofxh.shutdown()
