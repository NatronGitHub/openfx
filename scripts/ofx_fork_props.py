# SPDX-License-Identifier: BSD-3-Clause
"""Property metadata of the NatronGitHub/openfx headers, shared by check-props.py and
gen-ofxhost-py.py.

The metadata is the upstream inline format (@propdef, see ofx_prop_utils.py), read from
include/ofx*.h and also from the extension headers in include/nuke/ and include/tuttle/.

Upstream's check only looks at #defines with "Prop" in their name, so it misses properties
named otherwise (e.g. kOfxParamInterpType, kNatronOfxHostIsNatron). Here every
`#define kName "string"` of include/ must be one of:
  - a property, with a @propdef;
  - an alias of a property (another macro with the same string, e.g. kOfxParamPropPersistent);
  - a value of an enum property: listed in its @propdef `values`, or in EXTRA_VALUES below;
  - a name that is not a property (NAMES below): suite, action, parameter type, clip or
    parameter name, host name...
check_classification() reports the others, so a new property can't be added without metadata.
"""
import re
from pathlib import Path

import ofx_prop_utils

SCRIPTS = Path(__file__).resolve().parent
INCLUDE = SCRIPTS.parent / "include"

# Valid values of an enum property that its @propdef doesn't list (C macro names, by property
# string name): extension values of standard properties, and deprecated values (ofxOld.h).
_COMPONENTS = ["kNatronOfxImageComponentXY", "kFnOfxImageComponentMotionVectors",
               "kFnOfxImageComponentStereoDisparity", "kOfxImageComponentYUVA"]
_DEPTHS = ["kOfxBitDepthByteBGR", "kOfxBitDepthShortBGR", "kOfxBitDepthFloatBGR"]
_CONTEXTS = ["kOfxImageEffectContextReader", "kOfxImageEffectContextWriter",
             "kNatronOfxImageEffectContextTracker"]
EXTRA_VALUES = {
    "OfxImageEffectPropContext": _CONTEXTS,
    "OfxImageEffectPropSupportedContexts": _CONTEXTS,
    "OfxImageEffectPropComponents": _COMPONENTS,
    "OfxImageEffectPropSupportedComponents": _COMPONENTS,
    "OfxImageClipPropUnmappedComponents": _COMPONENTS,
    "OfxImageEffectPropPixelDepth": _DEPTHS,
    "OfxImageEffectPropSupportedPixelDepths": _DEPTHS,
    "OfxImageClipPropUnmappedPixelDepth": _DEPTHS,
    "OfxParamPropDoubleType": [
        # Vegas
        "kOfxParamDoubleTypePolar", "kOfxParamDoubleTypeChrominance",
        # ofxOld.h
        "kOfxParamDoubleTypeNormalisedX", "kOfxParamDoubleTypeNormalisedY",
        "kOfxParamDoubleTypeNormalisedXAbsolute", "kOfxParamDoubleTypeNormalisedYAbsolute",
        "kOfxParamDoubleTypeNormalisedXY", "kOfxParamDoubleTypeNormalisedXYAbsolute",
    ],
    # the @propdef of OfxPropType is a plain string; these are the object types
    "OfxPropType": ["kOfxTypeImageEffectHost", "kOfxTypeImageEffect", "kOfxTypeImageEffectInstance",
                    "kOfxTypeClip", "kOfxTypeImage", "kOfxTypeParameter", "kOfxTypeParameterInstance"],
    # int properties with named values
    "OfxParamPropLayoutHint": ["kOfxParamPropLayoutHintNormal", "kOfxParamPropLayoutHintDivider",
                               "kOfxParamPropLayoutHintNoNewLine"],
    "NatronOfxParamPropInViewerContextLayoutHint": [
        "kNatronOfxParamPropInViewerContextLayoutHintNormal",
        "kNatronOfxParamPropInViewerContextLayoutHintNormalDivider",
        "kNatronOfxParamPropInViewerContextLayoutHintAddNewLine"],
    # special children of a page parameter
    "OfxParamPropPageChild": ["kOfxParamPageSkipRow", "kOfxParamPageSkipColumn"],
}

# String #defines that are neither properties nor property values: (regex on the C macro
# name, what it is).
NAMES = [
    (r"k\w+Suite(V\d+)?", "suite name"),
    (r"k\w*Action\w*", "action name"),
    (r"k(Fn)?OfxParamType\w+", "parameter type"),
    (r"kOfxMessage\w+", "message type"),
    (r"kOfx(Colourspace\w+|ConfigIdentifier)", "colourspace or colour config name (ofxColour.h, ofx-native-*.h)"),
    (r"kOfxImageEffectPluginApi", "plug-in API name"),
    (r"kOfxImageEffect(Output|SimpleSource|TransitionSourceFrom|TransitionSourceTo)ClipName",
     "standard clip name"),
    (r"kOfxImageEffect(File|Proxy|Transition|Retimer)ParamName", "standard parameter name"),
    (r"(kNatronOfxHostName|kOfxSonyVegas\w*HostName|kOfxVegasProHostName)", "host name"),
    (r"kNukeOfxCameraParam\w+", "Nuke camera parameter name"),
    (r"kNatronOfxParam(StringSublabelName|OutputChannels|Process[RGBA](Label|Hint)?|UndoRedoText"
     r"|UndoRedoState|RightClickMenu|SelectionRectangleState)",
     "Natron parameter name, label or hint"),
    (r"kNatronParam(Tracking\w+|Format\w+)", "Natron parameter name"),
    (r"kNatronOfxImageComponentsPlane\w+", "Natron multi-plane component encoding"),
    (r"kNatronNativeOverlay\w+", "key in the kNatronOfxPropNativeOverlays encoding"),
    (r"kFnOfxImageEffectActionGetClipComponentsPropString",
     "prefix of the clip property names in kFnOfxImageEffectActionGetClipComponents"),
]

# #defines with "Prop" in their name that aren't properties, for upstream's check (it finds
# properties by name). Values listed in a @propdef or EXTRA_VALUES, and aliases, are found
# automatically and don't need to be listed here.
NOT_PROPERTIES = {
    "kFnOfxImageEffectActionGetClipComponentsPropString",
}

_STRING_DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(k\w+)[ \t]+"([^"]*)"', re.M)


def headers(include_dir=INCLUDE):
    include_dir = Path(include_dir)
    return sorted(include_dir.glob("ofx*.h")) + sorted((include_dir / "nuke").glob("*.h")) \
        + sorted((include_dir / "tuttle").glob("*.h"))


def all_inline_metadata(include_dir=INCLUDE):
    """{string name: {"metadata", "doc", "cname", "header"}} for every @propdef."""
    result = {}
    for header in headers(include_dir):
        for name, data in ofx_prop_utils.extract_inline_metadata(header).items():
            if name in result:
                raise ValueError(f"Duplicate property {name} found in {header} and previous definition")
            data["header"] = str(header.relative_to(Path(include_dir)))
            result[name] = data
    return result


def string_defines(include_dir=INCLUDE):
    """[(header, cname, string)] for every `#define kName "string"`."""
    out = []
    for header in headers(include_dir):
        for cname, value in _STRING_DEFINE.findall(header.read_text()):
            out.append((str(header.relative_to(Path(include_dir))), cname, value))
    return out


def classify(include_dir=INCLUDE):
    """Classify every string #define. Returns (classes, unclassified):
    classes is {cname: (kind, detail)} with kind in property, alias, value, name;
    unclassified is [(header, cname, string)]."""
    md = all_inline_metadata(include_dir)
    prop_cnames = {d["cname"] for d in md.values()}
    enum_values = {}
    for name, d in md.items():
        for v in d["metadata"].get("values") or []:
            enum_values.setdefault(v, name)
    extra = {c: prop for prop, cs in EXTRA_VALUES.items() for c in cs}
    names = [(re.compile(rx), why) for rx, why in NAMES]
    classes, unclassified = {}, []
    for header, cname, value in string_defines(include_dir):
        if cname in prop_cnames:
            classes[cname] = ("property", value)
        elif value in md:
            classes[cname] = ("alias", md[value]["cname"])
        elif value in enum_values:
            classes[cname] = ("value", enum_values[value])
        elif cname in extra:
            classes[cname] = ("value", extra[cname])
        else:
            why = next((w for rx, w in names if rx.fullmatch(cname)), None)
            if why:
                classes[cname] = ("name", why)
            else:
                unclassified.append((header, cname, value))
    # int-valued extra values (layout hints) aren't string defines
    for c, prop in extra.items():
        classes.setdefault(c, ("value", prop))
    return classes, unclassified
