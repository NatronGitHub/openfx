"""The batch mode of ofxhost.hostdemo (python -m ofxhost.hostdemo --batch, run_batch): its parser,
the same files, reports and output as one process per command line, the host's lifetime (fresh,
per-plugin, reuse), the host shutdowns between commands, and the workers that crash or hang (killed
and replaced, the batch goes on). The workers are child processes: a crash never reaches the pytest
process (#215). (The two regressions of review round 2, a shutdown that raises before a command and a
killed batch, are in regression/test_batch_regressions.py.)"""

import json
import os
import shlex
import subprocess
import sys

import pytest

from ofxhost import _batch
from ofxhost.hostdemo import is_batch, run_batch, scan_options
from ofxhost.testing import without_support_debug
from ofxhost_helpers import HOOK, PYDIR, Q
from ofxhost_helpers import batch_line, hook_line, ppm_files

INVERT = "net.sf.openfx.invertPlugin"
BASIC = "net.sf.openfx.basicPlugin"
CHOICES = "org.openeffects.support.choiceParamsPlugin"
RETIMER = "net.sf.openfx.retimer"


# --- the parser (no ofxh needed) ------------------------------------------------------------

def test_parse_batch():
    """parse_batch(): comments and blank lines skipped, shell quoting, line numbers, and --cwd, --env and
    --timeout only at the start of a line (later, they are hostDemo's options); an argument list isn't split
    again (#215)."""
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
    """A batch file's syntax errors name their line: unbalanced quotes, a batch option without its value, an
    empty --env name, a timeout that isn't a positive finite number (#215)."""
    with pytest.raises(_batch.BatchSyntaxError, match=where):
        _batch.parse_batch(text)


def test_scan():
    """scan_options() finds the plug-in (-p, the last one, not an option's value) and the host options;
    is_batch() is true for --batch, not for --batch as an option's value (#215; f653cace, #297)."""
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


@pytest.mark.parametrize("args, stdin, message", [
    (["--batch", "nope.txt"], "", None), (["--batch", "-", "-j", "-1"], "", None),
    (["--batch", "-", "-p", "x"], "", None), (["--batch", "-", "--timeout", "0"], "", None),
    (["--batch", "-", "--fresh-host"], "", None),
    (["--batch", "-", "--reuse-host", "--fresh-host-per-plugin"], "", None),
    (["--batch", "-", "--batch-report", "no/such/dir/r.jsonl"], "", None),
    (["--batch", "-"], "-p ok\n-p 'unbalanced\n", "line 2"),  # a syntax error on stdin
], ids=["no-file", "j-negative", "hostdemo-option", "timeout-0", "fresh-host", "two-host-modes", "report-dir",
        "syntax-error"])
def test_cli_usage_errors(tmp_path, args, stdin, message):
    """The batch's own usage errors, and a syntax error in the batch file (its line named): exit status 2 and
    nothing on stdout, before any worker (no ofxh needed) (#215)."""
    p = _cli(args, tmp_path, stdin=stdin)
    assert p.returncode == 2, p.stderr
    assert p.stdout == ""
    if message is not None:
        assert message in p.stderr


def test_cli_help(tmp_path):
    """--batch -h lists the batch's options (#215)."""
    p = _cli(["--batch", "-", "-h"], tmp_path)
    assert p.returncode == 0
    for opt in ("-j N", "--timeout SECONDS", "--reuse-host", "--fresh-host-per-plugin", "--batch-report FILE",
                "--cwd DIR", "--env NAME=VALUE"):
        assert opt in p.stdout


# --- runs (the ofxh module and the sample plug-ins) -----------------------------------------

# command lines of the plug-in repositories' scripts (see test_hostdemo_cli.py::test_same_as_hostdemo), each
# with its own outputs; and failures: a run error (with its report) and a usage error
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
    host's first command only), and the same files, byte for byte, with each host lifetime and -j: the batch's
    main contract, on the plug-in repositories' command lines (#215)."""
    sd, single = single_runs
    bd = tmp_path / "batch"
    bd.mkdir()
    p = _cli(["--batch", "-", *opts], bd, env, stdin="".join(batch_line(a) + "\n" for a in LINES))
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    assert p.returncode == 1  # (the two failing lines)
    assert "batch: 10 commands, 2 failed (0 crashed, 0 timed out)" in p.stderr
    assert [r["line"] for r in recs] == list(range(1, len(LINES) + 1))
    for r, s, args in zip(recs, single, LINES):
        assert r["args"] == args and r["cwd"] == str(bd) and r["env"] == {}
        assert (r["exitStatus"], r["ok"], r["crashed"], r["timedOut"]) == (s.returncode, s.returncode == 0, False,
                                                                             False), (args, r["stderr"])
        assert without_support_debug(r["stdout"]) == without_support_debug(s.stdout), args
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
        if n != "ofxPluginLog.txt":  # (the Support library's debug log: handles, addresses, and one scan per host)
            assert (sd / n).read_bytes() == (bd / n).read_bytes(), n


def test_cwd_env_and_report_file(tmp_path, env, sample_plugins):
    """--cwd (relative to the batch's directory), --env, and --batch-report: nothing on stdout; a missing --cwd
    directory and a --batch inside a batch fail their command (#215)."""
    (tmp_path / "x").mkdir()
    (tmp_path / "y").mkdir()
    bad = dict(env, OFX_PLUGIN_PATH=str(tmp_path))  # (the plug-ins come from the --env of the lines)
    path = os.pathsep.join(str(p) for p in sample_plugins)
    text = (f"--cwd x --env OFX_PLUGIN_PATH={shlex.quote(path)} {batch_line(Q)} -p {INVERT} -o out --frames 0\n"
            f"--cwd y --env OFX_PLUGIN_PATH={shlex.quote(path)} {batch_line(Q)} -p {INVERT} -o out --frames 1\n"
            f"{batch_line(Q)} -p {INVERT} -o out --frames 0\n"  # (no plug-in in the batch's own OFX_PLUGIN_PATH)
            "--cwd nowhere -p x\n"
            "--batch=x.txt\n")
    p = _cli(["--batch", "-", "--batch-report", "recs.jsonl", "-j", "2"], tmp_path, bad, stdin=text)
    assert p.returncode == 1 and p.stdout == ""
    recs = [json.loads(x) for x in (tmp_path / "recs.jsonl").read_text().splitlines()]
    assert [r["ok"] for r in recs] == [True, True, False, False, False]
    assert [r["cwd"] for r in recs] == [str(tmp_path / "x"), str(tmp_path / "y"), str(tmp_path),
                                        str(tmp_path / "nowhere"), str(tmp_path)]
    assert recs[0]["env"] == {"OFX_PLUGIN_PATH": path} and recs[2]["env"] == {}
    assert all(f.exists() for f in ppm_files(tmp_path / "x", "out", 0) + ppm_files(tmp_path / "y", "out", 1))
    assert not any(f.exists() for f in ppm_files(tmp_path, "out", 0))
    assert "not found" in recs[2]["stderr"]
    assert "no directory" in recs[3]["stderr"]
    assert "--batch inside a batch" in recs[4]["stderr"]


def _host_numbers(recs):
    return [r["stdout"].strip().rsplit(" ", 1)[-1] for r in recs]


@pytest.mark.parametrize("host,expected", [
    ("fresh", ["none"] * 6),                         # shut down after each command
    ("per-plugin", ["1", "1", "2", "3", "4", "4"]),  # and when the plug-in or the profile changes
    ("reuse", ["1", "1", "1", "1", "2", "2"]),       # a new host when the profile changes (--ofx14)
])
def test_host_lifetime(tmp_path, env, host, expected):
    """Which ofxh host each command runs on, in one worker (the test hook "host"): fresh, per plug-in and
    profile, or reused until the profile changes (#215)."""
    cmds = [hook_line("host"), hook_line("host", "-o", "x"),
            hook_line("host") + ["-p", BASIC, "-m", "1", "-c", "general"],
            hook_line("host"), hook_line("host", "--ofx14"), hook_line("host", "--ofx14")]
    recs = run_batch(cmds, jobs=1, host=host, cwd=tmp_path, env=env)
    assert all(r["ok"] for r in recs), [r["stderr"] for r in recs]
    assert _host_numbers(recs) == expected
    assert [(r["worker"], r["seq"]) for r in recs] == [([0, 1], n) for n in range(1, 7)]


@pytest.mark.parametrize("args,expected", [
    (["--gpu=opengl", "--gpu=opencl"], ("opencl", "")),
    (["--gpu=opencl", "--gpu=cpu"], ("", "")),
    (["--gpu=opencl", "--gpu=opengl"], ("opengl", "")),
    (["--gpu=opengl", "--gpu-renderer=egl-device:0"], ("opengl", "egl-device:0")),
    (["--gpu-renderer=glx:1", "--gpu=opengl", "--gpu-renderer=cgl:0"], ("opengl", "cgl:0")),
    (["-o", "--gpu=opengl"], ("", "")),  # (-o's value)
])
def test_host_key_gpu(ofxh_dir, args, expected):
    """The host options that a command needs (whether a kept host can run it) have its GPU render options as the
    command gives them to the host: the last --gpu and the last --gpu-renderer, as hostDemo's parsing takes them
    (#377)."""
    from ofxhost import hostdemo
    o = hostdemo._parse(args, "hostDemo")
    assert (o["gpu"], o["gpu_renderer"]) == expected
    options = _batch._host_key(scan_options(args)[1], args)[2]
    assert (options.gpu, options.gpu_renderer) == expected


def test_hook_not_inherited(tmp_path, env):
    """The test hook only works from the command's own --env: inherited from the batch's environment, it does
    nothing (#215)."""
    recs = run_batch([[*Q, "-p", INVERT, "--frames", "0"]], cwd=tmp_path, env=dict(env, **{HOOK: "kill"}))
    assert recs[0]["ok"] and not recs[0]["crashed"]


@pytest.mark.parametrize("host", ["per-plugin", "reuse"])
def test_unload_between_commands(tmp_path, env, host):
    """A host shut down before the next command (here: --ofx14, another profile) is shut down on the
    worker's own stdout and stderr: what its unload prints is in no command's record, and a crash
    there is a worker event, not the next command's crash (the command runs in a new worker) (#215)."""
    recs = run_batch([hook_line("unload"), hook_line("host", "--ofx14")], host=host, cwd=tmp_path, env=env)
    assert [r["ok"] for r in recs] == [True, True]
    assert all("unload" not in without_support_debug(r["stdout"]) + r["stderr"] for r in recs), recs
    recs = run_batch([hook_line("unload-crash"), hook_line("host", "--ofx14"), hook_line("host", "--ofx14")],
                     host=host, cwd=tmp_path, env=env)
    cmds, events = recs[:3], recs[3:]
    assert [r["ok"] for r in cmds] == [True, True, True], cmds
    assert all("unload" not in without_support_debug(r["stdout"]) + r["stderr"] for r in cmds)
    assert [(r["worker"], r["seq"]) for r in cmds] == [([0, 1], 1), ([0, 2], 1), ([0, 2], 2)]
    assert len(events) == 1 and events[0]["event"] == "died" and events[0]["line"] is None
    assert events[0]["exitStatus"] == 5 and events[0]["crashed"] and events[0]["worker"] == [0, 1]
    assert events[0]["seq"] == 1  # (the last command that the worker ran)
    assert "batch test hook: unload (stderr)" in events[0]["stderr"]


def test_shutdown_failure_record(tmp_path, env):
    """A worker that fails when it shuts down at the end of the batch: an "exit" event, the batch fails (#215)."""
    recs = run_batch([hook_line("unload-crash")], host="reuse", cwd=tmp_path, env=env)
    assert recs[0]["ok"]
    assert [(r["line"], r["event"], r["exitStatus"]) for r in recs[1:]] == [(None, "exit", 5)]
    p = _cli(["--batch", "-", "--reuse-host"], tmp_path, env, stdin=batch_line(hook_line("unload-crash")) + "\n")
    assert p.returncode == 1 and "1 worker events" in p.stderr
    assert json.loads(p.stdout.splitlines()[-1])["event"] == "exit"


def test_crash_and_timeout(tmp_path, env):
    """A worker that dies (a signal, exit()) or hangs past the timeout: its command is crashed or
    timedOut, with the output it wrote before; the worker is replaced and the batch goes on (#215)."""
    hooks = ["kill", "exit", "raise", "hang"] + (["segv"] if sys.platform != "darwin" else [])
    # (no SIGSEGV on macOS: it would make a system crash report; SIGKILL kills the worker as well)
    # (the hang: the batch's timeout, 2 s, on a worker that has run a command; the others their own --timeout 60)
    long = ["--timeout", "60"]
    cmds = []
    for h in hooks:
        cmds += [[*long, *hook_line("host")], hook_line(h) if h == "hang" else [*long, *hook_line(h)]]
    cmds.append([*long, *hook_line("host")])
    recs = run_batch(cmds, jobs=1, timeout=2, cwd=tmp_path, env=env)
    assert len(recs) == len(cmds)
    gen = 1
    for k, h in enumerate(hooks):
        before, r, after = recs[2 * k], recs[2 * k + 1], recs[2 * k + 2]
        assert before["ok"] and without_support_debug(before["stdout"]) == "batch test hook: host none\n", before
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


@pytest.mark.parametrize("first, opts, crashed, timed_out, summary", [
    (["--timeout", "1", *hook_line("hang")], ["--timeout", "100", "-j", "0"], False, True,
     "1 failed (0 crashed, 1 timed out)"),
    (hook_line("kill"), [], True, False, "1 failed (1 crashed, 0 timed out)"),
], ids=["timeout", "crash"])
def test_cli_failed_command(tmp_path, env, first, opts, crashed, timed_out, summary):
    """The command line of a batch whose first command hangs or crashes: the batch goes on (the next command
    renders), fails (status 1), and its summary counts the command; its record says crashed or timedOut. The hang:
    the line's own --timeout overrides the batch's (100 s), with -j 0, and faulthandler dumps the worker's threads
    (#215)."""
    text = batch_line(first) + "\n" + batch_line([*Q, "-p", INVERT, "-o", "z"]) + "\n"
    p = _cli(["--batch", "-", *opts], tmp_path, env, stdin=text)
    assert p.returncode == 1
    recs = [json.loads(x) for x in p.stdout.splitlines()]
    assert [(r["crashed"], r["timedOut"], r["ok"]) for r in recs] == [(crashed, timed_out, False),
                                                                       (False, False, True)]
    if timed_out:
        assert 1 <= recs[0]["seconds"] < 30
        assert "Timeout (0:00:01)!" in recs[0]["stderr"]
    assert summary in p.stderr
    assert all(f.exists() for f in ppm_files(tmp_path, "z", 0))


def test_cli_closed_stdout(tmp_path, env):
    """The records can't be written (| head): the batch runs to the end, and exits with status 3 (#215)."""
    e = dict(env, PYTHONPATH=os.pathsep.join([str(PYDIR), env["PYTHONPATH"]]))
    r, w = os.pipe()
    try:
        p = subprocess.Popen([sys.executable, "-m", "ofxhost.hostdemo", "--batch", "-"], cwd=tmp_path, env=e,
                             stdin=subprocess.PIPE, stdout=w, stderr=subprocess.PIPE, text=True)
    finally:
        os.close(w)
        os.close(r)  # (no reader: the batch's writes get EPIPE)
    _, err = p.communicate(batch_line([*Q, "-p", INVERT, "-o", "a"]) + "\n"
                           + batch_line([*Q, "-p", INVERT, "-o", "b"]) + "\n", timeout=300)
    assert p.returncode == 3, err
    assert "cannot write the records" in err and "Traceback" not in err
    assert all(f.exists() for f in ppm_files(tmp_path, "b", 0))


def test_api_commands(tmp_path, env):
    """run_batch takes strings, argument lists, dicts and Commands, and calls on_record in order; bad host modes
    and timeouts are refused (#215)."""
    seen = []
    (tmp_path / "sub").mkdir()
    recs = run_batch([f"{batch_line(Q)} -p {INVERT} -o s --frames 0",
                      [*Q, "-p", INVERT, "-o", "l", "--frames", "0"],
                      {"args": [*Q, "-p", INVERT, "-o", "d", "--frames", "0"], "cwd": "sub", "timeout": 100},
                      _batch.Command([*Q, "-p", INVERT, "-o", "c", "--frames", "0"], env={"OFX_PLUGIN_PATH": None})],
                     jobs=2, cwd=tmp_path, env=env, on_record=seen.append)
    assert seen == recs
    assert [r["ok"] for r in recs] == [True, True, True, False]
    assert [r["line"] for r in recs] == [1, 2, 3, 4]
    for f in ppm_files(tmp_path, "s", 0) + ppm_files(tmp_path, "l", 0) + ppm_files(tmp_path, "sub/d", 0):
        assert f.exists(), f
    with pytest.raises(ValueError):
        run_batch([], host="sometimes")
    with pytest.raises(ValueError):
        run_batch([{"args": ["-p", "x"], "timeout": -1}])


def test_on_record_raises(tmp_path, env):
    """on_record raises: it isn't called again, the batch goes on, OnRecordError has every record (#215)."""
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
    assert all(f.exists() for f in ppm_files(tmp_path, "c", 0))


def test_internal_error_record(tmp_path, env, monkeypatch):
    """A bug of the batch while it handles a command: an internalError record, and the batch goes on (#215)."""
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
