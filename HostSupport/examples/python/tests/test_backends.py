"""The two backends agree: the same renders with hostDemo (subprocess) and in-process (ofxhost.inprocess,
on the ofxh module) give the same images and the same report. This checks the HostSupport bindings
against hostDemo. Also: the backend selection, and the in-process backend's one host per process."""

import math
import os

import numpy as np
import pytest

import ofxhost
from ofxhost.testing import backend_available, check_backend

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
NOISE = "net.sf.openfx.noisePlugin"
RETIMER = "net.sf.openfx.retimer"
DOT = "net.sf.openfx.dotexample"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"


@pytest.fixture
def both(sample_plugins):
    """Both backends must be available (OFXH_REQUIRE=1: fail if one isn't)."""
    for b in ofxhost.BACKENDS:
        why = backend_available(b)
        if why is not None:
            if os.environ.get("OFXH_REQUIRE") == "1":
                pytest.fail(f"backend {b}: {why} (OFXH_REQUIRE=1)")
            pytest.skip(f"backend {b}: {why}")
    return sample_plugins


def rgba(h=24, w=32, seed=0):
    return np.random.default_rng(seed).random((h, w, 4), dtype=np.float32)


def _normalized(report):
    """What differs by design: the host name, the input specs (temporary PFM files or arrays), the
    output files (none in-process), the plug-in's file path (symbolic links); the means (summation
    order) are compared approximately."""
    r = dict(report)
    r["host"] = dict(r["host"], name=None)
    r["plugin"] = dict(r["plugin"])
    if r["plugin"].get("filePath"):
        r["plugin"]["filePath"] = os.path.realpath(r["plugin"]["filePath"])
    r["inputs"] = {c: dict(v, spec=None) for c, v in r["inputs"].items()}
    renders = []
    for x in r["renders"]:
        x = dict(x)
        if x.get("output"):
            x["output"] = dict(x["output"], files=None, mean=None)
        renders.append(x)
    r["renders"] = renders
    return r


def _means(report):
    return [x["output"]["mean"] for x in report["renders"] if x.get("output")]


def _same_numbers(a, b):
    """Equal JSON values (ints and floats compared as numbers, None for NaN)."""
    if isinstance(a, dict) and isinstance(b, dict):
        assert a.keys() == b.keys(), (list(a), list(b))
        for k in a:
            _same_numbers(a[k], b[k])
    elif isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        assert len(a) == len(b), (a, b)
        for x, y in zip(a, b):
            _same_numbers(x, y)
    elif isinstance(a, bool) or isinstance(b, bool):
        assert a is b, (a, b)
    elif isinstance(a, (int, float)) and isinstance(b, (int, float)):
        assert a == b, (a, b)
    else:
        assert a == b, (a, b)


def _compare(plugins, *args, pixels=True, **kw):
    """Render with both backends; check that the results agree. Returns them."""
    s = ofxhost.render(plugins, *args, backend="subprocess", **kw)
    i = ofxhost.render(plugins, *args, backend="inprocess", **kw)
    assert s.returncode == i.returncode
    _same_numbers(_normalized(s.report), _normalized(i.report))
    for ms, mi in zip(_means(s.report), _means(i.report)):
        assert [x is None for x in ms] == [x is None for x in mi]
        for x, y in zip(ms, mi):
            if x is not None:
                assert math.isclose(x, y, rel_tol=1e-12, abs_tol=1e-12), (ms, mi)
    assert list(s.outputs) == list(i.outputs)
    if pixels:
        for key in s.outputs:
            assert s.outputs[key].shape == i.outputs[key].shape, key
            assert np.array_equal(s.outputs[key], i.outputs[key], equal_nan=True), key
            assert s.images[key].bounds == i.images[key].bounds and s.images[key].premult == i.images[key].premult
        assert list(s.alpha) == list(i.alpha)
    return s, i


@pytest.mark.parametrize("depth", ["byte", "short", "float"])
def test_invert(both, depth):
    _compare(both, INVERT, inputs={"Source": rgba()}, depth=depth)


def test_default_pattern(both):
    """hostDemo's default input (a grey level, and the time and view drawn), at several times."""
    _compare(both, INVERT, depth="byte", frames=[0, 0.5, 1, 12.3])


@pytest.mark.parametrize("spec", ["ramp", "checker:8", "solid:0.25,0.5,1.5,-1", "rod:10,20,300,200"])
def test_patterns(both, spec):
    _compare(both, INVERT, inputs={"Source": spec}, depth="float", frames=[0])


def test_scale_and_window(both):
    _compare(both, INVERT, inputs={"Source": "default"}, frames=[1], scale=(0.5, 0.25), window=(10, 5, 200, 100))


def test_image_inputs(both, tmp_path):
    """Arrays at a position with a premultiplication, grey and RGB arrays, and PFM files (with an
    alpha file)."""
    _compare(both, INVERT, inputs={"Source": ofxhost.Image(rgba(10, 12), (100, 50, 112, 60), "OfxImageAlphaPremultiplied")},
             frames=[0], window=(90, 40, 120, 70))
    _compare(both, INVERT, inputs={"Source": rgba()[..., 0]}, frames=[0])
    _compare(both, INVERT, inputs={"Source": rgba()[..., :3]}, frames=[0], scale=0.5)
    a = rgba(8, 9, seed=3)
    ofxhost.write_pfm(tmp_path / "c.pfm", a[..., :3])
    ofxhost.write_pfm(tmp_path / "a.pfm", a[..., 3])
    _compare(both, INVERT, inputs={"Source": f"file:{tmp_path}/c.pfm+{tmp_path}/a.pfm@-3,4#unpremult"}, frames=[0])
    _compare(both, INVERT, inputs={"Source": tmp_path / "a.pfm"}, frames=[0], depth="short")


def test_params_mask_keys(both):
    y, x = np.mgrid[0:16, 0:24]
    mask = ((x // 4 + y // 4) % 2).astype(np.float32)
    _compare(both, BASIC, "general", inputs={"Source": rgba(16, 24), "Mask": mask},
             params={"scale": {0: 1., 10: 2.}, "scaleComponents": False, "scaleR": 3}, frames=[0, 5, 10])
    _compare(both, BASIC, "general", inputs={"Source": rgba(16, 24), "Mask": "none"}, params={"scale": 0.25})


def test_generator(both):
    """No input: the output gets the -d depth (default_output_depth); an infinite RoD."""
    _compare(both, NOISE, "general", inputs={"Source": "none"}, depth="byte", frames=[0, 1])
    _compare(both, NOISE, "general", inputs={"Source": "none"}, depth="float", frames=[0], query=True)


def test_temporal(both):
    _compare(both, RETIMER, inputs={"Source": "default"}, params={"Speed": 0.5}, frames=[0, 1, 3])
    _compare(both, RETIMER, "general", inputs={"Source": "default"}, params={"Duration": 2}, frames=[0])


def test_rod_and_choices(both):
    _compare(both, DOT, "general", inputs={"Source": np.zeros((48, 64, 3), np.float32)},
             params={"radius": 10.0, "position": (100.0, 50.0)}, frames=[0])
    _compare(both, CHOICES, inputs={"Source": rgba()}, params={"red_choice": "red: lots", "blue_choice": "blue_0.5"},
             frames=[0])


def test_ofx14(both):
    _compare(both, CHOICES, inputs={"Source": rgba()}, params={"red_choice": 2}, frames=[0], extra_args=["--ofx14"])
    for b in ofxhost.BACKENDS:
        params = ofxhost.describe(both, CHOICES, extra_args=["--ofx14"], backend=b)["contexts"][
            "OfxImageEffectContextFilter"]["params"]
        assert "blue_choice" not in [p["name"] for p in params], b


def test_buttons_and_messages(both):
    s, i = _compare(both, TESTER, params={"keysTest": None}, frames=[0], query=True)
    assert [m["text"] for m in i.report["messages"]] == ["Keys test passed"]


def test_progress(both):
    """The progress records (each field: the report comparison checks the keys too): Tester calls
    progressStart, progressUpdate and progressEnd in each render."""
    s, i = _compare(both, TESTER, inputs={"Source": rgba(8, 8)}, frames=[0, 1, 2])
    assert len(i.report["progress"]) == len(i.report["renders"]) >= 2
    assert i.report["progress"] == s.report["progress"]


def test_refused_images(both):
    """Each image too large for hostDemo (more than 2^28 pixels) is an image fetch error, with its
    message, as in hostDemo: the Tester's analyse button fetches its Source at each frame of its
    range (0 to 24) outside a render, where the whole RoD is fetched. (During a render, the samples
    fetch within the render window, with the default RoI; a tiled plug-in with a large RoI, or
    explicit fetch bounds, could make several refused fetches in one render.)"""
    s, i = _compare(both, TESTER, inputs={"Source": "rod:0,0,40000,40000"}, params={"analyseButton": None},
                    frames=[0], query=True)
    for r in (s, i):
        assert r.stderr.count("too large (more than 268435456 pixels)") == 25, r.stderr


def test_refused_images_are_all_counted(both):
    """The same refused fetch twice (the same bounds at the same time) is two errors: the analyse
    button pressed twice (-s in extra_args, then the params) makes 50, not 25 (the in-process backend
    de-duplicated them by (bounds, time) before #207)."""
    s, i = _compare(both, TESTER, inputs={"Source": "rod:0,0,40000,40000"}, params={"analyseButton": None},
                    extra_args=["-s", "analyseButton"], frames=[0], query=True)
    for r in (s, i):
        assert r.stderr.count("too large (more than 268435456 pixels)") == 50, r.stderr
        presses = [a for a in r.report["actions"] if a["action"] == "instanceChanged"]
        assert [a["param"] for a in presses] == ["analyseButton", "analyseButton"]


def test_describe(both):
    for plugin in (BASIC, TESTER, CHOICES):
        s = ofxhost.describe(both, plugin, backend="subprocess")
        i = ofxhost.describe(both, plugin, backend="inprocess")
        for d in (s, i):
            d["plugin"]["filePath"] = os.path.realpath(d["plugin"]["filePath"])
        _same_numbers(s, i)


@pytest.mark.parametrize("plugin, kw, message", [
    (BASIC, dict(inputs={"Source": "none"}), "not optional"),
    (BASIC, dict(params={"nope": 1}), 'no parameter named "nope"'),
    (BASIC, dict(params={"scale": "x"}), 'invalid value "x"'),
    (CHOICES, dict(params={"blue_choice": {0: "blue_0.5"}}), "doesn't animate"),
    (BASIC, dict(inputs={"Nope": "ramp"}), 'no input clip named "Nope"'),
])
def test_errors(both, plugin, kw, message):
    """The same failures: exit status 1, hostDemo's message, and the report so far."""
    s, i = _compare(both, plugin, check=False, **kw)
    assert s.returncode == 1 and message in s.stderr and message in i.stderr
    with pytest.raises(ofxhost.HostDemoError, match=message):
        ofxhost.render(both, plugin, backend="inprocess", **kw)


def test_strchoice_value_outside_the_enums(both):
    """A known difference: ofxh's string choice parameters refuse a value that is not one of their
    enums (kOfxStatErrValue), hostDemo's accept it."""
    kw = dict(inputs={"Source": rgba()}, params={"blue_choice": "blue_2"}, frames=[0])
    assert ofxhost.render(both, CHOICES, backend="subprocess", **kw).returncode == 0
    r = ofxhost.render(both, CHOICES, backend="inprocess", check=False, **kw)
    assert r.returncode == 1 and 'could not set parameter "blue_choice" (status 11)' in r.stderr


@pytest.mark.parametrize("kw", [dict(context="paint"), dict(inputs={"Source": "solid:1"}), dict(frames=[0, 0]),
                                dict(scale=2), dict(window=(5, 5, 5, 6)), dict(inputs={"Source": "file:/nonexistent.pfm"})])
def test_usage_errors(both, kw):
    """hostDemo exits before the run (no report): both raise HostDemoError, even with check=False."""
    for b in ofxhost.BACKENDS:
        with pytest.raises(ofxhost.HostDemoError):
            ofxhost.render(both, BASIC, backend=b, check=False, **kw)


def test_backend_selection(both, monkeypatch):
    monkeypatch.setenv("OFXHOST_BACKEND", "inprocess")
    assert ofxhost.default_backend() == "inprocess"
    assert ofxhost.render(both, INVERT, frames=[0], query=True).cmd[0] == "ofxhost.inprocess"
    assert ofxhost.default_backend("subprocess") == "subprocess"
    monkeypatch.setenv("OFXHOST_BACKEND", "nope")
    with pytest.raises(ValueError, match="OFXHOST_BACKEND"):
        ofxhost.render(both, INVERT)
    monkeypatch.delenv("OFXHOST_BACKEND")
    assert ofxhost.default_backend() == "subprocess"
    with pytest.raises(ValueError, match="not supported by the in-process backend"):
        ofxhost.render(both, INVERT, backend="inprocess", extra_args=["--describe", "d.json"])


def test_inprocess_one_host_per_process(both):
    """The in-process backend uses ofxh's one host: it refuses to replace another one."""
    import ofxh
    from ofxhost import inprocess
    inprocess.shutdown()
    ofxh.init(ofxh.Host(), [str(d) for d in both], use_env_path=False)
    try:
        with pytest.raises(RuntimeError, match="one host per process"):
            ofxhost.render(both, INVERT, frames=[0], backend="inprocess")
    finally:
        ofxh.shutdown()
    r = ofxhost.render(both, INVERT, frames=[0], query=True, backend="inprocess")
    host = ofxh.host()
    assert host is not None and r.report["host"]["profile"] == "default"
    # kept for the next render with the same plug-ins and profile, replaced for another profile
    ofxhost.render(both, INVERT, frames=[0], query=True, backend="inprocess")
    assert ofxh.host() is host
    assert ofxhost.render(both, INVERT, frames=[0], query=True, backend="inprocess",
                          extra_args=["--ofx14"]).report["host"]["profile"] == "ofx14"
    assert ofxh.host() is not host and ofxh.profile() == "ofx14"
    inprocess.shutdown()
    assert not ofxh.initialized()


def test_backends_marker(monkeypatch):
    """The backends marker skips the other backends with its reason; a marker without reason= fails
    (with OFXH_REQUIRE=1 too)."""
    m = pytest.mark.backends("subprocess", reason="why").mark
    with pytest.raises(pytest.skip.Exception, match="only on the subprocess backend: why"):
        check_backend("inprocess", m)
    monkeypatch.setenv("OFXH_REQUIRE", "1")
    with pytest.raises(pytest.fail.Exception, match="needs a reason"):
        check_backend("subprocess", pytest.mark.backends("subprocess").mark)


@pytest.mark.backends("subprocess", reason="a check of the marker itself")
def test_marked_subprocess_only(backend):
    assert backend == "subprocess"
