"""MultiplyGPU: the CPU fallback of a GPU-capable plug-in, an exact multiply.

Pattern: hostDemo has no GPU support, so the plug-in renders on the CPU (and the Makefile
build has no CUDA or OpenCL code: describe() says so). Check the exact multiply in float, and
the rounding at byte depth (the sample rounds to the nearest value and clamps to [0, 1]).
"""

import numpy as np

import ofxhost
from ofxhost.testing import assert_close, check_backend

PLUGIN = "net.sf.openfx.MultiplyGPU"
VALUE = np.array([0.5, 2.0, 0.25, 0.5])  # an RGBA parameter: r, g, b, a factors

k = np.arange(16 * 24 * 3).reshape(16, 24, 3) * 11 % 256
SRC = (k / 255).astype(np.float32)  # exact at byte depth
SRC_RGBA = np.dstack([SRC, np.ones(SRC.shape[:2], np.float32)])  # PFM inputs have alpha 1


def test_cpu_render(describe):
    flags = describe(PLUGIN)["contexts"]["OfxImageEffectContextFilter"]["flags"]
    assert flags["cpuRenderSupported"] == "true"


def test_multiply_float(render):
    r = render(PLUGIN, inputs={"Source": SRC}, params={"value": VALUE}, depth="float")
    assert_close(r.output, SRC_RGBA * VALUE, atol=1e-6)


def test_multiply_byte(render):
    r = render(PLUGIN, inputs={"Source": SRC}, params={"value": VALUE}, depth="byte")
    expected = np.floor(np.clip(SRC_RGBA * VALUE, 0, 1) * 255 + 0.5) / 255
    assert_close(r.output, expected, atol=1e-6)


def test_multiply_rgb_input(bundles):
    """An RGB input gives an RGB image and an RGB output, as the clips support RGB.

    In-process only: per-input components, and hostDemo's inputs are RGBA.
    """
    check_backend("inprocess")
    r = ofxhost.render(bundles, PLUGIN, inputs={"Source": SRC}, params={"value": VALUE}, depth="float",
                       components={"Source": "rgb"}, backend="inprocess")
    assert r.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentRGB"
    assert r.output.shape == SRC.shape
    assert_close(r.output, SRC * VALUE[:3], atol=1e-6)
