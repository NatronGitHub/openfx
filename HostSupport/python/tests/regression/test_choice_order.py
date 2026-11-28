"""Regression (#357): kOfxParamPropChoiceOrder had no values by default. It follows OFX 1.5.1 now: at least the
options' length, an entry nobody set is its index, and the values the plug-in set are kept (ChoiceOrderHook)."""

import pytest

ofxh = pytest.importorskip("ofxh")

CHOICES = "org.openeffects.support.choiceParamsPlugin"  # red: no order; green: appended out of order; blue: StrChoice


class _Value(ofxh.Param):
    """A Python parameter that holds one value (no keys)."""

    def __init__(self):
        super().__init__()
        self.value = None

    def get(self, time):
        return self.initial if self.value is None else self.value

    def set(self, value, time=None):
        self.value = value


class _PythonChoices(ofxh.ImageEffectInstance):
    """ChoiceParams with its three choice parameters in Python (their props are the instance's)."""

    def new_param(self, desc):
        return _Value() if desc.name in ("red_choice", "green_choice", "blue_choice") else None


ORDER, OPTION, ENUM = "OfxParamPropChoiceOrder", "OfxParamPropChoiceOption", "OfxParamPropChoiceEnum"


def test_choice_order_follows_the_options(plugin_dir):
    """kOfxParamPropChoiceOrder: "Default - Zero-based ordinal list of same length as OfxParamPropChoiceOption"
    (OFX 1.5.1 ofxParam.h). An option appended gets its index, the plug-in's values are kept, and resetting the options
    resets the order: in the descriptors, and on the instance's (Python) parameters. Before #357 the property had no
    values by default (suiteHandleChecks checks the C++ side)."""
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    plugin = ofxh.get_plugin(CHOICES)
    desc = {p.name: p for p in plugin.descriptor("filter").params}
    assert desc["red_choice"].props[ORDER] == (0, 1, 2)  # (no order: the default)
    assert desc["green_choice"].props[ORDER] == (0, 2, 1)  # (the plug-in's)
    with ofxh.create_instance(plugin, "filter", _PythonChoices()) as inst:
        red, green, blue = inst.param("red_choice"), inst.param("green_choice"), inst.param("blue_choice")
        assert red.props[ORDER] == (0, 1, 2) and green.props[ORDER] == (0, 2, 1) and blue.props[ORDER] == (0, 1, 2)
        red.props[OPTION] = (*red.props[OPTION], "red: more")
        assert red.props[ORDER] == (0, 1, 2, 3)
        green.props[OPTION] = (*green.props[OPTION], "green: more")
        assert green.props[ORDER] == (0, 2, 1, 3)
        blue.props[ENUM] = (*blue.props[ENUM], "blue_2.0")
        blue.props[OPTION] = (*blue.props[OPTION], "blue: more")
        assert blue.props[ORDER] == (0, 1, 2, 3)
        green.props[OPTION] = ()
        assert green.props[ORDER] == ()
        green.props[OPTION] = ("a", "b")
        assert green.props[ORDER] == (0, 1)
