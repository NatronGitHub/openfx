"""The sample host: run a graph of plug-in instances (ofxhost.graph) over a frame sequence, in this
process, on the HostSupport bindings (the ofxh module), with the images in memory between the nodes.

    from ofxhost import graph, graphhost
    g = graph.load("graph.json")                       # or a Graph built in Python
    r = graphhost.run(g, ["/path/to/plugins"])         # writes the outputs' files
    r.ok, r.report, r.images["out"][0.0]               # the JSON report; {node: {time: Image}}

The command line front end is ofxhost.cli (python -m ofxhost render graph.json).

This is the template for a real host on ofxh (minimal_host.py is the minimal one): it subclasses
ofxh.Host, ofxh.ImageEffectInstance, ofxh.ClipInstance and ofxh.Param, and calls the actions in a
host's order. What it does, as a real host:
- Project load. Each node's parameter values and keys are set before kOfxActionCreateInstance,
  with no instanceChanged (ofxCore.h, the create instance action: "if the values are being loaded
  from an old setup, that load should have taken place before the create instance action is
  called"); the report's paramChangedCount shows it. Then the clip preferences, then the push
  buttons (a null value in the graph) are pressed as a user would (instanceChanged, "user edited"),
  in order, at the project's first frame.
- The project: its size, offset, PAR, frame rate and frame range (Graph.project; the defaults are
  hostDemo's), given to every instance.
- The time domain: the frames to render are the graph's frames, else every frame of the outputs'
  time domains (their union). The spec says of kOfxImageEffectActionGetTimeDomain: "Only effects
  instantiated in the General Context can have this called on them. In all other the host is in
  strict control over the temporal duration of the effect." This host's choice, following Natron
  (OfxEffectInstance::getFrameRange), is to call it in the general, reader, writer and generator
  contexts. When the plug-in replies default (and in the other contexts), the spec's default: "the
  union of all the frame ranges of the non optional input clips, infinite if there are no non
  optional input clips" (infinite: the project's frame range). A time domain that is infinite or
  beyond +-1e7 is clamped to the project's frame range; NaN fails the run. The frame range of an
  input clip is the time domain of its input node, or the frames of its image sequence (a still
  image, and the test patterns: the project's frame range).
- The sequence renders: each node gets kOfxImageEffectActionBeginSequenceRender with the frames it
  will render (planned from the outputs' frames, the frames needed and the identities), sequential
  if they increase by a constant step, and EndSequenceRender if its begin succeeded. A render
  action is sequential if its node's sequence is and its time comes after the node's last render.
- Images in memory: each node renders the window it is asked for (its output, or what the nodes
  that use it need), at the render scale. Before the render action, it asks the plug-in which
  frames (kOfxImageEffectActionGetFramesNeeded) and regions (GetRegionsOfInterest) of its inputs
  it needs, and renders them; a fetch of something else renders it then. A node without tiles
  renders its whole RoD, and gets whole inputs. An identity (isIdentity) isn't rendered: its
  output is the image of the clip and time it gives. The images are kept for the current frame,
  then dropped. Each input gets its image at its clip's depth and components (converted if the
  clip preferences changed them).
- Image sequences, with the file name patterns of ofxhost.sequences ("####", "%04d", "%d", "%v",
  "%V", "%%"), as Natron:
  - the inputs' files ({"file": pattern} in the graph, ofxhost.graph.Files), read at the frame of
    each time (the first or last frame of the sequence outside it); and the outputs' files;
  - the file parameters of the plug-ins: the output file paths (string parameters in file path
    mode with kOfxParamPropStringFilePathExists = 0), and the file and proxy parameters of a reader
    or a writer (kOfxImageEffectFileParamName "filename", kOfxImageEffectProxyParamName "proxy", in
    file path mode, of a plug-in that has the reader or writer context). Their value is the
    pattern; paramGetValue gives the file name at the current time, and paramGetValueAtTime at
    that time (Natron's OfxStringInstance::get, KnobFile::getFileName), for the current view;
  - a reader's frame range: if the plug-in has an "originalFrameRange" (Integer2D) parameter and
    the graph doesn't set it, it is set from the files of the sequence, as Natron's
    Node::computeFrameRangeForReader (the first and last frames, or (1, 1) for one file or none).
- Inputs: an input clip the graph doesn't give is disconnected. Image files are given as float
  RGBA (an RGB file is opaque, a grey one r = g = b = a, as hostDemo's file inputs), at the
  project's PAR (HostSupport doesn't let the clip preferences change an input's PAR). Formats: PFM
  and .npy (ofxhost.imageio; no PNG or JPEG yet).
- Paths: the file inputs, the outputs and the file parameters are relative to the graph's
  directory (Graph.base), as Natron resolves file parameters against the project's path: the
  plug-ins get absolute file names.
- The report (Result.report, ofxhost.cli render --report): see the README.
- Errors: run() doesn't raise for a bad graph, a missing or unreadable file, an unknown plug-in,
  parameter or clip, or a failed action: the run ends with ok False and the report's "error" (the
  report is written too). It raises RuntimeError only if another ofxh host is initialized.

Limits: one view (view 0, which ofxh names "main": "%V" gives "main", where Natron's first view is
"Main"); the colour plane only; no host frame threading (single-threaded renders), so a plug-in that
fetches its inputs on its own worker threads (e.g. OpenMP) can't get the images that weren't rendered
before its render action (ofxh refuses actions there); no interacts.

A graph is like a script: it chooses the plug-ins that run and the files they read and write. Run
only the graphs you trust.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import logging
import math
import os
from collections.abc import Sequence
from typing import Any

import numpy as np

from . import Image, resolve_file_spec
from . import inprocess as _ip
from . import sequences
from .graph import Files, Graph, Node, Project, load, topological_order, _json_value, _keys, _scale
from .imageio import check_format, read_image, write_image

__all__ = ["run", "Result", "GraphError", "host", "is_file_param", "REPORT_SCHEMA", "REPORT_SCHEMA_VERSION"]

ofxh = _ip.ofxh
log = logging.getLogger("ofxhost")

REPORT_SCHEMA = "ofxhost.graphReport"
REPORT_SCHEMA_VERSION = 1

# (hostDemo's helpers, shared with the in-process backend)
_to_pixels, _intersect, _empty, _fits, _stats = _ip._to_pixels, _ip._intersect, _ip._empty, _ip._fits, _ip._stats
_ok, _stat_str = _ip._ok, _ip._stat_str
_NO_VALUE = {"group", "page", "pushbutton", "parametric"}
_TIME_DOMAIN_CONTEXTS = {"general", "reader", "writer", "generator"}
_MAX_FRAME = 1e7
_HELD = {"boolean", "choice"}


class GraphError(RuntimeError):
    """The graph can't be run: an unknown plug-in, a missing input file, an action that failed..."""


@dataclasses.dataclass
class Result:
    """ok: the run succeeded. report: the JSON report. images: {output node: {time: Image}} (top-down
    float32 pixels of the rendered window, as ofxhost.graph.execute()). files: the files written."""
    ok: bool
    report: dict
    images: dict
    files: list


def _union(a, b):
    return b if a is None else (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def _contains(a, b) -> bool:
    return a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]


def _canonical(px, rs, par):
    """Pixels at the render scale to canonical coordinates."""
    return (px[0] * par / rs[0], px[1] / rs[1], px[2] * par / rs[0], px[3] / rs[1])


def _frames_in(ranges, t: float) -> list[float]:
    """The frames of frames-needed ranges ([[t1, t2]...]): t1, t1 + 1, ... up to t2, and t2. None
    (the plug-in said nothing for the clip): [t]."""
    out: list[float] = []
    for lo, hi in [[t, t]] if ranges is None else ranges:  # ([]: no frames)
        f = float(lo)
        while f < hi - 1e-9:
            out.append(f)
            f += 1.
        out.append(float(hi))
    return list(dict.fromkeys(out))


def _rgba(pixels: np.ndarray) -> np.ndarray:
    """Any image (bottom-up, byte, short, half or float, 1, 3 or 4 components) as float64 RGBA in the
    native range of float (byte and short divided by their maximum): an RGB image is opaque, an Alpha
    image gives (a, a, a, a), as hostDemo's Pf files."""
    a = pixels.astype(np.float64)
    if pixels.dtype == np.uint8:
        a /= 255.
    elif pixels.dtype == np.uint16:
        a /= 65535.
    if a.ndim == 2:
        a = a[:, :, None]
    if a.shape[2] == 1:
        return np.repeat(a, 4, axis=2)
    if a.shape[2] == 3:
        return np.dstack([a, np.ones(a.shape[:2])])
    return a


_DTYPES = {"byte": np.uint8, "short": np.uint16, "half": np.float16, "float": np.float32}


def _convert(pixels: np.ndarray, depth: str, comps: str) -> np.ndarray:
    """An image at a clip's depth and components (the same array if it is already)."""
    n = _ip._NCOMPS.get(comps)
    if pixels.dtype == _DTYPES.get(depth) and pixels.ndim == 3 and pixels.shape[2] == n:
        return pixels
    if depth == "half":
        return np.ascontiguousarray(_ip._to_depth(_rgba(pixels), "float", comps), np.float16)
    return _ip._to_depth(_rgba(pixels), depth, comps)


# --- the host ---------------------------------------------------------------------------------

_LEVELS = {"fatal": logging.CRITICAL, "error": logging.ERROR, "warning": logging.WARNING,
           "question": logging.WARNING, "message": logging.INFO, "log": logging.DEBUG}


class Host(ofxh.Host):
    """The host: the messages go to the "ofxhost" logger; questions are answered yes (a batch host
    has nobody to ask)."""

    def message(self, type, id, text):
        log.log(_LEVELS.get(type, logging.INFO), "%s", text)
        return ofxh.Stat.ReplyYes if type == "question" else ofxh.Stat.OK


@contextlib.contextmanager
def host(plugin_dirs: Sequence = (), use_env_path: bool = False):
    """ofxh initialized with a Host and these plug-in directories (and $OFX_PLUGIN_PATH if
    use_env_path), shut down at the end. ofxh has one host per process: the in-process backend's
    (ofxhost.inprocess) is shut down first; another one raises RuntimeError."""
    if ofxh.initialized():
        if _ip._session is not None and ofxh.host() is _ip._session.host:
            _ip.shutdown()
        else:
            raise RuntimeError("an ofxh host is already initialized (ofxh has one host per process): "
                               "call ofxh.shutdown() first")
    ofxh.init(Host(), [os.fspath(d) for d in plugin_dirs], use_std_paths=False, use_env_path=use_env_path)
    try:
        yield
    finally:
        ofxh.shutdown()


def plugin_dirs_of(paths: Sequence) -> list[str]:
    """Plug-in directories: a .ofx.bundle path stands for its parent directory (as ofxhost.render())."""
    from . import _plugin_dirs
    return list(_plugin_dirs(paths))


# --- the parameters ---------------------------------------------------------------------------

def is_file_param(desc, contexts) -> bool:
    """Is this parameter descriptor a file parameter whose value may be a pattern (#136, as Natron):
    an output file path (a string in file path mode, kOfxParamPropStringFilePathExists = 0), or the
    file or proxy parameter (kOfxImageEffectFileParamName, kOfxImageEffectProxyParamName) of a
    plug-in that has the reader or writer context (contexts: its contexts)?"""
    if desc.type != "string" or desc.props.get("OfxParamPropStringMode", ("",))[0] != "OfxParamStringIsFilePath":
        return False
    if not desc.props.get("OfxParamPropStringFilePathExists", (1,))[0]:
        return True
    readers = {"reader", "writer", "OfxImageEffectContextReader", "OfxImageEffectContextWriter"}
    return desc.name in ("filename", "proxy") and bool(readers & set(contexts))


class FileParam(ofxh.Param):
    """A file parameter: it holds the pattern, and gives the plug-in the file name at the time it
    asks for (the current time for paramGetValue), for view 0 (the only one), made absolute against
    base (the graph's directory; None: the current directory), as Natron resolves file parameters
    against the project's path. An empty value stays empty."""

    def __init__(self, base: str | None = None):
        super().__init__()
        self.base = base
        self.pattern: str | None = None  # None: the default

    @property
    def value(self) -> str:
        """The pattern, as the graph gives it (or the default)."""
        return self.initial if self.pattern is None else self.pattern

    def path(self, name: str) -> str:
        """A name of the parameter, absolute (relative to base)."""
        return name if not name else os.path.abspath(os.path.join(self.base or os.getcwd(), name))

    def get(self, time):
        return self.path(sequences.expand(self.value, sequences.frame_of(time), self.instance.view_name(0)))

    def set(self, value, time=None):
        self.pattern = str(value)


# --- the instances and clips ------------------------------------------------------------------

class Instance(ofxh.ImageEffectInstance):
    """An instance of a node: the project, the timeline bounds, the messages, the file parameters."""

    def __init__(self, run: _Run, node: _NodeState):
        super().__init__()
        self.run = run
        self.node = node
        p = run.project
        self.project_size = p.size
        self.project_offset = p.offset
        self.project_extent = (p.offset[0] + p.size[0], p.offset[1] + p.size[1])
        self.project_pixel_aspect_ratio = p.pixel_aspect_ratio
        self.frame_rate = p.frame_rate
        self.effect_duration = p.frame_range[1] - p.frame_range[0] + 1.
        self.default_output_depth = run.depth
        self.rois: dict = {}  # the regions of interest of the current render (canonical), by clip

    def timeline_get_bounds(self):
        return tuple(self.run.project.frame_range)

    def new_clip_instance(self, desc, index):
        return Clip(self.run, self.node, desc.name)

    def new_param(self, desc):
        return FileParam(self.run.graph.base) if is_file_param(desc, self.node.plugin.contexts) else None

    def _record(self, kind, type, id, text):
        self.node.report["messages"].append({"kind": kind, "type": type, "id": id, "text": text})
        log.log(_LEVELS.get(type, logging.INFO), "%s: %s", self.node.name, text)
        return ofxh.Stat.ReplyYes if type == "question" else ofxh.Stat.OK

    def message(self, type, id, text):
        return self._record("message", type, id, text)

    def persistent_message(self, type, id, text):
        return self._record("persistentMessage", type, id, text)


@dataclasses.dataclass
class _Rendered:
    """A node's image: bottom-up pixels at its output depth, and their bounds (pixels at the scale)."""
    pixels: np.ndarray
    bounds: tuple


class Clip(ofxh.ClipInstance):
    """A clip. An input is connected to another node, an image sequence (Files), or what
    ofxhost.render() takes: a hostDemo input spec ("ramp", "checker:16", "file:x.pfm", "none"...),
    an array or an Image. An input that the graph doesn't give is disconnected."""

    def __init__(self, run: _Run, node: _NodeState, name: str):
        super().__init__()
        self.run = run
        src = node.sources.get(name)
        self.child: _NodeState | None = src if isinstance(src, _NodeState) else None
        self.files: _Files | None = src if isinstance(src, _Files) else None
        self.spec = src if isinstance(src, _ip._Spec) else _ip._Spec(type="none", text="none")

    # what the clip is connected to: the properties a host sets
    @property
    def connected(self):
        return self.is_output or self.child is not None or self.files is not None or self.spec.type != "none"

    def _out(self):
        return self.child.instance.clip("Output")

    @property
    def unmapped_bit_depth(self):
        if self.is_output:
            return self.run.depth
        if self.child is not None:
            return self._out().pixel_depth
        return "float" if self.files is not None else self.run.depth

    @property
    def unmapped_components(self):
        if self.is_output:
            return "rgba"
        if self.child is not None:
            return self._out().components
        return "rgba"  # (image files too: RGB is opaque RGBA, grey is r = g = b = a, as hostDemo's files)

    @property
    def premult(self):
        if self.is_output:
            return self.instance.output_premultiplication
        if self.child is not None:
            return self.child.instance.output_premultiplication
        if not self.connected or self.components == "rgb":
            return "opaque"
        if self.files is not None:
            return self.files.premult or "unpremult"
        return self.spec.premult or "unpremult"

    @property
    def aspect_ratio(self):
        if self.child is not None and not self.is_output:
            return self._out().aspect_ratio
        return self.run.project.pixel_aspect_ratio

    @property
    def frame_rate(self):
        if self.child is not None and not self.is_output:
            return self.child.instance.output_frame_rate
        return self.run.project.frame_rate

    @property
    def frame_range(self):
        if self.is_output:
            return self.instance.node.time_domain or self.run.project.frame_range
        if self.child is not None:
            return self.child.time_domain or self.run.project.frame_range
        if self.files is not None and self.files.frames:
            return (float(self.files.frames[0]), float(self.files.frames[-1]))
        return self.run.project.frame_range

    unmapped_frame_range = frame_range

    @property
    def format(self):
        if self.files is not None:
            h, w = self.files.size
            return (0, 0, w, h)
        s = self.spec
        if s.type == "file" and not self.is_output:
            h, w = s.data.shape[:2]
            return (math.floor(s.file_x / s.scale[0]), math.floor(s.file_y / s.scale[1]),
                    math.ceil((s.file_x + w) / s.scale[0]), math.ceil((s.file_y + h) / s.scale[1]))
        p = self.run.project
        return tuple(ofxh.canonical_to_pixel((p.offset[0], p.offset[1], p.offset[0] + p.size[0],
                                              p.offset[1] + p.size[1]), (1., 1.), p.pixel_aspect_ratio))

    # the regions and images
    def _spec_at(self, time):
        """The input at a time, as hostDemo's input spec: the file of that frame for a sequence."""
        return self.files.spec(time) if self.files is not None else self.spec

    def _rod(self, time, view, rs):
        if self.child is not None:
            st, rod = self.child.instance.get_region_of_definition_action(time, rs, view)
            return tuple(rod) if _ok(st) else (0., 0., 0., 0.)
        s = self._spec_at(time)
        if s.type == "none":
            return (0., 0., 0., 0.)
        if s.type == "rod":
            return s.rod
        if s.type == "file":
            par = self.aspect_ratio
            h, w = s.data.shape[:2]
            return (s.file_x / s.scale[0] * par, s.file_y / s.scale[1],
                    (s.file_x + w) / s.scale[0] * par, (s.file_y + h) / s.scale[1])
        p = self.run.project
        return (p.offset[0], p.offset[1], p.offset[0] + p.size[0], p.offset[1] + p.size[1])

    def region_of_definition(self, time, view):
        if self.is_output:
            return super().region_of_definition(time, view)  # the plug-in's RoD action
        return self._rod(time, view, tuple(self.instance.render_scale))

    def image(self, time, view, region=None, rs=None):
        """The input's image at a time over a canonical region (None: the current render's RoI, else
        the RoD), clipped to the RoD; the whole RoD if the plug-in or the clip has no tiles. rs: the
        render scale (None: the current action's). Returns an ofxh.ImageData at the clip's depth and
        components, or None."""
        if not self.connected or self.is_output:
            return None
        inst = self.instance
        rs = tuple(inst.render_scale) if rs is None else tuple(rs)
        par = self.aspect_ratio
        rod_c = self._rod(time, view, rs)
        rod = _to_pixels(rod_c, rs, par)
        if not (inst.supports_tiles and self.supports_tiles):
            region = rod_c
        elif region is None:
            region = inst.rois.get(self.name, rod_c)
        b = _intersect(_to_pixels(_intersect(region, rod_c), rs, par), rod)
        if _empty(b):
            return None
        depth, comps = self.pixel_depth, self.components
        if not _fits(b):
            log.error("%s.%s: an image of %dx%d pixels is too large", inst.node.name, self.name,
                      b[2] - b[0], b[3] - b[1])
            return None
        if self.child is not None:
            r = self.run.render(self.child, time, b, rs, view)
            if r is None:
                return None
            return ofxh.ImageData(_convert(r.pixels, depth, comps), r.bounds, rod=rod)
        if depth not in _ip._BPC or comps not in _ip._NCOMPS:
            raise GraphError(f"{inst.node.name}.{self.name}: can't make {depth} {comps} images")
        pixels = _ip._to_depth(_ip._pattern(self._spec_at(time), time, view, b, rod, rs), depth, comps)
        return ofxh.ImageData(pixels, b, rod=rod)

    def get_image(self, time, view, bounds, plane="Colour"):
        if self.is_output:
            return super().get_image(time, view, bounds, plane)
        return self.image(time, view, None if bounds is None else tuple(bounds))


class _Files:
    """An image sequence input: its frames, and the files read (as hostDemo's file inputs, pixels
    at scale 1: the render scale samples them). Raises GraphError for a missing or unreadable file."""

    def __init__(self, path: str, premult: str | None):
        self.path = path
        self.premult = premult
        self.still = not sequences.has_pattern(path)
        try:
            self.frames = [] if self.still else sequences.frames(path)
        except ValueError as e:
            raise GraphError(str(e)) from None
        first = path if self.still else (sequences.expand(path, self.frames[0]) if self.frames else None)
        if first is None or not os.path.isfile(first):
            raise GraphError(f"{path}: no such file" if self.still else f"{path}: no files of this sequence")
        a = self._read(first)
        self.size = a.shape[:2]
        self._specs: dict[str, Any] = {}

    @staticmethod
    def _read(path: str) -> np.ndarray:
        try:
            return read_image(path)
        except (OSError, ValueError) as e:
            raise GraphError(f"{path}: {e}") from None

    def spec(self, time):
        f = sequences.frame_of(time)
        if not self.still:
            f = min(max(f, self.frames[0]), self.frames[-1])  # (held outside the sequence)
        path = self.path if self.still else sequences.expand(self.path, f)
        s = self._specs.get(path)
        if s is None:
            if not os.path.isfile(path):
                raise GraphError(f"{path}: no such file (frame {f} of the sequence)")
            a = self._read(path)
            data = _ip._rgba(np.ascontiguousarray(a[::-1, :, :3] if a.shape[2] != 1 else a[::-1, :, 0]),
                             None if a.shape[2] != 4 else np.ascontiguousarray(a[::-1, :, 3]))
            s = self._specs[path] = _ip._Spec(type="file", text=path, data=data)
        return s


# --- a run ------------------------------------------------------------------------------------

@dataclasses.dataclass(eq=False)
class _NodeState:
    name: str
    node: Node
    plugin: Any = None
    instance: Any = None
    time_domain: tuple | None = None
    sources: dict = dataclasses.field(default_factory=dict)  # input clip -> _NodeState | _Files | _ip._Spec
    planned: set = dataclasses.field(default_factory=set)  # the frames it will render (beginSequenceRender)
    sequential: bool = False  # its beginSequenceRender's
    last_render: float | None = None
    report: dict = dataclasses.field(default_factory=dict)


class _Run:
    def __init__(self, graph: Graph, frames, scale):
        self.graph = graph
        self.project = graph.project or Project()
        self.depth = graph.depth
        self.scale = _scale(graph.scale if scale is None else scale)
        self.frames = frames
        self.nodes: dict[int, _NodeState] = {}  # id(Node) -> state
        self.cache: dict = {}  # (node name, time, view) -> _Rendered or None, for the current frame
        self._files: dict = {}
        self.project_window: tuple = (0, 0, 0, 0)  # (the project in pixels at the render scale)

    def source(self, ns: _NodeState, clip: str, v):
        """What an input clip is connected to: a node, an image sequence, or a hostDemo input (a spec
        string, an array or an Image; a "file:" spec or an array has pixels at the render scale)."""
        if isinstance(v, Node):
            if id(v) not in self.nodes:
                raise GraphError(f"{ns.name}.{clip}: its input {v!r} is not a node of the graph")
            return self.nodes[id(v)]
        if isinstance(v, Files):
            return self.files(v)
        try:
            if isinstance(v, str):
                spec = _ip._parse_spec(resolve_file_spec(v, self.graph.base) if v.startswith("file:") else v)
            else:
                spec = _ip._array_spec(clip, v)
        except (ValueError, TypeError) as e:
            raise GraphError(f"{ns.name}.{clip}: invalid input {v if isinstance(v, str) else type(v).__name__} ({e})")
        spec.scale = self.scale
        return spec

    def files(self, f: Files) -> _Files:
        pattern, premult = f.split()
        path = os.path.abspath(self.graph.path(pattern))
        if (path, premult) not in self._files:
            self._files[(path, premult)] = _Files(path, premult)
        return self._files[(path, premult)]

    @staticmethod
    def check(ns: _NodeState, action: str, st, **more) -> None:
        """Record an action in the node's report; raise GraphError if it failed."""
        ns.report["actions"].append({"action": action, "ok": _ok(st), "status": _stat_str(st), **more})
        if not _ok(st):
            raise GraphError(f"{ns.name}: the {action} action failed ({_stat_str(st)})")

    def call(self, ns: _NodeState, action: str, f, *args):
        """An action (an ofxh.CallbackError, a host callback that raised, is a failure)."""
        try:
            return f(*args)
        except ofxh.CallbackError as e:
            raise GraphError(f"{ns.name}: {action}: {e}") from e

    # --- the instances, inputs first
    def setup(self, ns: _NodeState) -> None:
        node = ns.node
        major = node.version[0] if node.version is not None else -1
        ns.plugin = ofxh.get_plugin(node.plugin, int(major))
        if ns.plugin is None:
            raise GraphError(f"{ns.name}: plug-in {node.plugin}" + (f" version {major}" if major >= 0 else "") +
                             " not found")
        r = ns.report
        r.update(plugin={"id": ns.plugin.id, "version": list(ns.plugin.version), "label": ns.plugin.label},
                 context=node.context, actions=[], messages=[], renders=[])
        if node.context not in ns.plugin.contexts:
            raise GraphError(f"{ns.name}: plug-in {node.plugin} has no {node.context} context "
                             f"(its contexts: {', '.join(ns.plugin.contexts)})")
        for clip, v in node.inputs.items():
            ns.sources[clip] = self.source(ns, clip, v)
        inst = Instance(self, ns)
        try:
            made = ofxh.create_instance(ns.plugin, node.context, inst)
        except ofxh.CallbackError as e:
            if e.instance is not None:
                e.instance.close()
            raise GraphError(f"{ns.name}: {e}") from e
        if made is None:
            raise GraphError(f"{ns.name}: could not create an instance of {node.plugin}")
        ns.instance = inst
        clips = inst.clips
        for clip in node.inputs:
            if clip not in clips or clips[clip].is_output:
                raise GraphError(f"{ns.name}: no input clip {clip!r} (its clips: {', '.join(clips)})")
        missing = [n for n, c in clips.items() if not c.is_optional and not c.connected]
        if missing:
            raise GraphError(f"{ns.name}: these clips are not optional, they can't be disconnected: {missing}")
        # the project load: the values and keys, before the create instance action, without instanceChanged
        names = set(inst.param_names)
        for name, v in node.params.items():
            if name not in names:
                raise GraphError(f"{ns.name}: no parameter {name!r}")
            if v is None:
                if inst.param_type(name) != "pushbutton":
                    raise GraphError(f"{ns.name}.{name}: null presses a push button, but it is a {inst.param_type(name)}")
                continue
            keys = None if isinstance(v, str) else _keys(v)
            for t, x in (sorted(keys.values.items(), key=lambda kv: float(kv[0])) if keys else [(None, v)]):
                try:
                    st = inst.set_param(name, x, None if t is None else float(t))
                except (ValueError, TypeError, ofxh.CallbackError) as e:
                    raise GraphError(f"{ns.name}.{name}: {e}") from e
                if int(st) != 0:
                    raise GraphError(f"{ns.name}.{name}: could not set {x!r} ({_stat_str(st)})")
        self.reader_frame_range(ns)
        self.check(ns, "createInstance", self.call(ns, "createInstance", inst.create_instance_action))
        r["paramChangedCount"] = inst.param_changed_count  # (0: the load sent no instanceChanged)
        self.clip_preferences(ns)
        # the push buttons, as a user presses them
        for name, v in node.params.items():
            if v is None:
                t = float(self.project.frame_range[0])
                self.check(ns, "instanceChanged", self.call(ns, "instanceChanged", inst.press, name, t),
                           param=name, time=t)
        if inst.clip_preferences_dirty:
            self.clip_preferences(ns)
        self.time_domain(ns)

    def reader_frame_range(self, ns: _NodeState) -> None:
        """Natron's Node::computeFrameRangeForReader: a reader's originalFrameRange from its files."""
        inst = ns.instance
        p = inst.param("filename") if "filename" in inst.param_names else None
        if (not isinstance(p, FileParam) or p.pattern is None or "originalFrameRange" not in inst.param_names
                or inst.param_type("originalFrameRange") != "integer2d" or "originalFrameRange" in ns.node.params):
            return
        try:
            fs = sequences.frames(p.path(p.pattern)) if sequences.has_pattern(p.pattern) else []
        except ValueError as e:
            raise GraphError(f"{ns.name}.filename: {e}") from None
        inst.set_param("originalFrameRange", (fs[0], fs[-1]) if len(fs) > 1 else (1, 1))

    def clip_preferences(self, ns: _NodeState) -> None:
        inst = ns.instance
        self.check(ns, "getClipPreferences", self.call(ns, "getClipPreferences", inst.get_clip_preferences_action))
        ns.report["clipPreferences"] = {
            "clips": {name: {"depth": c.pixel_depth, "components": c.components, "pixelAspectRatio": c.aspect_ratio,
                             "connected": c.connected}
                      for name, c in inst.clips.items()},
            "outputPremultiplication": inst.output_premultiplication, "frameRate": inst.output_frame_rate}

    def time_domain(self, ns: _NodeState) -> None:
        """The node's time domain: getTimeDomain in the general, reader, writer and generator contexts
        (Natron's choice; the spec calls it in the general context only), else, or if the plug-in
        replies default, the spec's default. None: infinite (the project's frame range)."""
        inst = ns.instance
        j = ns.report["timeDomain"] = {"action": None, "range": None}
        if ofxh.short_name("context", ofxh.ofx_name("context", ns.node.context)) in _TIME_DOMAIN_CONTEXTS:
            st, rng = self.call(ns, "getTimeDomain", inst.get_time_domain_action)
            j["action"] = _stat_str(st)
            if not _ok(st):
                raise GraphError(f"{ns.name}: the getTimeDomain action failed ({_stat_str(st)})")
            if int(st) == 0:
                if any(math.isnan(v) for v in rng):
                    raise GraphError(f"{ns.name}: getTimeDomain gave {tuple(rng)}")
                ns.time_domain = self.clamp(rng)
        if ns.time_domain is None:
            # "the union of all the frame ranges of the non optional input clips, infinite if there are
            # no non optional input clips" (infinite: None, the project's frame range)
            ranges = [c.frame_range for c in inst.clips.values() if not c.is_output and not c.is_optional]
            if ranges:
                ns.time_domain = self.clamp((min(r[0] for r in ranges), max(r[1] for r in ranges)))
        j["range"] = list(ns.time_domain) if ns.time_domain is not None else None

    def clamp(self, rng) -> tuple:
        """A frame range whose infinite or huge (beyond +-1e7) ends are the project's."""
        lo, hi = float(rng[0]), float(rng[1])
        p = self.project.frame_range
        lo = p[0] if not abs(lo) <= _MAX_FRAME else lo
        hi = p[1] if not abs(hi) <= _MAX_FRAME else hi
        return (lo, hi)

    # --- the renders
    def render(self, ns: _NodeState, t: float, window, rs, view: int = 0) -> _Rendered | None:
        """The node's image at time t, at least over window (pixels at the render scale rs), clipped to
        its RoD (its whole RoD without tiles); None if that is empty."""
        key = (ns.name, float(t), tuple(rs), view)
        if key in self.cache:
            cached = self.cache[key]
            if cached is None or _contains(cached.bounds, window):
                return cached
            window = _union(window, cached.bounds)
        inst = ns.instance
        out = inst.clip("Output")
        st, rod = self.call(ns, "getRegionOfDefinition", inst.get_region_of_definition_action, t, rs, view)
        if not _ok(st):
            raise GraphError(f"{ns.name}: the getRegionOfDefinition action failed at time {t} ({_stat_str(st)})")
        par = out.aspect_ratio
        rod_px = _to_pixels(rod, rs, par)
        tiles = inst.supports_tiles and out.supports_tiles
        win = rod_px if not tiles else _intersect(window, rod_px)
        rec: dict[str, Any] = {"time": float(t), "renderWindow": None if _empty(win) else list(win), "identity": None}
        ns.report["renders"].append(rec)
        if _empty(win):
            self.cache[key] = None
            return None
        if not _fits(win):
            raise GraphError(f"{ns.name}: a render window of {win[2] - win[0]}x{win[3] - win[1]} pixels is too large")
        # an identity isn't rendered: its output is its input's image
        st, idr = self.call(ns, "isIdentity", inst.is_identity_action, t, win, rs, "none", view)
        if int(st) == 0 and idr.get("clip") in inst.clips:
            rec["identity"] = {"clip": idr["clip"], "time": idr["time"]}
            img = inst.clip(idr["clip"]).image(idr["time"], view, _canonical(win, rs, par), rs)
            result = None if img is None else _Rendered(_convert(img.pixels, out.pixel_depth, out.components),
                                                         tuple(img.bounds))
            self.cache[key] = result
            return result
        # what it needs from its inputs, rendered first
        st, ranges = self.call(ns, "getFramesNeeded", inst.get_frames_needed_action, t)
        st2, rois = self.call(ns, "getRegionsOfInterest", inst.get_regions_of_interest_action, t,
                              _canonical(win, rs, par), rs, view)
        if not (_ok(st) and _ok(st2)):
            raise GraphError(f"{ns.name}: the getFramesNeeded or getRegionsOfInterest action failed at time {t}")
        rois = {c: tuple(r) for c, r in rois.items()}
        inst.rois = rois
        try:
            for name, clip in inst.clips.items():
                if clip.child is not None and not clip.is_output and name in rois:
                    for f in _frames_in(ranges.get(name), t):
                        clip.image(f, view, rois[name], rs)
            # sequential: the node's sequence is, and this render comes after its last one
            sequential = ns.sequential and (ns.last_render is None or t > ns.last_render)
            ns.last_render = t if ns.last_render is None else max(ns.last_render, t)
            rec["sequential"] = sequential
            st, planes = self.call(ns, "render", inst.render_action, t, win, rs, "none", sequential, False, False,
                                   view)
        finally:
            inst.rois = {}
        image = planes.get("Colour")
        rec.update(ok=int(st) == 0, status=_stat_str(st))
        if int(st) != 0 or image is None:
            raise GraphError(f"{ns.name}: the render action failed at time {t} ({_stat_str(st)}"
                             + ("" if image is not None else ", no output image") + ")")
        result = _Rendered(image.pixels, tuple(image.bounds))
        self.cache[key] = result
        return result


def _param_report(inst) -> dict:
    """The parameter values after the run, in the graph's form ({"keys": ...} for the parameters with
    keys, the pattern of a file parameter): the params of a graph node."""
    values = inst.param_values()
    out = {}
    for name in inst.param_names:
        type_ = inst.param_type(name)
        if type_ in _NO_VALUE or name not in values:
            continue
        p = inst.param(name)
        if isinstance(p, FileParam):
            out[name] = {"file": p.value}
        elif inst.param_keys(name):
            out[name] = {"keys": [[t, _json_value(v)] for t, v in inst.param_curve(name)], "interpolation": "linear"}
        else:
            out[name] = values[name]
    return out


def _frames(run: _Run, outputs: list[_NodeState]) -> list[float]:
    """The frames to render: the given ones, else each frame of the union of the outputs' time domains."""
    if run.frames is not None:
        return [float(t) for t in run.frames]
    ranges = [ns.time_domain or run.project.frame_range for ns in outputs]
    first, last = min(r[0] for r in ranges), max(r[1] for r in ranges)
    n = int(math.floor(last - first + 1e-9)) + 1
    return [first + k for k in range(max(n, 1))]


def _plan(r: _Run, ns: _NodeState, t: float, window, seen: set) -> None:
    """Add t to the frames the node will render, and the frames its inputs will render for it: the
    frames needed, or the identity's clip and time (with the RoD clipped to window, or to the
    project for an input). Only actions that don't render."""
    if (ns.name, t) in seen:
        return
    seen.add((ns.name, t))
    inst = ns.instance
    st, rod = r.call(ns, "getRegionOfDefinition", inst.get_region_of_definition_action, t, r.scale, 0)
    if not _ok(st):
        raise GraphError(f"{ns.name}: the getRegionOfDefinition action failed at time {t} ({_stat_str(st)})")
    win = _intersect(_to_pixels(rod, r.scale, inst.clip("Output").aspect_ratio), window)
    if _empty(win):
        return  # (nothing to render)
    st, idr = r.call(ns, "isIdentity", inst.is_identity_action, t, win, r.scale, "none", 0)
    if int(st) == 0 and idr.get("clip") in inst.clips:
        clip = inst.clip(idr["clip"])
        if clip.child is not None:
            _plan(r, clip.child, float(idr["time"]), r.project_window, seen)
        return
    ns.planned.add(t)
    st, ranges = r.call(ns, "getFramesNeeded", inst.get_frames_needed_action, t)
    if not _ok(st):
        raise GraphError(f"{ns.name}: the getFramesNeeded action failed at time {t} ({_stat_str(st)})")
    for name, clip in inst.clips.items():
        if clip.child is not None and not clip.is_output:
            for f in _frames_in(ranges.get(name), t):
                _plan(r, clip.child, f, r.project_window, seen)


def _sequence(frames) -> tuple[float, float, float, bool]:
    """(first, last, step, sequential) of the frames a node renders: sequential if they increase by
    a constant step (step 1 otherwise)."""
    fs = sorted(frames)
    step = fs[1] - fs[0] if len(fs) > 1 else 1.
    sequential = all(abs(b - a - step) < 1e-9 for a, b in zip(fs, fs[1:]))
    return fs[0], fs[-1], step if sequential else 1., sequential


def _validate(graph: Graph, r: _Run) -> None:
    """The checks before anything runs: the outputs are nodes, the graph has no cycle, the output
    formats are supported, the render scale is in (0, 1], the frames are finite."""
    if not graph.outputs:
        raise GraphError("the graph has no outputs")
    for name, pattern in graph.outputs.items():
        if name not in graph.nodes:
            raise GraphError(f"output {name!r} is not a node of the graph (its nodes: {', '.join(graph.nodes)})")
        if pattern is not None:
            check_format(os.fspath(pattern))
        topological_order(graph.nodes[name])  # (ValueError on a cycle)
    if not all(0. < s <= 1. for s in r.scale):
        raise GraphError(f"render scale {r.scale}: each component must be in (0, 1]")
    if r.frames is not None and (not r.frames or not all(math.isfinite(float(f)) for f in r.frames)):
        raise GraphError(f"frames {list(r.frames)}: give finite times")


def run(graph: Graph | str | os.PathLike, plugin_dirs: Sequence = (), *, frames: Sequence[float] | None = None,
        scale=None, use_env_path: bool = False, write: bool = True,
        report_path: str | os.PathLike | None = None) -> Result:
    """Run a graph (a Graph, or the path of a graph file): make its instances (inputs first), then
    render its outputs at each frame, and write their files (if write). frames and scale override the
    graph's. plugin_dirs: the plug-in directories (or .ofx.bundle paths), and $OFX_PLUGIN_PATH if
    use_env_path. A failure doesn't raise (a bad graph, file or plug-in directory, an unknown plug-in,
    a failed action...): the run ends with ok False and the report's "error", which is also logged.
    report_path: write the report there too (JSON), even after a failure. RuntimeError only if
    another ofxh host is initialized."""
    report: dict[str, Any] = {"schema": REPORT_SCHEMA, "schemaVersion": REPORT_SCHEMA_VERSION, "ok": False,
                              "error": None, "host": None, "project": None, "frames": None, "renderScale": None,
                              "nodes": {}, "outputs": {}}
    images: dict = {}
    written: list[str] = []
    try:
        if not isinstance(graph, Graph):
            graph = load(graph)
        r = _Run(graph, frames if frames is not None else graph.frames, scale)
        report.update(project=r.project.to_json(), renderScale=list(r.scale))
        _validate(graph, r)
        dirs = plugin_dirs_of(plugin_dirs)
        with host(dirs, use_env_path):
            report["host"] = {"name": ofxh.host().props["OfxPropName"][0]}
            _execute(graph, r, report, images, written, write)
        report["ok"] = True
    except (GraphError, ValueError, OSError, KeyError, TypeError, NotImplementedError) as e:
        report["error"] = str(e) if isinstance(e, GraphError) else f"{type(e).__name__}: {e}"
        log.error("%s", report["error"])
    if report_path is not None:
        try:
            with open(report_path, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=2, allow_nan=False)
                f.write("\n")
        except (OSError, ValueError) as e:
            report["ok"] = False
            report["error"] = report["error"] or f"the report: {e}"
            log.error("%s: %s", report_path, e)
    return Result(ok=report["ok"], report=report, images=images, files=written)


def _execute(graph: Graph, r: _Run, report: dict, images: dict, written: list, write: bool) -> None:
    made: list[_NodeState] = []  # the nodes with an instance (to close)
    begun: list[_NodeState] = []  # the nodes whose beginSequenceRender succeeded
    try:
        for name, node in graph.nodes.items():
            if id(node) in r.nodes:
                raise GraphError(f"node {name!r} is also node {r.nodes[id(node)].name!r}")
            r.nodes[id(node)] = _NodeState(name, node)
            report["nodes"][name] = r.nodes[id(node)].report
        outs = [r.nodes[id(graph.nodes[n])] for n in graph.outputs]
        order: list[_NodeState] = []
        for out in outs:
            for n in topological_order(out.node):
                if id(n) not in r.nodes:
                    raise GraphError(f"{n!r} is an input, but not a node of the graph")
                if r.nodes[id(n)] not in order:
                    order.append(r.nodes[id(n)])
        for ns in order:  # inputs first
            made.append(ns)
            r.setup(ns)
        fs = report["frames"] = _frames(r, outs)
        for ns in outs:
            pattern = graph.outputs[ns.name]
            if write and pattern is not None and len(fs) > 1 and not sequences.has_pattern(pattern):
                raise GraphError(f"output {ns.name!r}: {len(fs)} frames, but {pattern} has no frame number "
                                 "(e.g. ####)")
        p = r.project
        r.project_window = tuple(ofxh.canonical_to_pixel(
            (p.offset[0], p.offset[1], p.offset[0] + p.size[0], p.offset[1] + p.size[1]), r.scale,
            p.pixel_aspect_ratio))
        window = tuple(graph.window) if graph.window is not None else r.project_window
        # the frames each node will render, then its sequence render
        seen: set = set()
        for t in fs:
            for ns in outs:
                _plan(r, ns, t, window, seen)
        try:
            for ns in order:
                if not ns.planned:
                    continue
                first, last, step, ns.sequential = _sequence(ns.planned)
                st = r.call(ns, "beginSequenceRender", ns.instance.begin_sequence_render_action, first, last, step,
                            False, r.scale, ns.sequential)
                r.check(ns, "beginSequenceRender", st, first=first, last=last, step=step, sequential=ns.sequential)
                begun.append(ns)
            for t in fs:
                for ns in outs:
                    img = r.render(ns, t, window, r.scale)
                    written += _output(r, ns, t, img, window, graph.outputs[ns.name] if write else None, report,
                                       images)
                r.cache.clear()  # (the images are kept for the current frame only)
        finally:
            for ns in begun:
                first, last, step, _ = _sequence(ns.planned)
                try:
                    st = ns.instance.end_sequence_render_action(first, last, step, False, r.scale, ns.sequential)
                except ofxh.CallbackError:
                    st = ofxh.Stat.Failed
                ns.report["actions"].append({"action": "endSequenceRender", "ok": _ok(st), "status": _stat_str(st)})
    finally:
        for ns in reversed(made):
            if ns.instance is not None and ns.instance.alive:
                ns.report["params"] = _param_report(ns.instance)
                ns.instance.close()


def _output(r: _Run, ns: _NodeState, t: float, img: _Rendered | None, window, pattern: str | None,
            report: dict, images: dict) -> list[str]:
    """An output node's image at t: in the result (top-down float32, as ofxhost.render() gives them),
    in the report (its statistics, as hostDemo's report), and written to its file (over the whole
    window, the project by default, with the image at its place and transparent black around it)."""
    j: dict[str, Any] = {"time": t, "output": None, "files": []}
    report["outputs"].setdefault(ns.name, []).append(j)
    pixels = None
    if img is not None:
        j["output"] = _stats(img.pixels, img.bounds)
        del j["output"]["files"]
        scale = {np.dtype(np.uint8): 255., np.dtype(np.uint16): 65535.}.get(img.pixels.dtype, 1.)
        pixels = np.ascontiguousarray((img.pixels.astype(np.float32) / np.float32(scale))[::-1])
        images.setdefault(ns.name, {})[t] = Image(pixels, img.bounds,
                                                  _ip._ofx("premult", ns.instance.output_premultiplication))
    if pattern is None:
        return []
    w, h = window[2] - window[0], window[3] - window[1]
    nc = _ip._NCOMPS[ns.instance.clip("Output").components]
    full = np.zeros((h, w, nc), np.float32)
    if pixels is not None:
        b = _intersect(img.bounds, window)
        if not _empty(b):  # (top-down rows: from the top of the window, and of the image)
            full[window[3] - b[3]:window[3] - b[1], b[0] - window[0]:b[2] - window[0]] = \
                pixels[img.bounds[3] - b[3]:img.bounds[3] - b[1], b[0] - img.bounds[0]:b[2] - img.bounds[0]]
    path = r.graph.path(sequences.expand(pattern, sequences.frame_of(t), ns.instance.view_name(0)))
    files = write_image(path, full)
    j["files"] = files
    log.info("%s: wrote %s", ns.name, ", ".join(files))
    return files
