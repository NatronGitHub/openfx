"""hostDemo's command line on the in-process backend (ofxhost.inprocess, on the ofxh module).

A drop-in for the hostDemo binary in test scripts.

    python -m ofxhost.hostdemo [hostDemo's options] [[-s] name=value]...
    HostSupport/examples/python/hostdemo-inprocess [hostDemo's options]...   (the same, as a sh script)
    python -m ofxhost.hostdemo --batch FILE|- [-j N] [--timeout S] [--reuse-host]...  (batch mode)

It takes hostDemo's options with the same meaning and the same errors (see -h), writes the same
files, the same --report and --describe JSON, prints the plug-ins' messages as hostDemo does, and
exits with the same status. As hostDemo, it reads the whole command line before it imports ofxh
(but at --multiplane, which needs the Nuke extension), runs one command line per process, and
unloads the plug-ins at the end. README.md lists the few differences; the batch mode
(ofxhost._batch) runs many command lines in one Python.
"""

from __future__ import annotations

import ctypes
import os
import sys
from collections.abc import Sequence

from . import specs, split_components, write_json
from .constants import kOfxBitDepthByte, kOfxBitDepthFloat, kOfxBitDepthShort, kOfxImageEffectContextFilter
from .specs import CLIP_LENGTH, parse_double, parse_int, parse_n

__all__ = ["main", "run", "run_batch", "is_batch", "scan_options", "USAGE"]

_DEFAULT_PLUGIN = "net.sf.openfx.invertPlugin"

USAGE = """\
Usage: {prog} [options] [[-s] name=value[,value...]]...
Render an OFX plug-in found in OFX_PLUGIN_PATH (default: at t=0 and 1, on PAL SD inputs), with
hostDemo's command line, in-process on the HostSupport Python bindings (ofxhost.hostdemo).
  -h                 print this help and exit
  -p id              plug-in identifier (default: net.sf.openfx.invertPlugin)
  -m major           plug-in major version (default: the highest one)
  -c filter|general  context (default: filter)
  -o prefix          write <prefix>.<t>[l|r].ppm|pfm (default: Output)
  --format ppm|pfm   output format: plain 8-bit PPM (default), or float PFM
  -d byte|short|float  input depth before clip preferences (default: byte)
  -i Clip=spec       input: default|solid:r,g,b,a|ramp|checker:N|rod:x1,y1,x2,y2|none, or
                     file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque], then
                     optionally #rgb, #alpha or #rgba: the components of the input (default RGBA)
  -i Clip@Plane=spec an extra plane of an input (with --multiplane): Plane is ForwardMotionVector,
                     BackMotionVector, StereoDisparityLeft or StereoDisparityRight, and its two
                     components are the first two of spec (any -i spec but none)
  --frames t,...     render (or query) at these times (default: 0,1)
  --scale s[,sy]     render scale, in (0,1] (default: 1)
  --window x1,y1,x2,y2  render window, in pixels at the render scale, clipped to the RoD
                     (default: the RoD clipped to the project). A plug-in or output clip
                     without tiles (kOfxImageEffectPropSupportsTiles=0) renders its whole RoD,
                     and gets whole-RoD input images
  --query            only call the query actions (RoD, identity, frames needed, RoI)
  -s name=value      set a parameter after createInstance, with instanceChanged (at time 0)
  -s name@t=value    set a key at time t (numeric parameters that animate: linear
                     interpolation, rounded for integers, stepped for booleans and choices),
                     with instanceChanged at time t
  -s button[@t]      press a push button (instanceChanged, at time t)
  --report file.json write a JSON report of the run (even if it failed)
  --describe file.json  describe the plug-in as JSON (no instance, no render)
  --ofx14            be an OFX 1.4 host: only the OFX 1.4 host properties and suites,
                     no OfxSetHost, no StrChoice or Bytes params, one view
  --ofx11            (hostDemo only: an OFX 1.1 host such as VEGAS Pro; refused here)
  --opengl-describe  with --describe only: say the host supports OpenGL render
                     (kOfxImageEffectPropOpenGLRenderSupported "true"), to describe OpenGL-only plug-ins
  --multiplane       be a multi-plane host (kFnOfxImageEffectPropMultiPlanar, the plane suites,
                     the GetClipComponents action; Nuke-extension builds only)
  --no-normalised-defaults  be a host without normalised parameter defaults: double params
                     have no kOfxParamPropDefaultCoordinateSystem, their defaults are canonical
  --no-std-paths     only look for plug-ins in OFX_PLUGIN_PATH
  --no-cache         (no-op: ofxh has no plug-in cache file)
  --quiet            print only the plug-in's messages on stdout
  --allow-unwritten  don't fail on unwritten (NaN) output pixels (float only)
Environment: OFX_HOSTDEMO_KEEP_LOADED=1 keeps the scanned plug-ins loaded (by default they
are unloaded after the scan, and loaded again at their first use). OFXHOST_PRELOAD: libraries
to load first (os.pathsep-separated).
Batch mode: --batch FILE|- [-j N] [--timeout SECONDS] [--reuse-host|--fresh-host-per-plugin]
[--batch-report FILE] runs many command lines (one per line) in one Python ({prog} --batch - -h).
Formats and JSON schemas: HostSupport/examples/python/README.md. Exit status: 0 on success.
"""

_VALUE_OPTIONS = ("-p", "-m", "-c", "-o", "-s", "-d", "-i", "--format", "--report", "--describe", "--frames",
                  "--scale", "--window")
# the options without a value: (the key in _parse()'s result, its value); --no-cache is a no-op
_FLAGS = {"--no-std-paths": ("std_paths", False), "--quiet": ("quiet", True),
          "--allow-unwritten": ("allow_unwritten", True), "--query": ("query", True), "--ofx14": ("ofx14", True),
          "--opengl-describe": ("opengl_describe", True), "--no-normalised-defaults": ("normalised_defaults", False),
          "--multiplane": ("multiplane", True), "--no-cache": (None, None)}
# -i Clip@Plane=spec: the short plane names (the kPlanes table of hostDemo's parseInputPlaneArg())
_PLANES = ("ForwardMotionVector", "BackMotionVector", "StereoDisparityLeft", "StereoDisparityRight")
_DEPTHS = {"byte": "byte", "short": "short", "float": "float", kOfxBitDepthByte: "byte", kOfxBitDepthShort: "short",
           kOfxBitDepthFloat: "float"}


class _Exit(Exception):
    def __init__(self, status: int):
        super().__init__(status)
        self.status = status


def _err(text: str) -> None:
    sys.stdout.flush()
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


def _fail(text: str) -> None:
    """A usage error: hostDemo's message, and exit status 1."""
    _err(text)
    raise _Exit(1)


def _write_json(path: str, doc: dict) -> bool:
    """writeJson: False (after an error message) if it couldn't be written."""
    try:
        write_json(path, doc)
        return True
    except (OSError, ValueError):
        _err(f"Error: cannot write {path}")
        return False


def _preload() -> None:
    for lib in os.environ.get("OFXHOST_PRELOAD", "").split(os.pathsep):
        if lib:
            try:
                ctypes.CDLL(lib, mode=getattr(ctypes, "RTLD_GLOBAL", 0))
            except OSError as e:
                _err(f"Warning: OFXHOST_PRELOAD: cannot load {lib} ({e})")


def host_dirs() -> tuple:
    """The plug-in directories of $OFX_PLUGIN_PATH that exist (hostDemo ignores the others), absolute."""
    dirs = [d for d in os.environ.get("OFX_PLUGIN_PATH", "").split(os.pathsep) if d]
    return tuple(dict.fromkeys(os.path.abspath(d) for d in dirs if os.path.isdir(d)))


def scan_options(args: Sequence[str]) -> tuple[str, set]:
    """A hostDemo command line, without parsing the values.

    Returns:
        (its plug-in: the last -p, as hostDemo's parsing keeps, or hostDemo's default; the options
        that aren't values of another option, e.g. not the "--batch" of "-o --batch").
    """
    plugin = _DEFAULT_PLUGIN
    flags: set = set()
    i = 0
    while i < len(args):
        if args[i] in _VALUE_OPTIONS:
            if args[i] == "-p" and i + 1 < len(args):
                plugin = args[i + 1]
            i += 1  # (a value, which may be "-p")
        elif args[i].startswith("-"):
            flags.add(args[i])
        i += 1
    return plugin, flags


def _parse_input_plane(o: dict, v: str) -> None:
    """-i Clip@Plane=spec, checked as hostDemo checks it: into o["planes"], (clip, plane, specs.Spec)."""
    at, eq = v.find("@"), v.find("=")
    clip, plane = v[:at], v[at + 1:eq]
    error = "no clip name"
    spec = None
    if at > 0:
        error = ""
        if plane not in _PLANES:
            error = f'unknown plane "{plane}"'
        elif split_components(v[eq + 1:])[1] is not None:  # (the plane's components are its own)
            error = "a plane has no #rgb/#alpha/#rgba suffix"
        else:
            try:
                spec = specs.parse_spec(v[eq + 1:])
                if spec.type == "none":
                    error = "a plane can't be none"
            except specs.SpecError as e:
                error = str(e)
    if error:
        _fail(f'Error: invalid input plane "{v}" ({error}), expected '
              "Clip@ForwardMotionVector|BackMotionVector|StereoDisparityLeft|StereoDisparityRight=spec")
    if any((c, p) == (clip, plane) for c, p, _ in o["planes"]):
        _fail(f'Error: input plane "{clip}@{plane}" given twice (-i)')
    o["planes"].append((clip, plane, spec))


def _parse_input(o: dict, v: str, seen: set) -> None:
    """-i Clip=spec"""
    if "=" in v and v.find("@") != -1 and v.find("@") < v.find("="):
        _parse_input_plane(o, v)
        return
    eq = v.find("=")
    error = "no clip name"
    spec = None
    if eq > 0:
        try:
            spec = specs.input_spec(v[:eq], v[eq + 1:], resolve=False)
        except specs.SpecError as e:
            error = str(e)
    if spec is None:
        _fail(f'Error: invalid input "{v}" ({error}), expected {specs.INPUT_FORMS}, then optionally #rgb|#alpha|#rgba')
    if v[:eq] in seen:
        _fail(f'Error: input clip "{v[:eq]}" given twice (-i)')
    seen.add(v[:eq])
    o["inputs"].append((v[:eq], spec))


def _parse_frames(v: str) -> list[float]:
    """--frames t0,t1,...: distinct times, also in the output file names (12 significant digits)."""
    frames: list[float] = []
    for x in v.split(","):
        f = parse_double(x)
        if f is None or f in frames:
            _fail(f'Error: invalid frames "{v}", expected distinct times t0,t1,...')
        frames.append(f)
    n = specs.frame_named_twice(frames)
    if n is not None:
        _fail(f'Error: invalid frames "{v}": {n} is given twice to 12 significant digits (the output file names)')
    return frames


def _parse_scale(v: str) -> tuple:
    """--scale s[,sy], in (0,1]"""
    s = parse_n(v, 1, parse_double)
    s = s * 2 if s else parse_n(v, 2, parse_double)
    if not s or not specs.valid_scale(s):
        _fail(f'Error: invalid render scale "{v}", {specs.SCALE_EXPECTED}')
    return (s[0], s[1])


def _parse_window(v: str) -> tuple:
    """--window x1,y1,x2,y2"""
    w = parse_n(v, 4, parse_int)
    if not specs.valid_window(w):
        _fail(f'Error: invalid render window "{v}", {specs.WINDOW_EXPECTED}')
    return tuple(w)


def _parse_value(o: dict, arg: str, v: str, seen: set) -> None:
    """The value v of the option arg (one of _VALUE_OPTIONS), into o."""
    if arg in ("-p", "-o", "--report", "--describe"):
        o[{"-p": "plugin", "-o": "prefix", "--report": "report", "--describe": "describe"}[arg]] = v
    elif arg == "-s":
        o["assignments"].append(v)
    elif arg == "-m":
        m = parse_int(v)
        if m is None or m < 0:
            _fail(f'Error: invalid major version "{v}"')
        o["major"] = m
    elif arg == "-c":
        if v not in specs.CONTEXTS:
            _fail(f'Error: unsupported context "{v}", expected filter or general')
        o["context"] = specs.CONTEXTS[v]
    elif arg == "--format":
        if v not in ("ppm", "pfm"):
            _fail(f'Error: unsupported format "{v}", expected ppm or pfm')
        o["format"] = v
    elif arg == "-d":
        if v not in _DEPTHS:
            _fail(f'Error: unsupported depth "{v}", expected byte, short or float')
        o["depth"] = _DEPTHS[v]
    elif arg == "-i":
        _parse_input(o, v, seen)
    elif arg == "--frames":
        o["frames"] = _parse_frames(v)
    elif arg == "--scale":
        o["scale"] = _parse_scale(v)
    else:  # --window
        o["window"] = _parse_window(v)


def _parse(argv: list[str], prog: str) -> dict:
    """hostDemo's command line (run() in hostDemo.cpp), parsed as hostDemo parses it.

    Raises _Exit (after the help, or a usage error).
    """
    o = {"plugin": _DEFAULT_PLUGIN, "major": -1, "context": kOfxImageEffectContextFilter, "prefix": "Output",
         "depth": "byte", "assignments": [], "inputs": [], "report": "", "describe": "", "std_paths": True,
         "planes": [], "multiplane": False, "quiet": False, "ofx14": False, "opengl_describe": False,
         "normalised_defaults": True, "allow_unwritten": False, "format": "ppm", "frames": None, "scale": (1., 1.),
         "window": None, "query": False}
    seen: set = set()
    a = 0
    while a < len(argv):
        arg = argv[a]
        if arg in ("-h", "--help"):
            sys.stdout.write(USAGE.format(prog=prog))
            raise _Exit(0)
        if arg == "--multiplane":
            _check_multiplane()
        if arg == "--ofx11":
            # hostDemo's OFX 1.1 profile changes the plug-in descriptors and instances too (no
            # kOfxImageEffectInstancePropSequentialRender): ofxh.init() has no such profile
            _fail("Error: --ofx11: the in-process backend has no OFX 1.1 host profile: use hostDemo")
        if arg in _FLAGS:
            key, value = _FLAGS[arg]
            if key is not None:
                o[key] = value
        elif arg in _VALUE_OPTIONS:
            if a + 1 >= len(argv):
                _err(f"Error: option {arg} requires an argument")
                sys.stdout.write(USAGE.format(prog=prog))
                raise _Exit(1)
            a += 1
            _parse_value(o, arg, argv[a], seen)
        elif arg and arg[0] != "-" and "=" in arg:
            o["assignments"].append(arg)
        else:
            _err(f'Error: unknown argument "{arg}"')
            sys.stdout.write(USAGE.format(prog=prog))
            raise _Exit(1)
        a += 1
    if o["frames"] is None:
        o["frames"] = [0., CLIP_LENGTH]
    for spec in [spec for _, spec in o["inputs"]] + [spec for _, _, spec in o["planes"]]:
        spec.scale = o["scale"]  # (the pixels of a file input are pixels at the render scale)
    if o["report"] and o["describe"]:
        _fail("Error: --report and --describe can't be used together (--describe creates no instance)")
    if o["opengl_describe"] and not o["describe"]:
        _fail("Error: --opengl-describe needs --describe (hostDemo can't render with OpenGL)")
    if o["planes"] and not o["multiplane"]:
        _fail("Error: -i Clip@Plane=spec needs --multiplane")
    if o["multiplane"] and o["ofx14"]:
        _fail("Error: --multiplane and --ofx14 can't be used together (OFX 1.4 has no plane suite)")
    return o


def _check_multiplane() -> None:
    """--multiplane needs the Nuke extension, checked at the option as hostDemo checks it (this imports ofxh)."""
    from .build import OfxhNotFound, import_ofxh
    try:
        ofxh = import_ofxh(os.environ.get("OFX_PLUGIN_PATH", ""), for_cli=True)
    except OfxhNotFound as e:
        _fail(f"Error: {e}")
    if not ofxh.build_config.get("OFX_EXTENSIONS_NUKE"):
        _fail("Error: --multiplane needs a hostDemo built with OFX_EXTENSIONS_NUKE")


def _dump(ofxh) -> None:
    """A short PluginCache::dumpToStdOut(): the plug-ins and their contexts."""
    plugins = sorted(ofxh.plugins(), key=lambda p: (p.id, tuple(p.version)))
    if not plugins:
        print("No Plug-ins Found.")
    for p in plugins:
        print(f"Plug-in:{p.id}")
        print("Contexts:")
        for c in sorted(ofxh.ofx_name("context", c) for c in p.contexts):
            print(f"\t* {c}")
    sys.stdout.flush()


def run(argv: list[str], prog: str = "hostdemo", shutdown: bool = True) -> int:
    """Run hostDemo's command line in-process.

    Args:
        argv: the command line, without the program name. With --batch, the batch mode's options
            (ofxhost._batch).
        prog: the program name, for the help.
        shutdown: shut the in-process backend's ofxh host down at the end (the plug-ins' unload
            action), as at the end of hostDemo; False keeps it for the next run in this process (the
            batch mode).

    Returns:
        The exit status.
    """
    if is_batch(argv):
        from ._batch import main as batch_main
        return batch_main(argv)
    try:
        o = _parse(argv, prog)  # (-h and the usage errors without ofxh, as hostDemo's)
    except _Exit as e:
        return e.status
    from .build import OfxhNotFound, import_ofxh
    try:
        import_ofxh(os.environ.get("OFX_PLUGIN_PATH", ""), for_cli=True)
    except OfxhNotFound as e:
        _err(f"Error: {e}")
        return 1
    _preload()
    from . import inprocess as ip
    keep = os.environ.get("OFX_HOSTDEMO_KEEP_LOADED") == "1"
    dirs = host_dirs()
    profile = "ofx14" if o["ofx14"] else "default"
    try:
        if o["describe"]:
            return _describe(ip, o, dirs, profile, keep)
        if not o["quiet"]:
            ip.ensure_host(dirs, profile, keep, o["std_paths"], normalised_defaults=o["normalised_defaults"],
                           multiplane=o["multiplane"])
            _dump(ip.ofxh)
        job = ip.Job(o["plugin"], o["context"], o["inputs"], o["assignments"], o["depth"], o["frames"], o["scale"],
                     o["window"], query=o["query"], ofx14=o["ofx14"], allow_unwritten=o["allow_unwritten"],
                     major=o["major"], fmt=o["format"], prefix=o["prefix"], keep_loaded=keep,
                     use_std_paths=o["std_paths"], normalised_defaults=o["normalised_defaults"],
                     multiplane=o["multiplane"], planes=o["planes"])
        ret, report, _ = ip.run_job(dirs, job)
        if o["report"] and not _write_json(o["report"], report):
            ret = 1
        return ret
    finally:
        # (as hostDemo's clearPluginCache(): the plug-ins are unloaded after the report is written)
        sys.stdout.flush()
        if shutdown:
            ip.shutdown()


def _describe(ip, o: dict, dirs: tuple, profile: str, keep: bool) -> int:
    """--describe FILE: the exit status."""
    if o["opengl_describe"] and not ip.ofxh.build_config.get("OFX_SUPPORTS_OPENGLRENDER"):
        _err("Error: --opengl-describe: ofxh was built without OFX_SUPPORTS_OPENGLRENDER (no OpenGL render suite)")
        return 1
    ip.ensure_host(dirs, profile, keep, o["std_paths"], opengl_render=o["opengl_describe"],
                   normalised_defaults=o["normalised_defaults"], multiplane=o["multiplane"])
    if not o["quiet"]:
        _dump(ip.ofxh)
    plugin = ip.ofxh.get_plugin(o["plugin"], o["major"])
    if plugin is None:
        _err(specs.not_found(o["plugin"], o["major"]))
        return 1
    d = ip.ofxh.describe(plugin, None)
    ret = 0 if d.get("ok") else 1
    if not _write_json(o["describe"], ip.json_safe(d)):
        ret = 1
    return ret


def is_batch(argv: Sequence[str]) -> bool:
    """Is --batch an option of the command line (not the value of another option, as in "-o --batch")?"""
    flags = scan_options(argv)[1]
    return any(a == "--batch" or a.startswith("--batch=") for a in flags)


def run_batch(commands, jobs: int = 1, timeout: float | None = None, host: str = "fresh", **kwargs):
    """Run many hostDemo command lines in a pool of worker processes.

    Args:
        commands, jobs, timeout, host, **kwargs: see ofxhost._batch.run_batch(); host: "fresh" (a
            new OFX host for each command, the default), "per-plugin" or "reuse" (README.md has the
            trade-offs).

    Returns:
        The records: one dict per command, in order, then those of the worker events.
    """
    from ._batch import run_batch as rb
    return rb(commands, jobs=jobs, timeout=timeout, host=host, **kwargs)


def main(argv: list[str] | None = None) -> int:
    """python -m ofxhost.hostdemo: run(argv, default sys.argv[1:]); the exit status."""
    prog = os.path.basename(sys.argv[0]) if argv is None and sys.argv and sys.argv[0] else "hostdemo"
    if prog == "hostdemo.py":
        prog = "python -m ofxhost.hostdemo"
    return run(sys.argv[1:] if argv is None else list(argv), prog)


if __name__ == "__main__":
    sys.exit(main())
