"""ofxhost: render OFX plug-ins from Python, with the HostSupport demo host (hostDemo).

Two backends, with the same API and the same JSON report: "subprocess" (the default) runs hostDemo
in its own process and temporary directory, so that a crashing plug-in can't take down the
interpreter (images are PFM files); "inprocess" (ofxhost.inprocess) is hostDemo's test host in
Python, on the HostSupport bindings (the ofxh module), in this process (images are NumPy arrays).
Choose with render(..., backend=...) or describe(..., backend=...), or $OFXHOST_BACKEND.

NumPy images are float32, H x W x C, top-down (row 0 is the top row); OFX images and PFM files
are bottom-up, and the wrapper flips them.

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
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import numpy as np

from .build import hostdemo_candidates
from .constants import kOfxImageComponentAlpha, kOfxImageComponentRGB, kOfxImageComponentRGBA
from .pfm import encode_pfm, merge_alpha, parse_pfm, read_pfm, write_pfm, write_pfm_files
from .pixels import check_array
from .rects import scale_pair

__all__ = [
    "render", "describe", "RenderResult", "Image", "HostDemoError", "HostDemoNotFound",
    "find_hostdemo", "BACKENDS", "default_backend", "COMPONENTS", "split_components", "plugin_dirs",
    "resolve_file_spec", "read_pfm", "write_pfm", "parse_pfm", "encode_pfm",
]

OUTPUT_PREFIX = "Output"
DEPTHS = ("byte", "short", "float")
BACKENDS = ("subprocess", "inprocess")
# the components of an input (render(components=...), or a spec's "#rgb" suffix): short and OFX names
COMPONENTS = {"rgba": "rgba", "rgb": "rgb", "alpha": "alpha", kOfxImageComponentRGBA: "rgba",
              kOfxImageComponentRGB: "rgb", kOfxImageComponentAlpha: "alpha"}


def split_components(spec: str) -> tuple[str, str | None]:
    """Split the components suffix off an input spec.

    Args:
        spec: an input spec, which may end with "#rgb", "#alpha" or "#rgba" (hostDemo's -i suffix,
            on both backends; e.g. "ramp#rgb", "file:x.pfm#premult#alpha").

    Returns:
        (the spec without the suffix, the components or None).
    """
    head, sep, tail = spec.rpartition("#")
    return (head, tail) if sep and head and tail in ("rgb", "rgba", "alpha") else (spec, None)


def default_backend(backend: str | None = None) -> str:
    """The backend: the argument, else $OFXHOST_BACKEND, else "subprocess" (ValueError if unknown)."""
    b = backend or os.environ.get("OFXHOST_BACKEND") or "subprocess"
    if b not in BACKENDS:
        raise ValueError(f"backend {b!r}: expected one of {BACKENDS}"
                         + ("" if backend else " ($OFXHOST_BACKEND)"))
    return b


def _backend_for(backend: str | None, host_binary) -> str:
    """The backend of a call.

    A host_binary means hostDemo (subprocess), whatever $OFXHOST_BACKEND, and can't go with backend="inprocess".
    """
    if host_binary is not None:
        if backend == "inprocess":
            raise ValueError("host_binary is hostDemo's: it can't be given with backend=\"inprocess\"")
        if backend is None:
            return "subprocess"
    return default_backend(backend)


class HostDemoError(RuntimeError):
    """hostDemo failed: a non-zero exit status, a timeout, or an unreadable output.

    In-process: the run failed (returncode 1, with hostDemo's error message in stderr).
    """

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
    """The hostDemo binary wasn't found.

    Build it (CMake with -DBUILD_EXAMPLE_HOSTS=ON, or make -C HostSupport/examples), or set $OFXHOST_HOSTDEMO.
    """


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

def find_hostdemo(host_binary: str | os.PathLike | None = None) -> Path:
    """Locate hostDemo.

    Args:
        host_binary: a path, or a command found in $PATH; None: $OFXHOST_HOSTDEMO, else the build
            trees (ofxhost.build.hostdemo_candidates()).

    Returns:
        The absolute path of the binary.

    Raises:
        HostDemoNotFound: not found.
    """
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
    candidates = hostdemo_candidates()
    if not candidates:
        raise HostDemoNotFound(
            "hostDemo not found: build it (cmake -DBUILD_EXAMPLE_HOSTS=ON, or make -C HostSupport && make -C "
            "HostSupport/examples), or set OFXHOST_HOSTDEMO")
    return candidates[0].resolve()


# --- running hostDemo ------------------------------------------------------------------

def plugin_dirs(bundle_paths) -> list[str]:
    """The plug-in directories of render()'s bundle_paths (OFX_PLUGIN_PATH entries).

    Args:
        bundle_paths: a directory, a *.bundle path (which stands for its parent directory), or a
            list of these.

    Returns:
        The absolute directories, in order, without duplicates.

    Raises:
        FileNotFoundError: one isn't a directory.
    """
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


# as hostDemo parses it (hostDemoClipInstance.cpp): "#premult|#unpremult|#opaque" at the end,
# then "@x1,y1", then the alpha file after the last ".pfm+"
_FILE_SPEC = re.compile(r"(?P<rest>.*?)(?P<pos>@-?\d+,-?\d+)?(?P<premult>#(?:premult|unpremult|opaque))?$")


def resolve_file_spec(spec: str, base: str | os.PathLike | None = None) -> str:
    """Make the paths of a hostDemo input spec absolute.

    Args:
        spec: "file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]".
        base: the directory the paths are relative to (None: the current directory).

    Returns:
        The spec with absolute paths and the same suffixes.
    """
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


def _hostdemo_input(clip: str, value: Any, workdir: Path, index: int) -> str:
    """hostDemo's -i spec of an input of render().

    An array or an Image is written to PFM files in workdir, input<index>.pfm (and input<index>.alpha.pfm).
    """
    if isinstance(value, os.PathLike):
        return "file:" + str(Path(value).resolve())
    if isinstance(value, str):
        if value.startswith("file:"):
            return resolve_file_spec(value)
        return value
    bounds = premult = None
    if isinstance(value, Image):
        value, bounds, premult = value.pixels, value.bounds, value.premult
    a, origin, p = check_array(clip, value, bounds, premult)
    # (hostDemo reads the alpha of a PF file from a separate Pf file: "file:x.pfm+x.alpha.pfm")
    spec = "file:" + "+".join(write_pfm_files(workdir / f"input{index}.pfm", a))
    if origin is not None:
        spec += f"@{origin[0]},{origin[1]}"
    if p is not None:
        spec += "#" + p
    return spec


_OUTPUT_RE = re.compile(re.escape(OUTPUT_PREFIX) + r"\.(-?\d+(?:\.\d+)?(?:e[-+]\d+)?)([lr]?)(\.alpha)?\.pfm$")


def _output_key(t: float, view: str = ""):
    """The key of an output image in RenderResult.outputs: the time (an int if it is one), or (time, "l" or "r")."""
    key = int(t) if float(t).is_integer() else t
    return (key, view) if view else key


def _key_order(key) -> tuple:
    """The sort key of _output_key()'s keys: by time, then view."""
    return key if isinstance(key, tuple) else (key, "")


def _read_outputs(workdir: Path) -> tuple[dict, dict]:
    colour: dict = {}
    alpha: dict = {}
    for p in sorted(workdir.iterdir()):
        m = _OUTPUT_RE.match(p.name)
        if not m:
            continue
        key = _output_key(float(m.group(1)), m.group(2))
        (alpha if m.group(3) else colour)[key] = read_pfm(p)
    outputs: dict = {}
    alphas: dict = {}
    for key in sorted(colour, key=_key_order):
        img = colour[key]
        outputs[key] = merge_alpha(img, alpha.get(key))
        if img.ndim == 2:  # (Pf: an Alpha image)
            alphas[key] = img
        elif key in alpha:
            alphas[key] = alpha[key]
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
    env["OFX_PLUGIN_PATH"] = os.pathsep.join(plugin_dirs(bundle_paths))
    return env


def render(bundle_paths, plugin_id: str, context: str = "filter",
           inputs: Mapping[str, Any] | None = None,
           params: Mapping[str, Any] | None = None,
           depth: str = "float",
           frames: Sequence[float] | None = None, scale: float | Sequence[float] | None = None,
           window: Sequence[int] | None = None, query: bool = False,
           host_binary: str | os.PathLike | None = None, timeout: float | None = 60,
           extra_args: Sequence[str] = (), check: bool = True,
           backend: str | None = None, components: Mapping[str, str] | None = None) -> RenderResult:
    """Render a plug-in with hostDemo (by default at frames 0 and 1).

    Args:
        bundle_paths: a directory containing .ofx.bundle directories, a bundle path (its parent
            directory is used), or a list of these. They become OFX_PLUGIN_PATH; the standard
            plug-in locations and the plug-in cache are not used.
        plugin_id: the plug-in identifier.
        context: "filter" or "general" (or their kOfxImageEffectContext* names).
        inputs: clip name -> a NumPy array (H x W, H x W x 1, H x W x 3 or H x W x 4; top-down; at
            pixel 0,0), an Image (positioned at its bounds, with its premultiplication), a
            pathlib.Path or "file:path.pfm", or a hostDemo spec string: "none" (disconnected),
            "default", "solid:r,g,b,a", "ramp", "checker:N", "rod:x1,y1,x2,y2".
        params: parameter name -> value (number, bool, string, or a sequence for multi-dimensional
            parameters), passed as -s name=value; or keys, a mapping {time: value} (numeric
            parameters that animate; hostDemo interpolates linearly), passed as -s name@time=value
            in time order; or None to press a push button (-s name). In order: each one is followed
            by the instance changed action (at the key's time for a key).
        depth: the input clips' depth before the clip preferences: "byte", "short" or "float".
        frames: the times to render (hostDemo's --frames; default 0 and 1).
        scale: the render scale, s or (sx, sy) (a sequence or a NumPy array), in (0,1] (--scale).
            Image inputs are at that scale.
        window: the render window (x1, y1, x2, y2), in pixels at the render scale (--window).
        query: only run the query actions (--query): no output, see .report["renders"].
        host_binary: the hostDemo binary (see find_hostdemo()); subprocess backend only.
        timeout: hostDemo's timeout in seconds (HostDemoError after it); subprocess backend only.
        extra_args: hostDemo options. The in-process backend also takes --format ppm|pfm, -o
            PREFIX and --report FILE (it then writes the outputs and the report as hostDemo does,
            relative to the current directory); the subprocess backend refuses them (ValueError):
            it runs hostDemo with its own.
        check: raise HostDemoError on a non-zero exit status.
        backend: "subprocess" (hostDemo) or "inprocess" (ofxhost.inprocess, on the ofxh module; it
            supports only some extra_args); default: "subprocess" if host_binary is given
            (ValueError with "inprocess"), else $OFXHOST_BACKEND, else "subprocess".
        components: clip name -> "rgb", "alpha" or "rgba" (or the kOfxImageComponent* names): the
            components of that input (its unmapped components; default RGBA; a spec string may also
            end with "#rgb", "#alpha" or "#rgba", as hostDemo's -i, which this overrides); a clip not
            in inputs gets the default pattern.

    Returns:
        The RenderResult: the output images and the report.

    Raises:
        ValueError: a bad argument.
        HostDemoError: hostDemo failed (with check), timed out, or wrote no readable output.
        HostDemoNotFound: no hostDemo binary (subprocess backend).
    """
    comps = _render_components(depth, components)
    if _backend_for(backend, host_binary) == "inprocess":
        from . import inprocess
        return inprocess.render(bundle_paths, plugin_id, context=context, inputs=inputs, params=params, depth=depth,
                                frames=frames, scale=scale, window=window, query=query, extra_args=extra_args,
                                check=check, components=components)
    own = [a for a in extra_args if a in ("--format", "-o", "--report")]
    if own:
        raise ValueError(f"extra_args {own}: the subprocess backend runs hostDemo with its own --format pfm, -o and "
                         "--report (use backend=\"inprocess\" to write the files, or run hostDemo itself)")
    given = _given_inputs(inputs, comps)
    host = find_hostdemo(host_binary)
    env = _env(bundle_paths)
    with tempfile.TemporaryDirectory(prefix="ofxhost-") as tmp:
        workdir = Path(tmp)
        report_path = workdir / "report.json"
        cmd = [str(host), "--no-std-paths", "--no-cache", "--quiet", "--format", "pfm",
               "--report", str(report_path),
               "-p", plugin_id, "-c", context, "-d", depth, "-o", OUTPUT_PREFIX]
        for i, (clip, value) in enumerate(given.items()):
            # (hostDemo's "#rgb", "#alpha" or "#rgba" suffix; components= overrides the spec's)
            body, suffix = split_components(value) if isinstance(value, str) else (value, None)
            if clip in comps:
                suffix = None if comps[clip] == "rgba" else comps[clip]
            cmd += ["-i", f"{clip}={_hostdemo_input(clip, body, workdir, i)}" + (f"#{suffix}" if suffix else "")]
        cmd += _hostdemo_options(frames, scale, window, query)
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


def _render_components(depth: str, components: Mapping[str, str] | None) -> dict:
    """render()'s depth and components checked (ValueError): {clip: "rgba", "rgb" or "alpha"}."""
    if depth not in DEPTHS:
        raise ValueError(f"depth {depth!r}: expected one of {DEPTHS}")
    comps = {}
    for clip, c in (components or {}).items():
        if c not in COMPONENTS:
            raise ValueError(f"components of input {clip!r}: {c!r}, expected rgba, rgb or alpha")
        comps[clip] = COMPONENTS[c]
    return comps


def _given_inputs(inputs: Mapping | None, comps: Mapping) -> dict:
    """render()'s inputs, with the default pattern for the clips that only have components."""
    given = dict(inputs or {})
    for clip in comps:
        given.setdefault(clip, "default")
    return given


def _hostdemo_options(frames, scale, window, query: bool) -> list[str]:
    """hostDemo's --frames, --scale, --window and --query options for render()'s arguments."""
    cmd = []
    if frames is not None:
        cmd += ["--frames", ",".join(_format_value(float(f)) for f in frames)]
    if scale is not None:
        cmd += ["--scale", ",".join(_format_value(s) for s in scale_pair(scale))]
    if window is not None:
        cmd += ["--window", ",".join(str(int(v)) for v in window)]
    if query:
        cmd += ["--query"]
    return cmd


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


def write_json(path, doc) -> None:
    """Write a report as hostDemo does (indented, no NaN, a final newline); raises OSError or ValueError."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, allow_nan=False)
        f.write("\n")


def _time_name(t: float) -> str:
    """The time as hostDemo writes it in file names (C++ ostream, precision 12)."""
    return f"{float(t):.12g}"


def describe(bundle_paths, plugin_id: str, context: str | None = None,
             host_binary: str | os.PathLike | None = None, timeout: float | None = 60,
             extra_args: Sequence[str] = (), backend: str | None = None) -> dict:
    """hostDemo's --describe JSON of a plug-in: its contexts, clips, parameters and flags.

    Args:
        bundle_paths, plugin_id, host_binary, timeout, extra_args, backend: as render().
        context: hostDemo's -c (hostDemo describes every context anyway), or None.

    Returns:
        The JSON, as a dict.

    Raises:
        HostDemoError: hostDemo failed (e.g. the plug-in isn't found).
    """
    if _backend_for(backend, host_binary) == "inprocess":
        from . import inprocess
        return inprocess.describe(bundle_paths, plugin_id, context=context, extra_args=extra_args)
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
