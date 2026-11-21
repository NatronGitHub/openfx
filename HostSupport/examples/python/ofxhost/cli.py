"""The command line of the sample host (ofxhost.graphhost), after pyofx's:

    python -m ofxhost [-b DIR]... [--loglevel LEVEL] COMMAND ...

    list [TEXT]                     the plug-ins (those whose id or label contains TEXT)
    params PLUGIN [-c CTX] [-j]     a plug-in's parameters; -j: a one-node graph (JSON) with their
                                    defaults, to edit and render
    filter PLUGIN IN OUT [-c CTX] [-s NAME=VALUE]... [--frames F] [--save-graph G.json]
                                    render a plug-in on an image or a sequence (IN "in.####.pfm"),
                                    into OUT ("out.####.pfm"); the project is IN's image size
    render GRAPH.json [--frames F] [--scale S] [--report R.json] [--save-graph G.json] [--dump]
                                    run a graph (ofxhost.graph's JSON) and write its outputs;
                                    --dump prints the graph as JSON (normalised) instead

-b: a plug-in directory (or .ofx.bundle), repeatable; $OFX_PLUGIN_PATH is scanned too. Frames F:
"1-10", "0-20x2" (a step), or "1,3,5". -s NAME=VALUE: VALUE is JSON if it parses (1.5, true,
[1, 2]), else a string (a choice's option label, a file name); -s NAME presses a push button.
Images: .pfm and .npy (ofxhost.imageio). The exit status is 0 if it worked, 1 if the run failed,
2 for a usage error.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys

from . import graph as g
from . import graphhost, sequences
from .imageio import read_image

__all__ = ["main", "parse_frames"]

log = logging.getLogger("ofxhost")


def parse_frames(text: str) -> list[float]:
    """ "1-10" (inclusive), "0-20x2" (with a step), "1,3,5", or a mix: "1-3,10". """
    out: list[float] = []
    for part in text.split(","):
        part = part.strip()
        rng, _, step = part.partition("x")
        a, sep, b = rng[1:].partition("-") if rng.startswith("-") else rng.partition("-")
        if rng.startswith("-"):
            a = "-" + a
        try:
            if not sep:
                out.append(float(part))
                continue
            first, last, s = float(a), float(b), float(step) if step else 1.
        except ValueError:
            raise argparse.ArgumentTypeError(f"invalid frames {text!r} (e.g. 1-10, 0-20x2, 1,3,5)") from None
        if s <= 0 or last < first:
            raise argparse.ArgumentTypeError(f"invalid frame range {part!r}")
        n = int((last - first) / s + 1e-9)
        out += [first + k * s for k in range(n + 1)]
    return list(dict.fromkeys(out))


def _value(text: str):
    """A parameter value from the command line: JSON if it parses, else the string."""
    try:
        return json.loads(text)
    except ValueError:
        return text


def _plugin(pid: str):
    p = graphhost.ofxh.get_plugin(pid)
    if p is None:
        raise SystemExit(f"error: plug-in {pid} not found (add its directory with -b, or to OFX_PLUGIN_PATH)")
    return p


def _context(plugin, context: str | None) -> str:
    if context is None:
        return "filter" if "filter" in plugin.contexts else plugin.contexts[0]
    if context not in plugin.contexts:
        raise SystemExit(f"error: {plugin.id} has no {context} context (its contexts: {', '.join(plugin.contexts)})")
    return context


def cmd_list(args) -> int:
    for p in sorted(graphhost.ofxh.plugins(), key=lambda p: p.id):
        if args.text and args.text.lower() not in (p.id + " " + p.label).lower():
            continue
        print(f"{p.id} {p.version[0]}.{p.version[1]}  {p.label}  [{', '.join(p.contexts)}]")
    return 0


def template(plugin, context: str) -> g.Graph:
    """A one-node graph of a plug-in, with the defaults of its parameters (ofxh.param_template()), its
    input clips connected to hostDemo's default pattern (the optional ones to none), and its output
    written to <name>.####.pfm."""
    d = plugin.descriptor(context)
    name = plugin.id.replace(":", ".").split(".")[-1] or "node"
    node = g.Node(plugin.id, context, dict(graphhost.ofxh.param_template(plugin, context)), name=name,
                  version=tuple(plugin.version))
    node.inputs = {c.name: ("none" if c.is_optional else "default") for c in d.clips if not c.is_output}
    gr = g.Graph.of(node)
    gr.outputs[name] = name + ".####.pfm"
    gr.frames = [0., 1.]
    return gr


def cmd_params(args) -> int:
    plugin = _plugin(args.plugin)
    context = _context(plugin, args.context)
    if args.json:
        sys.stdout.write(template(plugin, context).dumps())
        return 0
    defaults = graphhost.ofxh.param_template(plugin, context)
    print(f"{plugin.id} {plugin.version[0]}.{plugin.version[1]} ({plugin.label}), {context} context")
    d = plugin.descriptor(context)
    print("clips: " + ", ".join(c.name + (" (optional)" if c.is_optional else "") for c in d.clips))
    for p in d.params:
        line = f"  {p.name}: {p.type}"
        if p.name in defaults:
            line += f" = {json.dumps(defaults[p.name])}"
        if p.label and p.label != p.name:
            line += f"  ({p.label})"
        if p.type == "choice" and p.choices:
            line += "  options: " + ", ".join(f"{k}={o!r}" for k, o in enumerate(p.choices))
        print(line)
    return 0


def _run(args, graph: g.Graph) -> int:
    if getattr(args, "save_graph", None):
        graph.save(args.save_graph)
    r = graphhost.run(graph, args.bundles, frames=getattr(args, "frames", None), scale=getattr(args, "scale", None),
                      use_env_path=True, report_path=getattr(args, "report", None))
    for f in r.files:
        print(f)
    if not r.ok:
        print(f"error: {r.report['error']}", file=sys.stderr)
    return 0 if r.ok else 1


def cmd_filter(args) -> int:
    still = not sequences.has_pattern(args.input)
    fs = [] if still else sequences.frames(args.input)
    if not still and not fs:
        raise SystemExit(f"error: {args.input}: no files of this sequence")
    if not still and not sequences.has_pattern(args.output):
        raise SystemExit(f"error: {args.output}: the input is a sequence, the output needs a frame number (####)")
    first = args.input if still else sequences.expand(args.input, fs[0])
    try:
        h, w = read_image(first).shape[:2]
    except (OSError, ValueError) as e:
        raise SystemExit(f"error: {e}")
    with graphhost.host(graphhost.plugin_dirs_of(args.bundles), use_env_path=True):
        plugin = _plugin(args.plugin)
        context = _context(plugin, args.context)
        clips = [c.name for c in plugin.descriptor(context).clips if not c.is_output]
        pid = plugin.id
    params = {}
    for s in args.set or []:
        name, eq, value = s.partition("=")
        params[name] = _value(value) if eq else None  # (no value: press a push button)
    src = "Source" if "Source" in clips else (clips[0] if clips else None)
    if src is None:
        raise SystemExit(f"error: {pid} has no input clip")
    node = g.Node(pid, context, params, inputs={src: g.Files(os.path.abspath(args.input))},
                  name=pid.replace(":", ".").split(".")[-1] or "node")
    graph = g.Graph.of(node, project=g.Project(size=(float(w), float(h)), pixel_aspect_ratio=1.,
                                               frame_range=(0., 0.) if still else (float(fs[0]), float(fs[-1]))))
    graph.outputs[node.name] = os.path.abspath(args.output)
    return _run(args, graph)


def cmd_render(args) -> int:
    try:
        graph = g.load(args.graph)
    except (OSError, ValueError) as e:
        raise SystemExit(f"error: {args.graph}: {e}")
    if args.dump:
        sys.stdout.write(graph.dumps())
        return 0
    return _run(args, graph)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m ofxhost", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="See ofxhost/cli.py and the README.")
    ap.add_argument("-b", "--bundles", action="append", default=[], metavar="DIR",
                    help="a plug-in directory or .ofx.bundle (repeatable); $OFX_PLUGIN_PATH is scanned too")
    ap.add_argument("--loglevel", default="warning", choices=["debug", "info", "warning", "error"],
                    help="the messages to show (the plug-ins' messages: info; log messages: debug)")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("list", help="list the plug-ins")
    p.add_argument("text", nargs="?", help="only those whose id or label contains this")
    p.set_defaults(func=cmd_list, needs_host=True)
    p = sub.add_parser("params", help="show a plug-in's parameters, or a graph template (-j)")
    p.add_argument("plugin")
    p.add_argument("-c", "--context", help="the context (default: filter, else the first one)")
    p.add_argument("-j", "--json", action="store_true", help="write a one-node graph with the defaults (JSON)")
    p.set_defaults(func=cmd_params, needs_host=True)
    p = sub.add_parser("filter", help="render a plug-in on an image or a sequence")
    p.add_argument("plugin")
    p.add_argument("input", help="an image, or a sequence (in.####.pfm)")
    p.add_argument("output", help="the output image or sequence (out.####.pfm)")
    p.add_argument("-c", "--context", help="the context (default: filter, else the first one)")
    p.add_argument("-s", "--set", action="append", metavar="NAME=VALUE", help="a parameter value (repeatable)")
    p.add_argument("--frames", type=parse_frames, help="the frames (default: the time domain)")
    p.add_argument("--save-graph", metavar="G.json", help="also save the graph it renders")
    p.add_argument("--report", metavar="R.json", help="write the JSON report")
    p.set_defaults(func=cmd_filter, needs_host=False)
    p = sub.add_parser("render", help="run a JSON graph")
    p.add_argument("graph")
    p.add_argument("--frames", type=parse_frames, help="the frames (default: the graph's, else the time domain)")
    p.add_argument("--scale", type=float, help="the render scale (default: the graph's)")
    p.add_argument("--report", metavar="R.json", help="write the JSON report")
    p.add_argument("--save-graph", metavar="G.json", help="save the graph (normalised) before rendering it")
    p.add_argument("--dump", action="store_true", help="print the graph as JSON, and don't render")
    p.set_defaults(func=cmd_render, needs_host=False)
    args = ap.parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.loglevel.upper()), format="%(levelname)s: %(message)s")
    try:
        if args.needs_host:
            with graphhost.host(graphhost.plugin_dirs_of(args.bundles), use_env_path=True):
                return args.func(args)
        return args.func(args)
    except FileNotFoundError as e:  # (a plug-in directory that doesn't exist)
        print(f"error: {e}", file=sys.stderr)
        return 2
    except SystemExit as e:
        if isinstance(e.code, str):
            print(e.code, file=sys.stderr)
            return 2
        raise


if __name__ == "__main__":
    sys.exit(main())
