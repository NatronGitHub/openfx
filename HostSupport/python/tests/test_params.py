"""Parameters: ofxh's C++ parameters (stored values, keys and linear interpolation, as hostDemo's
MyCurve), the Python parameters (ofxh.Param, made by new_param()), project loads and push buttons.

The plug-in is the Support Tester (GenericTest): its keysTest button checks the keys, the
interpolation, the derivative and the integral through the parameter suite, and messageText and
pbButton send messages from instanceChanged."""

import bisect
import gc

import pytest

ofxh = pytest.importorskip("ofxh")

TESTER = "net.sf.openfx.GenericTestPlugin"
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

    def message(self, type, id, text):
        self.messages.append((type, text))

    def new_param(self, desc):
        self.descriptors.append(desc)
        if self.python == "*" or desc.name in self.python:
            return self.param_class(desc, self.log)
        return None


class Value(ofxh.Param):
    """A Python parameter that holds one value (no keys), and logs the calls."""

    def __init__(self, desc, log):
        super().__init__()
        self.value = desc.default
        self.log = log

    def get(self, time=None):
        self.log.append(("get", self.name, time))
        return self.value

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

    def get(self, time=None):
        t = self._now() if time is None else time
        times = self._times()
        if not times:
            return self.value
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
            self.value = self.get()
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
    _init(plugin_dir)
    props = ofxh.host().props
    assert props["OfxParamHostPropSupportsChoiceAnimation"] == (1,)
    assert props["OfxParamHostPropSupportsBooleanAnimation"] == (1,)
    assert props["OfxParamHostPropSupportsStringAnimation"] == (0,)


def test_keys_test_of_the_plugin(plugin_dir):
    """The Tester's keys test (keys, interpolation, derivative, integral, through the suite)."""
    with _tester(plugin_dir) as inst:
        assert inst.press("keysTest") in OK
        assert inst.messages == [("message", "Keys test passed")]


def test_set_param_at_times(plugin_dir):
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


def test_project_load_sends_no_instance_changed(plugin_dir):
    """Values set before kOfxActionCreateInstance (a project load) send no instanceChanged; the
    plug-in reads them in its create instance action (ofxCore.h, the create instance
    pre-conditions)."""
    with _tester(plugin_dir, action=False) as inst:
        assert inst.set_param("messageText", "loaded") == ofxh.Stat.OK
        assert inst.set_param("keysDouble", 2., time=1.) == ofxh.Stat.OK
        assert inst.create_instance_action() == ofxh.Stat.OK
        assert inst.messages == []  # no instanceChanged: Tester sends messageText from it
        assert inst.get_param("messageText") == "loaded" and inst.param_keys("keysDouble") == [1.]
        # a user edit after the create instance action: the host sends it
        inst.begin_instance_changed_action()
        assert inst.param_changed_action("messageText") in OK
        inst.end_instance_changed_action()
        assert inst.messages[0] == ("message", "loaded")


def test_press(plugin_dir):
    with _tester(plugin_dir) as inst:
        assert inst.press("pbButton") in OK
        assert inst.messages == [("message", "Push Button Pressed - TestPassed!")]
        with pytest.raises(ValueError, match="not a push button"):
            inst.press("messageText")
        with pytest.raises(KeyError):
            inst.press("noSuchParam")


def test_param_values_and_descriptor(plugin_dir):
    with _tester(plugin_dir) as inst:
        values = inst.param_values()
        assert "pbButton" not in values and values["keysRGB"] == [0., 0., 0.]
        assert inst.param("keysDouble") is None  # a C++ parameter
        d = inst.param_descriptor("enableTest")
        assert d.type == "choice" and len(d.choices) >= 2 and d.default == 0
        assert inst.param_type("keys2D") == "double2d"


# ---------------------------------------------------------------------------- Python parameters

def test_python_params(plugin_dir):
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
        assert ("get", "messageText", None) in obj.log
        assert inst.messages[0] == ("message", "from Python")
        # a push button that is a Python parameter
        assert inst.press("pbButton") in OK
        assert inst.messages[-1] == ("message", "Push Button Pressed - TestPassed!")


def test_python_param_set_before_create_instance(plugin_dir):
    obj = Recorder(python=("messageText",), param_class=Value)
    with _tester(plugin_dir, obj, action=False) as inst:
        assert inst.set_param("messageText", "loaded") == ofxh.Stat.OK
        assert inst.create_instance_action() == ofxh.Stat.OK
        assert inst.messages == []
        assert inst.param("messageText").value == "loaded"


def test_python_param_changed(plugin_dir):
    """changed(prop_name): the plug-in's setEnabled() on enableDbl (Tester's enableTest)."""
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


def test_python_keys(plugin_dir):
    """The Tester's keys test with keysDouble a Python parameter: the plug-in's
    paramGetValueAtTime, paramSetValueAtTime, paramGetDerivative, paramGetIntegral, paramGetNumKeys,
    paramGetKeyTime, paramGetKeyIndex, paramDeleteKey and paramDeleteAllKeys call it."""
    obj = Recorder(python=("keysDouble",), param_class=Curve)
    with _tester(plugin_dir, obj) as inst:
        assert isinstance(inst.param("keysDouble"), Curve)
        assert inst.press("keysTest") in OK
        assert inst.messages == [("message", "Keys test passed")]
        inst.set_param("keysDouble", 1., time=0.)
        inst.set_param("keysDouble", 3., time=10.)
        assert inst.param_keys("keysDouble") == [0., 10.]  # (getNumKeys and getKeyTime: Python)


def test_python_param_defaults(plugin_dir):
    """The defaults of ofxh.Param: no keys; derive and integrate give HostSupport's status."""
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
            ofxh.Param.get(p)


def test_python_param_errors(plugin_dir):
    class Bad(Value):
        def get(self, time=None):
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
