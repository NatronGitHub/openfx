"""The minimal host (HostSupport/examples/python/minimal_host.py) renders exactly as hostDemo."""

import numpy as np
import pytest

import minimal_host
import ofxhost

INVERT = "net.sf.openfx.invertPlugin"


def _source(seed=1, shape=(48, 64, 4)):
    """A float32 RGBA image in [0, 1), top-down (ofxhost's convention)."""
    return np.random.default_rng(seed).random(shape, dtype=np.float32)


def _to_depth(bottom_up, depth):
    """hostDemo's conversion of its PFM inputs to the clip depth (hostDemoClipInstance.cpp, MyImage)."""
    if depth == "byte":
        return np.floor(np.clip(bottom_up.astype(np.float64), 0., 1.) * 255. + 0.5).astype(np.uint8)
    return np.ascontiguousarray(bottom_up, dtype=np.float32)


def _to_float(pixels):
    """hostDemo's conversion of its outputs to PFM (MyImage::getComponentFloat)."""
    if pixels.dtype == np.uint8:
        return pixels.astype(np.float32) / np.float32(255.)
    return pixels


@pytest.mark.parametrize("depth", ["byte", "float"])
def test_invert_matches_hostdemo(plugin_dir, hostdemo, depth):
    src = _source()
    r = ofxhost.render(plugin_dir, INVERT, inputs={"Source": src}, depth=depth, frames=[0],
                       host_binary=hostdemo)
    expected = next(iter(r.outputs.values()))  # float32, top-down (the first view)

    out = minimal_host.render([plugin_dir], INVERT, {"Source": _to_depth(src[::-1], depth)})
    assert out.bounds == (0, 0, 64, 48)
    assert out.pixels.dtype == (np.uint8 if depth == "byte" else np.float32)
    got = _to_float(out.pixels)[::-1]  # bottom-up -> top-down
    np.testing.assert_array_equal(got, expected)  # bit-identical (atol=0)


def test_negative_row_stride_input(plugin_dir):
    """An input whose rows go down in memory (kOfxImagePropRowBytes < 0) renders as a contiguous one."""
    bottom_up = _to_depth(_source(seed=2)[::-1], "float")
    top_down = np.ascontiguousarray(bottom_up[::-1])
    view = top_down[::-1]  # bottom-up again, with a negative row stride
    assert view.strides[0] < 0
    a = minimal_host.render([plugin_dir], INVERT, {"Source": bottom_up})
    b = minimal_host.render([plugin_dir], INVERT, {"Source": view})
    np.testing.assert_array_equal(a.pixels, b.pixels)
    np.testing.assert_array_equal(a.pixels[..., :3], 1. - bottom_up[..., :3])
