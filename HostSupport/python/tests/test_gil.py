"""The GIL is released during the actions: the plug-in calls back into the host (messages, parameter
values) during a render, other Python threads run while the plug-in works, and calling ofxh from
another thread while an action runs is refused."""

import threading

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")

INVERT = "net.sf.openfx.invertPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)


class Clip(ofxh.ClipInstance):
    """An input clip with an image (plain attributes: no Python code but get_image())."""

    connected = True
    unmapped_bit_depth = "float"
    unmapped_components = "rgba"

    def __init__(self, image=None, on_fetch=None, state=None):
        super().__init__()
        self.image = image
        self.on_fetch = on_fetch
        self.state = state

    def get_image(self, time, view, bounds, plane="Colour"):
        if self.is_output:
            return super().get_image(time, view, bounds, plane)
        if self.on_fetch is not None:
            self.on_fetch()
        h, w = self.image.shape[:2]
        r = ofxh.ImageData(self.image, (0, 0, w, h))
        if self.state is not None:
            # the last statement (no call after it, so no switch to another thread before the
            # callback returns to the plug-in)
            self.state["phase"] = "processing"
        return r


class Instance(ofxh.ImageEffectInstance):
    def __init__(self, image, on_fetch=None, state=None):
        super().__init__()
        self.image = image
        self.on_fetch = on_fetch
        self.state = state
        h, w = image.shape[:2]
        self.project_size = self.project_extent = (w, h)
        self.project_pixel_aspect_ratio = 1.
        self.messages = []

    def new_clip_instance(self, desc, index):
        return Clip() if desc.is_output else Clip(self.image, self.on_fetch, self.state)

    def message(self, type, id, text):
        self.messages.append((type, text))


def _render(inst, time=0.):
    assert inst.create_instance_action() == ofxh.Stat.OK
    assert inst.get_clip_preferences_action() in OK
    h, w = inst.image.shape[:2]
    assert inst.begin_sequence_render_action(time, time) in OK
    stat, outputs = inst.render_action(time, (0, 0, w, h))
    assert inst.end_sequence_render_action(time, time) in OK
    return stat, outputs


def test_callbacks_during_render(plugin_dir):
    """Tester's render reads keysDouble with paramGetValue and paramGetValueAtTime, and sends a
    persistent message if they differ: here they do (a Python parameter), so the render calls
    the parameter and the message callbacks."""

    class Keys(ofxh.Param):
        def __init__(self):
            super().__init__()
            self.times = []

        def get(self, time=None):
            self.times.append(time)
            return 1. if time is None else 2.

        def set(self, value, time=None):
            pass

    class Tester(Instance):
        def new_param(self, desc):
            return Keys() if desc.name == "keysDouble" else None

    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    image = np.random.default_rng(0).random((32, 48, 4), dtype=np.float32)
    with ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", Tester(image)) as inst:
        keys = inst.param("keysDouble")
        stat, outputs = _render(inst, time=3.)
        assert stat == ofxh.Stat.OK and outputs["Colour"].pixels.shape == (32, 48, 4)
        assert None in keys.times and 3. in keys.times
        assert ("error", "render: paramGetValue isn't the value at the render's time") in inst.messages


def test_other_threads_run_during_render(plugin_dir):
    """While the plug-in processes the image (after the last Python callback of the render), a
    Python thread runs: the GIL is released. (With the GIL held, no Python code could run there:
    the input's get_image() is the only Python code of the render.)"""
    state = {"phase": None, "observed": False, "stop": False}
    image = np.random.default_rng(1).random((2048, 2048, 4), dtype=np.float32)
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance(image, state=state)) as inst:

        def watch():
            while not state["stop"]:
                if state["phase"] == "processing" and inst.rendering:
                    state["observed"] = True
                    return

        t = threading.Thread(target=watch)
        t.start()
        try:
            stat, outputs = _render(inst)
        finally:
            state["stop"] = True
            t.join()
        assert stat == ofxh.Stat.OK
        np.testing.assert_array_equal(outputs["Colour"].pixels[..., :3], 1. - image[..., :3])
        assert state["observed"]


def test_no_calls_from_another_thread_during_an_action(plugin_dir):
    errors = []
    inst_ref = []

    def call():
        try:
            inst_ref[0].project_pixel_rect()
        except RuntimeError as e:
            errors.append(str(e))

    def fetched():
        t = threading.Thread(target=call)
        t.start()
        t.join()  # (releases the GIL: the thread runs during the action)

    image = np.zeros((8, 8, 4), np.float32)
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance(image, fetched)) as inst:
        inst_ref.append(inst)
        stat, _ = _render(inst)
        assert stat == ofxh.Stat.OK
        assert len(errors) == 1 and "another thread" in errors[0]
        # outside the actions, other threads may call ofxh
        call()
        assert len(errors) == 1
