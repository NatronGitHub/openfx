"""The GIL is released during the actions: the plug-in calls back into the host (messages, parameter
values) during a render, other Python threads run while the plug-in works, and calling ofxh from
another thread while an action runs is refused."""

import ctypes
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


def _prepare(inst):
    assert inst.create_instance_action() == ofxh.Stat.OK
    assert inst.get_clip_preferences_action() in OK


def _render_frame(inst, time=0.):
    h, w = inst.image.shape[:2]
    assert inst.begin_sequence_render_action(time, time) in OK
    stat, outputs = inst.render_action(time, (0, 0, w, h))
    assert inst.end_sequence_render_action(time, time) in OK
    return stat, outputs


def _render(inst, time=0.):
    _prepare(inst)
    h, w = inst.image.shape[:2]
    assert inst.begin_sequence_render_action(time, time) in OK
    stat, outputs = inst.render_action(time, (0, 0, w, h))
    assert inst.end_sequence_render_action(time, time) in OK
    return stat, outputs


def test_callbacks_during_render(plugin_dir):
    """Tester's render reads keysDouble with paramGetValue and paramGetValueAtTime, and sends a
    persistent message if they differ: here they do (a Python parameter whose value changes at each
    call), so the render calls the parameter and the message callbacks. get() always gets a time:
    the render's, for paramGetValue too (the timeline is at the render's time)."""

    class Keys(ofxh.Param):
        def __init__(self):
            super().__init__()
            self.times = []

        def get(self, time):
            self.times.append(time)
            return float(len(self.times))

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
        assert len(keys.times) >= 2 and set(keys.times) == {3.}
        assert ("error", "render: paramGetValue isn't the value at the render's time") in inst.messages


def test_other_threads_run_during_render(plugin_dir):
    """While the plug-in processes the image (after the last Python callback of the render), a
    Python thread runs: the GIL is released. (With the GIL held, no Python code could run there.)

    This relies on the fetch order of the Support Invert (Support/Plugins/Invert): it fetches the
    output image, then the source, then processes the pixels, and the source's get_image() is the
    only Python code of the render (the output's is the C++ default, the other clip values are
    plain attributes). A few renders: the watcher must be scheduled while one is processing."""
    image = np.random.default_rng(1).random((2048, 2048, 4), dtype=np.float32)
    state = {"phase": None, "observed": False, "stop": False}
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance(image, state=state)) as inst:
        _prepare(inst)

        def watch():
            while not state["stop"]:
                if state["phase"] == "processing" and inst.rendering:
                    state["observed"] = True
                    return

        for _ in range(5):
            state.update(phase=None, stop=False)
            t = threading.Thread(target=watch)
            t.start()
            try:
                stat, outputs = _render_frame(inst)
            finally:
                state["stop"] = True
                t.join()
            assert stat == ofxh.Stat.OK
            np.testing.assert_array_equal(outputs["Colour"].pixels[..., :3], 1. - image[..., :3])
            if state["observed"]:
                break
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


# OfxMessageSuiteV1.message, called as a plug-in's worker thread does (the text has no %)
_MESSAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p)


def test_plugin_worker_thread_may_call_back(plugin_dir):
    """A thread of the plug-in (e.g. OpenMP) that calls the host during an action: inside that
    callback, the bindings may be called (here project_pixel_rect(), a scoped binding). The thread is
    a Python thread started in a callback and joined there, which calls the message suite through
    ctypes, as a plug-in's thread would; a plain Python thread is refused (the test above)."""
    seen = []

    class Worker(Instance):
        def message(self, type, id, text):
            try:
                seen.append(("rect", self.project_pixel_rect()))
            except RuntimeError as e:
                seen.append(("error", str(e)))

    def fetched():
        suite = ofxh.fetch_suite("OfxMessageSuite", 1)
        message = _MESSAGE(ctypes.c_void_p.from_address(suite).value)
        handle = inst_ref[0].handle

        def worker():
            seen.append(("status", message(handle, b"OfxMessageLog", b"worker", b"from a worker")))

        t = threading.Thread(target=worker)
        t.start()
        t.join()

    inst_ref = []
    image = np.zeros((8, 8, 4), np.float32)
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Worker(image, fetched)) as inst:
        inst_ref.append(inst)
        stat, _ = _render(inst)
        assert stat == ofxh.Stat.OK
        assert seen == [("rect", (0, 0, 8, 8)), ("status", ofxh.Stat.OK)]


def test_nested_action_restores_the_timeline(plugin_dir):
    """An action called by a callback of another one (a RoD at time 5 during the render at time
    10) restores the outer action's time; a top-level action leaves the timeline at its time."""
    seen = []

    def fetched():
        inst = inst_ref[0]
        seen.append(inst.timeline_get_time())
        stat, _ = inst.get_region_of_definition_action(5.)
        seen.append((stat, inst.timeline_get_time()))

    inst_ref = []
    image = np.zeros((8, 8, 4), np.float32)
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance(image, fetched)) as inst:
        inst_ref.append(inst)
        stat, _ = _render(inst, time=10.)
        assert stat == ofxh.Stat.OK
        assert seen == [10., (ofxh.Stat.OK, 10.)] or seen == [10., (ofxh.Stat.ReplyDefault, 10.)]
        assert inst.timeline_get_time() == 10.
        inst.get_region_of_definition_action(7.)
        assert inst.timeline_get_time() == 7.
