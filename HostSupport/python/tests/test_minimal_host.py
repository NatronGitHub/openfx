"""The minimal host (HostSupport/examples/python/minimal_host.py), and in-process renders that are
bit-identical to hostDemo's."""

import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")
import minimal_host  # noqa: E402
import ofxhost  # noqa: E402

INVERT = "net.sf.openfx.invertPlugin"
RECTANGLE = "uk.co.thefoundry.GeneratorExample"  # Examples/Rectangle: draws in canonical coordinates


def _source(seed=1, shape=(48, 64, 4)):
    """A float32 RGBA image in [0, 1), top-down (ofxhost's convention)."""
    return np.random.default_rng(seed).random(shape, dtype=np.float32)


def _to_depth(bottom_up, depth):
    """hostDemo's conversion of its PFM inputs to the clip depth (hostDemoClipInstance.cpp, MyImage)."""
    if depth == "byte":
        return np.floor(np.clip(bottom_up.astype(np.float64), 0., 1.) * 255. + 0.5).astype(np.uint8)
    return np.ascontiguousarray(bottom_up, dtype=np.float32)


def _to_float(pixels):
    """hostDemo's conversion of its outputs to PFM (MyImage::getComponentFloat)."""
    if pixels.dtype == np.uint8:
        return pixels.astype(np.float32) / np.float32(255.)
    return pixels


def _render(plugin_dir, inputs):
    return minimal_host.render([plugin_dir], INVERT, inputs, use_env_path=False)


@pytest.mark.parametrize("depth", ["byte", "float"])
def test_invert_matches_hostdemo(plugin_dir, hostdemo, depth):
    src = _source()
    r = ofxhost.render(plugin_dir, INVERT, inputs={"Source": src}, depth=depth, frames=[0],
                       host_binary=hostdemo)
    expected = next(iter(r.outputs.values()))  # float32, top-down (the first view)

    out = _render(plugin_dir, {"Source": _to_depth(src[::-1], depth)})
    assert out.bounds == (0, 0, 64, 48)
    assert out.pixels.dtype == (np.uint8 if depth == "byte" else np.float32)
    got = _to_float(out.pixels)[::-1]  # bottom-up -> top-down
    np.testing.assert_array_equal(got, expected)  # bit-identical (atol=0)


def test_hd_input_renders_fully(plugin_dir):
    """The project is the input (with square pixels): a 1920x1080 input renders whole."""
    src = _to_depth(_source(seed=3, shape=(1080, 1920, 4)), "float")
    out = _render(plugin_dir, {"Source": src})
    assert out.bounds == (0, 0, 1920, 1080)
    np.testing.assert_array_equal(out.pixels[..., :3], 1. - src[..., :3])


def test_negative_row_stride_input(plugin_dir):
    """An input whose rows go down in memory (kOfxImagePropRowBytes < 0) renders as a contiguous one."""
    bottom_up = _to_depth(_source(seed=2)[::-1], "float")
    top_down = np.ascontiguousarray(bottom_up[::-1])
    view = top_down[::-1]  # bottom-up again, with a negative row stride
    assert view.strides[0] < 0
    a = _render(plugin_dir, {"Source": bottom_up})
    b = _render(plugin_dir, {"Source": view})
    np.testing.assert_array_equal(a.pixels, b.pixels)
    np.testing.assert_array_equal(a.pixels[..., :3], 1. - bottom_up[..., :3])


def test_input_released_after_render(plugin_dir):
    src = _to_depth(_source(seed=4), "float")
    before = sys.getrefcount(src)
    _render(plugin_dir, {"Source": src})
    assert sys.getrefcount(src) == before  # the images, clips and instance dropped it


class _FileSource(ofxh.ClipInstance):
    """hostDemo's file input (-i Source=file:...): its pixels are clip pixels at the render scale,
    from (0, 0), so its RoD is their bounds divided by the render scale, at the project PAR."""
    unmapped_bit_depth = "float"

    def __init__(self, image=None, scale=(1., 1.)):
        super().__init__()
        self.image, self.scale = image, scale

    def region_of_definition(self, time, view):
        if self.is_output:
            return super().region_of_definition(time, view)
        h, w = self.image.shape[:2]
        return ofxh.pixel_to_canonical((0, 0, w, h), self.scale, self.aspect_ratio)

    def get_image(self, time, view, bounds):
        if self.is_output:
            return super().get_image(time, view, bounds)
        h, w = self.image.shape[:2]
        return ofxh.ImageData(self.image, (0, 0, w, h))


class _LikeHostDemo(ofxh.ImageEffectInstance):
    """hostDemo's project (the defaults: PAL, PAR 768/720)."""

    def __init__(self, image, scale):
        super().__init__()
        self.image, self.scale = image, scale

    def new_clip_instance(self, desc, index):
        return _FileSource(None if desc.is_output else self.image, self.scale)


def _render_like_hostdemo(inst, time, scale):
    """hostDemo's renderFrame (hostDemo.cpp): the actions, the render window and the RoI."""
    OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
    assert inst.create_instance_action() in OK
    assert inst.get_clip_preferences_action() in OK
    assert inst.begin_sequence_render_action(time, time, render_scale=scale) in OK
    stat, rod = inst.get_region_of_definition_action(time, render_scale=scale)
    assert stat in OK
    out_clip = inst.clip("Output")
    par = out_clip.aspect_ratio
    r = ofxh.canonical_to_pixel(rod, scale, par)
    if inst.supports_tiles and out_clip.supports_tiles:
        p = inst.project_pixel_rect(scale)
        window = (max(r[0], p[0]), max(r[1], p[1]), min(r[2], p[2]), min(r[3], p[3]))
    else:
        window = r
    stat, _ = inst.is_identity_action(time, window, render_scale=scale)
    assert stat in OK
    assert inst.get_frames_needed_action(time)[0] in OK
    roi = ofxh.pixel_to_canonical(window, scale, par)
    assert inst.get_regions_of_interest_action(time, roi, render_scale=scale)[0] in OK
    stat, outputs = inst.render_action(time, window, render_scale=scale)
    assert stat == ofxh.Stat.OK
    assert inst.end_sequence_render_action(time, time, render_scale=scale) in OK
    return outputs["Colour"]


def test_rectangle_matches_hostdemo(plugin_dir, hostdemo):
    """A plug-in that draws in canonical coordinates (a rectangle at a normalised position), at
    render scale 0.5 and the PAL PAR: the RoD, the PAR and the render scale must be those of
    hostDemo, to the pixel."""
    scale = (0.5, 0.5)
    src = _source(seed=6, shape=(288, 360, 4))  # (at scale 0.5: the PAL frame, 720x576 pixels)
    r = ofxhost.render(plugin_dir, RECTANGLE, inputs={"Source": src}, frames=[0], scale=0.5,
                       host_binary=hostdemo)
    expected = next(iter(r.outputs.values()))
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    image = np.ascontiguousarray(src[::-1])
    with ofxh.create_instance(ofxh.get_plugin(RECTANGLE), "filter", _LikeHostDemo(image, scale)) as inst:
        out = _render_like_hostdemo(inst, 0., scale)
    assert out.pixels.dtype == np.float32
    assert out.pixels.shape == expected.shape
    np.testing.assert_array_equal(out.pixels[::-1], expected)
    assert not np.array_equal(out.pixels, image)  # (it drew something)


def test_cli_main(plugin_dir, tmp_path):
    """minimal_host.py's command line, in a new process; an instance left open is closed at exit,
    and nanobind reports no leak."""
    src = _to_depth(_source(seed=5), "float")
    np.save(tmp_path / "in.npy", src)
    script = f"""
import sys
import minimal_host, ofxh
rc = minimal_host.main(["minimal_host.py", {plugin_dir!r}, {INVERT!r}, "in.npy", "out.npy"])
ofxh.init(ofxh.Host(), [{plugin_dir!r}], use_env_path=False)
inst = ofxh.create_instance(ofxh.get_plugin({INVERT!r}), "filter", ofxh.ImageEffectInstance())
assert inst.create_instance_action() == ofxh.Stat.OK
sys.exit(rc)  # (inst is still open: ofxh.shutdown() at exit closes it)
"""
    env = dict(os.environ)
    env.pop("OFX_PLUGIN_PATH", None)
    env["PYTHONPATH"] = os.pathsep.join([str(Path(ofxh.__file__).resolve().parent),
                                         str(Path(minimal_host.__file__).resolve().parent)])
    (tmp_path / "run.py").write_text(script)
    cmd = [sys.executable, "-X", "faulthandler", "run.py"]
    # OFXH_TEST_LLDB=1: run it under lldb (a crash gives a backtrace, not a crash report)
    lldb = os.environ.get("OFXH_TEST_LLDB") == "1"
    if lldb:
        cmd = ["lldb", "--batch", "-o", "run", "-k", "bt all", "-k", "quit 1", "--"] + cmd
    p = subprocess.run(cmd, cwd=tmp_path, env=env, capture_output=True, text=True, timeout=300)
    assert p.returncode == 0, p.stdout + p.stderr
    if lldb:
        assert "exited with status = 0" in p.stdout, p.stdout + p.stderr
    assert "nanobind: leaked" not in p.stderr, p.stderr
    out = np.load(tmp_path / "out.npy")
    np.testing.assert_array_equal(out[..., :3], 1. - src[..., :3])
