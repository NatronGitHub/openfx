"""The two backends agree: the same renders with hostDemo (subprocess) and in-process (ofxhost.inprocess,
on the ofxh module) give the same images and the same report. This checks the HostSupport bindings
against hostDemo. Also: the backend selection, and the in-process backend's one host per process. (The
refused-image counts are in regression/test_inprocess_regressions.py.)"""

import os

import numpy as np
import pytest

import ofxhost
from ofxhost import constants as k
from ofxhost.testing import _hostdemo_host_strings, check_backend
from ofxhost_helpers import compare_backends, random_rgba, same_json

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
NOISE = "net.sf.openfx.noisePlugin"
RETIMER = "net.sf.openfx.retimer"
DOT = "net.sf.openfx.dotexample"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
GPUGAIN = "com.OpenFXSample.GPUGain"  # its clips support RGBA, RGB and Alpha


@pytest.mark.parametrize("depth", ["byte", "short", "float"])
def test_invert(both, depth):
    """Invert on a random RGBA array at each depth: the same report, pixels and bounds on both backends, which
    checks the HostSupport bindings against hostDemo (#207)."""
    compare_backends(both, INVERT, inputs={"Source": random_rgba()}, depth=depth)


def test_default_pattern(both):
    """hostDemo's default input (a grey level, and the time and view drawn), at several times, on both backends
    (#207)."""
    compare_backends(both, INVERT, depth="byte", frames=[0, 0.5, 1, 12.3])


@pytest.mark.parametrize("spec", ["ramp", "checker:8", "solid:0.25,0.5,1.5,-1", "rod:10,20,300,200"])
def test_patterns(both, spec):
    """hostDemo's input patterns (ramp, checker, solid, rod:) are the same on both backends (#207)."""
    compare_backends(both, INVERT, inputs={"Source": spec}, depth="float", frames=[0])


def test_scale_and_window(both):
    """A render scale and a render window, the scale also as a NumPy array and a NumPy scalar (f653cace, #297),
    give the same results on both backends (#207)."""
    compare_backends(both, INVERT, inputs={"Source": "default"}, frames=[1], scale=(0.5, 0.25),
                     window=(10, 5, 200, 100))
    # a NumPy array, or scalar, as the scale
    compare_backends(both, INVERT, inputs={"Source": "ramp"}, frames=[0], scale=np.array([0.5, 0.25]),
                     window=(10, 5, 200, 100))
    compare_backends(both, INVERT, inputs={"Source": "ramp"}, frames=[0], scale=np.float32(0.5))


def test_image_inputs(both, tmp_path):
    """Arrays at a position with a premultiplication, grey and RGB arrays, and PFM files (with an
    alpha file, a position and #unpremult; a Path) are the same inputs on both backends (#207)."""
    image = ofxhost.Image(random_rgba(10, 12), (100, 50, 112, 60), k.kOfxImagePreMultiplied)
    compare_backends(both, INVERT, inputs={"Source": image}, frames=[0], window=(90, 40, 120, 70))
    compare_backends(both, INVERT, inputs={"Source": random_rgba()[..., 0]}, frames=[0])
    compare_backends(both, INVERT, inputs={"Source": random_rgba()[..., :3]}, frames=[0], scale=0.5)
    a = random_rgba(8, 9, seed=3)
    ofxhost.write_pfm(tmp_path / "c.pfm", a[..., :3])
    ofxhost.write_pfm(tmp_path / "a.pfm", a[..., 3])
    compare_backends(both, INVERT, inputs={"Source": f"file:{tmp_path}/c.pfm+{tmp_path}/a.pfm@-3,4#unpremult"},
                     frames=[0])
    compare_backends(both, INVERT, inputs={"Source": tmp_path / "a.pfm"}, frames=[0], depth="short")


def test_components(both, tmp_path):
    """Per-input components (hostDemo's -i Clip=spec#rgb...): a suffix, components=, which overrides it, and
    arrays. (Alpha inputs: Invert renders an Alpha output, HostSupport's default.) The API path of the
    components (#330); the command line's is test_hostdemo_cli.py::test_components_suffix."""
    ofxhost.write_pfm(tmp_path / "c.pfm", random_rgba(8, 9)[..., :3])
    for plugin, kw in ((INVERT, dict(inputs={"Source": "ramp#rgb"})), (GPUGAIN, dict(inputs={"Source": "ramp#rgb"})),
                       (INVERT, dict(inputs={"Source": f"file:{tmp_path}/c.pfm#premult#rgb"})),
                       (GPUGAIN, dict(inputs={"Source": random_rgba()[..., :3]}, components={"Source": "rgb"})),
                       (INVERT, dict(inputs={"Source": "ramp#rgb"}, components={"Source": "alpha"})),
                       (INVERT, dict(components={"Source": k.kOfxImageComponentAlpha}))):
        compare_backends(both, plugin, frames=[0], window=(0, 0, 40, 30), **kw)


def test_no_normalised_defaults(both):
    """--no-normalised-defaults in extra_args gives the same report on both backends: the in-process backend
    parses extra_args itself (inprocess._extra()), not as hostDemo's command line (#330)."""
    compare_backends(both, TESTER, frames=[0], query=True, extra_args=["--no-normalised-defaults"])


def test_params_mask_keys(both):
    """Basic with keys, a checker mask and scaleComponents off, and with a disconnected Mask: the same
    parameter values per frame and pixels on both backends (#207)."""
    y, x = np.mgrid[0:16, 0:24]
    mask = ((x // 4 + y // 4) % 2).astype(np.float32)
    compare_backends(both, BASIC, "general", inputs={"Source": random_rgba(16, 24), "Mask": mask},
                     params={"scale": {0: 1., 10: 2.}, "scaleComponents": False, "scaleR": 3}, frames=[0, 5, 10])
    compare_backends(both, BASIC, "general", inputs={"Source": random_rgba(16, 24), "Mask": "none"},
                     params={"scale": 0.25})


def test_generator(both):
    """No input: the output gets the -d depth (default_output_depth); an infinite RoD; the same on both
    backends (#207)."""
    compare_backends(both, NOISE, "general", inputs={"Source": "none"}, depth="byte", frames=[0, 1])
    compare_backends(both, NOISE, "general", inputs={"Source": "none"}, depth="float", frames=[0], query=True)


def test_temporal(both):
    """Retimer's frames needed (filter context) and time domain (general context) are the same on both backends
    (#207)."""
    compare_backends(both, RETIMER, inputs={"Source": "default"}, params={"Speed": 0.5}, frames=[0, 1, 3])
    compare_backends(both, RETIMER, "general", inputs={"Source": "default"}, params={"Duration": 2}, frames=[0])


def test_rod_and_choices(both):
    """The dot example's RoD (not its input's), and Choice and StrChoice params set by label or enum, are the
    same on both backends (#207)."""
    compare_backends(both, DOT, "general", inputs={"Source": np.zeros((48, 64, 3), np.float32)},
                     params={"radius": 10.0, "position": (100.0, 50.0)}, frames=[0])
    compare_backends(both, CHOICES, inputs={"Source": random_rgba()},
                     params={"red_choice": "red: lots", "blue_choice": "blue_0.5"},
                     frames=[0])


def test_ofx14(both):
    """--ofx14 (the OFX 1.4 host profile) renders the same on both backends, and neither describes
    ChoiceParams' StrChoice param there (#207)."""
    compare_backends(both, CHOICES, inputs={"Source": random_rgba()}, params={"red_choice": 2}, frames=[0],
                     extra_args=["--ofx14"])
    for b in ofxhost.BACKENDS:
        params = ofxhost.describe(both, CHOICES, extra_args=["--ofx14"], backend=b)["contexts"][
            "OfxImageEffectContextFilter"]["params"]
        assert "blue_choice" not in [p["name"] for p in params], b


def test_buttons_and_messages(both):
    """A push button (params={name: None}) runs Tester's keysTest on both backends: the same messages, "Keys
    test passed" (#207)."""
    s, i = compare_backends(both, TESTER, params={"keysTest": None}, frames=[0], query=True)
    assert [m["text"] for m in i.report["messages"]] == ["Keys test passed"]


def test_progress(both):
    """The progress records (each field: the report comparison checks the keys too): Tester calls
    progressStart, progressUpdate and progressEnd in each render. The same records on both backends (#207,
    after #206)."""
    s, i = compare_backends(both, TESTER, inputs={"Source": random_rgba(8, 8)}, frames=[0, 1, 2])
    assert len(i.report["progress"]) == len(i.report["renders"]) >= 2
    assert i.report["progress"] == s.report["progress"]


def test_describe(both):
    """describe() gives the same JSON on both backends for Basic, Tester and ChoiceParams: the API path of the
    describe parity (#207); test_hostdemo_cli.py::test_describe is the command line's."""
    for plugin in (BASIC, TESTER, CHOICES):
        s = ofxhost.describe(both, plugin, backend="subprocess")
        i = ofxhost.describe(both, plugin, backend="inprocess")
        for d in (s, i):
            d["plugin"]["filePath"] = os.path.realpath(d["plugin"]["filePath"])
        same_json(s, i)


@pytest.mark.parametrize("plugin, kw, message", [
    (INVERT, dict(inputs={"Source": "none"}), "not optional"),
    (BASIC, dict(inputs={"Source": "none"}), "not optional"),
    (BASIC, dict(params={"nope": 1}), 'no parameter named "nope"'),
    (BASIC, dict(params={"scale": "x"}), 'invalid value "x"'),
    (CHOICES, dict(params={"blue_choice": {0: "blue_0.5"}}), "doesn't animate"),
    (BASIC, dict(inputs={"Nope": "ramp"}), 'no input clip named "Nope"'),
], ids=["invert-source-none", "basic-source-none", "unknown-param", "bad-value", "choice-keys", "unknown-clip"])
def test_errors(both, plugin, kw, message):
    """The same failures on both backends: with check=False, exit status 1, hostDemo's message and the report
    so far; with check=True (the default), HostDemoError with the message and a non-zero returncode, on each
    backend. (#118, #207)"""
    s, i = compare_backends(both, plugin, check=False, **kw)
    assert s.returncode == 1 and message in s.stderr and message in i.stderr
    for b in ofxhost.BACKENDS:
        with pytest.raises(ofxhost.HostDemoError, match=message) as e:
            ofxhost.render(both, plugin, backend=b, **kw)
        assert e.value.returncode != 0, b


def test_strchoice_value_outside_the_enums(both):
    """A known difference: ofxh's string choice parameters refuse a value that is not one of their
    enums (kOfxStatErrValue), hostDemo's accept it. Pinned on purpose: the specification doesn't say which (an open
    question for the maintainer), and this is where the decision will land. (#207)"""
    kw = dict(inputs={"Source": random_rgba()}, params={"blue_choice": "blue_2"}, frames=[0])
    assert ofxhost.render(both, CHOICES, backend="subprocess", **kw).returncode == 0
    r = ofxhost.render(both, CHOICES, backend="inprocess", check=False, **kw)
    assert r.returncode == 1 and 'could not set parameter "blue_choice" (status 11)' in r.stderr


@pytest.mark.parametrize("kw", [dict(context="paint"), dict(inputs={"Source": "solid:1"}), dict(frames=[0, 0]),
                                dict(scale=2), dict(window=(5, 5, 5, 6)),
                                dict(inputs={"Source": "file:/nonexistent.pfm"})],
                         ids=["paint-context", "bad-pattern", "frame-twice", "scale-above-1", "empty-window",
                              "missing-file"])
def test_usage_errors(both, kw):
    """hostDemo exits before the run (no report): both raise HostDemoError, even with check=False (#207)."""
    for b in ofxhost.BACKENDS:
        with pytest.raises(ofxhost.HostDemoError):
            ofxhost.render(both, BASIC, backend=b, check=False, **kw)


def test_backend_selection(both, monkeypatch):
    """The backend selection: $OFXHOST_BACKEND, default_backend()'s argument, subprocess by default, a bad
    value refused, and the extra_args that the in-process backend doesn't support refused (#207)."""
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
    """The in-process backend uses ofxh's one host: it refuses to replace another one; it keeps its own for the
    same plug-ins and profile, and replaces it for another profile (#207)."""
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


@pytest.mark.parametrize("ofx14", [False, True])
def test_inprocess_host_lists_hostdemos_depths(both, ofx14):
    """The in-process host lists hostDemo's supported depths, components and contexts, in both
    profiles: byte, short and float (its image code, as hostDemo's, makes no half; ofxh's own host
    lists half), so that a plug-in that builds its describe from the host's list sees the same host
    (openfx-misc#222, #213)."""
    import ofxh
    from ofxhost import inprocess
    r = ofxhost.render(both, INVERT, frames=[0], query=True, backend="inprocess",
                       extra_args=["--ofx14"] if ofx14 else [])
    assert r.report["host"]["profile"] == ("ofx14" if ofx14 else "default")
    props = ofxh.host().props
    try:
        depths = props[k.kOfxImageEffectPropSupportedPixelDepths]
        assert depths == (k.kOfxBitDepthByte, k.kOfxBitDepthShort, k.kOfxBitDepthFloat)
        assert depths == _hostdemo_host_strings("kOfxImageEffectPropSupportedPixelDepths")
        assert tuple(ofxh.short_name("depth", d) for d in depths) == ofxhost.DEPTHS
        for prop in ("kOfxImageEffectPropSupportedComponents", "kOfxImageEffectPropSupportedContexts"):
            assert props[getattr(k, prop)] == _hostdemo_host_strings(prop) != ()
    finally:
        inprocess.shutdown()


def test_backends_marker(monkeypatch):
    """The backends marker skips the other backends with its reason; a marker without reason= fails
    (with OFXH_REQUIRE=1 too). The consumers' tests use the marker (ofxhost.testing, #207)."""
    m = pytest.mark.backends("subprocess", reason="why").mark
    with pytest.raises(pytest.skip.Exception, match="only on the subprocess backend: why"):
        check_backend("inprocess", m)
    monkeypatch.setenv("OFXH_REQUIRE", "1")
    with pytest.raises(pytest.fail.Exception, match="needs a reason"):
        check_backend("subprocess", pytest.mark.backends("subprocess").mark)
