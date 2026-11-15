#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Validate the inline property metadata of the NatronGitHub/openfx headers.

Runs upstream's scripts/gen-props.py checks, unchanged, with three adaptations
for the fork's extension headers:

1. @propdef metadata is also read from include/nuke/*.h and include/tuttle/*.h
   (upstream only reads include/ofx*.h);
2. extension properties (their @propdef has an `extension:` key, e.g.
   `extension: natron`) don't have to be listed in a standard @propset;
3. a few extension #defines have "Prop" in their name but are values or aliases,
   not properties (NOT_PROPERTIES below); they are not required to have @propdef.

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

# extension #defines that look like properties but are values or aliases
NOT_PROPERTIES = {
    # Nuke: prefix for the names of props in kFnOfxImageEffectActionGetClipComponents
    "kFnOfxImageEffectActionGetClipComponentsPropString",
    # Natron / Nuke: values of the layout hint properties
    "kNatronOfxParamPropInViewerContextLayoutHintAddNewLine",
    "kNatronOfxParamPropInViewerContextLayoutHintNormal",
    "kNatronOfxParamPropInViewerContextLayoutHintNormalDivider",
    "kOfxParamPropLayoutHintDivider",
    "kOfxParamPropLayoutHintNoNewLine",
    "kOfxParamPropLayoutHintNormal",
    # Vegas: values of kOfxImageEffectPropRenderQuality, kOfxImageEffectPropVegasContext
    # and kOfxParamPropDoubleType
    "kOfxImageEffectPropRenderQualityBest",
    "kOfxImageEffectPropRenderQualityGood",
    "kOfxImageEffectPropRenderQualityPreview",
    "kOfxImageEffectPropVegasContextEvent",
    "kOfxImageEffectPropVegasContextEventFadeIn",
    "kOfxImageEffectPropVegasContextEventFadeOut",
    "kOfxImageEffectPropVegasContextGenerator",
    "kOfxImageEffectPropVegasContextMedia",
    "kOfxImageEffectPropVegasContextProject",
    "kOfxImageEffectPropVegasContextTrack",
    "kOfxImageEffectPropVegasContextUnknown",
    "kOfxParamPropColorWheelLevel",
    # alias of kOfxParamPropPersistant (same string value), which has the metadata
    "kOfxParamPropPersistent",
}

import ofx_prop_utils  # noqa: E402

_extract = ofx_prop_utils.extract_inline_metadata


def extract_all_inline_metadata(include_dir):
    include_dir = Path(include_dir)
    all_props = {}
    headers = sorted(include_dir.glob("ofx*.h")) + sorted((include_dir / "nuke").glob("*.h")) \
        + sorted((include_dir / "tuttle").glob("*.h"))
    for header in headers:
        for name, data in _extract(header).items():
            if name in all_props:
                raise ValueError(f"Duplicate property {name} found in {header} and previous definition")
            all_props[name] = data
    return all_props


ofx_prop_utils.extract_all_inline_metadata = extract_all_inline_metadata

spec = importlib.util.spec_from_file_location("gen_props", SCRIPTS / "gen-props.py")
gp = importlib.util.module_from_spec(spec)
sys.modules["gen_props"] = gp
spec.loader.exec_module(gp)

_get_props = gp.getPropertiesFromDir
gp.getPropertiesFromDir = lambda d: [p for p in _get_props(d) if p not in NOT_PROPERTIES]

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
    if errors:
        print(f"check-props: {len(errors)} error(s)", file=sys.stderr)
        sys.exit(1)
    print("check-props: OK")


if __name__ == "__main__":
    main()
