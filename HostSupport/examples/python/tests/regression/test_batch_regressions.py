"""Regression tests of the batch mode of ofxhost.hostdemo (python -m ofxhost.hostdemo --batch, run_batch): the
bugs found in review round 2 of #215 (49d388ca). The workers are child processes: a crash never reaches the
pytest process."""

import os
import shutil
import signal
import subprocess
import sys
import time

import pytest

from ofxhost.hostdemo import run_batch
from ofxhost_helpers import PYDIR, batch_line, hook_line


@pytest.mark.parametrize("host", ["per-plugin", "reuse"])
def test_unload_raises_before_start(tmp_path, env, host):
    """An exception in the host shutdown before a command (before its "started"): an error record for
    that command, without a wait for a "started" that never comes, and the worker goes on. Before the fix, the
    parent waited for that "started" (review round 2 of #215, 49d388ca)."""
    recs = run_batch([hook_line("unload-raise"), hook_line("host", "--ofx14"), hook_line("host", "--ofx14")],
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
    after a SIGKILL of the batch they end by themselves (they watch their parent). Before the fix, they
    outlived the batch (review round 2 of #215, 49d388ca)."""
    (tmp_path / "tmp").mkdir()  # (the batch's temporary directory, where the commands' output is captured)
    e = dict(env, PYTHONPATH=os.pathsep.join([str(PYDIR), env["PYTHONPATH"]]), TMPDIR=str(tmp_path / "tmp"))

    def hanging():
        """The commands that reached their hang hook."""
        return [f for f in (tmp_path / "tmp").glob("ofxhost-batch-*/*.out") if b"hook: hang" in f.read_bytes()]

    with subprocess.Popen([sys.executable, "-m", "ofxhost.hostdemo", "--batch", "-", "-j", "2", "--timeout", "200"],
                          cwd=tmp_path, env=e, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True) as p:
        try:
            p.stdin.write(batch_line(hook_line("hang")) + "\n" + batch_line(hook_line("hang")) + "\n")
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
