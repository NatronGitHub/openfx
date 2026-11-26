"""The batch mode of ofxhost.hostdemo (python -m ofxhost.hostdemo --batch, run_batch): its parser,
the same files, reports and output as one process per command line, the host's lifetime (fresh,
per-plugin, reuse), the host shutdowns between commands, and the workers that crash or hang (killed
and replaced, the batch goes on). The workers are child processes: a crash never reaches the pytest
process."""

import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

from ofxhost import _batch
from ofxhost.hostdemo import is_batch, run_batch, scan_options
from ofxhost.testing import check_backend

PYDIR = Path(__file__).resolve().parents[1]  # HostSupport/examples/python

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
RETIMER = "net.sf.openfx.retimer"
Q = ["--no-std-paths", "--no-cache", "--quiet"]
HOOK = "OFXHOST_BATCH_TEST_HOOK"


def _line(args) -> str:
    return " ".join(shlex.quote(a) for a in args)


# --- the parser (no ofxh needed) ------------------------------------------------------------

def test_parse_batch():
    cmds = _batch.parse_batch(
        "# a comment\n"
        "\n"
        "   # an indented comment\n"
        "-p x -i Source=ramp#rgb -o 'a b' --report \"c d.json\" name=a\\ b\n"
        "--cwd sub/dir --env OFX_PLUGIN_PATH=/p:/q --env GONE --env EMPTY= --timeout 2.5 -p y -s z@1=2  \n")
    assert [c.line for c in cmds] == [4, 5]
    assert cmds[0].args == ["-p", "x", "-i", "Source=ramp#rgb", "-o", "a b", "--report", "c d.json", "name=a b"]
    assert cmds[0].cwd is None and cmds[0].env == {} and cmds[0].timeout is None
    assert cmds[1].args == ["-p", "y", "-s", "z@1=2"]
    assert cmds[1].cwd == "sub/dir"
    assert cmds[1].env == {"OFX_PLUGIN_PATH": "/p:/q", "GONE": None, "EMPTY": ""}
    assert cmds[1].timeout == 2.5
    # the batch options only lead a line: later, they are hostDemo's (which refuses them)
    assert _batch.parse_command("-p x --cwd d").args == ["-p", "x", "--cwd", "d"]
    # an argument list is not split again
    assert _batch.parse_command(["--cwd", "d", "-o", "a b"]).args == ["-o", "a b"]


@pytest.mark.parametrize("text,where", [("-p ok\n-p 'x\n", "line 2"), ("--cwd\n", "line 1"),
                                        ("-p ok\n\n--env =1 -p x\n", "line 3"), ("--timeout 0 -p x\n", "line 1"),
                                        ("--timeout x -p x\n", "line 1"), ("--timeout inf\n", "line 1")])
def test_parse_errors(text, where):
    with pytest.raises(_batch.BatchSyntaxError, match=where):
        _batch.parse_batch(text)


def test_scan():
    assert scan_options([])[0] == INVERT
    assert scan_options(["-p", "a", "-o", "-p", "-p", "b"])[0] == "b"  # (-o's value is "-p")
    assert scan_options(["--ofx14", "-o", "--no-std-paths", "--quiet"]) == (INVERT, {"--ofx14", "--quiet"})
    # --batch as the value of an option isn't the batch mode
    assert is_batch(["-j", "2", "--batch", "x"]) and is_batch(["--batch=x"])
    assert not is_batch(["-o", "--batch", "-p", INVERT]) and not is_batch(["--report", "--batch=x"])


def _cli(args, cwd, env=None, stdin=None, timeout=300):
    e = dict(os.environ if env is None else env)
    e["PYTHONPATH"] = os.pathsep.join([str(PYDIR)] + ([e["PYTHONPATH"]] if e.get("PYTHONPATH") else []))
    p = subprocess.run([sys.executable, "-m", "ofxhost.hostdemo", *args], cwd=cwd, env=e, capture_output=True,
                       text=True, errors="replace", timeout=timeout,
                       **({"input": stdin} if stdin is not None else {"stdin": subprocess.DEVNULL}))
    assert p.returncode >= 0, f"killed by signal {-p.returncode}\n{p.stderr}"
    return p


@pytest.mark.parametrize("args", [["--batch", "nope.txt"], ["--batch", "-", "-j", "-1"], ["--batch", "-", "-p", "x"],
                                  ["--batch", "-", "--timeout", "0"], ["--batch", "-", "--fresh-host"],
                                  ["--batch", "-", "--reuse-host", "--fresh-host-per-plugin"],
                                  ["--batch", "-", "--batch-report", "no/such/dir/r.jsonl"]])
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
    for opt in ("-j N", "--timeout SECONDS", "--reuse-host", "--fresh-host-per-plugin", "--batch-report FILE",
                "--cwd DIR", "--env NAME=VALUE"):
        assert opt in p.stdout


# --- runs (the ofxh module and the sample plug-ins) -----------------------------------------

@pytest.fixture(scope="module")
def ofxh_dir(sample_plugins):
    check_backend("inprocess")
    import ofxh
    return Path(ofxh.__file__).resolve().parent


@pytest.fixture(scope="module")
def env(sample_plugins, ofxh_dir):
    e = dict(os.environ)
    e["OFX_PLUGIN_PATH"] = os.pathsep.join(str(p) for p in sample_plugins)
    e["PYTHONPATH"] = os.pathsep.join([str(PYDIR), str(ofxh_dir)])
    for k in ("OFX_HOSTDEMO_KEEP_LOADED", HOOK, "OFXHOST_CAPTURE"):
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


@pytest.fixture(scope="module")
def single_runs(tmp_path_factory, env):
    """Each line of LINES in its own process (python -m ofxhost.hostdemo), all in one directory: (the
    directory, the processes). Run once for the parametrizations of test_same_as_single_runs."""
    d = tmp_path_factory.mktemp("single")
    return d, [_cli(a, d, env) for a in LINES]


@pytest.mark.parametrize("opts", [[], ["-j", "3"], ["--fresh-host-per-plugin"], ["--reuse-host"],
                                  ["--reuse-host", "-j", "2"]],
                         ids=["fresh-j1", "fresh-j3", "per-plugin", "reuse-j1", "reuse-j2"])
def test_same_as_single_runs(tmp_path, env, single_runs, opts):
    """Each command of a batch gives what a separate run gives: its exit status, stdout and stderr
    (hostDemo's error messages; with a kept host, the plug-in scan's output is in the stderr of the
    host's first command only), and the same files, byte for byte."""
    sd, single = single_runs
    bd = tmp_path / "batch"
    bd.mkdir()
    p = _cli(["--batch", "-", *opts], bd, env, stdin="".join(_line(a) + "\n" for a in LINES))
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    assert p.returncode == 1  # (the two failing lines)
    assert "batch: 10 commands, 2 failed (0 crashed, 0 timed out)" in p.stderr
    assert [r["line"] for r in recs] == list(range(1, len(LINES) + 1))
    for r, s, args in zip(recs, single, LINES):
        assert r["args"] == args and r["cwd"] == str(bd) and r["env"] == {}
        assert (r["exitStatus"], r["ok"], r["crashed"], r["timedOut"]) == (s.returncode, s.returncode == 0, False,
                                                                             False), (args, r["stderr"])
        assert r["stdout"] == s.stdout, args
        if "--reuse-host" not in opts and "--fresh-host-per-plugin" not in opts:
            assert r["stderr"] == s.stderr, args
        else:  # (what plug-ins print during the scan, e.g. a failed describe, only once per host)
            err = s.stderr.splitlines()
            assert all(x in err for x in r["stderr"].splitlines()), args
            assert [x for x in err if x.startswith("Error")] == \
                [x for x in r["stderr"].splitlines() if x.startswith("Error")], args
        assert r["seconds"] > 0
        assert r["worker"][1] == 1 and r["seq"] >= 1
    if "-j" not in opts:
        assert [r["seq"] for r in recs] == list(range(1, len(LINES) + 1))
    assert [s.returncode for s in single] == [0] * 7 + [1, 1, 0]
    names = sorted(x.name for x in sd.iterdir())
    assert names == sorted(x.name for x in bd.iterdir())
    for n in names:
        assert (sd / n).read_bytes() == (bd / n).read_bytes(), n


def test_cwd_env_and_report_file(tmp_path, env, sample_plugins):
    """--cwd (relative to the batch's directory), --env, and --batch-report: nothing on stdout."""
    (tmp_path / "x").mkdir()
    (tmp_path / "y").mkdir()
    bad = dict(env, OFX_PLUGIN_PATH=str(tmp_path))  # (the plug-ins come from the --env of the lines)
    path = os.pathsep.join(str(p) for p in sample_plugins)
    text = (f"--cwd x --env OFX_PLUGIN_PATH={shlex.quote(path)} {_line(Q)} -p {INVERT} -o out --frames 0\n"
            f"--cwd y --env OFX_PLUGIN_PATH={shlex.quote(path)} {_line(Q)} -p {INVERT} -o out --frames 1\n"
            f"{_line(Q)} -p {INVERT} -o out --frames 0\n"  # (no plug-in in the batch's own OFX_PLUGIN_PATH)
            "--cwd nowhere -p x\n"
            "--batch=x.txt\n")
    p = _cli(["--batch", "-", "--batch-report", "recs.jsonl", "-j", "2"], tmp_path, bad, stdin=text)
    assert p.returncode == 1 and p.stdout == ""
    recs = [json.loads(x) for x in (tmp_path / "recs.jsonl").read_text().splitlines()]
    assert [r["ok"] for r in recs] == [True, True, False, False, False]
    assert [r["cwd"] for r in recs] == [str(tmp_path / "x"), str(tmp_path / "y"), str(tmp_path),
                                        str(tmp_path / "nowhere"), str(tmp_path)]
    assert recs[0]["env"] == {"OFX_PLUGIN_PATH": path} and recs[2]["env"] == {}
    assert (tmp_path / "x" / "out.0.ppm").exists() and (tmp_path / "y" / "out.1.ppm").exists()
    assert not (tmp_path / "out.0.ppm").exists()
    assert "not found" in recs[2]["stderr"]
    assert "no directory" in recs[3]["stderr"]
    assert "--batch inside a batch" in recs[4]["stderr"]


def _hook_line(hook, *more):
    return ["--env", f"{HOOK}={hook}", *Q, "-p", INVERT, "--frames", "0", *more]


def _host_numbers(recs):
    return [r["stdout"].strip().rsplit(" ", 1)[-1] for r in recs]


@pytest.mark.parametrize("host,expected", [
    ("fresh", ["none"] * 6),                         # shut down after each command
    ("per-plugin", ["1", "1", "2", "3", "4", "4"]),  # and when the plug-in or the profile changes
    ("reuse", ["1", "1", "1", "1", "2", "2"]),       # a new host when the profile changes (--ofx14)
])
def test_host_lifetime(tmp_path, env, host, expected):
    """Which ofxh host each command runs on, in one worker (the test hook "host")."""
    cmds = [_hook_line("host"), _hook_line("host", "-o", "x"),
            _hook_line("host") + ["-p", BASIC, "-m", "1", "-c", "general"],
            _hook_line("host"), _hook_line("host", "--ofx14"), _hook_line("host", "--ofx14")]
    recs = run_batch(cmds, jobs=1, host=host, cwd=tmp_path, env=env)
    assert all(r["ok"] for r in recs), [r["stderr"] for r in recs]
    assert _host_numbers(recs) == expected
    assert [(r["worker"], r["seq"]) for r in recs] == [([0, 1], n) for n in range(1, 7)]


def test_hook_not_inherited(tmp_path, env):
    """The test hook only works from the command's own --env."""
    recs = run_batch([[*Q, "-p", INVERT, "--frames", "0"]], cwd=tmp_path, env=dict(env, **{HOOK: "kill"}))
    assert recs[0]["ok"] and not recs[0]["crashed"]


@pytest.mark.parametrize("host", ["per-plugin", "reuse"])
def test_unload_between_commands(tmp_path, env, host):
    """A host shut down before the next command (here: --ofx14, another profile) is shut down on the
    worker's own stdout and stderr: what its unload prints is in no command's record, and a crash
    there is a worker event, not the next command's crash (the command runs in a new worker)."""
    recs = run_batch([_hook_line("unload"), _hook_line("host", "--ofx14")], host=host, cwd=tmp_path, env=env)
    assert [r["ok"] for r in recs] == [True, True]
    assert all("unload" not in r["stdout"] + r["stderr"] for r in recs), recs
    recs = run_batch([_hook_line("unload-crash"), _hook_line("host", "--ofx14"), _hook_line("host", "--ofx14")],
                     host=host, cwd=tmp_path, env=env)
    cmds, events = recs[:3], recs[3:]
    assert [r["ok"] for r in cmds] == [True, True, True], cmds
    assert all("unload" not in r["stdout"] + r["stderr"] for r in cmds)
    assert [(r["worker"], r["seq"]) for r in cmds] == [([0, 1], 1), ([0, 2], 1), ([0, 2], 2)]
    assert len(events) == 1 and events[0]["event"] == "died" and events[0]["line"] is None
    assert events[0]["exitStatus"] == 5 and events[0]["crashed"] and events[0]["worker"] == [0, 1]
    assert events[0]["seq"] == 1  # (the last command that the worker ran)
    assert "batch test hook: unload (stderr)" in events[0]["stderr"]


@pytest.mark.parametrize("host", ["per-plugin", "reuse"])
def test_unload_raises_before_start(tmp_path, env, host):
    """An exception in the host shutdown before a command (before its "started"): an error record for
    that command, without a wait for a "started" that never comes, and the worker goes on."""
    recs = run_batch([_hook_line("unload-raise"), _hook_line("host", "--ofx14"), _hook_line("host", "--ofx14")],
                     host=host, timeout=60, cwd=tmp_path, env=env)
    assert len(recs) == 3, recs  # (no worker event)
    assert recs[0]["ok"] and recs[2]["ok"], recs
    r = recs[1]
    assert not r["ok"] and not r["crashed"] and not r["timedOut"] and r["exitStatus"] == 1, r
    assert "RuntimeError: batch test hook: unload-raise" in r["stderr"], r
    assert [x["worker"] for x in recs] == [[0, 1]] * 3 and [x["seq"] for x in recs] == [1, 2, 3]


def _children(pid):
    p = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True)
    return [int(x) for x in p.stdout.split()]


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


@pytest.mark.skipif(os.name != "posix" or not shutil.which("pgrep"), reason="POSIX process groups, pgrep")
@pytest.mark.parametrize("sig", ["SIGTERM", "SIGKILL"])
def test_batch_killed_leaves_no_worker(tmp_path, env, sig):
    """The workers have their own process group: a SIGTERM of the batch kills them (its handler), and
    after a SIGKILL of the batch they end by themselves (they watch their parent)."""
    (tmp_path / "tmp").mkdir()  # (the batch's temporary directory, where the commands' output is captured)
    e = dict(env, PYTHONPATH=os.pathsep.join([str(PYDIR), env["PYTHONPATH"]]), TMPDIR=str(tmp_path / "tmp"))

    def hanging():
        """The commands that reached their hang hook."""
        return [f for f in (tmp_path / "tmp").glob("ofxhost-batch-*/*.out") if b"hook: hang" in f.read_bytes()]

    with subprocess.Popen([sys.executable, "-m", "ofxhost.hostdemo", "--batch", "-", "-j", "2", "--timeout", "200"],
                          cwd=tmp_path, env=e, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True) as p:
        try:
            p.stdin.write(_line(_hook_line("hang")) + "\n" + _line(_hook_line("hang")) + "\n")
            p.stdin.close()
            deadline = time.monotonic() + 120
            while (len(_children(p.pid)) < 2 or len(hanging()) < 2) and time.monotonic() < deadline:
                time.sleep(0.1)
            workers = _children(p.pid)
            assert len(workers) == 2 and len(hanging()) == 2, (workers, hanging())
            assert all(_alive(w) for w in workers)
            p.send_signal(getattr(signal, sig))
            p.wait(timeout=60)
            deadline = time.monotonic() + 20
            while any(_alive(w) for w in workers) and time.monotonic() < deadline:
                time.sleep(0.1)
            assert not any(_alive(w) for w in workers), workers
            assert p.returncode == (128 + signal.SIGTERM if sig == "SIGTERM" else -signal.SIGKILL), p.stderr.read()
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()


def test_shutdown_failure_record(tmp_path, env):
    """A worker that fails when it shuts down at the end of the batch: an "exit" event, the batch fails."""
    recs = run_batch([_hook_line("unload-crash")], host="reuse", cwd=tmp_path, env=env)
    assert recs[0]["ok"]
    assert [(r["line"], r["event"], r["exitStatus"]) for r in recs[1:]] == [(None, "exit", 5)]
    p = _cli(["--batch", "-", "--reuse-host"], tmp_path, env, stdin=_line(_hook_line("unload-crash")) + "\n")
    assert p.returncode == 1 and "1 worker events" in p.stderr
    assert json.loads(p.stdout.splitlines()[-1])["event"] == "exit"


def test_crash_and_timeout(tmp_path, env):
    """A worker that dies (a signal, exit()) or hangs past the timeout: its command is crashed or
    timedOut, with the output it wrote before; the worker is replaced and the batch goes on."""
    hooks = ["kill", "exit", "raise", "hang"] + (["segv"] if sys.platform != "darwin" else [])
    # (no SIGSEGV on macOS: it would make a system crash report; SIGKILL kills the worker as well)
    # (the hang: the batch's timeout, 2 s, on a worker that has run a command; the others their own --timeout 60)
    long = ["--timeout", "60"]
    cmds = []
    for h in hooks:
        cmds += [[*long, *_hook_line("host")], _hook_line(h) if h == "hang" else [*long, *_hook_line(h)]]
    cmds.append([*long, *_hook_line("host")])
    recs = run_batch(cmds, jobs=1, timeout=2, cwd=tmp_path, env=env)
    assert len(recs) == len(cmds)
    gen = 1
    for k, h in enumerate(hooks):
        before, r, after = recs[2 * k], recs[2 * k + 1], recs[2 * k + 2]
        assert before["ok"] and before["stdout"] == "batch test hook: host none\n", before
        assert before["worker"] == r["worker"] == [0, gen] and r["seq"] == before["seq"] + 1
        assert not r["ok"], r
        assert f"batch test hook: {h}\n" in r["stdout"], r
        if h == "raise":  # (a bug of the Python host: status 1 and a traceback, the worker lives)
            assert (r["exitStatus"], r["crashed"], r["timedOut"]) == (1, False, False)
            assert "RuntimeError: batch test hook: raise" in r["stderr"]
            assert after["worker"] == [0, gen] and after["seq"] == r["seq"] + 1
            continue
        assert f"batch test hook: {h} (stderr)\n" in r["stderr"], r
        if h == "hang":
            assert r["timedOut"] and not r["crashed"]
            assert 2 <= r["seconds"] < 60
            # (faulthandler's dump of the worker's Python threads, before it was killed)
            assert "Timeout (0:00:02)!" in r["stderr"] and "most recent call first" in r["stderr"], r["stderr"]
        else:
            assert r["crashed"] and not r["timedOut"]
            assert r["exitStatus"] == {"kill": -9, "exit": 3, "segv": -11}[h] or sys.platform == "win32", r
        if h == "segv":
            assert "Fatal Python error: Segmentation fault" in r["stderr"]
        gen += 1  # (a new worker for the next command)
        assert after["worker"] == [0, gen] and after["seq"] == 1
    assert recs[-1]["ok"]


def test_cli_timeouts(tmp_path, env):
    """--timeout, and a line's own --timeout, which overrides it; -j 0."""
    text = _line(["--timeout", "1", *_hook_line("hang")]) + "\n" + _line([*Q, "-p", INVERT, "-o", "z"]) + "\n"
    p = _cli(["--batch", "-", "--timeout", "100", "-j", "0"], tmp_path, env, stdin=text)
    assert p.returncode == 1
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    assert [(r["timedOut"], r["ok"]) for r in recs] == [(True, False), (False, True)]
    assert 1 <= recs[0]["seconds"] < 30
    assert "Timeout (0:00:01)!" in recs[0]["stderr"]
    assert "1 failed (0 crashed, 1 timed out)" in p.stderr
    assert (tmp_path / "z.0.ppm").exists()


def test_cli_crash_exit_status(tmp_path, env):
    """The batch goes on after a crash, and fails."""
    text = _line(_hook_line("kill")) + "\n" + _line([*Q, "-p", INVERT, "-o", "z"]) + "\n"
    p = _cli(["--batch", "-"], tmp_path, env, stdin=text)
    assert p.returncode == 1
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    assert [(r["crashed"], r["ok"]) for r in recs] == [(True, False), (False, True)]
    assert "1 failed (1 crashed, 0 timed out)" in p.stderr
    assert (tmp_path / "z.0.ppm").exists()


def test_cli_closed_stdout(tmp_path, env):
    """The records can't be written (| head): the batch runs to the end, and exits with status 3."""
    e = dict(env, PYTHONPATH=os.pathsep.join([str(PYDIR), env["PYTHONPATH"]]))
    r, w = os.pipe()
    try:
        p = subprocess.Popen([sys.executable, "-m", "ofxhost.hostdemo", "--batch", "-"], cwd=tmp_path, env=e,
                             stdin=subprocess.PIPE, stdout=w, stderr=subprocess.PIPE, text=True)
    finally:
        os.close(w)
        os.close(r)  # (no reader: the batch's writes get EPIPE)
    _, err = p.communicate(_line([*Q, "-p", INVERT, "-o", "a"]) + "\n" + _line([*Q, "-p", INVERT, "-o", "b"]) + "\n",
                           timeout=300)
    assert p.returncode == 3, err
    assert "cannot write the records" in err and "Traceback" not in err
    assert (tmp_path / "b.0.ppm").exists()


def test_api_commands(tmp_path, env):
    """run_batch takes strings, argument lists, dicts and Commands, and calls on_record in order."""
    seen = []
    (tmp_path / "sub").mkdir()
    recs = run_batch([f"{_line(Q)} -p {INVERT} -o s --frames 0",
                      [*Q, "-p", INVERT, "-o", "l", "--frames", "0"],
                      {"args": [*Q, "-p", INVERT, "-o", "d", "--frames", "0"], "cwd": "sub", "timeout": 100},
                      _batch.Command([*Q, "-p", INVERT, "-o", "c", "--frames", "0"], env={"OFX_PLUGIN_PATH": None})],
                     jobs=2, cwd=tmp_path, env=env, on_record=seen.append)
    assert seen == recs
    assert [r["ok"] for r in recs] == [True, True, True, False]
    assert [r["line"] for r in recs] == [1, 2, 3, 4]
    for f in ("s.0.ppm", "l.0.ppm", "sub/d.0.ppm"):
        assert (tmp_path / f).exists(), f
    with pytest.raises(ValueError):
        run_batch([], host="sometimes")
    with pytest.raises(ValueError):
        run_batch([{"args": ["-p", "x"], "timeout": -1}])


def test_on_record_raises(tmp_path, env):
    """on_record raises: it isn't called again, the batch goes on, OnRecordError has every record."""
    calls = []

    def broken(rec):
        calls.append(rec)
        raise BrokenPipeError(32, "Broken pipe")

    with pytest.raises(_batch.OnRecordError) as e:
        run_batch([[*Q, "-p", INVERT, "-o", n, "--frames", "0"] for n in "abc"], cwd=tmp_path, env=env,
                  on_record=broken)
    assert len(calls) == 1
    assert [r["ok"] for r in e.value.records] == [True] * 3
    assert isinstance(e.value.__cause__, BrokenPipeError)
    assert (tmp_path / "c.0.ppm").exists()


def test_internal_error_record(tmp_path, env, monkeypatch):
    """A bug of the batch while it handles a command: an internalError record, and the batch goes on."""
    real = _batch._read_text
    failed = []

    def read_text(path):
        if path.endswith(".out") and not failed:
            failed.append(path)
            raise RuntimeError("injected")
        return real(path)

    monkeypatch.setattr(_batch, "_read_text", read_text)
    recs = run_batch([[*Q, "-p", INVERT, "-o", n, "--frames", "0"] for n in "ab"], cwd=tmp_path, env=env)
    assert not recs[0]["ok"] and "RuntimeError: injected" in recs[0]["internalError"]
    assert recs[1]["ok"] and recs[1]["worker"] == [0, 2]
