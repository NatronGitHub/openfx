"""Generator (Noise): a generator, without an input.

Patterns:
- no input: the optional Source clip is disconnected ("none"). hostDemo has no generator
  context, so the plug-in runs in the general context;
- an infinite RoD: the host renders the project size (PAL SD, 720x576);
- deterministic output: the same parameters give the same output. The sample has no seed
  parameter: the seed is the time, so frames 0 and 1 differ. It also depends on how the
  image is split between threads, so compare renders with each other, not with stored
  reference images;
- a parameter scales the output (Noise).
"""

import numpy as np

from ofxhost.testing import assert_close

PLUGIN = "net.sf.openfx.noisePlugin"
INPUTS = {"Source": "none"}


def noise(render, **params):
    return render(PLUGIN, context="general", inputs=INPUTS, params=params)


def test_infinite_rod(render):
    r = noise(render)
    first = r.report["renders"][0]
    assert first["regionOfDefinition"]["rod"] == [-2**31, -2**31, 2**31 - 1, 2**31 - 1]
    assert first["renderWindow"] == [0, 0, 720, 576]
    assert r.output.shape == (576, 720, 4)
    assert r.report["clipPreferences"]["frameVarying"]  # set in getClipPreferences


def test_deterministic(render):
    a, b = noise(render), noise(render)
    for t in (0, 1):
        assert np.array_equal(a.outputs[t], b.outputs[t])
    assert not np.array_equal(a.outputs[0], a.outputs[1])  # the time is the seed


def test_noise_level(render):
    # values are uniform in [0, Noise): Noise=1 gives 5 times the default (0.2)
    default, one = noise(render), noise(render, Noise=1.0)
    assert 0 <= default.output.min() and default.output.max() < 0.2
    assert_close(one.output, 5 * default.output, rtol=1e-6)
    assert noise(render, Noise=0.0).output.max() == 0
