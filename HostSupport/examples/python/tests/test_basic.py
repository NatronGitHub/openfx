"""Tests of ofxhost, rendering the Support/Plugins samples with hostDemo."""

import sys

import numpy as np
import pytest

import ofxhost
from ofxhost.testing import assert_close

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"


def gradient(h=48, w=64):
    """An RGB gradient: r varies along x, g along y (top row 0), b constant."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25)]).astype(np.float32)


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
    # big-endian (positive scale) files are read too
    be = b"Pf\n7 5\n1.0\n" + np.ascontiguousarray(grey[::-1], dtype=">f4").tobytes()
    assert np.array_equal(ofxhost.parse_pfm(be), grey)
    with pytest.raises(ValueError):
        ofxhost.parse_pfm(data[:-1])  # truncated
    with pytest.raises(ValueError):
        ofxhost.parse_pfm(data + b"\0")  # trailing data


def test_stats_line():
    s = ofxhost.parse_stats_line(
        "stats frame=1 view=0 w=720 h=576 bounds=0,0,720,576 depth=byte comps=RGBA "
        "min=191,127,64,127 max=191,127,64,127 mean=191,127,nan,127.5 nan=0 crc32=ba5f4e88")
    assert s["frame"] == 1 and s["bounds"] == [0, 0, 720, 576] and s["comps"] == "RGBA"
    assert s["min"] == [191, 127, 64, 127] and s["mean"] == [191, 127, None, 127.5]
    assert s["crc32"] == "ba5f4e88"


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
    assert sorted(r.outputs) == [0, 1]
    for t, out in r.outputs.items():
        assert out.shape == (48, 64, 4)
        assert_close(out[..., :3], 1 - src, atol=1e-6, msg=f"frame {t}")
        # the sample Invert inverts every component, alpha included: a PF input has
        # alpha 1, so the output alpha is 0
        assert_close(out[..., 3], 0.0, msg=f"alpha, frame {t}")
        assert np.array_equal(r.alpha[t], out[..., 3])
    assert [s["frame"] for s in r.stats] == [0, 1]
    s = r.stats[0]
    assert (s["w"], s["h"], s["depth"], s["comps"], s["nan"]) == (64, 48, "float", "RGBA", 0)


def test_invert_byte(hostdemo, sample_plugins):
    src = np.round(gradient() * 255) / 255
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": src}, depth="byte",
                       frames=[0], host_binary=hostdemo)
    assert list(r.outputs) == [0]
    assert r.stats[0]["depth"] == "byte"
    out = r.outputs[0]
    # byte values are converted to float in [0,1]: k/255
    assert_close(out * 255, np.round(out * 255), atol=1e-3)
    assert_close(out[..., :3], 1 - src, atol=1.01 / 255)
    assert_close(out[..., 3], 0.0)


def test_disconnected_optional_mask(hostdemo, sample_plugins):
    # the basic plug-in (general context) scales the Source by scale, modulated by its Mask:
    # with the Mask disconnected, every pixel is scaled
    r = ofxhost.render(sample_plugins, BASIC, context="general",
                       inputs={"Source": "solid:0.5,0.25,0.125,1", "Mask": "none"},
                       params={"scale": 2.0}, depth="float", frames=[0], host_binary=hostdemo)
    out = r.outputs[0]
    assert out.shape == (576, 720, 4)
    assert_close(out, np.array([1.0, 0.5, 0.25, 1.0]), atol=1e-6)


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
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": src}, frames=[0],
                       host_binary=hostdemo)
    out = r.outputs[0]
    assert_close(out[0, :, :3], 0.0, atol=1e-6, msg="top row")
    assert out[1:, :, :3].min() >= 0.5 - 1e-6
    assert_close(out[..., :3], 1 - src, atol=1e-6)
    # g grows downwards in the input, so 1-g decreases downwards (below the marked row)
    assert out[1, 0, 1] > out[-1, 0, 1]


FAKE_HOST = '''\
import sys
args = sys.argv[1:]
if args == ["-h"]:
    print("Usage: hostDemo [--stats] [--report out.json] [--describe out.json]")
    sys.exit(0)
if "--describe" in args:
    open(args[args.index("--describe") + 1], "w").write('{"plugin": "fake"}')
    sys.exit(0)
open(args[args.index("--report") + 1], "w").write('{"regionOfDefinition": [0, 0, 2, 1]}')
with open("Output.0.pfm", "wb") as f:  # a 2x1 Pf (Alpha) image
    f.write(b"Pf\\n2 1\\n-1.0\\n" + bytes(4) + bytes([0, 0, 0x80, 0x3f]))
print("stats frame=0 view=0 w=2 h=1 bounds=0,0,2,1 depth=float comps=Alpha "
      "min=0 max=1 mean=0.5 nan=0 crc32=00000000")
'''


@pytest.mark.skipif(sys.platform == "win32", reason="needs an executable script")
def test_report_detection(tmp_path):
    # a fake hostDemo whose -h lists --report and --describe
    host = tmp_path / "fakeHostDemo"
    host.write_text(f"#!{sys.executable}\n" + FAKE_HOST)
    host.chmod(0o755)
    assert {"--report", "--describe", "--stats"} <= ofxhost.host_features(host)
    r = ofxhost.render(tmp_path, "fake", host_binary=host)
    assert r.report == {"regionOfDefinition": [0, 0, 2, 1]}
    assert "--report" in r.cmd
    assert r.outputs[0].shape == (1, 2, 1) and r.outputs[0][0, :, 0].tolist() == [0.0, 1.0]
    assert r.stats[0]["comps"] == "Alpha" and r.stats[0]["mean"] == [0.5]
    assert ofxhost.describe(tmp_path, "fake", host_binary=host) == {"plugin": "fake"}


def test_no_report_without_support(hostdemo, sample_plugins):
    if "--report" in ofxhost.host_features(hostdemo):
        pytest.skip("this hostDemo supports --report")
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": "ramp"}, frames=[0],
                       host_binary=hostdemo)
    assert r.report is None and "--report" not in r.cmd
