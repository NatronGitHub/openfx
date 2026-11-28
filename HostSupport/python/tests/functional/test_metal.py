"""ofxh's Metal renders (ofxh.init(metal=True), macOS builds with OFX_HOSTSUPPORT_METAL), with GPUGain built
with its Metal kernel (OFX_EXAMPLE_GPUGAIN_METAL, on by default on macOS) and a Metal device (#379).

The tests are marked metal: CI runs them (-m metal, OFXH_REQUIRE=1) in the macOS job, and deselects them in
the other runs. The tests that Metal shares with OpenCL and CUDA (a CPU plug-in, the CPU retry, "needed", the
session's own checks) are in test_gpu_apis.py.
"""

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
from ofxh_helpers import EXPECTED, GPU_APIS, SOURCE, WINDOW, gpu_device, gpu_instance, kCPU, render  # noqa: E402

pytestmark = pytest.mark.metal

METAL = GPU_APIS["metal"]  # its support is kOfxImageEffectPropMetalRenderSupported, other the textures'


@pytest.fixture
def device(plugin_dir):
    """ofxh initialized with a Metal device; yields metal_device(). Shut down after the test."""
    with gpu_device(METAL, plugin_dir) as d:
        yield d


def test_devices_and_host_properties(device):
    """metal_devices() lists the device of init(metal=True), and the host descriptor's
    kOfxImageEffectPropMetalRenderSupported ("Indicates whether a host or plug-in can support Metal render",
    ofxGPURender.h) and kOfxImageEffectPropMetalTextureSupported ("The host sets it on the host descriptor",
    upstream main) are "true". (#379)"""
    assert any(d["registry_id"] == device["registry_id"] for d in ofxh.metal_devices())
    assert ofxh.host().props[METAL.support][0] == "true"
    assert ofxh.host().props[METAL.other][0] == "true"


def test_gpugain_matches_cpu(device):
    """GPUGain with Metal buffers gives the CPU render's pixels, also when textures are preferred (GPUGain has no
    texture path). (#379)"""
    with gpu_instance(METAL, {"Source": SOURCE}) as inst:
        assert inst.render_mode(metal=True) == "metal-buffers"
        assert inst.render_mode(metal=True, prefer_textures=True) == "metal-buffers"  # (GPUGain has no textures)
        stat, cpu = render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
        stat, gpu = render(inst, metal="buffers")
        assert stat == ofxh.Stat.OK and inst.metal_result == "ok" and inst.metal_error == ""
        np.testing.assert_array_equal(gpu, cpu)


def test_metal_with_opencl_refused(device):
    """ofxh's API: metal= and opencl= can't be used in the same action, and metal= takes only "buffers" or
    "textures". (#379)"""
    with gpu_instance(METAL, {"Source": SOURCE}) as inst:
        with pytest.raises(ValueError):
            inst.render_action(0., WINDOW, metal="buffers", opencl="buffers")
        with pytest.raises(ValueError, match="expected"):
            inst.render_action(0., WINDOW, metal="images")


def test_texture_mode(device):
    """Metal textures (upstream main): render_mode() gives "metal-textures" only "for a path that both host and
    plug-in declared as supported" (kOfxImageEffectPropMetalTextureSupported "true"; "needed" isn't one of its
    values), buffers when both are supported unless prefer_textures ("both: the host chooses one of the two per
    action"), and the CPU otherwise. (GPUGain has no texture path, so no texture action is sent to it: the texture
    renders are checked by metalRenderChecks, with its own kernels.) (#379)"""
    with gpu_instance(METAL, {"Source": SOURCE}) as inst:
        inst.props[METAL.other] = ["true"]
        assert inst.render_mode(metal=True) == "metal-buffers"
        assert inst.render_mode(metal=True, prefer_textures=True) == "metal-textures"
        inst.props[METAL.support] = ["false"]
        assert inst.render_mode(metal=True) == "metal-textures"
        with pytest.raises(ValueError, match="doesn't render with Metal buffers"):
            inst.render_action(0., WINDOW, metal="buffers")
        inst.props[METAL.other] = ["needed"]
        assert inst.render_mode(metal=True, prefer_textures=True) == "cpu"
        inst.props[METAL.other] = ["false"]
        assert inst.render_mode(metal=True, prefer_textures=True) == "cpu"
        inst.props[kCPU] = ["false"]
        assert inst.render_mode(metal=True) == "none"
