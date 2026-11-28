"""kOfxImageEffectPropSupportsMultiResolution: hostDemo and ofxh report 1 (images of any size and offset). A plug-in
that declares 0 (GPUGain), or every plug-in in a host that reports 0 (hostDemo --ofx11, a Python host that sets 0),
gets every image in one frame, the project extent rooted at the origin: the output's RoD and every input's RoD and
image are that frame, each input cropped where it extends beyond it and padded with black and transparent pixels
where it falls short (ofxProcessingArch.rst: "pad images with an appropriate amount of black/transparent pixels to
enable them to work"). (#359)
"""

import ctypes

import numpy as np
import pytest

import ofxhost
from ofxhost import constants as k
from ofxhost.testing import selected_backends, suite_function, views
from ofxhost_helpers import (CLIP_GET_HANDLE, CLIP_GET_IMAGE, CLIP_GET_ROD, CLIP_HANDLE_FN, CLIP_RELEASE_IMAGE,
                             GET_IMAGE_FN, GET_INT_FN, GET_INT_N_FN, GET_POINTER_FN, IMAGE_EFFECT, PROP_GET_INT,
                             PROP_GET_INT_N, PROP_GET_POINTER, PROPERTY, RELEASE_FN, ROD_FN)

GPUGAIN = "com.OpenFXSample.GPUGain"  # kOfxImageEffectPropSupportsMultiResolution 0, no tiles
INVERT = "net.sf.openfx.invertPlugin"  # 1, tiles
BASIC = "net.sf.openfx.basicPlugin"  # 1, general context: Source and Mask
FRAME = (0., 0., 768., 576.)  # hostDemo's project extent (canonical)
FRAME_PX = (0, 0, 720, 576)  # in pixels (PAR 768/720)
SRC = np.random.default_rng(5).random((16, 24, 4), dtype=np.float32)  # top-down
AT = (100, 50, 124, 66)  # its pixel bounds: offset from the origin
GAINS = np.array([0.5, 0.5, 0.5, 1.], np.float32)  # GPUGain's scale 0.5 (scaleA stays 1)

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
    """GPUGain: its RoD (forced to the frame), render window and output are the frame; the input is padded, on
    both backends (#359)."""
    r = _gpugain(sample_plugins, SRC, AT)
    keys = views().keys(0)
    assert len(r.report["renders"]) == len(keys)
    for x, key in zip(r.report["renders"], keys):
        assert x["regionOfDefinition"]["rod"] == list(FRAME)
        assert x["regionsOfInterest"]["clips"]["Source"] == list(FRAME)
        assert x["renderWindow"] == list(FRAME_PX)
        assert r.images[key].bounds == FRAME_PX
        out = _bottom_up(r.outputs[key])
        inside = _in(out.shape, AT)
        np.testing.assert_allclose(out[inside].reshape(16, 24, 4), _bottom_up(SRC) * GAINS, atol=1e-6)
        assert np.all(out[~inside] == 0.)
        # the report's input RoD is the host's own, before the crop and pad
        assert x["inputs"]["Source"]["regionOfDefinition"][0] > 0


BIG = np.random.default_rng(6).random((40, 40, 4), dtype=np.float32)  # larger than what fits at its offset


# (the input, its pixel bounds, then the part of the output (bottom-up) that is the input's, and that input part)
@pytest.mark.parametrize("src, at, out_part, src_part", [
    (SRC, (-10, -5, 14, 11), np.s_[0:11, 0:14], np.s_[5:, 10:]),
    (BIG, (700, 560, 740, 600), np.s_[560:576, 700:720], np.s_[:16, :20]),
], ids=["negative-offset", "beyond-the-top-right"])
def test_input_beyond_the_frame_is_cropped(backend, sample_plugins, src, at, out_part, src_part):
    """An input at negative coordinates, or one that extends beyond the frame (top right): the part outside the
    frame is cropped, the rest of the frame is zeros, on both backends (#359)."""
    r = _gpugain(sample_plugins, src, at)
    for key in views().keys(0):
        assert r.images[key].bounds == FRAME_PX
        out = _bottom_up(r.outputs[key])
        np.testing.assert_allclose(out[out_part], _bottom_up(src)[src_part] * GAINS, atol=1e-6)
        assert np.all(out[~_in(out.shape, at)] == 0.)


def test_multires_plugin_gets_inputs_as_they_are(backend, sample_plugins):
    """Invert supports multiple resolutions, and so does the host: its RoD and output are those of its input, on
    both backends; the in-process host reports kOfxImageEffectPropSupportsMultiResolution 1 (#359)."""
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0])
    if backend == "inprocess":
        import ofxh
        assert ofxh.host().props[k.kOfxImageEffectPropSupportsMultiResolution] == (1,)
    keys = views().keys(0)
    assert [x["renderWindow"] for x in r.report["renders"]] == [list(AT)] * len(keys)
    for key in keys:
        assert r.images[key].bounds == AT
        np.testing.assert_allclose(r.outputs[key][..., :3], 1. - SRC[..., :3], atol=1e-6)


def _check_invert_padded(r, key):
    """Invert in a host that reports 0: the padding (transparent black) is inverted to 1 (outside of an input image,
    Invert would write 0). key: the output's (RenderResult.outputs)."""
    assert r.images[key].bounds == FRAME_PX
    out = _bottom_up(r.outputs[key])
    inside = _in(out.shape, AT)
    assert np.all(out[~inside] == 1.)
    np.testing.assert_allclose(out[inside].reshape(16, 24, 4)[..., :3], 1. - _bottom_up(SRC)[..., :3], atol=1e-6)


# a host that reports 0 on each backend: hostDemo --ofx11 (subprocess), a Python host (in-process)
HOSTS_REPORTING_0 = pytest.mark.parametrize("backend", selected_backends(), indirect=True,
                                            ids={"subprocess": "hostdemo-ofx11", "inprocess": "python-host"}.get)
# the in-process backend only (a Python host that reports 0, or the images as the plug-in fetches them, ctypes):
# the subprocess backend can never run these tests, so it isn't collected (as Support/Plugins/tests/test_ofx11.py)
IN_PROCESS_ONLY = pytest.mark.parametrize("backend", [b for b in selected_backends() if b == "inprocess"],
                                          indirect=True)
# hostDemo only (--ofx11: ofxh has no OFX 1.1 host profile), likewise
SUBPROCESS_ONLY = pytest.mark.parametrize("backend", [b for b in selected_backends() if b == "subprocess"],
                                          indirect=True)


# --- in-process: a Python host that reports 0, and the images as the plug-in fetches them (ctypes) -------------

def _fetch(instance_handle, name, time):
    """Fetch an input as a plug-in does: its clip RoD (canonical), and its image's bounds, RoD and RGBA float pixels
    (bottom-up)."""
    clip = ctypes.c_void_p()
    assert suite_function(IMAGE_EFFECT, 1, CLIP_GET_HANDLE, CLIP_HANDLE_FN)(
        ctypes.c_void_p(instance_handle), name.encode(), ctypes.byref(clip), None) == 0
    rod = (ctypes.c_double * 4)()
    assert suite_function(IMAGE_EFFECT, 1, CLIP_GET_ROD, ROD_FN)(clip, time, ctypes.byref(rod)) == 0
    img = ctypes.c_void_p()
    assert suite_function(IMAGE_EFFECT, 1, CLIP_GET_IMAGE, GET_IMAGE_FN)(clip, time, None, ctypes.byref(img)) == 0
    b, irod = (ctypes.c_int * 4)(), (ctypes.c_int * 4)()
    data, row_bytes = ctypes.c_void_p(), ctypes.c_int()
    get_int_n = suite_function(PROPERTY, 1, PROP_GET_INT_N, GET_INT_N_FN)
    get_int_n(img, b"OfxImagePropBounds", 4, ctypes.byref(b))
    get_int_n(img, b"OfxImagePropRegionOfDefinition", 4, ctypes.byref(irod))
    suite_function(PROPERTY, 1, PROP_GET_POINTER, GET_POINTER_FN)(img, b"OfxImagePropData", 0, ctypes.byref(data))
    suite_function(PROPERTY, 1, PROP_GET_INT, GET_INT_FN)(img, b"OfxImagePropRowBytes", 0, ctypes.byref(row_bytes))
    h, w = b[3] - b[1], b[2] - b[0]
    pixels = None
    if row_bytes.value == w * 16:  # float RGBA
        pixels = np.frombuffer(ctypes.string_at(data.value, row_bytes.value * h), np.float32).reshape(h, w, 4).copy()
    suite_function(IMAGE_EFFECT, 1, CLIP_RELEASE_IMAGE, RELEASE_FN)(img)
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


def _python_host_reports_0(monkeypatch):
    """Make the in-process backend's host report kOfxImageEffectPropSupportsMultiResolution 0 (init_properties()).

    The host is made again for the test; tmp_cwd's teardown (this module's pytestmark) shuts it down
    (inprocess.shutdown(), ofxh.shutdown()), so that the property doesn't leak into the next tests.
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


@IN_PROCESS_ONLY
def test_cropped_and_padded_image(backend, sample_plugins, monkeypatch):
    """GPUGain's Source image, as the plug-in fetches it (ctypes, in-process): the frame, padded with zeros outside
    the input; its clip RoD is the frame (#359)."""
    _, seen = _render_seen(sample_plugins, monkeypatch, GPUGAIN, {"Source": ofxhost.Image(SRC, AT)},
                           params={"scale": 0.5})
    s = seen["Source"]
    assert s["bounds"] == FRAME_PX and s["rod"] == FRAME_PX and s["clip_rod"] == FRAME
    inside = _in(s["pixels"].shape, AT)
    np.testing.assert_array_equal(s["pixels"][inside].reshape(16, 24, 4), _bottom_up(SRC))
    assert np.all(s["pixels"][~inside] == 0.)


@IN_PROCESS_ONLY
def test_image_as_it_is(backend, sample_plugins, monkeypatch):
    """Invert's Source image, as the plug-in fetches it (ctypes, in-process): the input as it is (#359)."""
    _, seen = _render_seen(sample_plugins, monkeypatch, INVERT, {"Source": ofxhost.Image(SRC, AT)})
    s = seen["Source"]
    assert s["bounds"] == AT and s["rod"] == AT
    np.testing.assert_array_equal(s["pixels"], _bottom_up(SRC))


@HOSTS_REPORTING_0
def test_host_reporting_0_pads_every_plugin(backend, sample_plugins, monkeypatch):
    """A host that reports 0, hostDemo --ofx11 or a Python host (the in-process backend's, made to report 0):
    Invert gets the frame, padded; a tile in the padding matches the full render (#359)."""
    if backend == "inprocess":
        _python_host_reports_0(monkeypatch)
        kw = {}
    else:
        kw = dict(extra_args=["--ofx11"])
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0],
                       **kw)
    if backend == "inprocess":
        import ofxh
        assert ofxh.host().props[k.kOfxImageEffectPropSupportsMultiResolution] == (0,)
        keys = views().keys(0)
    else:
        assert r.report["host"]["profile"] == "ofx11"
        keys = [0]  # (one view: the views are an extension)
    w = ofxhost.render(sample_plugins, INVERT, inputs={"Source": ofxhost.Image(SRC, AT)}, depth="float", frames=[0],
                       window=(0, 0, 50, 30), **kw)
    for key in keys:
        _check_invert_padded(r, key)
        assert w.images[key].bounds == (0, 0, 50, 30)
        np.testing.assert_array_equal(_bottom_up(w.outputs[key]), _bottom_up(r.outputs[key])[0:30, 0:50])


MASK = np.random.default_rng(7).random((30, 10, 4), dtype=np.float32)
TWO_INPUTS = {"Source": ofxhost.Image(SRC, AT), "Mask": ofxhost.Image(MASK, (300, 7, 310, 37))}  # sizes, offsets differ


@SUBPROCESS_ONLY
def test_ofx11_two_inputs_get_the_frame(backend, sample_plugins):
    """Basic (general context) in hostDemo --ofx11 (reports 0): Source and Mask, of different sizes and offsets, both
    get the frame as their RoI (no tiles), and so do the output's RoD and image (#359)."""
    r = ofxhost.render(sample_plugins, BASIC, "general", depth="float", frames=[0], extra_args=["--ofx11"],
                       inputs=TWO_INPUTS)
    x = r.report["renders"][0]
    assert x["regionOfDefinition"]["rod"] == list(FRAME)
    assert x["regionsOfInterest"]["clips"] == {"Source": list(FRAME), "Mask": list(FRAME)}
    assert r.images[0].bounds == FRAME_PX


@IN_PROCESS_ONLY
def test_python_host_reporting_0_two_inputs(backend, sample_plugins, monkeypatch):
    """Basic (general context) with Source and Mask of different sizes and offsets, in a Python host that reports 0:
    both images and clip RoDs, as the plug-in fetches them (ctypes), are the frame (no fetch bounds: the whole frame),
    and so is the output's RoD (#359)."""
    _python_host_reports_0(monkeypatch)
    r, seen = _render_seen(sample_plugins, monkeypatch, BASIC, TWO_INPUTS, context="general")
    for name in ("Source", "Mask"):
        s = seen[name]
        assert s["bounds"] == FRAME_PX and s["rod"] == FRAME_PX and s["clip_rod"] == FRAME, name
    assert r.report["renders"][0]["regionOfDefinition"]["rod"] == list(FRAME)


@IN_PROCESS_ONLY
def test_python_host_reporting_0_tile_gets_the_input_pixels(backend, sample_plugins, monkeypatch):
    """A tile render (Invert supports tiles) in a Python host that reports 0: the Source image that Invert fetches
    with no region is the whole frame, with the input's pixels where the input is (outside the tile too), and
    zeros elsewhere (#359)."""
    _python_host_reports_0(monkeypatch)
    _, seen = _render_seen(sample_plugins, monkeypatch, INVERT, {"Source": ofxhost.Image(SRC, AT)},
                           window=(0, 0, 50, 30))
    s = seen["Source"]
    assert s["bounds"] == FRAME_PX and s["rod"] == FRAME_PX
    inside = _in(s["pixels"].shape, AT)
    np.testing.assert_array_equal(s["pixels"][inside].reshape(16, 24, 4), _bottom_up(SRC))
    assert np.all(s["pixels"][~inside] == 0.)
