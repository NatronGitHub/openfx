"""GPUGain: the CPU path of a plug-in with Metal, CUDA, OpenCL and OpenGL render: bytes, shorts and floats; RGBA,
RGB and Alpha.

Pattern: hostDemo renders on the CPU unless asked for a GPU (--gpu, tested in
HostSupport/examples/python/tests), and the plug-in declares only the GPU paths it was built with (none for
CUDA and OpenCL in the default builds). r, g and b are scaled by scale * scaleR (G, B); alpha by scaleA only, and
an Alpha image by scaleA. Unchecking scaleComponents ignores the per-component scales. Integer depths are
clamped to [0, 1] and rounded to the nearest value (halves up); floats are not clamped.
"""

import numpy as np
import pytest

from ofxhost.testing import assert_close

PLUGIN = "com.OpenFXSample.GPUGain"
FILTER = "OfxImageEffectContextFilter"
COMPONENTS = ["OfxImageComponentRGBA", "OfxImageComponentRGB", "OfxImageComponentAlpha"]

SRC = np.random.default_rng(1).random((16, 24, 4), dtype=np.float32)
GAINS = np.array([0.5, 2.0, 0.25, 0.5])  # the r, g, b and a gains
PARAMS = {"scaleR": GAINS[0], "scaleG": GAINS[1], "scaleB": GAINS[2], "scaleA": GAINS[3]}

k = np.arange(16 * 24 * 3).reshape(16, 24, 3) * 11 % 256
EXACT = (k / 255).astype(np.float32)  # exact at byte and short depth
EXACT_RGBA = np.dstack([EXACT, np.ones(EXACT.shape[:2], np.float32)])  # PFM inputs have alpha 1


def _corner(r, h=16, w=24):
    """GPUGain doesn't support multiple resolutions: its output is the project frame, the input padded with
    zeros. The input's corner (top-down, as SRC), after checking that the padding is 0."""
    out = r.output[::-1]  # bottom-up: the input is at the origin
    padding = np.ones(out.shape[:2], bool)
    padding[:h, :w] = False
    assert out.shape[:2] == (576, 720) and np.all(out[padding] == 0.)
    return out[:h, :w][::-1]


def _rounded(x, depth):
    """x clamped to [0, 1] and rounded to the nearest value of the depth, halves up (floats: x)."""
    if depth == "float":
        return x
    m = {"byte": 255, "short": 65535}[depth]
    return np.floor(np.clip(x, 0, 1) * m + 0.5) / m


def test_describe(describe):
    d = describe(PLUGIN)
    assert d["plugin"]["grouping"] == "OFX Example (Support)"
    ctx = d["contexts"][FILTER]
    assert ctx["flags"]["cpuRenderSupported"] == "true"
    clips = {c["name"]: c for c in ctx["clips"]}
    assert clips["Output"]["supportedComponents"] == COMPONENTS
    assert clips["Source"]["supportedComponents"] == COMPONENTS


def test_gain_float(render):
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float",
               params={"scale": 0.5, "scaleR": 2.0, "scaleG": 0.25, "scaleB": 4.0, "scaleA": 0.5})
    expected = SRC * np.array([0.5 * 2.0, 0.5 * 0.25, 0.5 * 4.0, 0.5], np.float32)
    assert_close(_corner(r), expected, atol=1e-6)


def test_float_rgba_as_upstream(render):
    """Float RGBA, upstream's sample's format, gives upstream's results bit for bit: each component times its
    float gain (the double scale product rounded to float), and isIdentity compares the double scales (so a
    scale of 1 + 1e-12, 1.f as a float, isn't an identity). The --gpu tests compare the OpenGL and OpenCL
    renders with these CPU renders."""
    scales = {"scale": 0.7, "scaleR": 1.3, "scaleG": 0.37, "scaleB": 9.1, "scaleA": 0.123}
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float", params=scales)
    gains = np.array([0.7 * 1.3, 0.7 * 0.37, 0.7 * 9.1, 0.123]).astype(np.float32)
    np.testing.assert_array_equal(_corner(r), SRC * gains)
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float", params={"scaleR": 1 + 1e-12})
    assert not r.report["renders"][0]["isIdentity"]["identity"]
    np.testing.assert_array_equal(_corner(r), SRC)


@pytest.mark.parametrize("depth", ["byte", "short", "float"])
def test_gain_exact(render, depth):
    """Exact results at each depth: g's gain 2 clamps at byte and short depth, and some products are halves
    (k * 0.5 and k * 0.25 at byte depth, and alpha 1 * 0.5), rounded up."""
    r = render(PLUGIN, inputs={"Source": EXACT}, params=PARAMS, depth=depth)
    assert r.report["renders"][0]["output"]["depth"] == depth
    assert_close(_corner(r), _rounded(EXACT_RGBA * GAINS, depth), atol=1e-6)


def test_gain_rgb_input(render):
    """An RGB input gives an RGB image and an RGB output, as the clips support RGB."""
    r = render(PLUGIN, inputs={"Source": EXACT}, params=PARAMS, depth="float", components={"Source": "rgb"})
    assert r.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentRGB"
    assert r.report["clipPreferences"]["clips"]["Output"]["components"] == "OfxImageComponentRGB"
    assert r.output.shape == (576, 720, 3)
    assert_close(_corner(r), EXACT * GAINS[:3], atol=1e-6)


@pytest.mark.parametrize("depth", ["byte", "short", "float"])
def test_gain_alpha_input(render, depth):
    """An Alpha input (the alpha of an RGBA array) gives an Alpha output, scaled by scaleA (scale is only on r, g
    and b)."""
    src = np.dstack([EXACT, EXACT[..., :1]])
    r = render(PLUGIN, inputs={"Source": src}, params=dict(PARAMS, scale=3.0), depth=depth,
               components={"Source": "alpha"})
    assert r.report["clipPreferences"]["clips"]["Output"]["components"] == "OfxImageComponentAlpha"
    assert r.output.shape == (576, 720, 1)
    assert_close(_corner(r), _rounded(EXACT[..., :1] * GAINS[3], depth), atol=1e-6)


@pytest.mark.parametrize("components, enabled", [
    ("rgba", {"scale": True, "scaleR": True, "scaleG": True, "scaleB": True, "scaleA": True}),
    ("rgb", {"scale": True, "scaleR": True, "scaleG": True, "scaleB": True, "scaleA": False}),
    ("alpha", {"scale": False, "scaleR": False, "scaleG": False, "scaleB": False, "scaleA": True}),
    ("rgba-off", {"scale": True, "scaleR": False, "scaleG": False, "scaleB": False, "scaleA": False}),
])
def test_enabled_params(render, components, enabled):
    """Only the scales that change the render are enabled: scale and scaleR/G/B for r, g and b, scaleA for
    alpha, the per-component ones only with scaleComponents ("rgba-off": an RGBA source without it). An Alpha
    source with only scale changed is an identity."""
    params = {"scale": 3.0}
    if components == "rgba-off":
        components, params["scaleComponents"] = "rgba", False
    r = render(PLUGIN, inputs={"Source": SRC}, params=params, depth="float", components={"Source": components})
    assert {p: r.report["params"][p]["enabled"] for p in enabled} == enabled
    assert r.report["renders"][0]["isIdentity"]["identity"] == (components == "alpha")


def test_components_off(render):
    # scaleComponents off: only scale, on r, g and b
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float",
               params={"scale": 3.0, "scaleR": 2.0, "scaleA": 0.5, "scaleComponents": False})
    assert_close(_corner(r), SRC * np.array([3.0, 3.0, 3.0, 1.0], np.float32), atol=1e-5)


def test_default_is_identity(render):
    r = render(PLUGIN, inputs={"Source": SRC}, depth="float")
    identity = r.report["renders"][0]["isIdentity"]
    assert identity["identity"] and identity["clip"] == "Source"
    assert_close(_corner(r), SRC)
