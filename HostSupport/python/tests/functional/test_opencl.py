"""ofxh's OpenCL renders (ofxh.init(opencl=True), builds with OFX_HOSTSUPPORT_OPENCL), with GPUGain built
with its OpenCL kernels (OFX_SUPPORTS_OPENCLRENDER) and an OpenCL device (#370, #371).

The tests are marked opencl: CI runs them (-m opencl, OFXH_REQUIRE=1) in the Linux job's PoCL step, which
has a device and GPUGain's kernels, and deselects them in the other runs. The tests that OpenCL shares with Metal
and CUDA (a CPU plug-in, the CPU retry, "needed", the session's own check) are in test_gpu_apis.py.
"""

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
from ofxh_helpers import GPU_APIS, OK, gpu_device, gpu_instance, render  # noqa: E402
from ofxhost.testing import _skip_or_fail  # noqa: E402

pytestmark = pytest.mark.opencl

OPENCL = GPU_APIS["opencl"]  # its support is Buffers (kOfxImageEffectPropOpenCLRenderSupported), other Images


@pytest.fixture
def device(plugin_dir):
    """ofxh initialized with an OpenCL device; yields opencl_device(). Shut down after the test."""
    with gpu_device(OPENCL, plugin_dir) as d:
        yield d


def test_devices_and_host_properties(device):
    """opencl_devices() lists the device of init(opencl=True), and the host descriptor says what it can do: "a
    host indicates it supports OpenCL acceleration by setting kOfxImageEffectPropOpenCLSupported (Images) and/or
    kOfxImageEffectPropOpenCLRenderSupported (Buffers) on it's descriptor" (ofxGPURender.h). (#370)"""
    assert any(d["platform"] == device["platform"] and d["device"] == device["device"] for d in ofxh.opencl_devices())
    props = ofxh.host().props
    assert props[OPENCL.support][0] == "true"
    assert props[OPENCL.other][0] == ("true" if device["images"] else "false")


@pytest.mark.parametrize("dtype", ["uint8", "uint16", "float32"])
@pytest.mark.parametrize("mode, comps", [("buffers", 4), ("buffers", 3), ("buffers", 1), ("images", 4)],
                         ids=["buffers-rgba", "buffers-rgb", "buffers-alpha", "images-rgba"])
def test_gpugain_formats_match_cpu(device, mode, comps, dtype):
    """GPUGain at each depth, RGBA, RGB and Alpha: OpenCL Buffers give the CPU render's pixels (the same
    rounding, halves up). OpenCL Images (RGBA clips only: the session's images are CL_RGBA) give them exactly at
    float depth (no conversion), and to a step of the integer depths: read_imagef and write_imagef normalize the
    integer channels, and write_imagef's conversion may round halves to even, or approximate, with an error of at
    most 0.6 of a step (OpenCL 1.1 section 8.3.1.2). With both Buffers and Images, render_mode() gives Buffers
    unless prefer_images (the host's choice). (#370, #371)"""
    if mode == "images" and not device["images"]:
        _skip_or_fail("the OpenCL device has no image support")
    m = {"uint8": 255, "uint16": 65535, "float32": 1}[dtype]
    src = np.random.default_rng(2).random((16, 24, comps), dtype=np.float32) * 1.25
    src = src if m == 1 else np.round(np.clip(src, 0, 1) * m).astype(dtype)
    with gpu_instance(OPENCL, {"Source": src}) as inst:
        assert inst.set_param("scaleA", 0.75) in OK
        assert inst.render_mode(opencl=True) == "opencl-buffers"  # (Buffers when both: the host's choice)
        if mode == "images":
            assert inst.render_mode(opencl=True, prefer_images=True) == "opencl-images"
        stat, cpu = render(inst)
        assert stat == ofxh.Stat.OK and cpu.dtype == np.dtype(dtype) and cpu.shape == src.shape
        gains = np.array([0.75] if comps == 1 else [0.5, 0.5, 0.5, 0.75][:comps], np.float32)
        expected = src * gains if m == 1 else np.floor(np.clip(src * gains, 0, m) + 0.5)
        np.testing.assert_array_equal(cpu, expected.astype(dtype))
        stat, gpu = render(inst, opencl=mode)
        assert stat == ofxh.Stat.OK and inst.opencl_result == "ok" and inst.opencl_error == ""
        if mode == "buffers" or m == 1:
            np.testing.assert_array_equal(gpu, cpu)
        else:
            np.testing.assert_allclose(gpu.astype(np.float64), cpu.astype(np.float64), rtol=0, atol=1)
