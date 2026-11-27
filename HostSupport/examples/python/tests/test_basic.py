"""Tests of ofxhost, rendering the Support/Plugins samples with hostDemo."""

import numpy as np
import pytest

import ofxhost
from ofxhost.testing import assert_close, views

INVERT = "net.sf.openfx.invertPlugin"


def gradient(h=48, w=64):
    """An RGB gradient: r varies along x, g along y (top row 0), b constant."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25)]).astype(np.float32)


def test_to_pixels_clamps_infinite_and_nan_bounds():
    """A plug-in's infinite or NaN RoD gives the clamped pixel rectangle, as hostDemo, instead of raising."""
    from ofxhost import rects
    inf, nan = float("inf"), float("nan")
    big = (rects.INT_MIN, 0, rects.INT_MAX, 0)
    assert rects.to_pixels((-inf, nan, inf, nan), (1., 1.), 1.) == big
    assert rects.to_pixels((-inf, nan, inf, nan), (0.5, 0.5), 2.) == big
    # finite bounds: the enclosing rectangle, with hostDemo's epsilon
    assert rects.to_pixels((0.5, -0.5, 10.2, 768 / (768 / 720)), (1., 1.), 1.) == (0, -1, 11, 720)
    # a NaN RoI bound gives the RoD's, as hostDemo's maxD/minD
    assert rects.intersect((nan, 0., nan, 5.), (-1., -2., 3., 4.)) == (-1., 0., 3., 4.)


def test_pfm_round_trip(tmp_path):
    rgb = np.random.default_rng(0).random((5, 7, 3), dtype=np.float32)
    grey = rgb[..., 0].copy()
    ofxhost.write_pfm(tmp_path / "rgb.pfm", rgb)
    ofxhost.write_pfm(tmp_path / "grey.pfm", grey)
    assert np.array_equal(ofxhost.read_pfm(tmp_path / "rgb.pfm"), rgb)
    assert np.array_equal(ofxhost.read_pfm(tmp_path / "grey.pfm"), grey)
    data = (tmp_path / "rgb.pfm").read_bytes()
    assert data.startswith(b"PF\n7 5\n-1.0\n")
    # rows are bottom-up in the file: the first pixel is the bottom-left one
    first = np.frombuffer(data, dtype="<f4", count=3, offset=len(b"PF\n7 5\n-1.0\n"))
    assert np.array_equal(first, rgb[-1, 0])
    # big-endian (positive scale) files are read too, and NaN; not with hostDemo's rules (strict)
    be = b"Pf\n7 5\n1.0\n" + np.ascontiguousarray(grey[::-1], dtype=">f4").tobytes()
    assert np.array_equal(ofxhost.parse_pfm(be), grey)
    with pytest.raises(ValueError, match="only little-endian"):
        ofxhost.parse_pfm(be, strict=True)
    nan = ofxhost.encode_pfm(np.full((2, 3), np.nan, np.float32))
    assert np.isnan(ofxhost.parse_pfm(nan)).all()
    with pytest.raises(ValueError, match=r"pixel \(0,0\).*is not finite"):
        ofxhost.parse_pfm(nan, strict=True)
    with pytest.raises(ValueError):
        ofxhost.parse_pfm(data[:-1])  # truncated
    with pytest.raises(ValueError):
        ofxhost.parse_pfm(data + b"\0")  # trailing data


def test_assert_close_message():
    a = np.zeros((2, 3, 4), np.float32)
    b = a.copy()
    b[1, 2, 3] = 0.5
    assert_close(a, a)
    with pytest.raises(AssertionError, match=r"(?s)1 of 24 values differ.*at \(1, 2, 3\)"):
        assert_close(a, b, atol=0.1)
    b[0, 0, 0] = np.nan
    with pytest.raises(AssertionError, match="NaN in actual only: 0, in expected only: 1"):
        assert_close(a, b, atol=1)


def test_invert_float(hostdemo, sample_plugins):
    src = gradient()
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": src}, depth="float",
                       host_binary=hostdemo)
    v = views()
    assert list(r.outputs) == v.keys(0) + v.keys(1)
    for t, out in r.outputs.items():
        assert out.shape == (48, 64, 4)
        assert_close(out[..., :3], 1 - src, atol=1e-6, msg=f"frame {t}")
        # the sample Invert inverts every component, alpha included: a PF input has
        # alpha 1, so the output alpha is 0
        assert_close(out[..., 3], 0.0, msg=f"alpha, frame {t}")
        assert np.array_equal(r.alpha[t], out[..., 3])
    assert len(r.report["renders"]) == 2 * v.count
    for x in r.report["renders"]:
        o = x["output"]
        assert (o["w"], o["h"], o["depth"], o["comps"], o["nan"]) == (64, 48, "float", "RGBA", 0)


def test_mandatory_clip_none_fails(hostdemo, sample_plugins):
    with pytest.raises(ofxhost.HostDemoError, match="not optional") as e:
        ofxhost.render(sample_plugins, INVERT, inputs={"Source": "none"},
                       host_binary=hostdemo)
    assert e.value.returncode != 0
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": "none"},
                       host_binary=hostdemo, check=False)
    assert r.returncode != 0 and "not optional" in r.stderr


def test_orientation(hostdemo, sample_plugins):
    # a gradient with a marked (white) top row: inverted, the top row is black
    src = gradient() * 0.5
    src[0] = 1.0
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": src},
                       host_binary=hostdemo)
    for key in views().keys(0):
        out = r.outputs[key]
        assert_close(out[0, :, :3], 0.0, atol=1e-6, msg="top row")
        assert out[1:, :, :3].min() >= 0.5 - 1e-6
        assert_close(out[..., :3], 1 - src, atol=1e-6)
        # g grows downwards in the input, so 1-g decreases downwards (below the marked row)
        assert out[1, 0, 1] > out[-1, 0, 1]


def test_keys_and_buttons(hostdemo, sample_plugins):
    """Keys ({time: value}) are interpolated linearly by hostDemo; None presses a push button."""
    src = np.full((8, 8, 4), 0.5, np.float32)
    src[..., 3] = 1
    r = ofxhost.render(sample_plugins, "net.sf.openfx.basicPlugin", "general",
                       inputs={"Source": src, "Mask": "none"}, params={"scale": {0: 1., 10: 2.}},
                       frames=[0, 5], host_binary=hostdemo)
    assert r.report["params"]["scale"]["keys"] == [[0, 1], [10, 2]]
    v = views()
    assert [x["params"]["scale"] for x in r.report["renders"]] == [1] * v.count + [1.5] * v.count
    for key in v.keys(5):
        assert_close(r.images[key].pixels[..., 0], np.full((8, 8), 0.75, np.float32), atol=1e-6)
    r = ofxhost.render(sample_plugins, "net.sf.openfx.GenericTestPlugin", params={"keysTest": None},
                       frames=[0], query=True, host_binary=hostdemo)
    assert [m["text"] for m in r.report["messages"]] == ["Keys test passed"]


@pytest.mark.parametrize("ofx14", [False, True])
def test_views_reported(hostdemo, sample_plugins, ofx14):
    """The views the host reports to the plug-in (the Nuke extension's getViewCount and getViewName, which Tester's
    viewsTest sends) are the views it renders: left and right in Nuke builds; the name of the view after the last is
    kOfxStatErrBadIndex (which Support throws). With --ofx14 the host has no plane suite, and Support's fallback is a
    single view, "Main" (and "" for another index)."""
    import ofxh
    if not ofxh.build_config.get("OFX_EXTENSIONS_NUKE"):
        pytest.skip("getViewCount and getViewName are in the plane suite of the Nuke extension (OFX_EXTENSIONS_NUKE)")
    r = ofxhost.render(sample_plugins, "net.sf.openfx.GenericTestPlugin", params={"viewsTest": None}, frames=[0],
                       host_binary=hostdemo, extra_args=["--ofx14"] if ofx14 else [])
    assert list(r.outputs) == views(ofx14).keys(0) == ([0] if ofx14 else [(0, "l"), (0, "r")])
    assert [m["text"] for m in r.report["messages"]] == \
        ['views: Main; view 1: ""' if ofx14 else "views: left, right; view 2: kOfxStatErrBadIndex"]

