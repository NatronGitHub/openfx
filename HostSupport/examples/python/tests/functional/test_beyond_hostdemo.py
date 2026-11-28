"""The in-process backend and the sample host beyond hostDemo (#212): per-input components, the output files
and report of extra_args (--format, -o, --report), and the bounded planning of the sample host. (The worker
threads that fetch the output image first, the renders from several threads, an Alpha input of Invert and the
non-finite statistics are in regression/test_inprocess_regressions.py.)"""

import json

import numpy as np
import pytest

import ofxhost
from ofxhost.graph import Files, Graph, Node, load
from ofxhost.testing import assert_close, check_backend, views
from ofxhost_helpers import hostdemo_ramp

check_backend("inprocess")  # (the whole module: skipped, or failed with OFXH_REQUIRE=1)
from ofxhost import graphhost  # noqa: E402

pytestmark = pytest.mark.usefixtures("tmp_cwd")

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
GPUGAIN = "com.OpenFXSample.GPUGain"  # its clips support RGBA, RGB and Alpha


def _gains(value):
    """GPUGain's parameters for the gains (r, g, b, a)."""
    return dict(zip(("scaleR", "scaleG", "scaleB", "scaleA"), (float(v) for v in value)))


def _corner(out, h=6, w=8):
    """GPUGain doesn't support multiple resolutions: its output is the project frame, the input padded with
    zeros. The h x w corner at the origin, where the input is (the arrays are top row first), after checking
    that the padding is 0."""
    assert out.shape[:2] == (576, 720)
    padding = np.ones(out.shape[:2], bool)
    padding[-h:, :w] = False
    assert np.all(out[padding] == 0.)
    return out[-h:, :w]


# --- per-input components ---------------------------------------------------------------------

def test_rgb_input(sample_plugins):
    """An RGB input of a plug-in whose clips support RGB: the clip preferences keep RGB (HostSupport's
    default), the plug-in gets an RGB image, and renders an RGB output; the report says so (unmappedComponents,
    the spec with #rgb) (#212)."""
    k = np.arange(6 * 8 * 3).reshape(6, 8, 3) * 11 % 256
    src = (k / 255).astype(np.float32)
    value = np.array([0.5, 2.0, 0.25, 0.5])
    r = ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": src}, params=_gains(value),
                       components={"Source": "rgb"}, backend="inprocess")
    assert r.report["inputs"]["Source"]["unmappedComponents"] == "OfxImageComponentRGB"
    assert r.report["inputs"]["Source"]["spec"] == "array:8x6x3#rgb"
    clips = r.report["clipPreferences"]["clips"]
    assert clips["Source"]["components"] == "OfxImageComponentRGB"
    assert clips["Output"]["components"] == "OfxImageComponentRGB"
    assert r.output.shape == (576, 720, 3)
    assert r.report["renders"][0]["output"]["comps"] == "RGB"
    assert_close(_corner(r.output), src * value[:3], atol=1e-6)
    # the default: RGBA, opaque (as hostDemo's PFM inputs)
    d = ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": src}, params=_gains(value), backend="inprocess")
    assert d.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentRGBA"
    assert d.output.shape == (576, 720, 4)


@pytest.mark.parametrize("spec", ["ramp#rgb", "ramp"])
def test_rgb_input_to_an_rgba_clip(sample_plugins, spec):
    """Invert's clips support RGBA and Alpha: an RGB input gets RGBA (the closest supported
    components), opaque: alpha 1, which Invert inverts to 0. RGBA ramps have alpha = the ramp (#212)."""
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": spec}, frames=[0], window=(0, 0, 8, 6),
                       backend="inprocess")
    src = r.report["inputs"]["Source"]
    assert src["spec"] == spec
    assert src["unmappedComponents"] == ("OfxImageComponentRGB" if spec.endswith("#rgb") else "OfxImageComponentRGBA")
    assert r.report["clipPreferences"]["clips"]["Source"]["components"] == "OfxImageComponentRGBA"
    out = r.output
    assert_close(out[..., 0], 1 - hostdemo_ramp(), atol=1e-6)
    assert_close(out[..., 3], 0. if spec.endswith("#rgb") else 1 - hostdemo_ramp(), atol=1e-6)


def test_alpha_input(sample_plugins):
    """An Alpha input of GPUGain (Alpha supported): the plug-in gets an Alpha image of the ramp's
    alpha, and the output is Alpha (HostSupport's default, as the spec says: no input is RGBA); components=
    overrides a spec's suffix (#212)."""
    r = ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": "ramp"},
                       components={"Source": "OfxImageComponentAlpha"},
                       params=_gains([1, 1, 1, 0.5]), frames=[0], window=(0, 0, 8, 6), backend="inprocess")
    clips = r.report["clipPreferences"]["clips"]
    assert clips["Source"]["components"] == "OfxImageComponentAlpha"
    assert clips["Output"]["components"] == "OfxImageComponentAlpha"
    assert r.report["inputs"]["Source"]["spec"] == "ramp#alpha"
    assert r.output.shape == (576, 720, 1)
    assert_close(r.output[..., 0], 0.5 * hostdemo_ramp(576, 720), atol=1e-6)  # (the ramp covers the frame)
    # components= overrides a spec's suffix, and the report says so
    r = ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": "ramp#rgb"}, components={"Source": "alpha"},
                       frames=[0], window=(0, 0, 8, 6), backend="inprocess")
    assert r.report["inputs"]["Source"]["spec"] == "ramp#alpha"
    r = ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": "ramp#rgb"}, components={"Source": "rgba"},
                       frames=[0], window=(0, 0, 8, 6), backend="inprocess")
    assert r.report["inputs"]["Source"]["spec"] == "ramp"
    assert r.report["inputs"]["Source"]["unmappedComponents"] == "OfxImageComponentRGBA"
    assert r.report["clipPreferences"]["clips"]["Output"]["components"] == "OfxImageComponentRGBA"
    assert r.output.shape == (576, 720, 4)


def test_components_errors_and_the_subprocess_backend(sample_plugins):
    """Bad components are refused on both backends; the subprocess backend refuses --format, -o and --report in
    extra_args, as it writes its own PFM files and report (#212)."""
    for b in ofxhost.BACKENDS:
        with pytest.raises(ValueError, match="expected rgba, rgb or alpha"):
            ofxhost.render(sample_plugins, INVERT, components={"Source": "rg"}, backend=b)
    for a in ("--format", "-o", "--report"):
        with pytest.raises(ValueError, match="its own --format pfm"):
            ofxhost.render(sample_plugins, INVERT, extra_args=[a, "x"], backend="subprocess")


def test_split_components():
    """ofxhost.split_components() splits a spec's #rgb/#alpha/#rgba suffix, and only that (#212)."""
    assert ofxhost.split_components("ramp#rgb") == ("ramp", "rgb")
    assert ofxhost.split_components("file:x.pfm#premult#alpha") == ("file:x.pfm#premult", "alpha")
    assert ofxhost.split_components("file:x.pfm#premult") == ("file:x.pfm#premult", None)
    assert ofxhost.split_components("#rgb") == ("#rgb", None)


def test_graph_components(sample_plugins, tmp_path):
    """The sample host: an image file input with a "#rgb" suffix gives the clip RGB, and a test
    pattern with "#alpha" Alpha; a saved graph keeps the suffixes (#212)."""
    src = np.random.default_rng(1).random((6, 8, 3), dtype=np.float32)
    ofxhost.write_pfm(tmp_path / "in.pfm", src)
    value = [0.5, 2.0, 0.25, 0.5]
    m = Node(GPUGAIN, params=_gains(value), inputs={"Source": Files("in.pfm#rgb")}, name="m")
    a = Node(GPUGAIN, inputs={"Source": "ramp#alpha"}, name="a")
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
    assert out.shape == (576, 720, 3)
    assert_close(_corner(out), src * np.array(value[:3], np.float32), atol=1e-6)
    assert r.report["nodes"]["a"]["clipPreferences"]["clips"]["Output"]["components"] == "alpha"
    assert r.images["a"][0.].pixels.shape == (576, 720, 1)


# --- the output files and the report of extra_args --------------------------------------------

@pytest.mark.parametrize("fmt", ["pfm", "ppm"])
def test_output_files_and_report(sample_plugins, tmp_path, fmt):
    """--format, -o and --report in extra_args: hostDemo's files, relative to the caller's current
    directory, listed in the report, which is also written (#212)."""
    (tmp_path / "out").mkdir()  # (as hostDemo, -o doesn't make directories)
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": "ramp"}, depth="byte", frames=[0, 1],
                       window=(0, 0, 8, 6), extra_args=["--format", fmt, "-o", "out/x", "--report", "r.json"],
                       backend="inprocess")
    files = [x["output"]["files"] for x in r.report["renders"]]
    v = views()
    stems = v.stems("out/x", 0) + v.stems("out/x", 1)
    if fmt == "pfm":
        assert files == [[f"{x}.pfm", f"{x}.alpha.pfm"] for x in stems]
        for x, key in zip(stems, v.keys(0)):
            rgb = ofxhost.read_pfm(tmp_path / f"{x}.pfm")
            alpha = ofxhost.read_pfm(tmp_path / f"{x}.alpha.pfm")
            assert np.array_equal(np.dstack([rgb, alpha]), r.outputs[key])
    else:
        assert files == [[f"{x}.ppm"] for x in stems]
        for x, key in zip(stems, v.keys(0)):
            text = (tmp_path / f"{x}.ppm").read_text().split("\n")
            assert text[:5] == ["P3", "# OfxBitDepthByte OfxImageComponentRGBA, converted to 8 bits",
                                "# bounds 0 0 8 6", "8 6", "255"]
            p = np.array([[int(c) for c in line.split()] for line in text[5:] if line]).reshape(6, 8, 3)
            assert np.array_equal(p, np.round(r.outputs[key][..., :3] * 255).astype(int))
    assert json.loads((tmp_path / "r.json").read_text()) == json.loads(json.dumps(r.report))


def test_output_files_failure(sample_plugins, tmp_path):
    """A failed run still writes the report (hostDemo's --report), on the in-process backend (#212)."""
    r = ofxhost.render(sample_plugins, "no.such.plugin", extra_args=["--report", "r.json"], check=False,
                       backend="inprocess")
    assert r.returncode == 1
    j = json.loads((tmp_path / "r.json").read_text())
    assert j["ok"] is False and j["exitStatus"] == 1


# --- the sample host's planning -----------------------------------------------------------------

def test_plan_bounded(sample_plugins, monkeypatch):
    """A diamond whose paths ask a node for different windows: x0 is reached through A (an Invert, the
    whole window) and through B (a Basic, whose RoD is its small Source, so its Mask region of
    interest is small). The second window is inside the first: x0 is planned once, not once per
    path (#212)."""
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


def test_plan_union(monkeypatch):
    """_plan() plans a window inside the union of those planned at a (node, time) once, and the union
    when a window grows it; an empty window plans nothing and doesn't grow the union (#212)."""
    calls = []

    class Ns:
        name = "n"

    def spy(r, ns, t, window, seen):
        calls.append(window)

    monkeypatch.setattr(graphhost, "_plan_window", spy)
    seen = {}
    for w in [(5, 5, 5, 9), (0, 0, 10, 10), (2, 2, 8, 8), (30, 30, 30, 30), (5, 5, 20, 12), (0, 0, 20, 12)]:
        graphhost._plan(None, Ns(), 0., w, seen)
    assert calls == [(0, 0, 10, 10), (0, 0, 20, 12)]
    assert seen == {("n", 0.): (0, 0, 20, 12)}
