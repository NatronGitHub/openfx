"""Errors in the host callbacks: no exception reaches HostSupport or the plug-in. A callback that
raises makes the call fail (kOfxStatFailed, or a safe default), and the action re-raises the first
error as ofxh.CallbackError; ofxh.OfxError(status) gives a status, and is not an error."""

import numpy as np
import pytest

import ofxh

INVERT = "net.sf.openfx.invertPlugin"
TESTER = "net.sf.openfx.GenericTestPlugin"

OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)  # (the Support library replies default to instanceChanged)


class Recorder(ofxh.ImageEffectInstance):
    def __init__(self, fail=None):
        super().__init__()
        self.fail = fail
        self.messages = []

    def message(self, type, id, text):
        self.messages.append((type, text))
        if self.fail is not None:
            raise self.fail


def _tester(plugin_dir, fail=None):
    ofxh.init(ofxh.Host(), [plugin_dir])
    inst = ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", Recorder(fail))
    assert inst is not None
    assert inst.create() == ofxh.Stat.OK
    return inst


def _press(inst, button):
    inst.begin_instance_changed()
    try:
        return inst.param_changed(button)
    finally:
        inst.end_instance_changed()


def test_message(plugin_dir):
    with _tester(plugin_dir) as inst:
        assert _press(inst, "pbButton") in OK
        assert inst.messages == [("OfxMessageMessage", "Push Button Pressed - TestPassed!")]
        # the text is formatted in C++ (the plug-in passes it as a "%s" argument)
        assert inst.set_param("messageText", "100% %d %s") == ofxh.Stat.OK
        _press(inst, "messageText")
        assert inst.messages[-1] == ("OfxMessageWarning", "100% %d %s")  # (the persistent message)


def test_exception_in_callback(plugin_dir):
    with _tester(plugin_dir, fail=ValueError("boom")) as inst:
        with pytest.raises(ofxh.CallbackError) as e:
            _press(inst, "pbButton")
        assert isinstance(e.value.__cause__, ValueError)
        assert "boom" in str(e.value) and "instanceChanged" in str(e.value)
        assert e.value.status is not None
        # the error doesn't stay pending: the next action is fine
        inst.fail = None
        assert _press(inst, "pbButton") in OK


def test_ofx_error_is_a_status(plugin_dir):
    with _tester(plugin_dir, fail=ofxh.OfxError(ofxh.Stat.ReplyNo)) as inst:
        assert _press(inst, "pbButton") in OK  # no CallbackError
        assert len(inst.messages) == 1


class BadSource(ofxh.ClipInstance):
    unmapped_bit_depth = "float"

    def __init__(self, error):
        super().__init__()
        self.error = error

    def region_of_definition(self, time, view):
        if self.is_output:
            return super().region_of_definition(time, view)
        return (0., 0., 8 * self.aspect_ratio, 8.)

    def get_image(self, time, view, bounds):
        if self.is_output:
            return super().get_image(time, view, bounds)
        if self.error == "raise":
            raise KeyError("no image")
        if self.error == "dtype":
            return ofxh.ImageData(np.zeros((8, 8, 4), np.uint8), (0, 0, 8, 8))
        return ofxh.ImageData(np.zeros((4, 8, 4), np.float32), (0, 0, 8, 8))  # wrong shape


class BadInstance(ofxh.ImageEffectInstance):
    def __init__(self, error):
        super().__init__()
        self.error = error

    def new_clip_instance(self, desc, index):
        return BadSource(self.error)


@pytest.mark.parametrize("error,cause", [("raise", KeyError), ("dtype", ValueError), ("shape", ValueError)])
def test_bad_image(plugin_dir, error, cause):
    ofxh.init(ofxh.Host(), [plugin_dir])
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", BadInstance(error)) as inst:
        assert inst.create() == ofxh.Stat.OK
        assert inst.get_clip_preferences() == ofxh.Stat.OK
        assert inst.begin_sequence_render(0., 0.) in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
        with pytest.raises(ofxh.CallbackError) as e:
            inst.render(0., (0, 0, 8, 8))
        assert isinstance(e.value.__cause__, cause)
        assert e.value.status in (ofxh.Stat.OK, ofxh.Stat.Failed)  # (Invert renders nothing without a source)
        inst.end_sequence_render(0., 0.)
