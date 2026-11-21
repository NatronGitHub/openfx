"""Batch mode of ofxhost.hostdemo: many hostDemo command lines in one Python, in a pool of worker
processes that each keep one ofxh host (NatronGitHub/openfx#136).

    python -m ofxhost.hostdemo --batch FILE|- [-j N] [--timeout SECONDS]
                               [--fresh-host | --fresh-host-per-plugin] [--batch-report FILE]
    ofxhost.hostdemo.run_batch(commands, jobs=1, timeout=None, fresh_host=False)  # the records

Starting Python and importing NumPy and ofxh costs about 0.2 s per run of python -m
ofxhost.hostdemo; a batch pays it once per worker. Each command is a line of hostDemo's arguments
(without the program name), split as a POSIX shell does (shlex): quotes and backslashes work, and
"#" is literal inside a line ("-i Source=ramp#rgb"); blank lines and lines whose first non-blank
character is "#" are ignored. A line may start with batch options, before hostDemo's:
  --cwd DIR          run the command in DIR (relative to the batch's directory); default: the
                     batch's current directory
  --env NAME=VALUE   set an environment variable for the command (e.g. OFX_PLUGIN_PATH,
                     OFX_HOSTDEMO_KEEP_LOADED); --env NAME unsets it
Each command runs as a separate python -m ofxhost.hostdemo would: it writes its own output files
and --report or --describe JSON (relative to its directory), and its stdout and stderr are captured
at the file-descriptor level (the plug-ins' printf and std::cerr too) into files of the batch's
temporary directory, which survive a worker's crash. The result of each command is a record,
written as one JSON line, in the order of the commands, on stdout (or to --batch-report FILE):

  {"line": 3, "args": [...], "cwd": "/abs/dir", "exitStatus": 0, "ok": true, "stdout": "...",
   "stderr": "...", "seconds": 0.05, "crashed": false, "timedOut": false}

exitStatus is the command's exit status (0 or 1, as hostDemo's), or, if the worker died during the
command (crashed: a signal, or a plug-in that called exit()), the worker's: negative for a signal
(-11 for SIGSEGV), as subprocess gives it; timedOut: the command ran longer than --timeout, and
its worker was killed (exitStatus -9 on POSIX). A crashed or timed-out command keeps what it wrote
before (its stdout and stderr), and the batch continues in a new worker. The batch exits with
status 0 if every command succeeded, 1 otherwise (2: a usage error of the batch options); a summary
goes to stderr. If a worker fails when it shuts down after its last command (e.g. a plug-in that
crashes in its unload action), an extra record with "line": null and "shutdown": true says so,
and the batch fails.

The host's lifetime (the trade-off between speed and the state that leaks between runs):
- by default, a worker keeps its ofxh host while the plug-in directories ($OFX_PLUGIN_PATH, that
  exist), the profile (--ofx14), OFX_HOSTDEMO_KEEP_LOADED and --no-std-paths are unchanged: a
  plug-in gets its load and describe actions at its first use in the worker, and unload when the
  host shuts down (at the end of the batch, or when the plug-in directories, the profile or
  those options change). This is the fastest, but what a plug-in keeps between its instances
  (its globals, caches, static data, and its load and describe actions seen once) is shared by
  the commands that a worker runs; and the messages of its unload action aren't in any command's
  stdout (they go to the worker's own stderr);
- --fresh-host (fresh_host=True): the host is shut down after each command (the plug-ins get their
  unload action, and their binaries are unloaded), as at the end of a separate run, so each
  command gets a new scan, load and describe. The process stays: what survives the unloading of a
  binary (libraries it loaded, globals of libraries that the OS doesn't really unload, the C
  runtime's state, the environment) is still shared;
- --fresh-host-per-plugin (fresh_host="plugin"): the host is shut down before a command whose
  plug-in (-p) differs from the worker's previous one: runs of the same plug-in share it.
One process per command (python -m ofxhost.hostdemo, or hostDemo) isolates the runs completely.
With -j N, the commands go to the first free worker, so in any of these modes the runs that share a
worker are not known in advance.

Also beyond a separate run: the ofxh module is imported once per worker (the first command's
$OFX_PLUGIN_PATH chooses its build tree, as for a separate run, see ofxhost._ofxh), and
$OFXHOST_PRELOAD loads its libraries once per worker.

Test hook (for ofxhost's tests only): $OFXHOST_BATCH_TEST_HOOK in a command's environment (--env)
makes the worker, after the command ran, write a line to its stdout and stderr, then: "segv" (a
real SIGSEGV), "kill" (SIGKILL; on Windows TerminateProcess, exit code 9), "exit" (os._exit(3)), "hang" (sleep),
or "raise" (an uncaught Python exception in the host); "host" only writes "batch test hook: host N"
to its stdout, N the number of the ofxh host in the worker (1, 2...), or "none" if it is shut down.
"""

from __future__ import annotations

import argparse
import ctypes
import dataclasses
import json
import os
import queue
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

__all__ = ["run_batch", "parse_batch", "parse_command", "Command", "main", "BATCH_USAGE"]

_PKG_PARENT = str(Path(__file__).resolve().parents[1])  # HostSupport/examples/python: ofxhost's parent

BATCH_USAGE = """\
Batch mode: run many hostDemo command lines in one Python (a pool of worker processes that each keep
an ofxh host), instead of one process per command line.
  --batch FILE|-          the command lines (one per line, shell-like quoting; blank lines and
                          lines starting with # are ignored), or - for stdin. A line may start with
                          --cwd DIR (its directory) and --env NAME=VALUE (its environment)
  -j N                    N worker processes (default 1; 0: one per CPU)
  --timeout SECONDS       kill a command's worker after SECONDS (the command is timedOut)
  --fresh-host            shut the OFX host down after each command (default: keep it while
                          the plug-in directories, the profile and the options are the same)
  --fresh-host-per-plugin shut it down when the plug-in (-p) changes
  --batch-report FILE     write the records (one JSON line per command) to FILE, not stdout
Exit status: 0 if every command succeeded, else 1. See ofxhost/_batch.py and the README."""


# --- the command lines ----------------------------------------------------------------------

@dataclasses.dataclass
class Command:
    """A command of a batch: hostDemo's arguments, the directory to run them in (None: the batch's)
    and the environment changes (a value None unsets the variable); line: its number in the batch
    (1-based)."""
    args: list
    cwd: str | None = None
    env: dict = dataclasses.field(default_factory=dict)
    line: int | None = None


class BatchSyntaxError(ValueError):
    """A line of a batch that can't be parsed (unbalanced quotes, an option without its value)."""


def parse_command(text_or_args, line: int | None = None) -> Command:
    """A batch line (a string, split as a POSIX shell does) or an argument list: its leading --cwd
    DIR and --env NAME[=VALUE] options, then hostDemo's arguments."""
    if isinstance(text_or_args, str):
        try:
            args = shlex.split(text_or_args, comments=False, posix=True)
        except ValueError as e:
            raise BatchSyntaxError(f"line {line}: {e}" if line is not None else str(e)) from None
    else:
        args = [os.fspath(a) for a in text_or_args]
    cmd = Command([], line=line)
    i = 0
    while i < len(args) and args[i] in ("--cwd", "--env"):
        if i + 1 >= len(args):
            raise BatchSyntaxError((f"line {line}: " if line is not None else "") + f"{args[i]} needs a value")
        if args[i] == "--cwd":
            cmd.cwd = args[i + 1]
        else:
            name, eq, value = args[i + 1].partition("=")
            if not name:
                raise BatchSyntaxError((f"line {line}: " if line is not None else "") +
                                       f'invalid --env "{args[i + 1]}", expected NAME=VALUE or NAME')
            cmd.env[name] = value if eq else None
        i += 2
    cmd.args = args[i:]
    return cmd


def parse_batch(text: str) -> list[Command]:
    """The commands of a batch file: one per line; blank lines and lines whose first non-blank
    character is # are ignored ("#" inside a line is literal, as in "-i Source=ramp#rgb")."""
    cmds = []
    for n, raw in enumerate(text.splitlines(), 1):
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        cmds.append(parse_command(s, n))
    return cmds


def _commands(commands) -> list[Command]:
    out = []
    for i, c in enumerate(commands, 1):
        if isinstance(c, Command):
            out.append(c if c.line is not None else dataclasses.replace(c, line=i))
        elif isinstance(c, Mapping):
            cmd = parse_command(c.get("args", []), c.get("line", i))
            if c.get("cwd") is not None:
                cmd.cwd = os.fspath(c["cwd"])
            cmd.env.update(c.get("env") or {})
            out.append(cmd)
        else:
            out.append(parse_command(c, i))
    return out


def _plugin_of(args: Sequence[str]) -> str:
    """The -p of a hostDemo command line (the last one, as hostDemo's parsing keeps)."""
    plugin = "net.sf.openfx.invertPlugin"
    i = 0
    while i < len(args):
        if args[i] == "-p" and i + 1 < len(args):
            plugin = args[i + 1]
            i += 1
        elif args[i] in ("-m", "-c", "-o", "-s", "-d", "-i", "--format", "--report", "--describe", "--frames",
                         "--scale", "--window"):
            i += 1  # (a value, which may be "-p")
        i += 1
    return plugin


# --- the worker -----------------------------------------------------------------------------

_fflush: Any = None


def _flush_all() -> None:
    """Python's and the C stdio buffers, before the file descriptors 1 and 2 move."""
    global _fflush
    for f in (sys.stdout, sys.stderr):
        try:
            f.flush()
        except (AttributeError, ValueError, OSError):
            pass
    if _fflush is None:
        try:
            lib = ctypes.CDLL("ucrtbase") if sys.platform == "win32" else ctypes.CDLL(None)
            _fflush = lib.fflush
            _fflush.argtypes = [ctypes.c_void_p]
        except (OSError, AttributeError):
            _fflush = False
    if _fflush:
        _fflush(None)


def _shutdown_host() -> None:
    ip = sys.modules.get("ofxhost.inprocess")
    if ip is not None:
        ip.shutdown()


_hosts: list = []  # (the test hook "host": the hosts seen, kept so that their ids aren't reused)


def _test_hook(name: str) -> None:
    if name == "host":  # (which ofxh host the command ran on: its number in this worker, or none)
        ip = sys.modules.get("ofxhost.inprocess")
        session = getattr(ip, "_session", None) if ip is not None else None
        if session is None:
            os.write(1, b"batch test hook: host none\n")
            return
        if not any(h is session.host for h in _hosts):
            _hosts.append(session.host)
        n = next(i for i, h in enumerate(_hosts, 1) if h is session.host)
        os.write(1, f"batch test hook: host {n}\n".encode())
        return
    os.write(1, f"batch test hook: {name}\n".encode())
    os.write(2, f"batch test hook: {name} (stderr)\n".encode())
    if name == "segv":
        import faulthandler
        faulthandler._sigsegv()
    elif name == "kill":
        os.kill(os.getpid(), 9)  # (SIGKILL; on Windows, TerminateProcess with exit code 9)
    elif name == "exit":
        os._exit(3)
    elif name == "hang":
        while True:
            time.sleep(3600)
    elif name == "raise":
        raise RuntimeError("batch test hook: raise")


def _run_one(req: dict, state: dict) -> int:
    """Run a request in the worker: the command, with fds 1 and 2 on its capture files, in its
    directory and environment. The exit status."""
    from .hostdemo import run
    fresh = req.get("fresh", "none")
    saved_env = {k: os.environ.get(k) for k in req.get("env", {})}
    files = {1: open(req["stdout"], "wb"), 2: open(req["stderr"], "wb")}
    saved: dict = {}
    _flush_all()
    for fd, f in files.items():
        saved[fd] = os.dup(fd)
        os.dup2(f.fileno(), fd)
    ret = 1
    try:
        for k, v in req.get("env", {}).items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        os.chdir(req["cwd"])
        if fresh == "plugin":
            plugin = _plugin_of(req["args"])
            if state.get("plugin") not in (None, plugin):
                _shutdown_host()
            state["plugin"] = plugin
        try:
            ret = run(req["args"], "python -m ofxhost.hostdemo", shutdown=fresh == "always")
            hook = os.environ.get("OFXHOST_BATCH_TEST_HOOK")
            if hook:
                _test_hook(hook)
        except Exception:  # (a bug of the Python host: as a separate run, a traceback and status 1)
            sys.stdout.flush()
            traceback.print_exc()
            ret = 1
            try:
                _shutdown_host()  # (its state is unknown)
            except Exception:
                traceback.print_exc()
    finally:
        _flush_all()
        for fd, d in saved.items():
            os.dup2(d, fd)
            os.close(d)
        for f in files.values():
            f.close()
        for k, v in saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        os.chdir(state["cwd"])
    return ret


def worker_main() -> int:
    """A worker (python -m ofxhost._batch --worker): requests on stdin, replies on stdout, one JSON
    line each. Between the commands, fds 1 and 2 go to the worker's stderr, and fd 0 is null."""
    import faulthandler
    rd = os.fdopen(os.dup(0), "r", encoding="utf-8")
    wr = os.fdopen(os.dup(1), "w", encoding="utf-8")
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    os.dup2(2, 1)
    faulthandler.enable()  # (on fd 2: a crash's Python traceback goes to the command's stderr)
    state = {"cwd": os.getcwd(), "plugin": None}
    for line in rd:
        req = json.loads(line)
        t0 = time.perf_counter()
        status = _run_one(req, state)
        wr.write(json.dumps({"id": req["id"], "exitStatus": status, "seconds": time.perf_counter() - t0}) + "\n")
        wr.flush()
    _flush_all()
    _shutdown_host()  # (the plug-ins' unload actions: their output goes to the worker's stderr)
    _flush_all()
    return 0


# --- the pool -------------------------------------------------------------------------------

class _Worker:
    def __init__(self, python: str, env: dict, cwd: str, log: str):
        self.log_path = log
        self.log = open(log, "ab")
        self.proc = subprocess.Popen([python, "-m", "ofxhost._batch", "--worker"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log, env=env, cwd=cwd,
                                     text=True, encoding="utf-8", bufsize=1)
        self.replies: queue.Queue = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            for line in self.proc.stdout:
                self.replies.put(json.loads(line))
        except (ValueError, OSError):
            pass
        self.replies.put(None)  # (EOF: the worker is gone)

    def send(self, req: dict) -> bool:
        try:
            self.proc.stdin.write(json.dumps(req) + "\n")
            self.proc.stdin.flush()
            return True
        except OSError:
            return False

    def kill(self):
        try:
            self.proc.kill()
        except OSError:
            pass
        self.proc.wait()
        self.close_files()

    def close_files(self):
        for f in (self.proc.stdin, self.proc.stdout):
            try:
                f.close()
            except OSError:
                pass
        self.log.close()

    def log_text(self) -> str:
        try:
            return Path(self.log_path).read_bytes().decode("utf-8", "replace")
        except OSError:
            return ""


def _read_text(path: str) -> str:
    try:
        return Path(path).read_bytes().decode("utf-8", "replace")
    except OSError:
        return ""


def _worker_env(env: Mapping | None) -> dict:
    e = dict(os.environ if env is None else env)
    pp = e.get("PYTHONPATH")
    e["PYTHONPATH"] = _PKG_PARENT + (os.pathsep + pp if pp else "")
    # (the ofxh module of this process, if it imported one: the workers use the same)
    ofxh = sys.modules.get("ofxh")
    if ofxh is not None and getattr(ofxh, "__file__", None):
        e["PYTHONPATH"] = str(Path(ofxh.__file__).resolve().parent) + os.pathsep + e["PYTHONPATH"]
    return e


def run_batch(commands: Iterable, jobs: int = 1, timeout: float | None = None, fresh_host: bool | str = False,
              cwd: str | os.PathLike | None = None, env: Mapping | None = None, python: str | None = None,
              on_record: Callable[[dict], None] | None = None) -> list[dict]:
    """Run hostDemo command lines in a pool of `jobs` worker processes (0: one per CPU), each with
    one ofxh host: the records (dicts, see the module docstring), in the order of the commands.

    commands: batch lines (strings, shell-like quoting, with the optional leading --cwd and --env),
    argument lists, Command objects, or dicts {"args", "cwd", "env"}. timeout: seconds per
    command (its worker is killed after that). fresh_host: False (keep the host while the plug-in
    directories, the profile and the options are the same), True (shut it down after each
    command), or "plugin" (when the plug-in changes). cwd: the batch's directory (default: the
    current one), where commands without --cwd run and relative --cwd start. env: the workers'
    environment (default: os.environ). python: the workers' Python (default: sys.executable).
    on_record: called with each record, in order, as soon as it and those before are done.
    A worker that fails at its shutdown adds a record with "line": None and "shutdown": True."""
    cmds = _commands(commands)
    fresh = {False: "none", None: "none", True: "always", "plugin": "plugin"}.get(fresh_host)
    if fresh is None:
        raise ValueError(f'fresh_host must be False, True or "plugin", not {fresh_host!r}')
    if timeout is not None and not timeout > 0:
        raise ValueError(f"timeout must be > 0, not {timeout!r}")
    jobs = int(jobs) or (os.cpu_count() or 1)
    if jobs < 0:
        raise ValueError(f"jobs must be >= 0, not {jobs}")
    jobs = max(1, min(jobs, len(cmds)))
    base = os.path.abspath(os.fspath(cwd) if cwd is not None else os.getcwd())
    wenv = _worker_env(env)
    py = python or sys.executable
    tmp = tempfile.mkdtemp(prefix="ofxhost-batch-")
    records: list = [None] * len(cmds)
    extra: list = []
    lock = threading.Lock()
    next_cmd = [0]
    emitted = [0]
    live: set = set()  # (the workers running)

    def emit():  # (with lock held) the records done, in order
        while emitted[0] < len(cmds) and records[emitted[0]] is not None:
            if on_record is not None:
                on_record(records[emitted[0]])
            emitted[0] += 1

    def slot(k: int):
        w: _Worker | None = None
        nlog = 0
        try:
            while True:
                with lock:
                    i = next_cmd[0]
                    if i >= len(cmds):
                        break
                    next_cmd[0] += 1
                c = cmds[i]
                rec = {"line": c.line, "args": list(c.args), "cwd": os.path.join(base, c.cwd) if c.cwd else base,
                       "exitStatus": 1, "ok": False, "stdout": "", "stderr": "", "seconds": 0.0, "crashed": False,
                       "timedOut": False}
                out, err = os.path.join(tmp, f"{i}.out"), os.path.join(tmp, f"{i}.err")
                if "--batch" in c.args:
                    rec["stderr"] = "Error: --batch inside a batch\n"
                elif not os.path.isdir(rec["cwd"]):
                    rec["stderr"] = f"Error: no directory {rec['cwd']} (--cwd)\n"
                else:
                    if w is None:
                        nlog += 1
                        try:
                            w = _Worker(py, wenv, base, os.path.join(tmp, f"worker{k}.{nlog}.log"))
                        except OSError as e:
                            rec["stderr"] = f"Error: cannot start a worker ({py}): {e}\n"
                            with lock:
                                records[i] = rec
                                emit()
                            continue
                        with lock:
                            live.add(w)
                    req = {"id": i, "args": rec["args"], "cwd": rec["cwd"], "env": c.env, "stdout": out,
                           "stderr": err, "fresh": fresh}
                    t0 = time.perf_counter()
                    reply = None
                    if w.send(req):
                        try:
                            reply = w.replies.get(timeout=timeout)
                        except queue.Empty:
                            rec["timedOut"] = True
                    rec["seconds"] = time.perf_counter() - t0
                    rec["stdout"], rec["stderr"] = _read_text(out), _read_text(err)
                    if reply is not None:
                        rec["exitStatus"] = reply["exitStatus"]
                        rec["ok"] = reply["exitStatus"] == 0
                    else:  # (timed out, or the worker died: kill it, the next command gets a new one)
                        w.kill()
                        rec["exitStatus"] = w.proc.returncode
                        rec["crashed"] = not rec["timedOut"]
                        log = w.log_text()
                        if log:
                            rec["workerStderr"] = log
                        with lock:
                            live.discard(w)
                        w = None
                    for p in (out, err):
                        try:
                            os.unlink(p)
                        except OSError:
                            pass
                with lock:
                    records[i] = rec
                    emit()
        finally:
            if w is not None:  # (the end: the worker shuts its host down and exits)
                try:
                    w.proc.stdin.close()
                except OSError:
                    pass
                try:
                    status = w.proc.wait(timeout=timeout if timeout else None)
                    timed_out = False
                except subprocess.TimeoutExpired:
                    w.kill()
                    status, timed_out = w.proc.returncode, True
                w.close_files()
                if status != 0:
                    with lock:
                        extra.append({"line": None, "args": [], "cwd": base, "exitStatus": status, "ok": False,
                                      "stdout": "", "stderr": w.log_text(), "seconds": 0.0,
                                      "crashed": not timed_out, "timedOut": timed_out, "shutdown": True})

    threads = [threading.Thread(target=slot, args=(k,), daemon=True) for k in range(jobs)]
    try:
        for t in threads:
            t.start()
        for t in threads:
            while t.is_alive():
                t.join(0.2)
    except BaseException:  # (an interrupt: no new command, and the workers are killed)
        with lock:
            next_cmd[0] = len(cmds)
            workers = list(live)
        for w in workers:
            w.kill()
        raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for r in extra:
        if on_record is not None:
            on_record(r)
    return records + extra


def main(argv: Sequence[str]) -> int:
    """python -m ofxhost.hostdemo --batch ...: argv without the program name."""
    p = argparse.ArgumentParser(prog="python -m ofxhost.hostdemo --batch", description=BATCH_USAGE,
                                formatter_class=argparse.RawDescriptionHelpFormatter, usage="%(prog)s FILE|- "
                                "[-j N] [--timeout SECONDS] [--fresh-host | --fresh-host-per-plugin] "
                                "[--batch-report FILE]")
    p.add_argument("--batch", required=True, metavar="FILE|-")
    p.add_argument("-j", type=int, default=1, metavar="N", dest="jobs")
    p.add_argument("--timeout", type=float, default=None, metavar="SECONDS")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--fresh-host", action="store_true")
    g.add_argument("--fresh-host-per-plugin", action="store_true")
    p.add_argument("--batch-report", metavar="FILE")
    a = p.parse_args(list(argv))
    if a.jobs < 0:
        p.error(f"-j: invalid number of workers {a.jobs}")
    if a.timeout is not None and not a.timeout > 0:
        p.error(f"--timeout: expected a positive number of seconds, not {a.timeout}")
    try:
        text = sys.stdin.read() if a.batch == "-" else Path(a.batch).read_text(encoding="utf-8")
    except OSError as e:
        p.error(f"cannot read {a.batch}: {e.strerror}")
    try:
        cmds = parse_batch(text)
    except BatchSyntaxError as e:
        p.error(f"{a.batch}: {e}")
    out = open(a.batch_report, "w", encoding="utf-8") if a.batch_report else sys.stdout

    def write(rec):
        out.write(json.dumps(rec) + "\n")
        out.flush()

    t0 = time.perf_counter()
    try:
        recs = run_batch(cmds, jobs=a.jobs, timeout=a.timeout,
                         fresh_host=True if a.fresh_host else "plugin" if a.fresh_host_per_plugin else False,
                         on_record=write)
    finally:
        if out is not sys.stdout:
            out.close()
    failed = [r for r in recs if not r["ok"]]
    crashed = sum(r["crashed"] for r in recs)
    timed = sum(r["timedOut"] for r in recs)
    sys.stderr.write(f"batch: {len(cmds)} commands, {len(failed)} failed ({crashed} crashed, {timed} timed out), "
                     f"{time.perf_counter() - t0:.2f} s\n")
    return 1 if failed else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--worker"]:
        sys.exit(worker_main())
    sys.exit(main(sys.argv[1:]))
