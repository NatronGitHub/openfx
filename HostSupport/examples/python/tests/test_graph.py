"""Tests of ofxhost.graph: building, loading and executing plug-in graphs."""

import json
import os
from pathlib import Path

import numpy as np
import pytest

import ofxhost
from ofxhost import graph
from ofxhost.graph import FilePath, Files, Graph, Keys, Node, Project, execute, load, loads, topological_order
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


def test_round_trip_renders_the_same(hostdemo, sample_plugins, tmp_path):
    """A graph built in Python renders the same images (on each backend) as the graph saved and
    loaded, in its directory and in another one (its paths rewritten): with keys, a plug-in version,
    a file sequence and the render settings."""
    src = rgba()
    for f in (0, 1):
        ofxhost.write_pfm(tmp_path / f"in.{f:04d}.pfm", src[..., :3] * (1 + f) / 2)
    inv = Node(INVERT, inputs={"Source": Files("in.####.pfm")}, version=(1, 0))
    out = Node(BASIC, "general", {"scale": Keys({0: 1., 1: 3.}), "scaleComponents": False},
               inputs={"Source": inv, "Mask": "none"})
    g = Graph.of(out, frames=[0, 1], window=(0, 0, 40, 30), base=str(tmp_path))
    g.save(tmp_path / "g.json")
    h = load(tmp_path / "g.json")
    assert h.dumps() == g.dumps()
    (tmp_path / "elsewhere").mkdir()
    g.save(tmp_path / "elsewhere" / "g.json")
    e = load(tmp_path / "elsewhere" / "g.json")
    assert e.nodes["invertPlugin"].inputs["Source"] == Files("../in.####.pfm")
    e.save(tmp_path / "elsewhere" / "e.json")
    assert (tmp_path / "elsewhere" / "e.json").read_text() == (tmp_path / "elsewhere" / "g.json").read_text()
    a = execute(g, sample_plugins, host_binary=hostdemo)
    for other in (h, e):
        b = execute(other, sample_plugins, host_binary=hostdemo)
        assert sorted(a) == sorted(b) == [0., 1.]
        for t in a:
            assert a[t].bounds == b[t].bounds == (0, 0, 40, 30)
            assert np.array_equal(a[t].pixels, b[t].pixels)
        # Invert of the frame's file, scaled by the key value (1 at t=0, 3 at t=1)
        x = (1 - src[48 - 30:, :40, :3] * (1 + t) / 2) * (1 + 2 * t)
        assert_close(a[t].pixels[..., :3], x, atol=1e-5, msg=f"t={t}")
    with pytest.raises(ValueError, match="project"):
        h.project = Project()
        execute(h, sample_plugins, host_binary=hostdemo)
