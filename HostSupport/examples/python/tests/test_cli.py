"""Tests of the command line of the sample host (python -m ofxhost: ofxhost.cli)."""

import json
import os
import runpy
import sys

import numpy as np
import pytest

import ofxhost
from ofxhost import graph
from ofxhost.testing import assert_close, backend_available

why = backend_available("inprocess")
if why is not None:
    pytestmark = pytest.mark.skip(reason=f"ofxh: {why}")
else:
    import ofxh
    from ofxhost import cli

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"


@pytest.fixture(autouse=True)
def _cwd(tmp_path, monkeypatch):
    """In a temporary directory (PropTester writes ofxTestLog.txt there in the plug-in scan), without
    $OFX_PLUGIN_PATH, and without an ofxh host."""
    if os.environ.get("OFXH_REQUIRE") == "1" and why is not None:
        pytest.fail(f"ofxh: {why} (OFXH_REQUIRE=1)")
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("OFX_PLUGIN_PATH", raising=False)
    yield
    if why is None:
        ofxhost.inprocess.shutdown()
        ofxh.shutdown()


@pytest.fixture
def ofx(sample_plugins, capsys):
    """Run the command line with the sample plug-ins: (exit status, stdout, stderr)."""
    def run(*args):
        status = cli.main([a for d in sample_plugins for a in ("-b", str(d))] + [str(a) for a in args])
        out = capsys.readouterr()
        return status, out.out, out.err
    return run


def rgba(h=12, w=16):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25), 1 - x / w]).astype(np.float32)


def test_parse_frames():
    assert cli.parse_frames("1-3") == [1., 2., 3.] and cli.parse_frames("0-6x3") == [0., 3., 6.]
    assert cli.parse_frames("1,3,5") == [1., 3., 5.] and cli.parse_frames("1-2,10") == [1., 2., 10.]
    assert cli.parse_frames("-2--1") == [-2., -1.] and cli.parse_frames("2.5") == [2.5]
    for bad in ("a", "3-1", "1-3x0"):
        with pytest.raises(Exception, match="frame"):
            cli.parse_frames(bad)


def test_list(ofx):
    status, out, _ = ofx("list")
    assert status == 0 and f"{INVERT} 1.0  Invert  [filter]" in out.splitlines()
    status, out, _ = ofx("list", "gain")
    assert status == 0 and BASIC in out and INVERT not in out


def test_params(ofx, tmp_path):
    status, out, _ = ofx("params", BASIC, "-c", "general")
    assert status == 0 and "general context" in out and "  scale: double = 1.0" in out
    assert "clips: Source, Mask (optional), Output" in out
    status, out, _ = ofx("params", "org.openeffects.support.choiceParamsPlugin")
    assert "options: 0='red: none', 1='red: some', 2='red: lots'" in out
    # -j: a one-node graph with the defaults, which renders as it is
    status, out, _ = ofx("params", BASIC, "-c", "general", "-j")
    assert status == 0
    doc = json.loads(out)
    node = doc["nodes"]["basicPlugin"]
    assert node["plugin"] == BASIC and node["version"] == [1, 0] and node["context"] == "general"
    assert node["params"] == {"scale": 1.0, "scaleComponents": True, "scaleR": 1.0, "scaleG": 1.0, "scaleB": 1.0,
                              "scaleA": 1.0}
    assert node["inputs"] == {"Source": "default", "Mask": "none"}
    assert doc["outputs"] == {"basicPlugin": "basicPlugin.####.pfm"}
    (tmp_path / "g.json").write_text(out)
    status, out, _ = ofx("render", "g.json")
    assert status == 0 and out.split() == [str(tmp_path / f"basicPlugin.000{f}{x}.pfm")
                                           for f in (0, 1) for x in ("", ".alpha")]


def test_filter(ofx, tmp_path):
    """filter on a sequence (the project is the image size; the frames are the sequence's), with a
    parameter; then on a still image; --save-graph saves the graph it rendered, which renders the same."""
    for f in (1, 2, 3):
        np.save(tmp_path / f"in.{f:04d}.npy", rgba() * f / 4)
    status, out, err = ofx("filter", BASIC, "in.####.npy", "out/o.####.npy", "-s", "scale=2", "-s",
                           "scaleComponents=false", "--save-graph", "g.json", "--report", "r.json")
    assert status == 0, err
    assert out.split() == [str(tmp_path / "out" / f"o.000{f}.npy") for f in (1, 2, 3)]
    for f in (1, 2, 3):
        a = np.load(tmp_path / "out" / f"o.000{f}.npy")
        assert_close(a[..., :3], rgba()[..., :3] * f / 4 * 2, atol=1e-6)
        assert_close(a[..., 3], rgba()[..., 3] * f / 4, atol=1e-6)  # (scaleComponents off: alpha not scaled)
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["ok"] and r["frames"] == [1., 2., 3.] and r["project"]["size"] == [16., 12.]
    g = graph.load(tmp_path / "g.json")
    assert g.nodes["basicPlugin"].params == {"scale": 2, "scaleComponents": False}
    os.rename(tmp_path / "out", tmp_path / "first")
    status, out, _ = ofx("render", "g.json", "--frames", "2-3")
    assert status == 0 and len(out.split()) == 2
    assert np.array_equal(np.load(tmp_path / "out" / "o.0002.npy"), np.load(tmp_path / "first" / "o.0002.npy"))
    # a still image: frame 0
    ofxhost.write_pfm(tmp_path / "still.pfm", rgba()[..., :3])
    status, out, _ = ofx("filter", INVERT, "still.pfm", "inv.pfm")
    assert status == 0 and out.split() == [str(tmp_path / "inv.pfm"), str(tmp_path / "inv.alpha.pfm")]
    assert_close(ofxhost.imageio.read_image(tmp_path / "inv.pfm")[..., :3], 1 - rgba()[..., :3], atol=1e-6)


def test_render_dump(ofx, tmp_path):
    """--dump prints the graph, normalised (Graph.dumps()), and renders nothing."""
    (tmp_path / "g.json").write_text(json.dumps({"output": "a", "nodes": {"a": {"plugin": INVERT}}}))
    status, out, _ = ofx("render", "g.json", "--dump")
    assert status == 0 and out == graph.load(tmp_path / "g.json").dumps()
    assert json.loads(out)["outputs"] == {"a": None} and not list(tmp_path.glob("*.pfm"))


def test_errors(ofx, tmp_path):
    status, _, err = ofx("params", "no.such.plugin")
    assert status == 2 and "no.such.plugin not found" in err
    status, _, err = ofx("params", INVERT, "-c", "general")
    assert status == 2 and "no general context" in err
    status, _, err = ofx("filter", INVERT, "missing.####.pfm", "o.####.pfm")
    assert status == 2 and "no files" in err
    np.save(tmp_path / "in.0001.npy", rgba())
    status, _, err = ofx("filter", INVERT, "in.####.npy", "o.npy")
    assert status == 2 and "needs a frame number" in err
    status, _, err = ofx("filter", INVERT, "in.0001.npy", "o.npy", "-s", "nope=1")
    assert status == 1 and "no parameter 'nope'" in err
    status, _, err = ofx("render", "missing.json")
    assert status == 2 and "missing.json" in err
    assert cli.main(["-b", str(tmp_path / "nope"), "list"]) == 2


def test_python_m(sample_plugins, monkeypatch, capsys):
    """python -m ofxhost runs the command line."""
    monkeypatch.setattr(sys, "argv", ["ofxhost", "-b", str(sample_plugins[0]), "list", "invert"])
    with pytest.raises(SystemExit) as e:
        runpy.run_module("ofxhost", run_name="__main__")
    assert e.value.code == 0 and INVERT in capsys.readouterr().out
