#!/usr/bin/env python3
"""Render an OFX plug-in with hostDemo through ofxhost, save the outputs as .npy or PFM,
and print the stats (and the report, if hostDemo supports --report) as JSON.

Example:
  python hostDemo.py -b Support/Plugins/Invert/Linux-64-release -p net.sf.openfx.invertPlugin \
      -i Source=input.npy -o out --save npy
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ofxhost  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-b", "--bundles", action="append", required=True,
                    help="plug-in directory or .ofx.bundle (repeatable)")
    ap.add_argument("-p", "--plugin", required=True, help="plug-in identifier")
    ap.add_argument("-c", "--context", default="filter")
    ap.add_argument("-i", "--input", action="append", default=[], metavar="CLIP=SPEC",
                    help="CLIP=file.npy, CLIP=file.pfm, or a hostDemo spec (none, ramp, "
                         "solid:r,g,b,a, checker:N)")
    ap.add_argument("-d", "--depth", default="float", choices=ofxhost.DEPTHS)
    ap.add_argument("-f", "--frames", type=float, nargs="+")
    ap.add_argument("-o", "--output", default="Output", help="output prefix")
    ap.add_argument("--save", choices=("npy", "pfm", "none"), default="npy")
    ap.add_argument("--host", help="hostDemo binary (default: $OFXHOST_HOSTDEMO, build tree)")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--describe", action="store_true",
                    help="print the plug-in description (needs hostDemo --describe)")
    ap.add_argument("params", nargs="*", metavar="name=value")
    args = ap.parse_args(argv)

    try:
        if args.describe:
            print(json.dumps(ofxhost.describe(args.bundles, args.plugin, args.context,
                                              host_binary=args.host, timeout=args.timeout),
                             indent=2))
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
        params = {}
        for p in args.params:
            name, sep, value = p.partition("=")
            if not sep:
                ap.error(f"{p}: expected name=value")
            params[name] = value
        r = ofxhost.render(args.bundles, args.plugin, context=args.context, inputs=inputs,
                           params=params, depth=args.depth, frames=args.frames,
                           host_binary=args.host, timeout=args.timeout)
    except ofxhost.HostDemoError as e:
        print(e, file=sys.stderr)
        return 1

    sys.stderr.write(r.stderr)
    files = []
    for key, img in r.outputs.items():
        name = f"{args.output}.{key[0]}{key[1]}" if isinstance(key, tuple) else f"{args.output}.{key}"
        if args.save == "npy":
            files.append(f"{name}.npy")
            np.save(files[-1], img)
        elif args.save == "pfm":
            if img.shape[2] == 4:  # PFM has no RGBA: alpha goes to a separate Pf file
                ofxhost.write_pfm(f"{name}.alpha.pfm", img[..., 3])
                files.append(f"{name}.alpha.pfm")
                img = img[..., :3]
            ofxhost.write_pfm(f"{name}.pfm", img)
            files.append(f"{name}.pfm")
    print(json.dumps({"stats": r.stats, "report": r.report, "files": files}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
