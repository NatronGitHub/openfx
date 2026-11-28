"""Regression tests of ofxhost.rects with non-finite bounds (#293, fixed in #294; checked against the bindings
in #341): a plug-in's infinite or NaN RoD or RoI made the in-process backend and the sample host raise, where
hostDemo clamps."""

import math

import pytest

from ofxhost import rects
from ofxhost.testing import check_backend

inf, nan = math.inf, math.nan
BIG = (rects.INT_MIN, 0, rects.INT_MAX, 0)  # hostDemo's clamped rectangle of (-inf, nan, inf, nan)
RECTS = {"finite": (0.5, -0.5, 10.2, 768 / (768 / 720)), "finite-large": (-3.25, 2., 1e9, 7.999999),
         "infinite-nan": (-inf, nan, inf, nan), "huge-inverted": (1e300, -1e300, -inf, inf),
         "empty": (0., 0., 0., 0.)}
SCALES = {"rs1-par1": ((1., 1.), 1.), "rs0.5-par2": ((0.5, 0.5), 2.), "rs0.5,0.25-par2": ((0.5, 0.25), 2.),
          "rs1/3,3-par0.9": ((1 / 3, 3.), 0.9)}
# the pixel rectangles that hostDemo gives (its epsilon for the finite bounds), stated explicitly
EXPECTED = {("finite", "rs1-par1"): (0, -1, 11, 720), ("infinite-nan", "rs1-par1"): BIG,
            ("infinite-nan", "rs0.5-par2"): BIG}


@pytest.mark.parametrize("rect, scale", list(EXPECTED), ids=[f"{r}-{s}" for r, s in EXPECTED])
def test_to_pixels_hostdemo_values(rect, scale):
    """rects.to_pixels(), which the in-process backend and the sample host use, gives hostDemo's clamped pixel
    rectangle of an infinite or NaN RoD instead of raising, and the enclosing rectangle of finite bounds (with
    hostDemo's epsilon): the values stated in EXPECTED. Pure Python: it runs without ofxh (#294)."""
    (rs, par) = SCALES[scale]
    assert rects.to_pixels(RECTS[rect], rs, par) == EXPECTED[rect, scale]


@pytest.mark.parametrize("scale", SCALES)
@pytest.mark.parametrize("rect", RECTS)
def test_to_pixels_matches_the_bindings(rect, scale):
    """rects.to_pixels() gives what the bindings give (ofxh.canonical_to_pixel, hostDemo's code), for finite,
    large, infinite, NaN, inverted and empty bounds at several render scales and PARs (#341)."""
    check_backend("inprocess")  # (the bindings: skipped without ofxh, failed with OFXH_REQUIRE=1)
    import ofxh
    r, (rs, par) = RECTS[rect], SCALES[scale]
    assert rects.to_pixels(r, rs, par) == tuple(ofxh.canonical_to_pixel(r, rs, par)), (r, rs, par)


def test_intersect_with_a_nan_bound():
    """A NaN RoI bound gives the RoD's in rects.intersect(), as hostDemo's maxD/minD, instead of a NaN rectangle
    (#294)."""
    assert rects.intersect((nan, 0., nan, 5.), (-1., -2., 3., 4.)) == (-1., 0., 3., 4.)
