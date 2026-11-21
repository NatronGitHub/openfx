"""The batch mode of ofxhost.hostdemo (python -m ofxhost.hostdemo --batch, run_batch): its parser,
the same files, reports and output as one process per command line, the host's lifetime
(--fresh-host, --fresh-host-per-plugin), and the workers that crash or hang (killed and replaced,
the batch goes on). The workers are child processes: a crash never reaches the pytest process."""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from ofxhost import _batch
from ofxhost.hostdemo import run_batch
from ofxhost.testing import backend_available

PYDIR = Path(__file__).resolve().parents[1]  # HostSupport/examples/python

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
RETIMER = "net.sf.openfx.retimer"
Q = ["--no-std-paths", "--no-cache", "--quiet"]


# --- the parser (no ofxh needed) ------------------------------------------------------------

def test_parse_batch():
    cmds = _batch.parse_batch(
        "# a comment\n"
        "\n"
        "   # an indented comment\n"
        "-p x -i Source=ramp#rgb -o 'a b' --report \"c d.json\" name=a\\ b\n"
        "--cwd sub/dir --env OFX_PLUGIN_PATH=/p:/q --env GONE --env EMPTY= -p y -s z@1=2  \n")
    assert [c.line for c in cmds] == [4, 5]
    assert cmds[0].args == ["-p", "x", "-i", "Source=ramp#rgb", "-o", "a b", "--report", "c d.json", "name=a b"]
    assert cmds[0].cwd is None and cmds[0].env == {}
    assert cmds[1].args == ["-p", "y", "-s", "z@1=2"]
    assert cmds[1].cwd == "sub/dir"
    assert cmds[1].env == {"OFX_PLUGIN_PATH": "/p:/q", "GONE": None, "EMPTY": ""}
    # the batch options only lead a line: later, they are hostDemo's (which refuses them)
    assert _batch.parse_command("-p x --cwd d").args == ["-p", "x", "--cwd", "d"]
    # an argument list is not split again
    assert _batch.parse_command(["--cwd", "d", "-o", "a b"]).args == ["-o", "a b"]


@pytest.mark.parametrize("text,where", [("-p ok\n-p 'x\n", "line 2"), ("--cwd\n", "line 1"),
                                        ("-p ok\n\n--env =1 -p x\n", "line 3")])
def test_parse_errors(text, where):
    with pytest.raises(_batch.BatchSyntaxError, match=where):
        _batch.parse_batch(text)


def test_plugin_of():
    assert _batch._plugin_of([]) == INVERT
    assert _batch._plugin_of(["-p", "a", "-o", "-p", "-p", "b"]) == "b"  # (-o's value is "-p")


def _cli(args, cwd, env=None, stdin=None, timeout=300):
    e = dict(os.environ if env is None else env)
    e["PYTHONPATH"] = os.pathsep.join([str(PYDIR)] + ([e["PYTHONPATH"]] if e.get("PYTHONPATH") else []))
    p = subprocess.run([sys.executable, "-m", "ofxhost.hostdemo", *args], cwd=cwd, env=e, capture_output=True,
                       text=True, errors="replace", timeout=timeout,
                       **({"input": stdin} if stdin is not None else {"stdin": subprocess.DEVNULL}))
    assert p.returncode >= 0, f"killed by signal {-p.returncode}\n{p.stderr}"
    return p


@pytest.mark.parametrize("args", [["--batch", "nope.txt"], ["--batch", "-", "-j", "-1"], ["--batch", "-", "-p", "x"],
                                  ["--batch", "-", "--timeout", "0"],
                                  ["--batch", "-", "--fresh-host", "--fresh-host-per-plugin"]])
def test_cli_usage_errors(tmp_path, args):
    """The batch's own usage errors: exit status 2, before any worker (no ofxh needed)."""
    p = _cli(args, tmp_path, stdin="")
    assert p.returncode == 2, p.stderr
    assert p.stdout == ""


def test_cli_syntax_error(tmp_path):
    p = _cli(["--batch", "-"], tmp_path, stdin="-p ok\n-p 'unbalanced\n")
    assert p.returncode == 2 and "line 2" in p.stderr


def test_cli_help(tmp_path):
    p = _cli(["--batch", "-", "-h"], tmp_path)
    assert p.returncode == 0
    for opt in ("-j N", "--timeout SECONDS", "--fresh-host", "--fresh-host-per-plugin", "--batch-report FILE",
                "--cwd DIR", "--env NAME=VALUE"):
        assert opt in p.stdout


# --- runs (the ofxh module and the sample plug-ins) -----------------------------------------

@pytest.fixture(scope="module")
def ofxh_dir(sample_plugins):
    why = backend_available("inprocess")
    if why is not None:
        if os.environ.get("OFXH_REQUIRE") == "1":
            pytest.fail(f"backend inprocess: {why} (OFXH_REQUIRE=1)")
        pytest.skip(f"backend inprocess: {why}")
    import ofxh
    return Path(ofxh.__file__).resolve().parent


@pytest.fixture
def env(sample_plugins, ofxh_dir):
    e = dict(os.environ)
    e["OFX_PLUGIN_PATH"] = os.pathsep.join(str(p) for p in sample_plugins)
    e["PYTHONPATH"] = os.pathsep.join([str(PYDIR), str(ofxh_dir)])
    for k in ("OFX_HOSTDEMO_KEEP_LOADED", "OFXHOST_BATCH_TEST_HOOK", "OFXHOST_CAPTURE"):
        e.pop(k, None)
    return e


# command lines of the plug-in repositories' scripts (see test_hostdemo_cli.py), each with its own
# outputs; and failures: a run error (with its report) and a usage error
LINES = [
    [*Q, "-p", INVERT, "-c", "filter", "-d", "float", "--format", "pfm", "-o", "a", "--report", "a.json",
     "-i", "Source=ramp"],
    [*Q, "-p", BASIC, "-m", "1", "-c", "general", "-d", "short", "--format", "pfm", "-o", "b", "--report", "b.json",
     "-i", "Source=checker:16", "-i", "Mask=solid:0.25,0.5,0.75,1", "--frames", "0,0.5", "--scale", "0.5",
     "scale=2", "-s", "scaleComponents=1"],
    [*Q, "-p", BASIC, "-m", "1", "--describe", "c.json"],
    [*Q, "-p", RETIMER, "-c", "general", "-d", "float", "--format", "pfm", "-o", "d", "--report", "d.json",
     "--frames", "0,1,2", "-i", "Source=ramp", "-s", "Speed@0=0.5", "-s", "Speed@2=1.5"],
    [*Q, "-p", CHOICES, "--format", "pfm", "-o", "e", "--report", "e.json", "--frames", "0", "-d", "float"],
    [*Q, "-p", INVERT, "-c", "filter", "-o", "f", "--report", "f.json"],
    [*Q, "-p", INVERT, "--ofx14", "-d", "byte", "--format", "pfm", "-o", "g", "--report", "g.json", "--frames", "0",
     "--window", "100,50,300,250", "-i", "Source=ramp#rgb"],
    [*Q, "-p", "no.such.plugin", "--report", "h.json"],
    [*Q, "-p", INVERT, "--frames", "0,0"],
    ["-p", INVERT, "--frames", "0", "-o", "i"],  # (not --quiet: the plug-in list on stdout)
]


def _single(tmp_path, env, lines):
    """Each line in its own process (python -m ofxhost.hostdemo), all in one directory."""
    d = tmp_path / "single"
    d.mkdir()
    res = [_cli(a, d, env) for a in lines]
    return d, res


def _batch_run(tmp_path, name, env, lines, *opts):
    d = tmp_path / name
    d.mkdir()
    text = "".join(" ".join(_q(a) for a in args) + "\n" for args in lines)
    p = _cli(["--batch", "-", *opts], d, env, stdin=text)
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    return d, p, recs


def _q(a: str) -> str:
    import shlex
    return shlex.quote(a)


@pytest.mark.parametrize("opts", [[], ["-j", "3"], ["--fresh-host", "-j", "2"], ["--fresh-host-per-plugin"]],
                         ids=["j1", "j3", "fresh-j2", "fresh-per-plugin"])
def test_same_as_single_runs(tmp_path, env, opts):
    """Each command of a batch gives what a separate run gives: its exit status, stdout and stderr
    (hostDemo's error messages; with a reused host, the plug-in scan's output is in the stderr of
    the host's first command only), and the same files (byte for byte) and reports."""
    sd, single = _single(tmp_path, env, LINES)
    bd, p, recs = _batch_run(tmp_path, "batch", env, LINES, *opts)
    assert p.returncode == 1  # (the two failing lines)
    assert "batch: 10 commands, 2 failed (0 crashed, 0 timed out)" in p.stderr
    assert [r["line"] for r in recs] == list(range(1, len(LINES) + 1))
    for r, s, args in zip(recs, single, LINES):
        assert r["args"] == args
        assert r["cwd"] == str(bd)
        assert (r["exitStatus"], r["ok"], r["crashed"], r["timedOut"]) == (s.returncode, s.returncode == 0, False,
                                                                             False), (args, r["stderr"])
        assert r["stdout"] == s.stdout, args
        if "--fresh-host" in opts:
            assert r["stderr"] == s.stderr, args
        else:  # (what plug-ins print during the scan, e.g. a failed describe, only once per host)
            err = s.stderr.splitlines()
            assert all(x in err for x in r["stderr"].splitlines()), args
            assert [x for x in err if x.startswith("Error")] == \
                [x for x in r["stderr"].splitlines() if x.startswith("Error")], args
        assert r["seconds"] > 0
    assert [s.returncode for s in single] == [0] * 7 + [1, 1, 0]
    names = sorted(p.name for p in sd.iterdir())
    assert names == sorted(p.name for p in bd.iterdir())
    for n in names:  # (the images byte for byte, the JSON as values)
        if n.endswith(".json"):
            assert json.loads((sd / n).read_text()) == json.loads((bd / n).read_text()), n
        else:
            assert (sd / n).read_bytes() == (bd / n).read_bytes(), n


def test_cwd_env_and_report_file(tmp_path, env, sample_plugins):
    """--cwd (relative to the batch's directory), --env, and --batch-report: nothing on stdout."""
    (tmp_path / "x").mkdir()
    (tmp_path / "y").mkdir()
    bad = dict(env, OFX_PLUGIN_PATH=str(tmp_path))  # (the plug-ins come from the --env of the lines)
    path = os.pathsep.join(str(p) for p in sample_plugins)
    text = (f"--cwd x --env OFX_PLUGIN_PATH={_q(path)} {' '.join(Q)} -p {INVERT} -o out --frames 0\n"
            f"--cwd y --env OFX_PLUGIN_PATH={_q(path)} {' '.join(Q)} -p {INVERT} -o out --frames 1\n"
            f"{' '.join(Q)} -p {INVERT} -o out --frames 0\n"  # (no plug-in in the batch's own OFX_PLUGIN_PATH)
            "--cwd nowhere -p x\n")
    p = _cli(["--batch", "-", "--batch-report", "recs.jsonl", "-j", "2"], tmp_path, bad, stdin=text)
    assert p.returncode == 1 and p.stdout == ""
    recs = [json.loads(x) for x in (tmp_path / "recs.jsonl").read_text().splitlines()]
    assert [r["ok"] for r in recs] == [True, True, False, False]
    assert [r["cwd"] for r in recs] == [str(tmp_path / "x"), str(tmp_path / "y"), str(tmp_path),
                                        str(tmp_path / "nowhere")]
    assert (tmp_path / "x" / "out.0.ppm").exists() and (tmp_path / "y" / "out.1.ppm").exists()
    assert not (tmp_path / "out.0.ppm").exists()
    assert "not found" in recs[2]["stderr"]
    assert "no directory" in recs[3]["stderr"]


def _hook_line(hook, *more):
    return ["--env", f"OFXHOST_BATCH_TEST_HOOK={hook}", *Q, "-p", INVERT, "--frames", "0", *more]


def _host_numbers(recs):
    return [r["stdout"].strip().rsplit(" ", 1)[-1] for r in recs]


@pytest.mark.parametrize("fresh,expected", [
    (False, ["1", "1", "1", "1", "2", "2"]),       # a new host when the profile changes (--ofx14)
    (True, ["none"] * 6),                         # shut down after each command
    ("plugin", ["1", "1", "2", "3", "4", "4"]),   # and when the plug-in changes
], ids=["reuse", "fresh", "per-plugin"])
def test_host_lifetime(tmp_path, env, fresh, expected):
    """Which ofxh host each command runs on, in one worker (the test hook "host")."""
    cmds = [_hook_line("host"), _hook_line("host", "-o", "x"),
            _hook_line("host") + ["-p", BASIC, "-m", "1", "-c", "general"],
            _hook_line("host"), _hook_line("host", "--ofx14"), _hook_line("host", "--ofx14")]
    recs = run_batch(cmds, jobs=1, fresh_host=fresh, cwd=tmp_path, env=env)
    assert all(r["ok"] for r in recs), [r["stderr"] for r in recs]
    assert _host_numbers(recs) == expected


def test_crash_and_timeout(tmp_path, env):
    """A worker that dies (a signal, exit()) or hangs past the timeout: its command is crashed or
    timedOut, with the output it wrote before; the worker is replaced and the batch goes on."""
    hooks = ["kill", "exit", "raise", "hang"] + (["segv"] if sys.platform != "darwin" else [])
    # (no SIGSEGV on macOS: it would make a system crash report; SIGKILL kills the worker as well)
    cmds = []
    for h in hooks:
        cmds += [_hook_line("host"), _hook_line(h)]
    cmds.append(_hook_line("host"))
    recs = run_batch(cmds, jobs=1, timeout=10, cwd=tmp_path, env=env)
    assert len(recs) == len(cmds)
    for k, h in enumerate(hooks):
        before, r = recs[2 * k], recs[2 * k + 1]
        assert before["ok"] and before["stdout"].startswith("batch test hook: host"), before
        assert not r["ok"], r
        assert f"batch test hook: {h}\n" in r["stdout"], r
        if h == "raise":  # (a bug of the Python host: status 1 and a traceback, the worker lives)
            assert (r["exitStatus"], r["crashed"], r["timedOut"]) == (1, False, False)
            assert "RuntimeError: batch test hook: raise" in r["stderr"]
            continue
        assert f"batch test hook: {h} (stderr)\n" in r["stderr"], r
        if h == "hang":
            assert r["timedOut"] and not r["crashed"]
            assert 10 <= r["seconds"] < 60
        else:
            assert r["crashed"] and not r["timedOut"]
            assert r["exitStatus"] == {"kill": -9, "exit": 3, "segv": -11}[h] or sys.platform == "win32", r
        if h == "segv":
            assert "Fatal Python error: Segmentation fault" in r["stderr"]
    # a new worker after each crash or timeout (its first host); after "raise", the same worker,
    # whose host was shut down (its second host)
    assert _host_numbers([recs[2 * k + 2] for k in range(len(hooks))]) == ["2" if h == "raise" else "1"
                                                                           for h in hooks]
    assert recs[-1]["ok"]


def test_cli_crash_exit_status(tmp_path, env):
    """The batch goes on after a crash, and fails."""
    text = " ".join(map(_q, _hook_line("kill"))) + "\n" + " ".join(map(_q, [*Q, "-p", INVERT, "-o", "z"])) + "\n"
    p = _cli(["--batch", "-"], tmp_path, env, stdin=text)
    assert p.returncode == 1
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    assert [(r["crashed"], r["ok"]) for r in recs] == [(True, False), (False, True)]
    assert "1 failed (1 crashed, 0 timed out)" in p.stderr
    assert (tmp_path / "z.0.ppm").exists()


def test_api_commands(tmp_path, env):
    """run_batch takes strings, argument lists, dicts and Commands, and calls on_record in order."""
    seen = []
    (tmp_path / "sub").mkdir()
    recs = run_batch([f"{' '.join(Q)} -p {INVERT} -o s --frames 0",
                      [*Q, "-p", INVERT, "-o", "l", "--frames", "0"],
                      {"args": [*Q, "-p", INVERT, "-o", "d", "--frames", "0"], "cwd": "sub"},
                      _batch.Command([*Q, "-p", INVERT, "-o", "c", "--frames", "0"], env={"OFX_PLUGIN_PATH": None})],
                     jobs=2, cwd=tmp_path, env=env, on_record=seen.append)
    assert seen == recs
    assert [r["ok"] for r in recs] == [True, True, True, False]
    assert [r["line"] for r in recs] == [1, 2, 3, 4]
    for f in ("s.0.ppm", "l.0.ppm", "sub/d.0.ppm"):
        assert (tmp_path / f).exists(), f
    with pytest.raises(ValueError):
        run_batch([], fresh_host="sometimes")
