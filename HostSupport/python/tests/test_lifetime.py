"""Lifetime: the Python objects of the C++ objects that HostSupport deletes raise ReferenceError."""

import gc

import pytest

import ofxh

INVERT = "net.sf.openfx.invertPlugin"


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


def _instance(plugin_dir):
    ofxh.init(ofxh.Host(), [plugin_dir])
    inst = ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance())
    assert inst is not None
    return inst


def test_clip_after_close(plugin_dir):
    inst = _instance(plugin_dir)
    assert sorted(inst.made) == [("Output", 1), ("Source", 0)]
    clip = inst.clip("Source")
    assert clip.alive and clip.name == "Source" and not clip.is_output
    assert clip.instance is inst
    props = clip.props
    assert props["OfxPropName"] == ("Source",)
    assert inst.create() == ofxh.Stat.OK
    assert inst.close() == ofxh.Stat.OK
    assert not inst.alive and not clip.alive
    with pytest.raises(ReferenceError):
        clip.name
    with pytest.raises(ReferenceError):
        props["OfxPropName"]
    with pytest.raises(ReferenceError):
        inst.render(0., (0, 0, 1, 1))
    assert inst.close() is None  # idempotent
    # the plug-in stays usable
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance()) as again:
        assert again.create() == ofxh.Stat.OK
    assert not again.alive


def test_unbound_objects():
    with pytest.raises(ReferenceError):
        Clip().name
    with pytest.raises(ReferenceError):
        Instance().render(0., (0, 0, 1, 1))
    with pytest.raises(RuntimeError):
        ofxh.get_plugin(INVERT)  # not initialized


def test_shutdown_closes_instances(plugin_dir):
    host = ofxh.Host()
    ofxh.init(host, [plugin_dir])
    plugin = ofxh.get_plugin(INVERT)
    inst = ofxh.create_instance(plugin, "filter", Instance())
    assert inst.create() == ofxh.Stat.OK
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
    ofxh.init(ofxh.Host(), [plugin_dir])
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", Instance()) as inst2:
        assert inst2.create() == ofxh.Stat.OK


def test_instance_without_close_is_closed_by_shutdown(plugin_dir):
    inst = _instance(plugin_dir)
    assert inst.create() == ofxh.Stat.OK
    clip = inst.clip("Source")
    del inst
    gc.collect()
    assert clip.alive  # the C++ instance keeps its Python object until close() or shutdown()
    ofxh.shutdown()
    assert not clip.alive


def test_failed_creation_unbinds(plugin_dir):
    class Broken(ofxh.ImageEffectInstance):
        def new_clip_instance(self, desc, index):
            raise ValueError("no clip for you")

    ofxh.init(ofxh.Host(), [plugin_dir])
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
