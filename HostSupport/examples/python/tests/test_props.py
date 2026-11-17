"""The generated ofxhost.constants and ofxhost.props modules (scripts/gen-ofxhost-py.py)."""
from ofxhost import constants as k
from ofxhost.props import ALIASES, BY_CNAME, PROPS


def test_constants():
    assert k.kOfxStatOK == 0
    assert k.kOfxStatFailed == 1
    assert k.kOfxImageEffectPropContext == "OfxImageEffectPropContext"
    assert k.kOfxActionDescribeInteract == k.kOfxActionDescribe  # macro defined as another one
    assert k.kOfxFlagInfiniteMax == 2**31 - 1


def test_props():
    p = PROPS[k.kOfxImageEffectPropSupportedPixelDepths]
    assert (p.type, p.dimension, p.extension) == ("enum", 0, None)
    assert k.kOfxBitDepthFloat in p.values
    assert k.kOfxBitDepthFloatBGR in p.values  # Vegas extension value
    assert PROPS[k.kOfxImageEffectPropProjectSize][:4] == (
        "OfxImageEffectPropProjectSize", "kOfxImageEffectPropProjectSize", "double", 2)
    assert PROPS[k.kNatronOfxHostIsNatron].extension == "natron"
    # ofxNatron.h extension 13: keyboard modifiers (NatronGitHub/openfx#131)
    p = PROPS[k.kNatronOfxInteractPropKeyModifiers]
    assert (p.type, p.dimension, p.extension) == ("int", 1, "natron")
    p = PROPS[k.kNatronOfxHostPropInteractKeyModifiers]
    assert (p.type, p.dimension, p.extension) == ("bool", 1, "natron")
    assert (k.kNatronOfxKeyModifierShift, k.kNatronOfxKeyModifierControl,
            k.kNatronOfxKeyModifierAlt, k.kNatronOfxKeyModifierMeta) == (1, 2, 4, 8)
    assert PROPS[k.kOfxParamPropDefault].type == ("int", "double", "string", "pointer")
    assert PROPS[k.kOfxParamInterpType].values == (
        k.kOfxParamInterpTypeConstantStep, k.kOfxParamInterpTypeLinear, k.kOfxParamInterpTypeSmooth)


def test_every_property_has_its_constant():
    for name, p in PROPS.items():
        assert getattr(k, p.cname) == name
        types = p.type if isinstance(p.type, tuple) else (p.type,)
        assert set(types) <= {"string", "enum", "int", "bool", "double", "pointer"}, name
        assert p.type != "enum" or p.values, name


def test_aliases():
    assert ALIASES["kOfxParamPropPersistent"] == "kOfxParamPropPersistant"
    assert BY_CNAME["kOfxParamPropPersistent"] is PROPS["OfxParamPropPersistant"]
