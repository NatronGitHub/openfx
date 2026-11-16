"""ofxhost: render OFX plug-ins from Python, with the HostSupport demo host (hostDemo).

Each render runs hostDemo in its own process and temporary directory (HostSupport has
process-wide singletons, plug-ins can't be unloaded, and a crashing plug-in must not take
down the interpreter). Images are exchanged as PFM files.

Image contract: NumPy arrays are float32, H x W x C, top-down (row 0 is the top row).
OFX images and PFM files are bottom-up; the wrapper flips them.

    import ofxhost, numpy as np
    r = ofxhost.render("Support/Plugins/Invert/Linux-64-release", "net.sf.openfx.invertPlugin",
                       inputs={"Source": src}, depth="float")
    out = r.outputs[0]          # H x W x 4 (RGBA), top-down
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import numpy as np

from .pfm import encode_pfm, parse_pfm, read_pfm, write_pfm

__all__ = [
    "render", "describe", "RenderResult", "HostDemoError", "HostDemoNotFound",
    "find_hostdemo",
    "read_pfm", "write_pfm", "parse_pfm", "encode_pfm",
]

OUTPUT_PREFIX = "Output"
DEPTHS = ("byte", "short", "float")


class HostDemoError(RuntimeError):
    """hostDemo failed (non-zero exit, timeout, or unreadable output)."""

    def __init__(self, message: str, *, returncode: int | None = None,
                 stdout: str = "", stderr: str = "", cmd: Sequence[str] = ()):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.cmd = list(cmd)
        full = message
        if self.cmd:
            full += "\ncommand: " + " ".join(self.cmd)
        if stderr.strip():
            full += "\nstderr:\n" + stderr.rstrip()
        if stdout.strip():
            full += "\nstdout:\n" + stdout.rstrip()
        super().__init__(full)


class HostDemoNotFound(FileNotFoundError):
    """The hostDemo binary wasn't found (build it with make -C HostSupport/examples)."""


@dataclasses.dataclass
class RenderResult:
    """The result of a hostDemo run.

    outputs: frame -> float32 array, H x W x C, top-down. C is the number of components of
        the output image (4 for RGBA: the separate alpha file is merged in, 3 for RGB,
        1 for Alpha). With stereo views (Nuke/Vegas extension builds), the key is
        (frame, "l") or (frame, "r").
    alpha: frame -> H x W alpha, for RGBA outputs (the separate .alpha.pfm file) and Alpha
        outputs.
    report: the --report JSON (renders[i].output has the size, min, max, mean, nan count
        and crc32 of each output image).
    """
    outputs: dict
    alpha: dict
    report: dict
    returncode: int
    stdout: str
    stderr: str
    cmd: list

    @property
    def output(self) -> np.ndarray:
        """The output image of the first rendered frame."""
        if not self.outputs:
            raise HostDemoError("hostDemo wrote no output image",
                                returncode=self.returncode, stdout=self.stdout,
                                stderr=self.stderr, cmd=self.cmd)
        return next(iter(self.outputs.values()))


# --- locating hostDemo -----------------------------------------------------------------

_EXAMPLES_DIR = Path(__file__).resolve().parents[2]  # HostSupport/examples
_REPO_ROOT = _EXAMPLES_DIR.parents[1]
_EXE = "hostDemo.exe" if sys.platform == "win32" else "hostDemo"


def _build_tree_candidates() -> list[Path]:
    found: list[Path] = []
    # Makefile builds: HostSupport/examples/<OS>-release (then relwithdebinfo, debug)
    for config in ("release", "relwithdebinfo", "debug"):
        found += sorted(_EXAMPLES_DIR.glob(f"*-{config}/{_EXE}"))
    # CMake builds: <root>/build*/HostSupport/examples/hostDemo (possibly with a config dir)
    for pattern in (f"build*/HostSupport/examples/{_EXE}",
                    f"build*/HostSupport/examples/*/{_EXE}",
                    f"build*/bin/{_EXE}"):
        found += sorted(_REPO_ROOT.glob(pattern))
    return [p for p in found if p.is_file() and os.access(p, os.X_OK)]


def find_hostdemo(host_binary: str | os.PathLike | None = None) -> Path:
    """Locate hostDemo: the explicit argument, $OFXHOST_HOSTDEMO, then the build tree."""
    for source, value in (("host_binary", host_binary),
                          ("OFXHOST_HOSTDEMO", os.environ.get("OFXHOST_HOSTDEMO"))):
        if value:
            p = Path(value)
            if not p.is_file():
                which = shutil.which(str(value))
                if which is None:
                    raise HostDemoNotFound(f"{source}={value}: no such file")
                p = Path(which)
            return p.resolve()
    candidates = _build_tree_candidates()
    if not candidates:
        raise HostDemoNotFound(
            "hostDemo not found: build it with `make -C HostSupport && make -C "
            "HostSupport/examples`, or set OFXHOST_HOSTDEMO")
    return candidates[0].resolve()


# --- running hostDemo ------------------------------------------------------------------

def _plugin_dirs(bundle_paths) -> list[str]:
    """OFX_PLUGIN_PATH entries: directories as given, *.bundle paths as their parent."""
    if isinstance(bundle_paths, (str, os.PathLike)):
        bundle_paths = [bundle_paths]
    dirs: list[str] = []
    for b in bundle_paths:
        p = Path(b).resolve()
        if p.suffix == ".bundle":
            p = p.parent
        if not p.is_dir():
            raise FileNotFoundError(f"plug-in path {b}: no such directory")
        if str(p) not in dirs:
            dirs.append(str(p))
    return dirs


def _format_value(v: Any) -> str:
    if isinstance(v, str):
        return v
    if isinstance(v, (bool, np.bool_)):
        return "1" if v else "0"
    if isinstance(v, (int, np.integer)):
        return str(int(v))
    if isinstance(v, (float, np.floating)):
        f = float(v)
        if not np.isfinite(f):
            raise ValueError(f"parameter value {v!r} is not finite")
        return repr(f)
    if isinstance(v, (Sequence, np.ndarray)):
        return ",".join(_format_value(x) for x in v)
    raise TypeError(f"unsupported parameter value {v!r}")


def _input_spec(clip: str, value: Any, workdir: Path, index: int) -> str:
    if isinstance(value, os.PathLike):
        return "file:" + str(Path(value).resolve())
    if isinstance(value, str):
        if value.startswith("file:"):
            return "file:" + str(Path(value[5:]).resolve())
        return value
    a = np.asarray(value, dtype=np.float32)
    if a.ndim == 3 and a.shape[2] == 4:
        # hostDemo's PFM input gives (r,g,b,1): it can't carry an alpha channel
        if not np.all(a[:, :, 3] == 1.0):
            raise ValueError(f"input {clip}: RGBA arrays must have alpha == 1 (hostDemo's "
                             "PFM input is RGB with alpha 1, or grey with r=g=b=a)")
        a = a[:, :, :3]
    if not (a.ndim == 2 or (a.ndim == 3 and a.shape[2] in (1, 3))):
        raise ValueError(f"input {clip}: shape {a.shape} (expected H x W, H x W x 1, "
                         "H x W x 3 or H x W x 4)")
    if not np.all(np.isfinite(a)):
        raise ValueError(f"input {clip}: non-finite values")
    path = workdir / f"input{index}.pfm"
    write_pfm(path, a)
    return "file:" + str(path)


_OUTPUT_RE = re.compile(re.escape(OUTPUT_PREFIX) + r"\.(-?\d+(?:\.\d+)?)([lr]?)(\.alpha)?\.pfm$")


def _read_outputs(workdir: Path) -> tuple[dict, dict]:
    colour: dict = {}
    alpha: dict = {}
    for p in sorted(workdir.iterdir()):
        m = _OUTPUT_RE.match(p.name)
        if not m:
            continue
        v = float(m.group(1))
        key = int(v) if v.is_integer() else v
        if m.group(2):
            key = (key, m.group(2))
        (alpha if m.group(3) else colour)[key] = read_pfm(p)
    outputs: dict = {}
    alphas: dict = {}
    for key in sorted(colour, key=lambda k: k if isinstance(k, tuple) else (k, "")):
        img = colour[key]
        if img.ndim == 2:  # Pf: an Alpha image
            outputs[key] = img[:, :, None]
            alphas[key] = img
        elif key in alpha:
            outputs[key] = np.concatenate([img, alpha[key][:, :, None]], axis=2)
            alphas[key] = alpha[key]
        else:
            outputs[key] = img
    return outputs, alphas


def _run(cmd: list[str], env: dict, cwd: Path, timeout: float | None):
    try:
        return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                              errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        raise HostDemoError(f"hostDemo timed out after {timeout} s", stdout=out, stderr=err,
                            cmd=cmd) from None


def _env(bundle_paths) -> dict:
    env = dict(os.environ)
    env["OFX_PLUGIN_PATH"] = os.pathsep.join(_plugin_dirs(bundle_paths))
    return env


def render(bundle_paths, plugin_id: str, context: str = "filter",
           inputs: Mapping[str, Any] | None = None,
           params: Mapping[str, Any] | None = None,
           depth: str = "float",
           host_binary: str | os.PathLike | None = None, timeout: float | None = 60,
           extra_args: Sequence[str] = (), check: bool = True) -> RenderResult:
    """Render a plug-in with hostDemo at frames 0 and 1; return its output images and report.

    bundle_paths: a directory containing .ofx.bundle directories, a bundle path (its parent
        directory is used), or a list of these. They become OFX_PLUGIN_PATH; the standard
        plug-in locations and the plug-in cache are not used.
    inputs: clip name -> a NumPy array (H x W, H x W x 1, H x W x 3, or H x W x 4 with
        alpha 1; top-down; written to a PFM file), a pathlib.Path or "file:path.pfm", or a
        hostDemo spec string: "none" (disconnected), "default", "solid:r,g,b,a", "ramp",
        "checker:N".
    params: parameter name -> value (number, bool, string, or a sequence for
        multi-dimensional parameters). Passed as -s name=value.
    depth: the input clips' depth before clip preferences: "byte", "short" or "float".
    check: raise HostDemoError on a non-zero exit status (default).
    """
    if depth not in DEPTHS:
        raise ValueError(f"depth {depth!r}: expected one of {DEPTHS}")
    host = find_hostdemo(host_binary)
    env = _env(bundle_paths)
    with tempfile.TemporaryDirectory(prefix="ofxhost-") as tmp:
        workdir = Path(tmp)
        report_path = workdir / "report.json"
        cmd = [str(host), "--no-std-paths", "--no-cache", "--quiet", "--format", "pfm",
               "--report", str(report_path),
               "-p", plugin_id, "-c", context, "-d", depth, "-o", OUTPUT_PREFIX]
        for i, (clip, value) in enumerate((inputs or {}).items()):
            cmd += ["-i", f"{clip}={_input_spec(clip, value, workdir, i)}"]
        cmd += list(extra_args)
        for name, value in (params or {}).items():
            cmd += ["-s", f"{name}={_format_value(value)}"]

        p = _run(cmd, env, workdir, timeout)
        if check and p.returncode != 0:
            raise HostDemoError(f"hostDemo failed with exit status {p.returncode}",
                                returncode=p.returncode, stdout=p.stdout, stderr=p.stderr,
                                cmd=cmd)
        try:
            with open(report_path, encoding="utf-8") as f:
                report = json.load(f)
            outputs, alpha = _read_outputs(workdir)
        except (ValueError, OSError) as e:
            raise HostDemoError(f"can't read hostDemo's output: {e}", returncode=p.returncode,
                                stdout=p.stdout, stderr=p.stderr, cmd=cmd) from None

    return RenderResult(outputs=outputs, alpha=alpha, report=report,
                        returncode=p.returncode, stdout=p.stdout, stderr=p.stderr, cmd=cmd)


def describe(bundle_paths, plugin_id: str, context: str | None = None,
             host_binary: str | os.PathLike | None = None, timeout: float | None = 60,
             extra_args: Sequence[str] = ()) -> dict:
    """Return hostDemo's --describe JSON for a plug-in (params, clips, contexts, flags)."""
    host = find_hostdemo(host_binary)
    env = _env(bundle_paths)
    with tempfile.TemporaryDirectory(prefix="ofxhost-") as tmp:
        workdir = Path(tmp)
        out = workdir / "describe.json"
        cmd = [str(host), "--no-std-paths", "--no-cache", "--quiet", "-p", plugin_id]
        if context:
            cmd += ["-c", context]
        cmd += ["--describe", str(out), *extra_args]
        p = _run(cmd, env, workdir, timeout)
        if p.returncode != 0 or not out.is_file():
            raise HostDemoError(f"hostDemo --describe failed with exit status {p.returncode}",
                                returncode=p.returncode, stdout=p.stdout, stderr=p.stderr,
                                cmd=cmd)
        with open(out, encoding="utf-8") as f:
            return json.load(f)
