"""Errors in the host callbacks: no exception reaches HostSupport or the plug-in. A callback that
raises makes the call fail (kOfxStatFailed, or a safe default), and the action re-raises the first
error as ofxh.CallbackError; ofxh.OfxError(status) gives a status, and is not an error."""

import ctypes
import sys

import numpy as np
import pytest

ofxh = pytest.importorskip("ofxh")

INVERT = "net.sf.openfx.invertPlugin"  # (Support/Plugins/Invert doesn't check that it got its output image)
TESTER = "net.sf.openfx.GenericTestPlugin"

OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)  # (the Support library replies default to instanceChanged)


class Recorder(ofxh.ImageEffectInstance):
    def __init__(self, fail=None):
        super().__init__()
        self.fail = fail
        self.messages = []

    def message(self, message_type, message_id, text):
        self.messages.append((message_type, text))
        if self.fail is not None:
            raise self.fail


def _tester(plugin_dir, fail=None):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    inst = ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", Recorder(fail))
    assert inst is not None
    assert inst.create_instance_action() == ofxh.Stat.OK
    return inst


def _press(inst, button):
    inst.begin_instance_changed_action()
    try:
        return inst.param_changed_action(button)
    finally:
        inst.end_instance_changed_action()


# OfxMessageSuiteV1.message, called as a plug-in does (no variadic arguments: the text has no %)
_MESSAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p)


def _plugin_message(inst, message_type, text):
    suite = ofxh.fetch_suite("OfxMessageSuite", 1)
    assert suite
    message = _MESSAGE(ctypes.c_void_p.from_address(suite).value)
    return message(inst.handle, message_type.encode(), b"test", text.encode())


def test_message(plugin_dir):
    with _tester(plugin_dir) as inst:
        assert _press(inst, "pbButton") in OK
        assert inst.messages == [("message", "Push Button Pressed - TestPassed!")]
        # the text is formatted in C++ (the plug-in passes it as a "%s" argument)
        assert inst.set_param("messageText", "100% %d %s") == ofxh.Stat.OK
        _press(inst, "messageText")
        assert inst.messages[-1] == ("warning", "100% %d %s")  # (the persistent message)


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


def test_plugin_gets_the_status(plugin_dir):
    """What the plug-in's message suite call returns: the OfxError status, kOfxStatFailed for an
    exception (reported with sys.unraisablehook, outside any action), and the callback's Stat."""
    with _tester(plugin_dir, fail=ofxh.OfxError(ofxh.Stat.ReplyNo)) as inst:
        assert _plugin_message(inst, "OfxMessageQuestion", "sure?") == ofxh.Stat.ReplyNo
        assert inst.messages == [("question", "sure?")]
        inst.fail = None
        assert _plugin_message(inst, "OfxMessageMessage", "hello") == ofxh.Stat.OK


def test_plugin_gets_failed_and_error_is_reported(plugin_dir, monkeypatch):
    reported = []
    monkeypatch.setattr("sys.unraisablehook", lambda u: reported.append(u.exc_value))
    with _tester(plugin_dir, fail=ValueError("boom")) as inst:
        assert _plugin_message(inst, "OfxMessageError", "oops") == ofxh.Stat.Failed
    assert len(reported) == 1 and isinstance(reported[0], ValueError)


class BadSource(ofxh.ClipInstance):
    unmapped_bit_depth = "float"

    def __init__(self, error):
        super().__init__()
        self.error = error

    def region_of_definition(self, time, view):
        if self.is_output:
            return super().region_of_definition(time, view)
        return (0., 0., 8 * self.aspect_ratio, 8.)

    def get_image(self, time, view, bounds, plane="Colour"):
        if self.error is None:  # a good image
            if self.is_output:
                return super().get_image(time, view, bounds, plane)
            return ofxh.ImageData(np.zeros((8, 8, 4), np.float32), (0, 0, 8, 8))
        if self.is_output:
            if self.error == "small_output":
                return ofxh.ImageData(self.new_image((0, 0, 4, 4)), (0, 0, 4, 4))
            if self.error == "none_output":
                return None
            if self.error == "raise_output":
                raise KeyError("no output")
            return super().get_image(time, view, bounds, plane)
        if self.error == "raise":
            raise KeyError("no image")
        if self.error == "dtype":
            return ofxh.ImageData(np.zeros((8, 8, 4), np.uint8), (0, 0, 8, 8))
        if self.error == "readonly":
            a = np.zeros((8, 8, 4), np.float32)
            a.flags.writeable = False
            return ofxh.ImageData(a, (0, 0, 8, 8))
        if self.error.endswith("_output"):
            return ofxh.ImageData(np.zeros((8, 8, 4), np.float32), (0, 0, 8, 8))
        return ofxh.ImageData(np.zeros((4, 8, 4), np.float32), (0, 0, 8, 8))  # wrong shape


class BadInstance(ofxh.ImageEffectInstance):
    def __init__(self, error):
        super().__init__()
        self.error = error

    def new_clip_instance(self, desc, index):
        return BadSource(self.error)


@pytest.mark.parametrize("error,cause", [("raise", KeyError), ("dtype", ValueError), ("shape", ValueError),
                                         ("readonly", ValueError), ("small_output", ValueError),
                                         ("none_output", RuntimeError), ("raise_output", KeyError)])
def test_bad_image(plugin_dir, error, cause):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", BadInstance(error)) as inst:
        assert inst.create_instance_action() in OK
        assert inst.get_clip_preferences_action() == ofxh.Stat.OK
        assert inst.begin_sequence_render_action(0., 0.) in OK
        with pytest.raises(ofxh.CallbackError) as e:
            inst.render_action(0., (0, 0, 8, 8))
        assert isinstance(e.value.__cause__, cause)
        if error.endswith("_output"):
            # the plug-in got an output image that ofxh made (never NULL during a render), and the
            # render fails
            assert e.value.status == ofxh.Stat.Failed
            assert "no valid output image" in str(e.value) or error != "none_output"
        else:
            # the plug-in got no source image: Invert renders nothing, and succeeds
            assert e.value.status == ofxh.Stat.OK
        # a render with good images after the failed one: the failure (and, for the output errors,
        # the flag that makes abort() return 1 and the render fail) doesn't carry over
        inst.clip("Source").error = None
        inst.clip("Output").error = None
        st, out = inst.render_action(0., (0, 0, 8, 8))
        assert st == ofxh.Stat.OK
        assert "Colour" in out
        assert not inst.aborted
        inst.end_sequence_render_action(0., 0.)


def test_unsupported_arguments(plugin_dir):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", BadInstance(None)) as inst:
        assert inst.create_instance_action() == ofxh.Stat.OK
        with pytest.raises(ValueError):
            inst.render_action(0., (0, 0, 8, 8), planes=("Colour", "Colour"))
        with pytest.raises(ValueError):
            inst.render_action(0., (0, 0, 8, 8), planes=("uk.co.thefoundry.OfxImagePlaneBackwardMotionVector",))
        cfg = ofxh.build_config
        if not cfg["OFX_EXTENSIONS_NUKE"]:
            with pytest.raises(ValueError):
                inst.get_region_of_definition_action(0., view=1)
        if not (cfg["OFX_EXTENSIONS_NUKE"] or cfg["OFX_EXTENSIONS_VEGAS"]):
            with pytest.raises(ValueError):
                inst.render_action(0., (0, 0, 8, 8), view=1)
        if not cfg["OFX_EXTENSIONS_VEGAS"]:
            with pytest.raises(ValueError):
                inst.render_action(0., (0, 0, 8, 8), n_views=2)


class PlaneSource(ofxh.ClipInstance):
    """An input with a backward motion vector plane (components_present), whose images are 8x8x2."""
    unmapped_bit_depth = "float"

    def __init__(self):
        super().__init__()
        self.asked = []
        self.planes = ["Colour", "BackMotionVector"]  # (what components_present returns for an input)
        self.n_comps = 2  # (of the plane images)

    @property
    def components_present(self):
        return super().components_present if self.is_output else self.planes

    def get_image(self, time, view, bounds, plane="Colour"):
        self.asked.append(plane)
        if self.is_output or plane == "Colour":
            return super().get_image(time, view, bounds, plane)
        return ofxh.ImageData(np.full((8, 8, self.n_comps), 0.5, np.float32), (0, 0, 8, 8))


class PlaneInstance(ofxh.ImageEffectInstance):
    def new_clip_instance(self, desc, index):
        return PlaneSource()


_CLIP_GET_HANDLE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p),
                                    ctypes.c_void_p)
_CLIP_RELEASE_IMAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
_CLIP_GET_IMAGE_PLANE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_double, ctypes.c_int,
                                         ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
_PROP_GET_STRING = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_char_p))


def _suite_function(name, version, index, prototype):
    suite = ofxh.fetch_suite(name, version)
    assert suite
    return prototype(ctypes.c_void_p.from_address(suite + index * ctypes.sizeof(ctypes.c_void_p)).value)


def _image_plane(inst, clip, plane):
    """A plug-in's clipGetImagePlane (FnOfxImageEffectPlaneSuiteV2), at time 0, view 0: (status, image
    properties {components, premult} or None)."""
    handle = ctypes.c_void_p()
    get_handle = _suite_function("OfxImageEffectSuite", 1, 3, _CLIP_GET_HANDLE)
    assert get_handle(inst.handle, clip.encode(), ctypes.byref(handle), None) == ofxh.Stat.OK
    image = ctypes.c_void_p()
    st = _suite_function("uk.co.thefoundry.FnOfxImageEffectPlaneSuite", 2, 0, _CLIP_GET_IMAGE_PLANE)(
        handle, 0., 0, plane.encode(), None, ctypes.byref(image))
    if st != ofxh.Stat.OK:
        return st, None
    get_string = _suite_function("OfxPropertySuite", 1, 9, _PROP_GET_STRING)
    props = {}
    for key, name in (("components", "OfxImageEffectPropComponents"),
                      ("premult", "OfxImageEffectPropPreMultiplication"), ("uid", "OfxImagePropUniqueIdentifier")):
        v = ctypes.c_char_p()
        assert get_string(image, name.encode(), 0, ctypes.byref(v)) == ofxh.Stat.OK
        props[key] = v.value.decode()
    assert _suite_function("OfxImageEffectSuite", 1, 6, _CLIP_RELEASE_IMAGE)(image) == ofxh.Stat.OK
    return st, props


_NUKE = "uk.co.thefoundry.Ofx"  # (the names of nuke/fnOfxExtensions.h)
COLOUR, BACK = _NUKE + "ImagePlaneColour", _NUKE + "ImagePlaneBackMotionVector"
PRESENT = _NUKE + "ImageEffectClipPropPlanesPresent"  # (kFnOfxImageEffectPropComponentsPresent)


@pytest.mark.skipif(not ofxh.build_config["OFX_EXTENSIONS_NUKE"], reason="the planes need OFX_EXTENSIONS_NUKE")
def test_image_planes(plugin_dir, monkeypatch):
    """The Nuke extension's planes: a plug-in fetches a plane of components_present from get_image(plane=...),
    and gets no image of a plane that isn't present, or of a plane of the output. Also
    get_clip_components_action() and the plane names."""
    assert ofxh.ofx_name("plane", "BackMotionVector") == BACK
    assert ofxh.short_name("plane", _NUKE + "ImagePlaneStereoDisparityLeft") == "StereoDisparityLeft"
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", PlaneInstance()) as inst:
        assert inst.create_instance_action() in OK
        assert inst.get_clip_preferences_action() == ofxh.Stat.OK
        source = inst.clip("Source")
        assert source.props[PRESENT] == (COLOUR, BACK)
        assert inst.clip("Output").props[PRESENT] == (COLOUR,)
        st, props = _image_plane(inst, "Source", BACK)
        assert st == ofxh.Stat.OK
        assert props == {"components": _NUKE + "ImageComponentMotionVectors", "premult": "OfxImageOpaque",
                         "uid": "Source:0:0:" + BACK}
        assert source.asked == ["BackMotionVector"]
        assert _image_plane(inst, "Source", _NUKE + "ImagePlaneForwardMotionVector") == (ofxh.Stat.Failed, None)
        assert _image_plane(inst, "Output", BACK) == (ofxh.Stat.Failed, None)
        assert source.asked == ["BackMotionVector"]
        # (outside an action, a callback's error goes to sys.unraisablehook)
        errors = []
        monkeypatch.setattr(sys, "unraisablehook", lambda u: errors.append(u.exc_value))
        # a plane image of the wrong shape: no image, and the error
        source.n_comps = 4
        assert _image_plane(inst, "Source", BACK) == (ofxh.Stat.Failed, None)
        assert len(errors) == 1 and "the plane's components" in str(errors[0]), errors
        # a str isn't a list of planes (TypeError); a plug-in then gets the last good value
        source.planes, source.n_comps = "Colour", 2
        assert _image_plane(inst, "Source", BACK)[0] == ofxh.Stat.OK
        assert len(errors) == 2 and isinstance(errors[1], TypeError), errors
        with pytest.raises(ofxh.CallbackError) as e:
            source.props[PRESENT]
        assert isinstance(e.value.__cause__, TypeError)
        # (Invert isn't multi-plane: the Support library's default)
        st, cc = inst.get_clip_components_action(0.)
        assert st == ofxh.Stat.ReplyDefault
        assert cc == {"clips": {}, "pass_through_clip": None, "pass_through_time": 0., "pass_through_view": 0}


def test_render_without_clip_preferences(plugin_dir):
    """render_action() before any clip preferences (createInstance sets the default ones): the
    output clip has no depth yet, ofxh couldn't make an output image, so it refuses to render
    (the plug-in isn't called)."""
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    with ofxh.create_instance(ofxh.get_plugin(INVERT), "filter", BadInstance(None)) as inst:
        assert inst.clip("Output").pixel_depth == "none"
        with pytest.raises(ValueError, match="get_clip_preferences_action"):
            inst.render_action(0., (0, 0, 8, 8))


RECTANGLE = "uk.co.thefoundry.GeneratorExample"  # (normalised param defaults: they read project_size)


def test_create_instance_error_carries_the_instance(plugin_dir):
    class BadProject(ofxh.ImageEffectInstance):
        @property
        def project_size(self):
            raise ArithmeticError("no project")

    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    obj = BadProject()
    with pytest.raises(ofxh.CallbackError) as e:
        ofxh.create_instance(ofxh.get_plugin(RECTANGLE), "filter", obj)
    assert isinstance(e.value.__cause__, ArithmeticError)
    assert e.value.instance is obj and obj.alive  # the instance was made: the caller closes it
    obj.close()
    assert not obj.alive
