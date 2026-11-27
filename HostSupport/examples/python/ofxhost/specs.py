"""hostDemo's input specs, its number parsing, and the pixels of its test patterns, without ofxh.

The specs are those of its -i option: "ramp", "file:x.pfm@10,20#premult"...

ofxhost.hostdemo parses its command line with them before it imports ofxh, as hostDemo parses its
own before it loads plug-ins; ofxhost.inprocess and ofxhost.graphhost make their input images with
them. A spec may end with a components suffix, "#rgb", "#alpha" or "#rgba" (split_components()).
"""

from __future__ import annotations

import dataclasses
import math
import os
import sys
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

import numpy as np

from . import Image, _time_name, resolve_file_spec, split_components
from .constants import (kOfxImageComponentAlpha, kOfxImageComponentRGB, kOfxImageComponentRGBA,
                        kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral)
from .pfm import merge_alpha, read_pfm
from .pixels import check_array, to_rgba
from .rects import INT_MAX, INT_MIN, MAX_IMAGE_PIXELS

__all__ = ["CLIP_LENGTH", "COMPONENTS_EXPECTED", "CONTEXTS", "HOST_COMPONENTS", "INPUT_FORMS", "SCALE_EXPECTED",
           "WINDOW_EXPECTED", "not_found", "valid_scale", "valid_window", "frame_named_twice", "Spec", "SpecError",
           "parse_double", "parse_int", "parse_bool", "parse_n", "parse_host_components", "parse_spec", "array_spec",
           "input_spec", "resolve_spec", "read_file", "pattern", "from_components"]

CLIP_LENGTH = 1.0  # (hostDemo's OFXHOSTDEMOCLIPLENGTH: the default input is black at t=0, white at t=1)
_MAX_FLOAT32 = 3.4028234663852886e38


# --- numbers, as hostDemo parses them ----------------------------------------------------------

def parse_double(s: str) -> float | None:
    """A finite decimal number, else None, as hostDemo's parseFiniteDouble.

    Only decimal notation (no "nan", "inf", hexadecimal or spaces), and not a number that strtod
    can't represent (ERANGE: an overflow, a subnormal, or an underflow to 0 such as "1e-400").
    """
    if not s or any(c not in "0123456789+-.eE" for c in s):
        return None
    try:
        v = float(s)
    except ValueError:
        return None
    if not math.isfinite(v) or (v != 0 and abs(v) < sys.float_info.min):
        return None
    mantissa = s.lower().partition("e")[0]
    if v == 0 and any(c in "123456789" for c in mantissa):
        return None  # (underflow)
    return v


def parse_int(s: str) -> int | None:
    """A decimal int (32 bits), with an optional sign, else None, as hostDemo's parseInteger."""
    digits = s[1:] if s[:1] in ("+", "-") else s
    if not digits or not (digits.isascii() and digits.isdigit()):
        return None
    v = int(s)
    return v if INT_MIN <= v <= INT_MAX else None


def parse_bool(s: str) -> bool | None:
    if s in ("1", "true", "yes", "on"):
        return True
    if s in ("0", "false", "no", "off"):
        return False
    return None


def parse_n(s: str, n: int, parse: Callable[[str], Any]) -> list | None:
    """n comma-separated values, each parsed by parse, else None."""
    v = s.split(",")
    if len(v) != n:
        return None
    r = [parse(x) for x in v]
    return None if any(x is None for x in r) else r


# --- hostDemo's checks of its options (the messages end with these) ---------------------------

# -c: the short and the OFX names of the contexts
CONTEXTS = {"filter": kOfxImageEffectContextFilter, kOfxImageEffectContextFilter: kOfxImageEffectContextFilter,
            "general": kOfxImageEffectContextGeneral, kOfxImageEffectContextGeneral: kOfxImageEffectContextGeneral}
SCALE_EXPECTED = "expected s or sx,sy in (0,1]"
# --components: the host's kOfxImageEffectPropSupportedComponents by default (hostDemoHostDescriptor.cpp)
HOST_COMPONENTS = (kOfxImageComponentRGBA, kOfxImageComponentAlpha)
_HOST_COMPONENT_NAMES = {"rgba": kOfxImageComponentRGBA, "rgb": kOfxImageComponentRGB, "alpha": kOfxImageComponentAlpha}
COMPONENTS_EXPECTED = "expected distinct rgba, rgb or alpha, comma-separated"
# -i: the forms of an input spec
INPUT_FORMS = ("Clip=default|solid:r,g,b,a|ramp|checker:N|rod:x1,y1,x2,y2|none|file:rgb.pfm[+alpha.pfm][@x1,y1]"
               "[#premult|#unpremult|#opaque]")
WINDOW_EXPECTED = ("expected x1,y1,x2,y2 with x1<x2 and y1<y2, at most 1000000 pixels wide or high and 2^28 "
                   "pixels in all")


def parse_host_components(s: str) -> tuple[str, ...] | None:
    """--components c,...: distinct rgba, rgb or alpha, as OFX names in order; None if invalid."""
    comps = tuple(_HOST_COMPONENT_NAMES.get(c) for c in s.split(","))
    return comps if None not in comps and len(set(comps)) == len(comps) else None


def not_found(plugin: str, major: int = -1) -> str:
    """hostDemo's message for a plug-in it doesn't find (major: -1 for any version)."""
    return (f'Error: plug-in "{plugin}"' + (f" with major version {major}" if major >= 0 else "") +
            " not found. Check that OFX_PLUGIN_PATH contains the folder of its .ofx.bundle.")


def valid_scale(s: Sequence[float]) -> bool:
    """--scale sx,sy: both in (0,1]."""
    return all(0. < x <= 1. for x in s)


def valid_window(w: Sequence[int] | None) -> bool:
    """--window x1,y1,x2,y2: not empty, and at most 1000000 pixels wide or high and MAX_IMAGE_PIXELS in all."""
    return bool(w) and len(w) == 4 and w[0] < w[2] and w[1] < w[3] and w[2] - w[0] <= 1000000 and \
        w[3] - w[1] <= 1000000 and (w[2] - w[0]) * (w[3] - w[1]) <= MAX_IMAGE_PIXELS


def frame_named_twice(frames: Sequence[float]) -> str | None:
    """The first time of frames that another one has to 12 significant digits (the output file names)."""
    names: set = set()
    for f in frames:
        n = _time_name(f)
        if n in names:
            return n
        names.add(n)
    return None


# --- the input specs ---------------------------------------------------------------------------

class SpecError(ValueError):
    """An input spec that hostDemo refuses, with hostDemo's message."""


@dataclasses.dataclass
class Spec:
    """What an input clip contains (hostDemo's InputSpec).

    type: default, none, ramp, checker, solid, file or rod. text: the spec, for the report.
    data (file): H x W x 4 RGBA, bottom-up, float (PF: alpha 1 or the alpha file's; Pf: v, v, v, v).
    file_x, file_y: the file's position, in pixels at the render scale. premult (file): premult,
    unpremult, opaque or None. scale: the render scale. components: what is connected to the clip
    (rgba, rgb or alpha: a "#rgb" suffix...).
    """
    type: str = "default"
    text: str = "default"
    colour: tuple = (0., 0., 0., 0.)
    rod: tuple = (0., 0., 0., 0.)
    checker: int = 16
    data: Any = None
    file_x: int = 0
    file_y: int = 0
    premult: str | None = None
    scale: tuple = (1., 1.)
    components: str = "rgba"

    def file_format(self) -> tuple:
        """A file input's pixel bounds at scale 1 (its format)."""
        h, w = self.data.shape[:2]
        return (math.floor(self.file_x / self.scale[0]), math.floor(self.file_y / self.scale[1]),
                math.ceil((self.file_x + w) / self.scale[0]), math.ceil((self.file_y + h) / self.scale[1]))

    def file_rod(self, par: float) -> tuple:
        """A file input's RoD, in canonical coordinates (par: the clip's pixel aspect ratio)."""
        h, w = self.data.shape[:2]
        return (self.file_x / self.scale[0] * par, self.file_y / self.scale[1],
                (self.file_x + w) / self.scale[0] * par, (self.file_y + h) / self.scale[1])


def read_file(path: str) -> np.ndarray:
    """hostDemo's readPFM.

    A little-endian PFM with finite values, as a bottom-up float32 array (H x W x 3, or H x W for Pf). Raises
    SpecError with hostDemo's message.
    """
    try:
        return read_pfm(path, strict=True)[::-1]
    except OSError:
        raise SpecError(f"cannot open {path}") from None
    except ValueError as e:
        raise SpecError(str(e)) from None


def _file_spec(s: str) -> Spec:
    """A "file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]" spec."""
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
        px, py = (parse_int(x1), parse_int(y1)) if comma else (None, None)
        if px is not None and py is not None:
            fx, fy = px, py
            if not (-1000000 <= fx <= 1000000 and -1000000 <= fy <= 1000000):
                raise SpecError("file position out of range")
            path = path[:at]
    alpha_path = ""
    plus = path.rfind(".pfm+")
    if plus >= 0:
        alpha_path = path[plus + 5:]
        path = path[:plus + 4]
    if not path:
        raise SpecError("no file name")
    rgb = read_file(path)
    alpha = None
    if alpha_path:
        alpha = read_file(alpha_path)
        if rgb.ndim != 3 or alpha.ndim != 2 or alpha.shape != rgb.shape[:2]:
            raise SpecError(f"{alpha_path}: the alpha file must be a Pf file of the size of a PF file")
    return Spec(type="file", text=s, data=to_rgba(merge_alpha(rgb, alpha)), file_x=fx, file_y=fy, premult=premult)


def parse_spec(s: str) -> Spec:
    """hostDemo's parseInputSpec, without the components suffix. Raises SpecError."""
    if s in ("default", "none", "ramp"):
        return Spec(type=s, text=s)
    if s.startswith("rod:"):
        v = [parse_double(x) for x in s[4:].split(",")]
        if len(v) != 4 or any(x is None for x in v) or v[2] < v[0] or v[3] < v[1]:
            raise SpecError("invalid RoD, expected rod:x1,y1,x2,y2 with x1<=x2 and y1<=y2")
        return Spec(type="rod", text=s, rod=tuple(v))
    if s.startswith("file:"):
        return _file_spec(s)
    if s.startswith("checker:"):
        n = parse_int(s[8:])
        if n is None or n < 1 or n > 1000000:
            raise SpecError("invalid checker size")
        return Spec(type="checker", text=s, checker=n)
    if s.startswith("solid:"):
        v = s[6:].split(",")
        if len(v) != 4:
            raise SpecError("solid takes 4 values")
        c = [parse_double(x) for x in v]
        if any(x is None or abs(x) > _MAX_FLOAT32 for x in c):
            raise SpecError("invalid number")
        return Spec(type="solid", text=s, colour=tuple(c))
    raise SpecError("unknown input type")


def array_spec(clip: str, value: Any) -> Spec:
    """An array or an ofxhost.Image input.

    What the subprocess backend writes to PFM files, with the same checks and errors (ValueError).
    """
    bounds = premult = None
    if isinstance(value, Image):
        value, bounds, premult = value.pixels, value.bounds, value.premult
    a, origin, p = check_array(clip, value, bounds, premult)
    h, w = a.shape[:2]
    text = f"array:{w}x{h}x{1 if a.ndim == 2 else a.shape[2]}"
    fx, fy = origin or (0, 0)
    if origin is not None:
        text += f"@{fx},{fy}"
    if p is not None:
        text += "#" + p
    return Spec(type="file", text=text, data=to_rgba(np.ascontiguousarray(a[::-1])), file_x=fx, file_y=fy, premult=p)


def input_spec(clip: str, value: Any, components: str | None = None, resolve: bool = True,
               base: str | os.PathLike | None = None) -> Spec:
    """An input of ofxhost.render().

    Args:
        clip: the clip name.
        value: an array, an ofxhost.Image, a path, or a hostDemo spec string, with an optional
            components suffix ("ramp#rgb").
        components: "rgba", "rgb" or "alpha" (render(components=...)): overrides the suffix.
        resolve: make a "file:" spec's paths absolute (False: relative to the current directory,
            as hostDemo reads them).
        base: the directory that relative paths are relative to (None: the current directory).

    Returns:
        The Spec. Raises SpecError for a bad spec string, ValueError for a bad array.
    """
    suffix = None
    if isinstance(value, os.PathLike):
        spec = parse_spec("file:" + str((Path(base if base is not None else os.getcwd()) / value).resolve()))
    elif isinstance(value, str):
        text = value
        value, suffix = split_components(value)
        spec = parse_spec(resolve_file_spec(value, base) if resolve and value.startswith("file:") else value)
        spec.text = text
    else:
        spec = array_spec(clip, value)
    spec.components = components or suffix or "rgba"
    return spec


def resolve_spec(spec: str, base: str | os.PathLike | None = None) -> str:
    """resolve_file_spec() of a "file:" spec, keeping a components suffix.

    base: the directory the paths are relative to (default: the current directory).
    """
    body, comps = split_components(spec)
    return resolve_file_spec(body, base) + ("#" + comps if comps else "")


# --- the pixels of the inputs ------------------------------------------------------------------

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


def pattern(spec: Spec, time: float, view: int, b: Sequence[int], rod_px: Sequence[int], rs: Sequence[float], *,
            vegas: bool = False) -> np.ndarray:
    """An input's pixels, as hostDemo's evalPattern at the pixel centres.

    Args:
        spec: the input.
        time, view: what the plug-in fetches.
        b: the pixel bounds of the image, at the render scale rs.
        rod_px: the input's RoD in pixels (the ramp spans its width).
        rs: the render scale (sx, sy).
        vegas: the Vegas extension's default pattern (view 0 red, view 1 cyan).

    Returns:
        H x W x 4, float64, unpremultiplied RGBA, bottom-up (may be read-only).
    """
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
    if vegas:
        out[..., 0] = v if view == 0 else 0.
        out[..., 1] = v if view == 1 else 0.
        out[..., 2] = v if view == 1 else 0.
    else:
        out[..., 0] = out[..., 1] = out[..., 2] = v
    out[..., 3] = 1.
    return out


def from_components(rgba: np.ndarray, components: str) -> np.ndarray:
    """An RGBA pattern as an input of these components gives it, as RGBA.

    RGB is opaque (alpha 1), Alpha is (a, a, a, a), as hostDemo's Pf files; RGBA unchanged.
    """
    if components == "rgb":
        out = np.array(rgba, np.float64)
        out[..., 3] = 1.
        return out
    if components == "alpha":
        return np.repeat(np.asarray(rgba, np.float64)[..., 3:4], 4, axis=2)
    return rgba


# the OpenGL backends of openfx::HostSupportOpenGL (GL::backendName()), on every platform
OPENGL_BACKENDS = ("egl-surfaceless", "egl-device", "cgl", "wgl", "glx")


def parse_gpu_renderer(text: str) -> tuple[str, int] | None:
    """A renderer as BACKEND:ID (e.g. "egl-device:0", "cgl:16940800", "glx:-1"): (backend, id).

    As hostDemo's --gpu-renderer: BACKEND is one of OPENGL_BACKENDS, ID a decimal int (optional sign).

    Returns:
        (backend, id), or None if text isn't BACKEND:ID (gpu_renderer_error() is the message).
    """
    backend, sep, renderer_id = text.rpartition(":")
    digits = renderer_id[1:] if renderer_id[:1] in ("+", "-") else renderer_id
    if not sep or backend not in OPENGL_BACKENDS or not (digits.isascii() and digits.isdigit()):
        return None
    value = int(renderer_id)
    if not INT_MIN <= value <= INT_MAX:
        return None
    return backend, value


def gpu_renderer_error(text: str) -> str:
    """The message of a --gpu-renderer that isn't BACKEND:ID (hostDemo's)."""
    return f"'{text}': expected BACKEND:ID (see --gpu-renderers)"
