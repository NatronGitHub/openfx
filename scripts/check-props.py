#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Validate the inline property metadata of the NatronGitHub/openfx headers.

Runs upstream's scripts/gen-props.py checks, unchanged, with these adaptations
for the fork's extension headers (see ofx_fork_props.py):

1. @propdef metadata is also read from include/nuke/*.h and include/tuttle/*.h
   (upstream only reads include/ofx*.h);
2. extension properties (their @propdef has an `extension:` key, e.g.
   `extension: natron`) don't have to be listed in a standard @propset;
3. properties are the #defines with "Prop" in their name (upstream's rule), plus
   every #define that has a @propdef; enum values, aliases and NOT_PROPERTIES are not;
4. every `#define kName "string"` must be classified: property, alias, enum value
   or name (ofx_fork_props.classify). This catches properties without "Prop" in
   their name, which upstream's check misses.

The generated C++ headers are written to a temporary directory and discarded.
Exits with status 1 if any check reports an error.

usage: scripts/check-props.py [-v]
"""
import importlib.util
import logging
import sys
import tempfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
INCLUDE = SCRIPTS.parent / "include"
sys.path.insert(0, str(SCRIPTS))

import ofx_prop_utils  # noqa: E402

import ofx_fork_props  # noqa: E402


def extract_all_inline_metadata(include_dir):
    return ofx_fork_props.all_inline_metadata(include_dir)


ofx_prop_utils.extract_all_inline_metadata = extract_all_inline_metadata

spec = importlib.util.spec_from_file_location("gen_props", SCRIPTS / "gen-props.py")
gp = importlib.util.module_from_spec(spec)
sys.modules["gen_props"] = gp
spec.loader.exec_module(gp)

_get_props = gp.getPropertiesFromDir


def get_properties_from_dir(include_dir):
    classes, _ = ofx_fork_props.classify(include_dir)
    props = {p for p in _get_props(include_dir)
             if p not in ofx_fork_props.NOT_PROPERTIES and classes.get(p, ("property",))[0] == "property"}
    return sorted(props | {c for c, (kind, _) in classes.items() if kind == "property"})


gp.getPropertiesFromDir = get_properties_from_dir

_used_by_set = gp.check_props_used_by_set


def check_props_used_by_set(props_by_set, props_by_action, props_metadata):
    std = {k: v for k, v in props_metadata.items() if not v.get("extension")}
    return _used_by_set(props_by_set, props_by_action, std)


gp.check_props_used_by_set = check_props_used_by_set

errors = []
_error = logging.error


def counting_error(msg, *args, **kw):
    errors.append(msg)
    _error(msg, *args, **kw)


logging.error = counting_error


def main():
    verbose = "-v" in sys.argv[1:]
    logging.basicConfig(level=logging.INFO if verbose else logging.WARNING, format="%(levelname)s: %(message)s")
    with tempfile.TemporaryDirectory() as tmp:
        class Args:
            pass
        args = Args()
        args.verbose = verbose
        args.props_metadata = Path(tmp) / "ofxPropsMetadata.h"
        args.props_by_set = Path(tmp) / "ofxPropsBySet.h"
        args.propset_accessors = Path(tmp) / "ofxPropSetAccessors.h"
        args.propset_accessors_host = Path(tmp) / "ofxPropSetAccessorsHost.h"
        gp.dest_path = Path("/")
        gp.main(args)
    _, unclassified = ofx_fork_props.classify(INCLUDE)
    for header, cname, value in unclassified:
        logging.error(f"{header}: {cname} = \"{value}\" is not classified: add a @propdef if it is "
                      "a property, else list it in scripts/ofx_fork_props.py (EXTRA_VALUES or NAMES)")
    if errors:
        print(f"check-props: {len(errors)} error(s)", file=sys.stderr)
        sys.exit(1)
    print("check-props: OK")


if __name__ == "__main__":
    main()
