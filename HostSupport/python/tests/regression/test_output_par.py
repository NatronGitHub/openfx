"""Regression (#355): a generator may set its output PAR. Since #351, the output clip's PAR in the clip
preferences was read-only unless the host and the plug-in both support multiple clip PARs, so a generator without a
connected input could no longer set it."""

import pytest

ofxh = pytest.importorskip("ofxh")
from ofxhost import constants as k  # noqa: E402

NOISE = "net.sf.openfx.noisePlugin"  # kOfxImageEffectPropSupportsMultipleClipPARs 0, a pixelAspectRatio parameter


@pytest.mark.parametrize("host_pars, is_connected, expected", [(1, False, 2.), (0, False, 768. / 720.), (1, True, 0.9)],
                         ids=["generator", "host without multiple PARs", "connected input"])
def test_generator_output_par(plugin_dir, host_pars, is_connected, expected):
    """Noise doesn't support multiple clip PARs, but may set its output PAR when no input is connected,
    if the host supports multiple clip PARs (see Instance::setupClipPreferencesArgs()): its pixelAspectRatio
    parameter (2) is the output's PAR. Otherwise the output gets the project's PAR (a host without multiple clip
    PARs) or the input's (a connected input). Since #351, a generator could no longer set it; fixed in #355."""

    class Host(ofxh.Host):
        def init_properties(self, props):
            props[k.kOfxImageEffectPropSupportsMultipleClipPARs] = host_pars

    class Clip(ofxh.ClipInstance):
        connected = is_connected
        aspect_ratio = 0.9  # the input's PAR (the project's: 768/720)

    class Instance(ofxh.ImageEffectInstance):
        def new_clip_instance(self, desc, index):
            return ofxh.ClipInstance() if desc.is_output else Clip()

    ofxh.init(Host(), [plugin_dir], use_env_path=False)
    inst = ofxh.create_instance(ofxh.get_plugin(NOISE), "general", Instance())
    with inst:
        inst.set_param("pixelAspectRatio", 2.)
        assert inst.create_instance_action() in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
        assert inst.get_clip_preferences_action() == ofxh.Stat.OK
        assert inst.output_pixel_aspect_ratio == pytest.approx(expected)
