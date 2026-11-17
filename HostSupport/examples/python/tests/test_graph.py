"""Tests of ofxhost.graph: building, loading and executing plug-in graphs."""

import json
from pathlib import Path

import numpy as np
import pytest

import ofxhost
from ofxhost import graph
from ofxhost.graph import Node, execute, load, topological_order
from ofxhost.testing import assert_close

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"


def rgba(h=48, w=64):
    """An RGBA gradient (top-down): r along x, g along y, b constant, alpha along x."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25), 1 - x / w]).astype(np.float32)


def test_topological_order():
    a = Node(INVERT, name="a", inputs={"Source": "ramp"})
    b = Node(INVERT, name="b", inputs={"Source": a})
    c = Node(BASIC, "general", name="c", inputs={"Source": b, "Mask": a})
    assert topological_order(c) == [a, b, c]  # a once, although two nodes use it
    a.inputs["Source"] = c
    with pytest.raises(ValueError, match="cycle"):
        topological_order(c)


def test_load(tmp_path):
    (tmp_path / "g.json").write_text(json.dumps({
        "output": "c",
        "nodes": {
            "a": {"plugin": INVERT, "inputs": {"Source": "file:in.pfm"}},
            "c": {"plugin": BASIC, "context": "general", "params": {"scale": 2},
                  "inputs": {"Source": {"node": "a"}, "Mask": "none"}},
        }}))
    out, nodes = load(tmp_path / "g.json")
    assert out is nodes["c"] and out.context == "general" and out.params == {"scale": 2}
    assert out.inputs == {"Source": nodes["a"], "Mask": "none"}
    assert nodes["a"].inputs["Source"] == "file:" + str(tmp_path / "in.pfm")
    (tmp_path / "bad.json").write_text(json.dumps({
        "output": "a", "nodes": {"a": {"plugin": INVERT, "inputs": {"Source": {"node": "x"}}}}}))
    with pytest.raises(ValueError, match="unknown node 'x'"):
        load(tmp_path / "bad.json")


def test_chain(hostdemo, sample_plugins):
    """Invert(Invert(src)) is src, at a render scale and in a window, alpha included: each
    output is passed on with its bounds, alpha and premultiplication."""
    src = rgba()
    inv2 = Node(INVERT, inputs={"Source": Node(INVERT, inputs={"Source": src})})
    for scale, window in ((1.0, None), (0.5, (10, 5, 40, 30))):
        out = execute(inv2, sample_plugins, frames=[0, 0.5], scale=scale, window=window,
                      host_binary=hostdemo)
        assert sorted(out) == [0.0, 0.5]
        for t, img in out.items():
            x1, y1, x2, y2 = window or (0, 0, 64, 48)
            assert img.bounds == (x1, y1, x2, y2), (t, img.bounds)
            # top-down arrays: the window's rows, from the top of the 48-pixel source
            assert_close(img.pixels, src[48 - y2:48 - y1, x1:x2], atol=1e-6, msg=f"t={t}")


def test_dag(hostdemo, sample_plugins, monkeypatch):
    """A node used by two inputs is rendered once per time. The basic plug-in scales its
    Source by scale where its Mask is 1: Mask = Source = Invert(solid 0.25) = 0.75."""
    calls = []

    def counting_render(*args, **kw):
        calls.append((args[1], kw.get("query", False)))
        return ofxhost.render(*args, **kw)

    monkeypatch.setattr(graph, "render", counting_render)
    inv = Node(INVERT, inputs={"Source": "solid:0.25,0.25,0.25,0.25"})
    out = Node(BASIC, "general", {"scale": 2}, inputs={"Source": inv, "Mask": inv})
    img = execute(out, sample_plugins, frames=[0], host_binary=hostdemo)[0]
    # the RoD of each node (inputs first), the RoIs (outputs first), then one render per node
    assert calls == [(INVERT, True), (BASIC, True), (BASIC, True), (INVERT, False), (BASIC, False)]
    # alpha 0.75 inverted from 0.25; the colour is scaled by 1 + (2 - 1) * mask alpha
    assert_close(img.pixels[..., 3], np.full(img.pixels.shape[:2], 0.75, np.float32), atol=1e-6)
    assert_close(img.pixels[..., 0], np.full(img.pixels.shape[:2], 0.75 * 1.75, np.float32), atol=1e-6)


def test_file_spec_paths(tmp_path, monkeypatch):
    """The alpha file of a "file:rgb.pfm+alpha.pfm@x,y#premult" spec is made absolute too."""
    from ofxhost import resolve_file_spec
    monkeypatch.chdir(tmp_path)
    assert resolve_file_spec("file:a.pfm+b.pfm@3,-4#premult") == \
        f"file:{tmp_path.resolve() / 'a.pfm'}+{tmp_path.resolve() / 'b.pfm'}@3,-4#premult"
    assert resolve_file_spec("file:sub/a.pfm", "/base") == "file:" + str(Path("/base/sub/a.pfm").resolve())
    # an alpha file with another extension, and '@', '#' and '+' in names
    assert resolve_file_spec("file:x@1.pfm+a#b.dat@0,0", "/base") == \
        "file:" + str(Path("/base/x@1.pfm").resolve()) + "+" + str(Path("/base/a#b.dat").resolve()) + "@0,0"


def test_frames_ranges():
    assert graph._frames([[1.5, 1.5], [2, 2]]) == [1.5, 2.0]
    with pytest.raises(NotImplementedError):
        graph._frames([[0.5, 1.5]])


def test_load_errors(tmp_path):
    (tmp_path / "g.json").write_text(json.dumps({"output": "a", "nodes": {"a": {"inputs": {}}}}))
    with pytest.raises(ValueError, match="plugin"):
        load(tmp_path / "g.json")
    (tmp_path / "h.json").write_text(json.dumps(
        {"output": "a", "nodes": {"a": {"plugin": INVERT, "inputs": {"Source": [1, 2]}}}}))
    with pytest.raises(ValueError, match="Source"):
        load(tmp_path / "h.json")


def test_inputs_outside_the_project(hostdemo, sample_plugins):
    """Images whose bounds are outside the project-sized default RoD are planned with their real
    RoD: Invert(Invert(img)) gives img back, with all its bounds."""
    src = rgba(10, 12)
    for bounds, window in (((1000, 0, 1012, 10), (1000, 0, 1012, 10)), ((710, 0, 722, 10), (705, 0, 722, 10))):
        img = ofxhost.Image(src, bounds)
        out = execute(Node(INVERT, inputs={"Source": Node(INVERT, inputs={"Source": img})}),
                      sample_plugins, frames=[0], window=window, host_binary=hostdemo)[0]
        assert out.bounds == bounds, (bounds, out.bounds)
        assert_close(out.pixels, src, atol=1e-6)


def test_nothing_to_render(hostdemo, sample_plugins):
    with pytest.raises(ofxhost.HostDemoError, match="renders nothing"):
        execute(Node(INVERT, inputs={"Source": "ramp"}), sample_plugins, frames=[0],
                window=(5000, 5000, 5010, 5010), host_binary=hostdemo)


def test_large_input(hostdemo, sample_plugins):
    """An input larger than the project (3000x1500, more pixels than a 2048x2048 image) keeps all
    its bounds through a chain, and a window outside the project renders."""
    h, w = 1500, 3000
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    src = np.dstack([x / w, y / h, np.full_like(x, 0.25), np.ones_like(x)]).astype(np.float32)
    inv2 = Node(INVERT, inputs={"Source": Node(INVERT, inputs={"Source": ofxhost.Image(src, (0, 0, w, h))})})
    out = execute(inv2, sample_plugins, frames=[0], window=(0, 0, w, h), host_binary=hostdemo)[0]
    assert out.bounds == (0, 0, w, h)
    assert_close(out.pixels, src, atol=1e-6)
    out = execute(inv2, sample_plugins, frames=[0], window=(1000, 1000, 1100, 1100), host_binary=hostdemo)[0]
    assert out.bounds == (1000, 1000, 1100, 1100)
    assert_close(out.pixels, src[h - 1100:h - 1000, 1000:1100], atol=1e-6)


def test_no_tiles(hostdemo, sample_plugins):
    """A node without tiles (Invert with supportsTiles off) renders its whole RoD from whole-RoD
    inputs, so its input node is rendered on its whole RoD too, whatever the window and the RoI
    (testSmallRoI: Invert asks for the centre of the region it needs only)."""
    src = rgba(40, 60)
    img = ofxhost.Image(src, (100, 50, 160, 90))
    inner = Node(INVERT, inputs={"Source": img})
    outer = Node(INVERT, inputs={"Source": inner}, params={"supportsTiles": False, "testSmallRoI": True})
    out = execute(outer, sample_plugins, frames=[0], window=(120, 60, 130, 70), host_binary=hostdemo)[0]
    assert out.bounds == (100, 50, 160, 90), out.bounds
    assert_close(out.pixels, src, atol=1e-6)
    # with tiles, the input node is rendered on the small RoI only (the centre of the window), so
    # the output is that centre (whose pixels are wrong at the borders: Invert's own small RoI).
    # (An artifact of the graph executor: the input node is passed on with its rendered bounds as
    # its RoD, so the outer RoD shrinks too; a host would render the whole window.)
    outer.params["supportsTiles"] = True
    out = execute(outer, sample_plugins, frames=[0], window=(120, 60, 130, 70), host_binary=hostdemo)[0]
    assert out.bounds == (122, 62, 128, 68), out.bounds


def test_no_tiles_huge_rod(hostdemo, sample_plugins):
    """A node without tiles whose input RoD is too large for hostDemo: the input node is asked for
    the render window only, as hostDemo fetches it."""
    inner = Node(INVERT, inputs={"Source": "rod:0,0,40000,40000"})
    outer = Node(INVERT, inputs={"Source": inner}, params={"supportsTiles": False})
    out = execute(outer, sample_plugins, frames=[0], window=(0, 0, 64, 64), host_binary=hostdemo)[0]
    assert out.bounds == (0, 0, 64, 64), out.bounds
