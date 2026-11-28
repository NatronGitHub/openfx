"""Regressions of the host descriptor (#213): the host's supported depths, components and contexts were empty
(openfx-misc#222), and setting a variable-size host property to fewer values left the others."""

import pytest

ofxh = pytest.importorskip("ofxh")
from ofxhost import constants as k  # noqa: E402
from ofxhost.testing import _hostdemo_host_strings  # noqa: E402


@pytest.mark.parametrize("profile", ["default", "ofx14"])
def test_host_supported_depths_components_contexts(plugin_dir, profile):
    """The host lists the depths, components and contexts it supports (openfx-misc#222: HostSupport
    left them empty; fixed in #213), in both profiles (they are OFX 1.4 host properties): the depths of PyImage
    (byte, short, half and float), hostDemo's components and contexts, and hostDemo's depths plus
    half. (The in-process backend's host, which has no half, is checked in HostSupport/examples/python/tests:
    test_backends.py::test_inprocess_host_lists_hostdemos_depths.)"""
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False, profile=profile)
    props = ofxh.host().props
    depths = props[k.kOfxImageEffectPropSupportedPixelDepths]
    assert depths == (k.kOfxBitDepthByte, k.kOfxBitDepthShort, k.kOfxBitDepthHalf, k.kOfxBitDepthFloat)
    assert _hostdemo_host_strings("kOfxImageEffectPropSupportedPixelDepths") == tuple(
        d for d in depths if d != k.kOfxBitDepthHalf)
    comps = props[k.kOfxImageEffectPropSupportedComponents]
    assert k.kOfxImageComponentRGBA in comps and k.kOfxImageComponentAlpha in comps
    assert comps == _hostdemo_host_strings("kOfxImageEffectPropSupportedComponents")
    contexts = props[k.kOfxImageEffectPropSupportedContexts]
    assert k.kOfxImageEffectContextFilter in contexts and k.kOfxImageEffectContextGeneral in contexts
    assert contexts == _hostdemo_host_strings("kOfxImageEffectPropSupportedContexts")
    assert [ofxh.short_name("depth", d) for d in depths] == ["byte", "short", "half", "float"]


def test_set_variable_size_host_property(plugin_dir):
    """props[name] = values gives a variable-size property exactly these values (fewer drop the
    others), e.g. a host whose images have no half (ofxhost.inprocess). Before #213, fewer values than the property
    had left the others."""

    class Host(ofxh.Host):
        def init_properties(self, props):
            props[k.kOfxImageEffectPropSupportedPixelDepths] = (k.kOfxBitDepthByte, k.kOfxBitDepthFloat)

    ofxh.init(Host(), [plugin_dir], use_env_path=False, profile="ofx14")
    assert ofxh.host().props[k.kOfxImageEffectPropSupportedPixelDepths] == (k.kOfxBitDepthByte, k.kOfxBitDepthFloat)
