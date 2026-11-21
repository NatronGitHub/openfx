"""The in-process backend and the sample host beyond hostDemo (NatronGitHub/openfx#136, part 5): per-input
components, the output files and report of extra_args (--format, -o, --report), a plug-in's worker
thread that fetches the output image first, and the bounded planning of the sample host."""

import ctypes
import json
import os
import threading

import numpy as np
import pytest

import ofxhost
from ofxhost.graph import Files, Graph, Node, load
from ofxhost.testing import assert_close, backend_available

why = backend_available("inprocess")
if why is not None:
    if os.environ.get("OFXH_REQUIRE") == "1":
        pytest.fail(f"ofxh: {why} (OFXH_REQUIRE=1)", pytrace=False)
    pytestmark = pytest.mark.skip(reason=f"ofxh: {why}")
else:
    import ofxh
    from ofxhost import graphhost, inprocess

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
MULTIPLY = "net.sf.openfx.MultiplyGPU"  # its clips support RGBA, RGB and Alpha


@pytest.fixture(autouse=True)
def _cwd(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    yield
    if why is None:
        inprocess.shutdown()
        ofxh.shutdown()


def render(plugins, *args, **kw):
    return ofxhost.render(plugins, *args, backend="inprocess", **kw)


def ramp(h=6, w=8):
    """hostDemo's ramp: x / the RoD's width, at the pixel centres (720 pixels)."""
    return np.broadcast_to(((np.arange(w) + 0.5) / 720.)[None, :], (h, w))


# --- per-input components ---------------------------------------------------------------------

def test_rgb_input(sample_plugins):
    """An RGB input of a plug-in whose clips support RGB: the clip preferences keep RGB (HostSupport's
    default), the plug-in gets an RGB image, and renders an RGB output."""
    k = np.arange(6 * 8 * 3).reshape(6, 8, 3) * 11 % 256
    src = (k / 255).astype(np.float32)
    value = np.array([0.5, 2.0, 0.25, 0.5])
    r = render(sample_plugins, MULTIPLY, inputs={"Source": src}, params={"value": value}, components={"Source": "rgb"})
    assert r.report["inputs"]["Source"]["unmappedComponents"] == "OfxImageComponentRGB"
    assert r.report["inputs"]["Source"]["spec"] == "array:8x6x3#rgb"
    clips = r.report["clipPreferences"]["clips"]
    assert clips["Source"]["components"] == "OfxImageComponentRGB"
    assert clips["Output"]["components"] == "OfxImageComponentRGB"
    assert r.output.shape == (6, 8, 3)
    assert r.report["renders"][0]["output"]["comps"] == "RGB"
    assert_close(r.output, src * value[:3], atol=1e-6)
    # the default: RGBA, opaque (as hostDemo's PFM inputs)
    d = render(sample_plugins, MULTIPLY, inputs={"Source": src}, params={"value": value})
    assert d.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentRGBA"
    assert d.output.shape == (6, 8, 4)


@pytest.mark.parametrize("spec", ["ramp#rgb", "ramp"])
def test_rgb_input_to_an_rgba_clip(sample_plugins, spec):
    """Invert's clips support RGBA and Alpha: an RGB input gets RGBA (the closest supported
    components), opaque: alpha 1, which Invert inverts to 0. RGBA ramps have alpha = the ramp."""
    r = render(sample_plugins, INVERT, inputs={"Source": spec}, frames=[0], window=(0, 0, 8, 6))
    src = r.report["inputs"]["Source"]
    assert src["spec"] == spec
    assert src["unmappedComponents"] == ("OfxImageComponentRGB" if spec.endswith("#rgb") else "OfxImageComponentRGBA")
    assert r.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentRGBA"
    out = r.output
    assert_close(out[..., 0], 1 - ramp(), atol=1e-6)
    assert_close(out[..., 3], 0. if spec.endswith("#rgb") else 1 - ramp(), atol=1e-6)


def test_alpha_input(sample_plugins):
    """An Alpha input of MultiplyGPU (Alpha supported): the plug-in gets an Alpha image of the ramp's
    alpha (its output stays RGBA, the clip preferences' choice)."""
    r = render(sample_plugins, MULTIPLY, inputs={"Source": "ramp"}, components={"Source": "OfxImageComponentAlpha"},
               params={"value": [1, 1, 1, 0.5]}, frames=[0], window=(0, 0, 8, 6))
    assert r.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentAlpha"
    assert r.report["inputs"]["Source"]["spec"] == "ramp#alpha"
    seen = r.report["clipPreferences"]["clips"]["Output"]["components"]
    assert r.output.shape[2] == {"OfxImageComponentRGBA": 4, "OfxImageComponentAlpha": 1}[seen]


def test_components_errors_and_the_subprocess_backend(sample_plugins):
    with pytest.raises(ValueError, match="expected rgba, rgb or alpha"):
        render(sample_plugins, INVERT, components={"Source": "rg"})
    with pytest.raises(ValueError, match="in-process backend"):
        ofxhost.render(sample_plugins, INVERT, inputs={"Source": "ramp#rgb"}, backend="subprocess")
    with pytest.raises(ValueError, match="in-process backend"):
        ofxhost.render(sample_plugins, INVERT, components={"Source": "alpha"}, backend="subprocess")
    for a in ("--format", "-o", "--report"):
        with pytest.raises(ValueError, match="its own --format pfm"):
            ofxhost.render(sample_plugins, INVERT, extra_args=[a, "x"], backend="subprocess")


def test_split_components():
    assert ofxhost.split_components("ramp#rgb") == ("ramp", "rgb")
    assert ofxhost.split_components("file:x.pfm#premult#alpha") == ("file:x.pfm#premult", "alpha")
    assert ofxhost.split_components("file:x.pfm#premult") == ("file:x.pfm#premult", None)
    assert ofxhost.split_components("#rgb") == ("#rgb", None)


def test_graph_components(sample_plugins, tmp_path):
    """The sample host: an image file input with a "#rgb" suffix gives the clip RGB, and a test
    pattern with "#alpha" Alpha; a saved graph keeps the suffixes."""
    src = np.random.default_rng(1).random((6, 8, 3), dtype=np.float32)
    ofxhost.write_pfm(tmp_path / "in.pfm", src)
    value = [0.5, 2.0, 0.25, 0.5]
    m = Node(MULTIPLY, params={"value": value}, inputs={"Source": Files("in.pfm#rgb")}, name="m")
    a = Node(MULTIPLY, inputs={"Source": "ramp#alpha"}, name="a")
    g = Graph(nodes={"m": m, "a": a}, outputs={"m": None, "a": None}, frames=[0], window=(0, 0, 8, 6))
    g.save(tmp_path / "g.json")
    doc = json.loads((tmp_path / "g.json").read_text())
    assert doc["nodes"]["m"]["inputs"]["Source"] == {"file": "in.pfm#rgb"}
    assert doc["nodes"]["a"]["inputs"]["Source"] == "ramp#alpha"
    r = graphhost.run(load(tmp_path / "g.json"), sample_plugins, write=False)
    assert r.ok, r.report["error"]
    assert r.report["nodes"]["m"]["clipPreferences"]["clips"]["Source"]["components"] == "rgb"
    assert r.report["nodes"]["a"]["clipPreferences"]["clips"]["Source"]["components"] == "alpha"
    out = r.images["m"][0.].pixels
    assert out.shape == (6, 8, 3)
    assert_close(out, src * np.array(value[:3], np.float32), atol=1e-6)
    assert r.images["a"][0.].pixels.shape[:2] == (6, 8)


# --- the output files and the report of extra_args --------------------------------------------

@pytest.mark.parametrize("fmt", ["pfm", "ppm"])
def test_output_files_and_report(sample_plugins, tmp_path, fmt):
    """--format, -o and --report in extra_args: hostDemo's files, relative to the caller's current
    directory, listed in the report, which is also written."""
    (tmp_path / "out").mkdir()  # (as hostDemo, -o doesn't make directories)
    r = render(sample_plugins, INVERT, inputs={"Source": "ramp"}, depth="byte", frames=[0, 1], window=(0, 0, 8, 6),
               extra_args=["--format", fmt, "-o", "out/x", "--report", "r.json"])
    files = [x["output"]["files"] for x in r.report["renders"]]
    if fmt == "pfm":
        assert files == [["out/x.0.pfm", "out/x.0.alpha.pfm"], ["out/x.1.pfm", "out/x.1.alpha.pfm"]]
        rgb = ofxhost.read_pfm(tmp_path / "out/x.0.pfm")
        alpha = ofxhost.read_pfm(tmp_path / "out/x.0.alpha.pfm")
        assert np.array_equal(np.dstack([rgb, alpha]), r.outputs[0])
    else:
        assert files == [["out/x.0.ppm"], ["out/x.1.ppm"]]
        text = (tmp_path / "out/x.0.ppm").read_text().split("\n")
        assert text[:5] == ["P3", "# OfxBitDepthByte OfxImageComponentRGBA, converted to 8 bits", "# bounds 0 0 8 6",
                            "8 6", "255"]
        v = np.array([[int(c) for c in line.split()] for line in text[5:] if line]).reshape(6, 8, 3)
        assert np.array_equal(v, np.round(r.outputs[0][..., :3] * 255).astype(int))
    assert json.loads((tmp_path / "r.json").read_text()) == json.loads(json.dumps(r.report))


def test_output_files_failure(sample_plugins, tmp_path):
    """A failed run still writes the report (hostDemo's --report)."""
    r = render(sample_plugins, "no.such.plugin", extra_args=["--report", "r.json"], check=False)
    assert r.returncode == 1
    j = json.loads((tmp_path / "r.json").read_text())
    assert j["ok"] is False and j["exitStatus"] == 1


# --- a plug-in's worker thread that fetches the output image first ----------------------------

_CLIP_HANDLE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p),
                                ctypes.c_void_p)
_GET_IMAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.c_void_p,
                              ctypes.POINTER(ctypes.c_void_p))
_RELEASE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
_ROD = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.POINTER(ctypes.c_double * 4))


def _suite_function(index, proto):
    """A function of OfxImageEffectSuiteV1 (getPropertySet, getParamSet, clipDefine, clipGetHandle,
    clipGetPropertySet, clipGetImage, clipReleaseImage, clipGetRegionOfDefinition, ...)."""
    suite = ofxh.fetch_suite("OfxImageEffectSuite", 1)
    return proto(ctypes.c_void_p.from_address(suite + index * ctypes.sizeof(ctypes.c_void_p)).value)


@pytest.mark.parametrize("host", ["inprocess", "graphhost"])
def test_worker_thread_fetches_the_output_first(sample_plugins, monkeypatch, host):
    """A plug-in's worker thread (here a Python thread that calls the image effect suite through
    ctypes, started and joined in the first fetch of the output image, as a plug-in's OpenMP loop
    would) is the first to fetch the output image, and asks for the output's RoD: both work, without
    a RoD action on the worker (which ofxh refuses). The worker's image is then replaced by the main
    thread's (the test leaks it: one 8x6 image)."""
    seen = {}
    clip_class = inprocess._Clip if host == "inprocess" else graphhost.Clip
    original = clip_class.get_image

    def get_image(self, time, view, bounds, plane="Colour"):
        if self.is_output and "image" not in seen and threading.current_thread() is threading.main_thread():
            handle = ctypes.c_void_p()
            st = _suite_function(3, _CLIP_HANDLE)(ctypes.c_void_p(self.instance.handle), b"Output", ctypes.byref(handle),
                                                 None)
            assert st == 0

            def worker():
                img = ctypes.c_void_p()
                seen["image"] = _suite_function(5, _GET_IMAGE)(handle, time, None, ctypes.byref(img))
                if seen["image"] == 0:
                    _suite_function(6, _RELEASE)(img)
                rod = (ctypes.c_double * 4)()
                seen["rod"] = (_suite_function(7, _ROD)(handle, time, ctypes.byref(rod)), tuple(rod))

            t = threading.Thread(target=worker)
            t.start()
            t.join()
        return original(self, time, view, bounds, plane)

    monkeypatch.setattr(clip_class, "get_image", get_image)
    if host == "inprocess":
        r = render(sample_plugins, INVERT, inputs={"Source": "rod:0,0,768,576"}, frames=[0], window=(0, 0, 8, 6))
        assert r.report["ok"]
    else:
        r = graphhost.run(Graph.of(Node(INVERT, inputs={"Source": "ramp"}), frames=[0], window=(0, 0, 8, 6)),
                          sample_plugins, write=False)
        assert r.ok, r.report["error"]
    assert seen["image"] == 0  # kOfxStatOK
    assert seen["rod"] == (0, (0., 0., 768., 576.))


# --- the sample host's planning -----------------------------------------------------------------

def test_plan_bounded(sample_plugins, monkeypatch):
    """A diamond whose paths ask a node for different windows: x0 is reached through A (an Invert, the
    whole window) and through B (a Basic, whose RoD is its small Source, so its Mask region of
    interest is small). The second window is inside the first: x0 is planned once, not once per
    path (part 4 follow-up)."""
    planned = []
    original = graphhost._plan_window

    def spy(r, ns, t, window, seen):
        planned.append((ns.name, t, tuple(window)))
        return original(r, ns, t, window, seen)

    monkeypatch.setattr(graphhost, "_plan_window", spy)
    x0 = Node(INVERT, inputs={"Source": "ramp"}, name="x0")
    a = Node(INVERT, inputs={"Source": x0}, name="a")
    b = Node(BASIC, "general", inputs={"Source": "rod:0,0,100,100", "Mask": x0}, name="b")
    top = Node(BASIC, "general", inputs={"Source": a, "Mask": b}, name="top")
    r = graphhost.run(Graph(nodes={"x0": x0, "a": a, "b": b, "top": top}, outputs={"top": None}, frames=[0]),
                      sample_plugins, write=False)
    assert r.ok, r.report["error"]
    assert [p for p in planned if p[0] == "x0"] == [("x0", 0., (0, 0, 720, 576))]


def test_plan_union():
    """_plan() plans a window inside the union of those planned at a (node, time) once, and the union
    when a window grows it."""
    calls = []

    class Ns:
        name = "n"

    def spy(r, ns, t, window, seen):
        calls.append(window)

    old, graphhost._plan_window = graphhost._plan_window, spy
    try:
        seen = {}
        for w in [(0, 0, 10, 10), (2, 2, 8, 8), (5, 5, 20, 12), (0, 0, 20, 12)]:
            graphhost._plan(None, Ns(), 0., w, seen)
    finally:
        graphhost._plan_window = old
    assert calls == [(0, 0, 10, 10), (0, 0, 20, 12)]
    assert seen == {("n", 0.): (0, 0, 20, 12)}
