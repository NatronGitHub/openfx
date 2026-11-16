"""Tests of ofxhost.graph: building, loading and executing plug-in graphs."""

import json

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
    assert calls == [(BASIC, True), (INVERT, False), (BASIC, False)]
    # alpha 0.75 inverted from 0.25; the colour is scaled by 1 + (2 - 1) * mask alpha
    assert_close(img.pixels[..., 3], np.full(img.pixels.shape[:2], 0.75, np.float32), atol=1e-6)
    assert_close(img.pixels[..., 0], np.full(img.pixels.shape[:2], 0.75 * 1.75, np.float32), atol=1e-6)
