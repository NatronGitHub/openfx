"""Parameters: ofxh's C++ parameters (stored values, keys and linear interpolation, as hostDemo's
MyCurve), the Python parameters (ofxh.Param, made by new_param()), project loads and push buttons.

The plug-in is the Support Tester (GenericTest): its keysTest button checks the keys, the
interpolation, the derivative and the integral through the parameter suite, and messageText and
pbButton send messages from instanceChanged. Also param_props(), which follows instanceChanged's setEnabled (with
Basic)."""

import bisect
import gc

import pytest

ofxh = pytest.importorskip("ofxh")
from ofxhost import constants as k  # noqa: E402

TESTER = "net.sf.openfx.GenericTestPlugin"
BASIC = "net.sf.openfx.basicPlugin"  # instanceChanged: scaleComponents enables the per-component scales
CHOICES = "org.openeffects.support.choiceParamsPlugin"  # Choice params and a StrChoice param
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)  # (the Support library replies default to instanceChanged)


class Recorder(ofxh.ImageEffectInstance):
    """Records the messages; new_param() makes the Python parameters of self.python (names, or
    "*" for all)."""

    def __init__(self, python=(), param_class=None):
        super().__init__()
        self.messages = []
        self.python = python
        self.param_class = param_class
        self.log = []
        self.descriptors = []

    def message(self, message_type, message_id, text):
        self.messages.append((message_type, text))

    def new_param(self, desc):
        self.descriptors.append(desc)
        if self.python == "*" or desc.name in self.python:
            return self.param_class(desc, self.log)
        return None


class Value(ofxh.Param):
    """A Python parameter that holds one value (no keys), and logs the calls. Until set(), the
    value is initial: the default, with normalised spatial defaults in canonical coordinates (the
    parameter isn't bound in __init__ yet)."""

    def __init__(self, desc, log):
        super().__init__()
        self.value = None
        self.log = log

    def get(self, time):
        self.log.append(("get", self.name, time))
        return self.initial if self.value is None else self.value

    def set(self, value, time=None):
        self.log.append(("set", self.name, value, time))
        self.value = value

    def changed(self, prop_name):
        self.log.append(("changed", self.name, prop_name))


class Curve(Value):
    """A Python 1D double parameter with keys, as ofxh's C++ parameters (hostDemo's MyCurve):
    linear interpolation, the derivative and integral of the piecewise linear curve, and the key
    times within 1e-4 are the same key."""

    EPS = 1e-4

    def __init__(self, desc, log):
        super().__init__(desc, log)
        self.keys = {}  # time -> value

    def _times(self):
        return sorted(self.keys)

    def _find(self, t):
        near = [k for k in self.keys if abs(k - t) <= self.EPS]
        return min(near, key=lambda k: abs(k - t)) if near else None

    def _now(self):
        return self.instance.timeline_get_time()

    def get(self, time):
        t = time  # (always a time: the current one for paramGetValue)
        times = self._times()
        if not times:
            return self.initial if self.value is None else self.value
        i = bisect.bisect_right(times, t)
        if i == 0:
            return self.keys[times[0]]
        if i == len(times):
            return self.keys[times[-1]]
        t0, t1 = times[i - 1], times[i]
        return self.keys[t0] + (t - t0) / (t1 - t0) * (self.keys[t1] - self.keys[t0])

    def set(self, value, time=None):
        self.log.append(("set", self.name, value, time))
        if time is None:
            if not self.keys:
                self.value = value
                return None
            time = self._now()
        if not self.animates:
            self.value = value
            return None
        k = self._find(time)
        self.keys[time if k is None else k] = float(value)
        return None

    def derive(self, time):
        times = self._times()
        i = bisect.bisect_right(times, time)
        if len(times) < 2 or i == 0 or i == len(times):
            return 0.
        t0, t1 = times[i - 1], times[i]
        return (self.keys[t1] - self.keys[t0]) / (t1 - t0)

    def integrate(self, t1, t2):
        sign = -1. if t2 < t1 else 1.
        a, b = min(t1, t2), max(t1, t2)
        pts = [a] + [k for k in self._times() if a < k < b] + [b]
        return sign * sum(0.5 * (self.get(x) + self.get(y)) * (y - x) for x, y in zip(pts, pts[1:]))

    def num_keys(self):
        return len(self.keys)

    def key_time(self, nth):
        if not 0 <= nth < len(self.keys):
            raise ofxh.OfxError(ofxh.Stat.ErrBadIndex)
        return self._times()[nth]

    def key_index(self, time, direction):
        times = self._times()
        if direction == 0:
            k = self._find(time)
            return -1 if k is None else times.index(k)
        if direction > 0:
            later = [i for i, k in enumerate(times) if k > time + self.EPS]
            return later[0] if later else -1
        earlier = [i for i, k in enumerate(times) if k < time - self.EPS]
        return earlier[-1] if earlier else -1

    def delete_key(self, time):
        k = self._find(time)
        if k is None:
            raise ofxh.OfxError(ofxh.Stat.ErrBadIndex)
        if len(self.keys) == 1:
            self.value = self.keys[k]
        del self.keys[k]

    def delete_all_keys(self):
        if self.keys:
            self.value = self.get(self._now())
            self.keys.clear()


def _init(plugin_dir):
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)


def _tester(plugin_dir, obj=None, action=True):
    _init(plugin_dir)
    inst = ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", obj or Recorder())
    assert inst is not None
    if action:
        assert inst.create_instance_action() == ofxh.Stat.OK
    return inst


# ------------------------------------------------------------------------------- C++ parameters

def test_host_says_choices_and_booleans_animate(plugin_dir):
    """ofxh's host descriptor says that choice and boolean parameters animate (kOfxParamHostPropSupportsChoiceAnimation,
    kOfxParamHostPropSupportsBooleanAnimation 1) and strings don't (kOfxParamHostPropSupportsStringAnimation 0): what
    its C++ parameters do (test_set_param_at_times). (#197)"""
    _init(plugin_dir)
    props = ofxh.host().props
    assert props["OfxParamHostPropSupportsChoiceAnimation"] == (1,)
    assert props["OfxParamHostPropSupportsBooleanAnimation"] == (1,)
    assert props["OfxParamHostPropSupportsStringAnimation"] == (0,)


@pytest.mark.parametrize("keys_double", ["cpp", "python"])
def test_keys_test_of_the_plugin(plugin_dir, keys_double):
    """The Tester's keysTest button checks the keys, the interpolation, the derivative and the integral of
    keysDouble through the parameter suite, with keysDouble one of ofxh's C++ parameters ("cpp") or a Python Curve
    ("python": the plug-in's paramGetValueAtTime, paramSetValueAtTime, paramGetDerivative, paramGetIntegral,
    paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex, paramDeleteKey and paramDeleteAllKeys call it); then
    param_keys() lists the keys that set_param() made (with the Curve: its num_keys() and key_time()). (#197)"""
    obj = Recorder(python=("keysDouble",), param_class=Curve) if keys_double == "python" else Recorder()
    with _tester(plugin_dir, obj) as inst:
        if keys_double == "python":
            assert isinstance(inst.param("keysDouble"), Curve)
        assert inst.press("keysTest") in OK
        assert inst.messages == [("message", "Keys test passed")]
        inst.set_param("keysDouble", 1., time=0.)
        inst.set_param("keysDouble", 3., time=10.)
        assert inst.param_keys("keysDouble") == [0., 10.]


def test_set_param_at_times(plugin_dir):
    """set_param() and get_param() at times, on ofxh's C++ parameters (as hostDemo's MyCurve): linear interpolation of
    doubles, integers interpolated then rounded, booleans and choices held until the next key, colours per component;
    without a time, a parameter with keys gets a key at the current time; a parameter that doesn't animate has no
    keys; a choice index out of range is kOfxStatErrValue; strings don't animate. (#197)"""
    with _tester(plugin_dir) as inst:
        assert inst.param_keys("keysDouble") == []
        assert inst.set_param("keysDouble", 1., time=0.) == ofxh.Stat.OK
        assert inst.set_param("keysDouble", 3., time=10.) == ofxh.Stat.OK
        assert inst.param_keys("keysDouble") == [0., 10.]
        assert inst.get_param("keysDouble", 5.) == 2.
        assert inst.get_param("keysDouble", -1.) == 1. and inst.get_param("keysDouble", 20.) == 3.
        # integers: linear, then rounded
        inst.set_param("keysInt", 0, time=0.)
        inst.set_param("keysInt", 5, time=10.)
        assert inst.get_param("keysInt", 3.) == 2
        # booleans and choices are held until the next key
        inst.set_param("keysBool", False, time=0.)
        inst.set_param("keysBool", True, time=10.)
        assert inst.get_param("keysBool", 9.) is False and inst.get_param("keysBool", 10.) is True
        inst.set_param("keysChoice", 0, time=0.)
        inst.set_param("keysChoice", 2, time=10.)
        assert inst.get_param("keysChoice", 9.) == 0 and inst.get_param("keysChoice", 10.) == 2
        # colours and 2D: per component
        inst.set_param("keysRGB", (0., 0., 0.), time=0.)
        inst.set_param("keysRGB", (1., 2., 3.), time=10.)
        assert inst.get_param("keysRGB", 5.) == (.5, 1., 1.5)
        # without a time, a parameter with keys gets the key at the current time
        inst.timeline_goto_time(4.)
        assert inst.set_param("keysDouble", 7.) == ofxh.Stat.OK
        assert inst.param_keys("keysDouble") == [0., 4., 10.]
        assert inst.get_param("keysDouble") == 7.
        # a parameter that doesn't animate has no keys: set at a time sets the value
        inst.set_param("animateDbl", 2., time=3.)
        assert inst.param_keys("animateDbl") == [] and inst.get_param("animateDbl", 100.) == 2.
        # the choice index is checked
        assert inst.set_param("keysChoice", 99, time=0.) == ofxh.Stat.ErrValue
        # string parameters don't animate
        inst.set_param("messageText", "a", time=1.)
        assert inst.get_param("messageText", 2.) == "a" and inst.param_keys("messageText") == []


def test_choices_by_name(plugin_dir):
    """set_param() takes a choice's index or the label of one of its options (kOfxParamPropChoiceOption),
    at a time too; another string is a ValueError that lists the options. The same for a Python
    parameter: its set() gets the index. (#211)"""
    with _tester(plugin_dir) as inst:
        assert inst.set_param("keysChoice", "c") == ofxh.Stat.OK and inst.get_param("keysChoice") == 2
        assert inst.set_param("keysChoice", "b", time=5.) == ofxh.Stat.OK
        assert inst.param_curve("keysChoice") == [(5., 1)]
        assert inst.set_param("enableTest", "Disable parameter") == ofxh.Stat.OK
        assert inst.get_param("enableTest") == 1
        with pytest.raises(ValueError, match='no option "d" .*"a", "b", "c"'):
            inst.set_param("keysChoice", "d")
        assert inst.get_param("keysChoice", 5.) == 1
    ofxh.shutdown()
    obj = Recorder(python=("keysChoice",), param_class=Value)
    with _tester(plugin_dir, obj) as inst:
        assert inst.set_param("keysChoice", "b") == ofxh.Stat.OK
        assert inst.param("keysChoice").value == 1 and inst.get_param("keysChoice") == 1


def test_param_curve(plugin_dir):
    """param_curve(): the keys with their values, in time order, as get_param() gives them; setting
    them back on another instance gives the same values between the keys. (#211)"""
    with _tester(plugin_dir) as inst:
        assert inst.param_curve("keysDouble") == [] and inst.param_curve("messageText") == []
        inst.set_param("keysDouble", 3., time=10.)
        inst.set_param("keysDouble", 1., time=0.)
        inst.set_param("keysRGB", (1., 2., 3.), time=2.)
        inst.set_param("keysBool", True, time=1.)
        assert inst.param_curve("keysDouble") == [(0., 1.), (10., 3.)]
        assert inst.param_curve("keysRGB") == [(2., (1., 2., 3.))]
        assert inst.param_curve("keysBool") == [(1., True)]
        curve = inst.param_curve("keysDouble")
        with pytest.raises(KeyError):
            inst.param_curve("nope")
    ofxh.shutdown()
    with _tester(plugin_dir) as inst:
        for t, v in curve:
            inst.set_param("keysDouble", v, time=t)
        assert inst.get_param("keysDouble", 2.5) == 1.5 and inst.param_curve("keysDouble") == curve


def test_project_load_sends_no_instance_changed(plugin_dir):
    """Values set before kOfxActionCreateInstance (a project load) send no instanceChanged; the
    plug-in reads them in its create instance action (ofxCore.h, the create instance
    pre-conditions). (#197)"""
    with _tester(plugin_dir, action=False) as inst:
        assert inst.set_param("messageText", "loaded") == ofxh.Stat.OK
        assert inst.set_param("keysDouble", 2., time=1.) == ofxh.Stat.OK
        assert inst.create_instance_action() == ofxh.Stat.OK
        # no instanceChanged, counted host-side, and none seen by the plug-in (Tester sends
        # messageText as a message from instanceChanged)
        assert inst.param_changed_count == 0 and inst.messages == []
        assert inst.get_param("messageText") == "loaded" and inst.param_keys("keysDouble") == [1.]
        # the same checks see a user edit after the create instance action, which the host sends
        assert inst.set_param("messageText", "edited") == ofxh.Stat.OK
        inst.begin_instance_changed_action()
        assert inst.param_changed_action("messageText") in OK
        inst.end_instance_changed_action()
        assert inst.param_changed_count == 1
        assert inst.messages[0] == ("message", "edited")


def test_press_and_messages(plugin_dir):
    """press() sends a push button's instanceChanged (Tester's pbButton sends a message, which reaches message());
    a parameter that isn't a push button is a ValueError, an unknown one a KeyError. A message text with % (Tester
    passes messageText as the "%s" argument of the message suite) is formatted in C++, and arrives verbatim. (#194,
    #197)"""
    with _tester(plugin_dir) as inst:
        assert inst.press("pbButton") in OK
        assert inst.messages == [("message", "Push Button Pressed - TestPassed!")]
        with pytest.raises(ValueError, match="not a push button"):
            inst.press("messageText")
        with pytest.raises(KeyError):
            inst.press("noSuchParam")
        assert inst.set_param("messageText", "100% %d %s") == ofxh.Stat.OK
        inst.begin_instance_changed_action()
        inst.param_changed_action("messageText")
        inst.end_instance_changed_action()
        assert inst.messages[-1] == ("warning", "100% %d %s")  # (the persistent message)


def test_param_values_and_descriptor(plugin_dir):
    """param_values() has every parameter's value but the push buttons'; a C++ parameter has no Python object (param()
    is None); param_descriptor(), param_type() and param_initial() (a normalised spatial default in canonical
    coordinates for the project, as get_param() gives it). (#197)"""
    with _tester(plugin_dir) as inst:
        values = inst.param_values()
        assert "pbButton" not in values and values["keysRGB"] == [0., 0., 0.]
        assert inst.param("keysDouble") is None  # a C++ parameter
        d = inst.param_descriptor("enableTest")
        assert d.type == "choice" and len(d.choices) >= 2 and d.default == 0
        assert inst.param_type("keys2D") == "double2d"
        assert inst.param_initial("widgetPos") == (384., 288.) == inst.get_param("widgetPos")


def test_param_props(plugin_dir):
    """param_props() has the instance's properties (changed in instanceChanged), param_descriptor()
    the described ones. (#207)"""
    ofxh.init(ofxh.Host(), [plugin_dir], use_env_path=False)
    inst = ofxh.create_instance(ofxh.get_plugin(BASIC), "filter", ofxh.ImageEffectInstance())
    with inst:
        assert inst.create_instance_action() in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
        assert inst.param_props("scaleR")[k.kOfxParamPropEnabled] == (1,)
        inst.set_param("scaleComponents", False)
        inst.begin_instance_changed_action()
        inst.param_changed_action("scaleComponents", time=0.)
        inst.end_instance_changed_action()
        props = inst.param_props("scaleR")
        assert props[k.kOfxParamPropEnabled] == (0,) and props[k.kOfxParamPropType] == (k.kOfxParamTypeDouble,)
        assert inst.param_descriptor("scaleR").enabled
        with pytest.raises(KeyError):
            inst.param_props("nope")


def test_strchoice_values_are_checked(plugin_dir):
    """A StrChoice parameter refuses a value that isn't one of its enums (kOfxStatErrValue) and keeps its value. (#197;
    hostDemo accepts it: HostSupport/examples/python/tests test_backends.py::test_strchoice_value_outside_the_enums
    pins that difference)"""
    _init(plugin_dir)
    plugin = ofxh.get_plugin(CHOICES)
    desc = [p for p in plugin.descriptor("filter").params if p.type == "strchoice"][0]
    with ofxh.create_instance(plugin, "filter", ofxh.ImageEffectInstance()) as inst:
        assert inst.set_param(desc.name, desc.choice_enums[-1]) == ofxh.Stat.OK
        assert inst.get_param(desc.name) == desc.choice_enums[-1]
        assert inst.set_param(desc.name, "no such enum") == ofxh.Stat.ErrValue
        assert inst.get_param(desc.name) == desc.choice_enums[-1]


# ---------------------------------------------------------------------------- Python parameters

def test_python_params(plugin_dir):
    """new_param() makes a Python parameter of every descriptor: its type, instance and defaults (a normalised spatial
    default is as the plug-in set it, initial is in canonical coordinates, as the C++ parameters); get_param() and
    set_param() call its get() and set() (any sequence for a colour); the plug-in's paramGetValue from
    instanceChanged calls get() at the current time; a Python push button works. (#197)"""
    obj = Recorder(python="*", param_class=Value)
    with _tester(plugin_dir, obj) as inst:
        # new_param() got every descriptor, and made every parameter
        names = inst.param_names
        assert [d.name for d in obj.descriptors] == names
        for name in names:
            p = inst.param(name)
            assert isinstance(p, Value) and p.alive and p.name == name
            assert p.type == inst.param_type(name) and p.instance is inst
        assert inst.param("keysRGB").default == (0., 0., 0.) and inst.param("keysBool").default is False
        # a normalised spatial default (Tester's widgetPos, at the middle): default is as the plug-in
        # set it, initial in canonical coordinates for the project, as the C++ parameters
        w = inst.param("widgetPos")
        assert w.default == (.5, .5) and w.initial == (384., 288.) == inst.param_initial("widgetPos")
        assert inst.get_param("widgetPos") == (384., 288.)
        # get_param() and set_param() call get() and set()
        obj.log.clear()
        assert inst.set_param("keysDouble", 4., time=2.) == ofxh.Stat.OK
        assert inst.get_param("keysDouble", 3.) == 4.
        assert obj.log == [("set", "keysDouble", 4., 2.), ("get", "keysDouble", 3.)]
        assert inst.set_param("keysRGB", [1, 2, 3]) == ofxh.Stat.OK  # (any sequence)
        assert inst.param("keysRGB").value == (1., 2., 3.) and inst.get_param("keysRGB") == (1., 2., 3.)
        # the plug-in's paramGetValue and the messages: messageText (sent by instanceChanged)
        inst.set_param("messageText", "from Python")
        obj.log.clear()
        inst.begin_instance_changed_action()
        assert inst.param_changed_action("messageText") in OK
        inst.end_instance_changed_action()
        assert ("get", "messageText", 0.) in obj.log  # (paramGetValue: the current time)
        assert inst.messages[0] == ("message", "from Python")
        # a push button that is a Python parameter
        assert inst.press("pbButton") in OK
        assert inst.messages[-1] == ("message", "Push Button Pressed - TestPassed!")


def test_python_param_set_before_create_instance(plugin_dir):
    """A Python parameter set before kOfxActionCreateInstance (a project load) keeps the value and sends no
    instanceChanged (no message from Tester): the Python twin of test_project_load_sends_no_instance_changed. (#197)"""
    obj = Recorder(python=("messageText",), param_class=Value)
    with _tester(plugin_dir, obj, action=False) as inst:
        assert inst.set_param("messageText", "loaded") == ofxh.Stat.OK
        assert inst.create_instance_action() == ofxh.Stat.OK
        assert inst.messages == []
        assert inst.param("messageText").value == "loaded"


def test_python_param_changed(plugin_dir):
    """changed(prop_name): the plug-in's setEnabled() on enableDbl (Tester's enableTest). (#197)"""
    obj = Recorder(python=("enableDbl",), param_class=Value)
    with _tester(plugin_dir, obj) as inst:
        p = inst.param("enableDbl")
        assert p.enabled
        inst.set_param("enableTest", 1)
        inst.begin_instance_changed_action()
        inst.param_changed_action("enableTest")
        inst.end_instance_changed_action()
        assert ("changed", "enableDbl", "OfxParamPropEnabled") in obj.log
        assert not p.enabled and p.props["OfxParamPropEnabled"] == (0,)


def test_python_param_defaults(plugin_dir):
    """The defaults of ofxh.Param: no keys; derive and integrate give HostSupport's status. (#197)"""
    obj = Recorder(python=("keysDouble", "keysInt"), param_class=Value)
    with _tester(plugin_dir, obj) as inst:
        p = inst.param("keysDouble")
        assert p.num_keys() == 0 and p.key_index(0., 0) == -1 and p.delete_all_keys() is None
        with pytest.raises(ofxh.OfxError) as e:
            p.derive(0.)
        assert e.value.status == ofxh.Stat.ErrMissingHostFeature
        with pytest.raises(ofxh.OfxError) as e:
            inst.param("keysInt").integrate(0., 1.)
        assert e.value.status == ofxh.Stat.ErrUnsupported
        assert inst.param_keys("keysDouble") == []
        with pytest.raises(NotImplementedError):
            ofxh.Param.get(p, 0.)


def test_python_param_errors(plugin_dir):
    """A Python parameter's get() that raises: get_param() raises CallbackError with the cause; the plug-in's
    paramGetValue gets kOfxStatFailed (Support throws), and its instanceChanged action raises CallbackError with the
    message. (#197)"""
    class Bad(Value):
        def get(self, time):
            raise ValueError("bad get")

    obj = Recorder(python=("messageText",), param_class=Bad)
    with _tester(plugin_dir, obj) as inst:
        with pytest.raises(ofxh.CallbackError) as e:
            inst.get_param("messageText")
        assert isinstance(e.value.__cause__, ValueError)
        # the plug-in gets kOfxStatFailed (Support throws), and the action raises
        inst.begin_instance_changed_action()
        with pytest.raises(ofxh.CallbackError) as e:
            inst.param_changed_action("messageText")
        inst.end_instance_changed_action()
        assert "bad get" in str(e.value)


def test_new_param_must_return_a_param(plugin_dir):
    """new_param() must return an ofxh.Param or None: another object is a TypeError (the create_instance CallbackError's
    cause), and the same Param for two parameters is refused ("already bound"). (#197)"""
    class Wrong(Recorder):
        def new_param(self, desc):
            return 42

    _init(plugin_dir)
    with pytest.raises(ofxh.CallbackError) as e:
        ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", Wrong())
    assert isinstance(e.value.__cause__, TypeError)

    shared = []

    class Twice(Recorder):
        def new_param(self, desc):
            if not shared:
                shared.append(Value(desc, []))
            return shared[0]  # the same object for two parameters

    with pytest.raises(ofxh.CallbackError) as e:
        ofxh.create_instance(ofxh.get_plugin(TESTER), "filter", Twice())
    assert "already bound" in str(e.value)


def test_python_param_lifetime(plugin_dir):
    """A Python parameter's object (and its props) raise ReferenceError after the instance's close(): HostSupport
    deleted the parameter before the Python object goes (its __del__ sees it dead), as for clips
    (test_lifetime.py::test_deleted_before_python_objects_drop). (#197)"""
    seen = []

    class Watched(Value):
        def __del__(self):
            seen.append(self.alive)

    obj = Recorder(python=("keysDouble",), param_class=Watched)
    inst = _tester(plugin_dir, obj)
    p = inst.param("keysDouble")
    props = p.props
    assert p.alive and props["OfxParamPropType"] == ("OfxParamTypeDouble",)
    inst.close()
    assert not p.alive
    with pytest.raises(ReferenceError):
        p.name
    with pytest.raises(ReferenceError):
        props["OfxParamPropType"]
    del p
    gc.collect()
    assert seen == [False]  # (deleted by HostSupport before the Python object goes)


def test_python_param_wrong_type(plugin_dir):
    """A get() that returns a value of the wrong type: TypeError naming the parameter and get(). (#197)"""
    import numpy as np

    class Wrong(Value):
        returns = {}

        def get(self, time):
            return self.returns[self.name]

    obj = Recorder(python=("keysDouble", "keysRGB"), param_class=Wrong)
    with _tester(plugin_dir, obj) as inst:
        for name, value in (("keysDouble", "not a number"), ("keysRGB", (1., "x", 2.)), ("keysRGB", "abc")):
            Wrong.returns[name] = value
            with pytest.raises(ofxh.CallbackError) as e:
                inst.get_param(name)
            cause = e.value.__cause__
            assert isinstance(cause, TypeError) and name in str(cause) and "get()" in str(cause), value
        # any sequence but str and bytes: a NumPy array, a list
        for value in (np.array([1., 2., 3.]), [1., 2., 3.]):
            Wrong.returns["keysRGB"] = value
            assert inst.get_param("keysRGB") == (1., 2., 3.)
