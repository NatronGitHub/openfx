"""MultiBundle: two plug-ins in one bundle (multibundle.ofx.bundle).

Patterns:
- gamma: the output follows a known formula;
- dot: a plug-in whose output RoD is not the input's: check the RoD in the report, and the
  output image's size and pixels (in pixel coordinates, with the pixel aspect ratio).
"""

import numpy as np
import pytest

from ofxhost.testing import assert_close

GAMMA = "net.sf.openfx.gammaexample"
DOT = "net.sf.openfx.dotexample"


def test_gamma_formula(render):
    # each component is raised to the power scale + scale<C> (scaleR..scaleB default to 0);
    # alpha to the power scaleA (default 0): 1
    src = np.random.default_rng(1).random((16, 24, 3), dtype=np.float32)
    r = render(GAMMA, inputs={"Source": src}, params={"scale": 2.0, "scaleB": 1.0})
    out = r.output
    assert_close(out[..., :2], src[..., :2] ** 2, atol=1e-6)
    assert_close(out[..., 2], src[..., 2] ** 3, atol=1e-6)
    assert_close(out[..., 3], 1.0)


def test_dot_rod(render):
    # the dot's RoD is position +/- radius (canonical coordinates), whatever the input
    x, y, radius = 100.0, 50.0, 10.0
    src = np.zeros((48, 64, 3), np.float32)
    r = render(DOT, context="general", inputs={"Source": src},
               params={"radius": radius, "position": (x, y)})
    first = r.report["renders"][0]
    par = r.report["clipPreferences"]["clips"]["Output"]["pixelAspectRatio"]
    assert first["inputs"]["Source"]["regionOfDefinition"] == pytest.approx([0, 0, 64 * par, 48])
    assert first["regionOfDefinition"]["rod"] == pytest.approx(
        [x - radius, y - radius, x + radius, y + radius])

    # the output image covers the RoD in pixels: x is divided by the pixel aspect ratio
    x1, y1, x2, y2 = first["output"]["bounds"]
    assert (x1, x2) == (int((x - radius) / par), int(np.ceil((x + radius) / par)))
    assert (y1, y2) == (y - radius, y + radius)
    out = r.output
    assert out.shape == (y2 - y1, x2 - x1, 4)

    # value = 1 - d^2 / radius^2 inside the dot (d in pixels from the centre), 0 outside.
    # Output rows are top-down: row i is at pixel y2 - 1 - i.
    py, px = np.mgrid[y2 - 1:y1 - 1:-1, x1:x2].astype(np.float64)
    d2 = (px - x / par) ** 2 + (py - y) ** 2
    expected = np.maximum(0, 1 - d2 / radius ** 2)
    for c in range(4):  # colour (1, 1, 1, 1)
        assert_close(out[..., c], expected, atol=1e-5, msg=f"component {c}")
