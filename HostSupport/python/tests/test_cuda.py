"""ofxh's CUDA renders (ofxh.init(cuda=True), builds with OFX_HOSTSUPPORT_CUDA).

Without a CUDA device (CI has none): the module's CUDA API reports why there is no device, the host doesn't
say it supports CUDA, and cuda=True is refused; also the API of a build without OFX_HOSTSUPPORT_CUDA. These
tests run everywhere.

With a device and GPUGain built with its CUDA kernel (OFX_SUPPORTS_CUDARENDER): the tests marked cuda, which
CI deselects (-m "not cuda"): it has no CUDA device. They have not run on a CUDA device yet. The library's
logic on a fake CUDA driver is in cudaRenderChecks (HostSupport/examples), which CI runs.
"""

import os

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402

GPUGAIN = "com.OpenFXSample.GPUGain"  # CUDA "true" and streams "true" with its CUDA kernel, a CPU path
INVERT = "net.sf.openfx.invertPlugin"  # no CUDA render
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
WINDOW = (0, 0, 24, 16)
SOURCE = np.random.default_rng(1).random((16, 24, 4), dtype=np.float32)
# GPUGain with scale 0.5: r, g and b times 0.5, alpha unchanged (scaleA is 1)
EXPECTED = SOURCE * np.array([0.5, 0.5, 0.5, 1.], np.float32)

kCuda = "OfxImageEffectPropCudaRenderSupported"
kStream = "OfxImageEffectPropCudaStreamSupported"
kCPU = "OfxImageEffectPropCPURenderSupported"

BUILT = bool(ofxh.build_config.get("OFX_HOSTSUPPORT_CUDA"))


def _instance(plugin_id, inputs):
    """An instance in the filter context, with its clip preferences; GPUGain gets scale 0.5."""
    plugin = ofxh.get_plugin(plugin_id)
    if plugin is None:
        _skip(f"plug-in {plugin_id} not found")
    inst = ofxh.create_instance(plugin, "filter", minimal_host.Instance(inputs))
    assert inst.create_instance_action() in OK
    assert inst.get_clip_preferences_action() in OK
    if plugin_id == GPUGAIN:
        assert inst.set_param("scale", 0.5) in OK
    return inst


def _skip(why, var="OFXH_REQUIRE"):
    if os.environ.get(var) == "1":
        pytest.fail(f"{why} ({var}=1)")
    pytest.skip(why)


# --- without a CUDA device (CI) ---------------------------------------------------------------

@pytest.fixture
def no_device(plugin_dir):
    """ofxh initialized with cuda=True on a machine without a CUDA device; yields cuda_device()."""
    if ofxh.cuda_devices():
        pytest.skip("this machine has a CUDA device: the tests marked cuda cover it")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True)
    try:
        yield ofxh.cuda_device()
    finally:
        ofxh.shutdown()


@pytest.mark.skipif(not BUILT, reason="ofxh was built without OFX_HOSTSUPPORT_CUDA (test_not_built covers it)")
def test_no_device(no_device):
    """Without a CUDA device: cuda_device() says why, the host descriptor's kOfxImageEffectPropCudaRenderSupported
    and kOfxImageEffectPropCudaStreamSupported stay "false" (it can't pass CUDA memory), GPUGain renders on the
    CPU (render_mode(cuda=True) is "cpu"), and cuda=True is refused before anything is sent."""
    assert set(no_device) == {"error"} and no_device["error"]
    assert ofxh.host().props[kCuda][0] == "false" and ofxh.host().props[kStream][0] == "false"
    with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
        assert inst.render_mode(cuda=True) == "cpu"
        with pytest.raises(ValueError, match="needs a CUDA device"):
            inst.render_action(0., WINDOW, cuda=True)
        assert inst.cuda_result == ""  # nothing was sent with CUDA
        stat, outputs = inst.render_action(0., WINDOW)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(np.array(outputs["Colour"].pixels), EXPECTED, atol=1e-6)


@pytest.mark.skipif(not BUILT, reason="ofxh was built without OFX_HOSTSUPPORT_CUDA (test_not_built covers it)")
def test_options(plugin_dir):
    """cuda=True can't be used with metal=True, opencl or opengl=True; init()'s cuda_device must be -1 or an
    index."""
    with pytest.raises(ValueError, match="cuda_device"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True, cuda_device=-2)
    with pytest.raises(ValueError, match="cuda_uuid"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda_uuid="GPU-0")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True)
    try:
        with _instance(GPUGAIN, {"Source": SOURCE}) as inst:
            for other in ({"metal": "buffers"}, {"opencl": "buffers"}, {"opengl": True}):
                with pytest.raises(ValueError, match="can't be used with opengl, opencl or metal"):
                    inst.render_action(0., WINDOW, cuda=True, **other)
    finally:
        ofxh.shutdown()


@pytest.mark.skipif(BUILT, reason="ofxh was built with OFX_HOSTSUPPORT_CUDA")
def test_not_built(plugin_dir):
    """A build without OFX_HOSTSUPPORT_CUDA: no CUDA device and no CUDA option."""
    assert ofxh.cuda_devices() == [] and ofxh.cuda_device() is None
    with pytest.raises(ValueError, match="without OFX_HOSTSUPPORT_CUDA"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True)


# --- with a CUDA device (never in CI) ---------------------------------------------------------

@pytest.fixture
def device(plugin_dir):
    """ofxh initialized with a CUDA device; yields cuda_device(). Shut down after the test."""
    if not BUILT:
        _skip("ofxh was built without OFX_HOSTSUPPORT_CUDA", "OFXH_REQUIRE_CUDA")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True)
    try:
        d = ofxh.cuda_device()
        if not d or "error" in d:
            _skip(f"no CUDA device: {(d or {}).get('error')}", "OFXH_REQUIRE_CUDA")
        yield d
    finally:
        ofxh.shutdown()


def _gpugain(inputs):
    inst = _instance(GPUGAIN, inputs)
    if inst.props[kCuda][0] != "true":
        inst.close()
        _skip("GPUGain was built without CUDA (OFX_SUPPORTS_CUDARENDER)", "OFXH_REQUIRE_CUDA")
    return inst


def _render(inst, cuda=False):
    """Begin/Render/EndSequenceRender of WINDOW; returns (render status, output pixels or None)."""
    assert inst.begin_sequence_render_action(0., 0., cuda=cuda) in OK
    stat, outputs = inst.render_action(0., WINDOW, cuda=cuda)
    assert inst.end_sequence_render_action(0., 0., cuda=cuda) in OK
    out = outputs.get("Colour")
    return stat, (np.array(out.pixels) if out is not None else None)


@pytest.mark.cuda
def test_device_and_host_properties(device):
    """cuda_devices() lists the device of init(cuda=True), and the host descriptor's
    kOfxImageEffectPropCudaRenderSupported and kOfxImageEffectPropCudaStreamSupported are "true"."""
    assert any(d["uuid"] == device["uuid"] and d["uuid"].startswith("GPU-") for d in ofxh.cuda_devices())
    assert ofxh.host().props[kCuda][0] == "true" and ofxh.host().props[kStream][0] == "true"


@pytest.mark.cuda
def test_device_by_uuid(plugin_dir, device):
    """init(cuda_uuid=...) makes the device of that UUID."""
    ofxh.shutdown()
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True, cuda_uuid=device["uuid"])
    assert ofxh.cuda_device()["uuid"] == device["uuid"]


@pytest.mark.cuda
@pytest.mark.parametrize("stream", ["true", "false"])
def test_gpugain_matches_cpu(device, stream):
    """GPUGain with CUDA memory gives the CPU render's pixels, with a CUDA stream (the plug-in's
    kOfxImageEffectPropCudaStreamSupported "true") and without (then it waits for its kernel itself)."""
    with _gpugain({"Source": SOURCE}) as inst:
        inst.props[kStream] = [stream]
        assert inst.render_mode(cuda=True) == "cuda"
        stat, cpu = _render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
        stat, gpu = _render(inst, True)
        assert stat == ofxh.Stat.OK and inst.cuda_result == "ok" and inst.cuda_error == ""
        np.testing.assert_array_equal(gpu, cpu)


@pytest.mark.cuda
def test_cpu_plugin_stays_on_cpu(device):
    """Invert has no CUDA render: render_mode() is "cpu", and cuda=True is refused before anything is sent."""
    with _instance(INVERT, {"Source": SOURCE}) as inst:
        assert inst.render_mode(cuda=True) == "cpu"
        with pytest.raises(ValueError, match="doesn't render with CUDA"):
            inst.render_action(0., WINDOW, cuda=True)
        assert inst.cuda_result == ""


@pytest.mark.cuda
@pytest.mark.parametrize("cpu", ["true", "false"])
def test_cpu_retry(device, cpu):
    """GPUGain without a source returns kOfxStatGPURenderFailed with CUDA: "retry" if the plug-in can render on
    the CPU, "failed" if its kOfxImageEffectPropCPURenderSupported is "false"."""
    with _gpugain({}) as inst:
        inst.props[kCPU] = [cpu]
        stat, _ = inst.render_action(0., WINDOW, cuda=True)
        assert stat == ofxh.Stat.GPURenderFailed
        assert inst.cuda_result == ("retry" if cpu == "true" else "failed")


@pytest.mark.cuda
def test_session_refuses_without_host_support(device):
    """The RenderSession checks the precondition itself: with the host descriptor's
    kOfxImageEffectPropCudaRenderSupported "false", it refuses CUDA and the action isn't sent ("not_sent")."""
    with _gpugain({"Source": SOURCE}) as inst:
        ofxh.host().props[kCuda] = ["false"]
        stat, out = inst.render_action(0., WINDOW, cuda=True)
        assert stat == ofxh.Stat.Failed and inst.cuda_result == "not_sent" and "Colour" not in out
