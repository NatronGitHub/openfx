"""Plug-in instance graphs: build (or load) a DAG of nodes, and execute it with hostDemo.

A Node is a plug-in instance: its plug-in, context, parameters, and inputs. An input is
another Node, or anything ofxhost.render() takes (a NumPy array, an Image, a hostDemo spec
such as "ramp" or "none"). A Node's output may feed several inputs: the graph is a DAG.

    src = Node("net.sf.openfx.invertPlugin", inputs={"Source": "ramp"})
    out = Node("net.sf.openfx.basicPlugin", "general", {"scale": 2},
               inputs={"Source": src, "Mask": src})
    images = execute(out, bundles, frames=[0], scale=0.5)   # {0: Image}

execute() runs hostDemo once per node and time:
1. In topological order, outputs first, it asks each node what it needs (hostDemo --query):
   the frames and the region of each input, for the times and windows it was asked for.
   Requests for the same node and time are merged (the union of the windows).
2. Inputs first, it renders each node once per time, and passes each output to the nodes
   that use it, as an Image (positioned at its bounds, with its premultiplication).

Before step 1, the RoD of each node is computed inputs first, so that the queries see the real
RoDs of the input nodes (given as transparent images of that RoD). Limit: hostDemo's inputs are
the same image at all times, so a node may need only one time of each input Node (a range of
frames needed raises NotImplementedError).
"""

from __future__ import annotations

import dataclasses
import json
import math
import numbers
import os
from collections.abc import Mapping, Sequence
from typing import Any

from . import HostDemoError, Image, render, resolve_file_spec

__all__ = ["Node", "topological_order", "load", "execute"]


@dataclasses.dataclass(eq=False)  # nodes are compared by identity
class Node:
    plugin: str
    context: str = "filter"
    params: dict = dataclasses.field(default_factory=dict)
    inputs: dict = dataclasses.field(default_factory=dict)  # clip -> Node | render() input
    name: str = ""

    def __repr__(self) -> str:
        return f"Node({self.name or self.plugin!r})"


def _image_fits(px) -> bool:
    """As hostDemo's imageFits (float RGBA): at most 2^28 pixels, rows of less than 2^31 bytes."""
    w, h = px[2] - px[0], px[3] - px[1]
    m = 1 << 28
    return w <= m and h <= m and w * h <= m and w * 16 <= 0x7fffffff


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


def load(path: str | os.PathLike) -> tuple[Node, dict[str, Node]]:
    """Load a graph from a JSON file; return its output node and all its nodes by name.

        {"output": "b",
         "nodes": {"a": {"plugin": "net.sf.openfx.invertPlugin", "inputs": {"Source": "ramp"}},
                   "b": {"plugin": "net.sf.openfx.invertPlugin", "params": {},
                         "inputs": {"Source": {"node": "a"}}}}}

    An input is {"node": name}, or a string for ofxhost.render() ("ramp", "none",
    "file:x.pfm", relative to the JSON file's directory).
    """
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    base = os.path.dirname(os.path.abspath(path))
    if not isinstance(doc, dict) or not isinstance(doc.get("nodes"), dict):
        raise ValueError('a graph file needs "nodes": {name: {"plugin": ...}, ...}')
    for name, d in doc["nodes"].items():
        if not isinstance(d, dict) or "plugin" not in d:
            raise ValueError(f'node {name!r}: expected {{"plugin": id, ...}}')
        if not isinstance(d.get("inputs", {}), dict) or not isinstance(d.get("params", {}), dict):
            raise ValueError(f'node {name!r}: "inputs" and "params" are objects')
    nodes = {name: Node(d["plugin"], d.get("context", "filter"), dict(d.get("params", {})),
                        name=name)
             for name, d in doc["nodes"].items()}
    for name, d in doc["nodes"].items():
        for clip, v in d.get("inputs", {}).items():
            if isinstance(v, dict):
                if v.get("node") not in nodes:
                    raise ValueError(f"{name}.{clip}: unknown node {v.get('node')!r}")
                v = nodes[v["node"]]
            elif isinstance(v, str) and v.startswith("file:"):
                v = resolve_file_spec(v, base)
            elif not isinstance(v, str):
                raise ValueError(f"{name}.{clip}: an input is {{\"node\": name}} or a string, not {v!r}")
            nodes[name].inputs[clip] = v
    if doc.get("output") not in nodes:
        raise ValueError(f"unknown output node {doc.get('output')!r}")
    return nodes[doc["output"]], nodes


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


def execute(output: Node, bundles, frames: Sequence[float] = (0,), scale: float = 1.0,
            window: Sequence[int] | None = None, depth: str = "float",
            **render_args: Any) -> dict[float, Image]:
    """Render output at each of frames; return {time: Image}. window is in pixels at the
    render scale (default: the RoD clipped to the project). render_args go to
    ofxhost.render() (host_binary, timeout...)."""
    order = topological_order(output)
    common: dict[str, Any] = dict(depth=depth, scale=scale, **render_args)
    sx, sy = (float(scale), float(scale)) if isinstance(scale, numbers.Real) else map(float, scale)

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
            r = render(bundles, node.plugin, node.context, inputs, node.params, frames=[t],
                       query=True, **common).report
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
            r = render(bundles, node.plugin, node.context, inputs, node.params, frames=[t],
                       window=win, query=True, **common).report
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
            r = render(bundles, node.plugin, node.context, inputs, node.params, frames=[t],
                       window=win, **common)
            images[(node, t)] = next(iter(r.images.values()), None)  # None: nothing rendered
    out = {}
    for t in frames:
        img = images.get((output, float(t)))
        if img is None:
            raise HostDemoError(f"{output!r} renders nothing at time {t} (its RoD doesn't intersect "
                                "the window, or the project if no window is given)")
        out[float(t)] = img
    return out
