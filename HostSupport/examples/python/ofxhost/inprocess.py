"""The in-process backend of ofxhost.render() and ofxhost.describe().

hostDemo's test host, in Python, on the HostSupport bindings (the ofxh module).

    r = ofxhost.render(bundles, "net.sf.openfx.invertPlugin", inputs={"Source": src},
                       backend="inprocess")      # or OFXHOST_BACKEND=inprocess

It is a port of hostDemo's run() and renderFrame() (HostSupport/examples/hostDemo.cpp) and of its
clips and instance: the same actions in the same order, the same inputs, render windows, image
bounds, messages and errors, and the same JSON report, with NumPy arrays instead of image files (the
template of a host on ofxh is ofxhost.graphhost; minimal_host.py is the minimal one). README.md lists
where it differs from the subprocess backend (one ofxh host per process, the captured file
descriptors and current directory, extra_args, the output files) and what it does beyond hostDemo
(per-input components, worker threads that fetch the output image first). ofxhost.hostdemo is
hostDemo's whole command line on this backend.
"""

from __future__ import annotations

import contextlib
import dataclasses
import functools
import math
import os
import tempfile
import threading
import traceback
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, NamedTuple

import numpy as np

from . import (DEPTHS, OUTPUT_PREFIX, HostDemoError, Image, RenderResult, _format_value, _given_inputs,
               _hostdemo_options, _key_order, _output_key, _param_args, _render_components, _time_name, write_json)
from . import plugin_dirs, split_components, status, stdio
from .build import import_ofxh
from .constants import (kFnOfxImageEffectPropComponentsPresent, kFnOfxImageEffectPropMultiPlanar, kOfxBitDepthByte,
                        kOfxBitDepthFloat, kOfxBitDepthNone, kOfxBitDepthShort,
                        kOfxImageComponentAlpha, kOfxImageComponentNone, kOfxImageComponentRGB,
                        kOfxImageComponentRGBA, kOfxImageEffectContextGeneral,
                        kOfxImageEffectOutputClipName, kOfxImageEffectPropOpenGLRenderSupported,
                        kOfxImageEffectPropSupportedComponents, kOfxImageEffectPropSupportedPixelDepths,
                        kOfxParamPropAnimates,
                        kOfxParamPropChoiceOption, kOfxParamPropEnabled, kOfxParamPropSecret, kOfxPluginPropFilePath,
                        kOfxPropName)
from .pfm import alpha_path, encode_pfm, split_alpha
from .pixels import BYTES, NCOMPS, stats, to_depth, to_float
from .rects import MAX_IMAGE_PIXELS, fits, intersect, is_empty, scale_pair, to_pixels
from .specs import (CLIP_LENGTH, COMPONENTS_EXPECTED, CONTEXTS, HOST_COMPONENTS, INPUT_FORMS, SCALE_EXPECTED,
                    WINDOW_EXPECTED, Spec, SpecError, frame_named_twice, from_components, gpu_renderer_error,
                    input_spec, not_found, parse_bool, parse_double, parse_gpu_renderer, parse_host_components,
                    parse_int, parse_n, pattern, resolve_spec, valid_scale, valid_window)

__all__ = ["render", "describe", "shutdown", "session", "ensure_host", "Session", "Job", "run_job", "json_safe",
           "capture_enabled"]

ofxh = import_ofxh()

MAX_KEY_TIME = 1e15  # (hostDemo's limit of a key's time)

_VEGAS = bool(ofxh.build_config.get("OFX_EXTENSIONS_VEGAS"))
_NUKE = bool(ofxh.build_config.get("OFX_EXTENSIONS_NUKE"))


def _action(st) -> dict:
    return {"ok": status.succeeded(st), "status": status.name(st)}


def _named(action: str, result: dict) -> dict:
    return {"action": action, "ok": result["ok"], "status": result["status"]}


def _ofx(kind: str, name: str) -> str:
    return ofxh.ofx_name(kind, name)


# --- stdout and stderr ----------------------------------------------------------------------

def capture_enabled() -> bool:
    """Is stdout and stderr captured ($OFXHOST_CAPTURE, default 1)?

    0 leaves the file descriptors 1 and 2 alone: what a crashing plug-in writes there before the interpreter dies
    (e.g. an assert message) is then not lost in an unlinked temporary file, and RenderResult.stdout and .stderr are
    empty (hostDemo's messages go to the real stderr).
    """
    return os.environ.get("OFXHOST_CAPTURE", "1") != "0"


@contextlib.contextmanager
def _capture():
    """Capture stdout and stderr, in a temporary current directory, as the subprocess backend runs hostDemo.

    The file descriptors 1 and 2 (what hostDemo's stdout and stderr would get) go to out["stdout"]
    and out["stderr"] (unless $OFXHOST_CAPTURE=0). The files that plug-ins write in the current
    directory (e.g. logs) are deleted.

    The descriptors and the current directory are the process's: during the call, other threads'
    output is captured too, and their relative paths are relative to the temporary directory (render()
    and describe() run one at a time, see _one_at_a_time()).
    """
    out = {"stdout": "", "stderr": ""}
    files = {fd: tempfile.TemporaryFile() for fd in (1, 2)} if capture_enabled() else {}
    cwd = os.getcwd()
    try:
        with tempfile.TemporaryDirectory(prefix="ofxhost-") as tmp:
            os.chdir(tmp)
            try:
                with stdio.redirect({fd: f.fileno() for fd, f in files.items()}):
                    yield out
            finally:
                os.chdir(cwd)
    finally:
        for fd, name in ((1, "stdout"), (2, "stderr")):
            if fd in files:
                files[fd].seek(0)
                out[name] = files[fd].read().decode("utf-8", "replace")
                files[fd].close()


_lock = threading.RLock()


def _one_at_a_time(f):
    """Run f in one thread at a time (render(), describe() and shutdown()).

    render() and describe() change the process's current directory and file descriptors 1 and 2 until they
    return, and read the current directory first: concurrent calls would capture each other's output and restore
    what another left (stdout and stderr then write to a deleted file); shutdown() would shut the host down under
    a render. ofxh also refuses a call from another thread during an action. Reentrant only to avoid a deadlock: a
    nested call from the same thread isn't supported. ensure_host() and run_job() aren't serialized: call them
    from one thread.
    """
    @functools.wraps(f)
    def one_at_a_time(*args, **kwargs):
        with _lock:
            return f(*args, **kwargs)
    return one_at_a_time


# --- the host, the instance and the clips ---------------------------------------------------

class _Host(ofxh.Host):
    """hostDemo's Host (hostDemoHostDescriptor.cpp): its messages go to stdout, questions are answered yes."""

    def __init__(self, opengl_render: bool = False, multiplane: bool = False,
                 components: tuple = HOST_COMPONENTS):
        super().__init__()
        self.opengl_render = opengl_render
        self.multiplane = multiplane
        self.components = components

    def init_properties(self, props):
        # hostDemo --opengl-describe (an OFX 1.4 host property: also kept by profile="ofx14")
        if self.opengl_render:
            props[kOfxImageEffectPropOpenGLRenderSupported] = "true"
        # hostDemo --multiplane (Nuke extension builds): "Indicates that a host or plugin can fetch more
        # than a type of image from a clip"
        if self.multiplane:
            props[kFnOfxImageEffectPropMultiPlanar] = 1
        # hostDemo's depths (BYTES, DEPTHS): byte, short and float. ofxh's PyHost also lists half
        # (its images make float16 arrays), which this host's image code doesn't make. (Before the
        # OFX 1.4 restriction: it is an OFX 1.4 host property, kept by profile="ofx14".)
        props[kOfxImageEffectPropSupportedPixelDepths] = tuple(ofxh.ofx_name("depth", d) for d in DEPTHS)
        # hostDemo --components (an OFX 1.4 host property: also kept by profile="ofx14"); the images
        # may have any of the three components, whatever this says
        props[kOfxImageEffectPropSupportedComponents] = tuple(self.components)

    def message(self, message_type, message_id, text):
        prefix = {"log": "Log : ", "fatal": "Error : ", "error": "Error : ",
                  "question": "Question : "}.get(message_type, "Message : ")
        stdio.write(1, prefix + text + "\n")
        return ofxh.Stat.ReplyYes if message_type == "question" else ofxh.Stat.OK

    def persistent_message(self, message_type, message_id, text):
        return self.message(message_type, message_id, text)


class _Instance(ofxh.ImageEffectInstance):
    """hostDemo's MyEffectInstance (hostDemoEffectInstance.cpp).

    Records the messages and the progress, uses the input depth for the output of a generator, and reports the views
    it renders.
    """

    def __init__(self, run: _Run):
        super().__init__()
        self.run = run
        self.messages: list[dict] = []
        self.progress: list[dict] = []
        self.default_output_depth = run.job.depth
        self.view_count = run.n_views  # (the views rendered for each frame)

    def view_name(self, index):
        """hostDemo's getViewName: "main" for a single view, else "left" and "right" (index < view_count: ofxh
        refuses the others)."""
        return "main" if self.run.n_views == 1 else ("left", "right")[index]

    def new_clip_instance(self, desc, index):
        return _Clip(self.run, desc)

    def _record(self, kind, message_type, message_id, text):
        t = _ofx("message", message_type)
        stdio.write(1, f"{t} {message_id} {text}")  # (hostDemo's printf, without a newline)
        self.messages.append({"kind": kind, "type": t, "id": message_id, "text": text})
        return ofxh.Stat.OK

    def message(self, message_type, message_id, text):
        return self._record("message", message_type, message_id, text)

    def persistent_message(self, message_type, message_id, text):
        return self._record("persistentMessage", message_type, message_id, text)

    def clear_persistent_message(self):
        self.messages.append({"kind": "clearPersistentMessage", "type": "", "id": "", "text": ""})

    def progress_start(self, message, message_id):
        self.progress.append({"message": message, "id": message_id, "updates": 0, "lastValue": 0., "ends": 0})

    def progress_end(self):
        if self.progress:
            self.progress[-1]["ends"] += 1  # (the number of progressEnd calls, as hostDemo)

    def progress_update(self, t):
        if self.progress:
            self.progress[-1]["updates"] += 1
            self.progress[-1]["lastValue"] = t
        return True


class _Clip(ofxh.ClipInstance):
    """hostDemo's MyClipInstance (hostDemoClipInstance.cpp).

    The inputs are its patterns, PFM files or arrays; the output is ofxh's default image over the render window.
    """

    def __init__(self, run: _Run, desc):
        super().__init__()
        self.run = run
        self.spec = Spec()  # the input (hostDemo's -i; the default pattern if not given)
        self.planes: dict[str, Spec] = {}  # its other planes (-i Clip@Plane=spec, --multiplane), by short name

    @property
    def connected(self):
        return self.is_output or self.spec.type != "none"

    @property
    def unmapped_bit_depth(self):
        # (hostDemo's -d depth for the inputs; its default, byte, for the output)
        return "byte" if self.is_output else self.run.job.depth

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
        if not self.is_output and self.spec.type == "file":
            return self.spec.file_format()
        return (0, 0, 720, 576)

    @property
    def components_present(self):
        # --multiplane: the colour plane, then the other planes of an input (sorted, as hostDemo's map);
        # else HostSupport's (and none on a disconnected input)
        if not self.run.job.multiplane or not self.connected:
            return super().components_present
        return ["Colour", *sorted(self.planes)]

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
            return s.file_rod(self.aspect_ratio)
        return (0., 0., 768., 576.)

    def get_image(self, time, view, bounds, plane="Colour"):
        """hostDemo's fetchImage: the output covers the render window, an input the region it needs.

        An input covers bounds (or the RoI of the render, or the RoD) clipped to the RoD, or its whole
        RoD without tiles.
        """
        run = self.run
        if not self.connected:
            return None
        depth, comps = self.pixel_depth, self.components
        if depth not in BYTES or comps not in NCOMPS:
            run.err(f'Error: clip {self.name}: hostDemo can\'t make images with pixel depth "{_ofx("depth", depth)}" '
                    f'and components "{_ofx("components", comps)}"')
            run.image_errors += 1
            return None
        rs = tuple(run.instance.render_scale)  # (the current action's: the image's render scale)
        rod_c = self.region_of_definition(time, view)
        rod = to_pixels(rod_c, rs, self.aspect_ratio)
        if self.is_output:
            b = self._checked(self._output_bounds(rod, rs), time, depth, comps)
            return ofxh.ImageData(self.new_image(b), b, rod=rod)
        b = self._input_bounds(rod_c, rod, rs, bounds)
        if b is None:
            return None
        if plane != "Colour":
            # another plane of the input (ofxh asks only for those of components_present): its two
            # components are the first two of its spec's pattern, as hostDemo's
            b = self._checked(b, time, depth, "rgba", 2)
            rgba = pattern(self.planes[plane], time, view, b, rod, rs, vegas=_VEGAS)
            return ofxh.ImageData(np.ascontiguousarray(to_depth(rgba, depth, "rgba")[..., :2]), b, rod=rod)
        b = self._checked(b, time, depth, comps)
        pixels = to_depth(from_components(pattern(self.spec, time, view, b, rod, rs, vegas=_VEGAS),
                                          self.spec.components), depth, comps)
        return ofxh.ImageData(pixels, b, rod=rod)

    def _output_bounds(self, rod, rs) -> tuple:
        """The output image.

        The render window during a render, else the RoD (the RoD clipped to the project with tiles, or if it is too
        large).
        """
        inst = self.run.instance
        if self.run.rendering:
            return inst.render_window
        if not (inst.supports_tiles and self.supports_tiles) and fits(rod):
            return rod
        return intersect(rod, inst.project_pixel_rect(rs))

    def _input_bounds(self, rod_c, rod, rs, bounds) -> tuple | None:
        """An input image.

        Bounds (canonical, or None: the render's RoI, else the RoD), or the whole RoD without tiles, clipped to the
        RoD; None if that is empty.
        """
        run = self.run
        inst = run.instance
        whole = not (inst.supports_tiles and self.supports_tiles)
        too_large = whole and not fits(rod)
        if too_large:
            if self.name not in run.warned_clips:
                run.warned_clips.add(self.name)
                run.err(f"Warning: clip {self.name}: the plug-in or the clip doesn't support tiles, but the RoD "
                        f"({_rect_g(rod_c)}) is too large for hostDemo: "
                        "fetching the region of interest instead")
            whole = False
        region = rod_c
        if not whole:
            if bounds is not None:
                region = tuple(bounds)
            elif run.rendering and self.name in run.rois:
                region = run.rois[self.name]
        region = intersect(region, rod_c)
        if is_empty(region):
            return None  # nothing to fetch
        b = intersect(to_pixels(region, rs, self.aspect_ratio), rod)
        if too_large and not fits(b) and run.rendering:
            b = intersect(b, inst.render_window)
        return None if is_empty(b) else b

    def _checked(self, b, time, depth, comps, n_comps=None):
        """MyImage refuses images of more than 2^28 pixels (an image fetch error): empty bounds.

        n_comps: the number of components (None: those of comps).
        """
        b = (b[0], b[1], max(b[2], b[0]), max(b[3], b[1]))
        if not fits(b, (NCOMPS[comps] if n_comps is None else n_comps) * BYTES[depth]):
            # (each refused image is an error, as in hostDemo)
            self.run.err(f"Error: image of {b[2] - b[0]}x{b[3] - b[1]} pixels at time {_g(time)}: too large "
                         f"(more than {MAX_IMAGE_PIXELS} pixels)")
            self.run.image_errors += 1
            b = (b[0], b[1], b[0], b[1])
        return b


def _g(v: float) -> str:
    """A number as C++ ostream writes it (6 significant digits)."""
    return f"{float(v):g}"


def _rect_g(r) -> str:
    """x1,y1,x2,y2 with _g()."""
    return ",".join(_g(v) for v in r[:4])


# --- the session: ofxh's one host -----------------------------------------------------------

class HostOptions(NamedTuple):
    """The options of the backend's ofxh host besides its directories and profile (see ensure_host())."""
    keep_loaded: bool = False
    use_std_paths: bool = False
    opengl_render: bool = False
    normalised_defaults: bool = True
    multiplane: bool = False
    components: tuple = HOST_COMPONENTS
    gpu: str = ""  # "", "opengl" or "opencl"
    gpu_renderer: str = ""


@dataclasses.dataclass(frozen=True)
class Session:
    """The backend's ofxh host, and what it was initialized with."""
    host: Any
    dirs: tuple
    profile: str
    options: HostOptions = HostOptions()


_session: Session | None = None


def session() -> Session | None:
    """The backend's ofxh host, if it has one and it is still ofxh's host."""
    return _session if _session is not None and ofxh.host() is _session.host else None


@_one_at_a_time
def shutdown() -> None:
    """Shut down the backend's ofxh host (the next render starts a new one).

    Doesn't touch another ofxh host. Waits for a render() or describe() of another thread to return.
    """
    global _session
    if session() is not None:
        ofxh.shutdown()
    _session = None


class OpenGLRendererError(ValueError):
    """ensure_host(gpu_renderer=...): no OpenGL context on that renderer.

    Attributes:
        reason: why (ofxh.init()'s opengl_error, what ofxh.opengl_context() would say).
    """

    def __init__(self, gpu_renderer: str, reason: str):
        super().__init__(f"{gpu_renderer}: no OpenGL context ({reason})")
        self.reason = reason


def ensure_host(dirs: tuple, profile: str, keep_loaded: bool = False, use_std_paths: bool = False,
                opengl_render: bool = False, normalised_defaults: bool = True, multiplane: bool = False,
                components: Sequence[str] = HOST_COMPONENTS, gpu: str = "", gpu_renderer: str = "") -> None:
    """Make sure the backend's ofxh host is initialized with these settings.

    ofxh has one host per process: a new one replaces the backend's own, but not another one.

    Args:
        dirs: the plug-in directories.
        profile: "default" or "ofx14" (see ofxh.init()).
        keep_loaded: the scanned plug-ins stay loaded (hostDemo's OFX_HOSTDEMO_KEEP_LOADED=1).
        use_std_paths: also the standard plug-in locations (hostDemo without --no-std-paths).
        opengl_render: the host's kOfxImageEffectPropOpenGLRenderSupported is "true" (hostDemo
            --opengl-describe; for describing only: the host renders without OpenGL).
        normalised_defaults: False: no kOfxParamPropDefaultCoordinateSystem (hostDemo's
            --no-normalised-defaults, see ofxh.init()).
        multiplane: a multi-plane host (kFnOfxImageEffectPropMultiPlanar, hostDemo's --multiplane;
            Nuke extension builds).
        components: the host's kOfxImageEffectPropSupportedComponents, OFX names (hostDemo's
            --components).
        gpu: "opengl", an OpenGL context (hostDemo's --gpu=opengl, ofxh.init(opengl=True)): if it is
            made, the host says it supports OpenGL render, and renders with it the plug-ins that can.
            "opencl", the same with an OpenCL device (--gpu=opencl, ofxh.init(opencl=True)). "": neither.
        gpu_renderer: with gpu "opengl", the renderer of the context, "BACKEND:ID" (a renderer of
            ofxh.opengl_renderers(), see specs.parse_gpu_renderer()); "" for the default.

    Raises:
        RuntimeError: another ofxh host is initialized.
        ValueError: gpu_renderer isn't BACKEND:ID.
        OpenGLRendererError: no context can be made on gpu_renderer.
    """
    global _session
    options = HostOptions(keep_loaded=bool(keep_loaded), use_std_paths=bool(use_std_paths),
                          opengl_render=bool(opengl_render), normalised_defaults=bool(normalised_defaults),
                          multiplane=bool(multiplane), components=tuple(components), gpu=str(gpu),
                          gpu_renderer=str(gpu_renderer))
    gl_choice = {}
    if gpu == "opengl" and gpu_renderer:
        choice = parse_gpu_renderer(gpu_renderer)
        if choice is None:
            raise ValueError(gpu_renderer_error(gpu_renderer))
        backend, renderer_id = choice
        gl_choice = {"opengl_backends": [backend], "opengl_renderer": renderer_id}
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
    host = _Host(opengl_render, multiplane, options.components)
    try:
        ofxh.init(host, list(dirs), use_std_paths=use_std_paths, use_env_path=False, profile=profile,
                  keep_loaded=keep_loaded, normalised_defaults=normalised_defaults, opengl=gpu == "opengl",
                  opencl=gpu == "opencl", **gl_choice)
    except ValueError as e:
        if gl_choice and hasattr(e, "opengl_error"):
            raise OpenGLRendererError(gpu_renderer, e.opengl_error) from e
        raise
    _session = Session(host, dirs, profile, options)


def gpu_renderers() -> list[dict]:
    """The OpenGL renderers (ofxh.opengl_renderers()), each with "name": BACKEND:ID for gpu_renderer."""
    return [dict(r, name=f"{r['backend']}:{r['id']}") for r in ofxh.opengl_renderers()]


# --- a run of hostDemo ----------------------------------------------------------------------

class _Usage(Exception):
    """A command-line error: hostDemo exits with status 1 before it runs anything (no report)."""


class _Failed(Exception):
    """A step of the run failed, after its error message.

    HostDemo skips the next steps and exits with status 1, after the report.
    """


def _frames(frames) -> list[float]:
    if frames is None:
        return [0., CLIP_LENGTH]
    out: list[float] = []
    for f in frames:
        v = float(f)
        if not math.isfinite(v) or v in out:
            raise _Usage(f'Error: invalid frames "{",".join(map(str, frames))}", expected distinct times t0,t1,...')
        out.append(v)
    n = frame_named_twice(out)
    if n is not None:
        raise _Usage(f'Error: invalid frames: {n} is given twice to 12 significant digits (the output file names)')
    if not out:
        raise _Usage("Error: invalid frames \"\", expected distinct times t0,t1,...")
    return out


@dataclasses.dataclass
class _Extra:
    """The hostDemo options of extra_args."""
    ofx14: bool = False
    normalised_defaults: bool = True  # (--no-normalised-defaults: False)
    allow_unwritten: bool = False
    major: int = -1
    assignments: list = dataclasses.field(default_factory=list)
    format: str | None = None  # --format: ppm or pfm (None: not given)
    prefix: str | None = None  # -o (None: not given)
    report: str | None = None  # --report
    components: tuple = HOST_COMPONENTS  # --components


def _extra(extra_args: Sequence[str]) -> _Extra:
    """The hostDemo options that the in-process backend takes in extra_args.

    --ofx14, --no-normalised-defaults, --allow-unwritten, -m MAJOR, -s ASSIGNMENT (before the params'),
    --format ppm|pfm, -o PREFIX, --report FILE and --components C,...
    """
    x = _Extra()
    args = list(extra_args)
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--ofx14":
            x.ofx14 = True
        elif a == "--no-normalised-defaults":
            x.normalised_defaults = False
        elif a == "--allow-unwritten":
            x.allow_unwritten = True
        elif a in ("--quiet", "--no-cache", "--no-std-paths"):
            pass
        elif a in ("-m", "-s", "--format", "-o", "--report", "--components") and i + 1 < len(args):
            i += 1
            v = args[i]
            if a == "-m":
                x.major = parse_int(v)
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
            elif a == "--components":
                comps = parse_host_components(v)
                if comps is None:
                    raise _Usage(f'Error: invalid components "{v}", {COMPONENTS_EXPECTED}')
                x.components = comps
            else:
                x.report = v
        else:
            raise ValueError(f"extra_args {a!r}: not supported by the in-process backend (supported: --ofx14, "
                             "--no-normalised-defaults, --allow-unwritten, -m MAJOR, -s ASSIGNMENT, --format ppm|pfm, "
                             "-o PREFIX, --report FILE, --components C,...)")
        i += 1
    return x


@dataclasses.dataclass
class Job:
    """A run of hostDemo, its command line parsed (see ofxhost.hostdemo for the options).

    context: kOfxImageEffectContextFilter or ...General. inputs: [(clip, specs.Spec)], in order.
    assignments: hostDemo's -s values, in order. frames: the times, distinct. scale: (sx, sy).
    window: pixels at the render scale, or None. fmt: write the outputs to
    <prefix>.<t>[l|r].<fmt> ("ppm" or "pfm"), in out_dir (None: the current directory), or None:
    no files. keep_loaded, use_std_paths, normalised_defaults: of the host (see ofxh.init()).
    multiplane: a multi-plane host (hostDemo's --multiplane, Nuke extension builds); planes: the
    other planes of the inputs, [(clip, plane, specs.Spec)] (-i Clip@Plane=spec; plane: a short name,
    e.g. BackMotionVector). components: the host's supported components, OFX names (hostDemo's
    --components).
    """
    plugin_id: str
    context: str
    inputs: list
    assignments: list
    depth: str
    frames: list
    scale: tuple = (1., 1.)
    window: tuple | None = None
    query: bool = False
    ofx14: bool = False
    allow_unwritten: bool = False
    major: int = -1
    fmt: str | None = None
    prefix: str = OUTPUT_PREFIX
    out_dir: str | None = None
    keep_loaded: bool = False
    use_std_paths: bool = False
    normalised_defaults: bool = True
    multiplane: bool = False
    planes: list[tuple[str, str, Spec]] = dataclasses.field(default_factory=list)
    components: tuple = HOST_COMPONENTS
    gpu: str = ""  # hostDemo's --gpu=opengl or opencl: "opengl" or "opencl"
    gpu_renderer: str = ""  # with gpu "opengl": BACKEND:ID (not in hostDemo)


class _Run:
    """A run's state: the instance, the render state (hostDemo's setRenderState), the messages and the outputs."""

    def __init__(self, job: Job):
        self.job = job
        self.n_views = 1 if job.ofx14 else (2 if (_VEGAS or _NUKE) else 1)
        self.instance: _Instance | None = None
        self.rendering = False
        self.rois: dict = {}
        self.image_errors = 0
        self.warned_clips: set = set()
        self.warned_window = self.warned_large = False
        self.render_rod = None  # (time, view, RoD) of the output during the render (see _Clip)
        self.outputs: dict = {}  # frame or (frame, "l"|"r") -> (bottom-up pixels, bounds)

    @staticmethod
    def err(text: str) -> None:
        stdio.write(2, text + "\n")

    def call(self, what: str, f, *args, **kw):
        """An action: its result, with a CallbackError (a host callback raised) as a failure."""
        try:
            return f(*args, **kw)
        except ofxh.CallbackError as e:
            cause = e.__cause__
            tb = "".join(traceback.format_exception(type(cause), cause, cause.__traceback__)) if cause else ""
            self.err(f"Error: {what}: {e}" + ("\n" + tb.rstrip() if tb else ""))
            return e.status if e.status is not None else ofxh.Stat.Failed

    def call2(self, what: str, f, *args, **kw):
        """An action that returns (Stat, result): (status, {}) after a CallbackError."""
        r = self.call(what, f, *args, **kw)
        return r if isinstance(r, tuple) else (r, {})

    def check(self, st, message: str) -> None:
        """Raise _Failed after the error message if the action failed."""
        if not status.succeeded(st):
            self.err(f"Error: {message} (status {int(st)})")
            raise _Failed


def _set_param(run: _Run, inst, assignment: str):
    """setParamFromString: (name, time) or None after an error."""
    eq = assignment.find("=")
    name = assignment if eq < 0 else assignment[:eq]
    value = "" if eq < 0 else assignment[eq + 1:]
    has_time, time = False, 0.
    at = name.find("@")
    if at >= 0:
        t = parse_double(name[at + 1:])
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
    if has_time and not button and (not numeric or not props.get(kOfxParamPropAnimates, (0,))[0]):
        run.err(f'Error: parameter "{name}" doesn\'t animate ({ofx_type}): no key at a time')
        return None
    if button:
        return name, time
    v = _param_from_string(type_, value, props)
    if v is _NOT_SETTABLE:
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
    if st != ofxh.Stat.OK:
        run.err(f'Error: could not set parameter "{name}" (status {st})')
        return None
    return name, time


_NOT_SETTABLE = object()
_DIMENSIONS = {"double2d": 2, "double3d": 3, "rgb": 3, "rgba": 4, "integer2d": 2, "integer3d": 3}


def _param_from_string(type_: str, value: str, props) -> Any:
    """A parameter value from the command line, as hostDemo parses it.

    The value, None if it is invalid, or _NOT_SETTABLE for a type that the command line can't set.
    """
    if type_ in ("string", "custom", "strchoice"):
        return value
    if type_ == "bytes":
        return value.encode("utf-8", "surrogateescape")
    if type_ == "boolean":
        return parse_bool(value)
    if type_ in ("integer", "double", "choice"):
        r = parse_n(value, 1, parse_double if type_ == "double" else parse_int)
        v = r[0] if r else None
        if v is None and type_ == "choice":  # (an option's label)
            v = next((k for k, o in enumerate(props.get(kOfxParamPropChoiceOption, ())) if o == value), None)
        return v
    if type_ in _DIMENSIONS:
        r = parse_n(value, _DIMENSIONS[type_], parse_int if type_.startswith("integer") else parse_double)
        return tuple(r) if r else None
    return _NOT_SETTABLE


_NUMERIC = {"integer", "double", "boolean", "choice", "rgba", "rgb", "double2d", "integer2d", "double3d",
            "integer3d"}


def json_safe(v):
    """The report as hostDemo writes it (hostDemoJson.h).

    NaN and infinities, which JSON can't represent, are null (everywhere: e.g. the statistics of an output with an
    infinite value), and tuples are lists.
    """
    if isinstance(v, float):
        return v if math.isfinite(v) else None
    if isinstance(v, dict):
        return {k: json_safe(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [json_safe(x) for x in v]
    return v


def _param_value(inst, name: str, type_: str, time=None):
    """paramValueJson"""
    if type_ in ("bytes", "group", "page", "pushbutton", "parametric"):
        return None
    try:
        return json_safe(inst.get_param(name, time))
    except (ValueError, TypeError, ofxh.CallbackError):
        return None


def _keyed_params(inst, t: float) -> dict:
    """keyedParamValuesJson: the values at t of the parameters with keys"""
    j = {}
    for name in inst.param_names:
        if inst.param_type(name) in _NUMERIC and inst.param_keys(name):
            j[name] = _param_value(inst, name, inst.param_type(name), t)
    return j


# --- a frame: renderFrame -------------------------------------------------------------------

def _render_frame(run: _Run, inst, out_clip, t: float, view: int, sequential: bool, base: str, key, r: dict) -> None:
    """renderFrame: the actions of one time and view, into the report item r.

    base: the output file name without its extension (hostDemo's), key: the key of RenderResult.outputs. Raises
    _Failed.
    """
    fname = base + (".ppm" if run.job.fmt == "ppm" else ".pfm")
    rs = run.job.scale
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
    r["regionOfDefinition"] = _action(st)
    run.check(st, f"get region of definition action failed at time {_g(t)}")
    r["regionOfDefinition"]["rod"] = list(rod)
    r["supportsTiles"] = {"effect": inst.supports_tiles,
                          "clips": {name: clip.supports_tiles for name, clip in inst.clips.items()}}
    render_window = _render_window(run, inst, out_clip, rod, t, fname)
    if render_window is None:
        r["renderWindow"] = None
        r["output"] = None
        return
    r["renderWindow"] = list(render_window)
    rois = _frame_queries(run, inst, t, av, render_window, r)
    if run.job.query:
        r["render"] = None
        r["output"] = None
        return
    pixels, bounds = _frame_render(run, inst, t, view, av, render_window, rod, rois, sequential, r)
    run.outputs[key] = (pixels, bounds)
    files: list[str] = []
    if run.job.fmt is not None:
        files = _write_output(run, base, fname, pixels, bounds)
    r["output"] = stats(pixels, bounds, files)
    if pixels.dtype == np.float32:
        not_written = int(np.isnan(pixels).any(axis=2).sum())
        if not_written > 0:
            n = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])
            allow = run.job.allow_unwritten
            run.err(f"{'Warning: ' if allow else 'Error: '}{fname}: {not_written} of {n} pixels were not written by "
                    "the plug-in (they hold NaN" + ("), written as 0" if run.job.fmt == "ppm" else ")"))
            if not allow:
                raise _Failed


def _render_window(run: _Run, inst, out_clip, rod, t: float, fname: str) -> tuple | None:
    """The render window.

    --window (or the project) clipped to the RoD; the whole RoD without tiles, unless it is too large. None (after a
    warning) if it is empty.
    """
    rs = run.job.scale
    window = run.job.window
    rod_px = to_pixels(rod, rs, inst.project_pixel_aspect_ratio)
    render_window = intersect(rod_px, tuple(window) if window else inst.project_pixel_rect(rs))
    if not (inst.supports_tiles and out_clip.supports_tiles):
        if fits(rod_px):
            if window and not run.warned_window and render_window != rod_px:
                run.warned_window = True
                run.err("Warning: the plug-in or its output clip doesn't support tiles: rendering the whole RoD, "
                        "not --window")
            render_window = rod_px
        elif not run.warned_large:
            run.warned_large = True
            run.err(f"Warning: the plug-in or its output clip doesn't support tiles, but the RoD ({_rect_g(rod)}) "
                    "is too large for hostDemo: rendering "
                    f"{'--window' if window else 'the project'} instead")
    if is_empty(render_window):
        run.err(f"Warning: the output RoD ({_rect_g(rod)}) at time {_g(t)} "
                f"doesn't intersect the {'render window' if window else 'project'}: nothing to render"
                + ("" if run.job.query else f", {fname} not written"))
        return None
    return render_window


def _frame_queries(run: _Run, inst, t: float, av: int, render_window, r: dict) -> dict:
    """isIdentity (hostDemo renders an identity anyway), getFramesNeeded and getRegionsOfInterest, into r.

    The regions of interest. Raises _Failed.
    """
    rs = run.job.scale
    par = inst.project_pixel_aspect_ratio
    st, idr = run.call2("isIdentity", inst.is_identity_action, t, render_window, rs, "none", av)
    id_j = r["isIdentity"] = _action(st)
    run.check(st, f"is identity action failed at time {_g(t)}")
    identity = int(st) == ofxh.Stat.OK
    id_j["identity"] = identity
    id_j["clip"] = idr.get("clip") if identity else None
    id_j["time"] = idr.get("time") if identity else None
    if _NUKE:
        id_j["identityView"] = idr.get("view") if identity else None
    if run.job.multiplane:
        id_j["identityPlane"] = _ofx("plane", idr.get("plane")) if identity else None
        _clip_components(run, inst, t, av, r)
    st, ranges = run.call2("getFramesNeeded", inst.get_frames_needed_action, t)
    fn = r["framesNeeded"] = _action(st)
    run.check(st, f"get frames needed action failed at time {_g(t)}")
    # (sorted by clip name: HostSupport's map is ordered by the clips' addresses, which vary between runs)
    fn["clips"] = {c: [list(x) for x in v] for c, v in sorted(ranges.items())}
    roi = (render_window[0] * par / rs[0], render_window[1] / rs[1],
           render_window[2] * par / rs[0], render_window[3] / rs[1])
    st, rois = run.call2("getRegionsOfInterest", inst.get_regions_of_interest_action, t, roi, rs, av)
    roi_j = r["regionsOfInterest"] = _action(st)
    run.check(st, f"get regions of interest action failed at time {_g(t)}")
    roi_j["regionOfInterest"] = list(roi)
    roi_j["clips"] = {c: list(v) for c, v in sorted(rois.items())}
    return rois


def _clip_components(run: _Run, inst, t: float, view: int, r: dict) -> None:
    """--multiplane: the planes the plug-in needs and produces, and its pass-through clip, into r.

    (hostDemo renders the colour plane only.) Raises _Failed.
    """
    st, cc = run.call2("getClipComponents", inst.get_clip_components_action, t, view)
    j = r["clipComponents"] = _action(st)
    run.check(st, f"get clip components action failed at time {_g(t)}")
    ok = int(st) == ofxh.Stat.OK
    j["clips"] = {c: list(v) for c, v in sorted(cc.get("clips", {}).items())}  # (by clip name, as hostDemo)
    j["passThroughClip"] = cc.get("pass_through_clip")
    j["passThroughTime"] = cc.get("pass_through_time") if ok else None
    j["passThroughView"] = cc.get("pass_through_view") if ok else None


def _frame_render(run: _Run, inst, t: float, view: int, av: int, render_window, rod, rois: dict, sequential: bool,
                  r: dict) -> tuple:
    """The render action, into r (setRenderState: each render gets a new output image).

    Returns:
        (the output pixels, their bounds).

    Raises:
        _Failed: the render failed.
    """
    run.rendering, run.rois = True, dict(rois)
    run.render_rod = (t, av, tuple(rod))  # (the output's RoD during the render: see _Clip)
    errors = run.image_errors

    def render(opengl: bool, opencl: str = ""):
        return run.call2("render", inst.render_action, t, render_window, run.job.scale, "none",
                         sequential=sequential, interactive_render=False, draft=False,
                         view=view if (_NUKE or _VEGAS) else 0, n_views=run.n_views if _VEGAS else 1,
                         opengl=opengl, opencl=opencl)

    used_gl = used_cl = cpu_retry = False
    try:
        # hostDemo's renderFrame() with --gpu=opengl: OpenGL if the plug-in and the host can, the CPU if
        # OpenGL failed and the plug-in can render on the CPU (see hostDemo's withOpenGL()); the same
        # with --gpu=opencl (withOpenCL())
        if run.job.gpu == "opencl":
            mode = inst.render_mode(opencl=True)
        else:
            mode = inst.render_mode() if run.job.gpu else "cpu"
        if mode in ("opencl-buffers", "opencl-images"):
            st, planes = render(False, mode[len("opencl-"):])
            if inst.opencl_error:
                run.err(f"Error: OpenCL render: {inst.opencl_error}")
            used_cl = inst.opencl_result == "ok"
            # (ofxh's cpuRetryAllowed(): "retry"; refused by the session: if the plug-in can render on the CPU)
            if inst.opencl_result == "retry" or (inst.opencl_result == "not_sent" and inst.can_render_on_cpu()):
                cpu_retry = True
                st, planes = render(False)
            elif not used_cl and int(st) == ofxh.Stat.OK:
                st = ofxh.Stat.Failed
        elif mode == "none" and run.job.gpu == "opencl":
            run.err('Error: the plug-in can\'t render on the CPU (kOfxImageEffectPropCPURenderSupported "false"), '
                    "and hostDemo has no OpenCL device it can use")
            st, planes = ofxh.Stat.Failed, {}
        elif mode == "opengl":
            st, planes = render(True)
            if inst.opengl_error:
                run.err(f"Error: OpenGL render: {inst.opengl_error}")
            result = inst.opengl_result
            gl_failed = result != "ok"
            used_gl = not gl_failed
            # (ofxh's cpuRetryAllowed(): "retry"; not attached: if the plug-in can render on the CPU)
            if result == "retry" or (result == "not_sent" and inst.render_mode(False) == "cpu"):
                cpu_retry = True
                st, planes = render(False)
            elif gl_failed and int(st) == ofxh.Stat.OK:
                st = ofxh.Stat.Failed
        elif mode == "none":
            run.err('Error: the plug-in needs OpenGL (kOfxImageEffectPropOpenGLRenderSupported "needed"), '
                    "and hostDemo has no OpenGL context for it")
            st, planes = ofxh.Stat.Failed, {}
        else:
            st, planes = render(False)
    finally:
        run.rendering, run.rois, run.render_rod = False, {}, None
    render_j = r["render"] = _action(st)
    render_j["ok"] = int(st) == ofxh.Stat.OK and run.image_errors == errors
    render_j["imageFetchErrors"] = run.image_errors - errors
    if run.job.gpu == "opengl":
        render_j["openGL"] = used_gl
        render_j["cpuRetry"] = cpu_retry
    elif run.job.gpu == "opencl":
        render_j["openCL"] = used_cl
        render_j["cpuRetry"] = cpu_retry
    r["output"] = None
    image = planes.get("Colour")
    if int(st) != ofxh.Stat.OK or image is None:
        run.err(f"Error: render action failed at time {_g(t)} (status {int(st)}"
                f"{'' if image is not None else ', no output image'})")
        raise _Failed
    if run.image_errors != errors:
        run.err(f"Error: image fetches failed at time {_g(t)}")
        raise _Failed
    pixels, bounds = image.pixels, tuple(image.bounds)
    if pixels.dtype.itemsize not in (1, 2, 4) or pixels.dtype == np.float16:
        run.err(f"Error: render action at time {_g(t)}: hostDemo can't write {pixels.dtype} images")
        raise _Failed
    return pixels, bounds


_DEPTH_NAMES = {1: kOfxBitDepthByte, 2: kOfxBitDepthShort, 4: kOfxBitDepthFloat}
_COMPONENT_NAMES = {4: kOfxImageComponentRGBA, 3: kOfxImageComponentRGB, 1: kOfxImageComponentAlpha}


def _ppm(pixels: np.ndarray, bounds) -> bytes:
    """hostDemo's exportToPPM.

    A plain (P3) 8-bit PPM, top row first; short and float converted to 8 bits (float clamped to [0, 1], NaN written
    as 0), Alpha as grey levels.
    """
    h, w, nc = pixels.shape
    if pixels.dtype == np.uint8:
        v = pixels.astype(np.int64)
    elif pixels.dtype == np.uint16:
        v = (pixels.astype(np.int64) * 255 + 32767) // 65535
    else:
        f = pixels.astype(np.float64)
        v = np.floor(np.where(np.isnan(f) | (f <= 0.), 0., np.minimum(f, 1.)) * 255. + 0.5).astype(np.int64)
    rgb = np.repeat(v, 3, axis=2) if nc == 1 else v[..., :3]
    head = (f"P3\n# {_DEPTH_NAMES[pixels.dtype.itemsize]} {_COMPONENT_NAMES[nc]}, converted to 8 bits\n"
            f"# bounds {bounds[0]} {bounds[1]} {bounds[2]} {bounds[3]}\n{w} {h}\n255\n")
    rows = rgb[::-1].reshape(-1, 3)
    body = "".join(f"{a} {b} {c}\n" for a, b, c in rows.tolist())
    return (head + body).encode("ascii")


def _write_output(run: _Run, base: str, fname: str, pixels: np.ndarray, bounds) -> list[str]:
    """Write an output image as hostDemo does (exportToPPM, exportToPFM): the file names (as the report gives them).

    Raises _Failed after an error.
    """
    out: list[tuple[str, bytes]] = []
    if run.job.fmt == "ppm":
        out.append((fname, _ppm(pixels, bounds)))
    else:
        colour, alpha = split_alpha(to_float(pixels)[::-1])  # (encode_pfm takes top-down images)
        out.append((base + ".pfm", encode_pfm(colour)))
        if alpha is not None:
            out.append((alpha_path(base + ".pfm"), encode_pfm(alpha)))
    for name, data in out:
        path = os.path.join(run.job.out_dir, name) if run.job.out_dir else name
        try:
            with open(path, "wb") as fh:
                fh.write(data)
        except OSError:
            run.err(f"Error: cannot open {name} for writing")
            raise _Failed from None
    return [name for name, _ in out]


# --- the whole run: hostDemo's run() --------------------------------------------------------

def _plugin_json(plugin) -> dict:
    fp = plugin.descriptor().props.get(kOfxPluginPropFilePath)
    return {"id": plugin.id, "versionMajor": plugin.version[0], "versionMinor": plugin.version[1],
            "label": plugin.label, "grouping": plugin.grouping,
            "filePath": (fp[0] if len(fp) == 1 else list(fp)) if fp else None}


def _context(context: str) -> str:
    c = CONTEXTS.get(context)
    if c is None:
        raise _Usage(f'Error: unsupported context "{context}", expected filter or general')
    return c


def run_job(dirs: tuple, job: Job) -> tuple[int, dict, dict]:
    """hostDemo's run(), after the command line.

    Args:
        dirs: the plug-in directories.
        job: what to run.

    Returns:
        (the exit status, the report, the outputs {frame or (frame, "l"|"r"): (bottom-up pixels,
        bounds)}).

    Raises:
        OpenGLRendererError: job.gpu_renderer gives no OpenGL context (see ensure_host()).
    """
    run = _Run(job)
    profile = "ofx14" if job.ofx14 else "default"
    ensure_host(dirs, profile, job.keep_loaded, job.use_std_paths, normalised_defaults=job.normalised_defaults,
                multiplane=job.multiplane, components=job.components, gpu=job.gpu, gpu_renderer=job.gpu_renderer)
    # (with job.gpu_renderer, ensure_host() raised OpenGLRendererError if that renderer gave no context)
    gl_error = (ofxh.opengl_context() or {}).get("error") if job.gpu == "opengl" else None
    if gl_error:
        run.err(f"Warning: --gpu=opengl: no OpenGL context ({gl_error}): rendering on the CPU")
    cl_error = (ofxh.opencl_device() or {}).get("error") if job.gpu == "opencl" else None
    if cl_error:
        run.err(f"Warning: --gpu=opencl: no OpenCL device ({cl_error}): rendering on the CPU")
    plugin = ofxh.get_plugin(job.plugin_id, job.major)
    report: dict = {"schema": "hostDemo.report", "schemaVersion": 1, "ok": False, "plugin": {"id": job.plugin_id},
                    "context": job.context, "host": {"name": ofxh.host().props[kOfxPropName][0], "profile": profile},
                    "depth": _ofx("depth", job.depth), "frames": list(job.frames), "renderScale": list(job.scale),
                    "window": list(job.window) if job.window else None, "query": job.query, "inputs": {},
                    "actions": [], "clipPreferences": None, "timeDomain": None, "renders": []}
    short_context = ofxh.short_name("context", job.context)
    if plugin is None:
        run.err(not_found(job.plugin_id, job.major))
        return _finish(1, report, run)
    report["plugin"] = _plugin_json(plugin)
    if short_context not in plugin.contexts:
        run.err(f'Error: plug-in "{job.plugin_id}" does not support the {job.context} context.')
        return _finish(1, report, run)
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
        run.err(f'Error: could not create an instance of plug-in "{job.plugin_id}" in the {job.context} context.')
        return _finish(1, report, run)
    with inst:  # (kOfxActionDestroyInstance while the instance is whole, then delete it)
        ret = 0
        created = False
        try:
            _connect_inputs(run, inst, report)
            created = True
            _create(run, inst, report)
            _render_sequence(run, inst, report)
        except _Failed:
            ret = 1
        if created:
            _final_report(inst, report)
    return _finish(ret, report, run)


def _finish(ret: int, report: dict, run: _Run) -> tuple[int, dict, dict]:
    """run_job()'s result: the report gets ok and exitStatus, as hostDemo writes them in every report."""
    report["ok"] = ret == 0
    report["exitStatus"] = ret
    return ret, json_safe(report), run.outputs


def _connect_inputs(run: _Run, inst, report: dict) -> None:
    """The input clips (hostDemo's -i), into the report.

    Raises _Failed for an unknown clip, or a disconnected one that isn't optional.
    """
    clips = inst.clips
    failed = False
    for name, plane, spec in [(name, None, spec) for name, spec in run.job.inputs] + list(run.job.planes):
        clip = clips.get(name)
        if clip is None or clip.is_output:
            run.err(f'Error: the plug-in has no input clip named "{name}". Its clips are:' +
                    "".join(" " + c for c in clips))
            failed = True
            break
        if plane is None:
            clip.spec = spec
        else:
            clip.planes[plane] = spec
    for name, clip in clips.items():
        if not clip.is_output:
            j = report["inputs"][name] = {
                "spec": clip.spec.text, "connected": clip.connected, "optional": clip.is_optional,
                "isMask": clip.is_mask,
                "unmappedDepth": _ofx("depth", run.job.depth) if clip.connected else kOfxBitDepthNone,
                "unmappedComponents": _ofx("components", clip.spec.components) if clip.connected else
                kOfxImageComponentNone}
            if run.job.multiplane:
                # (its -i Clip@Plane=spec, by plane, as hostDemo's map)
                j["planes"] = {p: spec.text for c, p, spec in sorted(run.job.planes, key=lambda x: x[1]) if c == name}
    if failed:
        raise _Failed
    # "Any clip that is not optional will *always* be connected during a render action"
    if any(not c.is_optional and not c.connected for c in clips.values()):
        run.err("Error: these clips are not optional, they can't be disconnected (-i Clip=none):" +
                "".join(" " + n for n, c in clips.items() if not c.is_optional and not c.connected))
        raise _Failed


def _clip_preferences(run: _Run, inst, report: dict) -> None:
    ok = int(run.call("getClipPreferences", inst.get_clip_preferences_action)) == ofxh.Stat.OK
    report["actions"].append({"action": "getClipPreferences", "ok": ok, "status": None})
    if not ok:
        run.err("Error: get clip preferences action failed")
        raise _Failed


def _create(run: _Run, inst, report: dict) -> None:
    """The create instance action, the clip preferences, the command line's parameters, the time domain.

    hostDemo sets the parameters after the create instance action, as a user. Raises _Failed.
    """
    actions = report["actions"]
    st = run.call("createInstance", inst.create_instance_action)
    actions.append(_named("createInstance", _action(st)))
    run.check(st, "create instance action failed")
    _clip_preferences(run, inst, report)
    for assignment in run.job.assignments:
        _assign(run, inst, assignment, actions)
    # a clip preferences slave parameter changed: the clip preferences again
    if inst.clip_preferences_dirty:
        _clip_preferences(run, inst, report)
    report["clipPreferences"] = {
        "clips": {name: {"depth": _ofx("depth", c.pixel_depth), "components": _ofx("components", c.components),
                         "pixelAspectRatio": c.aspect_ratio,
                         "unmappedDepth": _ofx("depth", c.unmapped_bit_depth) if c.connected else kOfxBitDepthNone,
                         "unmappedComponents": _ofx("components", c.unmapped_components) if c.connected else
                         kOfxImageComponentNone,
                         "connected": c.connected}
                  for name, c in inst.clips.items()},
        "outputPremultiplication": _ofx("premult", inst.output_premultiplication),
        "outputFielding": _ofx("field", inst.output_fielding),
        "outputFrameRate": inst.output_frame_rate,
        "continuousSamples": inst.output_continuous_samples,
        "frameVarying": inst.output_frame_varying}
    if run.job.multiplane:
        # the planes present, as a plug-in reads them
        for name, c in inst.clips.items():
            report["clipPreferences"]["clips"][name]["planesPresent"] = \
                list(c.props[kFnOfxImageEffectPropComponentsPresent])
    if run.job.context == kOfxImageEffectContextGeneral:
        st, rng = run.call2("getTimeDomain", inst.get_time_domain_action)
        td = report["timeDomain"] = _action(st)
        td["range"] = list(rng) if int(st) == ofxh.Stat.OK else None
        actions.append(_named("getTimeDomain", _action(st)))
        run.check(st, "get time domain action failed")


def _assign(run: _Run, inst, assignment: str, actions: list) -> None:
    """A parameter of the command line, followed by instanceChanged (at its time), between begin and end.

    Raises _Failed.
    """
    inst.timeline_goto_time(0.)
    r = _set_param(run, inst, assignment)
    if r is None:
        raise _Failed
    name, time = r
    inst.timeline_goto_time(time)
    sts = (run.call("beginInstanceChanged", inst.begin_instance_changed_action, "user"),
           run.call("instanceChanged", inst.param_changed_action, name, "user", time, (1., 1.)),
           run.call("endInstanceChanged", inst.end_instance_changed_action, "user"))
    inst.timeline_goto_time(0.)
    eq = assignment.find("=")
    failed = False
    for what, st in zip(("beginInstanceChanged", "instanceChanged", "endInstanceChanged"), sts):
        a = _named(what, _action(st))
        a.update(param=name, value=None if eq < 0 else assignment[eq + 1:], time=time)
        actions.append(a)
        if not status.succeeded(st):
            run.err(f"Error: {what} action failed for parameter {name} (status {int(st)})")
            failed = True
    if failed:
        raise _Failed


def _render_sequence(run: _Run, inst, report: dict) -> None:
    """beginSequenceRender, each frame and view, endSequenceRender (also after a failure, if the begin was called).

    Raises _Failed.
    """
    job = run.job
    out_clip = inst.clips.get(kOfxImageEffectOutputClipName)
    if out_clip is None:
        run.err("Error: the plug-in has no output clip")
        raise _Failed
    frames = job.frames
    first, last = min(frames), max(frames)
    step, sequential = 1., True
    if len(frames) > 1:
        step = frames[1] - frames[0]
        for k in range(1, len(frames)):
            if not (frames[k] - frames[k - 1] > 0.) or abs(frames[k] - frames[k - 1] - step) > 1e-9:
                sequential = False
        if not sequential:
            step = 1.
    seq = dict(step=step, interactive=False, render_scale=job.scale, sequential=sequential, interactive_render=False,
               draft=False, view=0)
    failed = False

    seq_gl = False  # BeginSequenceRender ran with OpenGL: EndSequenceRender does the same (hostDemo)
    seq_cl = ""  # likewise with OpenCL, in this mode ("buffers" or "images")

    def sequence(name: str, action, begin: bool):
        # with OpenGL if the instance renders with it, else (or if it couldn't be attached) without.
        # (If render_mode() changed between Begin and End, ofxh would refuse End with opengl=True, where
        # hostDemo would send it: the plug-in's OpenGL support doesn't change during a sequence here.)
        # The same with OpenCL (refused by the session: without OpenCL).
        nonlocal seq_gl, seq_cl
        if begin:
            seq_gl = job.gpu == "opengl" and inst.render_mode() == "opengl"
            mode = inst.render_mode(opencl=True) if job.gpu == "opencl" else ""
            seq_cl = mode[len("opencl-"):] if mode.startswith("opencl-") else ""
        if seq_cl:
            st = run.call(name, action, first, last, opencl=seq_cl, **seq)
            if inst.opencl_result != "not_sent":
                return st
            seq_cl = ""
        if seq_gl:
            st = run.call(name, action, first, last, opengl=True, **seq)
            if inst.opengl_result != "not_sent":
                return st
            seq_gl = False
        return run.call(name, action, first, last, **seq)

    if not job.query:
        st = sequence("beginSequenceRender", inst.begin_sequence_render_action, True)
        report["actions"].append(_named("beginSequenceRender", _action(st)))
        if not status.succeeded(st):
            run.err(f"Error: begin render action failed (status {int(st)})")
            failed = True
    if not failed:
        try:
            _render_frames(run, inst, out_clip, sequential, report["renders"])
        except _Failed:
            failed = True
    if not job.query:
        st = sequence("endSequenceRender", inst.end_sequence_render_action, False)
        report["actions"].append(_named("endSequenceRender", _action(st)))
        if not status.succeeded(st):
            run.err(f"Error: end render action failed (status {int(st)})")
            failed = True
    if failed:
        raise _Failed


def _render_frames(run: _Run, inst, out_clip, sequential: bool, renders: list) -> None:
    """Each time and view, in order, until one fails (_Failed)."""
    for t in run.job.frames:
        for view in range(run.n_views):
            v = "" if run.n_views == 1 else "lr"[view]
            base = f"{run.job.prefix}.{_time_name(t)}{v}"
            key = _output_key(t, v)
            r: dict = {}
            renders.append(r)
            _render_frame(run, inst, out_clip, t, view, sequential, base, key, r)


def _final_report(inst, report: dict) -> None:
    """The final parameter values and states, the messages and the progress."""
    inst.timeline_goto_time(0.)
    params = report["params"] = {}
    for name in inst.param_names:
        type_ = inst.param_type(name)
        props = inst.param_props(name)
        p = params[name] = {"type": _ofx("param_type", type_), "value": _param_value(inst, name, type_)}
        if type_ in _NUMERIC:
            p["keys"] = [[k, _param_value(inst, name, type_, k)] for k in inst.param_keys(name)]
        p["enabled"] = bool(props.get(kOfxParamPropEnabled, (1,))[0])
        p["secret"] = bool(props.get(kOfxParamPropSecret, (0,))[0])
    report["messages"] = list(inst.messages)
    report["progress"] = [{"message": p["message"], "id": p["id"], "started": True, "ended": p["ends"] > 0,
                           "ends": p["ends"], "updates": p["updates"],
                           "lastValue": p["lastValue"] if p["updates"] > 0 else None}
                          for p in inst.progress]


# --- ofxhost.render() and ofxhost.describe(), in-process -------------------------------------

def _results(outputs: dict, report: dict):
    """outputs, alpha and images of a run, as the subprocess backend reads them from the files."""
    premult = (report.get("clipPreferences") or {}).get("outputPremultiplication")
    keyed = sorted(outputs.items(), key=lambda x: _key_order(x[0]))
    out, alpha, images = {}, {}, {}
    for key, (pixels, bounds) in keyed:
        img = np.ascontiguousarray(to_float(pixels)[::-1])  # top-down
        out[key] = img
        if img.shape[2] in (1, 4):
            alpha[key] = np.ascontiguousarray(img[:, :, -1])
        images[key] = Image(img, tuple(bounds), premult)
    return out, alpha, images


def _cmd(plugin_id, context, specs, depth, frames, scale, window, query, extra_args, assignments) -> list[str]:
    """The equivalent hostDemo command line (for the error messages)."""
    cmd = ["ofxhost.inprocess", "-p", plugin_id, "-c", context, "-d", depth]
    for clip, spec in specs:
        cmd += ["-i", f"{clip}={spec.text}"]
    cmd += _hostdemo_options(frames, scale, window, query)
    cmd += list(extra_args)
    for a in assignments:
        cmd += ["-s", a]
    return cmd


def _render_scale(scale) -> tuple:
    """render(scale=...), checked as hostDemo checks --scale (its shape is already checked)."""
    if scale is None:
        return (1., 1.)
    s = scale_pair(scale)
    if not valid_scale(s):
        raise _Usage(f'Error: invalid render scale "{scale}", {SCALE_EXPECTED}')
    return s


def _render_window_arg(window) -> tuple | None:
    if window is None:
        return None
    w = [int(v) for v in window]
    if not valid_window(w):
        raise _Usage(f'Error: invalid render window "{",".join(map(str, w))}", {WINDOW_EXPECTED}')
    return tuple(w)


def _input_specs(inputs: Mapping, comps: Mapping, sc: tuple, specs: list) -> None:
    """The inputs of render(), as hostDemo's -i, appended to specs.

    They are appended also after an error, for the command in the error message. Raises _Usage.
    """
    seen = set()
    for clip, value in _given_inputs(inputs, comps).items():
        try:
            spec = input_spec(clip, value, comps.get(clip))
        except SpecError as e:
            text = value if isinstance(value, str) else str(value)
            raise _Usage(f'Error: invalid input "{clip}={text}" ({e}), expected {INPUT_FORMS}') from None
        if clip in comps:
            # (the report's spec says what the input is: the effective components, which
            # override a suffix of the spec)
            spec.text = split_components(spec.text)[0] + ("" if comps[clip] == "rgba" else "#" + comps[clip])
        if clip in seen:
            raise _Usage(f'Error: input clip "{clip}" given twice (-i)')
        seen.add(clip)
        spec.scale = sc  # (a file's pixels are pixels at the render scale)
        specs.append((clip, spec))


def _write_report(path: str, report: dict, ret: int) -> int:
    """--report FILE of extra_args: the exit status, 1 if it can't be written."""
    try:
        write_json(path, report)
    except (OSError, ValueError):
        stdio.write(2, f"Error: cannot write {path}\n")
        ret = report["exitStatus"] = 1
        report["ok"] = False
    return ret


@_one_at_a_time
def render(bundle_paths, plugin_id: str, context: str = "filter", inputs: Mapping[str, Any] | None = None,
           params: Mapping[str, Any] | None = None, depth: str = "float", frames: Sequence[float] | None = None,
           scale: float | Sequence[float] | None = None, window: Sequence[int] | None = None, query: bool = False,
           extra_args: Sequence[str] = (), check: bool = True,
           components: Mapping[str, str] | None = None) -> RenderResult:
    """ofxhost.render(), in-process.

    Args:
        bundle_paths, plugin_id, context, inputs, params, depth, frames, scale, window, query,
            extra_args, check, components: as ofxhost.render().

    Returns:
        The RenderResult, as the subprocess backend's.

    Raises:
        ValueError: a bad argument.
        HostDemoError: the run failed (with check), or a command-line error (returncode 1, with
            hostDemo's message in stderr).
        RuntimeError: another ofxh host is initialized.
    """
    comps = _render_components(depth, components)
    dirs = tuple(plugin_dirs(bundle_paths))
    out_dir = os.getcwd()  # (the output files and the report are relative to the caller's directory)
    assignments = []
    for name, value in (params or {}).items():
        assignments += _param_args(name, value)[1::2]
    # (the same errors as the subprocess backend's command line: ValueError for non-finite numbers)
    for f in frames or ():
        _format_value(float(f))
    if scale is not None:
        for s in scale_pair(scale):  # (ValueError: not s or (sx, sy))
            _format_value(s)
    # (the paths relative to the current directory, which _capture() changes)
    inputs = {c: (resolve_spec(v) if isinstance(v, str) and v.startswith("file:") else
                  Path(v).resolve() if isinstance(v, os.PathLike) else v) for c, v in (inputs or {}).items()}
    specs: list = []
    ret, report, outputs = 1, {}, None
    with _capture() as cap:
        try:
            x = _extra(extra_args)
            ctx = _context(context)
            fr = _frames(frames)
            sc = _render_scale(scale)
            win = _render_window_arg(window)
            _input_specs(inputs, comps, sc, specs)
            # (hostDemo's order: the -s of extra_args, then those of params; its default format: ppm)
            job = Job(plugin_id, ctx, specs, x.assignments + assignments, depth, fr, sc, win, query=query,
                      ofx14=x.ofx14, normalised_defaults=x.normalised_defaults, allow_unwritten=x.allow_unwritten,
                      major=x.major, components=x.components,
                      fmt=x.format or ("ppm" if x.prefix is not None else None),
                      prefix=x.prefix if x.prefix is not None else OUTPUT_PREFIX, out_dir=out_dir)
            ret, report, outputs = run_job(dirs, job)
            if x.report is not None:
                ret = _write_report(os.path.join(out_dir, x.report), report, ret)
        except _Usage as e:
            stdio.write(2, str(e) + "\n")
    cmd = _cmd(plugin_id, context, specs, depth, frames, scale, window, query, extra_args, assignments)
    if outputs is None:
        raise HostDemoError("hostDemo (in-process) failed with exit status 1", returncode=1, stdout=cap["stdout"],
                            stderr=cap["stderr"], cmd=cmd)
    if check and ret != 0:
        raise HostDemoError(f"hostDemo (in-process) failed with exit status {ret}", returncode=ret,
                            stdout=cap["stdout"], stderr=cap["stderr"], cmd=cmd)
    out, alpha, images = _results(outputs, report)
    return RenderResult(outputs=out, alpha=alpha, report=report, returncode=ret, stdout=cap["stdout"],
                        stderr=cap["stderr"], cmd=cmd, images=images)


@_one_at_a_time
def describe(bundle_paths, plugin_id: str, context: str | None = None, extra_args: Sequence[str] = ()) -> dict:
    """ofxhost.describe(), in-process.

    ofxh.describe() of the plug-in in all its contexts (as hostDemo --describe, which describes them all whatever
    -c).

    Args:
        bundle_paths, plugin_id, context, extra_args: as ofxhost.describe().

    Returns:
        The JSON, as a dict.

    Raises:
        HostDemoError: the plug-in isn't found, or its description failed.
    """
    dirs = tuple(plugin_dirs(bundle_paths))
    cmd = ["ofxhost.inprocess", "-p", plugin_id] + (["-c", context] if context else []) + ["--describe"] + \
        list(extra_args)
    d = None
    with _capture() as cap:
        try:
            x = _extra(extra_args)
            if context:
                _context(context)
            ensure_host(dirs, "ofx14" if x.ofx14 else "default", normalised_defaults=x.normalised_defaults,
                        components=x.components)
            plugin = ofxh.get_plugin(plugin_id, x.major)
            if plugin is None:
                stdio.write(2, not_found(plugin_id) + "\n")
            else:
                d = ofxh.describe(plugin, None)
        except _Usage as e:
            stdio.write(2, str(e) + "\n")
    if d is None or not d.get("ok"):
        raise HostDemoError("hostDemo --describe (in-process) failed with exit status 1", returncode=1,
                            stdout=cap["stdout"], stderr=cap["stderr"], cmd=cmd)
    return d
