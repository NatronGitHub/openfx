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

Limits: the query in step 1 doesn't know the inputs' RoDs yet (they are only rendered in
step 2), so a node is queried with project-sized inputs ("default"). And hostDemo's inputs
are the same image at all times, so a node may need only one frame of each input Node.
"""

from __future__ import annotations

import dataclasses
import json
import math
import os
from collections.abc import Mapping, Sequence
from typing import Any

from . import Image, render

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
                v = "file:" + os.path.join(base, v[5:])
            nodes[name].inputs[clip] = v
    if doc.get("output") not in nodes:
        raise ValueError(f"unknown output node {doc.get('output')!r}")
    return nodes[doc["output"]], nodes


def _union(a, b):
    if a is None:
        return b
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def _frames(ranges: Sequence[Sequence[float]]) -> list[float]:
    """The frames in the frames-needed ranges: the integer frames of each [min,max] range,
    or min if it has none."""
    out: list[float] = []
    for lo, hi in ranges:
        fs = [float(f) for f in range(math.ceil(lo), math.floor(hi) + 1)] or [float(lo)]
        out += [f for f in fs if f not in out]
    return out


def execute(output: Node, bundles, frames: Sequence[float] = (0,), scale: float = 1.0,
            window: Sequence[int] | None = None, depth: str = "float",
            **render_args: Any) -> dict[float, Image]:
    """Render output at each of frames; return {time: Image}. window is in pixels at the
    render scale (default: the RoD clipped to the project). render_args go to
    ofxhost.render() (host_binary, timeout...)."""
    order = topological_order(output)
    common: dict[str, Any] = dict(depth=depth, scale=scale, **render_args)
    asked: dict[Node, dict[float, Any]] = {output: {float(t): window for t in frames}}
    needs: dict[tuple[Node, float], dict[str, float]] = {}  # (node, t) -> {clip: frame}

    # 1. outputs first: what does each node need from its input nodes?
    for node in reversed(order):
        children = {c: v for c, v in node.inputs.items() if isinstance(v, Node)}
        if not children:
            continue
        inputs = {c: ("default" if isinstance(v, Node) else v) for c, v in node.inputs.items()}
        for t, win in sorted(asked.get(node, {}).items()):
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
                if roi is None:
                    continue
                par = clips[clip]["pixelAspectRatio"]
                sx, sy = x["renderScale"]
                px = (math.floor(roi[0] * sx / par), math.floor(roi[1] * sy),
                      math.ceil(roi[2] * sx / par), math.ceil(roi[3] * sy))
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
                    v = images.get((v, f)) if f is not None else None
                    if v is None:  # not needed (empty RoI, nothing to render): transparent
                        v = "solid:0,0,0,0"
                inputs[clip] = v
            r = render(bundles, node.plugin, node.context, inputs, node.params, frames=[t],
                       window=win, **common)
            images[(node, t)] = next(iter(r.images.values()))
    return {float(t): images[(output, float(t))] for t in frames}
