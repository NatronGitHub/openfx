"""hostDemo's input specs and number parsing, without the ofxh module (ofxhost.hostdemo parses its
command line with them before it imports ofxh, as hostDemo does before it loads plug-ins); used by
ofxhost.inprocess and ofxhost.graphhost."""

from __future__ import annotations

import dataclasses
import math
import os
import sys
from pathlib import Path
from typing import Any

import numpy as np

from . import Image, _PREMULT, resolve_file_spec, split_components

INT_MIN, INT_MAX = -2**31, 2**31 - 1
EPS = 1e-6  # canonicalToPixelEnclosing
MAX_IMAGE_PIXELS = 1 << 28  # kMaxImagePixels
CLIP_LENGTH = 1.0  # OFXHOSTDEMOCLIPLENGTH: the default input is black at t=0, white at t=1

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
    components: str = "rgba"  # what is connected to the clip: rgba, rgb or alpha (a "#rgb" suffix...)


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
            px, py = (_parse_int(x1), _parse_int(y1)) if comma else (None, None)
            if px is not None and py is not None:
                fx, fy = px, py
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


def _input_spec(clip: str, value: Any, components: str | None = None, resolve: bool = True) -> _Spec:
    """An input of render(): an array, an Image, a path, or a hostDemo spec string, with an optional
    components suffix ("ramp#rgb"); components (from render(components=...)) overrides it. resolve:
    make a "file:" spec's paths absolute (False: relative to the current directory, as hostDemo)."""
    suffix = None
    if isinstance(value, os.PathLike):
        spec = _parse_spec("file:" + str(Path(value).resolve()))
    elif isinstance(value, str):
        text = value
        value, suffix = split_components(value)
        spec = _parse_spec(resolve_file_spec(value) if resolve and value.startswith("file:") else value)
        spec.text = text
    else:
        spec = _array_spec(clip, value)
    spec.components = components or suffix or "rgba"
    return spec


def _resolve(spec: str) -> str:
    """resolve_file_spec(), keeping a components suffix."""
    body, comps = split_components(spec)
    return resolve_file_spec(body) + ("#" + comps if comps else "")
