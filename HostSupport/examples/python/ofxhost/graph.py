"""Plug-in instance graphs: build a DAG of nodes, save it to JSON or load it, and execute it.

A Node is a plug-in instance: its plug-in (and version), context, parameters, and inputs. An input
is another Node, a Files image sequence, or anything ofxhost.render() takes (a NumPy array, an
Image, a hostDemo spec such as "ramp", "none" or "file:x.pfm"). A Node's output may feed several
inputs: the graph is a DAG. A Graph names the nodes, says which ones are its outputs (and the files
they are written to), and holds the project and render settings.

    src = Node("net.sf.openfx.invertPlugin", inputs={"Source": "ramp"})
    out = Node("net.sf.openfx.basicPlugin", "general", {"scale": Keys({0: 1., 10: 2.})},
               inputs={"Source": src, "Mask": src})
    g = Graph.of(out, frames=[0, 1])           # names the nodes; outputs {"basicPlugin": None}
    g.save("graph.json")                       # the JSON format below; load("graph.json") reads it
    images = execute(g, bundles)               # {0.0: Image, 1.0: Image}, with hostDemo (ofxhost.render)

Two hosts execute graphs: execute(), here, with hostDemo (either ofxhost backend, one render() per
node and time), and the sample host ofxhost.graphhost (in-process on ofxh, with in-memory images
between the nodes, the time domain, image sequences and the project).

The JSON format (schema "ofxhost.graph", version 1; save() writes the keys in this order, and
every key but "nodes" and "outputs" is optional):

    {"schema": "ofxhost.graph", "schemaVersion": 1,
     "project": {"size": [768.0, 576.0], "offset": [0.0, 0.0], "pixelAspectRatio": 1.0666666666666667,
                 "frameRate": 25.0, "frameRange": [0.0, 24.0]},
     "render": {"frames": [0.0, 1.0], "renderScale": [1.0, 1.0], "window": null, "depth": "float"},
     "outputs": {"b": "out.####.pfm"},
     "nodes": {"a": {"plugin": "net.sf.openfx.invertPlugin", "version": [1, 0], "context": "filter",
                     "params": {}, "inputs": {"Source": {"file": "in.####.pfm"}}},
               "b": {"plugin": "net.sf.openfx.basicPlugin", "context": "general",
                     "params": {"scale": {"keys": [[0.0, 1.0], [10.0, 2.0]], "interpolation": "linear"},
                                "scaleComponents": true},
                     "inputs": {"Source": {"node": "a"}, "Mask": "none"}}}}

- "project": the project of the sample host (ofxhost.graphhost; its defaults are hostDemo's, shown
  above). execute() renders in hostDemo's project, and refuses another one.
- "render": "frames" (null: the time domain of the outputs, in the sample host; execute() needs
  frames), "renderScale" (sx, sy), "window" (pixels at the render scale, null: the RoD clipped to
  the project), "depth" of the unconnected and pattern inputs (byte, short, float).
- "outputs": the output nodes, each with the file name pattern it is written to (see
  ofxhost.sequences: "out.####.pfm"), or null (rendered, not written; e.g. a writer plug-in).
  ("output": "b" is read as {"b": null}.)
- "params": a value (as ofxh.param_template() gives them: numbers, booleans, strings, lists; a
  choice's index or option label), keys {"keys": [[time, value]...], "interpolation": "linear"},
  or null to press a push button (after the instance is created, as a user would).
  "interpolation" is that of hostDemo and ofxh, the only one: linear, rounded for integers, held for
  booleans and choices.
- "inputs": {"node": name}, {"file": pattern} (an image sequence, see ofxhost.sequences; or a still
  image), or a string for ofxhost.render() ("ramp", "none", "file:x.pfm"...). Relative paths
  (inputs and outputs) are relative to the graph file's directory.
"version" is [major, minor] (the latest version of that major is used); without it, the latest.

save() then load() gives an equivalent graph, and saving that one gives the same bytes again.
"""

from __future__ import annotations

import dataclasses
import json
import math
import numbers
import os
from collections.abc import Mapping, Sequence
from typing import Any

import numpy as np

from . import DEPTHS, HostDemoError, Image, render, resolve_file_spec
from .sequences import expand, frame_of

__all__ = ["Node", "Keys", "Files", "Project", "Graph", "topological_order", "load", "loads", "execute",
           "SCHEMA", "SCHEMA_VERSION"]

SCHEMA = "ofxhost.graph"
SCHEMA_VERSION = 1
INTERPOLATIONS = ("linear",)  # (hostDemo's and ofxh's: linear, rounded for integers, held for booleans and choices)


@dataclasses.dataclass(eq=False)  # nodes are compared by identity
class Node:
    plugin: str
    context: str = "filter"
    params: dict = dataclasses.field(default_factory=dict)  # name -> value | Keys | {time: value} | None (press)
    inputs: dict = dataclasses.field(default_factory=dict)  # clip -> Node | Files | render() input
    name: str = ""
    version: tuple | None = None  # (major, minor) of the plug-in; None: the latest

    def __repr__(self) -> str:
        return f"Node({self.name or self.plugin!r})"


@dataclasses.dataclass
class Keys:
    """The keys of an animated parameter, {time: value}. A plain {time: value} dict in Node.params
    means the same. interpolation: "linear", hostDemo's and ofxh's (linear, rounded for integers,
    held for booleans and choices; the only one they have)."""
    values: dict
    interpolation: str = "linear"

    def __post_init__(self):
        if self.interpolation not in INTERPOLATIONS:
            raise ValueError(f"interpolation {self.interpolation!r}: hostDemo and ofxh interpolate keys linearly "
                             "(\"linear\"), rounded for integers and held for booleans and choices")
        if not self.values:
            raise ValueError("Keys: no keys")


@dataclasses.dataclass(frozen=True)
class Files:
    """An input read from image files: a file name pattern (ofxhost.sequences: "in.####.pfm"), one
    file per frame, or a still image (the same at all times). Relative to the graph's directory."""
    pattern: str


@dataclasses.dataclass
class Project:
    """The project of the sample host (ofxhost.graphhost); the defaults are hostDemo's. frame_range
    is the first and last frames (the timeline bounds)."""
    size: tuple = (768., 576.)
    offset: tuple = (0., 0.)
    pixel_aspect_ratio: float = 768. / 720.
    frame_rate: float = 25.
    frame_range: tuple = (0., 24.)

    def to_json(self) -> dict:
        return {"size": [float(v) for v in self.size], "offset": [float(v) for v in self.offset],
                "pixelAspectRatio": float(self.pixel_aspect_ratio), "frameRate": float(self.frame_rate),
                "frameRange": [float(v) for v in self.frame_range]}

    @classmethod
    def from_json(cls, d: Mapping) -> Project:
        known = {"size", "offset", "pixelAspectRatio", "frameRate", "frameRange"}
        if not isinstance(d, Mapping) or set(d) - known:
            raise ValueError(f'"project": an object with some of {sorted(known)}')
        p = cls()
        return cls(size=_floats(d.get("size", p.size), 2, "project size"),
                   offset=_floats(d.get("offset", p.offset), 2, "project offset"),
                   pixel_aspect_ratio=_number(d.get("pixelAspectRatio", p.pixel_aspect_ratio), "pixelAspectRatio"),
                   frame_rate=_number(d.get("frameRate", p.frame_rate), "frameRate"),
                   frame_range=_floats(d.get("frameRange", p.frame_range), 2, "frameRange"))


def _number(v, what: str) -> float:
    if isinstance(v, bool) or not isinstance(v, numbers.Real) or not math.isfinite(v):
        raise ValueError(f"{what}: a finite number, not {v!r}")
    return float(v)


def _floats(v, n: int, what: str) -> tuple:
    if isinstance(v, (str, bytes)) or not isinstance(v, Sequence) or len(v) != n:
        raise ValueError(f"{what}: {n} numbers, not {v!r}")
    return tuple(_number(x, what) for x in v)


def _json_value(v):
    """A parameter value as JSON data: lists, not tuples; bytes as lists of ints (as
    ofxh.param_template())."""
    if isinstance(v, (bytes, bytearray)):
        return list(v)
    if isinstance(v, np.ndarray):
        return v.tolist()
    if isinstance(v, np.generic):
        return v.item()
    if isinstance(v, (list, tuple)):
        return [_json_value(x) for x in v]
    if isinstance(v, float) and not math.isfinite(v):
        raise ValueError(f"parameter value {v!r} is not finite")
    return v


def _keys(v) -> Keys | None:
    """The Keys of a parameter value (a Keys or a {time: value} mapping), else None."""
    if isinstance(v, Keys):
        return v
    if isinstance(v, Mapping):
        return Keys(dict(v))
    return None


@dataclasses.dataclass(eq=False)
class Graph:
    """Named nodes, the output nodes (name -> output file pattern, or None), and the project and render
    settings (see the module docstring). base: the directory that relative paths are relative to
    (load(): the graph file's; None: the current directory)."""
    nodes: dict
    outputs: dict
    frames: list | None = None
    scale: tuple = (1., 1.)
    window: tuple | None = None
    depth: str = "float"
    project: Project | None = None
    base: str | None = None

    @classmethod
    def of(cls, *outputs: Node, **settings) -> Graph:
        """The graph of these output nodes and of the nodes they depend on, in topological order.
        Nodes without a name get one from their plug-in id ("invertPlugin", "invertPlugin2"...)."""
        order: list[Node] = []
        for out in outputs:
            order += [n for n in topological_order(out) if all(n is not m for m in order)]
        used = [n.name for n in order if n.name]
        if len(set(used)) != len(used):
            raise ValueError(f"two nodes have the same name: {sorted(used)}")
        names: dict[int, str] = {}
        for node in order:
            name = node.name
            if not name:
                stem = node.plugin.replace(":", ".").split(".")[-1] or "node"
                name, k = stem, 2
                while name in used:
                    name, k = f"{stem}{k}", k + 1
                used.append(name)
            names[id(node)] = name
        nodes = {names[id(n)]: n for n in order}
        return cls(nodes=nodes, outputs={names[id(o)]: None for o in outputs}, **settings)

    @property
    def output(self) -> Node:
        """The first output node."""
        return self.nodes[next(iter(self.outputs))]

    def path(self, p: str) -> str:
        """A path of the graph (an input or output file), relative to base."""
        return p if os.path.isabs(p) or self.base is None else os.path.join(self.base, p)

    def to_json(self) -> dict:
        """The graph as JSON data (the format of the module docstring). Raises ValueError for what
        JSON can't hold: an array or Image input, a node that isn't in nodes, a non-finite value."""
        names = {id(n): name for name, n in self.nodes.items()}
        if len(names) != len(self.nodes):
            raise ValueError("a node is in the graph under two names")
        for name in self.outputs:
            if name not in self.nodes:
                raise ValueError(f"output {name!r} is not a node of the graph")
        doc: dict[str, Any] = {"schema": SCHEMA, "schemaVersion": SCHEMA_VERSION}
        if self.project is not None:
            doc["project"] = self.project.to_json()
        doc["render"] = {"frames": None if self.frames is None else [float(t) for t in self.frames],
                         "renderScale": [float(s) for s in _scale(self.scale)],
                         "window": None if self.window is None else [int(v) for v in self.window],
                         "depth": self.depth}
        doc["outputs"] = {name: None if f is None else os.fspath(f) for name, f in self.outputs.items()}
        nodes = doc["nodes"] = {}
        for name, n in self.nodes.items():
            d: dict[str, Any] = {"plugin": n.plugin}
            if n.version is not None:
                d["version"] = [int(v) for v in n.version]
            d["context"] = n.context
            params = d["params"] = {}
            for p, v in n.params.items():
                k = _keys(v)
                params[p] = v if v is None else _json_value(v) if k is None else {
                    "keys": [[float(t), _json_value(x)] for t, x in sorted(k.values.items(), key=lambda kv: float(kv[0]))],
                    "interpolation": k.interpolation}
            inputs = d["inputs"] = {}
            for clip, v in n.inputs.items():
                if isinstance(v, Node):
                    if id(v) not in names:
                        raise ValueError(f"{name}.{clip}: its input node {v!r} is not in the graph")
                    inputs[clip] = {"node": names[id(v)]}
                elif isinstance(v, Files):
                    inputs[clip] = {"file": v.pattern}
                elif isinstance(v, str):
                    inputs[clip] = v
                else:
                    raise ValueError(f"{name}.{clip}: an input of type {type(v).__name__} can't be saved "
                                     "(write the image to a file, and use Files or \"file:x.pfm\")")
            nodes[name] = d
        return doc

    def dumps(self) -> str:
        """The JSON text (2-space indents, a newline at the end)."""
        return json.dumps(self.to_json(), indent=2, ensure_ascii=False, allow_nan=False) + "\n"

    def save(self, path: str | os.PathLike) -> None:
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.dumps())


def loads(text: str, base: str | None = None) -> Graph:
    """A graph from JSON text (see load())."""
    return _from_json(json.loads(text), base)


def load(path: str | os.PathLike) -> Graph:
    """A graph from a JSON file (the format of the module docstring). Relative paths stay as they are
    in the graph, relative to the file's directory (Graph.base)."""
    with open(path, encoding="utf-8") as f:
        return loads(f.read(), os.path.dirname(os.path.abspath(path)))


def _from_json(doc, base) -> Graph:
    if not isinstance(doc, dict) or not isinstance(doc.get("nodes"), dict):
        raise ValueError('a graph file needs "nodes": {name: {"plugin": ...}, ...}')
    if doc.get("schema", SCHEMA) != SCHEMA or doc.get("schemaVersion", SCHEMA_VERSION) != SCHEMA_VERSION:
        raise ValueError(f"schema {doc.get('schema')!r} version {doc.get('schemaVersion')!r}: expected "
                         f"{SCHEMA!r} version {SCHEMA_VERSION}")
    for name, d in doc["nodes"].items():
        if not isinstance(d, dict) or not isinstance(d.get("plugin"), str):
            raise ValueError(f'node {name!r}: expected {{"plugin": id, ...}}')
        if not isinstance(d.get("inputs", {}), dict) or not isinstance(d.get("params", {}), dict):
            raise ValueError(f'node {name!r}: "inputs" and "params" are objects')
        v = d.get("version")
        if v is not None and (not isinstance(v, list) or len(v) != 2 or not all(type(x) is int for x in v)):
            raise ValueError(f'node {name!r}: "version" is [major, minor]')
    nodes = {name: Node(d["plugin"], d.get("context", "filter"), _params(name, d.get("params", {})), name=name,
                        version=tuple(d["version"]) if d.get("version") is not None else None)
             for name, d in doc["nodes"].items()}
    for name, d in doc["nodes"].items():
        for clip, v in d.get("inputs", {}).items():
            if isinstance(v, dict) and set(v) == {"node"}:
                if v["node"] not in nodes:
                    raise ValueError(f"{name}.{clip}: unknown node {v['node']!r}")
                v = nodes[v["node"]]
            elif isinstance(v, dict) and set(v) == {"file"} and isinstance(v["file"], str):
                v = Files(v["file"])
            elif not isinstance(v, str):
                raise ValueError(f'{name}.{clip}: an input is {{"node": name}}, {{"file": pattern}} or a string, '
                                 f"not {v!r}")
            nodes[name].inputs[clip] = v
    outputs = doc.get("outputs")
    if outputs is None and "output" in doc:
        outputs = {doc["output"]: None}
    if not isinstance(outputs, dict) or not outputs:
        raise ValueError('a graph file needs "outputs": {node: file pattern or null}')
    for name, f in outputs.items():
        if name not in nodes:
            raise ValueError(f"unknown output node {name!r}")
        if f is not None and not isinstance(f, str):
            raise ValueError(f"output {name!r}: a file name pattern or null, not {f!r}")
    r = doc.get("render") or {}
    if not isinstance(r, dict) or set(r) - {"frames", "renderScale", "window", "depth"}:
        raise ValueError('"render": an object with some of frames, renderScale, window, depth')
    frames = r.get("frames")
    if frames is not None:
        if not isinstance(frames, list) or not frames:
            raise ValueError('"frames": a list of times, or null')
        frames = [_number(t, "frames") for t in frames]
    window = r.get("window")
    if window is not None:
        if not isinstance(window, list) or len(window) != 4 or not all(type(v) is int for v in window):
            raise ValueError('"window": [x1, y1, x2, y2] (integers), or null')
        window = tuple(window)
    depth = r.get("depth", "float")
    if depth not in DEPTHS:
        raise ValueError(f'"depth": one of {DEPTHS}, not {depth!r}')
    return Graph(nodes=nodes, outputs=dict(outputs), frames=frames,
                 scale=_floats(r.get("renderScale", [1., 1.]), 2, "renderScale"), window=window, depth=depth,
                 project=Project.from_json(doc["project"]) if doc.get("project") is not None else None, base=base)


def _params(node: str, params: dict) -> dict:
    out = {}
    for name, v in params.items():
        if isinstance(v, dict):
            if set(v) - {"keys", "interpolation"} or not isinstance(v.get("keys"), list):
                raise ValueError(f'{node}.{name}: keys are {{"keys": [[time, value]...], "interpolation": "linear"}}')
            if not all(isinstance(k, list) and len(k) == 2 for k in v["keys"]):
                raise ValueError(f"{node}.{name}: each key is [time, value]")
            v = Keys({_number(t, f"{node}.{name} key time"): x for t, x in v["keys"]},
                     v.get("interpolation", "linear"))
        out[name] = v
    return out


def _scale(scale) -> tuple:
    return (float(scale), float(scale)) if isinstance(scale, numbers.Real) else tuple(float(s) for s in scale)


def topological_order(output: Node) -> list[Node]:
    """The nodes that output depends on, inputs first, output last. Raises ValueError on a
    cycle."""
    order: list[Node] = []
    state: dict[int, str] = {}  # id(node) -> "visiting" | "done"

    def visit(node: Node, path: list[Node]):
        s = state.get(id(node))
        if s == "done":
            return
        if s == "visiting":
            raise ValueError("cycle in the graph: " + " -> ".join(map(repr, path + [node])))
        state[id(node)] = "visiting"
        for child in node.inputs.values():
            if isinstance(child, Node):
                visit(child, path + [node])
        state[id(node)] = "done"
        order.append(node)

    visit(output, [])
    return order


def _image_fits(px) -> bool:
    """As hostDemo's imageFits (float RGBA): at most 2^28 pixels, rows of less than 2^31 bytes."""
    w, h = px[2] - px[0], px[3] - px[1]
    m = 1 << 28
    return w <= m and h <= m and w * h <= m and w * 16 <= 0x7fffffff


def _union(a, b):
    if a is None:
        return b
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def _frames(ranges: Sequence[Sequence[float]]) -> list[float]:
    """The frames in the frames-needed ranges: each range must be a single time [t, t]
    (hostDemo's inputs are the same image at all times); a longer range raises."""
    out: list[float] = []
    for lo, hi in ranges:
        if abs(hi - lo) > 1e-9:
            raise NotImplementedError(f"frames needed [{lo}, {hi}]: hostDemo inputs are the same "
                                      "image at all times, so a node can't use a range of times")
        if float(lo) not in out:
            out.append(float(lo))
    return out


_EPS = 1e-6  # as hostDemo's canonicalToPixelEnclosing


def _to_pixels(rect, sx, sy, par):
    """A canonical rectangle to the enclosing pixel rectangle at the render scale."""
    return (math.floor(rect[0] * sx / par + _EPS), math.floor(rect[1] * sy + _EPS),
            math.ceil(rect[2] * sx / par - _EPS), math.ceil(rect[3] * sy - _EPS))


def _placeholder(rod):
    """A transparent input with the given RoD (canonical; None or empty: an empty RoD), for the
    queries, and for an input node that rendered nothing: hostDemo's "rod:" input, which has no
    pixels to write."""
    if rod is None or rod[2] <= rod[0] or rod[3] <= rod[1]:
        return "rod:0,0,0,0"
    return "rod:" + ",".join(repr(float(v)) for v in rod)


def execute(graph: Graph | Node, bundles, frames: Sequence[float] | None = None, scale=None,
            window: Sequence[int] | None = None, depth: str | None = None,
            **render_args: Any) -> dict[float, Image]:
    """Render a graph's (first) output node, or a node, at each of frames with hostDemo
    (ofxhost.render(), on either backend); return {time: Image}. frames, scale, window (pixels at
    the render scale; default: the RoD clipped to the project) and depth default to the graph's
    settings (frames: [0] if the graph has none). render_args go to ofxhost.render() (backend,
    host_binary, timeout...). A graph with a project raises ValueError: hostDemo's project is fixed
    (768x576, PAR 768/720). A Files input gives each render the file of its frame (hostDemo's
    "file:" input, whose pixels are at the render scale)."""
    g = graph if isinstance(graph, Graph) else Graph({"": graph}, {"": None})
    if g.project is not None:
        raise ValueError("execute() renders in hostDemo's project, which is fixed (768x576, PAR 768/720): "
                         "remove the graph's project, or run the graph with ofxhost.graphhost")
    output = g.output
    frames = [float(t) for t in (frames if frames is not None else g.frames if g.frames is not None else [0.])]
    scale = g.scale if scale is None else scale
    window = g.window if window is None else window
    depth = g.depth if depth is None else depth
    order = topological_order(output)
    extra = list(render_args.pop("extra_args", ()))
    common: dict[str, Any] = dict(depth=depth, scale=scale, **render_args)
    sx, sy = _scale(scale)

    def leaf(v, t: float):
        """An input that isn't a node, for render() at time t: a Files input is the file of t's
        frame; the paths are relative to the graph's directory."""
        if isinstance(v, Files):
            return "file:" + os.path.abspath(g.path(expand(v.pattern, frame_of(t))))
        if isinstance(v, str) and v.startswith("file:"):
            return resolve_file_spec(v, g.base)
        return v

    def run(node: Node, inputs: dict, t: float, **kw):
        params = {}
        for p, v in node.params.items():
            k = _keys(v)
            params[p] = v if k is None else dict(k.values)
        version = ["-m", str(int(node.version[0]))] if node.version is not None else []
        return render(bundles, node.plugin, node.context, {c: leaf(v, t) for c, v in inputs.items()}, params,
                      frames=[t], extra_args=extra + version, **common, **kw)

    # 0. inputs first: the RoD of each node at the requested times, from its inputs' RoDs (given
    # to the queries as transparent images of that RoD), so that the next queries see the real
    # input RoDs, not project-sized ones
    rods: dict[tuple[Node, float], Any] = {}
    pars: dict[Node, float] = {}

    def child_rod(child: Node, t: float):
        """The child's RoD at t; at another time than the output's frames, the RoD at the first
        of them (hostDemo's inputs and parameters don't change with time)."""
        if (child, t) in rods:
            return rods[(child, t)]
        return next((r for (n, _), r in rods.items() if n is child), None)

    def child_input(child: Node, t: float):
        return _placeholder(child_rod(child, t))

    for node in order:
        for t in sorted({float(f) for f in frames}):
            inputs = {c: (child_input(v, t) if isinstance(v, Node) else v) for c, v in node.inputs.items()}
            r = run(node, inputs, t, query=True).report
            rods[(node, t)] = r["renders"][0]["regionOfDefinition"].get("rod")
            pars[node] = r["clipPreferences"]["clips"]["Output"]["pixelAspectRatio"]
    asked: dict[Node, dict[float, Any]] = {output: {float(t): window for t in frames}}
    needs: dict[tuple[Node, float], dict[str, float]] = {}  # (node, t) -> {clip: frame}

    # 1. outputs first: what does each node need from its input nodes?
    for node in reversed(order):
        children = {c: v for c, v in node.inputs.items() if isinstance(v, Node)}
        if not children:
            continue
        for t, win in sorted(asked.get(node, {}).items()):
            inputs = {c: (child_input(v, t) if isinstance(v, Node) else v) for c, v in node.inputs.items()}
            r = run(node, inputs, t, window=win, query=True).report
            x = r["renders"][0]
            needs[(node, t)] = {}
            if x["renderWindow"] is None:
                continue  # nothing to render
            clips = r["clipPreferences"]["clips"]
            for clip, child in children.items():
                fs = _frames(x["framesNeeded"]["clips"].get(clip) or [[t, t]])
                if len(fs) != 1:
                    raise NotImplementedError(f"{node!r} needs frames {fs} of {clip}: hostDemo "
                                              "inputs are the same image at all times")
                needs[(node, t)][clip] = fs[0]
                roi = x["regionsOfInterest"]["clips"].get(clip)
                crod = child_rod(child, fs[0])
                tiles = x.get("supportsTiles") or {}
                no_tiles = not (tiles.get("effect", True) and tiles.get("clips", {}).get(clip, True))
                if (no_tiles and crod is not None
                        and _image_fits(_to_pixels(crod, *x["renderScale"], pars.get(child, clips[clip]["pixelAspectRatio"])))):
                    # no tiles: hostDemo fetches the whole RoD of the input (kOfxImageEffectPropSupportsTiles),
                    # unless it is too large for hostDemo
                    roi = crod
                if roi is None:
                    continue
                par = clips[clip]["pixelAspectRatio"]
                px = _to_pixels(roi, *x["renderScale"], par)
                if crod is not None:  # the child renders its RoD at most
                    cpx = _to_pixels(crod, *x["renderScale"], pars.get(child, par))
                    px = (max(px[0], cpx[0]), max(px[1], cpx[1]), min(px[2], cpx[2]), min(px[3], cpx[3]))
                if no_tiles and not _image_fits(px):
                    # still too large (HostSupport's default RoI of a clip without tiles is its RoD):
                    # clipped to the render window, as hostDemo does
                    rw = x["renderWindow"]
                    px = (max(px[0], rw[0]), max(px[1], rw[1]), min(px[2], rw[2]), min(px[3], rw[3]))
                if px[0] < px[2] and px[1] < px[3]:
                    w = asked.setdefault(child, {})
                    w[fs[0]] = _union(w.get(fs[0]), px)

    # 2. inputs first: render each node once per time
    images: dict[tuple[Node, float], Image] = {}
    for node in order:
        for t, win in sorted(asked.get(node, {}).items()):
            inputs = {}
            for clip, v in node.inputs.items():
                if isinstance(v, Node):
                    f = needs.get((node, t), {}).get(clip)
                    img = images.get((v, f)) if f is not None else None
                    # not needed, or nothing rendered: transparent, with the input's RoD
                    v = img if img is not None else child_input(v, t if f is None else f)
                inputs[clip] = v
            r = run(node, inputs, t, window=win)
            images[(node, t)] = next(iter(r.images.values()), None)  # None: nothing rendered
    out = {}
    for t in frames:
        img = images.get((output, float(t)))
        if img is None:
            raise HostDemoError(f"{output!r} renders nothing at time {t} (its RoD doesn't intersect "
                                "the window, or the project if no window is given)")
        out[float(t)] = img
    return out
