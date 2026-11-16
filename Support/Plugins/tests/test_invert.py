"""Invert: exact pixel values at each bit depth, compared with assert_close.

Pattern: an input whose values are exact at every depth, rendered at byte, short and float,
and compared with the formula. Values k/255 are exact in byte, in short (257k/65535) and in
float. hostDemo converts the input to the clip depth, and ofxhost returns the output as float
in [0, 1].
"""

import numpy as np
import pytest

from ofxhost.testing import assert_close

PLUGIN = "net.sf.openfx.invertPlugin"


def exact_image(h=16, w=24):
    """An RGB image of values k/255, k in 0..255."""
    k = np.arange(h * w * 3).reshape(h, w, 3) % 256
    return (k / 255).astype(np.float32)


@pytest.mark.parametrize("depth", ["byte", "short", "float"])
def test_exact_pixels(render, depth):
    src = exact_image()
    r = render(PLUGIN, inputs={"Source": src}, depth=depth)
    for t, out in r.outputs.items():  # hostDemo renders frames 0 and 1
        assert out.shape == (16, 24, 4)  # H x W x RGBA, top row first
        assert_close(out[..., :3], 1 - src, atol=1e-6, msg=f"frame {t}")
        # the sample inverts alpha too, and a PFM input has alpha 1
        assert_close(out[..., 3], 0.0, msg=f"alpha, frame {t}")
    assert r.report["renders"][0]["output"]["depth"] == depth


def test_report(render):
    # the report has what the host saw: the RoD (canonical coordinates: the width is
    # multiplied by the pixel aspect ratio), the output size, and statistics
    r = render(PLUGIN, inputs={"Source": exact_image()})
    par = r.report["clipPreferences"]["clips"]["Output"]["pixelAspectRatio"]
    first = r.report["renders"][0]
    assert first["regionOfDefinition"]["rod"] == pytest.approx([0, 0, 24 * par, 16])
    out = first["output"]
    assert (out["w"], out["h"], out["comps"], out["nan"]) == (24, 16, "RGBA", 0)
    assert out["max"][:3] == pytest.approx([1, 1, 1])
