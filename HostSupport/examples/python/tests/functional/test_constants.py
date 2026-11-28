"""The generated ofxhost.constants module (scripts/gen-ofxhost-py.py)."""
from ofxhost import constants as k


def test_constants():
    """The generated ofxhost.constants module has the header values: status codes, a property name, a macro
    defined as another one, kOfxFlagInfiniteMax; ofxhost and its users take the constants from it, generated
    by scripts/gen-ofxhost-py.py (#121; CI checks that it is up to date)."""
    assert k.kOfxStatOK == 0
    assert k.kOfxStatFailed == 1
    assert k.kOfxImageEffectPropContext == "OfxImageEffectPropContext"
    assert k.kOfxActionDescribeInteract == k.kOfxActionDescribe  # macro defined as another one
    assert k.kOfxFlagInfiniteMax == 2**31 - 1
