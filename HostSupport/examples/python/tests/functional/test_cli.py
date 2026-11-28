"""Tests of the command line of the sample host (python -m ofxhost: ofxhost.cli, #211)."""

import json
import os
import runpy
import sys

import numpy as np
import pytest

import ofxhost
from ofxhost import graph
from ofxhost.testing import assert_close, check_backend
from ofxhost_helpers import gradient_rgba

check_backend("inprocess")  # (the whole module: skipped, or failed with OFXH_REQUIRE=1)
from ofxhost import cli  # noqa: E402

pytestmark = pytest.mark.usefixtures("tmp_cwd")

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"


@pytest.fixture(autouse=True)
def _no_env_path(monkeypatch):
    """Without $OFX_PLUGIN_PATH: the plug-ins are those of -b."""
    monkeypatch.delenv("OFX_PLUGIN_PATH", raising=False)


@pytest.fixture
def ofx(sample_plugins, capsys):
    """Run the command line with the sample plug-ins: (exit status, stdout, stderr)."""
    def run(*args):
        status = cli.main([a for d in sample_plugins for a in ("-b", str(d))] + [str(a) for a in args])
        out = capsys.readouterr()
        return status, out.out, out.err
    return run


def rgba(h=12, w=16):
    """The test image: a 12 x 16 gradient (ofxhost_helpers.gradient_rgba)."""
    return gradient_rgba(h, w)


def test_parse_frames():
    """cli.parse_frames(): ranges, steps, lists, negative and fractional frames; hostDemo's number limits refuse
    an underflow to 0 and infinities (#211; the limits: f653cace, #297)."""
    assert cli.parse_frames("1-3") == [1., 2., 3.] and cli.parse_frames("0-6x3") == [0., 3., 6.]
    assert cli.parse_frames("1,3,5") == [1., 3., 5.] and cli.parse_frames("1-2,10") == [1., 2., 10.]
    assert cli.parse_frames("-2--1") == [-2., -1.] and cli.parse_frames("2.5") == [2.5]
    assert cli.parse_frames("1e-3") == [1e-3] and cli.parse_frames("-1e-3-1e0") == [-1e-3, 1. - 1e-3]
    # hostDemo's numbers: no underflow to 0, and no infinity
    for bad in ("a", "3-1", "1-3x0", "1e-400", "0-inf", "inf", "1e400", "1-1e16"):
        with pytest.raises(Exception, match="frame"):
            cli.parse_frames(bad)


def test_list(ofx):
    """The list command: one line per plug-in (identifier, version, label, contexts), and a filter (#211)."""
    status, out, _ = ofx("list")
    assert status == 0 and f"{INVERT} 1.0  Invert  [filter]" in out.splitlines()
    status, out, _ = ofx("list", "gain")
    assert status == 0 and BASIC in out and INVERT not in out


def test_params(ofx, tmp_path):
    """The params command: the parameters as text (types, defaults, options, clips), and -j: a one-node graph
    with the defaults that renders as it is (#211)."""
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
    assert node["inputs"] == {"Source": "default"}  # (the optional Mask left out: disconnected)
    assert doc["outputs"] == {"basicPlugin": "basicPlugin.####.pfm"}
    (tmp_path / "g.json").write_text(out)
    status, out, _ = ofx("render", "g.json")
    assert status == 0 and out.split() == [str(tmp_path / f"basicPlugin.000{f}{x}.pfm")
                                           for f in (0, 1) for x in ("", ".alpha")]


def test_filter(ofx, tmp_path):
    """filter on a sequence (the project is the image size; the frames are the sequence's), with a
    parameter; then on a still image; --save-graph saves the graph it rendered, which renders the same; --report
    (#211)."""
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
    """--dump prints the graph, normalised (Graph.dumps()), and renders nothing (#211)."""
    (tmp_path / "g.json").write_text(json.dumps({"outputs": {"a": None}, "nodes": {"a": {"plugin": INVERT}}}))
    status, out, _ = ofx("render", "g.json", "--dump")
    assert status == 0 and out == graph.load(tmp_path / "g.json").dumps()
    assert json.loads(out)["outputs"] == {"a": None} and not list(tmp_path.glob("*.pfm"))


def test_errors(ofx, tmp_path, capsys):
    """A failed run is "error: ..." and exit status 1 (no traceback); a usage error is status 2: the command
    line's contract (#211)."""
    np.save(tmp_path / "in.0001.npy", rgba())
    (tmp_path / "bad.pfm").write_bytes(b"PF\n2 2\n-1\n")
    a = {"plugin": INVERT, "inputs": {"Source": {"node": "b"}}}
    b = {"plugin": INVERT, "inputs": {"Source": {"node": "a"}}}
    (tmp_path / "cycle.json").write_text(json.dumps({"outputs": {"a": None}, "nodes": {"a": a, "b": b}}))
    (tmp_path / "bad.json").write_text(json.dumps({"outputs": {"a": None}, "nodes": {"a": {"plugin": INVERT, "x": 1}}}))
    failures = [
        (("params", "no.such.plugin"), "no.such.plugin not found"),
        (("params", INVERT, "-c", "general"), "no general context"),
        (("filter", "no.such.plugin", "in.0001.npy", "o.npy"), "no.such.plugin not found"),
        (("filter", INVERT, "missing.####.pfm", "o.####.pfm"), "no files"),
        (("filter", INVERT, "in.####.npy", "o.npy"), "needs a frame number"),
        (("filter", INVERT, "in.0001.npy", "o.png"), "unsupported image format"),
        (("filter", INVERT, "bad.pfm", "o.pfm"), "bytes of pixel data"),
        (("filter", INVERT, "in.0001.npy", "o.npy", "-s", "nope=1"), "no parameter 'nope'"),
        (("filter", BASIC, "in.0001.npy", "o.npy", "-s", "scale=NaN", "--save-graph", "g.json"), "not finite"),
        (("render", "missing.json"), "missing.json"),
        (("render", "cycle.json"), "cycle"),
        (("render", "bad.json"), "unknown key 'x'"),
        (("render", "bad.json", "--dump"), "unknown key 'x'"),
    ]
    for args, message in failures:
        status, out, err = ofx(*args)
        assert status == 1 and err.startswith("error: ") and message in err and "Traceback" not in err, (args, err)
    assert not (tmp_path / "g.json").exists()  # (the NaN graph wasn't written)
    assert cli.main(["-b", str(tmp_path / "nope"), "list"]) == 1
    for args in (["render", "g.json", "--scale", "0"], ["render", "g.json", "--frames", "3-1"], ["nope"]):
        with pytest.raises(SystemExit) as e:
            cli.main(args)
        assert e.value.code == 2
    capsys.readouterr()


def test_python_m(sample_plugins, monkeypatch, capsys):
    """python -m ofxhost runs the command line (the entry point, #211)."""
    monkeypatch.setattr(sys, "argv", ["ofxhost", "-b", str(sample_plugins[0]), "list", "invert"])
    with pytest.raises(SystemExit) as e:
        runpy.run_module("ofxhost", run_name="__main__")
    assert e.value.code == 0 and INVERT in capsys.readouterr().out
