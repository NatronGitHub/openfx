"""ofxh's Metal renders (ofxh.init(metal=True), macOS builds with OFX_HOSTSUPPORT_METAL), with GPUGain built
with its Metal kernel (OFX_EXAMPLE_GPUGAIN_METAL, on by default on macOS) and a Metal device.

The tests are marked metal: CI runs them (-m metal, OFXH_REQUIRE=1) in the macOS job, and deselects them in
the other runs.
"""

import os

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402

pytestmark = pytest.mark.metal

GPUGAIN = "com.OpenFXSample.GPUGain"  # Metal buffers "true" (no textures), a CPU path
INVERT = "net.sf.openfx.invertPlugin"  # no Metal render
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
WINDOW = (0, 0, 24, 16)
SOURCE = np.random.default_rng(1).random((16, 24, 4), dtype=np.float32)
# GPUGain with scale 0.5: r, g and b times 0.5, alpha unchanged (scaleA is 1)
EXPECTED = SOURCE * np.array([0.5, 0.5, 0.5, 1.], np.float32)

kMetal = "OfxImageEffectPropMetalRenderSupported"
kTextures = "OfxImageEffectPropMetalTextureSupported"
kCPU = "OfxImageEffectPropCPURenderSupported"


def _skip(why):
    if os.environ.get("OFXH_REQUIRE") == "1":
        pytest.fail(f"{why} (OFXH_REQUIRE=1)")
    pytest.skip(why)


@pytest.fixture
def device(plugin_dir):
    """ofxh initialized with a Metal device; yields metal_device(). Shut down after the test."""
    if not ofxh.build_config.get("OFX_HOSTSUPPORT_METAL"):
        _skip("ofxh was built without OFX_HOSTSUPPORT_METAL")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, metal=True)
    try:
        d = ofxh.metal_device()
        if not d or "error" in d:
            _skip(f"no Metal device: {(d or {}).get('error')}")
        yield d
    finally:
        ofxh.shutdown()


def _instance(plugin_id, inputs):
    """An instance in the filter context, with its clip preferences; GPUGain gets scale 0.5."""
    plugin = ofxh.get_plugin(plugin_id)
    if plugin is None:
        _skip(f"plug-in {plugin_id} not found")
    inst = ofxh.create_instance(plugin, "filter", minimal_host.Instance(inputs))
    assert inst.create_instance_action() in OK
    assert inst.get_clip_preferences_action() in OK
    if plugin_id == GPUGAIN:
        if inst.props[kMetal][0] != "true":
            _skip("GPUGain was built without Metal (OFX_EXAMPLE_GPUGAIN_METAL)")
        assert inst.set_param("scale", 0.5) in OK
    return inst


def _render(inst, metal=""):
    """Begin/Render/EndSequenceRender of WINDOW; returns (render status, output pixels or None)."""
    assert inst.begin_sequence_render_action(0., 0., metal=metal) in OK
    stat, outputs = inst.render_action(0., WINDOW, metal=metal)
    assert inst.end_sequence_render_action(0., 0., metal=metal) in OK
    out = outputs.get("Colour")
    return stat, (np.array(out.pixels) if out is not None else None)


def test_devices_and_host_properties(device):
    """metal_devices() lists the device of init(metal=True), and the host descriptor's
    kOfxImageEffectPropMetalRenderSupported ("Indicates whether a host or plug-in can support Metal render",
    ofxGPURender.h) and kOfxImageEffectPropMetalTextureSupported ("The host sets it on the host descriptor",
    upstream main) are "true"."""
    assert any(d["registry_id"] == device["registry_id"] for d in ofxh.metal_devices())
    assert ofxh.host().props[kMetal][0] == "true"
    assert ofxh.host().props[kTextures][0] == "true"


def test_gpugain_matches_cpu(device):
    """GPUGain with Metal buffers gives the CPU render's pixels."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        assert inst.render_mode(metal=True) == "metal-buffers"
        assert inst.render_mode(metal=True, prefer_textures=True) == "metal-buffers"  # (GPUGain has no textures)
        stat, cpu = _render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
        stat, gpu = _render(inst, "buffers")
        assert stat == ofxh.Stat.OK and inst.metal_result == "ok" and inst.metal_error == ""
        np.testing.assert_array_equal(gpu, cpu)


def test_cpu_plugin_stays_on_cpu(device):
    """Invert has no Metal render: render_mode() is "cpu", and metal=True is refused before anything is sent
    (the host MAY set kOfxImageEffectPropMetalEnabled only "If a plug-in and host have both set
    kOfxImageEffectPropMetalRenderSupported="true"")."""
    with _instance(INVERT, {"Source": SOURCE}) as inst:
        assert inst.render_mode(metal=True) == "cpu"
        for mode in ("buffers", "textures"):
            with pytest.raises(ValueError, match="doesn't render with Metal"):
                inst.render_action(0., WINDOW, metal=mode)
        assert inst.metal_result == ""  # nothing was sent with Metal
        stat, out = _render(inst)
        assert stat == ofxh.Stat.OK and out is not None


@pytest.mark.parametrize("cpu", ["true", "false"])
def test_cpu_retry(device, cpu):
    """GPUGain without a source returns kOfxStatGPURenderFailed with Metal ("the GPU kernels need a source
    image"): "retry" if the plug-in can render on the CPU, "failed" if its kOfxImageEffectPropCPURenderSupported
    is "false"."""
    with _instance(GPUGAIN, {}) as inst:
        inst.props[kCPU] = [cpu]
        assert inst.can_render_on_cpu() == (cpu == "true")
        assert inst.begin_sequence_render_action(0., 0., metal="buffers") in OK
        stat, _ = inst.render_action(0., WINDOW, metal="buffers")
        assert stat == ofxh.Stat.GPURenderFailed
        assert inst.metal_result == ("retry" if cpu == "true" else "failed")  # (of the last action)
        assert inst.end_sequence_render_action(0., 0., metal="buffers") in OK


def test_needed(device):
    """kOfxImageEffectPropMetalRenderSupported "needed" (a value of upstream main's @propdef) counts as Metal
    support; the CPU fallback is still decided by kOfxImageEffectPropCPURenderSupported alone."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        inst.props[kMetal] = ["needed"]
        assert inst.render_mode(metal=True) == "metal-buffers"
        stat, gpu = _render(inst, "buffers")
        assert stat == ofxh.Stat.OK and inst.metal_result == "ok"
        np.testing.assert_allclose(gpu, EXPECTED, atol=1e-6)


def test_session_refuses_without_host_support(device):
    """The RenderSession checks the precondition itself: with the host descriptor's
    kOfxImageEffectPropMetalRenderSupported "false", it refuses Metal and the action isn't sent ("not_sent");
    the render then works on the CPU."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        ofxh.host().props[kMetal] = ["false"]
        stat, out = inst.render_action(0., WINDOW, metal="buffers")
        assert stat == ofxh.Stat.Failed and inst.metal_result == "not_sent" and "Colour" not in out
        stat, cpu = _render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)


def test_metal_with_opencl_refused(device):
    """metal=True and opencl= can't be used together."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        with pytest.raises(ValueError):
            inst.render_action(0., WINDOW, metal="buffers", opencl="buffers")
        with pytest.raises(ValueError, match="expected"):
            inst.render_action(0., WINDOW, metal="images")


def test_texture_mode(device):
    """Metal textures (upstream main): render_mode() gives "metal-textures" only "for a path that both host and
    plug-in declared as supported" (kOfxImageEffectPropMetalTextureSupported "true"; "needed" isn't one of its
    values), buffers when both are supported unless prefer_textures ("both: the host chooses one of the two per
    action"), and the CPU otherwise. (GPUGain has no texture path, so no texture action is sent to it: the texture
    renders are checked by metalRenderChecks, with its own kernels.)"""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        inst.props[kTextures] = ["true"]
        assert inst.render_mode(metal=True) == "metal-buffers"
        assert inst.render_mode(metal=True, prefer_textures=True) == "metal-textures"
        inst.props[kMetal] = ["false"]
        assert inst.render_mode(metal=True) == "metal-textures"
        with pytest.raises(ValueError, match="doesn't render with Metal buffers"):
            inst.render_action(0., WINDOW, metal="buffers")
        inst.props[kTextures] = ["needed"]
        assert inst.render_mode(metal=True, prefer_textures=True) == "cpu"
        inst.props[kTextures] = ["false"]
        assert inst.render_mode(metal=True, prefer_textures=True) == "cpu"
        inst.props[kCPU] = ["false"]
        assert inst.render_mode(metal=True) == "none"


def test_texture_session_refuses_without_host_support(device):
    """With the host descriptor's kOfxImageEffectPropMetalTextureSupported "false", the session refuses textures
    ("only for a path that both host and plug-in declared as supported"): the action isn't sent ("not_sent")."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        inst.props[kTextures] = ["true"]
        inst.props[kMetal] = ["false"]
        ofxh.host().props[kTextures] = ["false"]
        stat, out = inst.render_action(0., WINDOW, metal="textures")
        assert stat == ofxh.Stat.Failed and inst.metal_result == "not_sent" and "Colour" not in out
