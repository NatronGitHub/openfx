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
    "render", "describe", "RenderResult", "Image", "HostDemoError", "HostDemoNotFound",
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
class Image:
    """An image with its position: what a render outputs, and what an input can be.

    pixels: float32, H x W x C (C = 1, 3 or 4), top-down.
    bounds: (x1, y1, x2, y2), in pixels at the render scale (OFX pixel coordinates, y up).
    premult: the OFX premultiplication state (kOfxImagePreMultiplied...), or None for the
        default (unpremultiplied, opaque for RGB).
    """
    pixels: np.ndarray
    bounds: tuple = (0, 0, 0, 0)
    premult: str | None = None


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
    images: the same keys as outputs -> Image, with the bounds and the output
        premultiplication: pass one as an input of another render at the same scale.
    """
    outputs: dict
    alpha: dict
    report: dict
    returncode: int
    stdout: str
    stderr: str
    cmd: list
    images: dict = dataclasses.field(default_factory=dict)

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


def _param_args(name: str, value: Any) -> list[str]:
    """The hostDemo -s arguments of a parameter value, keys ({time: value}) or a button press (None)."""
    if value is None:
        return ["-s", name]
    if isinstance(value, Mapping):
        if not value:
            raise ValueError(f"parameter {name!r}: no keys")
        return [a for t, v in sorted(value.items(), key=lambda kv: float(kv[0]))
                for a in ("-s", f"{name}@{_format_value(float(t))}={_format_value(v)}")]
    return ["-s", f"{name}={_format_value(value)}"]


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


_PREMULT = {"OfxImageAlphaPremultiplied": "premult", "OfxImageAlphaUnPremultiplied": "unpremult",
            "OfxImageOpaque": "opaque"}


# as hostDemo parses it (hostDemoClipInstance.cpp): "#premult|#unpremult|#opaque" at the end,
# then "@x1,y1", then the alpha file after the last ".pfm+"
_FILE_SPEC = re.compile(r"(?P<rest>.*?)(?P<pos>@-?\d+,-?\d+)?(?P<premult>#(?:premult|unpremult|opaque))?$")


def resolve_file_spec(spec: str, base: str | os.PathLike | None = None) -> str:
    """Make the paths of a hostDemo "file:rgb.pfm[+alpha.pfm][@x1,y1][#premult]" input spec
    absolute (relative to base, default the current directory), keeping the suffixes."""
    body = spec[5:] if spec.startswith("file:") else spec
    base = Path(base) if base is not None else Path.cwd()
    m = _FILE_SPEC.match(body)
    rest = m["rest"]
    rgb, sep, alpha = rest.rpartition(".pfm+")
    if sep:
        out = "file:" + str((base / (rgb + ".pfm")).resolve()) + "+" + str((base / alpha).resolve())
    else:
        out = "file:" + str((base / rest).resolve())
    return out + (m["pos"] or "") + (m["premult"] or "")


def _input_spec(clip: str, value: Any, workdir: Path, index: int) -> str:
    if isinstance(value, os.PathLike):
        return "file:" + str(Path(value).resolve())
    if isinstance(value, str):
        if value.startswith("file:"):
            return resolve_file_spec(value)
        return value
    bounds, premult = None, None
    if isinstance(value, Image):
        bounds, premult, value = value.bounds, value.premult, value.pixels
    a = np.asarray(value, dtype=np.float32)
    if a.ndim == 3 and a.shape[2] == 1:
        a = a[:, :, 0]
    if not (a.ndim == 2 or (a.ndim == 3 and a.shape[2] in (3, 4))):
        raise ValueError(f"input {clip}: shape {a.shape} (expected H x W, H x W x 1, "
                         "H x W x 3 or H x W x 4)")
    if not np.all(np.isfinite(a)):
        raise ValueError(f"input {clip}: non-finite values")
    path = workdir / f"input{index}.pfm"
    spec = "file:" + str(path)
    if a.ndim == 3 and a.shape[2] == 4:
        # hostDemo reads the alpha of a PF file from a separate Pf file
        write_pfm(workdir / f"input{index}.alpha.pfm", a[:, :, 3])
        spec += "+" + str(workdir / f"input{index}.alpha.pfm")
        a = a[:, :, :3]
    write_pfm(path, a)
    if bounds is not None:
        x1, y1, x2, y2 = (int(v) for v in bounds)
        if (x2 - x1, y2 - y1) != (a.shape[1], a.shape[0]):
            raise ValueError(f"input {clip}: bounds {bounds} don't match the size {a.shape[1]}x{a.shape[0]}")
        spec += f"@{x1},{y1}"
    if premult is not None:
        spec += "#" + _PREMULT[premult]
    return spec


_OUTPUT_RE = re.compile(re.escape(OUTPUT_PREFIX) + r"\.(-?\d+(?:\.\d+)?(?:e[-+]\d+)?)([lr]?)(\.alpha)?\.pfm$")


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
           frames: Sequence[float] | str | None = None, scale: float | Sequence[float] | None = None,
           window: Sequence[int] | None = None, query: bool = False,
           host_binary: str | os.PathLike | None = None, timeout: float | None = 60,
           extra_args: Sequence[str] = (), check: bool = True,
           load: Mapping[str, Any] | None = None) -> RenderResult:
    """Render a plug-in with hostDemo (by default at frames 0 and 1); return its output
    images and report.

    bundle_paths: a directory containing .ofx.bundle directories, a bundle path (its parent
        directory is used), or a list of these. They become OFX_PLUGIN_PATH; the standard
        plug-in locations and the plug-in cache are not used.
    inputs: clip name -> a NumPy array (H x W, H x W x 1, H x W x 3 or H x W x 4; top-down;
        written to PFM files, at pixel 0,0), an Image (positioned at its bounds, with its
        premultiplication), a pathlib.Path or "file:path.pfm", or a hostDemo spec string:
        "none" (disconnected), "default", "solid:r,g,b,a", "ramp", "checker:N".
    params: parameter name -> value (number, bool, string, or a sequence for
        multi-dimensional parameters), passed as -s name=value; or keys, a mapping
        {time: value} (numeric parameters that animate; hostDemo interpolates linearly), passed
        as -s name@time=value (in time order); or None to press a push button (-s name). In
        order: each one is followed by the instance changed action (at the key's time for a key).
    depth: the input clips' depth before clip preferences: "byte", "short" or "float".
    frames: the times to render (hostDemo --frames; default 0 and 1), or "domain": every frame
        of the effect's time domain (getTimeDomain; general context only).
    scale: the render scale, s or (sx, sy), in (0,1] (--scale). Image inputs are at that scale.
    window: the render window (x1, y1, x2, y2), in pixels at the render scale (--window).
    query: only run the query actions (--query): no output, see .report["renders"].
    check: raise HostDemoError on a non-zero exit status (default).
    load: parameters set before createInstance, without instanceChanged, as a host loading a
        project (--load; values or keys as in params, no button presses).
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
        if frames == "domain":
            cmd += ["--frames", "domain"]
        elif frames is not None:
            if isinstance(frames, str):
                raise ValueError(f"frames {frames!r}: expected a sequence of times or 'domain'")
            cmd += ["--frames", ",".join(_format_value(float(f)) for f in frames)]
        for name, value in (load or {}).items():
            if value is None:
                raise ValueError(f"load {name!r}: a push button can't be pressed before createInstance")
            cmd += ["--load" if a == "-s" else a for a in _param_args(name, value)]
        if scale is not None:
            cmd += ["--scale", _format_value(scale)]
        if window is not None:
            cmd += ["--window", ",".join(str(int(v)) for v in window)]
        if query:
            cmd += ["--query"]
        cmd += list(extra_args)
        for name, value in (params or {}).items():
            cmd += _param_args(name, value)

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
                        returncode=p.returncode, stdout=p.stdout, stderr=p.stderr, cmd=cmd,
                        images=_images(outputs, report))


def _images(outputs: dict, report: dict) -> dict:
    """outputs -> Image, with the bounds from the report (matched by file name)."""
    bounds = {}
    for r in report.get("renders") or []:
        o = r.get("output")
        if o and o.get("files"):
            bounds[Path(o["files"][0]).name] = tuple(o["bounds"])
    premult = (report.get("clipPreferences") or {}).get("outputPremultiplication")
    images = {}
    for key, pixels in outputs.items():
        t, view = key if isinstance(key, tuple) else (key, "")
        name = f"{OUTPUT_PREFIX}.{_time_name(t)}{view}.pfm"
        images[key] = Image(pixels, bounds.get(name, (0, 0, pixels.shape[1], pixels.shape[0])), premult)
    return images


def _time_name(t: float) -> str:
    """The time as hostDemo writes it in file names (C++ ostream, precision 12)."""
    return f"{float(t):.12g}"


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
