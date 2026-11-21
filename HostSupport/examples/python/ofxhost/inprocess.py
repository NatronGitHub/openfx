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
- the images aren't written to files: each fetch of an input gives the plug-in a new contiguous
  NumPy array at the clip's depth, made from the input (as hostDemo makes a new image per fetch;
  ofxh passes the array itself, without another copy), and outputs are the arrays the plug-in
  rendered into. The report's output "files" is [], and an array input's "spec" is
  "array:WxHxC[@x1,y1][#premult]";
- the report's host name is ofxh's ("net.sf.openfx.ofxh"), not hostDemo's;
- ofxh's string choice parameters refuse a value that is not one of their enums (hostDemo's accept it);
- timeout is not applied (a hang hangs the caller), and a plug-in crash kills the interpreter. Its
  last output (e.g. an assert message) is then lost with the captured stdout and stderr, which are
  unlinked temporary files: set OFXHOST_CAPTURE=0 to leave the file descriptors 1 and 2 alone;
- one host per process (ofxh): the backend keeps its ofxh host between renders with the same
  plug-in directories and profile, so the plug-ins stay loaded (hostDemo loads them in each run),
  and refuses to run if another ofxh host is initialized (RuntimeError). shutdown() releases it;
- stdout and stderr are captured at the file-descriptor level (the plug-ins' printf and
  std::cerr), for the duration of each call, which runs in a temporary current directory (as
  hostDemo's);
- extra_args: only --ofx14, --allow-unwritten, -m MAJOR, -s ASSIGNMENT, and --format ppm|pfm,
  -o PREFIX and --report FILE (see below); --quiet, --no-cache and --no-std-paths are no-ops;
  anything else raises ValueError.

Without --format or -o in extra_args, no file is written (the subprocess backend reads hostDemo's
PFM files, and gives the same arrays). With one of them, the outputs are written as hostDemo writes
them, relative to the caller's current directory: --format ppm (the default, as hostDemo's) writes
<prefix>.<t>[l|r].ppm (plain 8-bit PPM, top row first), --format pfm writes <prefix>.<t>[l|r].pfm
(PF: the RGB components, or Pf for Alpha) and <prefix>.<t>[l|r].alpha.pfm (the alpha of RGBA
images), and the report's output "files" lists them; -o defaults to "Output". --report FILE writes
the report there too (also after a failure).

Beyond hostDemo (which is frozen):
- per-input components: an input can give the plug-in an RGB or Alpha image instead of RGBA, with
  a "#rgb", "#alpha" (or "#rgba") suffix on a spec string, last ("ramp#rgb", "checker:8#alpha",
  "file:x.pfm#premult#rgb"), or render(components={"Source": "rgb"}), which also applies to arrays
  and Images. That is the clip's unmapped components (what is connected to it); the clip
  preferences then give its components (HostSupport's default is the closest one the clip
  supports, and the plug-in may ask for another), and each fetch converts the input to them: an RGB
  input is opaque (alpha 1 in an RGBA clip, premultiplication opaque), an Alpha input is the alpha
  of the pattern or file, and gives (a, a, a, a) to an RGBA clip, as hostDemo's Pf files do;
- a plug-in's worker thread (e.g. an OpenMP loop) may be the first to fetch the output image of a
  render, or ask for the output's RoD during it: during a render, the output clip's RoD at the
  render's time and view is the one computed before the render action, so no RoD action runs on
  the worker (ofxh refuses actions there, part 2). Another time or view still runs the action, and
  is refused on a worker thread.

ofxhost.hostdemo is hostDemo's whole command line on this backend, for the plug-in repositories'
test scripts (python -m ofxhost.hostdemo ARGS..., or the hostdemo-inprocess script).
"""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import json
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

from . import (COMPONENTS, DEPTHS, OUTPUT_PREFIX, HostDemoError, Image, RenderResult, _format_value, _param_args,
               _plugin_dirs, encode_pfm, split_components)
from .constants import kOfxImageEffectPropSupportedPixelDepths

__all__ = ["render", "describe", "shutdown", "import_ofxh", "OfxhNotFound"]


from ._ofxh import OfxhNotFound, import_ofxh  # noqa: E402  (where the ofxh module is found)

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


from ._specs import (_SpecError, _Spec, _array_spec, _clamp_int, _empty, _fits, _input_spec,  # noqa: E402,F401
                     _intersect, _parse_bool, _parse_double, _parse_int, _parse_n, _parse_spec, _read_pfm, _resolve,
                     _rgba, _to_pixels)

_BPC = {"byte": 1, "short": 2, "float": 4}
_NCOMPS = {"rgba": 4, "rgb": 3, "alpha": 1}

# --- stdout and stderr ----------------------------------------------------------------------

_fflush: Any = None  # the C fflush (ctypes), False if there is none, None until looked up


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


def _flush_python() -> None:
    for f in (sys.stdout, sys.stderr):
        try:
            f.flush()
        except (AttributeError, ValueError, OSError):
            pass


def capture_enabled() -> bool:
    """Is stdout and stderr captured ($OFXHOST_CAPTURE, default 1)? 0 leaves the file descriptors 1
    and 2 alone: what a crashing plug-in writes there before the interpreter dies (e.g. an assert
    message) is then not lost in an unlinked temporary file, and RenderResult.stdout and .stderr
    are empty (hostDemo's messages go to the real stderr)."""
    return os.environ.get("OFXHOST_CAPTURE", "1") != "0"


@contextlib.contextmanager
def _capture():
    """Capture the file descriptors 1 and 2 (what hostDemo's stdout and stderr would get) into
    out["stdout"] and out["stderr"] (unless $OFXHOST_CAPTURE=0), in a temporary current directory
    (as the subprocess backend runs hostDemo: the files that plug-ins write there, e.g. logs, are
    deleted)."""
    out = {"stdout": "", "stderr": ""}
    capture = capture_enabled()
    cwd = os.getcwd()
    tmp = tempfile.TemporaryDirectory(prefix="ofxhost-")
    files: dict = {}
    saved: dict = {}
    try:
        os.chdir(tmp.name)
        _flush_python()
        _flush_c()
        if capture:
            for fd in (1, 2):
                files[fd] = tempfile.TemporaryFile()
                saved[fd] = os.dup(fd)
                os.dup2(files[fd].fileno(), fd)
        yield out
    finally:
        _flush_python()
        _flush_c()
        for fd, d in saved.items():
            os.dup2(d, fd)
            os.close(d)
        for fd, name in ((1, "stdout"), (2, "stderr")):
            if fd in files:
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


def _from_components(rgba: np.ndarray, components: str) -> np.ndarray:
    """The RGBA pattern (h, w, 4) as an input of these components gives it, as RGBA: RGB is opaque
    (alpha 1), Alpha is (a, a, a, a) (hostDemo's Pf files); RGBA unchanged."""
    if components == "rgb":
        out = np.array(rgba, np.float64)
        out[..., 3] = 1.
        return out
    if components == "alpha":
        return np.repeat(np.asarray(rgba, np.float64)[..., 3:4], 4, axis=2)
    return rgba


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

    def init_properties(self, props):
        # hostDemo's depths (_BPC, DEPTHS): byte, short and float. ofxh's PyHost also lists half
        # (its images make float16 arrays), which this host's image code doesn't make. (Before the
        # OFX 1.4 restriction: it is an OFX 1.4 host property, kept by profile="ofx14".)
        props[kOfxImageEffectPropSupportedPixelDepths] = tuple(ofxh.ofx_name("depth", d) for d in DEPTHS)

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
        self.progress.append({"message": message, "id": id, "updates": 0, "lastValue": 0., "ended": False, "ends": 0})

    def progress_end(self):
        if self.progress:
            self.progress[-1]["ended"] = True
            self.progress[-1]["ends"] += 1  # (the number of progressEnd calls, as hostDemo)

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
    def unmapped_components(self):
        # (what is connected: RGBA, as hostDemo's inputs, unless the input says otherwise)
        return "rgba" if self.is_output else self.spec.components

    @property
    def premult(self):
        # the output: the clip preferences; an RGB or disconnected input is opaque; else
        # unpremultiplied (the patterns), or the premultiplication of a file
        if self.is_output:
            return self.instance.output_premultiplication
        if not self.connected or self.components == "rgb" or self.spec.components == "rgb":
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
            # during a render, at its time and view: the RoD computed before the render action (a
            # plug-in's worker thread may ask for it, e.g. by fetching the output image first, and
            # ofxh refuses actions on a worker thread); else the plug-in's RoD action
            rod = self.run.render_rod
            if self.run.rendering and rod is not None and rod[0] == time and (not _NUKE or rod[1] == view):
                return rod[2]
            return super().region_of_definition(time, view)
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
        rs = tuple(run.instance.render_scale)  # (the current action's: the image's render scale)
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
        pixels = _to_depth(_from_components(_pattern(self.spec, time, view, b, rod, rs), self.spec.components),
                           depth, comps)
        return ofxh.ImageData(pixels, b, rod=rod)

    def _checked(self, b, time, depth, comps):
        """MyImage refuses images of more than 2^28 pixels (an image fetch error): empty bounds."""
        b = (b[0], b[1], max(b[2], b[0]), max(b[3], b[1]))
        if not _fits(b, _NCOMPS[comps] * _BPC[depth]):
            # (each refused image is an error, as in hostDemo)
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
    options: tuple = ()  # (keep_loaded, use_std_paths)


_session: _Session | None = None


def shutdown() -> None:
    """Shut down the backend's ofxh host (the next render starts a new one). Doesn't touch another
    ofxh host."""
    global _session
    if _session is not None and ofxh.host() is _session.host:
        ofxh.shutdown()
    _session = None


def _ensure(dirs: tuple, profile: str, keep_loaded: bool = False, use_std_paths: bool = False) -> None:
    """The backend's ofxh host, for these plug-in directories, profile and options (ofxh has one
    host per process: a new one replaces the backend's own, but not another one). keep_loaded: the
    scanned plug-ins stay loaded (hostDemo's OFX_HOSTDEMO_KEEP_LOADED=1). use_std_paths: also the
    standard plug-in locations (hostDemo without --no-std-paths)."""
    global _session
    options = (bool(keep_loaded), bool(use_std_paths))
    if _session is not None and ofxh.host() is not _session.host:
        _session = None  # (shut down by someone else)
    if _session is not None and (_session.dirs, _session.profile, _session.options) == (dirs, profile, options):
        return
    if ofxh.initialized():
        if _session is None:
            raise RuntimeError("ofxhost's in-process backend needs ofxh's host, but another one is initialized "
                               "(ofxh has one host per process): call ofxh.shutdown() first, or use "
                               "backend=\"subprocess\"")
        ofxh.shutdown()
        _session = None
    host = _Host()
    ofxh.init(host, list(dirs), use_std_paths=use_std_paths, use_env_path=False, profile=profile,
              keep_loaded=keep_loaded)
    _session = _Session(host, dirs, profile, options)


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


@dataclasses.dataclass
class _Extra:
    """The hostDemo options of extra_args."""
    ofx14: bool = False
    allow_unwritten: bool = False
    major: int = -1
    assignments: list = dataclasses.field(default_factory=list)
    format: str | None = None  # --format: ppm or pfm (None: not given)
    prefix: str | None = None  # -o (None: not given)
    report: str | None = None  # --report


def _extra(extra_args: Sequence[str]) -> _Extra:
    """--ofx14, --allow-unwritten, -m MAJOR, -s ASSIGNMENT (hostDemo's -s, before the params'),
    --format ppm|pfm, -o PREFIX and --report FILE."""
    x = _Extra()
    args = list(extra_args)
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--ofx14":
            x.ofx14 = True
        elif a == "--allow-unwritten":
            x.allow_unwritten = True
        elif a in ("--quiet", "--no-cache", "--no-std-paths"):
            pass
        elif a in ("-m", "-s", "--format", "-o", "--report") and i + 1 < len(args):
            i += 1
            v = args[i]
            if a == "-m":
                x.major = _parse_int(v)
                if x.major is None or x.major < 0:
                    raise _Usage(f'Error: invalid major version "{v}"')
            elif a == "-s":
                x.assignments.append(v)
            elif a == "--format":
                if v not in ("ppm", "pfm"):
                    raise _Usage(f'Error: unsupported format "{v}", expected ppm or pfm')
                x.format = v
            elif a == "-o":
                x.prefix = v
            else:
                x.report = v
        else:
            raise ValueError(f"extra_args {a!r}: not supported by the in-process backend (supported: --ofx14, "
                             "--allow-unwritten, -m MAJOR, -s ASSIGNMENT, --format ppm|pfm, -o PREFIX, --report FILE)")
        i += 1
    return x


class _Run:
    """One render() call: hostDemo's run(), with its report."""

    def __init__(self, depth: str, scale, window, query: bool, allow_unwritten: bool, ofx14: bool,
                 fmt: str | None = None, prefix: str = OUTPUT_PREFIX, out_dir: str | None = None):
        self.depth = depth
        self.fmt = fmt  # the output files: "ppm", "pfm", or None (none written)
        self.prefix = prefix  # -o
        self.out_dir = out_dir  # where the relative output file names are (None: the current directory)
        self.scale = scale
        self.window = window
        self.query = query
        self.allow_unwritten = allow_unwritten
        self.ofx14 = ofx14
        self.n_views = 1 if ofx14 else (2 if (_VEGAS or _NUKE) else 1)
        self.instance: _Instance | None = None
        # the render state (setRenderState)
        self.rendering = False
        self.rois: dict = {}
        self.image_errors = 0
        self.warned_clips: set = set()
        self.warned_window = self.warned_large = False
        self.render_rod = None  # (time, view, RoD) of the output during the render (see _Clip)
        self.outputs: dict = {}  # frame or (frame, "l"|"r") -> (bottom-up pixels, bounds)

    @staticmethod
    def err(text: str) -> None:
        _write(2, text + "\n")

    def call(self, what: str, f, *args, **kw):
        """An action: its result, with a CallbackError (a host callback raised) as a failure."""
        try:
            return f(*args, **kw)
        except ofxh.CallbackError as e:
            cause = e.__cause__
            tb = "".join(traceback.format_exception(type(cause), cause, cause.__traceback__)) if cause else ""
            self.err(f"Error: {what}: {e}" + ("\n" + tb.rstrip() if tb else ""))
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


def json_safe(v):
    """The report as hostDemo writes it (hostDemoJson.h): NaN and infinities, which JSON can't
    represent, are null (everywhere: e.g. the statistics of an output with an infinite value)."""
    if isinstance(v, float):
        return v if math.isfinite(v) else None
    if isinstance(v, dict):
        return {k: json_safe(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [json_safe(x) for x in v]
    return v


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


def _stats(pixels: np.ndarray, bounds, files=()) -> dict:
    """computeStats and statsJson (files: the files written)"""
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
    j["files"] = list(files)
    return j


def _render_frame(run: _Run, inst, out_clip, t: float, view: int, sequential: bool, base: str, key, r: dict) -> bool:
    """renderFrame: the actions of one time and view, into the report item r. base: the output file
    name without its extension (hostDemo's), key: the key of RenderResult.outputs."""
    fname = base + (".ppm" if run.fmt == "ppm" else ".pfm")
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
                + ("" if run.query else f", {fname} not written"))
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
    run.rendering, run.rois = True, dict(rois)
    run.render_rod = (t, av, tuple(rod))  # (the output's RoD during the render: see _Clip)
    errors = run.image_errors
    rview = view if (_NUKE or _VEGAS) else 0
    try:
        st, planes = run.call2("render", inst.render_action, t, render_window, rs, "none", sequential, False, False,
                               rview, run.n_views if _VEGAS else 1)
    finally:
        run.rendering, run.rois, run.render_rod = False, {}, None
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
    run.outputs[key] = (pixels, bounds)
    files: list[str] = []
    if run.fmt is not None:
        files = _write_output(run, base, fname, pixels, bounds)
        if files is None:
            return False
    r["output"] = _stats(pixels, bounds, files)
    if pixels.dtype == np.float32:
        not_written = int(np.isnan(pixels).any(axis=2).sum())
        if not_written > 0:
            n = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])
            run.err(f"{'Warning: ' if run.allow_unwritten else 'Error: '}{fname}: {not_written} of {n} pixels "
                    "were not written by the plug-in (they hold NaN" + ("), written as 0" if run.fmt == "ppm" else ")"))
            return run.allow_unwritten
    return True


def _ppm(pixels: np.ndarray, bounds) -> bytes:
    """hostDemo's exportToPPM: a plain (P3) 8-bit PPM, top row first; short and float converted to 8
    bits (float clamped to [0, 1], NaN written as 0), Alpha as grey levels."""
    h, w, nc = pixels.shape
    if pixels.dtype == np.uint8:
        v = pixels.astype(np.int64)
    elif pixels.dtype == np.uint16:
        v = (pixels.astype(np.int64) * 255 + 32767) // 65535
    else:
        f = pixels.astype(np.float64)
        v = np.floor(np.where(np.isnan(f) | (f <= 0.), 0., np.minimum(f, 1.)) * 255. + 0.5).astype(np.int64)
    rgb = np.repeat(v, 3, axis=2) if nc == 1 else v[..., :3]
    depth = {1: "OfxBitDepthByte", 2: "OfxBitDepthShort", 4: "OfxBitDepthFloat"}[pixels.dtype.itemsize]
    comps = {4: "OfxImageComponentRGBA", 3: "OfxImageComponentRGB", 1: "OfxImageComponentAlpha"}[nc]
    head = (f"P3\n# {depth} {comps}, converted to 8 bits\n# bounds {bounds[0]} {bounds[1]} {bounds[2]} {bounds[3]}\n"
            f"{w} {h}\n255\n")
    rows = rgb[::-1].reshape(-1, 3)
    body = "".join(f"{a} {b} {c}\n" for a, b, c in rows.tolist())
    return (head + body).encode("ascii")


def _pfm_float(pixels: np.ndarray) -> np.ndarray:
    """getComponentFloat: byte and short divided by 255 and 65535, float unchanged (NaN included)."""
    if pixels.dtype == np.uint8:
        return pixels.astype(np.float32) / np.float32(255)
    if pixels.dtype == np.uint16:
        return pixels.astype(np.float32) / np.float32(65535)
    return pixels.astype(np.float32)


def _write_output(run: _Run, base: str, fname: str, pixels: np.ndarray, bounds) -> list[str] | None:
    """Write an output image as hostDemo does (exportToPPM, exportToPFM): the file names (as the
    report gives them), or None after an error."""
    out: list[tuple[str, bytes]] = []
    if run.fmt == "ppm":
        out.append((fname, _ppm(pixels, bounds)))
    else:
        f = _pfm_float(pixels)[::-1]  # (encode_pfm takes top-down images)
        nc = f.shape[2]
        if nc == 1:
            out.append((base + ".pfm", encode_pfm(f[:, :, 0])))
        else:
            out.append((base + ".pfm", encode_pfm(f[:, :, :3])))
            if nc == 4:
                out.append((base + ".alpha.pfm", encode_pfm(f[:, :, 3])))
    for name, data in out:
        path = os.path.join(run.out_dir, name) if run.out_dir else name
        try:
            with open(path, "wb") as fh:
                fh.write(data)
        except OSError:
            run.err(f"Error: cannot open {name} for writing")
            return None
    return [name for name, _ in out]


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
         allow_unwritten, major, fmt=None, prefix=OUTPUT_PREFIX, out_dir=None, keep_loaded=False,
         use_std_paths=False) -> tuple[int, dict, _Run]:
    """hostDemo's run(), after the command line: returns (exit status, report, run). fmt: write the
    outputs (ppm or pfm, as hostDemo --format) to prefix.<t>[l|r].<ext>, in out_dir (None: the
    current directory); None: no files."""
    run = _Run(depth, scale, window, query, allow_unwritten, ofx14, fmt, prefix, out_dir)
    _ensure(dirs, "ofx14" if ofx14 else "default", keep_loaded, use_std_paths)
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
                    "unmappedComponents": _ofx("components", clip.spec.components) if clip.connected else
                    "OfxImageComponentNone"}
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
                    base = f"{run.prefix}.{t:.12g}" + ("" if run.n_views == 1 else "lr"[view])
                    key: Any = int(t) if float(t).is_integer() else t
                    if run.n_views > 1:
                        key = (key, "lr"[view])
                    r = {}
                    renders.append(r)
                    if not _render_frame(run, inst, out_clip, t, view, sequential, base, key, r):
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
                                   "ends": p["ends"], "updates": p["updates"], "lastValue": p["lastValue"] if p["updates"] > 0 else None}
                                  for p in inst.progress]
    report["ok"] = ret == 0
    report["exitStatus"] = ret
    return ret, json_safe(report), run


def _results(run: _Run, report: dict):
    """outputs, alpha and images of a run, as the subprocess backend reads them from the files."""
    premult = (report.get("clipPreferences") or {}).get("outputPremultiplication")
    keyed = [(key, pixels, bounds) for key, (pixels, bounds) in run.outputs.items()]
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
           extra_args: Sequence[str] = (), check: bool = True,
           components: Mapping[str, str] | None = None) -> RenderResult:
    """ofxhost.render(), in-process (see ofxhost.render() for the arguments). components: clip name ->
    "rgb", "alpha" or "rgba" (or the kOfxImageComponent* names), the components of that input (its
    unmapped components; default RGBA, or the spec's "#rgb"... suffix); a clip not in inputs gets
    the default pattern."""
    if depth not in DEPTHS:
        raise ValueError(f"depth {depth!r}: expected one of {DEPTHS}")
    comps = {}
    for clip, c in (components or {}).items():
        if c not in COMPONENTS:
            raise ValueError(f"components of input {clip!r}: {c!r}, expected rgba, rgb or alpha")
        comps[clip] = COMPONENTS[c]
    dirs = tuple(_plugin_dirs(bundle_paths))
    out_dir = os.getcwd()  # (the output files and the report are relative to the caller's directory)
    assignments = []
    for name, value in (params or {}).items():
        assignments += _param_args(name, value)[1::2]
    # (the same errors as the subprocess backend's command line: ValueError for non-finite numbers)
    for f in frames or ():
        _format_value(float(f))
    if scale is not None:
        _format_value(scale)
    # (the paths relative to the current directory, which _capture() changes)
    inputs = {c: (_resolve(v) if isinstance(v, str) and v.startswith("file:") else
                  Path(v).resolve() if isinstance(v, os.PathLike) else v) for c, v in (inputs or {}).items()}
    specs: list = []
    with _capture() as cap:
        ret = 1
        report: dict = {}
        run = None
        try:
            x = _extra(extra_args)
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
            given = dict(inputs or {})
            for clip in comps:
                given.setdefault(clip, "default")  # (components alone: the default pattern)
            for clip, value in given.items():
                try:
                    spec = _input_spec(clip, value, comps.get(clip))
                except _SpecError as e:
                    text = value if isinstance(value, str) else str(value)
                    raise _Usage(f'Error: invalid input "{clip}={text}" ({e}), expected Clip=default|solid:r,g,b,a|'
                                 "ramp|checker:N|rod:x1,y1,x2,y2|none|file:rgb.pfm[+alpha.pfm][@x1,y1]"
                                 "[#premult|#unpremult|#opaque]") from None
                if clip in comps:
                    # (the report's spec says what the input is: the effective components, which
                    # override a suffix of the spec)
                    spec.text = split_components(spec.text)[0] + ("" if comps[clip] == "rgba" else "#" + comps[clip])
                if clip in seen:
                    raise _Usage(f'Error: input clip "{clip}" given twice (-i)')
                seen.add(clip)
                spec.scale = sc  # (a file's pixels are pixels at the render scale)
                specs.append((clip, spec))
            # (hostDemo's order: the -s of extra_args, then those of params)
            fmt = x.format or ("ppm" if x.prefix is not None else None)  # (hostDemo's default format: ppm)
            ret, report, run = _run(dirs, plugin_id, ctx, specs, x.assignments + assignments, depth, fr, sc, win,
                                    query, x.ofx14, x.allow_unwritten, x.major, fmt=fmt,
                                    prefix=x.prefix if x.prefix is not None else OUTPUT_PREFIX, out_dir=out_dir)
            if x.report is not None:
                report["ok"] = ret == 0
                report["exitStatus"] = ret
                try:
                    with open(os.path.join(out_dir, x.report), "w", encoding="utf-8") as f:
                        json.dump(report, f, indent=2, allow_nan=False)
                        f.write("\n")
                except (OSError, ValueError):
                    _write(2, f"Error: cannot write {x.report}\n")
                    ret = report["exitStatus"] = 1
                    report["ok"] = False
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
            x = _extra(extra_args)
            if context:
                _context(context)
            _ensure(dirs, "ofx14" if x.ofx14 else "default")
            plugin = ofxh.get_plugin(plugin_id, x.major)
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
