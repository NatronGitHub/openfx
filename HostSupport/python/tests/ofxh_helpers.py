"""Shared helpers of the ofxh tests (HostSupport/python/tests): the ofxh module, and the GPU
render APIs (OpenCL, Metal, CUDA) of the test_opencl, test_metal, test_cuda and test_gpu_apis modules.

conftest.py imports it (pytest puts this directory on sys.path), and so do the test modules, after their
pytest.importorskip("ofxh").
"""

import contextlib
import dataclasses
import os
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[3]
# minimal_host.py and the ofxhost package
sys.path.insert(0, str(REPO_ROOT / "HostSupport" / "examples" / "python"))

from ofxhost.testing import _skip_or_fail  # noqa: E402  (internal to openfx's suites: the OFXH_REQUIRE rule)

# OFXH_REQUIRE=1 (CI): fail instead of skipping when the module, the plug-ins or hostDemo are missing
REQUIRE = os.environ.get("OFXH_REQUIRE") == "1"

try:
    import ofxh  # PYTHONPATH: the build directory of the module
except ImportError:
    if REQUIRE:
        raise
    ofxh = None


# --- the GPU render APIs --------------------------------------------------------------------------------------------

GPUGAIN = "com.OpenFXSample.GPUGain"  # renders with each GPU API it was built with, and on the CPU
INVERT = "net.sf.openfx.invertPlugin"  # no GPU render
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault) if ofxh is not None else ()
WINDOW = (0, 0, 24, 16)
SOURCE = np.random.default_rng(1).random((16, 24, 4), dtype=np.float32)
# GPUGain with scale 0.5: r, g and b times 0.5, alpha unchanged (scaleA is 1)
EXPECTED = SOURCE * np.array([0.5, 0.5, 0.5, 1.], np.float32)
kCPU = "OfxImageEffectPropCPURenderSupported"


@dataclasses.dataclass(frozen=True)
class GpuApi:
    """What the tests need to know of a GPU API of ofxh (its init()/render_action() argument is its name)."""

    name: str  # "opencl", "metal" or "cuda": the keyword of init(), render_mode() and the actions
    label: str  # its name in ofxh's messages ("doesn't render with OpenCL")
    support: str  # the plug-in and host descriptor property of its buffers (memory) path
    other: str | None  # the property of its other path (OpenCL Images, Metal textures), or None
    prefer: str | None  # the render_mode() argument that prefers that other path
    mode: str | bool  # the actions' argument for a render with its buffers ("buffers", or True for CUDA)
    cpu_modes: tuple  # the arguments that a plug-in without that API refuses
    plugin_flag: str  # what builds GPUGain with it
    require: str  # the variable that makes "no device" a failure

    def kw(self, mode=None):
        """The keyword argument of an action rendering with this API (mode: one of cpu_modes, default: buffers)."""
        return {self.name: self.mode if mode is None else mode}

    def result(self, inst):
        """inst's <name>_result: "ok", "retry", "failed", "not_sent", or "" (nothing sent with this API)."""
        return getattr(inst, f"{self.name}_result")


GPU_APIS = {a.name: a for a in (
    GpuApi("opencl", "OpenCL", "OfxImageEffectPropOpenCLRenderSupported", "OfxImageEffectPropOpenCLSupported",
           "prefer_images", "buffers", ("buffers", "images"), "OFX_SUPPORTS_OPENCLRENDER", "OFXH_REQUIRE"),
    GpuApi("metal", "Metal", "OfxImageEffectPropMetalRenderSupported", "OfxImageEffectPropMetalTextureSupported",
           "prefer_textures", "buffers", ("buffers", "textures"), "OFX_EXAMPLE_GPUGAIN_METAL", "OFXH_REQUIRE"),
    GpuApi("cuda", "CUDA", "OfxImageEffectPropCudaRenderSupported", None, None, True, (True,),
           "OFX_SUPPORTS_CUDARENDER", "OFXH_REQUIRE_CUDA"),
)}


@contextlib.contextmanager
def gpu_device(api, plugin_dir):
    """ofxh initialized with a device of api (init(<name>=True)); gives <name>_device(). Skips (fails with
    api.require=1) if ofxh was built without the API or there is no device; shuts down after."""
    if not ofxh.build_config.get(f"OFX_HOSTSUPPORT_{api.name.upper()}"):
        _skip_or_fail(f"ofxh was built without OFX_HOSTSUPPORT_{api.name.upper()}", api.require)
    import minimal_host  # (HostSupport/examples/python, which conftest.py puts on sys.path)

    ofxh.init(minimal_host.Host(), [plugin_dir], use_env_path=False, **{api.name: True})
    try:
        d = getattr(ofxh, f"{api.name}_device")()
        if not d or "error" in d:
            _skip_or_fail(f"no {api.label} device: {(d or {}).get('error')}", api.require)
        yield d
    finally:
        ofxh.shutdown()


def instance(plugin_id, inputs):
    """An instance in the filter context of minimal_host.Instance(inputs), with its clip preferences; GPUGain gets
    scale 0.5."""
    import minimal_host  # (HostSupport/examples/python, which conftest.py puts on sys.path)

    plugin = ofxh.get_plugin(plugin_id)
    if plugin is None:
        _skip_or_fail(f"plug-in {plugin_id} not found")
    inst = ofxh.create_instance(plugin, "filter", minimal_host.Instance(inputs))
    assert inst.create_instance_action() in OK
    assert inst.get_clip_preferences_action() in OK
    if plugin_id == GPUGAIN:
        assert inst.set_param("scale", 0.5) in OK
    return inst


def gpu_instance(api, inputs):
    """instance(GPUGAIN, inputs), skipped (failed with api.require=1) if GPUGain was built without api."""
    inst = instance(GPUGAIN, inputs)
    if inst.props[api.support][0] != "true":
        inst.close()
        _skip_or_fail(f"GPUGain was built without {api.label} ({api.plugin_flag})", api.require)
    return inst


def render(inst, **kw):
    """Begin/Render/EndSequenceRender of WINDOW, with the GPU keyword argument kw (none: on the CPU); returns (render
    status, output pixels or None)."""
    assert inst.begin_sequence_render_action(0., 0., **kw) in OK
    stat, outputs = inst.render_action(0., WINDOW, **kw)
    assert inst.end_sequence_render_action(0., 0., **kw) in OK
    out = outputs.get("Colour")
    return stat, (np.array(out.pixels) if out is not None else None)
