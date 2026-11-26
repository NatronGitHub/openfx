"""ofxh's OpenGL renders (ofxh.init(opengl=True), builds with OFX_HOSTSUPPORT_OPENGL): an action with
opengl=True is refused unless the instance renders with OpenGL. kOfxImageEffectPropOpenGLEnabled
means something only "When a plug-in and host have established they can both use OpenGL renders"
(ofxGPURender.h): a plug-in that says "false" would render on the CPU into an output that the
host then overwrites with its cleared texture."""

import os

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402

INVERT = "net.sf.openfx.invertPlugin"  # kOfxImageEffectPropOpenGLRenderSupported "false"


def _skip(why):
    if os.environ.get("OFXH_REQUIRE") == "1":
        pytest.fail(f"{why} (OFXH_REQUIRE=1)")
    pytest.skip(why)


def test_opengl_refused_for_a_cpu_plugin(plugin_dir):
    if not ofxh.build_config.get("OFX_HOSTSUPPORT_OPENGL"):
        _skip("ofxh was built without OFX_HOSTSUPPORT_OPENGL")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opengl=True)
    gl = ofxh.opengl_context()
    if not gl or "error" in gl:
        _skip(f"no OpenGL context: {(gl or {}).get('error')}")
    assert ofxh.host().props["OfxImageEffectPropOpenGLRenderSupported"][0] == "true"
    source = np.zeros((8, 8, 4), np.float32)
    inst = ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", minimal_host.Instance({"Source": source}))
    OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
    assert inst.create_instance_action() in OK
    assert inst.get_clip_preferences_action() in OK
    assert inst.render_mode() == "cpu" and inst.render_mode(False) == "cpu"
    for action, args in ((inst.begin_sequence_render_action, (0., 0.)), (inst.render_action, (0., (0, 0, 8, 8))),
                         (inst.end_sequence_render_action, (0., 0.))):
        with pytest.raises(ValueError, match="doesn't render with OpenGL"):
            action(*args, opengl=True)
    assert inst.opengl_result == ""  # nothing was sent with OpenGL
    stat, outputs = inst.render_action(0., (0, 0, 8, 8))
    assert stat == ofxh.Stat.OK and "Colour" in outputs  # without OpenGL: rendered
