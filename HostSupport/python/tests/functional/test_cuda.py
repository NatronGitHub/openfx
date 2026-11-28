"""ofxh's CUDA renders (ofxh.init(cuda=True), builds with OFX_HOSTSUPPORT_CUDA) (#388).

Without a CUDA device (CI has none): the module's CUDA API reports why there is no device, the host doesn't
say it supports CUDA, and cuda=True is refused; also the API of a build without OFX_HOSTSUPPORT_CUDA. These
tests run everywhere.

With a device and GPUGain built with its CUDA kernel (OFX_SUPPORTS_CUDARENDER): the tests marked cuda, which
CI deselects (-m "not cuda"): it has no CUDA device; without a device they skip, and fail with
OFXH_REQUIRE_CUDA=1. The library's logic on a fake CUDA driver is in cudaRenderChecks
(HostSupport/tests/functional), which CI runs. The tests that CUDA shares with OpenCL and Metal (a CPU
plug-in, the CPU retry, the session's own check) are in test_gpu_apis.py.
"""

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402
from ofxh_helpers import (EXPECTED, GPU_APIS, GPUGAIN, SOURCE, WINDOW, gpu_device, gpu_instance,  # noqa: E402
                          instance, render)

CUDA = GPU_APIS["cuda"]
kCuda = "OfxImageEffectPropCudaRenderSupported"
kStream = "OfxImageEffectPropCudaStreamSupported"

BUILT = bool(ofxh.build_config.get("OFX_HOSTSUPPORT_CUDA"))


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
    CPU (render_mode(cuda=True) is "cpu"), and cuda=True is refused before anything is sent. (#388)"""
    assert set(no_device) == {"error"} and no_device["error"]
    assert ofxh.host().props[kCuda][0] == "false" and ofxh.host().props[kStream][0] == "false"
    with instance(GPUGAIN, {"Source": SOURCE}) as inst:
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
    index, and its cuda_uuid needs cuda=True. (#388)"""
    with pytest.raises(ValueError, match="cuda_device"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True, cuda_device=-2)
    with pytest.raises(ValueError, match="cuda_uuid"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda_uuid="GPU-0")
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True)
    try:
        with instance(GPUGAIN, {"Source": SOURCE}) as inst:
            for other in ({"metal": "buffers"}, {"opencl": "buffers"}, {"opengl": True}):
                with pytest.raises(ValueError, match="can't be used with opengl, opencl or metal"):
                    inst.render_action(0., WINDOW, cuda=True, **other)
    finally:
        ofxh.shutdown()


@pytest.mark.skipif(BUILT, reason="ofxh was built with OFX_HOSTSUPPORT_CUDA")
def test_not_built(plugin_dir):
    """A build without OFX_HOSTSUPPORT_CUDA: no CUDA device and no CUDA option. (It runs only in the builds
    without CUDA: extended.yml's pip-installed module.) (#388)"""
    assert ofxh.cuda_devices() == [] and ofxh.cuda_device() is None
    with pytest.raises(ValueError, match="without OFX_HOSTSUPPORT_CUDA"):
        ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True)


# --- with a CUDA device (never in CI) ---------------------------------------------------------

@pytest.fixture
def device(plugin_dir):
    """ofxh initialized with a CUDA device; yields cuda_device(). Shut down after the test."""
    with gpu_device(CUDA, plugin_dir) as d:
        yield d


@pytest.mark.cuda
def test_device_and_host_properties(device):
    """cuda_devices() lists the device of init(cuda=True), and the host descriptor's
    kOfxImageEffectPropCudaRenderSupported and kOfxImageEffectPropCudaStreamSupported are "true". (#388)"""
    assert any(d["uuid"] == device["uuid"] and d["uuid"].startswith("GPU-") for d in ofxh.cuda_devices())
    assert ofxh.host().props[kCuda][0] == "true" and ofxh.host().props[kStream][0] == "true"


@pytest.mark.cuda
def test_device_by_uuid(plugin_dir, device):
    """init(cuda_uuid=...) makes the device of that UUID. (#388)"""
    ofxh.shutdown()
    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, cuda=True, cuda_uuid=device["uuid"])
    assert ofxh.cuda_device()["uuid"] == device["uuid"]


@pytest.mark.cuda
@pytest.mark.parametrize("stream", ["true", "false"])
def test_gpugain_matches_cpu(device, stream):
    """GPUGain with CUDA memory gives the CPU render's pixels, with a CUDA stream (the plug-in's
    kOfxImageEffectPropCudaStreamSupported "true") and without (then it waits for its kernel itself). (#388)"""
    with gpu_instance(CUDA, {"Source": SOURCE}) as inst:
        inst.props[kStream] = [stream]
        assert inst.render_mode(cuda=True) == "cuda"
        stat, cpu = render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
        stat, gpu = render(inst, cuda=True)
        assert stat == ofxh.Stat.OK and inst.cuda_result == "ok" and inst.cuda_error == ""
        np.testing.assert_array_equal(gpu, cpu)
