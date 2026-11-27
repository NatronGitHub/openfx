"""ofxhost.hostdemo, hostDemo's command line on the in-process backend, against the hostDemo binary: the
same command lines (those of the plug-in repositories' test scripts) give the same exit status, the
same output files (byte for byte), the same --report (but the host name) and --describe JSON, and
the same error messages. Each run is a process, as the repositories' scripts run hostDemo."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

import ofxhost
from ofxhost import constants as k
from ofxhost.testing import check_backend

PYDIR = Path(__file__).resolve().parents[1]  # HostSupport/examples/python

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
RETIMER = "net.sf.openfx.retimer"
PROPTESTER = "net.sf.openfx.propertyTester"
MULTIPLY = "net.sf.openfx.MultiplyGPU"
OPENGL = "com.genarts:OpenGLSamplePlugin"
NOISE = "net.sf.openfx.noisePlugin"
GPUGAIN = "com.OpenFXSample.GPUGain"


@pytest.fixture(scope="module")
def hosts(sample_plugins):
    """(hostDemo, the ofxh module's directory): both are needed (OFXH_REQUIRE=1: fail if not)."""
    for b in ofxhost.BACKENDS:
        check_backend(b)
    import ofxh
    return ofxhost.find_hostdemo(), Path(ofxh.__file__).resolve().parent


def _env(plugins, ofxh_dir, **more):
    env = dict(os.environ)
    env["OFX_PLUGIN_PATH"] = os.pathsep.join(str(p) for p in plugins)
    env["PYTHONPATH"] = os.pathsep.join([str(PYDIR), str(ofxh_dir)])
    env.pop("OFX_HOSTDEMO_KEEP_LOADED", None)
    env.update(more)
    return env


def _run(cmd, cwd, env):
    p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace", timeout=300,
                       stdin=subprocess.DEVNULL)
    assert p.returncode >= 0, f"{cmd}: killed by signal {-p.returncode}\n{p.stderr}"
    return p


def both(tmp_path, hosts, plugins, args, files=(), **env):
    """Run args with hostDemo and with python -m ofxhost.hostdemo, each in its own directory (with
    copies of files); returns the two (CompletedProcess, directory)."""
    hostdemo, ofxh_dir = hosts
    out = []
    for name, cmd in (("hostDemo", [str(hostdemo)]), ("inprocess", [sys.executable, "-m", "ofxhost.hostdemo"])):
        d = tmp_path / name
        d.mkdir(parents=True)
        for f, data in files:
            (d / f).write_bytes(data)
        out.append((_run(cmd + list(args), d, _env(plugins, ofxh_dir, **env)), d))
    return out


def _normalized(report):
    r = dict(report)
    r["host"] = dict(r["host"], name=None)
    if r.get("plugin", {}).get("filePath"):
        r["plugin"] = dict(r["plugin"], filePath=os.path.realpath(r["plugin"]["filePath"]))
    renders = []
    for x in r.get("renders") or []:
        x = dict(x)
        if x.get("output"):
            x["output"] = dict(x["output"], mean=[None if v is None else round(v, 9) for v in x["output"]["mean"]])
        renders.append(x)
    r["renders"] = renders
    return r


def _same(a, b):
    """Equal JSON values (ints and floats compared as numbers)."""
    if isinstance(a, dict) and isinstance(b, dict):
        assert a.keys() == b.keys(), (sorted(a), sorted(b))
        for key in a:
            _same(a[key], b[key])
    elif isinstance(a, list) and isinstance(b, list):
        assert len(a) == len(b), (a, b)
        for x, y in zip(a, b):
            _same(x, y)
    elif isinstance(a, bool) or isinstance(b, bool):
        assert a is b, (a, b)
    else:
        assert a == b, (a, b)


def _strict(text):
    """JSON without NaN or Infinity (hostDemo writes them as null)."""
    def refuse(c):
        raise ValueError(f"non-finite JSON number {c}")
    return json.loads(text, parse_constant=refuse)


def _files(d: Path) -> dict:
    return {p.name: p.read_bytes() for p in sorted(d.iterdir()) if p.suffix in (".ppm", ".pfm")}


def _compare(tmp_path, hosts, plugins, args, files=(), report=True, **env):
    (s, sd), (i, idir) = both(tmp_path, hosts, plugins, args, files, **env)
    assert s.returncode == i.returncode, (s.stderr, i.stderr)
    assert _files(sd) == _files(idir)
    if report:
        _same(_normalized(_strict((sd / "r.json").read_text())), _normalized(_strict((idir / "r.json").read_text())))
    # the plug-ins' messages on stdout ("<type> <id> <text>"), with --quiet
    assert s.stdout == i.stdout
    return s, i, sd, idir


Q = ["--no-std-paths", "--no-cache", "--quiet"]


@pytest.mark.parametrize("args", [
    # openfx-misc rendertest.sh / smoketest.sh
    [*Q, "-p", INVERT, "-c", "filter", "-d", "float", "--format", "pfm", "-o", "out", "--report", "r.json",
     "-i", "Source=ramp"],
    [*Q, "-p", BASIC, "-m", "1", "-c", "general", "-d", "short", "--format", "pfm", "-o", "out", "--report", "r.json",
     "-i", "Source=checker:16", "-i", "Mask=solid:0.25,0.5,0.75,1", "--frames", "0,0.5", "--scale", "0.5",
     "scale=2", "-s", "scaleComponents=1"],
    [*Q, "-p", INVERT, "-d", "byte", "--format", "pfm", "-o", "out", "--report", "r.json", "--frames", "0",
     "--window", "100,50,300,250", "-i", "Source=file:in.pfm@10,20"],
    [*Q, "-p", INVERT, "--format", "pfm", "-o", "out", "--report", "r.json", "--window", "-90,-90,-10,-10"],
    [*Q, "-p", RETIMER, "-c", "general", "-d", "float", "--format", "pfm", "-o", "out", "--report", "r.json",
     "--frames", "0,1,2", "-i", "Source=ramp", "-s", "Speed@0=0.5", "-s", "Speed@2=1.5"],
    [*Q, "-p", TESTER, "-d", "float", "--format", "pfm", "-o", "out", "--report", "r.json", "--frames", "0",
     "-i", "Source=ramp", "-s", "analyseButton"],
    [*Q, "-p", CHOICES, "--format", "pfm", "-o", "out", "--report", "r.json", "--frames", "0", "-d", "float"],
    # an output with infinite values: null statistics in both reports
    [*Q, "-p", MULTIPLY, "--format", "pfm", "-o", "out", "--report", "r.json", "--frames", "0", "-d", "float",
     "-i", "Source=solid:3e38,0.5,0.25,1", "--window", "0,0,4,4", "value=10,1,1,1"],
    # openfx-gmic render_plugins.py (the host arguments first)
    ["--ofx14", *Q, "--allow-unwritten", "--format", "pfm", "-d", "float", "-p", INVERT, "-c", "filter", "--frames",
     "0", "-o", "out", "--report", "r.json", "-i", "Source=file:in.pfm"],
    # openfx-io tests/hostdemo/run.sh, openfx-arena run.cmake: the default PPM output, the default input
    [*Q, "-p", INVERT, "-c", "filter", "-o", "out", "--report", "r.json"],
    [*Q, "-p", INVERT, "-c", "filter", "--frames", "0", "-o", "out", "-d", "float", "--report", "r.json"],
    # --components: an RGB output (Noise's rgbOutput, in a host that supports RGB), as PFM and PPM
    [*Q, "--components", "rgba,rgb,alpha", "-p", NOISE, "-c", "general", "-d", "float", "--format", "pfm", "-o",
     "out", "--report", "r.json", "--frames", "0", "-i", "Source=none", "rgbOutput=1"],
    [*Q, "--components", "rgb,rgba", "-p", NOISE, "-c", "general", "-o", "out", "--report", "r.json", "--frames",
     "0", "-i", "Source=none", "rgbOutput=1"],
], ids=["misc-invert", "misc-basic", "misc-file-window", "misc-window-outside", "misc-keys", "misc-button",
        "misc-choices", "inf", "gmic-ofx14", "io-ppm", "arena-ppm", "rgb-output-pfm", "rgb-output-ppm"])
def test_same_as_hostdemo(tmp_path, hosts, sample_plugins, args):
    rgb = np.random.default_rng(0).random((24, 32, 3), dtype=np.float32)
    s, i, sd, idir = _compare(tmp_path, hosts, sample_plugins, args, files=[("in.pfm", ofxhost.encode_pfm(rgb))])
    assert s.returncode == 0, s.stderr
    assert _files(sd)


def test_describe(tmp_path, hosts, sample_plugins):
    (s, sd), (i, idir) = both(tmp_path, hosts, sample_plugins, [*Q, "-p", BASIC, "-m", "1", "--describe", "d.json"])
    assert s.returncode == i.returncode == 0, (s.stderr, i.stderr)
    _same(_strict((sd / "d.json").read_text()), _strict((idir / "d.json").read_text()))


@pytest.mark.parametrize("profile", [[], ["--ofx14"]], ids=["default", "ofx14"])
def test_opengl_describe(tmp_path, hosts, sample_plugins, profile):
    """--opengl-describe: the host's kOfxImageEffectPropOpenGLRenderSupported is "true", which the OpenGL
    example (Examples/OpenGL) prints in its describe action."""
    if not any(p.glob("example-OpenGL.ofx.bundle") for p in sample_plugins):
        why = "the OpenGL example (Examples/OpenGL, CMake builds) is not in the plug-in directories"
        if os.environ.get("OFXH_REQUIRE") == "1":
            pytest.fail(f"{why} (OFXH_REQUIRE=1)")
        pytest.skip(why)
    (s, sd), (i, idir) = both(tmp_path, hosts, sample_plugins,
                              [*Q, *profile, "--opengl-describe", "-p", OPENGL, "--describe", "d.json"])
    assert s.returncode == i.returncode == 0, (s.stderr, i.stderr)
    _same(_strict((sd / "d.json").read_text()), _strict((idir / "d.json").read_text()))
    for p in (s, i):
        assert "Host has OpenGL render support: true" in p.stderr
        assert "Host has OpenGL render support: false" not in p.stderr


@pytest.mark.parametrize("args", [
    [*Q, "-p", "no.such.plugin", "--report", "r.json"],                  # a run error: the report is written
    [*Q, "-p", INVERT, "-c", "general", "--report", "r.json"],           # no general context
    [*Q, "-p", INVERT, "-i", "Nope=ramp", "--report", "r.json"],         # no such clip
    [*Q, "-p", INVERT, "-s", "nope=1", "--report", "r.json"],            # no such parameter
], ids=["plugin", "context", "clip", "param"])
def test_run_errors(tmp_path, hosts, sample_plugins, args):
    s, i, _, _ = _compare(tmp_path, hosts, sample_plugins, args)
    assert s.returncode == 1
    assert [x for x in s.stderr.splitlines() if x.startswith("Error")] == \
        [x for x in i.stderr.splitlines() if x.startswith("Error")]


@pytest.mark.parametrize("args", [
    ["--frames", "0,0"], ["--frames", "0,x"], ["--frames", "1e-400"], ["--scale", "0"], ["--scale", "1,2"],
    ["--window", "1,1,1,1"],
    ["--format", "png"], ["-d", "half"], ["-c", "paint"], ["-m", "-1"], ["-i", "Source=bogus"], ["-i", "=ramp"],
    ["-i", "Source=ramp#rg"], ["-i", "Source=#rgb"], ["-i", "Source=ramp#rgb#alpha"],
    ["-i", "Source=ramp", "-i", "Source=none"], ["--report", "a.json", "--describe", "b.json"], ["--bogus"],
    # -i Clip@Plane=spec (checked as hostDemo checks it; refused without --multiplane)
    ["-i", "Source@Nope=ramp"], ["-i", "@BackMotionVector=ramp"], ["-i", "Source@BackMotionVector=none"],
    ["-i", "Source@BackMotionVector=bogus"], ["-i", "Source@BackMotionVector=ramp"],
    ["-i", "Source@BackMotionVector=ramp#rgb"], ["-i", "Source@StereoDisparityLeft=file:in.pfm#premult#alpha"],
    ["-i", "Source@StereoDisparityLeft=ramp", "-i", "Source@StereoDisparityLeft=checker:4"],
    ["-p"], ["--opengl-describe"], ["--opengl-describe", "--report", "r.json"],
    ["--components", "rgba,bogus"], ["--components", "rgb,rgb"], ["--components", ""], ["--components"],
    ["--components", "OfxImageComponentRGB"],
], ids=lambda a: " ".join(a))
def test_usage_errors(tmp_path, hosts, sample_plugins, args):
    """Command-line errors: exit status 1, hostDemo's message (its first line), no report."""
    (s, _), (i, _) = both(tmp_path, hosts, sample_plugins, [*Q, *args])
    assert s.returncode == i.returncode == 1
    first = [x for x in s.stderr.splitlines() if x.startswith("Error")][:1]
    assert first and first == [x for x in i.stderr.splitlines() if x.startswith("Error")][:1]


def test_components_cache_file(tmp_path, hosts, sample_plugins):
    """hostDemo --components: a list other than the default gets its own plug-in cache file."""
    hostdemo, ofxh_dir = hosts
    for comps, cache in (("rgba,alpha", "hostDemoPluginCache.xml"),
                         ("rgba,rgb,alpha", "hostDemoPluginCache-components-rgba-rgb-alpha.xml")):
        d = tmp_path / comps.replace(",", "-")
        d.mkdir()
        p = _run([str(hostdemo), "--no-std-paths", "--quiet", "--components", comps, "-p", INVERT, "--describe",
                  "d.json"], d, _env(sample_plugins, ofxh_dir))
        assert p.returncode == 0, p.stderr
        assert sorted(x.name for x in d.glob("hostDemoPluginCache*.xml")) == [cache]


def test_plane_components_suffix(tmp_path, hosts, sample_plugins):
    """-i Clip@Plane=spec#rgb: a plane's components are the plane's, so both hosts refuse the suffix."""
    (s, _), (i, _) = both(tmp_path, hosts, sample_plugins, [*Q, "-i", "Source@BackMotionVector=ramp#alpha"])
    assert s.returncode == i.returncode == 1
    for p in (s, i):
        assert "(a plane has no #rgb/#alpha/#rgba suffix)" in p.stderr


def _nuke():
    import ofxh
    return bool(ofxh.build_config["OFX_EXTENSIONS_NUKE"])  # (hostDemo is built with the same options as ofxh)


def test_multiplane(tmp_path, hosts, sample_plugins):
    """--multiplane: a multi-plane host with the Nuke extension (README.md), on both backends; without it,
    both refuse it."""
    colour = [*Q, "-p", INVERT, "-i", "Source=ramp", "--frames", "0", "--format", "pfm", "-o", "out", "--report",
              "r.json"]
    args = colour + ["--multiplane", "-i", "Source@BackMotionVector=solid:0.5,0.25,0,1"]
    if not _nuke():
        (s, sd), (i, idir) = both(tmp_path, hosts, sample_plugins, args)
        assert s.returncode == i.returncode == 1
        assert "Error: --multiplane needs a hostDemo built with OFX_EXTENSIONS_NUKE" in s.stderr
        assert "Error: --multiplane needs a hostDemo built with OFX_EXTENSIONS_NUKE" in i.stderr
        assert not (sd / "r.json").exists() and not (idir / "r.json").exists()
        # (refused at the option, before the errors of the next options)
        (s, _), (i, _) = both(tmp_path / "order", hosts, sample_plugins, [*Q, "--multiplane", "--frames", "0,0"])
        assert s.returncode == i.returncode == 1
        first = [x for x in s.stderr.splitlines() if x.startswith("Error")][:1]
        assert first == ["Error: --multiplane needs a hostDemo built with OFX_EXTENSIONS_NUKE"]
        assert first == [x for x in i.stderr.splitlines() if x.startswith("Error")][:1]
        return
    s, _, sd, _ = _compare(tmp_path, hosts, sample_plugins, args)
    assert s.returncode == 0, s.stderr
    r = json.loads((sd / "r.json").read_text())
    assert r["inputs"]["Source"]["planes"] == {"BackMotionVector": "solid:0.5,0.25,0,1"}
    assert r["clipPreferences"]["clips"]["Source"]["planesPresent"] == [k.kFnOfxImagePlaneColour,
                                                                        k.kFnOfxImagePlaneBackwardMotionVector]
    assert r["clipPreferences"]["clips"]["Output"]["planesPresent"] == [k.kFnOfxImagePlaneColour]
    for x in r["renders"]:
        # (Invert isn't multi-plane: the Support library's default, kOfxStatReplyDefault)
        assert x["clipComponents"] == {"ok": True, "status": "kOfxStatReplyDefault", "clips": {},
                                       "passThroughClip": None, "passThroughTime": None, "passThroughView": None}
        assert x["isIdentity"]["identityPlane"] is None and x["render"]["ok"]
    # the same files as without --multiplane (the colour plane)
    (p, pd), _ = both(tmp_path / "colour", hosts, sample_plugins, colour)
    assert p.returncode == 0, p.stderr
    assert _files(sd) == _files(pd)


@pytest.mark.parametrize("args", [
    # planes of each kind, a file plane, at a render scale, two frames (and views): the planes are in the
    # report only (no sample plug-in fetches them, so they change no output file)
    [*Q, "--multiplane", "-p", INVERT, "-d", "float", "--format", "pfm", "-o", "out", "--report", "r.json",
     "--scale", "0.5", "-i", "Source=checker:8", "-i", "Source@StereoDisparityRight=ramp",
     "-i", "Source@ForwardMotionVector=file:in.pfm@4,2", "-i", "Source@StereoDisparityLeft=default",
     "-i", "Source@BackMotionVector=solid:0.5,-0.25,0,1"],
    # an identity (Basic's scale 1), a disconnected optional input (no plane present), the queries only
    [*Q, "--multiplane", "-p", BASIC, "-m", "1", "-c", "general", "--report", "r.json", "--query", "--frames", "0",
     "-i", "Mask=none", "-i", "Source@BackMotionVector=ramp", "scale=1"],
    # a run error: no such clip (the report is written)
    [*Q, "--multiplane", "-p", INVERT, "--report", "r.json", "-i", "Nope@BackMotionVector=ramp"],
], ids=["planes", "identity-query", "no-clip"])
def test_multiplane_same_as_hostdemo(tmp_path, hosts, sample_plugins, args):
    """--multiplane runs give the same files, report (inputs.planes, planesPresent, identityPlane,
    clipComponents), stdout and exit status on both backends."""
    if not _nuke():
        pytest.skip("--multiplane needs OFX_EXTENSIONS_NUKE")
    rgb = np.random.default_rng(1).random((12, 16, 3), dtype=np.float32)
    s, i, _, _ = _compare(tmp_path, hosts, sample_plugins, args, files=[("in.pfm", ofxhost.encode_pfm(rgb))])
    assert [x for x in s.stderr.splitlines() if x.startswith("Error")] == \
        [x for x in i.stderr.splitlines() if x.startswith("Error")]
    assert s.returncode == (1 if "Nope@BackMotionVector=ramp" in args else 0), s.stderr


def test_multiplane_describe_and_usage(tmp_path, hosts, sample_plugins):
    """--multiplane --describe (the plug-in may describe itself differently to a multi-plane host), and
    --multiplane --ofx14, refused by both with hostDemo's message."""
    if not _nuke():
        pytest.skip("--multiplane needs OFX_EXTENSIONS_NUKE")
    (s, sd), (i, idir) = both(tmp_path, hosts, sample_plugins,
                              [*Q, "--multiplane", "-p", BASIC, "-m", "1", "--describe", "d.json"])
    assert s.returncode == i.returncode == 0, (s.stderr, i.stderr)
    _same(_strict((sd / "d.json").read_text()), _strict((idir / "d.json").read_text()))
    (s, _), (i, _) = both(tmp_path / "ofx14", hosts, sample_plugins, [*Q, "--multiplane", "--ofx14"])
    assert s.returncode == i.returncode == 1
    first = [x for x in s.stderr.splitlines() if x.startswith("Error")][:1]
    assert first == ["Error: --multiplane and --ofx14 can't be used together (OFX 1.4 has no plane suite)"]
    assert first == [x for x in i.stderr.splitlines() if x.startswith("Error")][:1]


def test_ofx11_usage(tmp_path, hosts, sample_plugins):
    """--ofx11 (hostDemo only, README.md): hostDemo refuses -d short (the profile has byte and float depths
    only) and another profile with it; the in-process backend refuses --ofx11 itself."""
    for args, msg in ((["-d", "short"], "Error: -d short: the --ofx11 host only supports byte and float depths"),
                      (["--ofx14"], "Error: --ofx11 can't be used with --ofx14, --multiplane, --opengl-describe "
                                    "or --gpu=opengl")):
        (s, _), (i, _) = both(tmp_path / args[0].lstrip("-"), hosts, sample_plugins, [*Q, "--ofx11", *args])
        assert s.returncode == i.returncode == 1
        assert [x for x in s.stderr.splitlines() if x.startswith("Error")][:1] == [msg]
        assert [x for x in i.stderr.splitlines() if x.startswith("Error")][:1] == \
            ["Error: --ofx11: the in-process backend has no OFX 1.1 host profile: use hostDemo"]


def test_help(tmp_path, hosts, sample_plugins):
    (s, _), (i, _) = both(tmp_path, hosts, sample_plugins, ["-h"])
    assert s.returncode == i.returncode == 0
    for opt in ("-p id", "--format ppm|pfm", "--report file.json", "--describe file.json", "--ofx14", "--ofx11",
                "--opengl-describe", "--quiet", "#rgb, #alpha or #rgba", "--no-normalised-defaults",
                "--multiplane", "-i Clip@Plane=spec", "--components c,..."):
        assert opt in s.stdout and opt in i.stdout


@pytest.mark.parametrize("keep", ["0", "1"])
def test_keep_loaded(tmp_path, hosts, sample_plugins, keep):
    """OFX_HOSTDEMO_KEEP_LOADED=1 (ofxh.init(keep_loaded=True)): the scanned plug-in stays loaded, so its
    first use sends no second Load and Describe, as hostDemo (hostdemo-checks.sh's lifecycle checks)."""
    (s, _), (i, _) = both(tmp_path, hosts, sample_plugins, [*Q, "-p", PROPTESTER, "--describe", "d.json"],
                          OFX_PROPTESTER_TRACE="1", OFX_HOSTDEMO_KEEP_LOADED=keep)
    assert s.returncode == i.returncode == 0, (s.stderr, i.stderr)

    def trace(p):
        return re.findall(r"propTester: OfxAction([A-Za-z]*)", p.stdout + p.stderr)
    expected = ["Load", "Describe", "Unload"] if keep == "1" else ["Load", "Describe", "Unload"] * 2
    assert trace(s) == expected
    assert trace(i) == expected


@pytest.mark.parametrize("plugin, spec", [(INVERT, "ramp#rgb"), (MULTIPLY, "ramp#rgb"),
                                          (INVERT, "file:in.pfm#premult#rgb"), (INVERT, "ramp#rgba"),
                                          (INVERT, "checker:8#alpha"), (INVERT, "file:in.pfm#alpha")])
def test_components_suffix(tmp_path, hosts, sample_plugins, plugin, spec):
    """-i Clip=spec#rgb|#alpha|#rgba: the input's unmapped components; the clip preferences map them to the
    clip's (Invert: RGBA or Alpha; MultiplyGPU also RGB). With an Alpha input, HostSupport's default output is
    Alpha (#332), so Invert renders it."""
    rgb = np.random.default_rng(0).random((24, 32, 3), dtype=np.float32)
    args = [*Q, "-p", plugin, "-i", f"Source={spec}", "--frames", "0", "-d", "float", "--format", "pfm", "-o", "out",
            "--report", "r.json"]
    s, _, sd, _ = _compare(tmp_path, hosts, sample_plugins, args, files=[("in.pfm", ofxhost.encode_pfm(rgb))])
    alpha = spec.endswith("#alpha")
    assert s.returncode == 0, s.stderr
    r = json.loads((sd / "r.json").read_text())
    if alpha:
        assert r["clipPreferences"]["clips"]["Output"]["components"] == k.kOfxImageComponentAlpha
    comps = {"rgb": k.kOfxImageComponentRGB, "alpha": k.kOfxImageComponentAlpha}.get(spec.rpartition("#")[2],
                                                                                      k.kOfxImageComponentRGBA)
    assert r["inputs"]["Source"] == dict(r["inputs"]["Source"], spec=spec, unmappedComponents=comps)
    if (plugin, spec) == (INVERT, "ramp#rgb"):  # (an RGBA clip, opaque input: the inverted alpha is 0)
        alphas = sorted(sd.glob("out.0*.alpha.pfm"))  # (out.0l and out.0r: two views)
        assert alphas and all(np.all(ofxhost.read_pfm(a) == 0.) for a in alphas)


@pytest.mark.parametrize("extra", [[], ["--ofx14"]])
def test_no_normalised_defaults(tmp_path, hosts, sample_plugins, extra):
    """--no-normalised-defaults: the Tester's widgetPos (normalised default 0.5, 0.5) keeps its default as is,
    in canonical coordinates; without it, the default is the middle of the project."""
    args = [*Q, *extra, "-p", TESTER, "--query", "--frames", "0", "--report", "r.json"]
    values = []
    for opts in ([], ["--no-normalised-defaults"]):
        s, _, sd, _ = _compare(tmp_path / str(len(opts)), hosts, sample_plugins, args + opts)
        assert s.returncode == 0, s.stderr
        values.append(json.loads((sd / "r.json").read_text())["params"]["widgetPos"]["value"])
    assert values == [[384., 288.], [0.5, 0.5]]


def test_entry_points(tmp_path, hosts, sample_plugins):
    """python -m ofxhost.hostdemo ... and the hostdemo-inprocess script are the same."""
    hostdemo, ofxh_dir = hosts
    env = _env(sample_plugins, ofxh_dir, OFXHOST_PYTHON=sys.executable)
    for cmd in ([sys.executable, "-m", "ofxhost.hostdemo"], [str(PYDIR / "hostdemo-inprocess")]):
        p = _run(cmd + [*Q, "-p", INVERT, "--frames", "0", "-o", "e", "--report", "r.json"], tmp_path, env)
        assert p.returncode == 0, p.stderr
        files = sorted(tmp_path.glob("e.0*.ppm"))  # (e.0.ppm, or e.0l.ppm and e.0r.ppm with two views)
        assert files and all(f.stat().st_size > 0 for f in files)
        assert json.loads((tmp_path / "r.json").read_text())["ok"] is True
        for f in files:
            f.unlink()


# --- --gpu=opengl ------------------------------------------------------------------------------

def _need_opengl(tmp_path, hosts, sample_plugins):
    """--gpu=opengl renders with OpenGL here: ofxh has OFX_HOSTSUPPORT_OPENGL, the GPUGain and OpenGL
    examples are built, and hostDemo gets an OpenGL context; else skip (OFXH_REQUIRE=1: fail)."""
    import ofxh
    why = None
    if not ofxh.build_config.get("OFX_HOSTSUPPORT_OPENGL"):
        why = "ofxh was built without OFX_HOSTSUPPORT_OPENGL"
    elif not any(p.glob("example-OpenGL.ofx.bundle") for p in sample_plugins):
        why = "the OpenGL example (Examples/OpenGL, CMake builds) is not in the plug-in directories"
    else:
        probe = tmp_path / "probe"
        probe.mkdir()
        p = _run([str(hosts[0]), *Q, "--gpu=opengl", "-p", INVERT, "--frames", "0", "-o", "probe"], probe,
                 _env(sample_plugins, hosts[1]))
        if p.returncode != 0 or "no OpenGL context" in p.stderr:
            why = (p.stderr.strip().splitlines() or ["hostDemo --gpu=opengl failed"])[0]
    if why:
        if os.environ.get("OFXH_REQUIRE") == "1":
            pytest.fail(f"{why} (OFXH_REQUIRE=1)")
        pytest.skip(why)


def _renders(d):
    return [r["render"] for r in _strict((d / "r.json").read_text())["renders"]]


def _gl_flags(d):
    """The (openGL, cpuRetry, ok) of the renders (one per view: two in Nuke or Vegas builds)."""
    return {(r["openGL"], r["cpuRetry"], r["ok"]) for r in _renders(d)}


def _first_pfm(d):
    """The first output's colour file (out.0.pfm, or out.0l.pfm with two views)."""
    return sorted(p for p in d.glob("out.0*.pfm") if ".alpha." not in p.name)[0]


@pytest.mark.parametrize("source,scale,gl", [("checker:5", "0.5", True), ("solid:4,3,0.25,1", "0.5", True),
                                             ("checker:5", "2", False)], ids=["opengl", "above-1", "cpu-retry"])
def test_gpu_gpugain(tmp_path, hosts, sample_plugins, source, scale, gl):
    """--gpu=opengl with GPUGain ("true": OpenGL with a CPU path): with a scale in [0, 1], its
    fixed-function OpenGL render gives the CPU render's pixels; with scale 2 it returns
    kOfxStatGPURenderFailed, and the host renders again on the CPU ("the host should retry the render
    with OpenGL rendering disabled", ofxGPURender.h). Values above 1 aren't clamped, in the source or
    after the modulation (float targets: (4, 3) times 0.5 is (2, 1.5)). Both backends give the same
    report and files."""
    _need_opengl(tmp_path, hosts, sample_plugins)
    args = ["-p", GPUGAIN, "-d", "float", "-i", f"Source={source}", f"scale={scale}", "--frames", "0",
            "--window", "0,0,64,48", "--format", "pfm", "-o", "out", "--report", "r.json"]
    _, _, sd, idir = _compare(tmp_path / "gpu", hosts, sample_plugins, [*Q, "--gpu=opengl", *args])
    assert _gl_flags(sd) == {(gl, not gl, True)}
    _, _, cd, _ = _compare(tmp_path / "cpu", hosts, sample_plugins, [*Q, *args])
    assert "openGL" not in _renders(cd)[0]
    assert _files(sd) == _files(cd)  # the OpenGL render's pixels are the CPU render's


def test_gpu_opengl_example(tmp_path, hosts, sample_plugins):
    """--gpu=opengl with the OpenGL example ("needed"): the source, with a red rectangle from (10, 10)
    in the render window (pixels of the window). Without --gpu=opengl, the host doesn't support OpenGL
    render, and the plug-in's describe fails with kOfxStatErrMissingHostFeature: no render."""
    _need_opengl(tmp_path, hosts, sample_plugins)
    from ofxhost.pfm import read_pfm
    args = ["-p", OPENGL, "-d", "float", "-i", "Source=checker:5", "--frames", "0", "--window", "16,8,80,40",
            "--format", "pfm", "-o", "out", "--report", "r.json"]
    _, _, sd, _ = _compare(tmp_path / "gpu", hosts, sample_plugins, [*Q, "--gpu=opengl", *args])
    assert _gl_flags(sd) == {(True, False, True)}
    out = read_pfm(_first_pfm(sd))
    # the source, in the render window (16, 8, 80, 40): checker:5, squares of 5 pixels alternating 0
    # and 1 from 0 at the origin (hostDemo's InputSpec), top row first
    ys = np.arange(40 - 1, 8 - 1, -1)[:, None]
    xs = np.arange(16, 80)[None, :]
    source = np.repeat((((xs // 5) + (ys // 5)) % 2).astype(np.float32)[..., None], 3, axis=2)
    h, w = out.shape[:2]
    rows = np.arange(h)[::-1][:, None]  # (PFM: bottom row first; the arrays are top row first)
    cols = np.arange(w)[None, :]
    red = (rows >= 10) & (cols >= 10)
    expected = np.where(red[..., None], np.array([1., 0., 0.], np.float32), source)
    np.testing.assert_allclose(out[..., :3], expected, atol=1e-6)
    (s, _), (i, _) = both(tmp_path / "nogl", hosts, sample_plugins, [*Q, "-p", OPENGL, "--frames", "0", "-o", "x"])
    assert s.returncode == i.returncode == 1, (s.stderr, i.stderr)



def test_gpu_renderers(tmp_path, hosts, sample_plugins):
    """--gpu-renderers lists the same renderers (BACKEND:ID and names) in both; --gpu-renderer=BACKEND:ID
    renders GPUGain with OpenGL on each, as on the CPU; an unknown id, a backend with no renderer here
    (also with its default id, -1), a malformed one (no colon, an unknown backend, an id outside int),
    or one without --gpu=opengl is the same error in both
    (status 1, the same message, no report)."""
    _need_opengl(tmp_path, hosts, sample_plugins)
    s, i, _, _ = _compare(tmp_path / "list", hosts, sample_plugins, ["--gpu-renderers"], report=False)
    assert s.returncode == 0, s.stderr
    names = [line.split("\t")[0] for line in s.stdout.splitlines()]
    assert names, "no renderer listed"
    args = ["-p", GPUGAIN, "-d", "float", "-i", "Source=checker:5", "scale=0.5", "--frames", "0",
            "--window", "0,0,64,48", "--format", "pfm", "-o", "out", "--report", "r.json"]
    _, _, cd, _ = _compare(tmp_path / "cpu", hosts, sample_plugins, [*Q, *args])
    for n, name in enumerate(names):
        _, _, sd, _ = _compare(tmp_path / f"gpu{n}", hosts, sample_plugins,
                               [*Q, "--gpu=opengl", f"--gpu-renderer={name}", *args])
        assert _gl_flags(sd) == {(True, False, True)}, name
        assert _files(sd) == _files(cd), name
    backend = names[0].rpartition(":")[0]
    # (and a backend with no renderer here, with its default id: wgl outside Windows, e.g.)
    unlisted = [b for b in ("wgl", "cgl", "glx", "egl-surfaceless", "egl-device")
                if b not in {n.rpartition(":")[0] for n in names}][0]
    bads = [f"--gpu-renderer={backend}:{1 << 30}", f"--gpu-renderer={unlisted}:-1", "--gpu-renderer=nocolon",
            "--gpu-renderer=vulkan:0",
            f"--gpu-renderer={backend}:{1 << 32}", f"--gpu-renderer={backend}:0x1"]
    for n, (bad, gpu) in enumerate([(b, g) for b in bads for g in (["--gpu=opengl"], [])]):
        (s, sd), (i, idir) = both(tmp_path / f"bad{n}", hosts, sample_plugins, [*Q, *gpu, bad, *args])
        assert s.returncode == i.returncode == 1, (bad, gpu, s.stderr, i.stderr)
        assert s.stderr.splitlines()[:1] == i.stderr.splitlines()[:1], (bad, gpu)
        assert not (sd / "r.json").exists() and not (idir / "r.json").exists(), (bad, gpu)
