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
from ofxhost.testing import backend_available

PYDIR = Path(__file__).resolve().parents[1]  # HostSupport/examples/python

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
RETIMER = "net.sf.openfx.retimer"
PROPTESTER = "net.sf.openfx.propertyTester"
MULTIPLY = "net.sf.openfx.MultiplyGPU"


@pytest.fixture(scope="module")
def hosts(sample_plugins):
    """(hostDemo, the ofxh module's directory): both are needed (OFXH_REQUIRE=1: fail if not)."""
    for b in ofxhost.BACKENDS:
        why = backend_available(b)
        if why is not None:
            if os.environ.get("OFXH_REQUIRE") == "1":
                pytest.fail(f"backend {b}: {why} (OFXH_REQUIRE=1)")
            pytest.skip(f"backend {b}: {why}")
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
        d.mkdir()
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
        for k in a:
            _same(a[k], b[k])
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
], ids=["misc-invert", "misc-basic", "misc-file-window", "misc-window-outside", "misc-keys", "misc-button",
        "misc-choices", "inf", "gmic-ofx14", "io-ppm", "arena-ppm"])
def test_same_as_hostdemo(tmp_path, hosts, sample_plugins, args):
    rgb = np.random.default_rng(0).random((24, 32, 3), dtype=np.float32)
    s, i, sd, idir = _compare(tmp_path, hosts, sample_plugins, args, files=[("in.pfm", ofxhost.encode_pfm(rgb))])
    assert s.returncode == 0, s.stderr
    assert _files(sd)


def test_describe(tmp_path, hosts, sample_plugins):
    (s, sd), (i, idir) = both(tmp_path, hosts, sample_plugins, [*Q, "-p", BASIC, "-m", "1", "--describe", "d.json"])
    assert s.returncode == i.returncode == 0, (s.stderr, i.stderr)
    _same(_strict((sd / "d.json").read_text()), _strict((idir / "d.json").read_text()))


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
    ["-i", "Source=ramp", "-i", "Source=none"], ["--report", "a.json", "--describe", "b.json"], ["--bogus"],
    ["-p"],
], ids=lambda a: " ".join(a))
def test_usage_errors(tmp_path, hosts, sample_plugins, args):
    """Command-line errors: exit status 1, hostDemo's message (its first line), no report."""
    (s, _), (i, _) = both(tmp_path, hosts, sample_plugins, [*Q, *args])
    assert s.returncode == i.returncode == 1
    first = [x for x in s.stderr.splitlines() if x.startswith("Error")][:1]
    assert first and first == [x for x in i.stderr.splitlines() if x.startswith("Error")][:1]


def test_help(tmp_path, hosts, sample_plugins):
    (s, _), (i, _) = both(tmp_path, hosts, sample_plugins, ["-h"])
    assert s.returncode == i.returncode == 0
    for opt in ("-p id", "--format ppm|pfm", "--report file.json", "--describe file.json", "--ofx14", "--quiet"):
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


def test_components_suffix(tmp_path, hosts, sample_plugins):
    """Beyond hostDemo: -i Clip=spec#rgb (the report gives the unmapped components)."""
    hostdemo, ofxh_dir = hosts
    p = _run([sys.executable, "-m", "ofxhost.hostdemo", *Q, "-p", INVERT, "-i", "Source=ramp#rgb", "--frames", "0",
              "--format", "pfm", "-o", "out", "--report", "r.json"], tmp_path, _env(sample_plugins, ofxh_dir))
    assert p.returncode == 0, p.stderr
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["inputs"]["Source"] == dict(r["inputs"]["Source"], spec="ramp#rgb", unmappedComponents="OfxImageComponentRGB")
    alphas = sorted(tmp_path.glob("out.0*.alpha.pfm"))  # (out.0l and out.0r with two views)
    assert alphas and all(np.all(ofxhost.read_pfm(a) == 0.) for a in alphas)  # (opaque, inverted)


def test_entry_points(tmp_path, hosts, sample_plugins):
    """python -m ofxhost hostdemo ... and the hostdemo-inprocess script are the same."""
    hostdemo, ofxh_dir = hosts
    env = _env(sample_plugins, ofxh_dir, OFXHOST_PYTHON=sys.executable)
    for cmd in ([sys.executable, "-m", "ofxhost", "hostdemo"], [str(PYDIR / "hostdemo-inprocess")]):
        p = _run(cmd + [*Q, "-p", INVERT, "--frames", "0", "-o", "e", "--report", "r.json"], tmp_path, env)
        assert p.returncode == 0, p.stderr
        files = sorted(tmp_path.glob("e.0*.ppm"))  # (e.0.ppm, or e.0l.ppm and e.0r.ppm with two views)
        assert files and all(f.stat().st_size > 0 for f in files)
        assert json.loads((tmp_path / "r.json").read_text())["ok"] is True
        for f in files:
            f.unlink()
