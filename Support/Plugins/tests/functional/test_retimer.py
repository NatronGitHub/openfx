"""Retimer: a temporal effect.

Patterns:
- frames needed, from the report (framesNeeded: the input frame ranges for each output time);
- an output that depends on the time and on a parameter. The "default" input of hostDemo is
  a grey level t at time t (with t drawn in the reverse colour, away from the corners; with the
  Vegas extension, red in view 0 and cyan in view 1). The filter context output at time t is the
  input at time integral(speed, 0, t), blended between the two nearest frames: at t=1, the
  level is the speed;
- the time domain (general context), from the report.
"""

import math

import pytest

from ofxhost.testing import assert_close, views

PLUGIN = "net.sf.openfx.retimer"


def test_frames_needed(render):
    # the frames it fetches: the two around the source time integral(speed, 0, t) = speed * t
    """framesNeeded in the report: at time t, the two frames around the source time speed * t. (#120; fixed in #124)"""
    r = render(PLUGIN, inputs={"Source": "default"}, params={"Speed": 0.5})
    for item in r.report["renders"]:
        src = math.floor(0.5 * item["time"])
        assert item["framesNeeded"]["clips"] == {"Source": [[src, src + 1]]}


# the colour of the "default" input in views 0 and 1: grey, or with the Vegas extension red and cyan (its level, the
# time, multiplies it)
GREY, VEGAS_COLOURS = [(1., 1., 1.)] * 2, [(1., 0., 0.), (0., 1., 1.)]


@pytest.mark.parametrize("speed", [0.0, 0.5, 1.0, 2.0])
def test_speed(render, speed):
    """The output at time t is the input at the source time integral(speed, 0, t): at t=0 level 0, at t=1 the level is
    the speed, in each view (#377: grey, or red and cyan with the Vegas extension). (#120)"""
    r = render(PLUGIN, inputs={"Source": "default"}, params={"Speed": speed})
    v = views()
    colours = VEGAS_COLOURS if v.vegas else GREY
    assert v.count <= len(colours)
    for colour, k0, k1 in zip(colours, v.keys(0), v.keys(1)):
        assert_close(r.outputs[k0][0, 0, :3], 0.0)  # source time 0 at t=0
        # source time = speed at t=1: the level is the speed
        assert_close(r.outputs[k1][0, 0, :3], [speed * c for c in colour], atol=1e-6)


def test_time_domain(render):
    # general context: the output lasts Duration times the input ([0, 25] for "default")
    """General context: the time domain is Duration times the input's ([0, 25] for "default"). (#120)"""
    r = render(PLUGIN, context="general", inputs={"Source": "default"}, params={"Duration": 2})
    assert r.report["timeDomain"]["range"] == [0, 50]
