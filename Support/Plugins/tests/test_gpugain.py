"""GPUGain: the CPU path of a plug-in with Metal, CUDA and OpenCL render, float RGBA only.

Pattern: hostDemo has no GPU support, so the plug-in renders on the CPU, and the plug-in
declares only the GPU paths it was built with (none for CUDA and OpenCL in the default
builds). r, g and b are scaled by scale * scaleR (G, B); alpha by scaleA only. Unchecking
scaleComponents ignores the per-component scales.
"""

import numpy as np

from ofxhost.testing import assert_close

PLUGIN = "com.OpenFXSample.GPUGain"
FILTER = "OfxImageEffectContextFilter"

SRC = np.random.default_rng(1).random((16, 24, 4), dtype=np.float32)


def _corner(r):
    """GPUGain doesn't support multiple resolutions: its output is the project frame, the input padded with
    zeros. The input's corner (top-down, as SRC), after checking that the padding is 0."""
    out = r.output[::-1]  # bottom-up: the input is at the origin
    padding = np.ones(out.shape[:2], bool)
    padding[:16, :24] = False
    assert out.shape[:2] == (576, 720) and np.all(out[padding] == 0.)
    return out[:16, :24][::-1]


def test_describe(describe):
    d = describe(PLUGIN)
    assert d["plugin"]["grouping"] == "OFX Example (Support)"
    ctx = d["contexts"][FILTER]
    assert ctx["flags"]["cpuRenderSupported"] == "true"
    clips = {c["name"]: c for c in ctx["clips"]}
    assert clips["Output"]["supportedComponents"] == ["OfxImageComponentRGBA"]


def test_gain_float(render):
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float",
               params={"scale": 0.5, "scaleR": 2.0, "scaleG": 0.25, "scaleB": 4.0, "scaleA": 0.5})
    expected = SRC * np.array([0.5 * 2.0, 0.5 * 0.25, 0.5 * 4.0, 0.5], np.float32)
    assert_close(_corner(r), expected, atol=1e-6)


def test_components_off(render):
    # scaleComponents off: only scale, on r, g and b
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float",
               params={"scale": 3.0, "scaleR": 2.0, "scaleA": 0.5, "scaleComponents": False})
    assert_close(_corner(r), SRC * np.array([3.0, 3.0, 3.0, 1.0], np.float32), atol=1e-5)


def test_default_is_identity(render):
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float")
    identity = r.report["renders"][0]["isIdentity"]
    assert identity["identity"] and identity["clip"] == "Source"
    assert_close(_corner(r), SRC)
