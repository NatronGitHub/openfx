"""The in-process backend of ofxhost.render() and ofxhost.describe(): hostDemo's test host, in
Python, on the HostSupport bindings (the ofxh module; NatronGitHub/openfx#136, part 3).

    r = ofxhost.render(bundles, "net.sf.openfx.invertPlugin", inputs={"Source": src},
                       backend="inprocess")      # or OFXHOST_BACKEND=inprocess

It has the API and the JSON report of the subprocess backend (hostDemo --report, see README.md):
the same actions in the same order, the same inputs (the same patterns, PFM files and arrays, at
each clip's depth), the same render window and image bounds, the same messages, and the same
errors (returncode 1, with hostDemo's message in stderr). This module is a port of hostDemo's
run() and renderFrame() (HostSupport/examples/hostDemo.cpp) and of its clips and instance
(hostDemoClipInstance.cpp, hostDemoEffectInstance.cpp); it is also an example of a full host on
ofxh: Host, ImageEffectInstance and ClipInstance subclasses, and the actions in a host's order.

Differences from the subprocess backend:
- the images aren't written to files: inputs are NumPy arrays given to the plug-in without a copy
  (a view of the input converted once to the clip's depth), and outputs are the arrays the plug-in
  rendered into. The report's output "files" is [], and an array input's "spec" is
  "array:WxHxC[@x1,y1][#premult]";
- the report's host name is ofxh's ("net.sf.openfx.ofxh"), not hostDemo's;
- ofxh's string choice parameters refuse a value that is not one of their enums (hostDemo's accept it);
- timeout is not applied (a hang hangs the caller), and a plug-in crash kills the interpreter;
- one host per process (ofxh): the backend keeps its ofxh host between renders with the same
  plug-in directories and profile, so the plug-ins stay loaded (hostDemo loads them in each run),
  and refuses to run if another ofxh host is initialized (RuntimeError). shutdown() releases it;
- stdout and stderr are captured at the file-descriptor level (the plug-ins' printf and
  std::cerr), for the duration of each call, which runs in a temporary current directory (as
  hostDemo's);
- extra_args: only --ofx14, --allow-unwritten and -m MAJOR (and the no-ops --quiet, --no-cache,
  --no-std-paths); anything else raises ValueError.
"""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import math
import os
import sys
import tempfile
import traceback
import zlib
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import numpy as np

from . import (DEPTHS, OUTPUT_PREFIX, HostDemoError, Image, RenderResult, _format_value, _param_args,
               _plugin_dirs, _PREMULT, resolve_file_spec)

__all__ = ["render", "describe", "shutdown", "import_ofxh", "OfxhNotFound"]


class OfxhNotFound(ImportError):
    """The ofxh module (the HostSupport Python bindings) can't be imported."""


_REPO_ROOT = Path(__file__).resolve().parents[4]


def import_ofxh():
    """Import ofxh: from sys.path (PYTHONPATH), else from an openfx CMake build tree
    (build*/HostSupport/python, built with -DOFX_PYTHON_BINDINGS=ON)."""
    try:
        import ofxh
        return ofxh
    except ImportError:
        pass
    for d in sorted(_REPO_ROOT.glob("build*/HostSupport/python")):
        if any(d.glob("ofxh*.so")) or any(d.glob("ofxh*.pyd")):
            sys.path.insert(0, str(d))
            try:
                import ofxh
                return ofxh
            except ImportError:
                sys.path.remove(str(d))
    raise OfxhNotFound("the ofxh module (HostSupport Python bindings) is not importable: build it with "
                       "cmake -DOFX_PYTHON_BINDINGS=ON and set PYTHONPATH to <build>/HostSupport/python, "
                       "or pip install ./HostSupport/python")


ofxh = import_ofxh()

# --- hostDemo's constants -------------------------------------------------------------------

CLIP_LENGTH = 1.0  # OFXHOSTDEMOCLIPLENGTH: the default input is black at t=0, white at t=1
PAL_PAR = 768.0 / 720.0
MAX_IMAGE_PIXELS = 1 << 28  # kMaxImagePixels
MAX_KEY_TIME = 1e15
EPS = 1e-6  # canonicalToPixelEnclosing
INT_MIN, INT_MAX = -2**31, 2**31 - 1

_VEGAS = bool(ofxh.build_config.get("OFX_EXTENSIONS_VEGAS"))
_NUKE = bool(ofxh.build_config.get("OFX_EXTENSIONS_NUKE"))

_STAT_NAMES = {0: "kOfxStatOK", 1: "kOfxStatFailed", 2: "kOfxStatErrFatal", 3: "kOfxStatErrUnknown",
               4: "kOfxStatErrMissingHostFeature", 5: "kOfxStatErrUnsupported", 6: "kOfxStatErrExists",
               7: "kOfxStatErrFormat", 8: "kOfxStatErrMemory", 9: "kOfxStatErrBadHandle",
               10: "kOfxStatErrBadIndex", 11: "kOfxStatErrValue", 12: "kOfxStatReplyYes",
               13: "kOfxStatReplyNo", 14: "kOfxStatReplyDefault"}


def _stat_str(st) -> str:
    """OFX::StatStr()"""
    return _STAT_NAMES.get(int(st), "(unknown error code)")


def _ok(st) -> bool:
    return int(st) in (0, 14)  # kOfxStatOK, kOfxStatReplyDefault


def _action(st) -> dict:
    return {"ok": _ok(st), "status": _stat_str(st)}


def _named(action: str, result: dict) -> dict:
    return {"action": action, "ok": result["ok"], "status": result["status"]}


def _ofx(kind: str, name: str) -> str:
    return ofxh.ofx_name(kind, name)


# --- numbers and rectangles, as hostDemo parses and computes them -----------------------------

def _parse_double(s: str):
    """parseFiniteDouble: a finite decimal, or None."""
    if not s or any(c not in "0123456789+-.eE" for c in s):
        return None
    try:
        v = float(s)
    except ValueError:
        return None
    if not math.isfinite(v) or (v != 0 and abs(v) < sys.float_info.min):  # (strtod's ERANGE)
        return None
    return v


def _parse_int(s: str):
    """parseInteger: a decimal int, or None."""
    if not s or any(not (c.isdigit() and c.isascii()) and not (k == 0 and c in "+-") for k, c in enumerate(s)):
        return None
    try:
        v = int(s)
    except ValueError:
        return None
    return v if INT_MIN <= v <= INT_MAX else None


def _parse_bool(s: str):
    if s in ("1", "true", "yes", "on"):
        return True
    if s in ("0", "false", "no", "off"):
        return False
    return None


def _parse_n(s: str, n: int, parse):
    v = s.split(",")
    if len(v) != n:
        return None
    r = [parse(x) for x in v]
    return None if any(x is None for x in r) else r


def _clamp_int(v: float) -> int:
    if math.isnan(v):
        return 0
    if v <= INT_MIN:
        return INT_MIN
    if v >= INT_MAX:
        return INT_MAX
    return int(v)


def _to_pixels(r, rs, par):
    """canonicalToPixelEnclosing"""
    return (_clamp_int(math.floor(r[0] * rs[0] / par + EPS)), _clamp_int(math.floor(r[1] * rs[1] + EPS)),
            _clamp_int(math.ceil(r[2] * rs[0] / par - EPS)), _clamp_int(math.ceil(r[3] * rs[1] - EPS)))


def _intersect(a, b):
    return (max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3]))


def _empty(r) -> bool:
    return r[2] <= r[0] or r[3] <= r[1]


def _fits(b, bytes_per_pixel: int = 16) -> bool:
    """imageFits"""
    w, h = b[2] - b[0], b[3] - b[1]
    return w <= MAX_IMAGE_PIXELS and h <= MAX_IMAGE_PIXELS and w * h <= MAX_IMAGE_PIXELS and \
        w * bytes_per_pixel <= 0x7fffffff


_BPC = {"byte": 1, "short": 2, "float": 4}
_NCOMPS = {"rgba": 4, "rgb": 3, "alpha": 1}

# --- stdout and stderr ----------------------------------------------------------------------

_fflush = None


def _flush_c() -> None:
    """fflush(NULL): the C stdio buffers of the plug-ins (and ours) before the descriptors move."""
    global _fflush
    if _fflush is None:
        try:
            lib = ctypes.CDLL("ucrtbase") if sys.platform == "win32" else ctypes.CDLL(None)
            _fflush = lib.fflush
            _fflush.argtypes = [ctypes.c_void_p]
        except (OSError, AttributeError):
            _fflush = False
    if _fflush:
        _fflush(None)


def _write(fd: int, text: str) -> None:
    """Write to a file descriptor, after the C buffers (the order of hostDemo's output)."""
    _flush_c()
    data = text.encode("utf-8", "replace")
    while data:
        n = os.write(fd, data)
        data = data[n:]


@contextlib.contextmanager
def _capture():
    """Capture the file descriptors 1 and 2 (what hostDemo's stdout and stderr would get) into
    out["stdout"] and out["stderr"], in a temporary current directory (as the subprocess backend
    runs hostDemo: the files that plug-ins write there, e.g. logs, are deleted)."""
    out = {"stdout": "", "stderr": ""}
    cwd = os.getcwd()
    tmp = tempfile.TemporaryDirectory(prefix="ofxhost-")
    os.chdir(tmp.name)
    for f in (sys.stdout, sys.stderr):
        try:
            f.flush()
        except (AttributeError, ValueError, OSError):
            pass
    _flush_c()
    files = {fd: tempfile.TemporaryFile() for fd in (1, 2)}
    saved = {fd: os.dup(fd) for fd in (1, 2)}
    try:
        for fd, f in files.items():
            os.dup2(f.fileno(), fd)
        yield out
    finally:
        _flush_c()
        for fd in (1, 2):
            os.dup2(saved[fd], fd)
            os.close(saved[fd])
        for fd, name in ((1, "stdout"), (2, "stderr")):
            files[fd].seek(0)
            out[name] = files[fd].read().decode("utf-8", "replace")
            files[fd].close()
        os.chdir(cwd)
        tmp.cleanup()


# --- the inputs -----------------------------------------------------------------------------

# 5x3 bitmaps of the digits 0..9 and the period (hostDemoClipInstance.cpp)
_DIGITS = np.array([
    [[0, 1, 0], [1, 0, 1], [1, 0, 1], [1, 0, 1], [0, 1, 0]],
    [[0, 1, 0], [1, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0]],
    [[0, 1, 0], [1, 0, 1], [0, 0, 1], [0, 1, 0], [1, 1, 1]],
    [[1, 1, 0], [0, 0, 1], [0, 1, 1], [0, 0, 1], [1, 1, 0]],
    [[0, 1, 0], [1, 0, 0], [1, 0, 1], [1, 1, 1], [0, 0, 1]],
    [[1, 1, 1], [1, 0, 0], [1, 1, 0], [0, 0, 1], [1, 1, 0]],
    [[0, 1, 1], [1, 0, 0], [1, 1, 0], [1, 0, 1], [0, 1, 0]],
    [[1, 1, 1], [0, 0, 1], [0, 1, 0], [0, 1, 1], [0, 1, 0]],
    [[0, 1, 0], [1, 0, 1], [0, 1, 0], [1, 0, 1], [0, 1, 0]],
    [[0, 1, 0], [1, 0, 1], [0, 1, 1], [0, 0, 1], [1, 1, 0]],
    [[0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 1, 0]]], dtype=bool)


class _SpecError(ValueError):
    pass


@dataclasses.dataclass
class _Spec:
    """What an input clip contains (hostDemo's InputSpec)."""
    type: str = "default"  # default, none, ramp, checker, solid, file, rod
    text: str = "default"  # for the report
    colour: tuple = (0., 0., 0., 0.)
    rod: tuple = (0., 0., 0., 0.)
    checker: int = 16
    data: Any = None  # file: float32 (h, w, 4), bottom-up, RGBA (PF: alpha 1 or the alpha file; Pf: v,v,v,v)
    file_x: int = 0
    file_y: int = 0
    premult: str | None = None  # file: the short name (premult, unpremult, opaque), or None
    scale: tuple = (1., 1.)  # file: the render scale


def _read_pfm(path: str) -> np.ndarray:
    """hostDemo's readPFM: a little-endian PFM, as a bottom-up float32 array (h, w[, 3])."""
    try:
        with open(path, "rb") as f:
            buf = f.read()
    except OSError:
        raise _SpecError(f"cannot open {path}") from None
    if len(buf) < 3 or buf[0:1] != b"P" or buf[1:2] not in (b"F", b"f") or not buf[2:3].isspace():
        raise _SpecError(f"{path}: not a PFM file (it must start with PF or Pf)")
    ncomps = 3 if buf[1:2] == b"F" else 1
    pos = 2

    def token():
        nonlocal pos
        while pos < len(buf) and buf[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(buf) and not buf[pos:pos + 1].isspace() and pos - start < 64:
            pos += 1
        return buf[start:pos].decode("latin-1")

    ws, hs = token(), token()
    w, h = _parse_int(ws), _parse_int(hs)
    if w is None or h is None or w <= 0 or h <= 0 or w > 65536 or h > 65536 or w * h > (1 << 24):
        raise _SpecError(f'{path}: invalid PFM size "{ws} {hs}" (the width and height must be positive, at '
                         "most 65536, and at most 2^24 pixels)")
    ss = token()
    scale = _parse_double(ss)
    if scale is None or scale >= 0:
        raise _SpecError(f'{path}: invalid PFM scale "{ss}" (it must be negative: only little-endian PFM files '
                         "are supported)")
    if pos >= len(buf) or not buf[pos:pos + 1].isspace():
        raise _SpecError(f"{path}: no whitespace after the PFM scale")
    pos += 1
    expected = w * h * ncomps * 4
    found = len(buf) - pos
    if found != expected:
        raise _SpecError(f"{path}: a {w}x{h} {'PF' if ncomps == 3 else 'Pf'} file has {expected} bytes of pixel "
                         f"data, but there are {found} {'(truncated)' if found < expected else '(trailing data)'}")
    a = np.frombuffer(buf, dtype="<f4", count=w * h * ncomps, offset=pos).astype(np.float32)
    bad = np.flatnonzero(~np.isfinite(a))
    if bad.size:
        i = int(bad[0])
        pixel = i // ncomps
        raise _SpecError(f"{path}: the value of channel {i % ncomps} of pixel ({pixel % w},{pixel // w}) (x,y from "
                         "the bottom left) is not finite")
    return a.reshape((h, w, 3) if ncomps == 3 else (h, w))


def _rgba(rgb: np.ndarray, alpha: np.ndarray | None = None) -> np.ndarray:
    """PF gives (r,g,b,1) (or the alpha file's alpha), Pf gives r=g=b=a=value: (h, w, 4) float32."""
    if rgb.ndim == 2:
        return np.repeat(rgb[:, :, None], 4, axis=2)
    a = np.ones(rgb.shape[:2], np.float32) if alpha is None else alpha
    return np.dstack([rgb, a]).astype(np.float32)


def _parse_spec(s: str) -> _Spec:
    """hostDemo's parseInputSpec (raises _SpecError with its message)."""
    if s in ("default", "none", "ramp"):
        return _Spec(type=s, text=s)
    if s.startswith("rod:"):
        v = [_parse_double(x) for x in s[4:].split(",")]
        if len(v) != 4 or any(x is None for x in v) or v[2] < v[0] or v[3] < v[1]:
            raise _SpecError("invalid RoD, expected rod:x1,y1,x2,y2 with x1<=x2 and y1<=y2")
        return _Spec(type="rod", text=s, rod=tuple(v))
    if s.startswith("file:"):
        path = s[5:]
        premult = None
        hash_ = path.rfind("#")
        if hash_ >= 0 and path[hash_ + 1:] in ("premult", "unpremult", "opaque"):
            premult = path[hash_ + 1:]
            path = path[:hash_]
        fx = fy = 0
        at = path.rfind("@")
        if at >= 0:
            x1, comma, y1 = path[at + 1:].partition(",")
            if comma and _parse_int(x1) is not None and _parse_int(y1) is not None:
                fx, fy = _parse_int(x1), _parse_int(y1)
                if not (-1000000 <= fx <= 1000000 and -1000000 <= fy <= 1000000):
                    raise _SpecError("file position out of range")
                path = path[:at]
        alpha_path = ""
        plus = path.rfind(".pfm+")
        if plus >= 0:
            alpha_path = path[plus + 5:]
            path = path[:plus + 4]
        if not path:
            raise _SpecError("no file name")
        rgb = _read_pfm(path)
        alpha = None
        if alpha_path:
            alpha = _read_pfm(alpha_path)
            if rgb.ndim != 3 or alpha.ndim != 2 or alpha.shape != rgb.shape[:2]:
                raise _SpecError(f"{alpha_path}: the alpha file must be a Pf file of the size of a PF file")
        return _Spec(type="file", text=s, data=_rgba(rgb, alpha), file_x=fx, file_y=fy, premult=premult)
    if s.startswith("checker:"):
        n = _parse_int(s[8:])
        if n is None or n < 1 or n > 1000000:
            raise _SpecError("invalid checker size")
        return _Spec(type="checker", text=s, checker=n)
    if s.startswith("solid:"):
        v = s[6:].split(",")
        if len(v) != 4:
            raise _SpecError("solid takes 4 values")
        c = [_parse_double(x) for x in v]
        if any(x is None or abs(x) > 3.4028234663852886e38 for x in c):
            raise _SpecError("invalid number")
        return _Spec(type="solid", text=s, colour=tuple(c))
    raise _SpecError("unknown input type")


def _array_spec(clip: str, value: Any) -> _Spec:
    """An array or an ofxhost.Image input: what the subprocess backend writes to PFM files (the
    same checks and errors as ofxhost._input_spec)."""
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
    h, w = a.shape[:2]
    if w == 0 or h == 0:
        raise ValueError("PFM: empty image")  # (as encode_pfm)
    text = f"array:{w}x{h}x{1 if a.ndim == 2 else a.shape[2]}"
    fx = fy = 0
    if bounds is not None:
        x1, y1, x2, y2 = (int(v) for v in bounds)
        if (x2 - x1, y2 - y1) != (w, h):
            raise ValueError(f"input {clip}: bounds {bounds} don't match the size {w}x{h}")
        fx, fy = x1, y1
        text += f"@{x1},{y1}"
    p = None
    if premult is not None:
        p = _PREMULT[premult]
        text += "#" + p
    flipped = a[::-1]  # bottom-up, as in the PFM file
    data = _rgba(flipped) if a.ndim == 2 or a.shape[2] == 3 else np.ascontiguousarray(flipped, np.float32)
    return _Spec(type="file", text=text, data=data, file_x=fx, file_y=fy, premult=p)


def _input_spec(clip: str, value: Any) -> _Spec:
    """An input of render(): an array, an Image, a path, or a hostDemo spec string."""
    if isinstance(value, os.PathLike):
        return _parse_spec("file:" + str(Path(value).resolve()))
    if isinstance(value, str):
        return _parse_spec(resolve_file_spec(value) if value.startswith("file:") else value)
    return _array_spec(clip, value)


def _text_mask(time: float, view: int, px: np.ndarray, py: np.ndarray) -> np.ndarray:
    """textPixel: the pixels (h, w) of the time and view drawn by the default pattern."""
    at = abs(time)
    s = 5
    glyphs = (((int(at) // 10) % 10, 50, 50), (int(at) % 10, 70, 50), (10, 90, 50),
              (int(at * 10) % 10, 110, 50), (abs(view) % 10, 50, 90))
    m = np.zeros((py.size, px.size), bool)
    for d, x0, y0 in glyphs:
        i = (px - x0) // s
        j = (py - y0) // s
        vx = (px >= x0) & (i < 3)
        vy = (py >= y0) & (j < 5)
        if vx.any() and vy.any():
            m[np.ix_(vy, vx)] |= _DIGITS[d][j[vy]][:, i[vx]]
    return m


def _pattern(spec: _Spec, time: float, view: int, b, rod_px, rs) -> np.ndarray:
    """evalPattern at the pixel centres of the bounds b (pixels at the render scale rs): (h, w, 4)
    float64, unpremultiplied RGBA, bottom-up."""
    X = (np.arange(b[0], b[2], dtype=np.float64) + 0.5) / rs[0]
    Y = (np.arange(b[1], b[3], dtype=np.float64) + 0.5) / rs[1]
    h, w = Y.size, X.size
    t = spec.type
    if t == "solid":
        return np.broadcast_to(np.array(spec.colour, np.float64), (h, w, 4))
    if t == "ramp":
        W = (rod_px[2] - rod_px[0]) / rs[0]
        v = X / W if W > 0 else np.zeros_like(X)
        return np.broadcast_to(v[None, :, None], (h, w, 4))
    if t == "checker":
        i = np.floor(X / spec.checker).astype(np.int64)
        j = np.floor(Y / spec.checker).astype(np.int64)
        v = ((i[None, :] + j[:, None]) % 2 != 0).astype(np.float64)
        return np.broadcast_to(v[:, :, None], (h, w, 4))
    if t == "file":
        fh, fw = spec.data.shape[:2]
        fx = np.floor(X * spec.scale[0]) - spec.file_x
        fy = np.floor(Y * spec.scale[1]) - spec.file_y
        vx = (fx >= 0) & (fx < fw)
        vy = (fy >= 0) & (fy < fh)
        out = np.zeros((h, w, 4), np.float64)
        if vx.any() and vy.any():
            out[np.ix_(vy, vx)] = spec.data[fy[vy].astype(np.intp)][:, fx[vx].astype(np.intp)]
        return out
    if t == "rod":
        return np.zeros((h, w, 4), np.float64)
    # the default pattern: the value grows with time (clamped only in integer images), the time and
    # the view drawn in the reverse colour
    v = np.full((h, w), time / CLIP_LENGTH, np.float64)
    m = _text_mask(time, view, np.floor(X).astype(np.int64), np.floor(Y).astype(np.int64))
    v0 = time / CLIP_LENGTH
    v[m] = 1. - v0 if v0 <= 1. else 0.
    out = np.empty((h, w, 4), np.float64)
    if _VEGAS:
        out[..., 0] = v if view == 0 else 0.
        out[..., 1] = v if view == 1 else 0.
        out[..., 2] = v if view == 1 else 0.
    else:
        out[..., 0] = out[..., 1] = out[..., 2] = v
    out[..., 3] = 1.
    return out


def _to_depth(rgba: np.ndarray, depth: str, comps: str) -> np.ndarray:
    """The pattern at the clip's depth and components (RGB drops the alpha, Alpha keeps it)."""
    v = rgba[..., 3:4] if comps == "alpha" else rgba[..., :_NCOMPS[comps]]
    if depth == "float":
        return np.ascontiguousarray(v, dtype=np.float32)
    c = np.where(np.isnan(v) | (v <= 0), 0., np.minimum(v, 1.))
    if depth == "byte":
        return np.ascontiguousarray(np.floor(c * 255. + 0.5), dtype=np.uint8)
    return np.ascontiguousarray(np.floor(c * 65535. + 0.5), dtype=np.uint16)


# --- the host, the instance and the clips ---------------------------------------------------

class _Host(ofxh.Host):
    """hostDemo's Host (hostDemoHostDescriptor.cpp): its messages go to stdout, questions are
    answered yes."""

    def message(self, type, id, text):
        prefix = {"log": "Log : ", "fatal": "Error : ", "error": "Error : ",
                  "question": "Question : "}.get(type, "Message : ")
        _write(1, prefix + text + "\n")
        return ofxh.Stat.ReplyYes if type == "question" else ofxh.Stat.OK

    def persistent_message(self, type, id, text):
        return self.message(type, id, text)


class _Instance(ofxh.ImageEffectInstance):
    """hostDemo's MyEffectInstance (hostDemoEffectInstance.cpp): records the messages and the
    progress, and uses the input depth for the output of a generator."""

    def __init__(self, run: _Run):
        super().__init__()
        self.run = run
        self.messages: list[dict] = []
        self.progress: list[dict] = []
        self.default_output_depth = run.depth

    def new_clip_instance(self, desc, index):
        return _Clip(self.run, desc)

    def _record(self, kind, type, id, text):
        t = _ofx("message", type)
        _write(1, f"{t} {id} {text}")  # (hostDemo's printf, without a newline)
        self.messages.append({"kind": kind, "type": t, "id": id, "text": text})
        return ofxh.Stat.OK

    def message(self, type, id, text):
        return self._record("message", type, id, text)

    def persistent_message(self, type, id, text):
        return self._record("persistentMessage", type, id, text)

    def clear_persistent_message(self):
        self.messages.append({"kind": "clearPersistentMessage", "type": "", "id": "", "text": ""})

    def progress_start(self, message, id):
        self.progress.append({"message": message, "id": id, "updates": 0, "lastValue": 0., "ended": False})

    def progress_end(self):
        if self.progress:
            self.progress[-1]["ended"] = True

    def progress_update(self, t):
        if self.progress:
            self.progress[-1]["updates"] += 1
            self.progress[-1]["lastValue"] = t
        return True


class _Clip(ofxh.ClipInstance):
    """hostDemo's MyClipInstance (hostDemoClipInstance.cpp): the inputs are its patterns, PFM
    files or arrays; the output is ofxh's default image over the render window."""

    def __init__(self, run: _Run, desc):
        super().__init__()
        self.run = run
        self.spec = _Spec()  # the input (hostDemo's -i; the default pattern if not given)

    @property
    def connected(self):
        return self.is_output or self.spec.type != "none"

    @property
    def unmapped_bit_depth(self):
        # (hostDemo's -d depth for the inputs; its default, byte, for the output)
        return "byte" if self.is_output else self.run.depth

    @property
    def premult(self):
        # the output: the clip preferences; an RGB or disconnected input is opaque; else
        # unpremultiplied (the patterns), or the premultiplication of a file
        if self.is_output:
            return self.instance.output_premultiplication
        if not self.connected or self.components == "rgb":
            return "opaque"
        return self.spec.premult or "unpremult"

    @property
    def format(self):
        s = self.spec
        if not self.is_output and s.type == "file":
            h, w = s.data.shape[:2]
            return (math.floor(s.file_x / s.scale[0]), math.floor(s.file_y / s.scale[1]),
                    math.ceil((s.file_x + w) / s.scale[0]), math.ceil((s.file_y + h) / s.scale[1]))
        return (0, 0, 720, 576)

    def region_of_definition(self, time, view):
        if self.is_output:
            return super().region_of_definition(time, view)  # the plug-in's RoD action
        s = self.spec
        if s.type == "none":
            return (0., 0., 0., 0.)
        if s.type == "rod":
            return s.rod
        if s.type == "file":
            par = self.aspect_ratio
            h, w = s.data.shape[:2]
            return (s.file_x / s.scale[0] * par, s.file_y / s.scale[1],
                    (s.file_x + w) / s.scale[0] * par, (s.file_y + h) / s.scale[1])
        return (0., 0., 768., 576.)

    def get_image(self, time, view, bounds, plane="Colour"):
        """fetchImage: the output covers the render window; an input covers bounds (or the RoI of
        the render, or the RoD) clipped to the RoD, or its whole RoD without tiles."""
        run = self.run
        if not self.connected:
            return None
        depth, comps = self.pixel_depth, self.components
        if depth not in _BPC or comps not in _NCOMPS:
            run.err(f'Error: clip {self.name}: hostDemo can\'t make images with pixel depth "{_ofx("depth", depth)}" '
                    f'and components "{_ofx("components", comps)}"')
            run.image_errors += 1
            return None
        rs = run.render_scale
        par = self.aspect_ratio
        rod_c = self.region_of_definition(time, view)
        rod = _to_pixels(rod_c, rs, par)
        inst = run.instance
        tiles = inst.supports_tiles and self.supports_tiles
        if self.is_output:
            if run.rendering:
                b = inst.render_window
            elif not tiles and _fits(rod):
                b = rod
            else:
                b = _intersect(rod, inst.project_pixel_rect(rs))
            b = self._checked(b, time, depth, comps)
            return ofxh.ImageData(self.new_image(b), b, rod=rod)
        whole = not tiles
        too_large = False
        if whole and not _fits(rod):
            too_large = True
            if self.name not in run.warned_clips:
                run.warned_clips.add(self.name)
                run.err(f"Warning: clip {self.name}: the plug-in or the clip doesn't support tiles, but the RoD "
                        f"({_g(rod_c[0])},{_g(rod_c[1])},{_g(rod_c[2])},{_g(rod_c[3])}) is too large for hostDemo: "
                        "fetching the region of interest instead")
            whole = False
        region = rod_c
        if whole:
            pass
        elif bounds is not None:
            region = tuple(bounds)
        elif run.rendering and self.name in run.rois:
            region = run.rois[self.name]
        region = _intersect(region, rod_c)
        if _empty(region):
            return None  # nothing to fetch
        b = _intersect(_to_pixels(region, rs, par), rod)
        if too_large and not _fits(b) and run.rendering:
            b = _intersect(b, inst.render_window)
        if _empty(b):
            return None
        b = self._checked(b, time, depth, comps)
        pixels = _to_depth(_pattern(self.spec, time, view, b, rod, rs), depth, comps)
        return ofxh.ImageData(pixels, b, rod=rod)

    def _checked(self, b, time, depth, comps):
        """MyImage refuses images of more than 2^28 pixels (an image fetch error): empty bounds."""
        b = (b[0], b[1], max(b[2], b[0]), max(b[3], b[1]))
        if not _fits(b, _NCOMPS[comps] * _BPC[depth]):
            if (b, time) not in self.run.refused:
                self.run.refused.add((b, time))
                self.run.err(f"Error: image of {b[2] - b[0]}x{b[3] - b[1]} pixels at time {_g(time)}: too large "
                             f"(more than {MAX_IMAGE_PIXELS} pixels)")
                self.run.image_errors += 1
            b = (b[0], b[1], b[0], b[1])
        return b


def _g(v: float) -> str:
    """A number as C++ ostream writes it (6 significant digits)."""
    return f"{float(v):g}"


# --- the session: ofxh's one host -----------------------------------------------------------

@dataclasses.dataclass
class _Session:
    host: Any
    dirs: tuple
    profile: str


_session: _Session | None = None


def shutdown() -> None:
    """Shut down the backend's ofxh host (the next render starts a new one). Doesn't touch another
    ofxh host."""
    global _session
    if _session is not None and ofxh.host() is _session.host:
        ofxh.shutdown()
    _session = None


def _ensure(dirs: tuple, profile: str) -> None:
    """The backend's ofxh host, for these plug-in directories and profile (ofxh has one host per
    process: a new one replaces the backend's own, but not another one)."""
    global _session
    if _session is not None and ofxh.host() is not _session.host:
        _session = None  # (shut down by someone else)
    if _session is not None and (_session.dirs, _session.profile) == (dirs, profile):
        return
    if ofxh.initialized():
        if _session is None:
            raise RuntimeError("ofxhost's in-process backend needs ofxh's host, but another one is initialized "
                               "(ofxh has one host per process): call ofxh.shutdown() first, or use "
                               "backend=\"subprocess\"")
        ofxh.shutdown()
        _session = None
    host = _Host()
    ofxh.init(host, list(dirs), use_std_paths=False, use_env_path=False, profile=profile)
    _session = _Session(host, dirs, profile)


# --- a run of hostDemo ----------------------------------------------------------------------

class _Usage(Exception):
    """A command-line error: hostDemo exits with status 1 before it runs anything (no report)."""


def _frames(frames) -> list[float]:
    if frames is None:
        return [0., CLIP_LENGTH]
    out: list[float] = []
    names: set[str] = set()
    for f in frames:
        v = float(f)
        if not math.isfinite(v) or v in out:
            raise _Usage(f'Error: invalid frames "{",".join(map(str, frames))}", expected distinct times t0,t1,...')
        out.append(v)
        n = f"{v:.12g}"
        if n in names:
            raise _Usage(f'Error: invalid frames: {n} is given twice to 12 significant digits (the output file names)')
        names.add(n)
    if not out:
        raise _Usage("Error: invalid frames \"\", expected distinct times t0,t1,...")
    return out


def _extra(extra_args: Sequence[str]) -> tuple[bool, bool, int]:
    ofx14, allow_unwritten, major = False, False, -1
    args = list(extra_args)
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--ofx14":
            ofx14 = True
        elif a == "--allow-unwritten":
            allow_unwritten = True
        elif a in ("--quiet", "--no-cache", "--no-std-paths"):
            pass
        elif a == "-m" and i + 1 < len(args):
            i += 1
            major = _parse_int(args[i])
            if major is None or major < 0:
                raise _Usage(f'Error: invalid major version "{args[i]}"')
        else:
            raise ValueError(f"extra_args {a!r}: not supported by the in-process backend (supported: --ofx14, "
                             "--allow-unwritten, -m MAJOR)")
        i += 1
    return ofx14, allow_unwritten, major


class _Run:
    """One render() call: hostDemo's run(), with its report."""

    def __init__(self, depth: str, scale, window, query: bool, allow_unwritten: bool, ofx14: bool):
        self.depth = depth
        self.scale = scale
        self.window = window
        self.query = query
        self.allow_unwritten = allow_unwritten
        self.ofx14 = ofx14
        self.n_views = 1 if ofx14 else (2 if (_VEGAS or _NUKE) else 1)
        self.instance: _Instance | None = None
        # the render state (setRenderState)
        self.rendering = False
        self.render_scale = (1., 1.)
        self.rois: dict = {}
        self.image_errors = 0
        self.warned_clips: set = set()
        self.warned_window = self.warned_large = False
        self.refused: set = set()
        self.outputs: dict = {}  # key -> (bottom-up pixels, bounds)

    @staticmethod
    def err(text: str) -> None:
        _write(2, text + "\n")

    def call(self, what: str, f, *args, **kw):
        """An action: its result, with a CallbackError (a host callback raised) as a failure."""
        try:
            return f(*args, **kw)
        except ofxh.CallbackError as e:
            self.err(f"Error: {what}: {e}\n" + "".join(traceback.format_exception(e.__cause__)).rstrip())
            return e.status if e.status is not None else ofxh.Stat.Failed

    def call2(self, what: str, f, *args):
        """An action that returns (Stat, result): (status, {}) after a CallbackError."""
        r = self.call(what, f, *args)
        return r if isinstance(r, tuple) else (r, {})


def _set_param(run: _Run, inst, assignment: str):
    """setParamFromString: (name, time) or None after an error."""
    eq = assignment.find("=")
    name = assignment if eq < 0 else assignment[:eq]
    value = "" if eq < 0 else assignment[eq + 1:]
    has_time, time = False, 0.
    at = name.find("@")
    if at >= 0:
        t = _parse_double(name[at + 1:])
        if t is None or not abs(t) < MAX_KEY_TIME:
            run.err(f'Error: invalid key time in "{assignment}", expected name@time=value[,value...]')
            return None
        has_time, time, name = True, t, name[:at]
    names = set(inst.param_names)
    if name and name not in names:
        run.err(f'Error: the plug-in has no parameter named "{name}"')
        return None
    type_ = inst.param_type(name) if name else ""
    button = type_ == "pushbutton"
    if not name or (eq < 0 and not button):
        run.err(f'Error: invalid parameter assignment "{assignment}", expected name=value[,value...], '
                "name@time=value[,value...] or a push button name")
        return None
    props = inst.param_props(name)
    ofx_type = _ofx("param_type", type_)
    numeric = type_ not in ("string", "custom", "strchoice", "bytes", "pushbutton")
    if has_time and not button and (not numeric or not props.get("OfxParamPropAnimates", (0,))[0]):
        run.err(f'Error: parameter "{name}" doesn\'t animate ({ofx_type}): no key at a time')
        return None
    if button:
        return name, time
    v: Any = None
    if type_ in ("string", "custom", "strchoice"):
        v = value
    elif type_ == "bytes":
        v = value.encode("utf-8", "surrogateescape")
    elif type_ == "boolean":
        v = _parse_bool(value)
    elif type_ == "integer":
        r = _parse_n(value, 1, _parse_int)
        v = r[0] if r else None
    elif type_ == "choice":
        r = _parse_n(value, 1, _parse_int)
        v = r[0] if r else None
        if v is None:
            opts = props.get("OfxParamPropChoiceOption", ())
            v = next((k for k, o in enumerate(opts) if o == value), None)
    elif type_ == "double":
        r = _parse_n(value, 1, _parse_double)
        v = r[0] if r else None
    elif type_ in ("double2d", "double3d", "rgb", "rgba"):
        r = _parse_n(value, {"double2d": 2, "double3d": 3, "rgb": 3, "rgba": 4}[type_], _parse_double)
        v = tuple(r) if r else None
    elif type_ in ("integer2d", "integer3d"):
        r = _parse_n(value, 2 if type_ == "integer2d" else 3, _parse_int)
        v = tuple(r) if r else None
    else:
        run.err(f'Error: cannot set parameter "{name}" of type {ofx_type}')
        return None
    if v is None:
        run.err(f'Error: invalid value "{value}" for parameter "{name}" of type {ofx_type}')
        return None
    try:
        st = int(inst.set_param(name, v, time if has_time else None))
    except (ValueError, TypeError, ofxh.CallbackError) as e:
        run.err(f'Error: could not set parameter "{name}" ({e})')
        return None
    if st != 0:
        run.err(f'Error: could not set parameter "{name}" (status {st})')
        return None
    return name, time


_NUMERIC = {"integer", "double", "boolean", "choice", "rgba", "rgb", "double2d", "integer2d", "double3d",
            "integer3d"}


def _json_value(v):
    if isinstance(v, tuple):
        return [_json_value(x) for x in v]
    if isinstance(v, float) and not math.isfinite(v):
        return None
    return v


def _param_value(inst, name: str, type_: str, time=None):
    """paramValueJson"""
    if type_ in ("bytes", "group", "page", "pushbutton", "parametric"):
        return None
    try:
        return _json_value(inst.get_param(name, time))
    except (ValueError, TypeError, ofxh.CallbackError):
        return None


def _keyed_params(inst, t: float) -> dict:
    """keyedParamValuesJson: the values at t of the parameters with keys"""
    j = {}
    for name in inst.param_names:
        if inst.param_type(name) in _NUMERIC and inst.param_keys(name):
            j[name] = _param_value(inst, name, inst.param_type(name), t)
    return j


def _stats(pixels: np.ndarray, bounds) -> dict:
    """computeStats and statsJson (files: none)"""
    h, w, nc = pixels.shape
    j: dict = {"w": bounds[2] - bounds[0], "h": bounds[3] - bounds[1], "bounds": list(bounds),
               "depth": {1: "byte", 2: "short", 4: "float"}[pixels.dtype.itemsize],
               "comps": {4: "RGBA", 3: "RGB", 1: "Alpha"}[nc]}
    v = pixels.reshape(-1, nc).astype(np.float64)
    nan = np.isnan(v)
    mins, maxs, means = [], [], []
    for c in range(nc):
        x = v[~nan[:, c], c]
        if x.size == 0:
            mins.append(None)
            maxs.append(None)
            means.append(None)
        else:
            mins.append(float(x.min()))
            maxs.append(float(x.max()))
            means.append(float(x.sum() / x.size))
    j["min"], j["max"], j["mean"] = mins, maxs, means
    j["nan"] = int(nan.any(axis=1).sum()) if nc else 0
    data = np.ascontiguousarray(pixels, dtype=pixels.dtype.newbyteorder("<"))
    j["crc32"] = f"{zlib.crc32(data.tobytes()) & 0xffffffff:08x}"
    j["files"] = []
    return j


def _render_frame(run: _Run, inst, out_clip, t: float, view: int, sequential: bool, base: str, r: dict) -> bool:
    """renderFrame: the actions of one time and view, into the report item r."""
    rs = run.scale
    par = inst.project_pixel_aspect_ratio
    r["time"] = t
    r["view"] = view
    r["renderScale"] = list(rs)
    inst.timeline_goto_time(t)  # (paramGetValue gives the values at that time)
    r["params"] = _keyed_params(inst, t)
    inputs = r["inputs"] = {}
    for name, clip in inst.clips.items():
        if not clip.is_output:
            inputs[name] = {"connected": clip.connected,
                            "regionOfDefinition": list(clip.region_of_definition(t, view)) if clip.connected else None}
    av = view if _NUKE else 0  # (the views of the actions other than render: the Nuke extension)
    st, rod = run.call2("getRegionOfDefinition", inst.get_region_of_definition_action, t, rs, av)
    rod_j = r["regionOfDefinition"] = _action(st)
    if not _ok(st):
        run.err(f"Error: get region of definition action failed at time {_g(t)} (status {int(st)})")
        return False
    rod_j["rod"] = list(rod)

    r["supportsTiles"] = {"effect": inst.supports_tiles,
                          "clips": {name: clip.supports_tiles for name, clip in inst.clips.items()}}
    output_tiles = inst.supports_tiles and out_clip.supports_tiles
    rod_px = _to_pixels(rod, rs, par)
    window = run.window
    render_window = _intersect(rod_px, tuple(window) if window else inst.project_pixel_rect(rs))
    if not output_tiles:
        if _fits(rod_px):
            if window and not run.warned_window and render_window != rod_px:
                run.warned_window = True
                run.err("Warning: the plug-in or its output clip doesn't support tiles: rendering the whole RoD, "
                        "not --window")
            render_window = rod_px
        elif not run.warned_large:
            run.warned_large = True
            run.err(f"Warning: the plug-in or its output clip doesn't support tiles, but the RoD ({_g(rod[0])},"
                    f"{_g(rod[1])},{_g(rod[2])},{_g(rod[3])}) is too large for hostDemo: rendering "
                    f"{'--window' if window else 'the project'} instead")
    if _empty(render_window):
        run.err(f"Warning: the output RoD ({_g(rod[0])},{_g(rod[1])},{_g(rod[2])},{_g(rod[3])}) at time {_g(t)} "
                f"doesn't intersect the {'render window' if window else 'project'}: nothing to render"
                + ("" if run.query else f", {base}.pfm not written"))
        r["renderWindow"] = None
        r["output"] = None
        return True
    r["renderWindow"] = list(render_window)

    # is the effect an identity? hostDemo renders it anyway
    st, idr = run.call2("isIdentity", inst.is_identity_action, t, render_window, rs, "none", av)
    id_j = r["isIdentity"] = _action(st)
    if not _ok(st):
        run.err(f"Error: is identity action failed at time {_g(t)} (status {int(st)})")
        return False
    identity = int(st) == 0
    id_j["identity"] = identity
    id_j["clip"] = idr.get("clip") if identity else None
    id_j["time"] = idr.get("time") if identity else None
    if _NUKE:
        id_j["identityView"] = idr.get("view") if identity else None

    st, ranges = run.call2("getFramesNeeded", inst.get_frames_needed_action, t)
    fn = r["framesNeeded"] = _action(st)
    if not _ok(st):
        run.err(f"Error: get frames needed action failed at time {_g(t)} (status {int(st)})")
        return False
    fn["clips"] = {c: [list(x) for x in v] for c, v in ranges.items()}

    roi = (render_window[0] * par / rs[0], render_window[1] / rs[1],
           render_window[2] * par / rs[0], render_window[3] / rs[1])
    st, rois = run.call2("getRegionsOfInterest", inst.get_regions_of_interest_action, t, roi, rs, av)
    roi_j = r["regionsOfInterest"] = _action(st)
    if not _ok(st):
        run.err(f"Error: get regions of interest action failed at time {_g(t)} (status {int(st)})")
        return False
    roi_j["regionOfInterest"] = list(roi)
    roi_j["clips"] = {c: list(v) for c, v in rois.items()}
    if run.query:
        r["render"] = None
        r["output"] = None
        return True

    # setRenderState: each render gets a new output image (ofxh's render_action)
    run.rendering, run.render_scale, run.rois = True, rs, dict(rois)
    errors = run.image_errors
    rview = view if (_NUKE or _VEGAS) else 0
    try:
        st, planes = run.call2("render", inst.render_action, t, render_window, rs, "none", sequential, False, False,
                               rview, run.n_views if _VEGAS else 1)
    finally:
        run.rendering, run.rois = False, {}
    render_j = r["render"] = _action(st)
    render_j["ok"] = int(st) == 0 and run.image_errors == errors
    render_j["imageFetchErrors"] = run.image_errors - errors
    r["output"] = None
    image = planes.get("Colour")
    if int(st) != 0 or image is None:
        run.err(f"Error: render action failed at time {_g(t)} (status {int(st)}"
                f"{'' if image is not None else ', no output image'})")
        return False
    if run.image_errors != errors:
        run.err(f"Error: image fetches failed at time {_g(t)}")
        return False
    pixels, bounds = image.pixels, tuple(image.bounds)
    if pixels.dtype.itemsize not in (1, 2, 4) or pixels.dtype == np.float16:
        run.err(f"Error: render action at time {_g(t)}: hostDemo can't write {pixels.dtype} images")
        return False
    run.outputs[base] = (pixels, bounds)
    r["output"] = _stats(pixels, bounds)
    if pixels.dtype == np.float32:
        not_written = int(np.isnan(pixels).any(axis=2).sum())
        if not_written > 0:
            n = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])
            run.err(f"{'Warning: ' if run.allow_unwritten else 'Error: '}{base}.pfm: {not_written} of {n} pixels "
                    "were not written by the plug-in (they hold NaN)")
            return run.allow_unwritten
    return True


def _plugin_json(plugin) -> dict:
    fp = plugin.descriptor().props.get("OfxPluginPropFilePath")
    return {"id": plugin.id, "versionMajor": plugin.version[0], "versionMinor": plugin.version[1],
            "label": plugin.label, "grouping": plugin.grouping,
            "filePath": (fp[0] if len(fp) == 1 else list(fp)) if fp else None}


def _context(context: str) -> str:
    if context in ("filter", "OfxImageEffectContextFilter"):
        return "OfxImageEffectContextFilter"
    if context in ("general", "OfxImageEffectContextGeneral"):
        return "OfxImageEffectContextGeneral"
    raise _Usage(f'Error: unsupported context "{context}", expected filter or general')


def _run(dirs, plugin_id, context, specs, assignments, depth, frames, scale, window, query, ofx14,
         allow_unwritten, major) -> tuple[int, dict, _Run]:
    """hostDemo's run(), after the command line: returns (exit status, report, run)."""
    run = _Run(depth, scale, window, query, allow_unwritten, ofx14)
    _ensure(dirs, "ofx14" if ofx14 else "default")
    plugin = ofxh.get_plugin(plugin_id, major)
    ret = 0
    report: dict = {"schema": "hostDemo.report", "schemaVersion": 1, "ok": False, "plugin": {"id": plugin_id},
                    "context": context,
                    "host": {"name": ofxh.host().props["OfxPropName"][0], "profile": "ofx14" if ofx14 else "default"},
                    "depth": _ofx("depth", depth), "frames": list(frames), "renderScale": list(scale),
                    "window": list(window) if window else None, "query": query, "inputs": {}, "actions": [],
                    "clipPreferences": None, "timeDomain": None, "renders": []}
    actions = report["actions"]
    renders = report["renders"]
    report_instance = None
    short_context = ofxh.short_name("context", context)
    if plugin is None:
        run.err(f'Error: plug-in "{plugin_id}"' + (f" with major version {major}" if major >= 0 else "") +
                " not found. Check that OFX_PLUGIN_PATH contains the folder of its .ofx.bundle.")
        return 1, report, run
    if short_context not in plugin.contexts:
        run.err(f'Error: plug-in "{plugin_id}" does not support the {context} context.')
        report["plugin"] = _plugin_json(plugin)
        return 1, report, run
    report["plugin"] = _plugin_json(plugin)
    inst = _Instance(run)
    run.instance = inst
    try:
        made = ofxh.create_instance(plugin, short_context, inst)
    except ofxh.CallbackError as e:
        if e.instance is not None:
            e.instance.close()
        run.err(f"Error: {e}")
        made = None
    if made is None:
        run.err(f'Error: could not create an instance of plug-in "{plugin_id}" in the {context} context.')
        return 1, report, run
    with inst:  # (kOfxActionDestroyInstance while the instance is whole, then delete it)
        clips = inst.clips
        # the input clips
        for name, spec in specs:
            clip = clips.get(name)
            if clip is None or clip.is_output:
                run.err(f'Error: the plug-in has no input clip named "{name}". Its clips are:' +
                        "".join(" " + c for c in clips))
                ret = 1
                break
            clip.spec = spec
        for name, clip in clips.items():
            if not clip.is_output:
                report["inputs"][name] = {
                    "spec": clip.spec.text, "connected": clip.connected, "optional": clip.is_optional,
                    "isMask": clip.is_mask,
                    "unmappedDepth": _ofx("depth", depth) if clip.connected else "OfxBitDepthNone",
                    "unmappedComponents": "OfxImageComponentRGBA" if clip.connected else "OfxImageComponentNone"}
        # "Any clip that is not optional will *always* be connected during a render action"
        if ret == 0 and any(not c.is_optional and not c.connected for c in clips.values()):
            run.err("Error: these clips are not optional, they can't be disconnected (-i Clip=none):" +
                    "".join(" " + n for n, c in clips.items() if not c.is_optional and not c.connected))
            ret = 1
        # the create instance action (hostDemo sets the command-line parameters after it, as a user)
        if ret == 0:
            report_instance = inst
            st = run.call("createInstance", inst.create_instance_action)
            actions.append(_named("createInstance", _action(st)))
            if not _ok(st):
                run.err(f"Error: create instance action failed (status {int(st)})")
                ret = 1
        if ret == 0:
            ok = int(run.call("getClipPreferences", inst.get_clip_preferences_action)) == 0
            actions.append({"action": "getClipPreferences", "ok": ok, "status": None})
            if not ok:
                run.err("Error: get clip preferences action failed")
                ret = 1
        # the parameters, each one followed by instanceChanged (at its time), between begin and end
        for assignment in assignments:
            if ret != 0:
                break
            inst.timeline_goto_time(0.)
            r = _set_param(run, inst, assignment)
            if r is None:
                ret = 1
                break
            name, time = r
            inst.timeline_goto_time(time)
            sts = (run.call("beginInstanceChanged", inst.begin_instance_changed_action, "user"),
                   run.call("instanceChanged", inst.param_changed_action, name, "user", time, (1., 1.)),
                   run.call("endInstanceChanged", inst.end_instance_changed_action, "user"))
            inst.timeline_goto_time(0.)
            eq = assignment.find("=")
            for what, st in zip(("beginInstanceChanged", "instanceChanged", "endInstanceChanged"), sts):
                a = _named(what, _action(st))
                a.update(param=name, value=None if eq < 0 else assignment[eq + 1:], time=time)
                actions.append(a)
                if not _ok(st):
                    run.err(f"Error: {what} action failed for parameter {name} (status {int(st)})")
                    ret = 1
        # a clip preferences slave parameter changed: the clip preferences again
        if ret == 0 and inst.clip_preferences_dirty:
            ok = int(run.call("getClipPreferences", inst.get_clip_preferences_action)) == 0
            actions.append({"action": "getClipPreferences", "ok": ok, "status": None})
            if not ok:
                run.err("Error: get clip preferences action failed")
                ret = 1
        if ret == 0:
            report["clipPreferences"] = {
                "clips": {name: {"depth": _ofx("depth", c.pixel_depth), "components": _ofx("components", c.components),
                                 "pixelAspectRatio": c.aspect_ratio,
                                 "unmappedDepth": _ofx("depth", c.unmapped_bit_depth) if c.connected else
                                 "OfxBitDepthNone",
                                 "unmappedComponents": _ofx("components", c.unmapped_components) if c.connected else
                                 "OfxImageComponentNone",
                                 "connected": c.connected}
                          for name, c in clips.items()},
                "outputPremultiplication": _ofx("premult", inst.output_premultiplication),
                "outputFielding": _ofx("field", inst.output_fielding),
                "outputFrameRate": inst.output_frame_rate,
                "continuousSamples": inst.output_continuous_samples,
                "frameVarying": inst.output_frame_varying}
        if ret == 0 and context == "OfxImageEffectContextGeneral":
            st, rng = run.call2("getTimeDomain", inst.get_time_domain_action)
            td = report["timeDomain"] = _action(st)
            td["range"] = list(rng) if int(st) == 0 else None
            actions.append(_named("getTimeDomain", _action(st)))
            if not _ok(st):
                run.err(f"Error: get time domain action failed (status {int(st)})")
                ret = 1
        out_clip = clips.get("Output")
        if ret == 0 and out_clip is None:
            run.err("Error: the plug-in has no output clip")
            ret = 1
        if ret == 0:
            first, last = min(frames), max(frames)
            step, sequential = 1., True
            if len(frames) > 1:
                step = frames[1] - frames[0]
                for k in range(1, len(frames)):
                    if not (frames[k] - frames[k - 1] > 0.) or abs(frames[k] - frames[k - 1] - step) > 1e-9:
                        sequential = False
                if not sequential:
                    step = 1.
            if not query:
                st = run.call("beginSequenceRender", inst.begin_sequence_render_action, first, last, step, False,
                              scale, sequential, False, False, 0)
                actions.append(_named("beginSequenceRender", _action(st)))
                if not _ok(st):
                    run.err(f"Error: begin render action failed (status {int(st)})")
                    ret = 1
            for t in frames:
                if ret != 0:
                    break
                for view in range(run.n_views):
                    if ret != 0:
                        break
                    base = f"{OUTPUT_PREFIX}.{t:.12g}" + ("" if run.n_views == 1 else "lr"[view])
                    r = {}
                    renders.append(r)
                    if not _render_frame(run, inst, out_clip, t, view, sequential, base, r):
                        ret = 1
            if not query:
                st = run.call("endSequenceRender", inst.end_sequence_render_action, first, last, step, False,
                              scale, sequential, False, False, 0)
                actions.append(_named("endSequenceRender", _action(st)))
                if not _ok(st):
                    run.err(f"Error: end render action failed (status {int(st)})")
                    ret = 1
        # the final parameter values and states, the messages and the progress
        if report_instance is not None:
            inst.timeline_goto_time(0.)
            params = report["params"] = {}
            for name in inst.param_names:
                type_ = inst.param_type(name)
                props = inst.param_props(name)
                p = params[name] = {"type": _ofx("param_type", type_), "value": _param_value(inst, name, type_)}
                if type_ in _NUMERIC:
                    p["keys"] = [[k, _param_value(inst, name, type_, k)] for k in inst.param_keys(name)]
                p["enabled"] = bool(props.get("OfxParamPropEnabled", (1,))[0])
                p["secret"] = bool(props.get("OfxParamPropSecret", (0,))[0])
            report["messages"] = list(inst.messages)
            report["progress"] = [{"message": p["message"], "id": p["id"], "started": True, "ended": p["ended"],
                                   "updates": p["updates"], "lastValue": p["lastValue"] if p["updates"] > 0 else None}
                                  for p in inst.progress]
    report["ok"] = ret == 0
    report["exitStatus"] = ret
    return ret, report, run


def _results(run: _Run, report: dict):
    """outputs, alpha and images of a run, as the subprocess backend reads them from the files."""
    premult = (report.get("clipPreferences") or {}).get("outputPremultiplication")
    keyed = []
    for base, (pixels, bounds) in run.outputs.items():
        name = base[len(OUTPUT_PREFIX) + 1:]
        view = name[-1] if run.n_views > 1 else ""
        v = float(name[:-1] if view else name)
        key = int(v) if v.is_integer() else v
        if view:
            key = (key, view)
        keyed.append((key, pixels, bounds))
    keyed.sort(key=lambda x: x[0] if isinstance(x[0], tuple) else (x[0], ""))
    outputs, alpha, images = {}, {}, {}
    for key, pixels, bounds in keyed:
        if pixels.dtype == np.uint8:
            f = pixels.astype(np.float32) / np.float32(255)
        elif pixels.dtype == np.uint16:
            f = pixels.astype(np.float32) / np.float32(65535)
        else:
            f = pixels.astype(np.float32)
        img = np.ascontiguousarray(f[::-1])  # top-down
        outputs[key] = img
        if img.shape[2] in (1, 4):
            alpha[key] = np.ascontiguousarray(img[:, :, -1])
        images[key] = Image(img, tuple(bounds), premult)
    return outputs, alpha, images


def _cmd(plugin_id, context, specs, depth, frames, scale, window, query, extra_args, assignments) -> list[str]:
    """The equivalent hostDemo command line (for the error messages)."""
    cmd = ["ofxhost.inprocess", "-p", plugin_id, "-c", context, "-d", depth]
    for clip, spec in specs:
        cmd += ["-i", f"{clip}={spec.text}"]
    if frames is not None:
        cmd += ["--frames", ",".join(repr(float(f)) for f in frames)]
    if scale is not None:
        cmd += ["--scale", ",".join(repr(float(s)) for s in (scale if isinstance(scale, Sequence) else [scale]))]
    if window is not None:
        cmd += ["--window", ",".join(str(int(v)) for v in window)]
    if query:
        cmd += ["--query"]
    cmd += list(extra_args)
    for a in assignments:
        cmd += ["-s", a]
    return cmd


def render(bundle_paths, plugin_id: str, context: str = "filter", inputs: Mapping[str, Any] | None = None,
           params: Mapping[str, Any] | None = None, depth: str = "float", frames: Sequence[float] | None = None,
           scale: float | Sequence[float] | None = None, window: Sequence[int] | None = None, query: bool = False,
           extra_args: Sequence[str] = (), check: bool = True) -> RenderResult:
    """ofxhost.render(), in-process (see ofxhost.render() for the arguments)."""
    if depth not in DEPTHS:
        raise ValueError(f"depth {depth!r}: expected one of {DEPTHS}")
    dirs = tuple(_plugin_dirs(bundle_paths))
    assignments = []
    for name, value in (params or {}).items():
        assignments += _param_args(name, value)[1::2]
    # (the same errors as the subprocess backend's command line: ValueError for non-finite numbers)
    for f in frames or ():
        _format_value(float(f))
    if scale is not None:
        _format_value(scale)
    # (the paths relative to the current directory, which _capture() changes)
    inputs = {c: (resolve_file_spec(v) if isinstance(v, str) and v.startswith("file:") else
                  Path(v).resolve() if isinstance(v, os.PathLike) else v) for c, v in (inputs or {}).items()}
    specs: list = []
    with _capture() as cap:
        ret = 1
        report: dict = {}
        run = None
        try:
            ofx14, allow_unwritten, major = _extra(extra_args)
            ctx = _context(context)
            fr = _frames(frames)
            if scale is None:
                sc = (1., 1.)
            else:
                s = [float(scale)] * 2 if not isinstance(scale, Sequence) else [float(x) for x in scale]
                if len(s) == 1:
                    s = s * 2
                if len(s) != 2 or not all(0. < x <= 1. for x in s):
                    raise _Usage(f'Error: invalid render scale "{scale}", expected s or sx,sy in (0,1]')
                sc = (s[0], s[1])
            win = None
            if window is not None:
                w = [int(v) for v in window]
                if len(w) != 4 or w[0] >= w[2] or w[1] >= w[3] or w[2] - w[0] > 1000000 or w[3] - w[1] > 1000000 \
                        or (w[2] - w[0]) * (w[3] - w[1]) > MAX_IMAGE_PIXELS:
                    raise _Usage(f'Error: invalid render window "{",".join(map(str, w))}", expected x1,y1,x2,y2 with '
                                 "x1<x2 and y1<y2, at most 1000000 pixels wide or high and 2^28 pixels in all")
                win = tuple(w)
            seen = set()
            for clip, value in (inputs or {}).items():
                try:
                    spec = _input_spec(clip, value)
                except _SpecError as e:
                    text = value if isinstance(value, str) else str(value)
                    raise _Usage(f'Error: invalid input "{clip}={text}" ({e}), expected Clip=default|solid:r,g,b,a|'
                                 "ramp|checker:N|rod:x1,y1,x2,y2|none|file:rgb.pfm[+alpha.pfm][@x1,y1]"
                                 "[#premult|#unpremult|#opaque]") from None
                if clip in seen:
                    raise _Usage(f'Error: input clip "{clip}" given twice (-i)')
                seen.add(clip)
                spec.scale = sc  # (a file's pixels are pixels at the render scale)
                specs.append((clip, spec))
            ret, report, run = _run(dirs, plugin_id, ctx, specs, assignments, depth, fr, sc, win, query, ofx14,
                                    allow_unwritten, major)
        except _Usage as e:
            _write(2, str(e) + "\n")
            run = None
    cmd = _cmd(plugin_id, context, specs, depth, frames, scale, window, query, extra_args, assignments)
    if run is None:
        raise HostDemoError("hostDemo (in-process) failed with exit status 1", returncode=1, stdout=cap["stdout"],
                            stderr=cap["stderr"], cmd=cmd)
    if check and ret != 0:
        raise HostDemoError(f"hostDemo (in-process) failed with exit status {ret}", returncode=ret,
                            stdout=cap["stdout"], stderr=cap["stderr"], cmd=cmd)
    outputs, alpha, images = _results(run, report)
    return RenderResult(outputs=outputs, alpha=alpha, report=report, returncode=ret, stdout=cap["stdout"],
                        stderr=cap["stderr"], cmd=cmd, images=images)


def describe(bundle_paths, plugin_id: str, context: str | None = None, extra_args: Sequence[str] = ()) -> dict:
    """ofxhost.describe(), in-process: ofxh.describe() of the plug-in in all its contexts (as hostDemo
    --describe, which describes them all whatever -c)."""
    dirs = tuple(_plugin_dirs(bundle_paths))
    cmd = ["ofxhost.inprocess", "-p", plugin_id] + (["-c", context] if context else []) + ["--describe"] + \
        list(extra_args)
    d = None
    with _capture() as cap:
        try:
            ofx14, _, major = _extra(extra_args)
            if context:
                _context(context)
            _ensure(dirs, "ofx14" if ofx14 else "default")
            plugin = ofxh.get_plugin(plugin_id, major)
            if plugin is None:
                _write(2, f'Error: plug-in "{plugin_id}" not found. Check that OFX_PLUGIN_PATH contains the folder '
                          "of its .ofx.bundle.\n")
            else:
                d = ofxh.describe(plugin, None)
        except _Usage as e:
            _write(2, str(e) + "\n")
    if d is None or not d.get("ok"):
        raise HostDemoError("hostDemo --describe (in-process) failed with exit status 1", returncode=1,
                            stdout=cap["stdout"], stderr=cap["stderr"], cmd=cmd)
    return d
