"""Plug-in instance graphs: build a DAG of nodes, save it to JSON or load it (the sample host,
ofxhost.graphhost, runs them).

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
    r = graphhost.run(g, bundles)              # r.images["basicPlugin"]: {0.0: Image, 1.0: Image}

The JSON format (schema "ofxhost.graph", version 1; save() writes the keys in this order, and
every key but "nodes" and "outputs" is optional):

    {"schema": "ofxhost.graph", "schemaVersion": 1,
     "project": {"size": [768.0, 576.0], "offset": [0.0, 0.0], "pixelAspectRatio": 1.0666666666666667,
                 "frameRate": 25.0, "frameRange": [0.0, 24.0]},
     "render": {"frames": [0.0, 1.0], "renderScale": [1.0, 1.0], "window": null, "depth": "float"},
     "outputs": {"b": "out.####.pfm"},
     "nodes": {"a": {"plugin": "net.sf.openfx.invertPlugin", "version": [1, 0], "context": "filter",
                     "params": {}, "inputs": {"Source": {"file": "in.####.pfm#premult"}}},
               "b": {"plugin": "net.sf.openfx.basicPlugin", "context": "general",
                     "params": {"scale": {"keys": [[0.0, 1.0], [10.0, 2.0]], "interpolation": "linear"},
                                "scaleComponents": true},
                     "inputs": {"Source": {"node": "a"}, "Mask": "none"}}}}

- "project": the project (its defaults are hostDemo's, shown above).
- "render": "frames" (null: every frame of the outputs' time domains), "renderScale" (sx, sy),
  "window" (pixels at the render scale, null: the project), "depth" of the unconnected and pattern
  inputs (byte, short, float).
- "outputs": the output nodes, each with the file name pattern it is written to (see
  ofxhost.sequences: "out.####.pfm"), or null (rendered, not written; e.g. a writer plug-in).
  ("output": "b" is read as {"b": null}.)
- "params": a value (as ofxh.param_template() gives them: numbers, booleans, strings, lists; a
  choice's index or option label), keys {"keys": [[time, value]...], "interpolation": "linear"},
  a file path {"file": path} (FilePath: the value of a file parameter, which may be a pattern),
  or null to press a push button (after the instance is created, as a user would).
  "interpolation" is that of hostDemo and ofxh, the only one: linear, rounded for integers, held for
  booleans and choices.
- "inputs": {"node": name}, {"file": pattern} (an image sequence, see ofxhost.sequences, or a still
  image; "#premult", "#unpremult" or "#opaque" at the end gives the files' premultiplication), or a
  string for ofxhost.render(): a test pattern ("default", "ramp", "checker:16", "solid:r,g,b,a",
  "rod:x1,y1,x2,y2"), "none", or "file:x.pfm". An input clip that the graph doesn't give is
  disconnected, as in a real host: the test patterns must be asked for.
- Relative paths (file inputs, outputs and file parameters) are relative to the graph file's
  directory (Graph.base). save() rewrites them for the directory it saves to (as absolute paths if
  there is no relative one), so that a graph saved elsewhere still finds its files.
"version" is [major, minor] (the latest version of that major is used); without it, the latest.
Unknown keys are refused, in the nodes and at the top level.

save() then load() gives an equivalent graph, and saving that one gives the same bytes again.

A graph is like a script: it chooses the plug-ins that run, with their parameters, and the files
it reads and writes, anywhere the user can. Run only the graphs you trust.
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

from . import DEPTHS, resolve_file_spec, split_components
from .rects import scale_pair

__all__ = ["Node", "Keys", "Files", "FilePath", "Project", "Graph", "topological_order", "load", "loads", "stem",
           "json_value", "keys_of", "SCHEMA", "SCHEMA_VERSION"]

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


_PREMULTS = ("premult", "unpremult", "opaque")


@dataclasses.dataclass(frozen=True)
class Files:
    """An input read from image files: a file name pattern (ofxhost.sequences: "in.####.pfm"), one
    file per frame, or a still image (the same at all times). Relative to the graph's directory.
    A "#premult", "#unpremult" or "#opaque" suffix gives the premultiplication of the files
    (default: unpremultiplied, as hostDemo's file inputs); then a "#rgb", "#alpha" or "#rgba" suffix
    the components they give the clip (its unmapped components, default RGBA: an RGB file is opaque
    RGBA, a grey one r = g = b = a), e.g. "in.####.pfm#rgb", "matte.pfm#premult#alpha"."""
    pattern: str

    def split(self) -> tuple[str, str | None]:
        """(the file name pattern, the premultiplication suffix or None), without the components."""
        pattern = split_components(self.pattern)[0]
        head, sep, tail = pattern.rpartition("#")
        return (head, tail) if sep and tail in _PREMULTS else (pattern, None)

    @property
    def components(self) -> str:
        """The components suffix: "rgb", "alpha", or "rgba" (the default)."""
        return split_components(self.pattern)[1] or "rgba"


class FilePath(str):
    """The value of a file parameter ({"file": path} in the JSON): a path, or a file name pattern,
    relative to the graph's directory; save() rewrites it as it does the file inputs."""

    def __repr__(self) -> str:
        return f"FilePath({str.__repr__(self)})"


def stem(plugin_id: str) -> str:
    """A node name from a plug-in id: its last component ("net.sf.openfx.invertPlugin": "invertPlugin")."""
    return plugin_id.replace(":", ".").split(".")[-1] or "node"


def _rebase(p: str, src: str | None, dst: str | None) -> str:
    """A path of a graph whose base is src (None: the current directory), for a graph whose base is
    dst: the same if it is empty or absolute or the bases are the same, else relative to dst, or
    absolute if there is no relative path (another drive)."""
    if not p or dst is None or os.path.isabs(p):  # (an empty path stays empty: "no file")
        return p
    src = os.path.abspath(src or os.getcwd())
    if src == os.path.abspath(dst):
        return p
    full = os.path.normpath(os.path.join(src, p))
    try:
        return os.path.relpath(full, dst)
    except ValueError:
        return full


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
                   pixel_aspect_ratio=_positive(d.get("pixelAspectRatio", p.pixel_aspect_ratio), "pixelAspectRatio"),
                   frame_rate=_number(d.get("frameRate", p.frame_rate), "frameRate"),
                   frame_range=_floats(d.get("frameRange", p.frame_range), 2, "frameRange"))


def _number(v, what: str) -> float:
    if isinstance(v, bool) or not isinstance(v, numbers.Real) or not math.isfinite(v):
        raise ValueError(f"{what}: a finite number, not {v!r}")
    return float(v)


def _positive(v, what: str) -> float:
    x = _number(v, what)
    if x <= 0:
        raise ValueError(f"{what}: a positive number, not {v!r}")
    return x


def _floats(v, n: int, what: str) -> tuple:
    if isinstance(v, (str, bytes)) or not isinstance(v, Sequence) or len(v) != n:
        raise ValueError(f"{what}: {n} numbers, not {v!r}")
    return tuple(_number(x, what) for x in v)


def json_value(v):
    """A parameter value as JSON data: lists, not tuples; bytes as lists of ints (as
    ofxh.param_template())."""
    if isinstance(v, (bytes, bytearray)):
        return list(v)
    if isinstance(v, np.ndarray):
        return v.tolist()
    if isinstance(v, np.generic):
        return v.item()
    if isinstance(v, (list, tuple)):
        return [json_value(x) for x in v]
    if isinstance(v, float) and not math.isfinite(v):
        raise ValueError(f"parameter value {v!r} is not finite")
    return v


def keys_of(v) -> Keys | None:
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
                s = stem(node.plugin)
                name, k = s, 2
                while name in used:
                    name, k = f"{s}{k}", k + 1
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

    def to_json(self, base: str | os.PathLike | None = None) -> dict:
        """The graph as JSON data (the format of the module docstring). base: the directory the JSON
        will be in (save(): the file's), for which the relative paths are rewritten (None: as they
        are). Raises ValueError for what JSON can't hold: an array or Image input, a node that isn't in
        nodes, a non-finite value."""
        dst = None if base is None else os.path.abspath(base)

        def path(p: str) -> str:
            return _rebase(p, self.base, dst)

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
                         "renderScale": [float(s) for s in scale_pair(self.scale)],
                         "window": None if self.window is None else [int(v) for v in self.window],
                         "depth": self.depth}
        doc["outputs"] = {name: None if f is None else path(os.fspath(f)) for name, f in self.outputs.items()}
        nodes = doc["nodes"] = {}
        for name, n in self.nodes.items():
            d: dict[str, Any] = {"plugin": n.plugin}
            if n.version is not None:
                d["version"] = [int(v) for v in n.version]
            d["context"] = n.context
            params = d["params"] = {}
            for p, v in n.params.items():
                k = keys_of(v)
                params[p] = v if v is None else {"file": path(v)} if isinstance(v, FilePath) else \
                    json_value(v) if k is None else {
                    "keys": [[float(t), json_value(x)] for t, x in sorted(k.values.items(), key=lambda kv: float(kv[0]))],
                    "interpolation": k.interpolation}
            inputs = d["inputs"] = {}
            for clip, v in n.inputs.items():
                if isinstance(v, Node):
                    if id(v) not in names:
                        raise ValueError(f"{name}.{clip}: its input node {v!r} is not in the graph")
                    inputs[clip] = {"node": names[id(v)]}
                elif isinstance(v, Files):
                    pattern, premult = v.split()
                    comps = split_components(v.pattern)[1]
                    inputs[clip] = {"file": path(pattern) + ("#" + premult if premult else "") +
                                    ("#" + comps if comps else "")}
                elif isinstance(v, str):
                    # (a hostDemo "file:" spec is made absolute when the base changes)
                    rebase = v.startswith("file:") and dst is not None and \
                        os.path.abspath(self.base or os.getcwd()) != dst
                    if rebase:
                        body, comps = split_components(v)
                        v = resolve_file_spec(body, self.base) + ("#" + comps if comps else "")
                    inputs[clip] = v
                else:
                    raise ValueError(f"{name}.{clip}: an input of type {type(v).__name__} can't be saved "
                                     "(write the image to a file, and use Files or \"file:x.pfm\")")
            nodes[name] = d
        return doc

    def dumps(self, base: str | os.PathLike | None = None) -> str:
        """The JSON text (2-space indents, a newline at the end); base: as to_json()."""
        return json.dumps(self.to_json(base), indent=2, ensure_ascii=False, allow_nan=False) + "\n"

    def save(self, path: str | os.PathLike) -> None:
        """Write the JSON file, with the relative paths rewritten for its directory."""
        text = self.dumps(os.path.dirname(os.path.abspath(path)))  # (before opening: an error writes nothing)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


def loads(text: str, base: str | None = None) -> Graph:
    """A graph from JSON text (see load())."""
    return _from_json(json.loads(text), base)


def load(path: str | os.PathLike) -> Graph:
    """A graph from a JSON file (the format of the module docstring). Relative paths stay as they are
    in the graph, relative to the file's directory (Graph.base)."""
    with open(path, encoding="utf-8") as f:
        return loads(f.read(), os.path.dirname(os.path.abspath(path)))


_TOP_KEYS = ("schema", "schemaVersion", "project", "render", "outputs", "output", "nodes")
_NODE_KEYS = ("plugin", "version", "context", "params", "inputs")


def _unknown(d: dict, known, where: str) -> None:
    extra = [k for k in d if k not in known]
    if extra:
        raise ValueError(f"{where}: unknown key {extra[0]!r} (the keys are {', '.join(known)})")


def _from_json(doc, base) -> Graph:
    if not isinstance(doc, dict) or not isinstance(doc.get("nodes"), dict):
        raise ValueError('a graph file needs "nodes": {name: {"plugin": ...}, ...}')
    _unknown(doc, _TOP_KEYS, "the graph")
    if "output" in doc and "outputs" in doc:
        raise ValueError('the graph has both "output" and "outputs": give "outputs" only')
    if doc.get("schema", SCHEMA) != SCHEMA or doc.get("schemaVersion", SCHEMA_VERSION) != SCHEMA_VERSION:
        raise ValueError(f"schema {doc.get('schema')!r} version {doc.get('schemaVersion')!r}: expected "
                         f"{SCHEMA!r} version {SCHEMA_VERSION}")
    for name, d in doc["nodes"].items():
        if not isinstance(d, dict) or not isinstance(d.get("plugin"), str):
            raise ValueError(f'node {name!r}: expected {{"plugin": id, ...}}')
        _unknown(d, _NODE_KEYS, f"node {name!r}")
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
        if isinstance(v, dict) and set(v) == {"file"} and isinstance(v["file"], str):
            v = FilePath(v["file"])
        elif isinstance(v, dict):
            if set(v) - {"keys", "interpolation"} or not isinstance(v.get("keys"), list):
                raise ValueError(f'{node}.{name}: keys are {{"keys": [[time, value]...], "interpolation": "linear"}}')
            if not all(isinstance(k, list) and len(k) == 2 for k in v["keys"]):
                raise ValueError(f"{node}.{name}: each key is [time, value]")
            v = Keys({_number(t, f"{node}.{name} key time"): x for t, x in v["keys"]},
                     v.get("interpolation", "linear"))
        out[name] = v
    return out


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
