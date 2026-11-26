"""kOfxImageEffectPropSupportsMultiResolution: hostDemo and ofxh report 1 (images of any size and offset). A plug-in
that declares 0 (GPUGain), or every plug-in in a host that reports 0 (hostDemo --ofx11, a Python host that sets 0),
gets every image in one frame, the project extent rooted at the origin: the output's RoD and every input's RoD and
image are that frame, each input cropped where it extends beyond it and padded with black and transparent pixels
where it falls short (ofxProcessingArch.rst: "pad images with an appropriate amount of black/transparent pixels to
enable them to work").
"""

import ctypes

import numpy as np
import pytest

import ofxhost
from ofxhost import constants as k

GPUGAIN = "com.OpenFXSample.GPUGain"  # kOfxImageEffectPropSupportsMultiResolution 0, no tiles
INVERT = "net.sf.openfx.invertPlugin"  # 1, tiles
BASIC = "net.sf.openfx.basicPlugin"  # 1, general context: Source and Mask
FRAME = (0., 0., 768., 576.)  # hostDemo's project extent (canonical)
FRAME_PX = (0, 0, 720, 576)  # in pixels (PAR 768/720)
SRC = np.random.default_rng(5).random((16, 24, 4), dtype=np.float32)  # top-down
AT = (100, 50, 124, 66)  # its pixel bounds: offset from the origin
GAINS = np.array([0.5, 0.5, 0.5, 1.], np.float32)  # GPUGain's scale 0.5 (scaleA stays 1)
IN_PROCESS = "a Python host that reports 0, or the image as the plug-in fetches it (ctypes)"

pytestmark = pytest.mark.usefixtures("tmp_cwd")


def _bottom_up(a):
    return a[::-1]


def _in(shape, b):
    """The mask of the pixels of bounds b in an image of the frame (bottom-up)."""
    m = np.zeros(shape[:2], bool)
    m[max(b[1], 0):b[3], max(b[0], 0):b[2]] = True
    return m


def _gpugain(sample_plugins, src, at, **kw):
    return ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": ofxhost.Image(src, at)}, depth="float",
                          frames=[0], params={"scale": 0.5}, **kw)


def test_non_multires_plugin_gets_the_frame(backend, sample_plugins):
    """GPUGain: its RoD (forced to the frame), render window and output are the frame; the input is padded."""
    r = _gpugain(sample_plugins, SRC, AT)
    x = r.report["renders"][0]
    assert x["regionOfDefinition"]["rod"] == list(FRAME)
    assert x["regionsOfInterest"]["clips"]["Source"] == list(FRAME)
    assert x["renderWindow"] == list(FRAME_PX)
    assert r.images[0].bounds == FRAME_PX
    out = _bottom_up(r.output)
    inside = _in(out.shape, AT)
    np.testing.assert_allclose(out[inside].reshape(16, 24, 4), _bottom_up(SRC) * GAINS, atol=1e-6)
    assert np.all(out[~inside] == 0.)
    # the report's input RoD is the host's own, before the crop and pad
    assert x["inputs"]["Source"]["regionOfDefinition"][0] > 0


def test_negative_offset_is_cropped(backend, sample_plugins):
    """An input at negative coordinates: the part at x < 0 or y < 0 is cropped."""
    at = (-10, -5, 14, 11)
    r = _gpugain(sample_plugins, SRC, at)
    assert r.images[0].bounds == FRAME_PX
    out = _bottom_up(r.output)
    np.testing.assert_allclose(out[0:11, 0:14], _bottom_up(SRC)[5:, 10:] * GAINS, atol=1e-6)
    assert np.all(out[~_in(out.shape, at)] == 0.)


def test_larger_input_is_cropped(backend, sample_plugins):
    """An input that extends beyond the frame (top right) is cropped to it."""
    big = np.random.default_rng(6).random((40, 40, 4), dtype=np.float32)
    at = (700, 560, 740, 600)
    r = _gpugain(sample_plugins, big, at)
    assert r.images[0].bounds == FRAME_PX
    out = _bottom_up(r.output)
    np.testing.assert_allclose(out[560:576, 700:720], _bottom_up(big)[:16, :20] * GAINS, atol=1e-6)
    assert np.all(out[~_in(out.shape, at)] == 0.)


def test_multires_plugin_gets_inputs_as_they_are(backend, sample_plugins):
    """Invert supports multiple resolutions, and so does the host: its RoD and output are those of its input."""
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0])
    assert r.report["renders"][0]["renderWindow"] == list(AT)
    assert r.images[0].bounds == AT
    np.testing.assert_allclose(r.output[..., :3], 1. - SRC[..., :3], atol=1e-6)


def _check_invert_padded(r):
    """Invert in a host that reports 0: the padding (transparent black) is inverted to 1 (outside of an input image,
    Invert would write 0)."""
    assert r.images[0].bounds == FRAME_PX
    out = _bottom_up(r.output)
    inside = _in(out.shape, AT)
    assert np.all(out[~inside] == 1.)
    np.testing.assert_allclose(out[inside].reshape(16, 24, 4)[..., :3], 1. - _bottom_up(SRC)[..., :3], atol=1e-6)


@pytest.mark.backends("subprocess", reason="hostDemo --ofx11: ofxh has no OFX 1.1 host profile")
def test_ofx11_host_pads_every_plugin(backend, sample_plugins):
    """hostDemo --ofx11 reports 0: Invert gets the frame; a tile in the padding matches the full render."""
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0],
                       extra_args=["--ofx11"])
    assert r.report["host"]["profile"] == "ofx11"
    _check_invert_padded(r)
    w = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0],
                       window=(0, 0, 50, 30), extra_args=["--ofx11"])
    assert w.images[0].bounds == (0, 0, 50, 30)
    np.testing.assert_array_equal(_bottom_up(w.output), _bottom_up(r.output)[0:30, 0:50])


@pytest.mark.backends("subprocess", reason="hostDemo --ofx11: ofxh has no OFX 1.1 host profile")
def test_ofx11_two_inputs_get_the_frame(backend, sample_plugins):
    """Basic (general context) in hostDemo --ofx11: Source and Mask, of different sizes and offsets, both get the
    frame as their RoD (their RoI, without tiles), and so does the output."""
    mask = np.random.default_rng(7).random((30, 10, 4), dtype=np.float32)
    r = ofxhost.render(sample_plugins, BASIC, "general", depth="float", frames=[0], extra_args=["--ofx11"],
                       inputs={"Source": ofxhost.Image(SRC, AT), "Mask": ofxhost.Image(mask, (300, 7, 310, 37))})
    x = r.report["renders"][0]
    assert x["regionOfDefinition"]["rod"] == list(FRAME)
    assert x["regionsOfInterest"]["clips"] == {"Source": list(FRAME), "Mask": list(FRAME)}
    assert r.images[0].bounds == FRAME_PX


# --- in-process: a Python host that reports 0, and the images as the plug-in fetches them (ctypes) -------------

_CLIP_HANDLE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p),
                                ctypes.c_void_p)
_GET_IMAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.c_void_p,
                              ctypes.POINTER(ctypes.c_void_p))
_RELEASE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
_ROD = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.POINTER(ctypes.c_double * 4))
_GET_POINTER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                ctypes.POINTER(ctypes.c_void_p))
_GET_INT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                            ctypes.POINTER(ctypes.c_int))
_GET_INT_N = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                              ctypes.POINTER(ctypes.c_int * 4))


def _fn(suite, index, proto):
    """A function of OfxImageEffectSuiteV1 or OfxPropertySuiteV1."""
    import ofxh
    s = ofxh.fetch_suite(suite, 1)
    return proto(ctypes.c_void_p.from_address(s + index * ctypes.sizeof(ctypes.c_void_p)).value)


def _fetch(instance_handle, name, time):
    """Fetch an input as a plug-in does: its clip RoD (canonical), and its image's bounds, RoD and RGBA float pixels
    (bottom-up)."""
    clip = ctypes.c_void_p()
    assert _fn("OfxImageEffectSuite", 3, _CLIP_HANDLE)(ctypes.c_void_p(instance_handle), name.encode(),
                                                       ctypes.byref(clip), None) == 0
    rod = (ctypes.c_double * 4)()
    assert _fn("OfxImageEffectSuite", 7, _ROD)(clip, time, ctypes.byref(rod)) == 0
    img = ctypes.c_void_p()
    assert _fn("OfxImageEffectSuite", 5, _GET_IMAGE)(clip, time, None, ctypes.byref(img)) == 0
    b, irod = (ctypes.c_int * 4)(), (ctypes.c_int * 4)()
    data, row_bytes = ctypes.c_void_p(), ctypes.c_int()
    _fn("OfxPropertySuite", 15, _GET_INT_N)(img, b"OfxImagePropBounds", 4, ctypes.byref(b))
    _fn("OfxPropertySuite", 15, _GET_INT_N)(img, b"OfxImagePropRegionOfDefinition", 4, ctypes.byref(irod))
    _fn("OfxPropertySuite", 8, _GET_POINTER)(img, b"OfxImagePropData", 0, ctypes.byref(data))
    _fn("OfxPropertySuite", 11, _GET_INT)(img, b"OfxImagePropRowBytes", 0, ctypes.byref(row_bytes))
    h, w = b[3] - b[1], b[2] - b[0]
    pixels = None
    if row_bytes.value == w * 16:  # float RGBA
        pixels = np.frombuffer(ctypes.string_at(data.value, row_bytes.value * h), np.float32).reshape(h, w, 4).copy()
    _fn("OfxImageEffectSuite", 6, _RELEASE)(img)
    return {"clip_rod": tuple(rod), "bounds": tuple(b), "rod": tuple(irod), "pixels": pixels}


def _render_seen(sample_plugins, monkeypatch, plugin, inputs, context="filter", **kw):
    """Render in-process; during the render (the first fetch of the output), fetch each input as the plug-in does."""
    from ofxhost import inprocess
    seen = {}
    original = inprocess._Clip.get_image

    def get_image(self, time, view, bounds, plane="Colour"):
        if self.is_output and not seen:
            for name in inputs:
                seen[name] = _fetch(self.instance.handle, name, time)
        return original(self, time, view, bounds, plane)

    monkeypatch.setattr(inprocess._Clip, "get_image", get_image)
    r = ofxhost.render(sample_plugins, plugin, context, inputs=inputs, depth="float", frames=[0], backend="inprocess",
                       **kw)
    assert r.report["ok"] and seen
    return r, seen


@pytest.fixture
def host_reports_0(monkeypatch):
    """The in-process backend's host reports kOfxImageEffectPropSupportsMultiResolution 0 (init_properties()).

    The host is made again for the test; tmp_cwd's teardown shuts it down (inprocess.shutdown(), ofxh.shutdown()),
    so that the property doesn't leak into the next tests.
    """
    import ofxh
    from ofxhost import inprocess
    inprocess.shutdown()
    ofxh.shutdown()
    original = inprocess._Host.init_properties

    def init_properties(self, props):
        original(self, props)
        props[k.kOfxImageEffectPropSupportsMultiResolution] = 0

    monkeypatch.setattr(inprocess._Host, "init_properties", init_properties)


@pytest.mark.backends("inprocess", reason=IN_PROCESS)
def test_host_property(backend, sample_plugins):
    import ofxh
    ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, frames=[0])
    assert ofxh.host().props[k.kOfxImageEffectPropSupportsMultiResolution] == (1,)


@pytest.mark.backends("inprocess", reason=IN_PROCESS)
def test_cropped_and_padded_image(backend, sample_plugins, monkeypatch):
    """GPUGain's Source image: the frame, padded with zeros outside the input; its clip RoD is the frame."""
    _, seen = _render_seen(sample_plugins, monkeypatch, GPUGAIN, {"Source": ofxhost.Image(SRC, AT)},
                           params={"scale": 0.5})
    s = seen["Source"]
    assert s["bounds"] == FRAME_PX and s["rod"] == FRAME_PX and s["clip_rod"] == FRAME
    inside = _in(s["pixels"].shape, AT)
    np.testing.assert_array_equal(s["pixels"][inside].reshape(16, 24, 4), _bottom_up(SRC))
    assert np.all(s["pixels"][~inside] == 0.)


@pytest.mark.backends("inprocess", reason=IN_PROCESS)
def test_image_as_it_is(backend, sample_plugins, monkeypatch):
    """Invert's Source image: the input as it is."""
    _, seen = _render_seen(sample_plugins, monkeypatch, INVERT, {"Source": ofxhost.Image(SRC, AT)})
    s = seen["Source"]
    assert s["bounds"] == AT and s["rod"] == AT
    np.testing.assert_array_equal(s["pixels"], _bottom_up(SRC))


@pytest.mark.backends("inprocess", reason=IN_PROCESS)
def test_python_host_reporting_0_pads_every_plugin(backend, sample_plugins, host_reports_0):
    """A Python host that reports 0: Invert gets the frame; a tile in the padding matches the full render."""
    import ofxh
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0])
    assert ofxh.host().props[k.kOfxImageEffectPropSupportsMultiResolution] == (0,)
    _check_invert_padded(r)
    w = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0],
                       window=(0, 0, 50, 30))
    assert w.images[0].bounds == (0, 0, 50, 30)
    np.testing.assert_array_equal(_bottom_up(w.output), _bottom_up(r.output)[0:30, 0:50])


@pytest.mark.backends("inprocess", reason=IN_PROCESS)
def test_python_host_reporting_0_two_inputs(backend, sample_plugins, host_reports_0, monkeypatch):
    """Basic (general context) with Source and Mask of different sizes and offsets, in a Python host that reports 0:
    both images and clip RoDs are the frame (no fetch bounds: the whole frame), and so is the output's RoD."""
    mask = np.random.default_rng(7).random((30, 10, 4), dtype=np.float32)
    r, seen = _render_seen(sample_plugins, monkeypatch, BASIC, {"Source": ofxhost.Image(SRC, AT),
                                                                "Mask": ofxhost.Image(mask, (300, 7, 310, 37))},
                           context="general")
    for name in ("Source", "Mask"):
        s = seen[name]
        assert s["bounds"] == FRAME_PX and s["rod"] == FRAME_PX and s["clip_rod"] == FRAME, name
    assert r.report["renders"][0]["regionOfDefinition"]["rod"] == list(FRAME)


@pytest.mark.backends("inprocess", reason=IN_PROCESS)
def test_python_host_reporting_0_tile_gets_the_input_pixels(backend, sample_plugins, host_reports_0, monkeypatch):
    """A tile render (Invert supports tiles) in a Python host that reports 0: the Source image that Invert fetches
    with no region is the whole frame, with the input's pixels where the input is (outside the tile too), and
    zeros elsewhere."""
    _, seen = _render_seen(sample_plugins, monkeypatch, INVERT, {"Source": ofxhost.Image(SRC, AT)},
                           window=(0, 0, 50, 30))
    s = seen["Source"]
    assert s["bounds"] == FRAME_PX and s["rod"] == FRAME_PX
    inside = _in(s["pixels"].shape, AT)
    np.testing.assert_array_equal(s["pixels"][inside].reshape(16, 24, 4), _bottom_up(SRC))
    assert np.all(s["pixels"][~inside] == 0.)
