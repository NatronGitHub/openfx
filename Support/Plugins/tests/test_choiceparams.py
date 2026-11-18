"""ChoiceParams: Choice params whose UI order differs from the index, and a StrChoice param.

Pattern: a Choice param's value is the option index, whatever its kOfxParamPropChoiceOrder (UI
order, OFX 1.5): green's options are appended as none, lots, some, so index 1 is "lots" (1.0)
and index 2 "some" (0.5). A StrChoice param's value is the option enum ("blue_0.5"). hostDemo
supports StrChoice; on hosts without it, the plug-in doesn't define blue_choice and the blue
scale is 1. Alpha is never scaled.
"""

import numpy as np

from ofxhost.testing import assert_close

PLUGIN = "org.openeffects.support.choiceParamsPlugin"
FILTER = "OfxImageEffectContextFilter"

SRC = np.random.default_rng(2).random((16, 24, 4), dtype=np.float32)
RED = [0.0, 0.5, 1.0]  # red_choice index -> scale
GREEN = [0.0, 1.0, 0.5]  # green_choice index -> scale (appended out of UI order)
BLUE = {"blue_0.0": 0.0, "blue_0.5": 0.5, "blue_1.0": 1.0}


def test_describe(describe):
    params = {p["name"]: p for p in describe(PLUGIN)["contexts"][FILTER]["params"]}
    assert params["red_choice"]["choiceOptions"] == ["red: none", "red: some", "red: lots"]
    assert params["green_choice"]["choiceOptions"] == ["green: none", "green: lots", "green: some"]
    blue = params["blue_choice"]
    assert blue["type"] == "OfxParamTypeStrChoice" and blue["default"] == "blue_0.0"


def test_defaults(render):
    # every choice defaults to its first option, scale 0
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float")
    assert_close(r.output, SRC * np.array([0, 0, 0, 1], np.float32), atol=1e-6)


def test_choices(render):
    for ri, gi, bi in ((1, 1, "blue_0.5"), (2, 2, "blue_1.0"), (0, 1, "blue_0.0")):
        r = render(PLUGIN, inputs={"Source": SRC}, depth="float",
                   params={"red_choice": ri, "green_choice": gi, "blue_choice": bi})
        scales = np.array([RED[ri], GREEN[gi], BLUE[bi], 1.0], np.float32)
        assert_close(r.output, SRC * scales, atol=1e-6)


def test_byte(render):
    # integer depths: the scaled value is truncated, and clamped to [0, 255]
    src = (np.arange(16 * 24 * 3).reshape(16, 24, 3) * 7 % 256 / 255).astype(np.float32)
    r = render(PLUGIN, inputs={"Source": src}, depth="byte",
               params={"red_choice": 1, "green_choice": 1, "blue_choice": "blue_0.5"})
    expected = np.dstack([np.floor(src * 255 * np.array([0.5, 1.0, 0.5])) / 255,
                          np.ones(src.shape[:2])])
    assert_close(r.output, expected, atol=1e-6)
