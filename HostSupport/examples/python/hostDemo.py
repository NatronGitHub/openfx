#!/usr/bin/env python3
"""Render an OFX plug-in with hostDemo through ofxhost at frames 0 and 1, save the outputs
as <output>.<frame>.npy, and print the report as JSON.

Example:
  python hostDemo.py -b Support/Plugins/Invert/Linux-64-release -p net.sf.openfx.invertPlugin \
      -i Source=input.npy -o out
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
    ap.add_argument("-o", "--output", default="Output", help="output prefix")
    ap.add_argument("--host", help="hostDemo binary (default: $OFXHOST_HOSTDEMO, build tree)")
    ap.add_argument("params", nargs="*", metavar="name=value")
    args = ap.parse_args(argv)

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
                           params=params, depth=args.depth, host_binary=args.host)
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
