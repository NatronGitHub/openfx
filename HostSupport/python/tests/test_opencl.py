"""ofxh's OpenCL renders (ofxh.init(opencl=True), builds with OFX_HOSTSUPPORT_OPENCL), with GPUGain built
with its OpenCL kernels (OFX_SUPPORTS_OPENCLRENDER) and an OpenCL device.

The tests are marked opencl: CI runs them (-m opencl, OFXH_REQUIRE=1) in the Linux job's PoCL step, which
has a device and GPUGain's kernels, and deselects them in the other runs.
"""

import os

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402

pytestmark = pytest.mark.opencl

GPUGAIN = "com.OpenFXSample.GPUGain"  # OpenCL Buffers and Images "true", a CPU path
INVERT = "net.sf.openfx.invertPlugin"  # no OpenCL render
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
WINDOW = (0, 0, 24, 16)
SOURCE = np.random.default_rng(1).random((16, 24, 4), dtype=np.float32)
# GPUGain with scale 0.5: r, g and b times 0.5, alpha unchanged (scaleA is 1)
EXPECTED = SOURCE * np.array([0.5, 0.5, 0.5, 1.], np.float32)

kBuffers = "OfxImageEffectPropOpenCLRenderSupported"
kImages = "OfxImageEffectPropOpenCLSupported"
kCPU = "OfxImageEffectPropCPURenderSupported"


def _skip(why):
    if os.environ.get("OFXH_REQUIRE") == "1":
        pytest.fail(f"{why} (OFXH_REQUIRE=1)")
    pytest.skip(why)


@pytest.fixture
def device(plugin_dir):
    """ofxh initialized with an OpenCL device; yields opencl_device(). Shut down after the test."""
    if not ofxh.build_config.get("OFX_HOSTSUPPORT_OPENCL"):
        _skip("ofxh was built without OFX_HOSTSUPPORT_OPENCL")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, opencl=True)
    try:
        d = ofxh.opencl_device()
        if not d or "error" in d:
            _skip(f"no OpenCL device: {(d or {}).get('error')}")
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
        if inst.props[kBuffers][0] != "true":
            _skip("GPUGain was built without OpenCL (OFX_SUPPORTS_OPENCLRENDER)")
        assert inst.set_param("scale", 0.5) in OK
    return inst


def _render(inst, opencl=""):
    """Begin/Render/EndSequenceRender of WINDOW; returns (render status, output pixels or None)."""
    assert inst.begin_sequence_render_action(0., 0., opencl=opencl) in OK
    stat, outputs = inst.render_action(0., WINDOW, opencl=opencl)
    assert inst.end_sequence_render_action(0., 0., opencl=opencl) in OK
    out = outputs.get("Colour")
    return stat, (np.array(out.pixels) if out is not None else None)


def test_devices_and_host_properties(device):
    """opencl_devices() lists the device of init(opencl=True), and the host descriptor says what it can do: "a
    host indicates it supports OpenCL acceleration by setting kOfxImageEffectPropOpenCLSupported (Images) and/or
    kOfxImageEffectPropOpenCLRenderSupported (Buffers) on it's descriptor" (ofxGPURender.h)."""
    assert any(d["platform"] == device["platform"] and d["device"] == device["device"] for d in ofxh.opencl_devices())
    props = ofxh.host().props
    assert props[kBuffers][0] == "true"
    assert props[kImages][0] == ("true" if device["images"] else "false")


@pytest.mark.parametrize("mode", ["buffers", "images"])
def test_gpugain_matches_cpu(device, mode):
    """GPUGain with OpenCL Buffers and with OpenCL Images gives the CPU render's pixels."""
    if mode == "images" and not device["images"]:
        _skip("the OpenCL device has no image support")
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        assert inst.render_mode(opencl=True) == "opencl-buffers"  # (Buffers when both: the host's choice)
        assert inst.render_mode(opencl=True, prefer_images=True) == "opencl-images"
        stat, cpu = _render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
        stat, gpu = _render(inst, mode)
        assert stat == ofxh.Stat.OK and inst.opencl_result == "ok" and inst.opencl_error == ""
        np.testing.assert_array_equal(gpu, cpu)


def test_cpu_plugin_stays_on_cpu(device):
    """Invert has no OpenCL render: render_mode() is "cpu", and opencl= is refused before anything is sent
    (the host MAY set kOfxImageEffectPropOpenCLEnabled only "If a plug-in and host have both set" a support
    property to "true")."""
    with _instance(INVERT, {"Source": SOURCE}) as inst:
        assert inst.render_mode(opencl=True) == "cpu"
        for mode in ("buffers", "images"):
            with pytest.raises(ValueError, match="doesn't render with OpenCL"):
                inst.render_action(0., WINDOW, opencl=mode)
        assert inst.opencl_result == ""  # nothing was sent with OpenCL
        stat, out = _render(inst)
        assert stat == ofxh.Stat.OK and out is not None


@pytest.mark.parametrize("cpu", ["true", "false"])
def test_cpu_retry(device, cpu):
    """GPUGain without a source returns kOfxStatGPURenderFailed with OpenCL ("the GPU kernels need a source
    image"): "retry" if the plug-in can render on the CPU ("If a call using OpenCL rendering fails, the host
    may re-attempt using CPU buffers instead"), "failed" if its kOfxImageEffectPropCPURenderSupported is
    "false"."""
    with _instance(GPUGAIN, {}) as inst:
        inst.props[kCPU] = [cpu]
        assert inst.can_render_on_cpu() == (cpu == "true")
        assert inst.begin_sequence_render_action(0., 0., opencl="buffers") in OK
        stat, _ = inst.render_action(0., WINDOW, opencl="buffers")
        assert stat == ofxh.Stat.GPURenderFailed
        assert inst.opencl_result == ("retry" if cpu == "true" else "failed")  # (of the last action)
        assert inst.end_sequence_render_action(0., 0., opencl="buffers") in OK


def test_needed_buffers(device):
    """kOfxImageEffectPropOpenCLRenderSupported "needed" (a value of upstream main's @propdef) counts as Buffers
    support; the CPU fallback is still decided by kOfxImageEffectPropCPURenderSupported alone."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        inst.props[kBuffers] = ["needed"]
        inst.props[kImages] = ["false"]
        assert inst.render_mode(opencl=True, prefer_images=True) == "opencl-buffers"
        stat, gpu = _render(inst, "buffers")
        assert stat == ofxh.Stat.OK and inst.opencl_result == "ok"
        np.testing.assert_allclose(gpu, EXPECTED, atol=1e-6)


def test_session_refuses_without_host_support(device):
    """The RenderSession checks the precondition itself: with the host descriptor's
    kOfxImageEffectPropOpenCLRenderSupported "false", it refuses Buffers and the action isn't sent
    ("not_sent"); the render then works on the CPU."""
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        ofxh.host().props[kBuffers] = ["false"]
        stat, out = inst.render_action(0., WINDOW, opencl="buffers")
        assert stat == ofxh.Stat.Failed and inst.opencl_result == "not_sent" and "Colour" not in out
        stat, cpu = _render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
