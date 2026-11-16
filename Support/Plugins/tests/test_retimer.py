"""Retimer: a temporal effect.

Patterns:
- frames needed, from the report (framesNeeded: the input frame ranges for each output time);
- an output that depends on the time and on a parameter. The "default" input of hostDemo is
  a grey level t at time t (with t drawn in the reverse colour, away from the corners). The
  filter context output at time t is the input at time integral(speed, 0, t), blended
  between the two nearest frames: at t=1, the grey level is the speed;
- the time domain (general context), from the report.
"""

import math

import pytest

from ofxhost.testing import assert_close

PLUGIN = "net.sf.openfx.retimer"


def test_frames_needed(render):
    # the frames it fetches: the two around the source time integral(speed, 0, t) = speed * t
    r = render(PLUGIN, inputs={"Source": "default"}, params={"Speed": 0.5})
    for item in r.report["renders"]:
        src = math.floor(0.5 * item["time"])
        assert item["framesNeeded"]["clips"] == {"Source": [[src, src + 1]]}


@pytest.mark.parametrize("speed", [0.0, 0.5, 1.0, 2.0])
def test_speed(render, speed):
    r = render(PLUGIN, inputs={"Source": "default"}, params={"Speed": speed})
    assert_close(r.outputs[0][0, 0, :3], 0.0)  # source time 0 at t=0
    assert_close(r.outputs[1][0, 0, :3], speed, atol=1e-6)  # source time = speed at t=1


def test_time_domain(render):
    # general context: the output lasts Duration times the input ([0, 25] for "default")
    r = render(PLUGIN, context="general", inputs={"Source": "default"}, params={"Duration": 2})
    assert r.report["timeDomain"]["range"] == [0, 50]
