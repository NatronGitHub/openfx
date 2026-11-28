"""Lifetime: the Python objects of the C++ objects that HostSupport deletes raise ReferenceError;
close() and shutdown() can't run inside an action."""

import gc

import pytest

ofxh = pytest.importorskip("ofxh")

INVERT = "net.sf.openfx.invertPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"


class Clip(ofxh.ClipInstance):
    pass


class Instance(ofxh.ImageEffectInstance):
    def __init__(self):
        super().__init__()
        self.made = []

    def new_clip_instance(self, desc, index):
        c = Clip()
        self.made.append((desc.name, index))
        return c


def _init(plugin_dir, host=None):
    ofxh.init(host or ofxh.Host(), [plugin_dir], use_env_path=False)


def _instance(plugin_dir, obj=None, plugin=INVERT):
    _init(plugin_dir)
    inst = ofxh.create_instance(ofxh.get_plugin(plugin), "filter", obj or Instance())
    assert inst is not None
    return inst


def test_clip_after_close(plugin_dir):
    """After close(), the instance's clips and properties raise ReferenceError, and so do its actions; close() again is
    a no-op, and the plug-in stays usable for a new instance: HostSupport deleted the C++ objects, and the Python
    objects say so instead of crashing. (#194)"""
    inst = _instance(plugin_dir)
    assert sorted(inst.made) == [("Output", 1), ("Source", 0)]
    clip = inst.clip("Source")
    assert clip.alive and clip.name == "Source" and not clip.is_output
    assert clip.instance is inst
    props = clip.props
    assert props["OfxPropName"] == ("Source",)
    assert inst.create_instance_action() == ofxh.Stat.OK
    assert inst.close() is None
    assert not inst.alive and not clip.alive
    with pytest.raises(ReferenceError):
        clip.name
    with pytest.raises(ReferenceError):
        props["OfxPropName"]
    with pytest.raises(ReferenceError):
        inst.render_action(0., (0, 0, 1, 1))
    assert inst.close() is None  # a no-op
    # the plug-in stays usable
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance()) as again:
        assert again.create_instance_action() == ofxh.Stat.OK
    assert not again.alive


def test_unbound_objects():
    """A ClipInstance or ImageEffectInstance that HostSupport didn't make (unbound) raises ReferenceError; get_plugin()
    before init() is a RuntimeError. (#194)"""
    with pytest.raises(ReferenceError):
        Clip().name
    with pytest.raises(ReferenceError):
        Instance().render_action(0., (0, 0, 1, 1))
    with pytest.raises(RuntimeError):
        ofxh.get_plugin(INVERT)  # not initialized


def test_shutdown_closes_instances(plugin_dir):
    """shutdown() sends kOfxActionDestroyInstance and deletes the instances, clips, plug-ins and host (their Python
    objects raise ReferenceError); a second init() before it is refused (one host per process), a second shutdown()
    does nothing, and a new init() loads the plug-ins again. (#194)"""
    host = ofxh.Host()
    _init(plugin_dir, host)
    plugin = ofxh.get_plugin(INVERT)
    inst = ofxh.create_instance(plugin, "filter", Instance())
    assert inst.create_instance_action() == ofxh.Stat.OK
    clip = inst.clip("Output")
    with pytest.raises(RuntimeError):
        ofxh.init(ofxh.Host(), [plugin_dir])  # one host per process
    ofxh.shutdown()  # kOfxActionDestroyInstance, delete, unload the plug-ins, delete the host
    assert not ofxh.initialized()
    assert not inst.alive and not clip.alive and not host.alive
    with pytest.raises(ReferenceError):
        plugin.id
    with pytest.raises(ReferenceError):
        host.props
    ofxh.shutdown()  # (nothing to do)
    # a new host, and the plug-ins loaded again
    _init(plugin_dir)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance()) as inst2:
        assert inst2.create_instance_action() == ofxh.Stat.OK


def test_instance_without_close_is_closed_by_shutdown(plugin_dir):
    """An instance dropped by Python without close() stays alive (the C++ instance keeps its Python object) until
    shutdown(), which closes it. (#194)"""
    inst = _instance(plugin_dir)
    assert inst.create_instance_action() == ofxh.Stat.OK
    clip = inst.clip("Source")
    del inst
    gc.collect()
    assert clip.alive  # the C++ instance keeps its Python object until close() or shutdown()
    ofxh.shutdown()
    assert not clip.alive


def test_deleted_before_python_objects_drop(plugin_dir):
    """The Python objects are already deleted (ReferenceError) when the adapters drop them. (#194)"""
    seen = []

    class Watched(ofxh.ClipInstance):
        def __del__(self):
            seen.append(self.alive)

    class Inst(ofxh.ImageEffectInstance):
        def new_clip_instance(self, desc, index):
            return Watched()

    inst = _instance(plugin_dir, Inst())
    inst.close()
    del inst
    gc.collect()
    assert seen == [False, False]


def test_failed_creation_unbinds(plugin_dir):
    """new_clip_instance() that raises, or returns something that isn't a ClipInstance: create_instance() raises
    CallbackError with the cause (ValueError, TypeError), and the Python instance is unbound (no half-made instance
    stays). (#194; an error after the instance is made:
    test_errors.py::test_create_instance_error_carries_the_instance)"""
    class Broken(ofxh.ImageEffectInstance):
        def new_clip_instance(self, desc, index):
            raise ValueError("no clip for you")

    _init(plugin_dir)
    obj = Broken()
    with pytest.raises(ofxh.CallbackError) as e:
        ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", obj)
    assert isinstance(e.value.__cause__, ValueError)
    assert not obj.alive

    class WrongType(ofxh.ImageEffectInstance):
        def new_clip_instance(self, desc, index):
            return object()

    with pytest.raises(ofxh.CallbackError) as e:
        ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", WrongType())
    assert isinstance(e.value.__cause__, TypeError)


def test_failed_init_shuts_down(plugin_dir):
    """init_properties() that raises: init() raises that exception (it calls init_properties() itself, no CallbackError)
    and leaves no host, so that a new init() works. (#194)"""
    class BadHost(ofxh.Host):
        def init_properties(self, props):
            raise KeyError("no properties")

    with pytest.raises(KeyError):  # (init() calls init_properties() itself: no CallbackError)
        ofxh.init(BadHost(), [plugin_dir], use_env_path=False)
    assert not ofxh.initialized()
    _init(plugin_dir)  # a clean state


@pytest.mark.parametrize("call", ["close", "shutdown"])
def test_close_and_shutdown_refused_in_callbacks(plugin_dir, call):
    """close() and shutdown() inside a callback of an action (Tester's pbButton message) are refused with RuntimeError
    (they would delete the objects the action uses): the action's CallbackError, and the instance and host stay.
    (#194)"""
    class Closer(ofxh.ImageEffectInstance):
        def message(self, message_type, message_id, text):
            self.close() if call == "close" else ofxh.shutdown()

    inst = _instance(plugin_dir, Closer(), plugin=TESTER)
    assert inst.create_instance_action() == ofxh.Stat.OK
    inst.begin_instance_changed_action()
    with pytest.raises(ofxh.CallbackError) as e:
        inst.param_changed_action("pbButton")  # the Tester sends a message
    inst.end_instance_changed_action()
    assert isinstance(e.value.__cause__, RuntimeError)
    assert inst.alive and ofxh.initialized()


def test_settable_callbacks(plugin_dir):
    """A callback may be a plain attribute: an instance's frame_rate set in __init__ is what the plug-in gets
    (effect_duration keeps its default), a clip's frame_rate defaults to its instance's and can be set. (#194)"""
    class Thirty(ofxh.ImageEffectInstance):
        def __init__(self):
            super().__init__()
            self.frame_rate = 30.

    inst = _instance(plugin_dir, Thirty())
    assert inst.frame_rate == 30. and inst.effect_duration == 25.
    clip = inst.clip("Source")
    assert clip.frame_rate == 30.  # (the default: the instance's)
    clip.frame_rate = 24.
    assert clip.frame_rate == 24.
    assert inst.props["OfxImageEffectPropFrameRate"] == (30.,)
