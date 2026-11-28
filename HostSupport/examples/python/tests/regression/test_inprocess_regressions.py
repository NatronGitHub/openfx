"""Regression tests of the in-process backend (ofxhost.inprocess) and of the sample host (ofxhost.graphhost):
each names the issue or the review that found the bug."""

import ctypes
import gc
import json
import os
import threading
import time as _time
import weakref

import numpy as np
import pytest

import ofxhost
from ofxhost.graph import Graph, Node
from ofxhost.testing import assert_close, check_backend, suite_function, views
from ofxhost_helpers import (CLIP_GET_HANDLE, CLIP_GET_IMAGE, CLIP_GET_ROD, CLIP_HANDLE_FN, CLIP_RELEASE_IMAGE,
                             GET_IMAGE_FN, GET_POINTER_FN, IMAGE_EFFECT, PROP_GET_POINTER, PROPERTY, RELEASE_FN, ROD_FN,
                             compare_backends, hostdemo_ramp)

check_backend("inprocess")  # (the whole module: skipped, or failed with OFXH_REQUIRE=1)
from ofxhost import graphhost, inprocess  # noqa: E402

pytestmark = pytest.mark.usefixtures("tmp_cwd")

INVERT = "net.sf.openfx.invertPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
GPUGAIN = "com.OpenFXSample.GPUGain"


@pytest.mark.parametrize("extra_args, refused, presses", [([], 25, 1), (["-s", "analyseButton"], 50, 2)],
                         ids=["one-press", "two-presses"])
def test_refused_images_are_all_counted(both, extra_args, refused, presses):
    """Each image too large for hostDemo (more than 2^28 pixels) is an image fetch error, with its message, as in
    hostDemo, on both backends: the Tester's analyse button fetches its Source at each frame of its range (0 to 24)
    outside a render, where the whole RoD is fetched: 25 errors for a press. The same refused fetch twice (the same
    bounds at the same time: the button pressed again, with -s in extra_args) is two errors: 50, not 25. That
    would have failed with the (bounds, time) de-duplication removed in #207 (7b2d5de9, #211). (During a render,
    the samples fetch within the render window, with the default RoI; a tiled plug-in with a large RoI, or
    explicit fetch bounds, could make several refused fetches in one render.)"""
    s, i = compare_backends(both, TESTER, inputs={"Source": "rod:0,0,40000,40000"}, params={"analyseButton": None},
                            extra_args=extra_args, frames=[0], query=True)
    for r in (s, i):
        assert r.stderr.count("too large (more than 268435456 pixels)") == refused, r.stderr
        pressed = [a for a in r.report["actions"] if a["action"] == "instanceChanged"]
        assert [a["param"] for a in pressed] == ["analyseButton"] * presses


def test_alpha_input_of_invert(sample_plugins):
    """Invert needs the same components on Source and Output. The render runs in the filter context,
    where the spec doesn't let the host remap the output, so it is the Source's Alpha; in the general
    context, the default output is "RGBA if any of the inputs is RGBA - otherwise A if the effect has
    any inputs" (ofxClipPreferences.rst). With an RGBA output, the render failed (#332)."""
    r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": "ramp"}, components={"Source": "alpha"},
                       frames=[0], window=(0, 0, 8, 6), backend="inprocess")
    clips = r.report["clipPreferences"]["clips"]
    assert clips["Source"]["components"] == "OfxImageComponentAlpha"
    assert clips["Output"]["components"] == "OfxImageComponentAlpha"
    assert r.output.shape == (6, 8, 1)
    assert_close(r.output[..., 0], 1 - hostdemo_ramp(), atol=1e-6)


@pytest.mark.parametrize("host", ["inprocess", "graphhost"])
def test_worker_threads_fetch_the_output_first(sample_plugins, monkeypatch, host):
    """Several worker threads of a plug-in (here Python threads that call the image effect suite through
    ctypes, started and joined in the first fetch of the output image, as a plug-in's OpenMP loop
    would) are the first to fetch the output image, concurrently (each one's get_image() sleeps, so
    that they all run it before one of them returns), and ask for the output's RoD. Each fetch works,
    without a RoD action on a worker (which ofxh refuses), and there is one output image in the render:
    the same image (handle and pixel data) for every fetch, the main thread's too. The arrays that the
    other get_image() calls made are dropped, not leaked: none is alive after the render. (Review round 1 of
    #212, 2381af31: lost pixels and a leaked image.)"""
    n = 4
    seen = {"images": [], "data": [], "rods": []}
    made = []  # weak references to the pixel arrays of the output's get_image() calls
    clip_class = inprocess._Clip if host == "inprocess" else graphhost.Clip
    original = clip_class.get_image

    def get_image(self, time, view, bounds, plane="Colour"):
        main = threading.current_thread() is threading.main_thread()
        if self.is_output and main and not seen["images"]:
            handle = ctypes.c_void_p()
            get_handle = suite_function(IMAGE_EFFECT, 1, CLIP_GET_HANDLE, CLIP_HANDLE_FN)
            st = get_handle(ctypes.c_void_p(self.instance.handle), b"Output", ctypes.byref(handle), None)
            assert st == 0

            def worker():
                img = ctypes.c_void_p()
                st = suite_function(IMAGE_EFFECT, 1, CLIP_GET_IMAGE, GET_IMAGE_FN)(handle, time, None,
                                                                                    ctypes.byref(img))
                data = ctypes.c_void_p()
                if st == 0:
                    get_pointer = suite_function(PROPERTY, 1, PROP_GET_POINTER, GET_POINTER_FN)
                    get_pointer(img, b"OfxImagePropData", 0, ctypes.byref(data))
                seen["images"].append((st, img.value))
                seen["data"].append(data.value)
                rod = (ctypes.c_double * 4)()
                get_rod = suite_function(IMAGE_EFFECT, 1, CLIP_GET_ROD, ROD_FN)
                seen["rods"].append((get_rod(handle, time, ctypes.byref(rod)), tuple(rod)))
                if st == 0:
                    suite_function(IMAGE_EFFECT, 1, CLIP_RELEASE_IMAGE, RELEASE_FN)(img)

            threads = [threading.Thread(target=worker) for _ in range(n)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
        elif self.is_output and not main:
            _time.sleep(0.05)  # (lets the other workers into get_image(): they race)
        r = original(self, time, view, bounds, plane)
        if self.is_output and r is not None:
            made.append(weakref.ref(r.pixels))
        return r

    monkeypatch.setattr(clip_class, "get_image", get_image)
    if host == "inprocess":
        r = ofxhost.render(sample_plugins, INVERT, inputs={"Source": "rod:0,0,768,576"}, frames=[0],
                           window=(0, 0, 8, 6), backend="inprocess")
        assert r.report["ok"]
        assert np.all(r.output == 1.)  # (Invert of transparent black: the plug-in rendered into the image)
    else:
        r = graphhost.run(Graph.of(Node(INVERT, inputs={"Source": "ramp"}), frames=[0], window=(0, 0, 8, 6)),
                          sample_plugins, write=False)
        assert r.ok, r.report["error"]
    assert [st for st, _ in seen["images"]] == [0] * n  # kOfxStatOK
    assert len({h for _, h in seen["images"]}) == 1  # one image
    assert len(set(seen["data"])) == 1 and seen["data"][0]
    assert seen["rods"] == [(0, (0., 0., 768., 576.))] * n
    # (each worker's get_image(), then the main thread's, and the main thread's in each other view)
    assert len(made) == n + (views().count if host == "inprocess" else 1)
    del r
    gc.collect()
    assert [w for w in made if w() is not None] == []  # (none leaked)


def test_renders_from_several_threads(sample_plugins):
    """Renders started at once from several Python threads run one after the other: each gets its own
    output, and the current directory and the file descriptors 1 and 2 (which each render changes until
    it returns) are the caller's again (#292, fixed in #341)."""
    # (the host, before the threads)
    ofxhost.render(sample_plugins, INVERT, inputs={"Source": "ramp"}, frames=[0], backend="inprocess")
    before = os.getcwd(), [(os.fstat(fd).st_dev, os.fstat(fd).st_ino) for fd in (1, 2)]
    n = 4
    start = threading.Barrier(n, timeout=60)
    results, errors = {}, []

    def one(k):
        try:
            start.wait()
            results[k] = ofxhost.render(sample_plugins, INVERT, inputs={"Source": f"solid:{k / 8},0,0,1"},
                                        frames=[0], backend="inprocess")
        except Exception as e:  # (reported by the assert below)
            errors.append(repr(e))

    threads = [threading.Thread(target=one, args=(k,)) for k in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(60)
    assert not any(t.is_alive() for t in threads)
    assert not errors, errors
    assert sorted(results) == list(range(n))
    for k, r in results.items():
        assert_close(r.output[..., 0], 1 - k / 8, atol=1e-6)
    assert (os.getcwd(), [(os.fstat(fd).st_dev, os.fstat(fd).st_ino) for fd in (1, 2)]) == before


def test_non_finite_statistics_are_null(sample_plugins, tmp_path):
    """An output with infinite values (3e38 * 10 overflows float): its min, max and mean are null in
    the report, as hostDemo writes NaN and infinities (hostDemoJson.h), and the report, in memory and written
    with --report, is strict JSON (review round 1 of #212, 2381af31)."""
    r = ofxhost.render(sample_plugins, GPUGAIN, inputs={"Source": "solid:3e38,0.5,0.25,1"},
                       params={"scaleR": 10}, frames=[0], window=(0, 0, 4, 4), extra_args=["--report", "r.json"],
                       backend="inprocess")
    out = r.report["renders"][0]["output"]
    assert out["min"][0] is None and out["max"][0] is None and out["mean"][0] is None
    assert out["mean"][1] == 0.5
    json.dumps(r.report, allow_nan=False)

    def strict(c):
        raise ValueError(c)
    assert json.loads((tmp_path / "r.json").read_text(), parse_constant=strict) == json.loads(json.dumps(r.report))
