"""Tests of ofxhost.graph: building, saving and loading plug-in graphs (ofxhost.graphhost runs them)."""

import json
import os
from pathlib import Path

import numpy as np
import pytest

from ofxhost.graph import FilePath, Files, Graph, Keys, Node, Project, load, loads, topological_order

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"


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
    g = load(tmp_path / "g.json")
    out, nodes = g.output, g.nodes
    assert out is nodes["c"] and out.context == "general" and out.params == {"scale": 2}
    assert out.inputs == {"Source": nodes["a"], "Mask": "none"}
    assert g.outputs == {"c": None} and g.frames is None and g.project is None
    # paths stay as they are in the file, relative to its directory
    assert nodes["a"].inputs["Source"] == "file:in.pfm" and g.base == os.path.abspath(tmp_path)
    (tmp_path / "bad.json").write_text(json.dumps({
        "output": "a", "nodes": {"a": {"plugin": INVERT, "inputs": {"Source": {"node": "x"}}}}}))
    with pytest.raises(ValueError, match="unknown node 'x'"):
        load(tmp_path / "bad.json")


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


def test_load_errors(tmp_path):
    (tmp_path / "g.json").write_text(json.dumps({"output": "a", "nodes": {"a": {"inputs": {}}}}))
    with pytest.raises(ValueError, match="plugin"):
        load(tmp_path / "g.json")
    (tmp_path / "h.json").write_text(json.dumps(
        {"output": "a", "nodes": {"a": {"plugin": INVERT, "inputs": {"Source": [1, 2]}}}}))
    with pytest.raises(ValueError, match="Source"):
        load(tmp_path / "h.json")


def _graph():
    """A graph with every kind of node, input and parameter that JSON holds."""
    src = Node(INVERT, inputs={"Source": "ramp"}, version=(1, 0))
    seq = Node(INVERT, name="seq", inputs={"Source": Files("in.####.pfm")})
    out = Node(BASIC, "general", {"scale": Keys({0: 1.5, 1.0: 2}), "scaleComponents": True,
                                  "scaleR": {0.5: 0.25}, "scaleG": 0.75},
               inputs={"Source": src, "Mask": "none"}, name="out")
    g = Graph.of(out, seq, frames=[0, 1], scale=0.5, window=(4, 2, 30, 20), depth="short")
    g.outputs["out"] = "out.####.pfm"
    return g


def test_save_load_round_trip(tmp_path):
    """save() then load() gives an equivalent graph, and saving it again gives the same bytes."""
    g = _graph()
    g.base = str(tmp_path)  # (the paths stay as they are when the graph is saved in its base)
    assert list(g.nodes) == ["invertPlugin", "out", "seq"] and g.outputs == {"out": "out.####.pfm", "seq": None}
    g.save(tmp_path / "g.json")
    text = (tmp_path / "g.json").read_text()
    doc = json.loads(text)
    assert doc["schema"] == "ofxhost.graph" and doc["schemaVersion"] == 1
    assert doc["render"] == {"frames": [0.0, 1.0], "renderScale": [0.5, 0.5], "window": [4, 2, 30, 20], "depth": "short"}
    assert doc["nodes"]["out"]["params"]["scale"] == {"keys": [[0.0, 1.5], [1.0, 2]], "interpolation": "linear"}
    assert doc["nodes"]["out"]["params"]["scaleR"] == {"keys": [[0.5, 0.25]], "interpolation": "linear"}
    assert doc["nodes"]["out"]["inputs"] == {"Source": {"node": "invertPlugin"}, "Mask": "none"}
    assert doc["nodes"]["seq"]["inputs"] == {"Source": {"file": "in.####.pfm"}}
    assert doc["nodes"]["invertPlugin"]["version"] == [1, 0] and "version" not in doc["nodes"]["out"]
    h = load(tmp_path / "g.json")
    assert h.dumps() == text  # the second save: the same bytes
    assert h.frames == [0., 1.] and h.scale == (.5, .5) and h.window == (4, 2, 30, 20) and h.depth == "short"
    out = h.nodes["out"]
    assert out.params["scale"] == Keys({0.: 1.5, 1.: 2}) and out.params["scaleR"] == Keys({.5: .25})
    assert out.inputs["Source"] is h.nodes["invertPlugin"] and h.nodes["invertPlugin"].version == (1, 0)
    assert h.nodes["seq"].inputs == {"Source": Files("in.####.pfm")}
    # a project, a push button press, a file parameter, a premultiplied sequence
    g.project = Project(size=(64., 48.), pixel_aspect_ratio=1., frame_range=(1., 5.))
    g.nodes["out"].params["press"] = None
    g.nodes["out"].params["lut"] = FilePath("luts/a.cube")
    g.nodes["seq"].inputs["Source"] = Files("in.####.pfm#premult")
    assert json.loads(g.dumps())["nodes"]["out"]["params"]["lut"] == {"file": "luts/a.cube"}
    assert loads(g.dumps()).nodes["out"].params["lut"] == FilePath("luts/a.cube")
    assert loads(g.dumps()).dumps() == g.dumps()
    assert loads(g.dumps()).project == g.project and loads(g.dumps()).nodes["out"].params["press"] is None


def test_save_elsewhere(tmp_path):
    """Saved in another directory, the relative paths (file inputs, outputs, file parameters) are
    rewritten for it; a "file:" spec is made absolute; saving the loaded graph there again gives the
    same bytes, and saving it back in its first directory gives the first file."""
    g = _graph()
    g.base = str(tmp_path / "a")
    g.nodes["out"].params["lut"] = FilePath("luts/x.cube")
    g.nodes["out"].params["proxy"] = FilePath("")  # (no file: stays empty)
    g.nodes["out"].inputs["Mask"] = "file:m.pfm#premult"
    (tmp_path / "a").mkdir()
    (tmp_path / "b" / "c").mkdir(parents=True)
    g.save(tmp_path / "a" / "g.json")
    g.save(tmp_path / "b" / "c" / "g.json")
    doc = json.loads((tmp_path / "b" / "c" / "g.json").read_text())
    assert doc["nodes"]["seq"]["inputs"]["Source"] == {"file": "../../a/in.####.pfm"}
    assert doc["outputs"]["out"] == "../../a/out.####.pfm"
    assert doc["nodes"]["out"]["params"]["lut"] == {"file": "../../a/luts/x.cube"}
    assert doc["nodes"]["out"]["params"]["proxy"] == {"file": ""}
    assert doc["nodes"]["out"]["inputs"]["Mask"] == "file:" + str((tmp_path / "a" / "m.pfm").resolve()) + "#premult"
    h = load(tmp_path / "b" / "c" / "g.json")
    assert h.path(h.nodes["seq"].inputs["Source"].pattern) == os.path.join(str(tmp_path / "b" / "c"), "../../a/in.####.pfm")
    h.save(tmp_path / "b" / "c" / "h.json")
    assert (tmp_path / "b" / "c" / "h.json").read_text() == (tmp_path / "b" / "c" / "g.json").read_text()
    h.save(tmp_path / "a" / "h.json")
    first = json.loads((tmp_path / "a" / "g.json").read_text())
    back = json.loads((tmp_path / "a" / "h.json").read_text())
    assert back["nodes"]["seq"] == first["nodes"]["seq"] and back["outputs"] == first["outputs"]


def test_load_strict():
    """Unknown keys are refused, at the top level and in the nodes; and "output" with "outputs"."""
    node = {"plugin": INVERT, "inputs": {"Source": "ramp"}}
    with pytest.raises(ValueError, match="unknown key 'ouputs'"):
        loads(json.dumps({"ouputs": {"a": None}, "output": "a", "nodes": {"a": node}}))
    with pytest.raises(ValueError, match="node 'a': unknown key 'param'"):
        loads(json.dumps({"outputs": {"a": None}, "nodes": {"a": {**node, "param": {}}}}))
    with pytest.raises(ValueError, match='both "output" and "outputs"'):
        loads(json.dumps({"outputs": {"a": None}, "output": "a", "nodes": {"a": node}}))
    # a project's pixel aspect ratio is positive
    for par in (0, -1.):
        with pytest.raises(ValueError, match="pixelAspectRatio: a positive number"):
            loads(json.dumps({"outputs": {"a": None}, "nodes": {"a": node}, "project": {"pixelAspectRatio": par}}))


def test_save_errors():
    with pytest.raises(ValueError, match="can't be saved"):
        Graph.of(Node(INVERT, inputs={"Source": np.zeros((2, 2, 4), np.float32)})).dumps()
    with pytest.raises(ValueError, match="not in the graph"):
        a = Node(INVERT)
        Graph({"b": Node(INVERT, inputs={"Source": a})}, {"b": None}).dumps()
    with pytest.raises(ValueError, match="same name"):
        Graph.of(Node(INVERT, name="x", inputs={"Source": Node(INVERT, name="x")}))
    with pytest.raises(ValueError, match="interpolat"):
        Keys({0: 1}, "bezier")
    with pytest.raises(ValueError, match="interpolat"):
        loads(json.dumps({"outputs": {"a": None}, "nodes": {"a": {"plugin": INVERT, "params": {
            "x": {"keys": [[0, 1]], "interpolation": "smooth"}}}}}))
    with pytest.raises(ValueError, match="not finite"):
        Graph.of(Node(INVERT, params={"x": float("nan")})).dumps()
    with pytest.raises(ValueError, match="schema"):
        loads(json.dumps({"schema": "x", "outputs": {"a": None}, "nodes": {"a": {"plugin": INVERT}}}))
