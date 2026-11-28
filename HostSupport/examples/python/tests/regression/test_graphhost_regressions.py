"""Regression tests of the sample host (ofxhost.graphhost): image bounds outside or larger than the project,
nodes without tiles, images too large for the host, non-finite frames and times, and the sequences it renders.
Each names the review or the issue that found the bug."""

import math
import os

import numpy as np
import pytest

import ofxhost
from ofxhost.graph import Files, Graph, Node
from ofxhost.testing import assert_close, check_backend
from ofxhost_helpers import gradient_rgba, graph_output, run_graph, write_sequence

check_backend("inprocess")  # (the whole module: skipped, or failed with OFXH_REQUIRE=1)
import ofxh  # noqa: E402
from ofxhost import graphhost  # noqa: E402

pytestmark = pytest.mark.usefixtures("tmp_cwd")

INVERT = "net.sf.openfx.invertPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"


def _large(h=1500, w=3000):
    """A float RGBA image larger than the project (3000x1500: more pixels than a 2048x2048 image)."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.dstack([x / w, y / h, np.full_like(x, 0.25), np.ones_like(x)]).astype(np.float32)


@pytest.mark.parametrize("source, bounds, window", [
    (lambda: gradient_rgba(10, 12), (1000, 0, 1012, 10), (1000, 0, 1012, 10)),
    (lambda: gradient_rgba(10, 12), (710, 0, 722, 10), (705, 0, 722, 10)),
    (_large, (0, 0, 3000, 1500), (0, 0, 3000, 1500)),
    (_large, (0, 0, 3000, 1500), (1000, 1000, 1100, 1100)),
], ids=["outside-the-project", "across-the-project-edge", "larger-than-the-project", "window-outside-the-project"])
def test_invert_twice_keeps_the_bounds(sample_plugins, source, bounds, window):
    """Invert(Invert(img)) gives img back with all its bounds, within the render window: an image outside the
    project, across its edge, larger than the project (more pixels than a 2048x2048 image), and a window outside
    the project. The sample host cropped them to the project (review fixes of #126: cf8bbe4f, 3bd67299)."""
    src = source()
    inv2 = Node(INVERT, inputs={"Source": Node(INVERT, inputs={"Source": ofxhost.Image(src, bounds)})})
    g = Graph.of(inv2, frames=[0], window=window)
    out = graph_output(run_graph(g, sample_plugins), g)[0]
    x1, y1 = max(bounds[0], window[0]), max(bounds[1], window[1])
    x2, y2 = min(bounds[2], window[2]), min(bounds[3], window[3])
    assert out.bounds == (x1, y1, x2, y2), (bounds, window, out.bounds)
    # top-down arrays: the rows from the top of the image
    assert_close(out.pixels, src[bounds[3] - y2:bounds[3] - y1, x1 - bounds[0]:x2 - bounds[0]], atol=1e-6)


def test_nothing_to_render(sample_plugins):
    """An output whose RoD doesn't meet the window renders nothing: no image, a null output (review fixes of
    #126, cf8bbe4f)."""
    r = run_graph(Graph.of(Node(INVERT, inputs={"Source": "ramp"}, name="inv"), frames=[0],
                           window=(5000, 5000, 5010, 5010)), sample_plugins)
    assert "inv" not in r.images and r.report["outputs"]["inv"] == [{"time": 0., "output": None, "files": []}]


def test_no_tiles(sample_plugins):
    """A node without tiles (Invert with supportsTiles off) renders its whole RoD from whole-RoD
    inputs, so its input node is rendered on its whole RoD too, whatever the window and the RoI
    (testSmallRoI: Invert asks for the centre of the region it needs only). openfx-io's hostDemo tests found
    it (openfx-io#114, fixed in #128)."""
    src = gradient_rgba(40, 60)
    inner = Node(INVERT, inputs={"Source": ofxhost.Image(src, (100, 50, 160, 90))})
    outer = Node(INVERT, inputs={"Source": inner}, params={"supportsTiles": False, "testSmallRoI": True})
    g = Graph.of(outer, frames=[0], window=(120, 60, 130, 70))
    out = graph_output(run_graph(g, sample_plugins), g)[0]
    assert out.bounds == (100, 50, 160, 90), out.bounds
    assert_close(out.pixels, src, atol=1e-6)
    # with tiles: the window
    outer.params["supportsTiles"] = True
    assert graph_output(run_graph(g, sample_plugins), g)[0].bounds == (120, 60, 130, 70)


@pytest.mark.parametrize("whole_input", [False, True], ids=["plugin-roi", "whole-input-roi"])
def test_no_tiles_huge_rod(sample_plugins, monkeypatch, whole_input):
    """A node without tiles whose input's RoD is too large for an image renders the render window, as hostDemo,
    and its input node renders the window only: with the RoI that the plug-in gives, and when the plug-in asks for
    the whole of the input (its RoI forced to 40000x40000). The sample host failed, or rendered the whole RoD
    (78a9d63b, #128; f653cace, #297)."""
    if whole_input:
        original = graphhost.Instance.get_regions_of_interest_action

        def rois(self, time, roi, render_scale=(1., 1.), view=0):
            if self.node.name != "outer":
                return original(self, time, roi, render_scale, view)
            return ofxh.Stat.OK, {"Source": (0., 0., 40000., 40000.)}

        monkeypatch.setattr(graphhost.Instance, "get_regions_of_interest_action", rois)
    inner = Node(INVERT, inputs={"Source": "rod:0,0,40000,40000"}, name="inner")
    outer = Node(INVERT, inputs={"Source": inner}, params={"supportsTiles": False}, name="outer")
    g = Graph.of(outer, frames=[0], window=(0, 0, 64, 64))
    r = run_graph(g, sample_plugins)
    assert graph_output(r, g)[0].bounds == (0, 0, 64, 64)
    assert [x["renderWindow"] for x in r.report["nodes"]["inner"]["renders"]] == [[0, 0, 64, 64]]


def test_input_too_large(sample_plugins, monkeypatch):
    """An input image too large for the host (a region of interest of 40000x40000 pixels) fails the
    run instead of being left out (f653cace, #297)."""
    def rois(self, time, roi, render_scale=(1., 1.), view=0):
        return ofxh.Stat.OK, {"Source": (0., 0., 40000., 40000.)}

    monkeypatch.setattr(graphhost.Instance, "get_regions_of_interest_action", rois)
    r = graphhost.run(Graph.of(Node(INVERT, inputs={"Source": "rod:0,0,40000,40000"}), frames=[0],
                               window=(0, 0, 64, 64)), sample_plugins)
    assert not r.ok and "is too large" in r.report["error"], r.report["error"]


def test_infinite_frames_needed(sample_plugins, monkeypatch):
    """Frames needed with an infinite or NaN bound fail the run (instead of a loop without end), and so do finite
    ones that would make huge lists, or no progress (f653cace, #297)."""
    inner = Node(INVERT, inputs={"Source": "ramp"})
    for bound in (math.inf, math.nan, 1e9, 2.**53):
        def frames_needed(self, time, bound=bound):
            return ofxh.Stat.OK, {"Source": [(time, bound)]}

        monkeypatch.setattr(graphhost.Instance, "get_frames_needed_action", frames_needed)
        r = graphhost.run(Graph.of(Node(INVERT, inputs={"Source": inner}), frames=[0]), sample_plugins)
        assert not r.ok and "the bounds must be finite, within +-1e+07" in r.report["error"], r.report["error"]


def test_infinite_identity_time(sample_plugins, tmp_path, monkeypatch):
    """An identity at an infinite time (of an image sequence input) fails the run, without an
    OverflowError (f653cace, #297)."""
    seq = write_sequence(tmp_path, [1, 2])

    def is_identity(self, time, window, render_scale=(1., 1.), field="none", view=0, plane="Colour"):
        return ofxh.Stat.OK, {"clip": "Source", "time": math.inf, "view": 0, "plane": "Colour"}

    monkeypatch.setattr(graphhost.Instance, "is_identity_action", is_identity)
    r = graphhost.run(Graph.of(Node(INVERT, inputs={"Source": Files(seq)}), frames=[1]), sample_plugins)
    assert not r.ok and "isIdentity gave the time inf" in r.report["error"], r.report["error"]


def test_sequence_frames_are_not_kept(sample_plugins, tmp_path, monkeypatch):
    """The frames read from a sequence are kept for the current frame only: each one is read twice, to plan
    the renders (its region of definition) and to render (the first one also when the sequence is opened). The
    sample host kept them until the end of the run (#357, 86a71041)."""
    seq = write_sequence(tmp_path, range(1, 6))
    sizes = []
    reads = []
    spec, read = graphhost._Files.spec, graphhost._Files._read

    def spy(self, time):
        s = spec(self, time)
        sizes.append(len(self._specs))
        return s

    def counted(path):
        reads.append(os.path.basename(path))
        return read(path)

    monkeypatch.setattr(graphhost._Files, "spec", spy)
    monkeypatch.setattr(graphhost._Files, "_read", staticmethod(counted))
    run_graph(Graph.of(Node(INVERT, inputs={"Source": Files(seq)}), frames=[1, 2, 3, 4, 5]), sample_plugins)
    assert sizes and max(sizes) == 1
    assert {f: reads.count(f) for f in set(reads)} == {f"in.{f:04d}.pfm": 3 if f == 1 else 2 for f in range(1, 6)}


def test_renders_outside_the_sequence(sample_plugins):
    """A render outside a begun sequence (here, for a push button pressed during the setup: the
    Tester's analyse fetches its Source, an Invert node, at each frame of its range) gets its own
    sequence: beginSequenceRender [t, t] (not sequential), the render, endSequenceRender, as
    kOfxImageEffectActionRender requires ("kOfxImageEffectActionBeginSequenceRender has been called
    on the instance"). Review round 2 of #211 (af675639)."""
    inv = Node(INVERT, inputs={"Source": "ramp"}, name="inv")
    tester = Node(TESTER, params={"analyseButton": None}, inputs={"Source": inv}, name="t")
    r = run_graph(Graph(nodes={"inv": inv, "t": tester}, outputs={"t": None}, frames=[0]), sample_plugins)
    acts = r.report["nodes"]["inv"]["actions"]
    names = [a["action"] for a in acts]
    assert names[:2] == ["createInstance", "getClipPreferences"]
    # the analyse renders: 24 frames, 0 to 23 (the Tester's loop over its range), each in its own sequence
    wrapped = acts[2:2 + 3 * 24]
    assert [a["action"] for a in wrapped] == ["beginSequenceRender", "render", "endSequenceRender"] * 24
    assert [(a["first"], a["last"], a["sequential"]) for a in wrapped[::3]] == [(float(f), float(f), False)
                                                                              for f in range(24)]
    assert all(a["ok"] for a in wrapped)
    # then the planned sequence of the output's frame
    assert names[2 + 3 * 24:] == ["beginSequenceRender", "render", "endSequenceRender"]
    assert acts[2 + 3 * 24]["first"] == 0. and acts[2 + 3 * 24 + 1]["sequential"] is True
