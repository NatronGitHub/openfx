"""The Nuke extension's image planes (OFX_EXTENSIONS_NUKE, #350): a plug-in fetches a plane of an input's
components_present with clipGetImagePlane (FnOfxImageEffectPlaneSuiteV2, called through ctypes as a plug-in does),
and gets no image of a plane that isn't present, or of a plane of the output."""

import ctypes
import sys

import numpy as np
import pytest

from ofxhost.testing import suite_function

ofxh = pytest.importorskip("ofxh")

INVERT = "net.sf.openfx.invertPlugin"
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)

# the indexes of the suite functions
CLIP_GET_HANDLE = 3  # OfxImageEffectSuiteV1.clipGetHandle
CLIP_RELEASE_IMAGE = 6  # OfxImageEffectSuiteV1.clipReleaseImage
CLIP_GET_IMAGE_PLANE = 0  # FnOfxImageEffectPlaneSuiteV2.clipGetImagePlane
PROP_GET_STRING = 9  # OfxPropertySuiteV1.propGetString


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


def _image_plane(inst, clip, plane):
    """A plug-in's clipGetImagePlane (FnOfxImageEffectPlaneSuiteV2), at time 0, view 0: (status, image
    properties {components, premult} or None)."""
    handle = ctypes.c_void_p()
    get_handle = suite_function("OfxImageEffectSuite", 1, CLIP_GET_HANDLE, _CLIP_GET_HANDLE)
    assert get_handle(inst.handle, clip.encode(), ctypes.byref(handle), None) == ofxh.Stat.OK
    image = ctypes.c_void_p()
    st = suite_function("uk.co.thefoundry.FnOfxImageEffectPlaneSuite", 2, CLIP_GET_IMAGE_PLANE, _CLIP_GET_IMAGE_PLANE)(
        handle, 0., 0, plane.encode(), None, ctypes.byref(image))
    if st != ofxh.Stat.OK:
        return st, None
    get_string = suite_function("OfxPropertySuite", 1, PROP_GET_STRING, _PROP_GET_STRING)
    props = {}
    for key, name in (("components", "OfxImageEffectPropComponents"),
                      ("premult", "OfxImageEffectPropPreMultiplication"), ("uid", "OfxImagePropUniqueIdentifier")):
        v = ctypes.c_char_p()
        assert get_string(image, name.encode(), 0, ctypes.byref(v)) == ofxh.Stat.OK
        props[key] = v.value.decode()
    assert suite_function("OfxImageEffectSuite", 1, CLIP_RELEASE_IMAGE, _CLIP_RELEASE_IMAGE)(image) == ofxh.Stat.OK
    return st, props


_NUKE = "uk.co.thefoundry.Ofx"  # (the names of nuke/fnOfxExtensions.h)
COLOUR, BACK = _NUKE + "ImagePlaneColour", _NUKE + "ImagePlaneBackMotionVector"
PRESENT = _NUKE + "ImageEffectClipPropPlanesPresent"  # (kFnOfxImageEffectPropComponentsPresent)


@pytest.mark.skipif(not ofxh.build_config["OFX_EXTENSIONS_NUKE"], reason="the planes need OFX_EXTENSIONS_NUKE")
def test_image_planes(plugin_dir, monkeypatch):
    """The Nuke extension's planes: a plug-in fetches a plane of components_present from get_image(plane=...),
    and gets no image of a plane that isn't present, or of a plane of the output. Also
    get_clip_components_action() and the plane names. (#350)"""
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
