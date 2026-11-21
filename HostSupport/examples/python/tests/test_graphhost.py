"""Tests of the sample host (ofxhost.graphhost): a graph over a frame sequence, in-process on ofxh,
with in-memory images between the nodes; the project load, time domains, image sequences and file
name patterns, push buttons, identities, and the JSON report."""

import json

import numpy as np
import pytest

import ofxhost
from ofxhost import sequences
from ofxhost.graph import Files, Graph, Keys, Node, Project, execute, load
from ofxhost.testing import assert_close, backend_available

why = backend_available("inprocess")
if why is not None:
    pytestmark = pytest.mark.skip(reason=f"ofxh: {why}")  # (OFXH_REQUIRE=1 fails in the fixture below)
else:
    import ofxh
    from ofxhost import graphhost

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
RETIMER = "net.sf.openfx.retimer"
NOISE = "net.sf.openfx.noisePlugin"


@pytest.fixture(autouse=True)
def _cwd(tmp_path, monkeypatch):
    """In a temporary directory (the plug-in scan makes PropTester write ofxTestLog.txt there), and
    without an ofxh host."""
    import os
    if os.environ.get("OFXH_REQUIRE") == "1" and why is not None:
        pytest.fail(f"ofxh: {why} (OFXH_REQUIRE=1)")
    monkeypatch.chdir(tmp_path)
    yield
    if why is None:
        ofxhost.inprocess.shutdown()
        ofxh.shutdown()


def rgba(h=24, w=32):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25), 1 - x / w]).astype(np.float32)


def sequence(tmp_path, frames, h=6, w=8, name="in.####.pfm"):
    """An RGB sequence: frame f is the constant f / 10."""
    for f in frames:
        ofxhost.write_pfm(tmp_path / sequences.expand(name, f), np.full((h, w, 3), f / 10, np.float32))
    return str(tmp_path / name)


def run(g, plugins, **kw):
    r = graphhost.run(g, plugins, **kw)
    assert r.ok, r.report["error"]
    json.dumps(r.report, allow_nan=False)  # (JSON)
    return r


@pytest.mark.parametrize("scale", [1.0, 0.5])
def test_same_images_as_execute(sample_plugins, scale):
    """A DAG (one node feeding two inputs), with test patterns and an array: the same images and
    bounds as ofxhost.graph.execute() (hostDemo's project, the default one)."""
    inv = Node(INVERT, inputs={"Source": "ramp"})
    out = Node(BASIC, "general", {"scale": 2.}, inputs={"Source": inv, "Mask": inv})
    arr = Node(INVERT, inputs={"Source": Node(INVERT, inputs={"Source": rgba()})})
    for g in (Graph.of(out, frames=[0, 1], scale=scale), Graph.of(arr, frames=[0], window=(4, 2, 20, 16))):
        a = execute(g, sample_plugins, backend="inprocess")
        r = run(g, sample_plugins)
        b = r.images[next(iter(g.outputs))]
        assert sorted(a) == sorted(b)
        for t in a:
            assert a[t].bounds == b[t].bounds and a[t].premult == b[t].premult, t
            assert np.array_equal(a[t].pixels, b[t].pixels), t
    assert_close(b[0].pixels, rgba()[24 - 16:24 - 2, 4:20], atol=1e-6)


def test_report(sample_plugins, tmp_path):
    g = Graph.of(Node(BASIC, "general", {"scale": 0.5}, inputs={"Source": "ramp", "Mask": "none"}, name="b"),
                 frames=[0, 1])
    r = run(g, sample_plugins, report_path=tmp_path / "r.json")
    j = json.loads((tmp_path / "r.json").read_text())
    assert j == json.loads(json.dumps(r.report))
    assert j["schema"] == "ofxhost.graphReport" and j["schemaVersion"] == 1 and j["ok"] and j["error"] is None
    assert j["frames"] == [0., 1.] and j["project"]["size"] == [768., 576.] and j["host"]["name"] == "net.sf.openfx.ofxh"
    n = j["nodes"]["b"]
    assert n["plugin"]["id"] == BASIC and n["context"] == "general" and n["paramChangedCount"] == 0
    assert [a["action"] for a in n["actions"]] == ["createInstance", "getClipPreferences", "beginSequenceRender",
                                                   "endSequenceRender"]
    assert n["timeDomain"] == {"action": "kOfxStatReplyDefault", "range": [0., 24.]}  # (the Source's: the project's)
    assert [x["time"] for x in n["renders"]] == [0., 1.] and all(x["ok"] for x in n["renders"])
    assert n["renders"][0]["renderWindow"] == [0, 0, 720, 576]
    assert n["params"]["scale"] == 0.5 and n["clipPreferences"]["clips"]["Mask"]["connected"] is False
    out = j["outputs"]["b"]
    assert [o["time"] for o in out] == [0., 1.] and out[0]["files"] == []
    assert out[0]["output"]["w"] == 720 and out[0]["output"]["comps"] == "RGBA" and len(out[0]["output"]["crc32"]) == 8
    # the report's params are a graph's: they render the same
    g2 = Graph.of(Node(BASIC, "general", n["params"], inputs={"Source": "ramp", "Mask": "none"}, name="b"),
                  frames=[0, 1])
    assert run(g2, sample_plugins).report["outputs"] == r.report["outputs"]


def test_project_load_and_push_buttons(sample_plugins, tmp_path):
    """The values and keys are set before createInstance, without instanceChanged; then the buttons
    are pressed (instanceChanged). Invert reads supportsTiles in its createInstance: loaded as
    false, it renders its whole RoD, whatever the window."""
    g = Graph.of(Node(INVERT, params={"supportsTiles": False}, inputs={"Source": rgba()}, name="inv"),
                 frames=[0], window=(2, 2, 6, 6))
    r = run(g, sample_plugins)
    assert r.report["nodes"]["inv"]["paramChangedCount"] == 0
    assert r.images["inv"][0].bounds == (0, 0, 32, 24)  # (the whole RoD)
    seq = sequence(tmp_path, range(1, 6))
    tester = Node(TESTER, params={"keysDouble": Keys({0: 1., 10: 3.}), "keysChoice": "c", "messageText": "hi",
                                  "analyseButton": None, "pbButton": None},
                  inputs={"Source": Files(seq)}, name="t")
    r = run(Graph.of(tester, frames=[2]), sample_plugins)
    n = r.report["nodes"]["t"]
    assert n["paramChangedCount"] == 0
    presses = [a for a in n["actions"] if a["action"] == "instanceChanged"]
    assert [(a["param"], a["time"], a["ok"]) for a in presses] == [("analyseButton", 0., True), ("pbButton", 0., True)]
    assert [m["text"] for m in n["messages"]] == ["Push Button Pressed - TestPassed!"]
    p = n["params"]
    assert p["keysDouble"] == {"keys": [[0., 1.], [10., 3.]], "interpolation": "linear"}
    assert p["keysChoice"] == 2 and p["messageText"] == "hi"
    # the analyse button fetched the Source at the frames of its range, the sequence's (1 to 5: the
    # Tester's loop stops before the last)
    assert p["analysisParam"] == {"keys": [[f, f] for f in (1., 2., 3., 4.)], "interpolation": "linear"}


def test_time_domains_and_sequences(sample_plugins, tmp_path):
    """The frames: those of the outputs' time domains. Retimer (general) gives its own (0 to the
    Source's last frame times Duration); Basic replies default: the union of its non-optional
    inputs' ranges, the sequence's; Noise has no non-optional input: the project's frame range.
    The Retimer blends the files of the frames it needs (rendered from the in-memory input node),
    the first one held before the sequence."""
    seq = sequence(tmp_path, range(1, 6))
    project = Project(size=(8., 6.), pixel_aspect_ratio=1., frame_range=(1., 5.))
    src = Node(INVERT, inputs={"Source": Files(seq)}, name="src")
    ret = Node(RETIMER, "general", {"Speed": 0.5, "Duration": 2.}, inputs={"Source": src}, name="ret")
    r = run(Graph(nodes={"src": src, "ret": ret}, outputs={"ret": None}, project=project), sample_plugins)
    assert r.report["nodes"]["ret"]["timeDomain"] == {"action": "kOfxStatOK", "range": [0., 10.]}
    assert r.report["nodes"]["src"]["timeDomain"]["range"] == [1., 5.]
    assert r.report["frames"] == [float(t) for t in range(11)]
    for t, v in ((0, 0.1), (2, 0.1), (3, 0.15), (6, 0.3), (9, 0.45)):  # source time t / 2, then inverted
        assert_close(r.images["ret"][float(t)].pixels[..., :3], 1 - v, atol=1e-6, msg=f"t={t}")
    # Retimer asked for its two frames: the input node rendered them (frames 1 and 2 for t=3)
    times = [x["time"] for x in r.report["nodes"]["src"]["renders"]]
    assert 1. in times and 2. in times
    basic = Node(BASIC, "general", {"scale": 2.}, inputs={"Source": Files(seq), "Mask": "none"}, name="b")
    r = run(Graph(nodes={"b": basic}, outputs={"b": None}, project=project), sample_plugins)
    assert r.report["nodes"]["b"]["timeDomain"] == {"action": "kOfxStatReplyDefault", "range": [1., 5.]}
    assert r.report["frames"] == [1., 2., 3., 4., 5.]
    assert_close(r.images["b"][4.].pixels[..., :3], 0.8, atol=1e-6)
    noise = Node(NOISE, "general", name="n", inputs={"Source": "none"})
    r = run(Graph(nodes={"n": noise}, outputs={"n": None}, project=project), sample_plugins)
    assert r.report["nodes"]["n"]["timeDomain"]["range"] is None and r.report["frames"] == [1., 2., 3., 4., 5.]


def test_output_files(sample_plugins, tmp_path):
    """The outputs are written to their patterns, over the project, relative to the graph file; a
    still image input is the same at all frames."""
    still = tmp_path / "in.npy"
    np.save(still, rgba())
    (tmp_path / "sub").mkdir()
    g = Graph.of(Node(INVERT, inputs={"Source": Files("../in.npy")}, name="inv"), frames=[3, 4],
                 project=Project(size=(40., 30.), pixel_aspect_ratio=1.))
    g.outputs["inv"] = "out/inv.%03d.npy"
    g.save(tmp_path / "sub" / "g.json")
    r = run(load(tmp_path / "sub" / "g.json"), sample_plugins)
    assert r.files == [str(tmp_path / "sub" / "out" / f"inv.{f:03d}.npy") for f in (3, 4)]
    a = np.load(r.files[1])
    assert a.shape == (30, 40, 4)
    assert_close(a[30 - 24:, :32], 1 - rgba(), atol=1e-6)  # (the image at the bottom left of the project)
    assert np.all(a[:30 - 24] == 0) and np.all(a[:, 32:] == 0)
    # PFM: an RGBA image is written as RGB and an alpha file, as hostDemo
    g.outputs["inv"] = "o.####.pfm"
    r = run(load_and_save(g, tmp_path), sample_plugins)
    assert [f.rsplit("/", 1)[1] for f in r.files] == ["o.0003.pfm", "o.0003.alpha.pfm", "o.0004.pfm", "o.0004.alpha.pfm"]
    assert_close(ofxhost.imageio.read_image(r.files[2]), a, atol=1e-6)


def load_and_save(g, tmp_path):
    g.save(tmp_path / "sub" / "g.json")
    return load(tmp_path / "sub" / "g.json")


def test_identity(sample_plugins):
    """Basic with all its scales at 1 is an identity: not rendered, its output is its Source's image."""
    src = Node(INVERT, inputs={"Source": rgba()}, name="src")
    b = Node(BASIC, "general", {"scale": 1.}, inputs={"Source": src, "Mask": "none"}, name="b")
    r = run(Graph.of(b, frames=[0]), sample_plugins)
    rec = r.report["nodes"]["b"]["renders"]
    assert rec == [{"time": 0., "renderWindow": [0, 0, 32, 24], "identity": {"clip": "Source", "time": 0.}}]
    assert_close(r.images["b"][0].pixels, 1 - rgba(), atol=1e-6)


def test_file_params(sample_plugins, monkeypatch):
    """A file parameter holds the pattern; the plug-in's paramGetValue gets the file name of the
    current frame, paramGetValueAtTime that of its time (the Tester's messageText, made a file
    parameter here: it sends its value as a message in instanceChanged)."""
    class Instance(ofxh.ImageEffectInstance):
        def __init__(self):
            super().__init__()
            self.messages = []

        def new_param(self, desc):
            return graphhost.FileParam() if desc.name == "messageText" else None

        def message(self, type, id, text):
            self.messages.append(text)

    with graphhost.host(ofxhost._plugin_dirs(sample_plugins)):
        with ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", Instance()) as inst:
            assert inst.set_param("messageText", "shot.####.%V_%v.%03d.100%%.pfm") == ofxh.Stat.OK
            assert inst.create_instance_action() == ofxh.Stat.OK
            assert inst.get_param("messageText", 6.6) == "shot.0007.main_M.007.100%.pfm"
            inst.param_changed_action("messageText", "user", 12.)
            assert inst.messages[0] == "shot.0012.main_M.012.100%.pfm"
            assert inst.param("messageText").pattern == "shot.####.%V_%v.%03d.100%%.pfm"
    # which parameters: output file paths, and a reader's or writer's filename and proxy

    class Desc:
        def __init__(self, name, exists, mode="OfxParamStringIsFilePath", type="string"):
            self.name, self.type = name, type
            self.props = {"OfxParamPropStringMode": (mode,), "OfxParamPropStringFilePathExists": (exists,)}

    assert graphhost.is_file_param(Desc("out", 0), ["general"])
    assert not graphhost.is_file_param(Desc("lut", 1), ["general", "reader"])
    assert graphhost.is_file_param(Desc("filename", 1), ["general", "reader"])
    assert graphhost.is_file_param(Desc("proxy", 1), ["writer"])
    assert not graphhost.is_file_param(Desc("filename", 1), ["general"])
    assert not graphhost.is_file_param(Desc("filename", 0, "OfxParamStringIsSingleLine"), ["reader"])
    # in a run, the report keeps the pattern
    monkeypatch.setattr(graphhost, "is_file_param", lambda desc, contexts: desc.name == "messageText")
    r = run(Graph.of(Node(TESTER, params={"messageText": "a.####.pfm"}, inputs={"Source": rgba()}, name="t"), frames=[0]),
            sample_plugins)
    assert r.report["nodes"]["t"]["params"]["messageText"] == "a.####.pfm"


def test_sequences(tmp_path):
    assert sequences.expand("a.####.b", 7) == "a.0007.b" and sequences.expand("a.##.b", 123) == "a.123.b"
    assert sequences.expand("a.%04d.%d", -7) == "a.-007.-7" and sequences.expand("%V-%v", 0, "left") == "left-L"
    assert sequences.expand("100%%%d %x", 5) == "100%5 %x"
    assert sequences.has_pattern("a.%d") and not sequences.has_pattern("a.%%d.pfm") and not sequences.has_pattern("a")
    assert sequences.frame_of(2.5) == 3 and sequences.frame_of(2.49) == 2 and sequences.frame_of(-0.5) == 0
    for n in ("in.0001.pfm", "in.0010.pfm", "in.12345.pfm", "in.001.pfm", "in.0002.alpha.pfm", "in.x.pfm"):
        (tmp_path / n).write_bytes(b"")
    assert sequences.frames(str(tmp_path / "in.####.pfm")) == [1, 10, 12345]  # (not 001: not ####)
    assert sequences.frames(str(tmp_path / "in.%d.pfm")) == [12345]  # (the others have leading zeros)
    assert sequences.frames(str(tmp_path / "in.0001.pfm")) == [0] and sequences.frames(str(tmp_path / "no.pfm")) == []
    with pytest.raises(ValueError, match="directory"):
        sequences.frames(str(tmp_path / "d.####" / "a.####.pfm"))


def test_errors(sample_plugins, tmp_path):
    """A failed run ends with ok False and the error in the report, and closes its instances."""
    cases = [(Node("no.such.plugin"), "not found"),
             (Node(INVERT, params={"nope": 1}), "no parameter 'nope'"),
             (Node(INVERT, inputs={"Nope": "ramp"}), "no input clip 'Nope'"),
             (Node(INVERT, inputs={"Source": "none"}), "not optional"),
             (Node(INVERT, inputs={"Source": Files("missing.####.pfm")}), "no files"),
             (Node(INVERT, params={"supportsTiles": None}), "push button"),
             (Node(INVERT, "general"), "no general context"),
             (Node(TESTER, params={"keysChoice": "z"}), 'no option "z"')]
    for node, message in cases:
        r = graphhost.run(Graph.of(node, frames=[0]), sample_plugins)
        assert not r.ok and message in r.report["error"], (message, r.report["error"])
    assert not ofxh.initialized()


def test_one_host(sample_plugins):
    """The sample host takes ofxh's host from the in-process backend, and refuses another one."""
    ofxhost.render(sample_plugins, INVERT, inputs={"Source": rgba()}, frames=[0], backend="inprocess")
    run(Graph.of(Node(INVERT, inputs={"Source": rgba()}), frames=[0]), sample_plugins)
    ofxh.init(ofxh.Host(), [], use_env_path=False)
    with pytest.raises(RuntimeError, match="one host per process"):
        graphhost.run(Graph.of(Node(INVERT)), sample_plugins)
