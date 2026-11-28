"""ofxh's OpenGL renders (ofxh.init(opengl=True), builds with OFX_HOSTSUPPORT_OPENGL): an action with
opengl=True is refused unless the instance renders with OpenGL. kOfxImageEffectPropOpenGLEnabled
means something only "When a plug-in and host have established they can both use OpenGL renders"
(ofxGPURender.h): a plug-in that says "false" would render on the CPU into an output that the
host then overwrites with its cleared texture.

A build without OFX_HOSTSUPPORT_OPENGL (the default, as extended.yml's pip-installed ofxh) skips the
OpenGL tests, and test_not_built checks its API. OFXH_REQUIRE_OPENGL=1 (build.yml, whose builds all have
OpenGL) makes a build without it fail; OFXH_REQUIRE=1 makes a missing context or renderer fail."""

import os

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402

INVERT = "net.sf.openfx.invertPlugin"  # kOfxImageEffectPropOpenGLRenderSupported "false"


BUILT = bool(ofxh.build_config.get("OFX_HOSTSUPPORT_OPENGL"))


def _skip(why, var="OFXH_REQUIRE"):
    if os.environ.get(var) == "1":
        pytest.fail(f"{why} ({var}=1)")
    pytest.skip(why)


def _need_built():
    if not BUILT:
        _skip("ofxh was built without OFX_HOSTSUPPORT_OPENGL (test_not_built covers it)", "OFXH_REQUIRE_OPENGL")


@pytest.mark.skipif(BUILT, reason="ofxh was built with OFX_HOSTSUPPORT_OPENGL")
def test_not_built(plugin_dir):
    """A build without OFX_HOSTSUPPORT_OPENGL: no renderer, no context and no opengl option."""
    assert ofxh.opengl_renderers() == [] and ofxh.opengl_context() is None
    with pytest.raises(ValueError, match="without OFX_HOSTSUPPORT_OPENGL"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opengl=True)


def test_opengl_refused_for_a_cpu_plugin(plugin_dir):
    _need_built()
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


# --- renderer selection (ofxh.opengl_renderers(), init(opengl_backends=..., opengl_renderer=...)) --

GPUGAIN = "com.OpenFXSample.GPUGain"  # kOfxImageEffectPropOpenGLRenderSupported "true", with a CPU path
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
# the backends of these tests: OFXH_OPENGL_BACKENDS (comma-separated names, e.g. "glx" under Xvfb in
# CI), else the platform's default ones
BACKENDS = [b for b in os.environ.get("OFXH_OPENGL_BACKENDS", "").split(",") if b] or None


def _renderers():
    """The renderers of BACKENDS; skips if ofxh has no OpenGL (OFXH_REQUIRE_OPENGL=1: fails) or there is no
    renderer (OFXH_REQUIRE=1: fails)."""
    _need_built()
    renderers = ofxh.opengl_renderers(BACKENDS)
    if not renderers:
        _skip(f"no OpenGL renderer (backends: {BACKENDS or 'the default ones'})")
    return renderers


def _gpugain(opengl):
    """GPUGain's output (scale 0.5) for a float checker, rendered with OpenGL or on the CPU."""
    source = np.zeros((48, 64, 4), np.float32)
    source[::2, ::2] = (1., 0.5, 0.25, 1.)
    plugin = ofxh.get_plugin(GPUGAIN)
    if plugin is None:
        _skip(f"{GPUGAIN} is not built (example-GPUGain-support)")
    inst = ofxh.create_instance(plugin, "filter", minimal_host.Instance({"Source": source}))
    with inst:
        assert inst.create_instance_action() in OK
        assert inst.set_param("scale", 0.5) in OK
        assert inst.get_clip_preferences_action() in OK
        window = (0, 0, 64, 48)
        if opengl:
            assert inst.render_mode() == "opengl"
        assert inst.begin_sequence_render_action(0., 0., opengl=opengl) in OK
        stat, outputs = inst.render_action(0., window, opengl=opengl)
        assert stat == ofxh.Stat.OK, (stat, inst.opengl_error)
        if opengl:
            assert inst.opengl_result == "ok", inst.opengl_error
        assert inst.end_sequence_render_action(0., 0., opengl=opengl) in OK
        return np.array(outputs["Colour"].pixels)


def test_renderers_listed():
    """opengl_renderers() gives at least the default renderer, with its id and strings."""
    for r in _renderers():
        assert set(r) >= {"backend", "id", "renderer", "vendor", "version", "glsl", "video_memory", "software",
                          "max_texture_size"}
        assert isinstance(r["id"], int) and r["renderer"] and r["max_texture_size"] > 0 and r["video_memory"] >= 0


def test_each_renderer_renders_gpugain(plugin_dir):
    """A context on each listed renderer, chosen by its id, renders GPUGain with OpenGL as the CPU does."""
    for r in _renderers():
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opengl=True,
                  opengl_backends=[r["backend"]], opengl_renderer=r["id"])
        try:
            gl = ofxh.opengl_context()
            assert gl and "error" not in gl, gl
            assert (gl["backend"], gl["id"], gl["renderer"]) == (r["backend"], r["id"], r["renderer"])
            assert ofxh.host().props["OfxImageEffectPropOpenGLRenderSupported"][0] == "true"
            np.testing.assert_allclose(_gpugain(True), _gpugain(False), atol=1e-3,
                                       err_msg=f"{r['backend']}:{r['id']} {r['renderer']}")
        finally:
            ofxh.shutdown()


def test_bad_renderer_refused(plugin_dir):
    """An unknown renderer id is refused cleanly: a ValueError that says why, and no host is left
    initialized; so is an unknown backend, or a choice without opengl=True; backends that aren't a list
    of names are a TypeError."""
    renderers = _renderers()
    backend = renderers[0]["backend"]
    # a listed backend with an unknown id, and a backend with no renderer here (wgl outside Windows,
    # e.g.) with its default id
    unlisted = [b for b in ("wgl", "cgl", "glx", "egl-surfaceless", "egl-device")
                if b not in {r["backend"] for r in renderers}][0]
    for backends, renderer in (([backend], 1 << 30), ([unlisted], -1)):
        try:
            with pytest.raises(ValueError, match=r"no OpenGL context on the chosen backends or renderer") as e:
                ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opengl=True,
                          opengl_backends=backends, opengl_renderer=renderer)
            assert e.value.opengl_error, (backends, renderer)
            assert not ofxh.initialized()
        finally:
            ofxh.shutdown()
    with pytest.raises(ValueError, match="unknown OpenGL backend"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opengl=True, opengl_backends=["vulkan"])
    with pytest.raises(ValueError, match="need opengl=True"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opengl_renderer=0)
    with pytest.raises(ValueError, match="unknown OpenGL backend"):
        ofxh.opengl_renderers(["vulkan"])
    with pytest.raises(TypeError, match="not a str"):
        ofxh.opengl_renderers(backend)  # (a name, not a list of names)
    with pytest.raises(TypeError, match="must be a str"):
        ofxh.opengl_renderers([0])
