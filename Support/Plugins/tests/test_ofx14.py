"""OFX 1.4 hosts: the plug-in still works without the OFX 1.5 features, with its fallbacks.

Pattern: hostDemo --ofx14 (extra_args=["--ofx14"]) is a host that implements OFX 1.4 only. It
has only the OFX 1.4 host properties (reading kOfxParamHostPropSupportsStrChoice, for example,
gives kOfxStatErrUnknown) and suites, doesn't call OfxSetHost, and refuses StrChoice and Bytes
params. Every plug-in must work in such hosts: describe and render it there, and check its
fallbacks. ChoiceParams doesn't define its StrChoice param (blue_choice), and its blue scale is
then 1; Invert has no OFX 1.5 feature, and renders the same in both hosts.
"""

import numpy as np

from ofxhost.testing import assert_close

OFX14 = ["--ofx14"]
FILTER = "OfxImageEffectContextFilter"
SRC = np.random.default_rng(3).random((16, 24, 4), dtype=np.float32)


def test_choiceparams_without_strchoice(describe, render):
    plugin = "org.openeffects.support.choiceParamsPlugin"
    params = {p["name"]: p for p in describe(plugin, extra_args=OFX14)["contexts"][FILTER]["params"]}
    assert "blue_choice" not in params and params["red_choice"]["type"] == "OfxParamTypeChoice"
    r = render(plugin, inputs={"Source": SRC}, depth="float", params={"red_choice": 2, "green_choice": 2},
               extra_args=OFX14)
    assert r.report["host"]["profile"] == "ofx14"
    assert_close(r.output, SRC * np.array([1.0, 0.5, 1.0, 1.0], np.float32), atol=1e-6)


def test_invert_same_in_both_hosts(render):
    default = render("net.sf.openfx.invertPlugin", inputs={"Source": SRC}, depth="float")
    ofx14 = render("net.sf.openfx.invertPlugin", inputs={"Source": SRC}, depth="float", extra_args=OFX14)
    assert default.report["host"]["profile"] == "default" and ofx14.report["host"]["profile"] == "ofx14"
    assert_close(ofx14.output, default.output, atol=0)
