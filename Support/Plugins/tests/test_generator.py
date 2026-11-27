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

from ofxhost.testing import assert_close, views

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
    v = views()
    for key in v.keys(0) + v.keys(1):
        assert np.array_equal(a.outputs[key], b.outputs[key])
    for k0, k1 in zip(v.keys(0), v.keys(1)):
        assert not np.array_equal(a.outputs[k0], a.outputs[k1])  # the time is the seed


def test_noise_level(render):
    # values are uniform in [0, Noise): Noise=1 gives 5 times the default (0.2)
    default, one = noise(render), noise(render, Noise=1.0)
    assert 0 <= default.output.min() and default.output.max() < 0.2
    assert_close(one.output, 5 * default.output, rtol=1e-6)
    assert noise(render, Noise=0.0).output.max() == 0


def test_rgb_output(render):
    """An RGB output when the host supports RGB (hostDemo --components).

    --components sets the host's kOfxImageEffectPropSupportedComponents ("the components
    supported by a clip or host"). With rgbOutput, Noise asks for an RGB output in the general
    context if the host supports RGB: it gets RGB with --components rgba,rgb,alpha, and RGBA with
    the default (RGBA and Alpha). Its RGB noise is the RGB of its RGBA noise.
    """
    host_rgb = ["--components", "rgba,rgb,alpha"]
    rgb = render(PLUGIN, context="general", inputs=INPUTS, params={"rgbOutput": True}, frames=[0],
                 extra_args=host_rgb)
    rgba = render(PLUGIN, context="general", inputs=INPUTS, params={"rgbOutput": True}, frames=[0])
    assert rgb.report["clipPreferences"]["clips"]["Output"]["components"] == "OfxImageComponentRGB"
    assert rgba.report["clipPreferences"]["clips"]["Output"]["components"] == "OfxImageComponentRGBA"
    assert rgb.output.shape == (576, 720, 3) and rgba.output.shape == (576, 720, 4)
    assert np.array_equal(rgb.output, rgba.output[..., :3])
    # without rgbOutput, the host's components don't change the output
    plain = render(PLUGIN, context="general", inputs=INPUTS, frames=[0], extra_args=host_rgb)
    assert np.array_equal(plain.output, rgba.output)
