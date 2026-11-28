"""Helpers shared by the ofxhost tests (functional/ and regression/): not a test module (the shared fixtures
are in conftest.py).

conftest.py puts this directory on sys.path, so the test modules import it as ofxhost_helpers. It has the
JSON comparisons, the test images, the comparison of the two ofxhost
backends, the batch helpers of python -m ofxhost.hostdemo --batch, and the ctypes prototypes of the suite
functions that the tests call as a plug-in does.
"""

import ctypes
import json
import math
import os
import shlex
from pathlib import Path

import numpy as np

import ofxhost
from ofxhost.testing import views

PYDIR = Path(__file__).resolve().parents[1]  # HostSupport/examples/python: the ofxhost package

INVERT = "net.sf.openfx.invertPlugin"
# hostDemo's options of the plug-in repositories' scripts: no standard plug-in paths, no plug-in cache, no
# progress output
Q = ["--no-std-paths", "--no-cache", "--quiet"]


# --- JSON -----------------------------------------------------------------------------------------

def same_json(a, b):
    """Equal JSON values: the same keys, ints and floats compared as numbers (1 == 1.0), None for NaN."""
    if isinstance(a, dict) and isinstance(b, dict):
        assert a.keys() == b.keys(), (sorted(a), sorted(b))
        for key in a:
            same_json(a[key], b[key])
    elif isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        assert len(a) == len(b), (a, b)
        for x, y in zip(a, b):
            same_json(x, y)
    elif isinstance(a, bool) or isinstance(b, bool):
        assert a is b, (a, b)
    else:
        assert a == b, (a, b)


def strict_json(text):
    """JSON text without NaN or Infinity (hostDemo writes them as null): ValueError if there is one."""
    def refuse(c):
        raise ValueError(f"non-finite JSON number {c}")
    return json.loads(text, parse_constant=refuse)


# --- images -----------------------------------------------------------------------------------------

def random_rgba(h=24, w=32, seed=0):
    """A float RGBA array of uniform random values (top row first)."""
    return np.random.default_rng(seed).random((h, w, 4), dtype=np.float32)


def gradient_rgba(h=24, w=32):
    """A float RGBA gradient: r along x, g along y (top row 0), b 0.25, alpha 1 - x / w."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / (w - 1), y / (h - 1), np.full_like(x, 0.25), 1 - x / w]).astype(np.float32)


def hostdemo_ramp(h=6, w=8):
    """The top-left h x w pixels of hostDemo's ramp: x / the RoD's width, at the pixel centres (720 pixels)."""
    return np.broadcast_to(((np.arange(w) + 0.5) / 720.)[None, :], (h, w))


# --- the two backends -------------------------------------------------------------------------------

def _normalized_report(report):
    """What differs by design: the host name, the input specs (temporary PFM files or arrays), the
    output files (none in-process), the plug-in's file path (symbolic links); the means (summation
    order) are compared approximately."""
    r = dict(report)
    r["host"] = dict(r["host"], name=None)
    r["plugin"] = dict(r["plugin"])
    if r["plugin"].get("filePath"):
        r["plugin"]["filePath"] = os.path.realpath(r["plugin"]["filePath"])
    r["inputs"] = {c: dict(v, spec=None) for c, v in r["inputs"].items()}
    renders = []
    for x in r["renders"]:
        x = dict(x)
        if x.get("output"):
            x["output"] = dict(x["output"], files=None, mean=None)
        renders.append(x)
    r["renders"] = renders
    return r


def _means(report):
    return [x["output"]["mean"] for x in report["renders"] if x.get("output")]


def compare_backends(plugins, *args, pixels=True, **kw):
    """Render with both backends (ofxhost.render's arguments); check that the results agree: the exit status,
    the report (normalized), the means, the outputs, their pixels, bounds and premultiplication. Returns the two
    RenderResults (subprocess, inprocess)."""
    s = ofxhost.render(plugins, *args, backend="subprocess", **kw)
    i = ofxhost.render(plugins, *args, backend="inprocess", **kw)
    assert s.returncode == i.returncode
    same_json(_normalized_report(s.report), _normalized_report(i.report))
    for ms, mi in zip(_means(s.report), _means(i.report)):
        assert [x is None for x in ms] == [x is None for x in mi]
        for x, y in zip(ms, mi):
            if x is not None:
                assert math.isclose(x, y, rel_tol=1e-12, abs_tol=1e-12), (ms, mi)
    assert list(s.outputs) == list(i.outputs)
    if pixels:
        for key in s.outputs:
            assert s.outputs[key].shape == i.outputs[key].shape, key
            assert np.array_equal(s.outputs[key], i.outputs[key], equal_nan=True), key
            assert s.images[key].bounds == i.images[key].bounds and s.images[key].premult == i.images[key].premult
        assert list(s.alpha) == list(i.alpha)
    return s, i


# --- python -m ofxhost.hostdemo --batch ---------------------------------------------------------------

HOOK = "OFXHOST_BATCH_TEST_HOOK"  # the batch workers' test hook (ofxhost._batch)


def batch_line(args) -> str:
    """A batch file line: args quoted for the shell."""
    return " ".join(shlex.quote(a) for a in args)


def hook_line(hook, *more):
    """The arguments of a command that runs the test hook (its own --env), then renders Invert at frame 0."""
    return ["--env", f"{HOOK}={hook}", *Q, "-p", INVERT, "--frames", "0", *more]


def ppm_files(directory: Path, prefix: str, t: float) -> list[Path]:
    """The output files of frame t of a run with -o prefix (the default --format ppm): one per view."""
    return [directory / f"{x}.ppm" for x in views().stems(prefix, t)]


# --- the suite functions that the tests call as a plug-in does (ofxhost.testing.suite_function) ---------

IMAGE_EFFECT, PROPERTY = "OfxImageEffectSuite", "OfxPropertySuite"
# the indexes of their functions (OfxImageEffectSuiteV1, OfxPropertySuiteV1)
CLIP_GET_HANDLE, CLIP_GET_IMAGE, CLIP_RELEASE_IMAGE, CLIP_GET_ROD = 3, 5, 6, 7
PROP_GET_POINTER, PROP_GET_INT, PROP_GET_INT_N = 8, 11, 15
# and their prototypes
CLIP_HANDLE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.c_void_p)
GET_IMAGE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.c_void_p,
                                ctypes.POINTER(ctypes.c_void_p))
RELEASE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
ROD_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.POINTER(ctypes.c_double * 4))
GET_POINTER_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_void_p))
GET_INT_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                              ctypes.POINTER(ctypes.c_int))
GET_INT_N_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                ctypes.POINTER(ctypes.c_int * 4))


# --- the sample host (ofxhost.graphhost) ------------------------------------------------------------------

def write_sequence(directory, frames, h=6, w=8, name="in.####.pfm"):
    """Write an RGB PFM sequence in directory: frame f is the constant f / 10. Returns its pattern's path."""
    from ofxhost import sequences
    for f in frames:
        ofxhost.write_pfm(directory / sequences.expand(name, f), np.full((h, w, 3), f / 10, np.float32))
    return str(directory / name)


def run_graph(g, plugins, **kw):
    """graphhost.run(g, plugins, **kw), which must succeed with a report that is JSON (no NaN)."""
    from ofxhost import graphhost
    r = graphhost.run(g, plugins, **kw)
    assert r.ok, r.report["error"]
    json.dumps(r.report, allow_nan=False)
    return r


def graph_output(r, g):
    """The images of the graph's (first) output node: {time: Image}."""
    return r.images[next(iter(g.outputs))]
