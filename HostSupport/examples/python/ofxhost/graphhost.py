"""The sample host.

Run a graph of plug-in instances (ofxhost.graph) over a frame sequence, in this process, on the HostSupport bindings
(the ofxh module), with the images in memory between the nodes.

    from ofxhost import graph, graphhost
    g = graph.load("graph.json")                       # or a Graph built in Python
    r = graphhost.run(g, ["/path/to/plugins"])         # writes the outputs' files
    r.ok, r.report, r.images["out"][0.0]               # the JSON report; {node: {time: Image}}

It is the template for a real host on ofxh (minimal_host.py is the minimal one): Host,
ImageEffectInstance, ClipInstance and Param subclasses, and the actions in a host's order, as Natron
calls them: the project load, time domains, sequence renders, frames needed and regions of
interest, identities, image sequences and file parameters. README.md says what it does, why, and
its report; the command line is ofxhost.cli (python -m ofxhost render graph.json). A graph is like
a script: it chooses the plug-ins that run and the files they read and write. Run only the graphs
you trust.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import logging
import math
import os
import sys
from collections.abc import Sequence
from typing import Any

import numpy as np

import ofxhost
from . import build, constants, imageio, pixels, rects, sequences, specs, status
from . import graph as g

__all__ = ["run", "Result", "GraphError", "host", "is_file_param", "REPORT_SCHEMA", "REPORT_SCHEMA_VERSION"]

ofxh = build.import_ofxh()
log = logging.getLogger("ofxhost")

REPORT_SCHEMA = "ofxhost.graphReport"
REPORT_SCHEMA_VERSION = 1

_VEGAS = bool(ofxh.build_config.get("OFX_EXTENSIONS_VEGAS"))  # (the default pattern's colours)
_OUTPUT = constants.kOfxImageEffectOutputClipName
_NO_VALUE = {"group", "page", "pushbutton", "parametric"}
_TIME_DOMAIN_CONTEXTS = {"general", "reader", "writer", "generator"}
_MAX_FRAME = 1e7


class GraphError(RuntimeError):
    """The graph can't be run: an unknown plug-in, a missing input file, an action that failed..."""


@dataclasses.dataclass
class Result:
    """The result of a run.

    Attributes:
        ok: the run succeeded.
        report: the JSON report (README.md).
        images: {output node: {time: Image}}, top-down float32 pixels of the rendered window.
        files: the files written.
    """
    ok: bool
    report: dict
    images: dict
    files: list


def _frames_in(ranges, t: float, where: str) -> list[float]:
    """The frames of frames-needed ranges [[t1, t2]...]: t1, t1 + 1... up to t2, and t2.

    None (the plug-in said nothing for the clip): [t]. Raises GraphError for a bound that isn't finite
    or is beyond +-1e7 (_MAX_FRAME).
    """
    out: list[float] = []
    for lo, hi in [[t, t]] if ranges is None else ranges:  # ([]: no frames)
        if not (abs(lo) <= _MAX_FRAME and abs(hi) <= _MAX_FRAME):  # (also NaN)
            raise GraphError(f"{where}: frames needed [{lo}, {hi}] at time {t}: the bounds must be finite, "
                             f"within +-{_MAX_FRAME:g}")
        f = float(lo)
        while f < hi - 1e-9:
            out.append(f)
            f += 1.
        out.append(float(hi))
    return list(dict.fromkeys(out))


# --- the host ---------------------------------------------------------------------------------

_LEVELS = {"fatal": logging.CRITICAL, "error": logging.ERROR, "warning": logging.WARNING,
           "question": logging.WARNING, "message": logging.INFO, "log": logging.DEBUG}


class Host(ofxh.Host):
    """The host.

    The messages go to the "ofxhost" logger; questions are answered yes (a batch host has nobody to ask).
    """

    def message(self, message_type, message_id, text):
        log.log(_LEVELS.get(message_type, logging.INFO), "%s", text)
        return ofxh.Stat.ReplyYes if message_type == "question" else ofxh.Stat.OK


@contextlib.contextmanager
def host(plugin_dirs: Sequence = (), use_env_path: bool = False):
    """ofxh initialized with a Host, shut down at the end (a context manager).

    ofxh has one host per process: the in-process backend's (ofxhost.inprocess) is shut down first.

    Args:
        plugin_dirs: the plug-in directories.
        use_env_path: also $OFX_PLUGIN_PATH.

    Raises:
        RuntimeError: another ofxh host is initialized.
    """
    if ofxh.initialized():
        backend = sys.modules.get("ofxhost.inprocess")
        if backend is not None and backend.session() is not None:
            backend.shutdown()
        else:
            raise RuntimeError("an ofxh host is already initialized (ofxh has one host per process): "
                               "call ofxh.shutdown() first")
    ofxh.init(Host(), [os.fspath(d) for d in plugin_dirs], use_std_paths=False, use_env_path=use_env_path)
    try:
        yield
    finally:
        ofxh.shutdown()


# --- the parameters ---------------------------------------------------------------------------

_READER_WRITER = {"reader", "writer", constants.kOfxImageEffectContextReader, constants.kOfxImageEffectContextWriter}


def is_file_param(desc, contexts) -> bool:
    """Is this a file parameter whose value may be a file name pattern, as in Natron?

    Args:
        desc: the parameter descriptor.
        contexts: the plug-in's contexts.

    Returns:
        True for an output file path (a string in file path mode, with
        kOfxParamPropStringFilePathExists = 0), and for the file and proxy parameters
        (kOfxImageEffectFileParamName, kOfxImageEffectProxyParamName) of a plug-in that has the
        reader or writer context.
    """
    mode = desc.props.get(constants.kOfxParamPropStringMode, ("",))[0]
    if desc.type != "string" or mode != constants.kOfxParamStringIsFilePath:
        return False
    if not desc.props.get(constants.kOfxParamPropStringFilePathExists, (1,))[0]:
        return True
    return desc.name in (constants.kOfxImageEffectFileParamName, constants.kOfxImageEffectProxyParamName) and \
        bool(_READER_WRITER & set(contexts))


class FileParam(ofxh.Param):
    """A file parameter.

    It holds the pattern, and gives the plug-in the file name at the time it asks for (the current time for
    paramGetValue), for view 0 (the only one), made absolute against base (the graph's directory; None: the current
    directory), as Natron resolves file parameters against the project's path. An empty value stays empty.
    """

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
        self.window = None  # the render window of the current render (pixels at the render scale)
        self.render_rod = None  # (time, view, RoD) during a render action: see Clip.region_of_definition()

    def timeline_get_bounds(self):
        return tuple(self.run.project.frame_range)

    def new_clip_instance(self, desc, index):
        return Clip(self.run, self.node, desc.name)

    def new_param(self, desc):
        return FileParam(self.run.graph.base) if is_file_param(desc, self.node.plugin.contexts) else None

    def _record(self, kind, message_type, message_id, text):
        self.node.report["messages"].append({"kind": kind, "type": message_type, "id": message_id, "text": text})
        log.log(_LEVELS.get(message_type, logging.INFO), "%s: %s", self.node.name, text)
        return ofxh.Stat.ReplyYes if message_type == "question" else ofxh.Stat.OK

    def message(self, message_type, message_id, text):
        return self._record("message", message_type, message_id, text)

    def persistent_message(self, message_type, message_id, text):
        return self._record("persistentMessage", message_type, message_id, text)


@dataclasses.dataclass
class _Rendered:
    """A node's image: bottom-up pixels at its output depth, and their bounds (pixels at the scale)."""
    pixels: np.ndarray
    bounds: tuple


class Clip(ofxh.ClipInstance):
    """A clip: an input is another node, an image sequence (Files), or what ofxhost.render() takes.

    That is a hostDemo input spec ("ramp", "checker:16", "file:x.pfm", "none"...), an array or an
    Image. An input that the graph doesn't give is disconnected.
    """

    def __init__(self, run: _Run, node: _NodeState, name: str):
        super().__init__()
        self.run = run
        src = node.sources.get(name)
        self.child: _NodeState | None = src if isinstance(src, _NodeState) else None
        self.files: _Files | None = src if isinstance(src, _Files) else None
        self.spec = src if isinstance(src, specs.Spec) else specs.Spec(type="none", text="none")
        # what is connected, for an image sequence or a test pattern: RGBA, unless the input has a
        # "#rgb" or "#alpha" suffix (another node: its output's components)
        self.input_components = self.files.components if self.files is not None else self.spec.components

    # what the clip is connected to: the properties a host sets
    @property
    def connected(self):
        return self.is_output or self.child is not None or self.files is not None or self.spec.type != "none"

    def _out(self):
        return self.child.instance.clip(_OUTPUT)

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
        # (image files too: RGB is opaque RGBA, grey is r = g = b = a, as hostDemo's files, unless the
        # input has a components suffix)
        return self.input_components

    @property
    def premult(self):
        if self.is_output:
            return self.instance.output_premultiplication
        if self.child is not None:
            return self.child.instance.output_premultiplication
        if not self.connected or self.components == "rgb" or self.input_components == "rgb":
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
        if self.spec.type == "file" and not self.is_output:
            return self.spec.file_format()
        return rects.to_pixels(self.run.project_rect(), (1., 1.), self.run.project.pixel_aspect_ratio)

    # the regions and images
    def _spec_at(self, time):
        """The input at a time, as hostDemo's input spec: the file of that frame for a sequence."""
        return self.files.spec(time) if self.files is not None else self.spec

    def _rod(self, time, view, rs):
        if self.child is not None:
            st, rod = self.child.instance.get_region_of_definition_action(time, rs, view)
            return tuple(rod) if status.succeeded(st) else (0., 0., 0., 0.)
        s = self._spec_at(time)
        if s.type == "none":
            return (0., 0., 0., 0.)
        if s.type == "rod":
            return s.rod
        if s.type == "file":
            return s.file_rod(self.aspect_ratio)
        return self.run.project_rect()

    def region_of_definition(self, time, view):
        if self.is_output:
            # during the render action, at its time and view: the RoD the host computed before it, so
            # that a plug-in's worker thread that asks for it (or fetches the output image first, which
            # needs it) runs no action (ofxh refuses actions on worker threads); else the RoD action
            rod = self.instance.render_rod
            if rod is not None and rod[0] == time and rod[1] == view:
                return rod[2]
            return super().region_of_definition(time, view)
        return self._rod(time, view, tuple(self.instance.render_scale))

    def image(self, time, view, region=None, rs=None):
        """The input's image at a time, as the plug-in fetches it.

        Args:
            time, view: the image's.
            region: canonical (None: the current render's region of interest, else the RoD),
                clipped to the RoD. Without tiles (the plug-in's or the clip's), the whole RoD,
                unless it is too large for an image: then the region, clipped to the render window
                if it is still too large, as hostDemo.
            rs: the render scale (None: the current action's).

        Returns:
            An ofxh.ImageData at the clip's depth and components, or None if the region is empty.

        Raises:
            GraphError: the image is too large, or the clip's depth and components can't be made.
        """
        if not self.connected or self.is_output:
            return None
        inst = self.instance
        rs = tuple(inst.render_scale) if rs is None else tuple(rs)
        par = self.aspect_ratio
        rod_c = self._rod(time, view, rs)
        rod = rects.to_pixels(rod_c, rs, par)
        tiles = inst.supports_tiles and self.supports_tiles
        if not tiles and rects.fits(rod):
            region = rod_c
        elif region is None:
            region = inst.rois.get(self.name, rod_c)
        b = rects.intersect(rects.to_pixels(rects.intersect(region, rod_c), rs, par), rod)
        if not tiles and not rects.fits(b) and inst.window is not None:
            b = rects.intersect(b, inst.window)
        if rects.is_empty(b):
            return None
        if not rects.fits(b):
            raise GraphError(f"{inst.node.name}.{self.name}: an image of {b[2] - b[0]}x{b[3] - b[1]} pixels at time "
                             f"{time} is too large")
        depth, comps = self.pixel_depth, self.components
        if self.child is not None:
            r = self.run.render(self.child, time, b, rs, view)
            if r is None:
                return None
            return ofxh.ImageData(pixels.convert(r.pixels, depth, comps), r.bounds, rod=rod)
        if depth not in pixels.BYTES or comps not in pixels.NCOMPS:
            raise GraphError(f"{inst.node.name}.{self.name}: can't make {depth} {comps} images")
        data = pixels.to_depth(specs.from_components(specs.pattern(self._spec_at(time), time, view, b, rod, rs,
                                                                   vegas=_VEGAS), self.input_components), depth, comps)
        return ofxh.ImageData(data, b, rod=rod)

    def get_image(self, time, view, bounds, plane="Colour"):
        if self.is_output:
            return super().get_image(time, view, bounds, plane)
        return self.image(time, view, None if bounds is None else tuple(bounds))


class _Files:
    """An image sequence input.

    Its frames, and the files read (as hostDemo's file inputs, pixels at scale 1: the render scale samples them).
    Raises GraphError for a missing or unreadable file.
    """

    def __init__(self, path: str, premult: str | None, components: str = "rgba"):
        self.path = path
        self.premult = premult
        self.components = components  # (a "#rgb" or "#alpha" suffix: what the files give the clip)
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
        self._specs: dict[str, Any] = {}  # path -> Spec, for the current frame

    def clear(self) -> None:
        """Forget the frames read (a still image is kept: it is the same at every frame)."""
        if not self.still:
            self._specs.clear()

    @staticmethod
    def _read(path: str) -> np.ndarray:
        try:
            return imageio.read_image(path)
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
            data = pixels.to_rgba(np.ascontiguousarray(self._read(path)[::-1]))  # (bottom-up)
            s = self._specs[path] = specs.Spec(type="file", text=path, data=data)
        return s


# --- a run ------------------------------------------------------------------------------------

@dataclasses.dataclass(eq=False)
class _NodeState:
    name: str
    node: g.Node
    plugin: Any = None
    instance: Any = None
    time_domain: tuple | None = None
    sources: dict = dataclasses.field(default_factory=dict)  # input clip -> _NodeState | _Files | Spec
    planned: set = dataclasses.field(default_factory=set)  # the frames it will render (beginSequenceRender)
    sequential: bool = False  # its beginSequenceRender's
    seq: tuple | None = None  # (first, last) of its begun sequence render, None outside
    last_render: float | None = None
    report: dict = dataclasses.field(default_factory=dict)


class _Run:
    def __init__(self, graph: g.Graph, frames, scale):
        self.graph = graph
        self.project = graph.project or g.Project()
        self.depth = graph.depth
        self.scale = rects.scale_pair(graph.scale if scale is None else scale)
        self.frames = frames
        self.nodes: dict[int, _NodeState] = {}  # id(Node) -> state
        self.cache: dict = {}  # (node name, time, view) -> _Rendered or None, for the current frame
        self._files: dict = {}  # (path, premult, components) -> _Files
        self.project_window: tuple = (0, 0, 0, 0)  # (the project in pixels at the render scale)

    def project_rect(self) -> tuple:
        """The project, in canonical coordinates."""
        p = self.project
        return (p.offset[0], p.offset[1], p.offset[0] + p.size[0], p.offset[1] + p.size[1])

    def source(self, ns: _NodeState, clip: str, v):
        """What an input clip is connected to.

        A node, an image sequence, or a hostDemo input (a spec string, an array or an Image; a "file:" spec or an
        array has pixels at the render scale).
        """
        if isinstance(v, g.Node):
            if id(v) not in self.nodes:
                raise GraphError(f"{ns.name}.{clip}: its input {v!r} is not a node of the graph")
            return self.nodes[id(v)]
        if isinstance(v, g.Files):
            return self.files(v)
        try:
            spec = specs.input_spec(clip, v, base=self.graph.base)
        except (ValueError, TypeError) as e:
            raise GraphError(f"{ns.name}.{clip}: invalid input {v if isinstance(v, str) else type(v).__name__} ({e})")
        spec.scale = self.scale
        return spec

    def files(self, f: g.Files) -> _Files:
        pattern, premult = f.split()
        comps = f.components
        path = os.path.abspath(self.graph.path(pattern))
        if (path, premult, comps) not in self._files:
            self._files[(path, premult, comps)] = _Files(path, premult, comps)
        return self._files[(path, premult, comps)]

    def end_frame(self) -> None:
        """Forget the images of the current frame: those rendered and those read from files."""
        self.cache.clear()
        for f in self._files.values():
            f.clear()

    @staticmethod
    def check(ns: _NodeState, action: str, st, **more) -> None:
        """Record an action in the node's report; raise GraphError if it failed."""
        ns.report["actions"].append({"action": action, "ok": status.succeeded(st), "status": status.name(st), **more})
        if not status.succeeded(st):
            raise GraphError(f"{ns.name}: the {action} action failed ({status.name(st)})")

    def call(self, ns: _NodeState, action: str, f, *args, **kw):
        """An action (an ofxh.CallbackError, a host callback that raised, is a failure)."""
        try:
            return f(*args, **kw)
        except ofxh.CallbackError as e:
            raise GraphError(f"{ns.name}: {action}: {e}") from e

    # --- the instances, inputs first
    def setup(self, ns: _NodeState) -> None:
        """Make the node's instance, as a host loads a project.

        The values and keys, then the create instance action ("if the values are being loaded from an old setup,
        that load should have taken place before the create instance action is called", ofxCore.h), the clip
        preferences, the push buttons (instanceChanged, "user edited", at the project's first frame), and the time
        domain.
        """
        inst = self._create(ns)
        self._load_params(ns, inst)
        self.reader_frame_range(ns)
        self.check(ns, "createInstance", self.call(ns, "createInstance", inst.create_instance_action))
        ns.report["paramChangedCount"] = inst.param_changed_count  # (0: the load sent no instanceChanged)
        self.clip_preferences(ns)
        # the push buttons, as a user presses them
        for name, v in ns.node.params.items():
            if v is None:
                t = float(self.project.frame_range[0])
                self.check(ns, "instanceChanged", self.call(ns, "instanceChanged", inst.press, name, t),
                           param=name, time=t)
        if inst.clip_preferences_dirty:
            self.clip_preferences(ns)
        self.time_domain(ns)

    def _create(self, ns: _NodeState):
        """The plug-in, the inputs and the instance (no action yet)."""
        node = ns.node
        major = node.version[0] if node.version is not None else -1
        ns.plugin = ofxh.get_plugin(node.plugin, int(major))
        if ns.plugin is None:
            raise GraphError(f"{ns.name}: plug-in {node.plugin}" + (f" version {major}" if major >= 0 else "") +
                             " not found")
        ns.report.update(plugin={"id": ns.plugin.id, "version": list(ns.plugin.version), "label": ns.plugin.label},
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
        return inst

    @staticmethod
    def _load_params(ns: _NodeState, inst) -> None:
        """The project load: the values and keys, before the create instance action, without instanceChanged."""
        names = set(inst.param_names)
        for name, v in ns.node.params.items():
            if name not in names:
                raise GraphError(f"{ns.name}: no parameter {name!r}")
            if v is None:
                if inst.param_type(name) != "pushbutton":
                    raise GraphError(f"{ns.name}.{name}: null presses a push button, but it is a "
                                     f"{inst.param_type(name)}")
                continue
            keys = None if isinstance(v, str) else g.keys_of(v)
            for t, x in (sorted(keys.values.items(), key=lambda kv: float(kv[0])) if keys else [(None, v)]):
                try:
                    st = inst.set_param(name, x, None if t is None else float(t))
                except (ValueError, TypeError, ofxh.CallbackError) as e:
                    raise GraphError(f"{ns.name}.{name}: {e}") from e
                if int(st) != ofxh.Stat.OK:
                    raise GraphError(f"{ns.name}.{name}: could not set {x!r} ({status.name(st)})")

    def reader_frame_range(self, ns: _NodeState) -> None:
        """Natron's Node::computeFrameRangeForReader: a reader's originalFrameRange from its files."""
        inst = ns.instance
        name = constants.kOfxImageEffectFileParamName
        p = inst.param(name) if name in inst.param_names else None
        if (not isinstance(p, FileParam) or p.pattern is None or "originalFrameRange" not in inst.param_names
                or inst.param_type("originalFrameRange") != "integer2d" or "originalFrameRange" in ns.node.params):
            return
        try:
            fs = sequences.frames(p.path(p.pattern)) if sequences.has_pattern(p.pattern) else []
        except ValueError as e:
            raise GraphError(f"{ns.name}.{name}: {e}") from None
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
        """The node's time domain.

        GetTimeDomain in the general, reader, writer and generator contexts (Natron's choice; the spec calls it in
        the general context only), else, or if the plug-in replies default, the spec's default. None: infinite (the
        project's frame range).
        """
        inst = ns.instance
        j = ns.report["timeDomain"] = {"action": None, "range": None}
        if ofxh.short_name("context", ofxh.ofx_name("context", ns.node.context)) in _TIME_DOMAIN_CONTEXTS:
            st, rng = self.call(ns, "getTimeDomain", inst.get_time_domain_action)
            j["action"] = status.name(st)
            if not status.succeeded(st):
                raise GraphError(f"{ns.name}: the getTimeDomain action failed ({status.name(st)})")
            if int(st) == ofxh.Stat.OK:
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
    def render_window(self, ns: _NodeState, t: float, window, rs, view: int) -> tuple:
        """The node's RoD at t, and its render window.

        Returns:
            (the RoD, the render window: window clipped to the RoD, or the whole RoD without tiles,
            unless it is too large: then window, as hostDemo).
        """
        inst = ns.instance
        st, rod = self.call(ns, "getRegionOfDefinition", inst.get_region_of_definition_action, t, rs, view)
        if not status.succeeded(st):
            raise GraphError(f"{ns.name}: the getRegionOfDefinition action failed at time {t} ({status.name(st)})")
        out = inst.clip(_OUTPUT)
        rod_px = rects.to_pixels(rod, rs, out.aspect_ratio)
        whole = not (inst.supports_tiles and out.supports_tiles) and rects.fits(rod_px)
        return tuple(rod), rod_px if whole else rects.intersect(window, rod_px)

    def render(self, ns: _NodeState, t: float, window, rs, view: int = 0) -> _Rendered | None:
        """The node's image at time t, at least over window (pixels at the render scale rs).

        Returns:
            The image, clipped to the node's RoD (its whole RoD without tiles); None if that is empty.
        """
        key = (ns.name, float(t), tuple(rs), view)
        if key in self.cache:
            cached = self.cache[key]
            if cached is None or rects.contains(cached.bounds, window):
                return cached
            window = rects.union(window, cached.bounds)
        rod, win = self.render_window(ns, t, window, rs, view)
        rec: dict[str, Any] = {"time": float(t), "renderWindow": None if rects.is_empty(win) else list(win),
                               "identity": None}
        ns.report["renders"].append(rec)
        if rects.is_empty(win):
            self.cache[key] = None
            return None
        if not rects.fits(win):
            raise GraphError(f"{ns.name}: a render window of {win[2] - win[0]}x{win[3] - win[1]} pixels is too large")
        identity, result = self._identity(ns, t, win, rs, view, rec)
        if not identity:
            result = self._render_action(ns, t, win, rs, view, rod, rec)
        self.cache[key] = result
        return result

    def _identity(self, ns: _NodeState, t: float, win, rs, view: int, rec: dict) -> tuple[bool, _Rendered | None]:
        """An identity isn't rendered: its output is its input's image.

        Returns:
            (is the node an identity there, and then its image, None if empty).
        """
        inst = ns.instance
        st, idr = self.call(ns, "isIdentity", inst.is_identity_action, t, win, rs, "none", view)
        if int(st) != ofxh.Stat.OK or idr.get("clip") not in inst.clips:
            return False, None
        time = _identity_time(ns, idr)
        out = inst.clip(_OUTPUT)
        rec["identity"] = {"clip": idr["clip"], "time": time}
        img = inst.clip(idr["clip"]).image(time, view, rects.to_canonical(win, rs, out.aspect_ratio), rs)
        if img is None:
            return True, None
        return True, _Rendered(pixels.convert(img.pixels, out.pixel_depth, out.components), tuple(img.bounds))

    def _render_inputs(self, ns: _NodeState, t: float, win, rs, view: int) -> None:
        """Render what the node needs from its input nodes, before its render action.

        That is the frames needed over the regions of interest, which are kept in inst.rois.
        """
        inst = ns.instance
        ranges, rois = self.needs(ns, t, win, rs, view)
        inst.rois = {c: tuple(r) for c, r in rois.items()}
        for name, clip, frames in _input_frames(ns, ranges, inst.rois, t):
            for f in frames:
                clip.image(f, view, inst.rois[name], rs)

    def needs(self, ns: _NodeState, t: float, win, rs, view: int) -> tuple[dict, dict]:
        """The node's getFramesNeeded and getRegionsOfInterest at t, for the window win (pixels at rs)."""
        inst = ns.instance
        st, ranges = self.call(ns, "getFramesNeeded", inst.get_frames_needed_action, t)
        st2, rois = self.call(ns, "getRegionsOfInterest", inst.get_regions_of_interest_action, t,
                              rects.to_canonical(win, rs, inst.clip(_OUTPUT).aspect_ratio), rs, view)
        if not (status.succeeded(st) and status.succeeded(st2)):
            raise GraphError(f"{ns.name}: the getFramesNeeded or getRegionsOfInterest action failed at time {t}")
        return ranges, rois

    def _render_action(self, ns: _NodeState, t: float, win, rs, view: int, rod, rec: dict) -> _Rendered:
        """The render action, after the inputs it needs, in the node's sequence render or its own."""
        inst = ns.instance
        inst.window = win
        try:
            self._render_inputs(ns, t, win, rs, view)
            # "kOfxImageEffectActionBeginSequenceRender has been called on the instance" (the render
            # action's precondition): outside a begun sequence that covers t (a render for a push
            # button, a fetch that wasn't planned), the render gets its own sequence, [t, t], not
            # sequential, as Natron's analysis renders
            covered = ns.seq is not None and ns.seq[0] - 1e-9 <= t <= ns.seq[1] + 1e-9
            if covered:  # sequential: the node's sequence is, and this render comes after its last one
                sequential = ns.sequential and (ns.last_render is None or t > ns.last_render)
                ns.last_render = t if ns.last_render is None else max(ns.last_render, t)
            else:
                sequential = False
                self.check(ns, "beginSequenceRender",
                           self.call(ns, "beginSequenceRender", inst.begin_sequence_render_action, t, t, step=1.,
                                     interactive=False, render_scale=rs, sequential=False),
                           first=t, last=t, step=1., sequential=False)
            rec["sequential"] = sequential
            inst.render_rod = (float(t), view, rod)
            try:
                st, planes = self.call(ns, "render", inst.render_action, t, win, rs, "none", sequential=sequential,
                                       interactive_render=False, draft=False, view=view)
                ns.report["actions"].append({"action": "render", "ok": int(st) == ofxh.Stat.OK,
                                             "status": status.name(st), "time": float(t), "sequential": sequential})
            finally:
                if not covered:
                    _end_sequence(ns, t, t, 1., rs, sequential=False)
        finally:
            inst.rois = {}
            inst.window = inst.render_rod = None
        image = planes.get("Colour")
        rec.update(ok=int(st) == ofxh.Stat.OK, status=status.name(st))
        if int(st) != ofxh.Stat.OK or image is None:
            raise GraphError(f"{ns.name}: the render action failed at time {t} ({status.name(st)}"
                             + ("" if image is not None else ", no output image") + ")")
        return _Rendered(image.pixels, tuple(image.bounds))


def _end_sequence(ns: _NodeState, first: float, last: float, step: float, rs, sequential: bool) -> None:
    """endSequenceRender, into the report (a failure doesn't stop the run)."""
    try:
        st = ns.instance.end_sequence_render_action(first, last, step=step, interactive=False, render_scale=rs,
                                                    sequential=sequential)
    except ofxh.CallbackError:
        st = ofxh.Stat.Failed
    ns.report["actions"].append({"action": "endSequenceRender", "ok": status.succeeded(st),
                                 "status": status.name(st)})


def _identity_time(ns: _NodeState, idr: dict) -> float:
    """The time of an identity (isIdentity's); GraphError if it isn't finite."""
    t = float(idr["time"])
    if not math.isfinite(t):
        raise GraphError(f"{ns.name}: isIdentity gave the time {t} of clip {idr['clip']}")
    return t


def _param_report(inst) -> dict:
    """The parameter values after the run, in the form of a graph node's params.

    {"keys": ...} for the parameters with keys, {"file": pattern} for a file parameter.
    """
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
            out[name] = {"keys": [[t, g.json_value(v)] for t, v in inst.param_curve(name)]}
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


def _plan(r: _Run, ns: _NodeState, t: float, window, seen: dict) -> None:
    """Plan a render of the node at t over window, and the renders of its inputs for it.

    t is added to the frames the node will render if its RoD meets window (pixels at the render
    scale: the output's window, or the region of interest of the node that uses it; its whole RoD
    without tiles), and its inputs are planned: the frames needed over its regions of interest, or
    the identity's clip and time. Only actions that don't render.

    seen: the union of the windows already planned for each (node, time). A window inside it isn't
    planned again, and a larger one plans the new union: in a deep DAG whose regions of interest
    differ along each path (diamonds), the plans of a node are bounded by the growths of its unions,
    not by the number of paths that reach it."""
    key = (ns.name, t)
    window = tuple(window)
    if rects.is_empty(window):
        return  # (nothing is asked: an empty window doesn't grow the union)
    old = seen.get(key)
    if old is not None:
        if rects.contains(old, window):
            return
        window = rects.union(old, window)
    seen[key] = window
    _plan_window(r, ns, t, window, seen)


def _plan_window(r: _Run, ns: _NodeState, t: float, window, seen: dict) -> None:
    """_plan() of a window that isn't planned yet."""
    inst = ns.instance
    _, win = r.render_window(ns, t, window, r.scale, 0)
    if rects.is_empty(win):
        return  # (nothing to render)
    st, idr = r.call(ns, "isIdentity", inst.is_identity_action, t, win, r.scale, "none", 0)
    if int(st) == ofxh.Stat.OK and idr.get("clip") in inst.clips:
        clip = inst.clip(idr["clip"])
        time = _identity_time(ns, idr)
        if clip.child is not None:
            _plan(r, clip.child, time, win, seen)
        return
    ns.planned.add(t)
    ranges, rois = r.needs(ns, t, win, r.scale, 0)
    for name, clip, frames in _input_frames(ns, ranges, rois, t):
        roi = rects.to_pixels(rois[name], r.scale, clip.aspect_ratio)
        for f in frames:
            _plan(r, clip.child, f, roi, seen)


def _input_frames(ns: _NodeState, ranges: dict, rois: dict, t: float):
    """(clip name, clip, frames) of the node's connected input clips that have a region of interest."""
    for name, clip in ns.instance.clips.items():
        if clip.child is not None and not clip.is_output and name in rois:
            yield name, clip, _frames_in(ranges.get(name), t, f"{ns.name}.{name}")


def _sequence(frames) -> tuple[float, float, float, bool]:
    """(first, last, step, sequential) of the frames a node renders.

    Sequential if they increase by a constant step (step 1 otherwise).
    """
    fs = sorted(frames)
    step = fs[1] - fs[0] if len(fs) > 1 else 1.
    sequential = all(abs(b - a - step) < 1e-9 for a, b in zip(fs, fs[1:]))
    return fs[0], fs[-1], step if sequential else 1., sequential


def _validate(graph: g.Graph, r: _Run) -> None:
    """The checks before anything runs.

    The outputs are nodes, the graph has no cycle, the output formats are supported, the render scale is in (0, 1],
    the frames are finite, the project's pixel aspect ratio is positive.
    """
    if not graph.outputs:
        raise GraphError("the graph has no outputs")
    for name, file_pattern in graph.outputs.items():
        if name not in graph.nodes:
            raise GraphError(f"output {name!r} is not a node of the graph (its nodes: {', '.join(graph.nodes)})")
        if file_pattern is not None:
            imageio.check_format(os.fspath(file_pattern))
        g.topological_order(graph.nodes[name])  # (ValueError on a cycle)
    if not all(0. < s <= 1. for s in r.scale):
        raise GraphError(f"render scale {r.scale}: each component must be in (0, 1]")
    if r.frames is not None and (not r.frames or not all(math.isfinite(float(f)) for f in r.frames)):
        raise GraphError(f"frames {list(r.frames)}: give finite times")
    par = r.project.pixel_aspect_ratio
    if not (math.isfinite(par) and par > 0):
        raise GraphError(f"project pixel aspect ratio {par}: it must be positive")


def run(graph: g.Graph | str | os.PathLike, plugin_dirs: Sequence = (), *, frames: Sequence[float] | None = None,
        scale=None, use_env_path: bool = False, write: bool = True,
        report_path: str | os.PathLike | None = None) -> Result:
    """Run a graph: make its instances (inputs first), then render its outputs at each frame.

    A failure doesn't raise (a bad graph, file or plug-in directory, an unknown plug-in, a failed
    action...): the run ends with ok False and the report's "error", which is also logged.

    Args:
        graph: a Graph, or the path of a graph file.
        plugin_dirs: the plug-in directories (or .ofx.bundle paths).
        frames: the frames to render, instead of the graph's.
        scale: the render scale, s or (sx, sy), instead of the graph's.
        use_env_path: also the plug-ins of $OFX_PLUGIN_PATH.
        write: write the outputs' files.
        report_path: write the report there too (JSON), even after a failure.

    Returns:
        The Result.

    Raises:
        RuntimeError: another ofxh host is initialized.
    """
    report: dict[str, Any] = {"schema": REPORT_SCHEMA, "schemaVersion": REPORT_SCHEMA_VERSION, "ok": False,
                              "error": None, "host": None, "project": None, "frames": None, "renderScale": None,
                              "nodes": {}, "outputs": {}}
    images: dict = {}
    written: list[str] = []
    try:
        if not isinstance(graph, g.Graph):
            try:
                graph = g.load(graph)
            except (json.JSONDecodeError, UnicodeDecodeError) as e:
                raise GraphError(f"{os.fspath(graph)}: not a JSON graph ({e})") from None
        r = _Run(graph, frames if frames is not None else graph.frames, scale)
        report.update(project=r.project.to_json(), renderScale=list(r.scale))
        _validate(graph, r)
        dirs = ofxhost.plugin_dirs(plugin_dirs)
        with host(dirs, use_env_path):
            report["host"] = {"name": ofxh.host().props[constants.kOfxPropName][0]}
            _execute(graph, r, report, images, written, write)
        report["ok"] = True
    except (GraphError, ValueError, OSError) as e:
        report["error"] = str(e) if isinstance(e, GraphError) else f"{type(e).__name__}: {e}"
        log.error("%s", report["error"])
    if report_path is not None:
        try:
            ofxhost.write_json(report_path, report)
        except (OSError, ValueError) as e:
            report["ok"] = False
            report["error"] = report["error"] or f"the report: {e}"
            log.error("%s: %s", report_path, e)
    return Result(ok=report["ok"], report=report, images=images, files=written)


def _node_states(graph: g.Graph, r: _Run, report: dict) -> tuple[list[_NodeState], list[_NodeState]]:
    """(the outputs' states, every node's state in topological order, inputs first)."""
    for name, node in graph.nodes.items():
        if id(node) in r.nodes:
            raise GraphError(f"node {name!r} is also node {r.nodes[id(node)].name!r}")
        r.nodes[id(node)] = _NodeState(name, node)
        report["nodes"][name] = r.nodes[id(node)].report
    outs = [r.nodes[id(graph.nodes[n])] for n in graph.outputs]
    order: list[_NodeState] = []
    for out in outs:
        for n in g.topological_order(out.node):
            if id(n) not in r.nodes:
                raise GraphError(f"{n!r} is an input, but not a node of the graph")
            if r.nodes[id(n)] not in order:
                order.append(r.nodes[id(n)])
    return outs, order


def _execute(graph: g.Graph, r: _Run, report: dict, images: dict, written: list, write: bool) -> None:
    made: list[_NodeState] = []  # the nodes with an instance (to close)
    try:
        outs, order = _node_states(graph, r, report)
        for ns in order:  # inputs first
            made.append(ns)
            r.setup(ns)
        r.end_frame()  # (the images rendered for the push buttons)
        fs = report["frames"] = _frames(r, outs)
        for ns in outs:
            pattern = graph.outputs[ns.name]
            if write and pattern is not None and len(fs) > 1:
                _check_distinct_files(ns, pattern, fs)
        r.project_window = rects.to_pixels(r.project_rect(), r.scale, r.project.pixel_aspect_ratio)
        window = tuple(graph.window) if graph.window is not None else r.project_window
        # the frames each node will render, then its sequence render
        seen: dict = {}  # (node name, time) -> the union of the windows planned
        for t in fs:
            for ns in outs:
                _plan(r, ns, t, window, seen)
            r.end_frame()  # (the regions of definition read the files)
        _render_frames(r, order, outs, fs, window, graph.outputs if write else {}, report, images, written)
    finally:
        for ns in reversed(made):
            if ns.instance is not None and ns.instance.alive:
                ns.report["params"] = _param_report(ns.instance)
                ns.instance.close()


def _output_name(ns: _NodeState, pattern: str, t: float) -> str:
    """The file name of an output node's image at t (the frame of t is floor(t + 0.5))."""
    return sequences.expand(pattern, sequences.frame_of(t), ns.instance.view_name(0))


def _check_distinct_files(ns: _NodeState, pattern: str, fs: list[float]) -> None:
    """Raise GraphError if two of the times would write the same file."""
    if not sequences.has_pattern(pattern):
        raise GraphError(f"output {ns.name!r}: {len(fs)} frames, but {pattern} has no frame number (e.g. ####)")
    by_file: dict[str, float] = {}
    for t in fs:
        f = _output_name(ns, pattern, t)
        if by_file.setdefault(f, t) != t:
            raise GraphError(f"output {ns.name!r}: the times {by_file[f]:.12g} and {t:.12g} would both write {f}")


def _render_frames(r: _Run, order: list[_NodeState], outs: list[_NodeState], fs: list[float], window,
                   patterns: dict, report: dict, images: dict, written: list) -> None:
    """Render the frames: each node's sequence render, the outputs at each frame, the sequences' ends.

    A node's sequence covers the frames it plans to render; the ends are those of the sequences that
    began.
    """
    begun: list[_NodeState] = []  # the nodes whose beginSequenceRender succeeded
    try:
        for ns in order:
            if not ns.planned:
                continue
            first, last, step, ns.sequential = _sequence(ns.planned)
            st = r.call(ns, "beginSequenceRender", ns.instance.begin_sequence_render_action, first, last, step=step,
                        interactive=False, render_scale=r.scale, sequential=ns.sequential)
            r.check(ns, "beginSequenceRender", st, first=first, last=last, step=step, sequential=ns.sequential)
            begun.append(ns)
            ns.seq = (first, last)
        for t in fs:
            for ns in outs:
                img = r.render(ns, t, window, r.scale)
                written += _output(r, ns, t, img, window, patterns.get(ns.name), report, images)
            r.end_frame()
    finally:
        for ns in begun:
            ns.seq = None
            first, last, step, _ = _sequence(ns.planned)
            _end_sequence(ns, first, last, step, r.scale, sequential=ns.sequential)


def _output(r: _Run, ns: _NodeState, t: float, img: _Rendered | None, window, pattern: str | None,
            report: dict, images: dict) -> list[str]:
    """An output node's image at t.

    In the result (top-down float32, as ofxhost.render() gives them), in the report (its statistics, as hostDemo's
    report), and written to its file (over the whole window, the project by default, with the image at its place and
    transparent black around it).
    """
    j: dict[str, Any] = {"time": t, "output": None, "files": []}
    report["outputs"].setdefault(ns.name, []).append(j)
    top_down = None
    if img is not None:
        j["output"] = pixels.stats(img.pixels, img.bounds)
        del j["output"]["files"]
        top_down = np.ascontiguousarray(pixels.to_float(img.pixels)[::-1])
        images.setdefault(ns.name, {})[t] = ofxhost.Image(top_down, img.bounds,
                                                  ofxh.ofx_name("premult", ns.instance.output_premultiplication))
    if pattern is None:
        return []
    w, h = window[2] - window[0], window[3] - window[1]
    nc = pixels.NCOMPS[ns.instance.clip(_OUTPUT).components]
    full = np.zeros((h, w, nc), np.float32)
    if top_down is not None:
        b = rects.intersect(img.bounds, window)
        if not rects.is_empty(b):  # (top-down rows: from the top of the window, and of the image)
            full[window[3] - b[3]:window[3] - b[1], b[0] - window[0]:b[2] - window[0]] = \
                top_down[img.bounds[3] - b[3]:img.bounds[3] - b[1], b[0] - img.bounds[0]:b[2] - img.bounds[0]]
    path = r.graph.path(_output_name(ns, pattern, t))
    files = imageio.write_image(path, full)
    j["files"] = files
    log.info("%s: wrote %s", ns.name, ", ".join(files))
    return files
