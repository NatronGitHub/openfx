"""The generated ofxhost.constants module (scripts/gen-ofxhost-py.py)."""
from ofxhost import constants as k


def test_constants():
    assert k.kOfxStatOK == 0
    assert k.kOfxStatFailed == 1
    assert k.kOfxImageEffectPropContext == "OfxImageEffectPropContext"
    assert k.kOfxActionDescribeInteract == k.kOfxActionDescribe  # macro defined as another one
    assert k.kOfxFlagInfiniteMax == 2**31 - 1
