"""Batch mode of ofxhost.hostdemo.

Many hostDemo command lines in one Python, in a pool of worker processes, instead of paying the start of Python and
the imports (0.1 to 0.2 s) for each.

    python -m ofxhost.hostdemo --batch FILE|- [-j N] [--timeout SECONDS]
                               [--reuse-host | --fresh-host-per-plugin] [--batch-report FILE]
    ofxhost.hostdemo.run_batch(commands, jobs=1, timeout=None, host="fresh")   # the records

Each command line runs as a separate run would, in its directory (--cwd) and environment (--env),
with its stdout and stderr captured at the file-descriptor level; a crashed or hung command
(--timeout) is recorded, and its worker replaced. The workers are child processes that speak JSON
lines over pipes, in their own process group. README.md describes the command lines, the records,
the exit status and the host's lifetime (fresh, per-plugin, reuse).
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

from . import stdio
from .hostdemo import is_batch, scan_options

__all__ = ["run_batch", "parse_batch", "parse_command", "Command", "BatchSyntaxError", "OnRecordError", "HOSTS",
           "main", "BATCH_USAGE"]

_PKG_PARENT = str(Path(__file__).resolve().parents[1])  # HostSupport/examples/python: ofxhost's parent

HOSTS = ("fresh", "per-plugin", "reuse")  # the host's lifetime (run_batch(host=...))

BATCH_USAGE = """\
Batch mode: run many hostDemo command lines in one Python (a pool of worker processes), instead of
one process per command line.
  --batch FILE|-          the command lines (one per line, POSIX shell quoting; blank lines and
                          lines starting with # are ignored), or - for stdin. A line may start with
                          --cwd DIR (its directory), --env NAME=VALUE (its environment) and
                          --timeout SECONDS (its timeout)
  -j N                    N worker processes (default 1; 0: one per CPU)
  --timeout SECONDS       kill a command's worker after SECONDS (the command is timedOut)
  --fresh-host-per-plugin keep the OFX host while the plug-in (-p) is the same (default: a new
                          host for each command, as hostDemo)
  --reuse-host            keep the OFX host while the plug-in directories, the profile and the
                          options are the same (the fastest; the plug-ins' state is shared)
  --batch-report FILE     write the records (one JSON line per command) to FILE, not stdout
Exit status: 0 if every command succeeded, 1 if one failed, 2 for a usage error, 3 for an internal
error. See ofxhost/_batch.py and the README."""


# --- the command lines ----------------------------------------------------------------------

@dataclasses.dataclass
class Command:
    """A command of a batch.

    hostDemo's arguments, the directory to run them in (None: the batch's), the environment changes (a value None
    unsets the variable), its timeout in seconds (None: the batch's); line: its number in the batch (1-based).
    """
    args: list
    cwd: str | None = None
    env: dict = dataclasses.field(default_factory=dict)
    timeout: float | None = None
    line: int | None = None


class BatchSyntaxError(ValueError):
    """A line of a batch that can't be parsed (unbalanced quotes, an option without its value)."""


class OnRecordError(RuntimeError):
    """run_batch()'s on_record raised (e.g. BrokenPipeError writing the records).

    The batch went on without calling it again. .records: all the records; __cause__: the exception.
    """

    def __init__(self, records: list):
        super().__init__("on_record raised: the batch went on without it")
        self.records = records


def _timeout_value(v, where: str) -> float:
    try:
        t = float(v)
    except (TypeError, ValueError):
        t = float("nan")
    if not 0 < t < float("inf"):
        raise BatchSyntaxError(f"{where}invalid timeout {v!r}, expected a positive number of seconds")
    return t


def parse_command(text_or_args, line: int | None = None) -> Command:
    """Parse a command of a batch.

    Args:
        text_or_args: a batch line (a string, split as a POSIX shell does) or an argument list: its
            leading --cwd DIR, --env NAME[=VALUE] and --timeout SECONDS options, then hostDemo's
            arguments.
        line: its line number, for the record and the messages.

    Returns:
        The Command.

    Raises:
        BatchSyntaxError: unbalanced quotes, an option without its value, a bad --env or --timeout.
    """
    where = f"line {line}: " if line is not None else ""
    if isinstance(text_or_args, str):
        try:
            args = shlex.split(text_or_args, comments=False, posix=True)
        except ValueError as e:
            raise BatchSyntaxError(f"{where}{e}") from None
    else:
        args = [os.fspath(a) for a in text_or_args]
    cmd = Command([], line=line)
    i = 0
    while i < len(args) and args[i] in ("--cwd", "--env", "--timeout"):
        if i + 1 >= len(args):
            raise BatchSyntaxError(f"{where}{args[i]} needs a value")
        v = args[i + 1]
        if args[i] == "--cwd":
            cmd.cwd = v
        elif args[i] == "--timeout":
            cmd.timeout = _timeout_value(v, where)
        else:
            name, eq, value = v.partition("=")
            if not name:
                raise BatchSyntaxError(f'{where}invalid --env "{v}", expected NAME=VALUE or NAME')
            cmd.env[name] = value if eq else None
        i += 2
    cmd.args = args[i:]
    return cmd


def parse_batch(text: str) -> list[Command]:
    """The commands of a batch file (parse_command() of each line; BatchSyntaxError).

    Blank lines and lines whose first non-blank character is # are ignored ("#" inside a line is literal, as in "-i
    Source=ramp#rgb").
    """
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
            if c.get("timeout") is not None:
                cmd.timeout = _timeout_value(c["timeout"], f"command {i}: ")
            out.append(cmd)
        else:
            out.append(parse_command(c, i))
    return out


# --- the worker -----------------------------------------------------------------------------

def _session():
    """The in-process backend's ofxh host (ofxhost.inprocess.Session), if it is imported and has one."""
    ip = sys.modules.get("ofxhost.inprocess")
    return ip.session() if ip is not None else None


def _shutdown_host(state: dict) -> None:
    """Shut the in-process backend's host down (if there is one), with the test hooks "unload" and "unload-crash"."""
    if _session() is None:
        return
    hook = state.pop("unload_hook", None)
    if hook:
        stdio.flush()
        os.write(1, b"batch test hook: unload\n")
        os.write(2, b"batch test hook: unload (stderr)\n")
        if hook == "unload-crash":
            os._exit(5)
        if hook == "unload-raise":
            raise RuntimeError("batch test hook: unload-raise")
    sys.modules["ofxhost.inprocess"].shutdown()


def _host_key(flags: set) -> tuple:
    """The in-process backend's host settings that a command line needs.

    Returns:
        The (dirs, profile, options) that ofxhost.hostdemo.run() would give the host, in the current
        directory and environment.
    """
    dirs = [d for d in os.environ.get("OFX_PLUGIN_PATH", "").split(os.pathsep) if d]
    dirs_t = tuple(dict.fromkeys(os.path.abspath(d) for d in dirs if os.path.isdir(d)))
    return (dirs_t, "ofx14" if "--ofx14" in flags else "default",
            (os.environ.get("OFX_HOSTDEMO_KEEP_LOADED") == "1", "--no-std-paths" not in flags,
             "--opengl-describe" in flags,
             "--no-normalised-defaults" not in flags, "--multiplane" in flags))


_hosts: list = []  # (the test hook "host": the hosts seen, kept so that their ids aren't reused)


def _test_hook(name: str, state: dict) -> None:
    """The test hook of ofxhost's tests.

    OFXHOST_BATCH_TEST_HOOK, set by a command's own --env (not inherited), makes the worker, after the command ran,
    write a line to the command's stdout and stderr, then: "segv" (a real SIGSEGV), "kill" (SIGKILL;
    TerminateProcess on Windows), "exit" (os._exit(3)), "hang" (sleep), or "raise" (an uncaught Python exception in
    the host). "host" only writes "batch test hook: host N" (N: the number of the ofxh host in the worker, 1, 2...,
    or "none"). "unload", "unload-crash" and "unload-raise" make the next shutdown of the host (in the per-plugin
    and reuse modes) write "batch test hook: unload" to the worker's stdout and stderr, then "unload-crash" exits
    the worker with status 5, and "unload-raise" raises (before the next command: its record is an error, the worker
    goes on).
    """
    if name == "host":  # (which ofxh host the command ran on: its number in this worker, or none)
        session = _session()
        if session is None:
            os.write(1, b"batch test hook: host none\n")
            return
        if not any(h is session.host for h in _hosts):
            _hosts.append(session.host)
        n = next(i for i, h in enumerate(_hosts, 1) if h is session.host)
        os.write(1, f"batch test hook: host {n}\n".encode())
        return
    if name in ("unload", "unload-crash", "unload-raise"):
        state["unload_hook"] = name
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


def _set_env(changes: Mapping) -> None:
    for k, v in changes.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v


def _prepare_host(req: dict, state: dict) -> None:
    """The host shutdown that a command needs (another plug-in, or other host options), before the command's capture.

    The unload actions' output and crashes are the worker's.
    """
    mode = req.get("host", "fresh")
    plugin, flags = scan_options(req["args"])
    need = mode == "per-plugin" and state.get("plugin") not in (None, plugin)
    session = _session()
    if mode != "fresh" and session is not None and \
            _host_key(flags) != (session.dirs, session.profile, session.options):
        need = True
    if need:
        _shutdown_host(state)
    state["plugin"] = plugin


def _run_command(req: dict, state: dict) -> int:
    """The command itself, with its test hook.

    Its exit status (1 after a bug of the Python host, with a traceback, as a separate run).
    """
    import faulthandler

    from .hostdemo import run
    if req.get("timeout"):  # (the Python threads' stacks, on the command's stderr, before the kill)
        faulthandler.dump_traceback_later(req["timeout"], exit=False)
    try:
        status = run(req["args"], "python -m ofxhost.hostdemo", shutdown=req.get("host", "fresh") == "fresh")
        hook = req.get("env", {}).get("OFXHOST_BATCH_TEST_HOOK")  # (the command's own --env only)
        if hook:
            _test_hook(hook, state)
        return status
    except Exception:  # (a bug of the Python host: as a separate run, a traceback and status 1)
        sys.stdout.flush()
        traceback.print_exc()
        try:
            _shutdown_host(state)  # (its state is unknown)
        except Exception:
            traceback.print_exc()
        return 1
    finally:
        faulthandler.cancel_dump_traceback_later()


def _run_one(req: dict, state: dict, send: Callable[[dict], None]) -> dict:
    """Run a request in the worker.

    Its environment and directory, the host shutdown that it needs (on the worker's own fds), "started", then the
    command with fds 1 and 2 on its capture files. The reply.
    """
    env = req.get("env", {})
    saved_env = {k: os.environ.get(k) for k in env}
    t0 = time.perf_counter()
    reply: dict = {"done": req["id"], "exitStatus": 1, "seconds": 0.0}
    files: dict = {}
    try:
        try:
            _set_env(env)
            os.chdir(req["cwd"])
        except (OSError, ValueError) as e:
            send({"started": req["id"]})
            reply["error"] = f"Error: cannot run the command in {req['cwd']} with --env {env}: {e}"
            return reply
        _prepare_host(req, state)
        send({"started": req["id"]})
        t0 = time.perf_counter()
        try:
            for fd, key in ((1, "stdout"), (2, "stderr")):
                files[fd] = open(req[key], "wb")
        except OSError as e:
            reply["error"] = f"Error: cannot open the capture file: {e}"
            return reply
        with stdio.redirect({fd: f.fileno() for fd, f in files.items()}):
            reply["exitStatus"] = _run_command(req, state)
    finally:
        stdio.flush()
        for f in files.values():
            f.close()
        _set_env(saved_env)
        os.chdir(state["cwd"])
        reply["seconds"] = time.perf_counter() - t0
    return reply


def _watch_parent() -> None:
    """End the worker if the batch dies without killing it (e.g. SIGKILL).

    The workers have their own process group (POSIX), so the signals of the batch's group don't reach them. Linux:
    PR_SET_PDEATHSIG (SIGKILL when the thread of the batch that started the worker ends: the slot's thread, which
    waits for its worker before it ends); everywhere on POSIX, a thread that exits when the parent changes (the
    worker is reparented). Windows: closing the console ends every process attached to it, workers included; a batch
    killed by TerminateProcess leaves its workers to end at their next read of a request (end of file), after the
    command that runs.
    """
    if os.name != "posix":
        return
    parent = os.getppid()
    if sys.platform.startswith("linux"):
        try:
            libc = ctypes.CDLL(None, use_errno=True)
            libc.prctl(1, 9, 0, 0, 0)  # (PR_SET_PDEATHSIG, SIGKILL)
        except (OSError, AttributeError):
            pass
        if os.getppid() != parent:  # (the parent died before prctl)
            os._exit(1)

    def watch():
        while os.getppid() == parent:
            time.sleep(0.5)
        os._exit(1)

    threading.Thread(target=watch, daemon=True, name="ofxhost-batch-parent").start()


def worker_main() -> int:
    """A worker: requests on stdin; "ready", "started" and the replies on stdout, one JSON line each.

    Between the commands, fds 1 and 2 go to the worker's stderr, and fd 0 is null.
    """
    import faulthandler
    rd = os.fdopen(os.dup(0), "r", encoding="utf-8")
    wr = os.fdopen(os.dup(1), "w", encoding="utf-8")
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    os.dup2(2, 1)
    faulthandler.enable()  # (on fd 2: a crash's Python traceback goes to the command's stderr)

    def send(msg: dict) -> None:
        if "started" in msg:
            state["started"] = msg["started"]
        wr.write(json.dumps(msg) + "\n")
        wr.flush()

    _watch_parent()
    from . import hostdemo  # noqa: F401  (and NumPy: the imports before "ready"; ofxh is imported by
    # the first command, since its $OFX_PLUGIN_PATH chooses the module's build tree)
    state: dict = {"cwd": os.getcwd(), "plugin": None, "started": None}
    send({"ready": os.getpid()})
    for line in rd:
        try:
            req = json.loads(line)
        except ValueError:
            continue
        try:
            reply = _run_one(req, state, send)
        except Exception:  # (a bug of the worker, or an error in a host shutdown before the command:
            # an error reply, after "started" if it wasn't sent, and the worker goes on)
            reply = {"done": req.get("id"), "exitStatus": 1, "seconds": 0.0,
                     "error": "Error: internal error in the batch worker:\n" + traceback.format_exc()}
            if state["started"] != req.get("id"):
                send({"started": req.get("id")})
        send(reply)
    stdio.flush()
    _shutdown_host(state)  # (the plug-ins' unload actions: their output goes to the worker's stderr)
    stdio.flush()
    return 0


# --- the pool -------------------------------------------------------------------------------

_WORKER_CODE = ("import os, sys; sys.path[:] = [p for p in sys.path if p not in ('', os.getcwd())]; "
                "sys.path.insert(0, {!r}); from ofxhost._batch import worker_main; sys.exit(worker_main())")
_TIMEOUT = "timeout"


class _Worker:
    def __init__(self, python: str, env: dict, tmp: str, slot: int, gen: int):
        self.slot, self.gen, self.seq = slot, gen, 0
        self.log_path = os.path.join(tmp, f"worker{slot}.{gen}.log")
        self.log = open(self.log_path, "ab")
        kw: dict = {"start_new_session": True} if os.name == "posix" else \
            {"creationflags": getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)}
        try:
            self.proc = subprocess.Popen([python, "-c", _WORKER_CODE.format(_PKG_PARENT)], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=self.log, env=env, cwd=tmp, text=True,
                                         encoding="utf-8", bufsize=1, **kw)
        except OSError:
            self.log.close()
            raise
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.stdin, self.stdout = self.proc.stdin, self.proc.stdout
        self.replies: queue.Queue = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            for line in self.stdout:
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue  # (not ours: e.g. a sitecustomize that prints before "ready")
                if isinstance(msg, dict):
                    self.replies.put(msg)
        except (OSError, ValueError):
            pass
        self.replies.put(None)  # (EOF: the worker is gone)

    def wait(self, kind: str, timeout: float | None, also: str | None = None):
        """The worker's next message of this kind ("ready", "started", "done"), or of the kind also.

        Returns:
            The message, None if the worker is gone, _TIMEOUT after timeout seconds.
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            try:
                msg = self.replies.get(timeout=None if deadline is None else max(0., deadline - time.monotonic()))
            except queue.Empty:
                return _TIMEOUT
            if msg is None:
                self.replies.put(None)  # (for the next wait)
                return None
            if kind in msg or (also is not None and also in msg):
                return msg

    def send(self, req: dict) -> bool:
        try:
            self.stdin.write(json.dumps(req) + "\n")
            self.stdin.flush()
            return True
        except OSError:
            return False

    def kill(self):
        """Kill the worker and its process group (POSIX) or tree (Windows), then wait for it."""
        if os.name == "posix":  # (also after the worker died: the helpers it started in its group)
            import signal
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        elif self.proc.poll() is None and sys.platform == "win32":
            taskkill = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "taskkill.exe")
            try:
                subprocess.run([taskkill, "/F", "/T", "/PID", str(self.proc.pid)], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, check=False, timeout=30)
            except (OSError, subprocess.SubprocessError):
                pass
        if self.proc.poll() is None:
            try:
                self.proc.kill()
            except OSError:
                pass
        self.proc.wait()
        self.close_files()

    def close_files(self):
        for f in (self.stdin, self.stdout):
            try:
                f.close()
            except OSError:
                pass
        self.log.close()

    def log_text(self) -> str:
        return _read_text(self.log_path)


def _read_text(path: str) -> str:
    try:
        return Path(path).read_bytes().decode("utf-8", "replace")
    except OSError:
        return ""


def _worker_env(env: Mapping | None) -> dict:
    e = dict(os.environ if env is None else env)
    # (the ofxh module of this process, if it imported one: the workers use the same)
    ofxh = sys.modules.get("ofxh")
    path = getattr(ofxh, "__file__", None) if ofxh is not None else None
    if path:
        pp = e.get("PYTHONPATH")
        e["PYTHONPATH"] = str(Path(path).resolve().parent) + (os.pathsep + pp if pp else "")
    return e


def _record(c: Command, cwd: str, **more) -> dict:
    rec = {"line": c.line, "args": list(c.args), "cwd": cwd, "env": dict(c.env), "exitStatus": 1, "ok": False,
           "stdout": "", "stderr": "", "seconds": 0.0, "crashed": False, "timedOut": False, "worker": None,
           "seq": None}
    rec.update(more)
    return rec


class _Pool:
    """A batch's worker slots (one thread each), and its records."""

    def __init__(self, cmds: list[Command], jobs: int, timeout: float | None, host: str, base: str, env: dict,
                 python: str, tmp: str, on_record: Callable[[dict], None] | None):
        self.cmds, self.jobs, self.timeout, self.host = cmds, jobs, timeout, host
        self.base, self.env, self.python, self.tmp, self.on_record = base, env, python, tmp, on_record
        self.records: list = [None] * len(cmds)
        self.events: list = []
        self.lock = threading.Lock()  # (the records, the next command, the workers)
        self.emit_lock = threading.Lock()  # (on_record, in order, outside of lock)
        self.next = 0  # the next command to run
        self.emitted = 0  # the records given to on_record
        self.error: Exception | None = None  # what on_record raised
        self.stop = False  # interrupted: no new worker
        self.live: set = set()

    def run(self) -> list:
        """Run the commands: the records, then those of the worker events."""
        threads = [threading.Thread(target=self.slot, args=(k,), daemon=True) for k in range(self.jobs)]
        try:
            for t in threads:
                t.start()
            for t in threads:
                while t.is_alive():
                    t.join(0.2)
        except BaseException:  # (an interrupt: no new command, no more records, and the workers are killed)
            with self.lock:
                self.next = len(self.cmds)
                self.stop = True
                workers = list(self.live)
            for w in workers:
                w.kill()
            for t in threads:  # (the slots end once their workers are gone: none is left writing at exit)
                t.join(10.)
            raise
        for i, c in enumerate(self.cmds):  # (none should be left: a bug of the batch otherwise)
            if self.records[i] is None:
                self.store(i, _record(c, self.base, stderr="Error: internal error in the batch: the command didn't "
                                      "run\n", internalError="the command has no record"))
        for r in self.events:
            self.call(r)
        return self.records + self.events

    def call(self, rec: dict) -> None:
        """on_record, until it raises."""
        if self.on_record is not None and self.error is None and not self.stop:
            try:
                self.on_record(rec)
            except Exception as e:
                self.error = e

    def store(self, i: int, rec: dict) -> None:
        """The record of command i; on_record for the records that are done, in order."""
        with self.lock:
            self.records[i] = rec
        with self.emit_lock:
            while True:
                with self.lock:
                    k = self.emitted
                    if k >= len(self.cmds) or self.records[k] is None:
                        break
                    self.emitted = k + 1
                self.call(self.records[k])

    def event(self, kind: str, w: _Worker | None, status, stderr: str, **more) -> None:
        rec = {"line": None, "event": kind, "args": [], "cwd": self.base, "env": {}, "exitStatus": status,
               "ok": False, "stdout": "", "stderr": stderr, "seconds": 0.0, "crashed": kind != "start",
               "timedOut": False, "worker": [w.slot, w.gen] if w is not None else None,
               "seq": w.seq if w is not None else None}
        rec.update(more)
        with self.lock:
            self.events.append(rec)

    def retire(self, w: _Worker) -> None:
        w.kill()
        with self.lock:
            self.live.discard(w)

    def start(self, k: int, gen: int, t: float | None) -> _Worker | None:
        """A new worker in slot k, ready (None after a "start" event)."""
        try:
            w = _Worker(self.python, self.env, self.tmp, k, gen)
        except OSError as e:
            self.event("start", None, None, f"Error: cannot start a worker ({self.python}): {e}\n")
            return None
        with self.lock:
            self.live.add(w)
        ready = w.wait("ready", None if t is None else max(60., t))
        if not isinstance(ready, dict):
            self.retire(w)
            self.event("start", w, w.proc.returncode, w.log_text(), timedOut=ready == _TIMEOUT)
            return None
        return w

    def slot(self, k: int) -> None:
        """Slot k's thread: the next command, until there is none, then its worker's exit."""
        cur: dict = {"w": None, "gen": 0}
        try:
            while True:
                with self.lock:
                    i = self.next
                    if i >= len(self.cmds):
                        break
                    self.next = i + 1
                c = self.cmds[i]
                try:
                    rec = self.command(k, i, c, cur)
                except Exception:  # (a bug of the batch: an internal-error record, and a new worker)
                    rec = _record(c, self.base, stderr="Error: internal error in the batch\n",
                                  internalError=traceback.format_exc())
                    if cur["w"] is not None:
                        self.retire(cur["w"])
                        cur["w"] = None
                self.store(i, rec)
        finally:
            if cur["w"] is not None:
                self.close(cur["w"])

    def close(self, w: _Worker) -> None:
        """The end: the worker shuts its host down and exits (an "exit" event if it fails)."""
        try:
            w.stdin.close()
        except OSError:
            pass
        try:
            status, hung = w.proc.wait(timeout=max(60., self.timeout or 0.)), False
        except subprocess.TimeoutExpired:
            w.kill()
            status, hung = w.proc.returncode, True
        w.close_files()
        with self.lock:
            self.live.discard(w)
        if status != 0:
            self.event("exit", w, status, w.log_text(), timedOut=hung, crashed=not hung)

    def command(self, k: int, i: int, c: Command, cur: dict) -> dict:
        """Run a command in the slot's worker cur["w"] (a new one, cur["gen"] + 1, if None): its record."""
        rec = _record(c, os.path.join(self.base, c.cwd) if c.cwd else self.base)
        if is_batch(c.args):
            rec["stderr"] = "Error: --batch inside a batch\n"
            return rec
        if not os.path.isdir(rec["cwd"]):
            rec["stderr"] = f"Error: no directory {rec['cwd']} (--cwd)\n"
            return rec
        t = c.timeout if c.timeout is not None else self.timeout
        out, err = os.path.join(self.tmp, f"{i}.out"), os.path.join(self.tmp, f"{i}.err")
        req = {"id": i, "args": rec["args"], "cwd": rec["cwd"], "env": c.env, "stdout": out, "stderr": err,
               "host": self.host, "timeout": t}
        for _attempt in range(2):
            if self.stop:  # (interrupted: no new worker)
                break
            if cur["w"] is None:
                cur["gen"] += 1
                cur["w"] = self.start(k, cur["gen"], t)
                if cur["w"] is None:
                    continue
            started = self.send(cur, req, t)
            if started is not None:
                self.finish(rec, cur, started, t, out, err)
                return rec
        rec["crashed"] = True
        rec["stderr"] = "Error: no worker could run the command (see the worker events)\n"
        return rec

    def send(self, cur: dict, req: dict, t: float | None) -> dict | None:
        """Send the request to the worker, and wait until the command starts.

        The worker's message, or None after a "died" event (the worker died, or hung, before the command: not its
        fault).
        """
        w = cur["w"]
        w.seq += 1
        started = w.wait("started", t, also="done") if w.send(req) else None
        if isinstance(started, dict):
            return started
        cur["w"] = None
        self.retire(w)
        w.seq -= 1  # (the event's seq: the last command that the worker ran)
        self.event("died", w, w.proc.returncode, w.log_text(), timedOut=started == _TIMEOUT,
                   crashed=started != _TIMEOUT)
        return None

    def finish(self, rec: dict, cur: dict, started: dict, t: float | None, out: str, err: str) -> None:
        """Wait for the command's end, or kill its worker (a timeout, a crash), into its record."""
        w = cur["w"]
        t0 = time.perf_counter()
        done = started if "done" in started else w.wait("done", t)
        if done == _TIMEOUT:
            rec["timedOut"] = True
            w.wait("done", 2.)  # (the time for the worker's stack dump to be written)
        rec.update(worker=[w.slot, w.gen], seq=w.seq)
        if isinstance(done, dict):
            rec["exitStatus"], rec["seconds"] = done["exitStatus"], done["seconds"]
        else:  # (timed out, or the worker died: kill it first, then read what it wrote)
            cur["w"] = None
            self.retire(w)
            rec["exitStatus"], rec["seconds"] = w.proc.returncode, time.perf_counter() - t0
            rec["crashed"] = not rec["timedOut"]
            log = w.log_text()
            if log:
                rec["workerStderr"] = log
        rec["ok"] = isinstance(done, dict) and done["exitStatus"] == 0 and "error" not in done
        rec["stdout"], rec["stderr"] = _read_text(out), _read_text(err)
        if isinstance(done, dict) and done.get("error"):
            rec["stderr"] += done["error"].rstrip("\n") + "\n"
        for p in (out, err):
            try:
                os.unlink(p)
            except OSError:
                pass


def run_batch(commands: Iterable, jobs: int = 1, timeout: float | None = None, host: str = "fresh",
              cwd: str | os.PathLike | None = None, env: Mapping | None = None, python: str | None = None,
              on_record: Callable[[dict], None] | None = None) -> list[dict]:
    """Run hostDemo command lines in a pool of worker processes (see README.md for the records).

    Args:
        commands: batch lines (strings, POSIX shell quoting, with the optional leading --cwd, --env
            and --timeout), argument lists, Command objects, or dicts {"args", "cwd", "env", "timeout"}.
        jobs: the number of workers (0: one per CPU).
        timeout: seconds per command (its worker is killed after that), unless the command has its own.
        host: "fresh" (a new host for each command, the default), "per-plugin" (shut it down when
            the plug-in changes) or "reuse" (keep it while the plug-in directories, the profile and
            the options are the same).
        cwd: the batch's directory (default: the current one), where commands without --cwd run and
            relative --cwd start.
        env: the workers' environment (default: os.environ).
        python: the workers' Python (default: sys.executable).
        on_record: called with each record, in order, as soon as it and those before are done; if it
            raises, it isn't called again, the batch goes on, and run_batch() raises OnRecordError.

    Returns:
        The records (dicts), in the order of the commands, then those of the worker events.

    Raises:
        OnRecordError: on_record raised (.records: the records).
    """
    cmds = _commands(commands)
    if host not in HOSTS:
        raise ValueError(f"host must be one of {', '.join(HOSTS)}, not {host!r}")
    if timeout is not None:
        timeout = _timeout_value(timeout, "")
    jobs = int(jobs)
    if jobs < 0:
        raise ValueError(f"jobs must be >= 0, not {jobs}")
    jobs = max(1, min(jobs or (os.cpu_count() or 1), len(cmds)))
    base = os.path.abspath(os.fspath(cwd) if cwd is not None else os.getcwd())
    tmp = tempfile.mkdtemp(prefix="ofxhost-batch-")
    try:
        pool = _Pool(cmds, jobs, timeout, host, base, _worker_env(env), python or sys.executable, tmp, on_record)
        out = pool.run()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if pool.error is not None:
        raise OnRecordError(out) from pool.error
    return out


def main(argv: Sequence[str]) -> int:
    """python -m ofxhost.hostdemo --batch ...: argv without the program name."""
    p = argparse.ArgumentParser(prog="python -m ofxhost.hostdemo --batch", description=BATCH_USAGE, allow_abbrev=False,
                                formatter_class=argparse.RawDescriptionHelpFormatter, usage="%(prog)s FILE|- "
                                "[-j N] [--timeout SECONDS] [--reuse-host | --fresh-host-per-plugin] "
                                "[--batch-report FILE]")
    p.add_argument("--batch", required=True, metavar="FILE|-")
    p.add_argument("-j", type=int, default=1, metavar="N", dest="jobs")
    p.add_argument("--timeout", type=float, default=None, metavar="SECONDS")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--reuse-host", action="store_true")
    g.add_argument("--fresh-host-per-plugin", action="store_true")
    p.add_argument("--batch-report", metavar="FILE")
    a = p.parse_args(list(argv))
    if a.jobs < 0:
        p.error(f"-j: invalid number of workers {a.jobs}")
    if a.timeout is not None and not 0 < a.timeout < float("inf"):
        p.error(f"--timeout: expected a positive number of seconds, not {a.timeout}")
    try:
        text = sys.stdin.read() if a.batch == "-" else Path(a.batch).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        p.error(f"cannot read {a.batch}: {e}")
    try:
        cmds = parse_batch(text)
    except BatchSyntaxError as e:
        p.error(f"{a.batch}: {e}")
    try:
        out = open(a.batch_report, "w", encoding="utf-8") if a.batch_report else sys.stdout
    except OSError as e:
        p.error(f"cannot write {a.batch_report}: {e}")

    def write(rec):
        out.write(json.dumps(rec) + "\n")
        out.flush()

    def stop(signum, frame):  # (SIGTERM, SIGHUP: run_batch kills the workers, which have their own group)
        raise SystemExit(128 + signum)

    import signal
    for name in ("SIGTERM", "SIGHUP"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), stop)
    t0 = time.perf_counter()
    try:
        recs = run_batch(cmds, jobs=a.jobs, timeout=a.timeout,
                         host="reuse" if a.reuse_host else "per-plugin" if a.fresh_host_per_plugin else "fresh",
                         on_record=write)
    except OnRecordError as e:
        if isinstance(e.__cause__, BrokenPipeError):  # (e.g. | head: no "Exception ignored" at exit)
            null = os.open(os.devnull, os.O_WRONLY)
            os.dup2(null, out.fileno())
            os.close(null)
        sys.stderr.write(f"batch: internal error: cannot write the records: {e.__cause__!r}\n")
        return 3
    except Exception:
        sys.stderr.write("batch: internal error:\n" + traceback.format_exc())
        return 3
    finally:
        if out is not sys.stdout:
            out.close()
    cmd_recs = [r for r in recs if r.get("line") is not None]
    failed = sum(not r["ok"] for r in cmd_recs)
    crashed = sum(bool(r["crashed"]) for r in cmd_recs)
    timed = sum(bool(r["timedOut"]) for r in cmd_recs)
    events = len(recs) - len(cmd_recs)
    internal = sum("internalError" in r for r in cmd_recs)
    sys.stderr.write(f"batch: {len(cmds)} commands, {failed} failed ({crashed} crashed, {timed} timed out)"
                     + (f", {events} worker events" if events else "")
                     + (f", {internal} internal errors" if internal else "")
                     + f", {time.perf_counter() - t0:.2f} s\n")
    return 3 if internal else 1 if failed or events else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
