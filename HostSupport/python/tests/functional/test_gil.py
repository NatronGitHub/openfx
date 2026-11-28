"""The GIL is released during the actions: the plug-in calls back into the host (messages, parameter
values) during a render, other Python threads run while the plug-in works, and calling ofxh from
another thread while an action runs is refused."""

import ctypes
import threading

import numpy as np
import pytest

from ofxhost.testing import suite_function

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

    def message(self, message_type, message_id, text):
        self.messages.append((message_type, text))


def _prepare(inst):
    assert inst.create_instance_action() == ofxh.Stat.OK
    assert inst.get_clip_preferences_action() in OK


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
    the render's, for paramGetValue too (the timeline is at the render's time). (#197)"""

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
    output image, then the source, then processes the pixels. Clip.get_image() is Python for both
    (the output's calls the default), and the source's is the last Python code of the render: the
    other clip values are plain attributes, and the defaults are C++. The watcher sees the render
    running from outside (its call raises RuntimeError: a binding runs on the main thread) after
    that last statement. A few renders: the watcher must be scheduled while one is processing. (#197)"""
    image = np.random.default_rng(1).random((2048, 2048, 4), dtype=np.float32)
    state = {"phase": None, "observed": False, "stop": False}
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance(image, state=state)) as inst:
        _prepare(inst)

        def running():
            """a binding (the render) runs on the main thread"""
            try:
                inst.rendering
            except RuntimeError:
                return True
            return False

        def watch():
            while not state["stop"]:
                if state["phase"] == "processing" and running():
                    state["observed"] = True
                    return

        for _ in range(5):
            state.update(phase=None, stop=False)
            t = threading.Thread(target=watch)
            t.start()
            try:
                h, w = inst.image.shape[:2]
                assert inst.begin_sequence_render_action(0., 0.) in OK
                stat, outputs = inst.render_action(0., (0, 0, w, h))
                state["phase"] = "done"  # (the render returned: the watcher's checks no longer count)
                assert inst.end_sequence_render_action(0., 0.) in OK
            finally:
                state["stop"] = True
                t.join()
            assert stat == ofxh.Stat.OK
            np.testing.assert_array_equal(outputs["Colour"].pixels[..., :3], 1. - image[..., :3])
            if state["observed"]:
                break
        assert state["observed"]


def test_no_calls_from_another_thread_during_an_action(plugin_dir):
    """A Python thread's ofxh call during an action (here while the plug-in fetches its source) is refused with
    RuntimeError ("another thread"): the bindings run on the thread of the action; outside the actions, other threads
    may call ofxh. (#197)"""
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
        t = threading.Thread(target=call)
        t.start()
        t.join()
        assert len(errors) == 1


# OfxMessageSuiteV1.message, called as a plug-in's worker thread does (the text has no %)
MESSAGE = 0  # the index of OfxMessageSuiteV1.message
_MESSAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p)


def test_plugin_worker_thread_may_call_back(plugin_dir):
    """A thread of the plug-in (e.g. OpenMP) that calls the host during an action: inside that
    callback, the bindings that run no action may be called (here project_pixel_rect(), a scoped
    binding), but not the actions or set_param(). The thread is a Python thread started in a callback
    and joined there, which calls the message suite through ctypes, as a plug-in's thread would; a
    plain Python thread is refused (the test above). (#197)"""
    seen = []

    class Worker(Instance):
        def message(self, message_type, message_id, text):
            try:
                seen.append(("rect", self.project_pixel_rect()))
            except RuntimeError as e:
                seen.append(("error", str(e)))
            for name, call in (("rod", lambda: self.get_region_of_definition_action(0.)),
                               ("set_param", lambda: self.set_param("x", 1)),
                               ("begin", lambda: self.begin_instance_changed_action())):
                try:
                    call()
                    seen.append((name, "allowed"))
                except RuntimeError as e:
                    seen.append((name, "worker thread" in str(e)))

    def fetched():
        message = suite_function("OfxMessageSuite", 1, MESSAGE, _MESSAGE)
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
        assert seen == [("rect", (0, 0, 8, 8)), ("rod", True), ("set_param", True), ("begin", True),
                        ("status", ofxh.Stat.OK)]


def test_nested_action_restores_the_timeline(plugin_dir):
    """An action called by a callback of another one (a RoD at time 5 during the render at time
    10) restores the outer action's time; a top-level action leaves the timeline at its time. (#197)"""
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
        # the instance changed actions without a time: the current time (not 0)
        inst.begin_instance_changed_action()
        inst.clip_changed_action("Source")
        inst.end_instance_changed_action()
        assert inst.timeline_get_time() == 7.


def test_worker_errors_stay_in_their_thread(plugin_dir):
    """Two workers inside a callback: B's callback raises while A's scope is open (each scope of
    a worker has its own pending error): A's binding succeeds, B's raises B's error, and the render
    action raises nothing.

    A opens project_pixel_rect() and blocks in its first callback (project_offset); B's
    project_offset raises, and B blocks in project_size, with its error pending; A finishes, then
    B. With one pending error for all the scopes, A would raise B's error and B nothing. (#197)"""
    import time as _time
    ev_b_start, ev_a, ev_b = threading.Event(), threading.Event(), threading.Event()
    seen = {}
    order, waits = [], []

    class Workers(Instance):
        @property
        def project_offset(self):
            name = threading.current_thread().name
            if name == "A":
                order.append("A blocked")
                ev_b_start.set()
                waits.append(ev_a.wait(10))
            elif name == "B":
                order.append("B raised")
                raise ValueError("B's error")
            return (0., 0.)

        @property
        def project_size(self):
            if threading.current_thread().name == "B":
                ev_a.set()
                waits.append(ev_b.wait(10))
            h, w = self.image.shape[:2]
            return (w, h)

        @project_offset.setter
        def project_offset(self, value):  # (Instance.__init__ sets the project)
            pass

        @project_size.setter
        def project_size(self, value):
            pass

        def message(self, message_type, message_id, text):
            name = threading.current_thread().name
            try:
                seen[name] = ("rect", self.project_pixel_rect())
            except ofxh.CallbackError as e:
                seen[name] = ("error", str(e.__cause__))
            if name == "A":
                order.append("A done")
                ev_b.set()

    def fetched():
        message = suite_function("OfxMessageSuite", 1, MESSAGE, _MESSAGE)
        handle = inst_ref[0].handle

        def a():
            message(handle, b"OfxMessageLog", b"A", b"A")

        def b():
            ev_b_start.wait(10)
            _time.sleep(0.05)  # (A is blocked in project_offset)
            message(handle, b"OfxMessageLog", b"B", b"B")

        ta, tb = threading.Thread(target=a, name="A"), threading.Thread(target=b, name="B")
        ta.start()
        tb.start()
        ta.join()
        tb.join()

    inst_ref = []
    image = np.zeros((8, 8, 4), np.float32)
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Workers(image, fetched)) as inst:
        inst_ref.append(inst)
        stat, _ = _render(inst)  # (no CallbackError)
        assert stat == ofxh.Stat.OK
        assert seen == {"A": ("rect", (0, 0, 8, 8)), "B": ("error", "B's error")}
        assert waits == [True, True] and order == ["A blocked", "B raised", "A done"]


def test_render_actions_dont_nest(plugin_dir):
    """A callback of a render can't start another render (nor begin or end a sequence): it would
    release the outer render's output image. (#197)"""
    seen = []

    def fetched():
        inst = inst_ref[0]
        for name, call in (("render", lambda: inst.render_action(0., (0, 0, 8, 8))),
                           ("begin", lambda: inst.begin_sequence_render_action(0., 0.)),
                           ("end", lambda: inst.end_sequence_render_action(0., 0.))):
            try:
                call()
                seen.append((name, "allowed"))
            except RuntimeError as e:
                seen.append((name, "during a render" in str(e)))

    inst_ref = []
    image = np.random.default_rng(2).random((8, 8, 4), dtype=np.float32)
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance(image, fetched)) as inst:
        inst_ref.append(inst)
        stat, outputs = _render(inst)
        assert stat == ofxh.Stat.OK and seen == [("render", True), ("begin", True), ("end", True)]
        np.testing.assert_array_equal(outputs["Colour"].pixels[..., :3], 1. - image[..., :3])
