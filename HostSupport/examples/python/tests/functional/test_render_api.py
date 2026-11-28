"""ofxhost's render API (ofxhost.render, the RenderResult) on both backends, its PFM reader and writer, and the
assert_close helper of ofxhost.testing, which the plug-in repositories' tests use."""

import numpy as np
import pytest

import ofxhost
from ofxhost.testing import assert_close, views

INVERT = "net.sf.openfx.invertPlugin"


def gradient(h=48, w=64):
    """An RGB gradient: r varies along x, g along y (top row 0), b constant."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25)]).astype(np.float32)


def test_pfm_round_trip(tmp_path):
    """ofxhost's PFM files (write_pfm, read_pfm, encode_pfm, parse_pfm): RGB and grey round trips, rows
    bottom-up in the file, big-endian files and NaN read, both refused in strict mode (hostDemo's rules), and
    truncated or trailing data refused. The consumers' tests read and write hostDemo's images with it (#118)."""
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
    """ofxhost.testing.assert_close's failure message: the number of values that differ, where the worst one
    is, and NaN on one side only, so that a consumer's failing test says what differs (#118)."""
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
    """The RenderResult contract on both backends: outputs keyed by the views of frames 0 and 1, the alpha
    dict, the report's renders (one per view and frame) and their output records; Invert's pixels are 1 - src
    and alpha 0 (#118)."""
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


def test_orientation(hostdemo, sample_plugins):
    """Arrays are top row first through a render, on both backends (the input written to PFM bottom-up by
    ofxhost, the output read back): a marked top row stays the top row (#118)."""
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


def test_keys(hostdemo, sample_plugins):
    """Keys ({time: value}) are interpolated linearly by the host: scale 1 at t=0 and 1.5 at t=5, in the report
    (keys and the value of each render) and in the pixels (#132). (A push button, params={name: None}, is
    test_backends.py::test_buttons_and_messages.)"""
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


@pytest.mark.parametrize("ofx14", [False, True])
def test_views_reported(hostdemo, sample_plugins, ofx14):
    """The views the host reports to the plug-in (the Nuke extension's getViewCount and getViewName, which Tester's
    viewsTest sends) are the views it renders: left and right in Nuke builds; the name of the view after the last is
    kOfxStatErrBadIndex (which Support throws). With --ofx14 the host has no plane suite, and Support's fallback is a
    single view, "Main" (and "" for another index). (#372, fixed in #377.)"""
    import ofxh
    if not ofxh.build_config.get("OFX_EXTENSIONS_NUKE"):
        pytest.skip("getViewCount and getViewName are in the plane suite of the Nuke extension (OFX_EXTENSIONS_NUKE)")
    r = ofxhost.render(sample_plugins, "net.sf.openfx.GenericTestPlugin", params={"viewsTest": None}, frames=[0],
                       host_binary=hostdemo, extra_args=["--ofx14"] if ofx14 else [])
    assert list(r.outputs) == views(ofx14).keys(0) == ([0] if ofx14 else [(0, "l"), (0, "r")])
    assert [m["text"] for m in r.report["messages"]] == \
        ['views: Main; view 1: ""' if ofx14 else "views: left, right; view 2: kOfxStatErrBadIndex"]
