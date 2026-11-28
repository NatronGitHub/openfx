"""What plug-ins declare: contexts, clips, parameters and flags, from describe().

Pattern: describe() returns hostDemo's --describe JSON, without creating an instance:
{"plugin": {...}, "contexts": {context: {"flags", "clips", "params"}}}. Clips and params are
lists, in definition order.
"""

FILTER = "OfxImageEffectContextFilter"
GENERAL = "OfxImageEffectContextGeneral"
PAINT = "OfxImageEffectContextPaint"


def by_name(items):
    return {item["name"]: item for item in items}


def test_contexts_and_clips(describe):
    """Basic's label and version, its three contexts, its clips in definition order, the optional Alpha mask of the
    general context, and the paint context's mandatory Brush. (#120)"""
    d = describe("net.sf.openfx.basicPlugin")
    assert d["plugin"]["label"] == "Gain" and d["plugin"]["versionMajor"] == 1
    assert sorted(d["contexts"]) == sorted([FILTER, GENERAL, PAINT])
    assert [c["name"] for c in d["contexts"][FILTER]["clips"]] == ["Source", "Output"]
    mask = by_name(d["contexts"][GENERAL]["clips"])["Mask"]
    assert mask["optional"] and mask["isMask"]
    assert mask["supportedComponents"] == ["OfxImageComponentAlpha"]
    brush = by_name(d["contexts"][PAINT]["clips"])["Brush"]  # mandatory in the paint context
    assert not brush["optional"] and brush["isMask"]


def test_params(describe):
    """A double parameter's type, default, min, max and parent group, and the group's type. (#120)"""
    params = by_name(describe("net.sf.openfx.basicPlugin")["contexts"][FILTER]["params"])
    scale_r = params["scaleR"]
    assert scale_r["type"] == "OfxParamTypeDouble"
    assert (scale_r["default"], scale_r["min"], scale_r["max"]) == (1, 0, 10)
    assert scale_r["parent"] == "componentScales"  # in a group
    assert params["componentScales"]["type"] == "OfxParamTypeGroup"


def test_flags(describe):
    """The effect flags: Retimer's temporal clip access, Field's fieldRenderTwiceAlways. (#120)"""
    flags = describe("net.sf.openfx.retimer")["contexts"][FILTER]["flags"]
    assert flags["temporalClipAccess"]  # it fetches images at other times
    assert describe("net.sf.openfx.fieldPlugin")["contexts"][FILTER]["flags"]["fieldRenderTwiceAlways"]


def test_secret_param(describe):
    # Tester (GenericTest) only renders an invert, but declares a variety of parameters
    """Tester's secret and visible parameters, and its push button's type. (#120)"""
    params = by_name(describe("net.sf.openfx.GenericTestPlugin")["contexts"][FILTER]["params"])
    assert params["secretTest"]["secret"] and not params["Insignificant"]["secret"]
    assert params["pbButton"]["type"] == "OfxParamTypePushButton"
