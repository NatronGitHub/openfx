"""What ofxh's OpenCL, Metal and CUDA renders share (#370, #379, #388): a plug-in without the API renders on the CPU,
a failed GPU render is retried on the CPU only if the plug-in can render there, "needed" counts as support, and the
RenderSession checks the host's support itself. Each test runs once per API (the api fixture), with its API's marker
(opencl, metal, cuda), so that CI's -m opencl (PoCL step), -m metal (macOS job) and -m "not opencl and not metal and
not cuda" select them as they select the tests of test_opencl.py, test_metal.py and test_cuda.py: each case needs a
device of its API, and GPUGain built with it.
"""

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
from ofxh_helpers import (EXPECTED, GPU_APIS, INVERT, OK, SOURCE, WINDOW, gpu_device, gpu_instance,  # noqa: E402
                          instance, kCPU, render)


def _apis(*names):
    """The api fixture's parameters for these APIs, each with its marker."""
    return [pytest.param(n, marks=getattr(pytest.mark, n), id=n) for n in names]


@pytest.fixture
def api(request, plugin_dir):
    """ofxh initialized with a device of the API request.param (indirect parametrization); yields its GpuApi. Shut
    down after the test."""
    a = GPU_APIS[request.param]
    with gpu_device(a, plugin_dir):
        yield a


@pytest.mark.parametrize("api", _apis("opencl", "metal", "cuda"), indirect=True)
def test_cpu_plugin_stays_on_cpu(api):
    """Invert has no GPU render: render_mode() is "cpu", and each GPU mode is refused before anything is sent; the
    render then works on the CPU. "If a plug-in and host have both set" the API's support property to "true", "then
    the host MAY set" its Enabled property (ofxGPURender.h: kOfxImageEffectPropOpenCLEnabled,
    kOfxImageEffectPropMetalEnabled, kOfxImageEffectPropCudaEnabled): not for a plug-in that didn't. (#370, #379,
    #388)"""
    with instance(INVERT, {"Source": SOURCE}) as inst:
        assert inst.render_mode(**{api.name: True}) == "cpu"
        for mode in api.cpu_modes:
            with pytest.raises(ValueError, match=f"doesn't render with {api.label}"):
                inst.render_action(0., WINDOW, **api.kw(mode))
        assert api.result(inst) == ""  # nothing was sent with the API
        stat, out = render(inst)
        assert stat == ofxh.Stat.OK and out is not None


@pytest.mark.parametrize("cpu", ["true", "false"])
@pytest.mark.parametrize("api", _apis("opencl", "metal", "cuda"), indirect=True)
def test_cpu_retry(api, cpu):
    """GPUGain without a source returns kOfxStatGPURenderFailed with a GPU API ("the GPU kernels need a source
    image"): "retry" if the plug-in can render on the CPU ("If a call using OpenCL rendering fails, the host may
    re-attempt using CPU buffers instead", ofxGPURender.h; ofxh does the same for Metal and CUDA), "failed" if its
    kOfxImageEffectPropCPURenderSupported is "false". (#370, #379, #388)"""
    with gpu_instance(api, {}) as inst:
        inst.props[kCPU] = [cpu]
        assert inst.can_render_on_cpu() == (cpu == "true")
        assert inst.begin_sequence_render_action(0., 0., **api.kw()) in OK
        stat, _ = inst.render_action(0., WINDOW, **api.kw())
        assert stat == ofxh.Stat.GPURenderFailed
        assert api.result(inst) == ("retry" if cpu == "true" else "failed")  # (of the last action)
        assert inst.end_sequence_render_action(0., 0., **api.kw()) in OK


@pytest.mark.parametrize("api", _apis("opencl", "metal"), indirect=True)
def test_needed(api):
    """The buffers support property "needed" (a value of upstream main's @propdef of
    kOfxImageEffectPropOpenCLRenderSupported and kOfxImageEffectPropMetalRenderSupported) counts as support, also
    when the other path (OpenCL Images, Metal textures) is preferred but not supported; the CPU fallback is still
    decided by kOfxImageEffectPropCPURenderSupported alone. (#370, #379)"""
    with gpu_instance(api, {"Source": SOURCE}) as inst:
        inst.props[api.support] = ["needed"]
        inst.props[api.other] = ["false"]
        assert inst.render_mode(**{api.name: True, api.prefer: True}) == f"{api.name}-buffers"
        stat, gpu = render(inst, **api.kw())
        assert stat == ofxh.Stat.OK and api.result(inst) == "ok"
        np.testing.assert_allclose(gpu, EXPECTED, atol=1e-6)


@pytest.mark.parametrize("api, mode", [
    pytest.param("opencl", "buffers", marks=pytest.mark.opencl, id="opencl-buffers"),
    pytest.param("metal", "buffers", marks=pytest.mark.metal, id="metal-buffers"),
    pytest.param("metal", "textures", marks=pytest.mark.metal, id="metal-textures"),
    pytest.param("cuda", True, marks=pytest.mark.cuda, id="cuda"),
], indirect=["api"])
def test_session_refuses_without_host_support(api, mode):
    """The RenderSession checks the precondition itself: with the host descriptor's support property of the path
    "false" (kOfxImageEffectPropOpenCLRenderSupported, kOfxImageEffectPropMetalRenderSupported,
    kOfxImageEffectPropMetalTextureSupported for a plug-in that supports textures: "only for a path that both host
    and plug-in declared as supported", kOfxImageEffectPropCudaRenderSupported), it refuses the path and the action
    isn't sent ("not_sent"); the render then works on the CPU. (#370, #379, #388)"""
    with gpu_instance(api, {"Source": SOURCE}) as inst:
        host_prop = api.support
        if mode == "textures":
            inst.props[api.other] = ["true"]
            inst.props[api.support] = ["false"]
            host_prop = api.other
        ofxh.host().props[host_prop] = ["false"]
        stat, out = inst.render_action(0., WINDOW, **api.kw(mode))
        assert stat == ofxh.Stat.Failed and api.result(inst) == "not_sent" and "Colour" not in out
        stat, cpu = render(inst)
        assert stat == ofxh.Stat.OK
        np.testing.assert_allclose(cpu, EXPECTED, atol=1e-6)
