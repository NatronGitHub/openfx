"""The command line of the sample host (ofxhost.graphhost), after pyofx's:

    python -m ofxhost [-b DIR]... [--loglevel LEVEL] list|params|filter|render ...

-b: a plug-in directory (or .ofx.bundle), repeatable; $OFX_PLUGIN_PATH is scanned too. list shows
the plug-ins, params a plug-in's parameters (-j: a one-node graph with their defaults), filter
renders a plug-in on an image or a sequence, render runs a JSON graph (ofxhost.graph). Frames:
"1-10", "0-20x2" (a step), or "1,3,5". -s NAME=VALUE: VALUE is JSON if it parses, else a string;
-s NAME presses a push button. Images: .pfm and .npy (ofxhost.imageio). The exit status is 0 if it
worked, 1 if it failed ("error: ..." on stderr), 2 for a usage error. The ofxh module is imported
after the command line is read: without PYTHONPATH, it is the one of the openfx build tree that
holds the -b (or $OFX_PLUGIN_PATH) directories, if there is one.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys

from . import graph as g
from . import plugin_dirs, sequences
from .build import OfxhNotFound, import_ofxh
from .constants import kOfxImageEffectSimpleSourceClipName
from .imageio import check_format, read_image
from .specs import parse_double

__all__ = ["main", "parse_frames"]

log = logging.getLogger("ofxhost")


class Failure(Exception):
    """The command failed (exit status 1), with this message."""


_MAX_TIME = 1e15  # (hostDemo's limit of a time)


def _range(part: str) -> tuple[float, float, float] | None:
    """(first, last, step) of "first-last[xstep]", else None.

    The "-" between first and last is one that isn't a sign: not first, and not after an exponent's "e" ("1e-3-2").
    """
    rng, _, step = part.partition("x")
    for k, c in enumerate(rng):
        if c == "-" and k > 0 and rng[k - 1] not in "eE":
            first, last = parse_double(rng[:k]), parse_double(rng[k + 1:])
            s = parse_double(step) if step else 1.
            return None if first is None or last is None or s is None else (first, last, s)
    return None


def parse_frames(text: str) -> list[float]:
    """The frames of the command line.

    Args:
        text: "1-10" (inclusive), "0-20x2" (with a step), "1,3,5", or a mix: "1-3,10"; decimal
            numbers, as hostDemo's --frames.

    Returns:
        The frames, in order, without duplicates.

    Raises:
        argparse.ArgumentTypeError: not frames, a range that is empty or has a step <= 0, or a time
            beyond +-1e15.
    """
    out: list[float] = []
    for part in text.split(","):
        part = part.strip()
        f = parse_double(part)
        if f is not None:
            out.append(f)
            continue
        r = _range(part)
        if r is None:
            raise argparse.ArgumentTypeError(f"invalid frames {text!r} (e.g. 1-10, 0-20x2, 1,3,5)")
        first, last, s = r
        if s <= 0 or last < first or not (abs(first) < _MAX_TIME and abs(last) < _MAX_TIME):
            raise argparse.ArgumentTypeError(f"invalid frame range {part!r}")
        n = int((last - first) / s + 1e-9)
        out += [first + k * s for k in range(n + 1)]
    if not all(abs(f) < _MAX_TIME for f in out):
        raise argparse.ArgumentTypeError(f"invalid frames {text!r}")
    return list(dict.fromkeys(out))


def _scale(text: str) -> float:
    try:
        s = float(text)
    except ValueError:
        s = float("nan")
    if not 0. < s <= 1.:
        raise argparse.ArgumentTypeError(f"invalid render scale {text!r}: it must be in (0, 1]")
    return s


def _value(text: str):
    """A parameter value from the command line: JSON if it parses, else the string."""
    try:
        return json.loads(text)
    except ValueError:
        return text


def _graphhost():
    """ofxhost.graphhost, which imports ofxh: imported after the command line is read."""
    from . import graphhost
    return graphhost


def _plugin(pid: str):
    p = _graphhost().ofxh.get_plugin(pid)
    if p is None:
        raise Failure(f"plug-in {pid} not found (add its directory with -b, or to OFX_PLUGIN_PATH)")
    return p


def _context(plugin, context: str | None) -> str:
    if context is None:
        return "filter" if "filter" in plugin.contexts else plugin.contexts[0]
    if context not in plugin.contexts:
        raise Failure(f"{plugin.id} has no {context} context (its contexts: {', '.join(plugin.contexts)})")
    return context


def cmd_list(args) -> int:
    for p in sorted(_graphhost().ofxh.plugins(), key=lambda p: p.id):
        if args.text and args.text.lower() not in (p.id + " " + p.label).lower():
            continue
        print(f"{p.id} {p.version[0]}.{p.version[1]}  {p.label}  [{', '.join(p.contexts)}]")
    return 0


def template(plugin, context: str) -> g.Graph:
    """A one-node graph of a plug-in, to edit and render.

    Args:
        plugin: the ofxh plug-in.
        context: one of its contexts.

    Returns:
        The graph: the defaults of the plug-in's parameters (ofxh.param_template(); the file
        parameters as {"file": path}), its mandatory input clips on hostDemo's default test pattern
        (the optional ones left out: disconnected), and its output written to <name>.####.pfm.
    """
    gh = _graphhost()
    d = plugin.descriptor(context)
    params = dict(gh.ofxh.param_template(plugin, context))
    for p in d.params:
        if p.name in params and gh.is_file_param(p, plugin.contexts):
            params[p.name] = g.FilePath(params[p.name])
    name = g.stem(plugin.id)
    node = g.Node(plugin.id, context, params, name=name, version=tuple(plugin.version))
    node.inputs = {c.name: "default" for c in d.clips if not c.is_output and not c.is_optional}
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
    defaults = _graphhost().ofxh.param_template(plugin, context)
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


def _run(args, graph) -> int:
    if getattr(args, "save_graph", None):
        try:
            graph.save(args.save_graph)
        except (OSError, ValueError) as e:
            raise Failure(f"--save-graph {args.save_graph}: {e}") from None
    r = _graphhost().run(graph, args.bundles, frames=getattr(args, "frames", None), scale=getattr(args, "scale", None),
                      use_env_path=True, report_path=getattr(args, "report", None))
    for f in r.files:
        print(f)
    if not r.ok:
        raise Failure(r.report["error"])
    return 0


def cmd_filter(args) -> int:
    still = not sequences.has_pattern(args.input)
    try:
        check_format(args.output)
        fs = [] if still else sequences.frames(args.input)
        if not still and not fs:
            raise Failure(f"{args.input}: no files of this sequence")
        if not still and not sequences.has_pattern(args.output):
            raise Failure(f"{args.output}: the input is a sequence, the output needs a frame number (####)")
        h, w = read_image(args.input if still else sequences.expand(args.input, fs[0])).shape[:2]
    except (OSError, ValueError) as e:
        raise Failure(str(e)) from None
    gh = _graphhost()
    with gh.host(plugin_dirs(args.bundles), use_env_path=True):
        plugin = _plugin(args.plugin)
        context = _context(plugin, args.context)
        d = plugin.descriptor(context)
        clips = [c.name for c in d.clips if not c.is_output]
        files = {p.name for p in d.params if gh.is_file_param(p, plugin.contexts)}
        pid = plugin.id
    params = {}
    for s in args.set or []:
        name, eq, value = s.partition("=")
        params[name] = _value(value) if eq else None  # (no value: press a push button)
        if name in files and isinstance(params[name], str):
            params[name] = g.FilePath(params[name])  # (a file parameter: saved graphs rewrite it)
    src = kOfxImageEffectSimpleSourceClipName if kOfxImageEffectSimpleSourceClipName in clips else \
        (clips[0] if clips else None)
    if src is None:
        raise Failure(f"{pid} has no input clip")
    node = g.Node(pid, context, params, inputs={src: g.Files(os.path.abspath(args.input))}, name=g.stem(pid))
    graph = g.Graph.of(node, project=g.Project(size=(float(w), float(h)), pixel_aspect_ratio=1.,
                                               frame_range=(0., 0.) if still else (float(fs[0]), float(fs[-1]))))
    graph.outputs[node.name] = os.path.abspath(args.output)
    return _run(args, graph)


def cmd_render(args) -> int:
    if args.dump:
        try:
            sys.stdout.write(g.load(args.graph).dumps())
        except (OSError, ValueError) as e:
            raise Failure(f"{args.graph}: {e}") from None
        return 0
    if args.save_graph:
        try:
            graph = g.load(args.graph)
        except (OSError, ValueError) as e:
            raise Failure(f"{args.graph}: {e}") from None
        return _run(args, graph)
    return _run(args, args.graph)  # (run() loads it, and reports a bad file as a failed run)


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="python -m ofxhost", description="The sample host (ofxhost.graphhost).",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="python -m ofxhost hostdemo [hostDemo's options]: hostDemo's command line, "
                                        "in-process (ofxhost.hostdemo). See ofxhost/cli.py and the README.")
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
    p.add_argument("--scale", type=_scale, help="the render scale, in (0, 1] (default: 1)")
    p.add_argument("--save-graph", metavar="G.json", help="also save the graph it renders")
    p.add_argument("--report", metavar="R.json", help="write the JSON report")
    p.set_defaults(func=cmd_filter, needs_host=False)
    p = sub.add_parser("render", help="run a JSON graph")
    p.add_argument("graph")
    p.add_argument("--frames", type=parse_frames, help="the frames (default: the graph's, else the time domain)")
    p.add_argument("--scale", type=_scale, help="the render scale, in (0, 1] (default: the graph's)")
    p.add_argument("--report", metavar="R.json", help="write the JSON report")
    p.add_argument("--save-graph", metavar="G.json", help="save the graph (normalised) before rendering it")
    p.add_argument("--dump", action="store_true", help="print the graph as JSON, and don't render")
    p.set_defaults(func=cmd_render, needs_host=False)
    return ap


def _import_host(bundles) -> None:
    """Import ofxh (without PYTHONPATH: the one of the build tree of the -b directories), then ofxhost.graphhost."""
    dirs = [os.path.abspath(b) for b in bundles] + os.environ.get("OFX_PLUGIN_PATH", "").split(os.pathsep)
    import_ofxh(os.pathsep.join(d for d in dirs if d), for_cli=True)
    _graphhost()


def main(argv=None) -> int:
    """python -m ofxhost: run the command line argv (default: sys.argv[1:]); the exit status."""
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv[:1] == ["hostdemo"]:
        from .hostdemo import run
        return run(argv[1:], "python -m ofxhost hostdemo")
    args = _parser().parse_args(argv)  # (a usage error exits with status 2)
    logging.basicConfig(level=getattr(logging, args.loglevel.upper()), format="%(levelname)s: %(message)s")
    try:
        _import_host(args.bundles)
        if args.needs_host:
            try:
                dirs = plugin_dirs(args.bundles)
            except OSError as e:
                raise Failure(str(e)) from None
            with _graphhost().host(dirs, use_env_path=True):
                return args.func(args)
        return args.func(args)
    except OfxhNotFound as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except Failure as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except OSError as e:  # (e.g. a plug-in directory that doesn't exist, in filter)
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
