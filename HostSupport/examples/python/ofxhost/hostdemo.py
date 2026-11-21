"""hostDemo's command line on the in-process backend (ofxhost.inprocess, on the ofxh module): a
drop-in for the hostDemo binary in the plug-in repositories' test scripts (NatronGitHub/openfx#136,
part 5).

    python -m ofxhost.hostdemo [hostDemo's options] [[-s] name=value]...
    HostSupport/examples/python/hostdemo-inprocess [hostDemo's options]...   (the same, as a sh script;
                                                                     $OFXHOST_PYTHON: the Python)
    python -m ofxhost hostdemo [hostDemo's options]...                       (the same)
    python -m ofxhost.hostdemo --batch FILE|- [-j N] [--timeout S] [--fresh-host]...  (batch mode)

It takes hostDemo's options, with the same meaning and the same errors (see -h): -p, -m, -c, -o,
--format ppm|pfm, -d, -i (the same input specs), --frames, --scale, --window, --query, -s (values,
keys, push buttons) and bare name=value assignments, --report, --describe, --ofx14,
--allow-unwritten, --no-std-paths, --no-cache and --quiet. It reads the plug-ins of
$OFX_PLUGIN_PATH (and the standard locations without --no-std-paths), keeps them loaded after the
scan with $OFX_HOSTDEMO_KEEP_LOADED=1, writes the same output files
(<prefix>.<t>[l|r].ppm, or .pfm and .alpha.pfm), the same --report and --describe JSON, prints the
plug-ins' messages on stdout as hostDemo does ("<type> <id> <text>", also with --quiet), and
exits with the same status (0 on success, 1 on failure). As hostDemo, it runs in the current
directory, one run per process, so a test harness gets the same crash isolation, and its
timeouts work the same way; the plug-ins are unloaded at the end (kOfxActionUnload), after the
report is written.

What differs from hostDemo (README.md has the list):
- the report's host name is ofxh's ("net.sf.openfx.ofxh"), and its plugin.filePath is absolute;
- the numbers in the JSON are written by Python (0.0, not 0); NaN and infinities are null, as in
  hostDemo; the means can differ in their last bits (NumPy's pairwise summation);
- during a render, an output image fetch runs no nested RoD action (the host's RoD of the render
  is used), so a plug-in that traces its actions prints fewer RoD actions;
- --no-cache is a no-op: ofxh has no plug-in cache file; without --quiet, the list of the
  plug-ins found is shorter than hostDemo's dump (Plug-in:id and its contexts);
- ofxh's string choice parameters refuse a value outside their enums (hostDemo's accept it);
- a bug of the Python host (an uncaught exception) exits with status 1, a traceback, and no report;
- beyond hostDemo, an input spec may end with a components suffix, "#rgb", "#alpha" or "#rgba"
  ("-i Source=ramp#rgb"): the clip's unmapped components (see ofxhost.inprocess);
- $OFXHOST_PRELOAD: libraries (os.pathsep-separated) loaded with RTLD_GLOBAL before the plug-in
  scan, e.g. MacPorts' libtbbmalloc for IO.ofx (oneTBB 2023.2 can crash at exit when OpenImageIO
  loaded it, see NatronGitHub/openfx#136).
The batch mode (--batch, or run_batch() from Python; ofxhost._batch has the details) runs many
command lines in one Python, in a pool of worker processes that each keep an ofxh host, instead of
paying the start of Python and the imports of NumPy and ofxh (about 0.2 s) for each: each line runs
as a separate run would, in its own directory (--cwd) and environment (--env), with its stdout and
stderr captured, and a crashed or hung command (--timeout) is recorded and its worker replaced.
What a reused host shares between runs (plug-in globals, the plug-in scan): see the README.
As hostDemo, the whole command line is read before ofxh is imported: -h and the usage errors work
without the bindings. Windows (a native python.exe): python -m ofxhost.hostdemo, with Windows paths
in PYTHONPATH separated by ";" (the hostdemo-inprocess script does that when cygpath exists).
"""

from __future__ import annotations

import ctypes
import json
import os
import sys

__all__ = ["main", "run", "run_batch", "USAGE"]

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
  --no-std-paths     only look for plug-ins in OFX_PLUGIN_PATH
  --no-cache         (no-op: ofxh has no plug-in cache file)
  --quiet            print only the plug-in's messages on stdout
  --allow-unwritten  don't fail on unwritten (NaN) output pixels (float only)
Environment: OFX_HOSTDEMO_KEEP_LOADED=1 keeps the scanned plug-ins loaded (by default they
are unloaded after the scan, and loaded again at their first use). OFXHOST_PRELOAD: libraries
to load first (os.pathsep-separated).
Batch mode: --batch FILE|- [-j N] [--timeout SECONDS] [--fresh-host|--fresh-host-per-plugin]
[--batch-report FILE] runs many command lines (one per line) in one Python ({prog} --batch - -h).
Formats and JSON schemas: HostSupport/examples/python/README.md. Exit status: 0 on success.
"""

_VALUE_OPTIONS = ("-p", "-m", "-c", "-o", "-s", "-d", "-i", "--format", "--report", "--describe", "--frames",
                  "--scale", "--window")


class _Exit(Exception):
    def __init__(self, status: int):
        super().__init__(status)
        self.status = status


def _err(text: str) -> None:
    sys.stdout.flush()
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


def _write_json(path: str, doc: dict) -> bool:
    """writeJson: False (after an error message) if it couldn't be written."""
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2, allow_nan=False)
            f.write("\n")
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


def _parse(argv: list[str], prog: str) -> dict:
    """hostDemo's command line (run() in hostDemo.cpp), parsed as hostDemo parses it."""
    from . import _specs as ip  # (not ofxh: it is imported after the command line, as hostDemo loads plug-ins)
    o = {"plugin": "net.sf.openfx.invertPlugin", "major": -1, "context": "OfxImageEffectContextFilter",
         "prefix": "Output", "depth": "byte", "assignments": [], "inputs": [], "report": "", "describe": "",
         "std_paths": True, "quiet": False, "ofx14": False, "allow_unwritten": False, "format": "ppm",
         "frames": None, "scale": (1., 1.), "window": None, "query": False}
    seen: set = set()
    a = 0
    while a < len(argv):
        arg = argv[a]
        if arg in ("-h", "--help"):
            sys.stdout.write(USAGE.format(prog=prog))
            raise _Exit(0)
        if arg == "--no-std-paths":
            o["std_paths"] = False
        elif arg == "--no-cache":
            pass
        elif arg == "--quiet":
            o["quiet"] = True
        elif arg == "--allow-unwritten":
            o["allow_unwritten"] = True
        elif arg == "--query":
            o["query"] = True
        elif arg == "--ofx14":
            o["ofx14"] = True
        elif arg in _VALUE_OPTIONS:
            if a + 1 >= len(argv):
                _err(f"Error: option {arg} requires an argument")
                sys.stdout.write(USAGE.format(prog=prog))
                raise _Exit(1)
            a += 1
            v = argv[a]
            if arg == "-p":
                o["plugin"] = v
            elif arg == "-m":
                m = ip._parse_int(v)
                if m is None or m < 0:
                    _err(f'Error: invalid major version "{v}"')
                    raise _Exit(1)
                o["major"] = m
            elif arg == "-c":
                c = {"filter": "OfxImageEffectContextFilter", "general": "OfxImageEffectContextGeneral",
                     "OfxImageEffectContextFilter": "OfxImageEffectContextFilter",
                     "OfxImageEffectContextGeneral": "OfxImageEffectContextGeneral"}.get(v)
                if c is not None:
                    o["context"] = c
                else:
                    _err(f'Error: unsupported context "{v}", expected filter or general')
                    raise _Exit(1)
            elif arg == "-o":
                o["prefix"] = v
            elif arg == "--format":
                if v not in ("ppm", "pfm"):
                    _err(f'Error: unsupported format "{v}", expected ppm or pfm')
                    raise _Exit(1)
                o["format"] = v
            elif arg == "-d":
                d = {"byte": "byte", "short": "short", "float": "float", "OfxBitDepthByte": "byte",
                     "OfxBitDepthShort": "short", "OfxBitDepthFloat": "float"}.get(v)
                if d is None:
                    _err(f'Error: unsupported depth "{v}", expected byte, short or float')
                    raise _Exit(1)
                o["depth"] = d
            elif arg == "-i":
                eq = v.find("=")
                error = "no clip name"
                spec = None
                if eq > 0:
                    try:
                        spec = ip._input_spec(v[:eq], v[eq + 1:], resolve=False)
                    except ip._SpecError as e:
                        error = str(e)
                if spec is None:
                    _err(f'Error: invalid input "{v}" ({error}), expected Clip=default|solid:r,g,b,a|ramp|checker:N|'
                         "rod:x1,y1,x2,y2|none|file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]")
                    raise _Exit(1)
                if v[:eq] in seen:
                    _err(f'Error: input clip "{v[:eq]}" given twice (-i)')
                    raise _Exit(1)
                seen.add(v[:eq])
                o["inputs"].append((v[:eq], spec))
            elif arg == "--report":
                o["report"] = v
            elif arg == "--describe":
                o["describe"] = v
            elif arg == "--frames":
                frames: list[float] = []
                for x in v.split(","):
                    f = ip._parse_double(x)
                    if f is None or f in frames:
                        _err(f'Error: invalid frames "{v}", expected distinct times t0,t1,...')
                        raise _Exit(1)
                    frames.append(f)
                names: set = set()
                for f in frames:
                    n = f"{f:.12g}"
                    if n in names:
                        _err(f'Error: invalid frames "{v}": {n} is given twice to 12 significant digits (the output '
                             "file names)")
                        raise _Exit(1)
                    names.add(n)
                o["frames"] = frames
            elif arg == "--scale":
                s = ip._parse_n(v, 1, ip._parse_double)
                s = s * 2 if s else ip._parse_n(v, 2, ip._parse_double)
                if not s or not all(0. < x <= 1. for x in s):
                    _err(f'Error: invalid render scale "{v}", expected s or sx,sy in (0,1]')
                    raise _Exit(1)
                o["scale"] = (s[0], s[1])
            elif arg == "--window":
                w = ip._parse_n(v, 4, ip._parse_int)
                if not w or w[0] >= w[2] or w[1] >= w[3] or w[2] - w[0] > 1000000 or w[3] - w[1] > 1000000 or \
                        (w[2] - w[0]) * (w[3] - w[1]) > ip.MAX_IMAGE_PIXELS:
                    _err(f'Error: invalid render window "{v}", expected x1,y1,x2,y2 with x1<x2 and y1<y2, at most '
                         "1000000 pixels wide or high and 2^28 pixels in all")
                    raise _Exit(1)
                o["window"] = tuple(w)
            else:  # -s
                o["assignments"].append(v)
        elif arg and arg[0] != "-" and "=" in arg:
            o["assignments"].append(arg)
        else:
            _err(f'Error: unknown argument "{arg}"')
            sys.stdout.write(USAGE.format(prog=prog))
            raise _Exit(1)
        a += 1
    if o["frames"] is None:
        o["frames"] = [0., ip.CLIP_LENGTH]
    for _, spec in o["inputs"]:
        spec.scale = o["scale"]  # (the pixels of a file input are pixels at the render scale)
    if o["report"] and o["describe"]:
        _err("Error: --report and --describe can't be used together (--describe creates no instance)")
        raise _Exit(1)
    return o


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
    """Run hostDemo's command line (argv without the program name): the exit status. shutdown:
    shut the in-process backend's ofxh host down at the end (the plug-ins' unload action), as at
    the end of hostDemo; False keeps it for the next run in this process (the batch mode). With
    --batch, argv is the batch mode's options (ofxhost._batch)."""
    if _is_batch(argv):
        from ._batch import main as batch_main
        return batch_main(argv)
    try:
        o = _parse(argv, prog)  # (-h and the usage errors without ofxh, as hostDemo's)
    except _Exit as e:
        return e.status
    dirs = [d for d in os.environ.get("OFX_PLUGIN_PATH", "").split(os.pathsep) if d]
    from ._ofxh import OfxhNotFound, import_ofxh
    try:
        import_ofxh(os.pathsep.join(dirs), for_cli=True)
    except OfxhNotFound as e:
        _err(f"Error: {e}")
        return 1
    _preload()
    from . import inprocess as ip
    keep = os.environ.get("OFX_HOSTDEMO_KEEP_LOADED") == "1"
    # (the plug-in directories that exist: hostDemo ignores the others)
    dirs = tuple(dict.fromkeys(os.path.abspath(d) for d in dirs if os.path.isdir(d)))
    ret = 1
    try:
        if o["describe"]:
            ip._ensure(dirs, "ofx14" if o["ofx14"] else "default", keep, o["std_paths"])
            if not o["quiet"]:
                _dump(ip.ofxh)
            plugin = ip.ofxh.get_plugin(o["plugin"], o["major"])
            if plugin is None:
                _err(f'Error: plug-in "{o["plugin"]}"' + (f' with major version {o["major"]}' if o["major"] >= 0 else "")
                     + " not found. Check that OFX_PLUGIN_PATH contains the folder of its .ofx.bundle.")
                return 1
            d = ip.ofxh.describe(plugin, None)
            ret = 0 if d.get("ok") else 1
            if not _write_json(o["describe"], ip.json_safe(d)):
                ret = 1
            return ret
        if not o["quiet"]:
            ip._ensure(dirs, "ofx14" if o["ofx14"] else "default", keep, o["std_paths"])
            _dump(ip.ofxh)
        ret, report, _ = ip._run(dirs, o["plugin"], o["context"], o["inputs"], o["assignments"], o["depth"],
                                 o["frames"], o["scale"], o["window"], o["query"], o["ofx14"], o["allow_unwritten"],
                                 o["major"], fmt=o["format"], prefix=o["prefix"], out_dir=None, keep_loaded=keep,
                                 use_std_paths=o["std_paths"])
        report["ok"] = ret == 0  # (as hostDemo writes them in every report)
        report["exitStatus"] = ret
        if o["report"] and not _write_json(o["report"], report):
            ret = 1
        return ret
    finally:
        # (as hostDemo's clearPluginCache(): the plug-ins are unloaded after the report is written)
        sys.stdout.flush()
        if shutdown:
            ip.shutdown()


def _is_batch(argv) -> bool:
    return any(a == "--batch" or a.startswith("--batch=") for a in argv)


def run_batch(commands, jobs: int = 1, timeout: float | None = None, fresh_host: bool | str = False, **kwargs):
    """Run many hostDemo command lines in a pool of `jobs` worker processes that each keep an ofxh
    host: the list of their records (one dict per command, in order). See ofxhost._batch.run_batch
    for the arguments and the records, and the README for the trade-offs of fresh_host."""
    from ._batch import run_batch as rb
    return rb(commands, jobs=jobs, timeout=timeout, fresh_host=fresh_host, **kwargs)


def main(argv: list[str] | None = None) -> int:
    prog = os.path.basename(sys.argv[0]) if argv is None and sys.argv and sys.argv[0] else "hostdemo"
    if prog == "hostdemo.py":
        prog = "python -m ofxhost.hostdemo"
    return run(sys.argv[1:] if argv is None else list(argv), prog)


if __name__ == "__main__":
    sys.exit(main())
