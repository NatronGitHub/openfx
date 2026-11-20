#!/usr/bin/env python3
"""Render an OFX plug-in, or a graph of plug-ins, with hostDemo through ofxhost, and save
the outputs as <output>.<frame>.npy. For one plug-in, print the report as JSON; for a graph
(-g graph.json, see ofxhost.graph.load), print the bounds of each output.

Examples:
  python hostDemo.py -b Support/Plugins/Invert/Linux-64-release -p net.sf.openfx.invertPlugin \
      -i Source=input.npy -o out
  python hostDemo.py -b Support/Plugins -g graph.json --frames 0,1 --scale 0.5 -o out
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ofxhost  # noqa: E402
import ofxhost.graph  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-b", "--bundles", action="append", required=True,
                    help="plug-in directory or .ofx.bundle (repeatable)")
    ap.add_argument("-p", "--plugin", help="plug-in identifier")
    ap.add_argument("-g", "--graph", help="render the output node of this JSON graph instead")
    ap.add_argument("--frames", help="times t0,t1,... (default: 0,1; 0 for a graph)")
    ap.add_argument("--scale", type=float, help="render scale, in (0,1]")
    ap.add_argument("--window", help="render window x1,y1,x2,y2, in pixels at the render scale")
    ap.add_argument("-c", "--context", default="filter")
    ap.add_argument("-i", "--input", action="append", default=[], metavar="CLIP=SPEC",
                    help="CLIP=file.npy, CLIP=file.pfm, or a hostDemo spec (none, ramp, "
                         "solid:r,g,b,a, checker:N)")
    ap.add_argument("-d", "--depth", default="float", choices=ofxhost.DEPTHS)
    ap.add_argument("-o", "--output", default="Output", help="output prefix")
    ap.add_argument("--host", help="hostDemo binary (default: $OFXHOST_HOSTDEMO, build tree)")
    ap.add_argument("--backend", choices=ofxhost.BACKENDS,
                    help="hostDemo in a subprocess, or in-process on the ofxh module (default: "
                         "$OFXHOST_BACKEND, else subprocess)")
    ap.add_argument("params", nargs="*", metavar="name=value")
    args = ap.parse_args(argv)
    if args.host and args.backend == "inprocess":
        ap.error("--host is hostDemo's: not with --backend inprocess")
    if bool(args.plugin) == bool(args.graph):
        ap.error("give one of -p PLUGIN or -g GRAPH")
    frames = [float(f) for f in args.frames.split(",")] if args.frames else None
    window = [int(v) for v in args.window.split(",")] if args.window else None
    if args.graph:
        try:
            out, _ = ofxhost.graph.load(args.graph)
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:  # bad file, JSON or graph
            print(f"{args.graph}: invalid graph: {e!r}", file=sys.stderr)
            return 1
        try:
            images = ofxhost.graph.execute(out, args.bundles, frames=frames or [0],
                                           scale=args.scale or 1.0, window=window,
                                           depth=args.depth, host_binary=args.host,
                                           backend=args.backend)
        except (ofxhost.HostDemoError, ValueError, NotImplementedError) as e:  # failed run, cycle, frame range
            print(e, file=sys.stderr)
            return 1
        for t, img in images.items():
            np.save(f"{args.output}.{t:g}.npy", img.pixels)
        print(json.dumps({f"{t:g}": {"bounds": img.bounds, "premult": img.premult}
                          for t, img in images.items()}, indent=2))
        return 0

    inputs = {}
    for spec in args.input:
        clip, sep, value = spec.partition("=")
        if not sep:
            ap.error(f"-i {spec}: expected CLIP=SPEC")
        if value.endswith(".npy"):
            inputs[clip] = np.load(value, allow_pickle=False)
        elif value.endswith(".pfm") and not value.startswith("file:"):
            inputs[clip] = Path(value)
        else:
            inputs[clip] = value
    bad = [p for p in args.params if "=" not in p]
    if bad:
        ap.error("parameters must be name=value: " + " ".join(bad))
    params = dict(p.partition("=")[::2] for p in args.params)
    try:
        r = ofxhost.render(args.bundles, args.plugin, context=args.context, inputs=inputs,
                           params=params, depth=args.depth, frames=frames, scale=args.scale,
                           window=window, host_binary=args.host, backend=args.backend)
    except ofxhost.HostDemoError as e:
        print(e, file=sys.stderr)
        return 1
    sys.stderr.write(r.stderr)
    for key, img in r.outputs.items():  # key: frame, or (frame, "l"|"r")
        name = "".join(map(str, key)) if isinstance(key, tuple) else key
        np.save(f"{args.output}.{name}.npy", img)
    print(json.dumps(r.report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
