"""OFX 1.1 hosts such as VEGAS Pro: the Support library works without the OFX 1.2+ properties.

Pattern: hostDemo --ofx11 (extra_args=["--ofx11"]) is an OFX 1.1 host modelled on VEGAS Pro
(openfx-misc#66): only the OFX 1.1 host properties (API version 1.1) and suites, no tiles, no
multi-resolution, no overlays, byte and float depths, a single clip PAR
(kOfxImageEffectPropSupportsMultipleClipPARs 0), no kOfxParamPropDefaultCoordinateSystem, and no
kOfxImageEffectInstancePropSequentialRender on the plug-in descriptors and instances. Every
plug-in must work in such hosts. Tester sets the output PAR in its clip preferences (which such
a host doesn't let it do), writes kOfxImageEffectInstancePropSequentialRender when its instance
is created, and reads the default coordinate system of a double param: none of these may fail.
Its describe also orders its page, a property that hostDemo, as Flame 2027, doesn't have. The
OFX 1.1 host refuses the persistent messages, as DaVinci Resolve and Fusion 2026 do. --ofx11 is a
hostDemo test hook, which the in-process backend (ofxh) doesn't have: these tests run on hostDemo
only.
"""

import numpy as np
import pytest

from ofxhost.testing import assert_close, selected_backends

OFX11 = ["--ofx11"]
SRC = np.random.default_rng(11).random((16, 24, 4), dtype=np.float32)
# hostDemo --ofx11 only, the backends marker says why; the in-process backend can never run these
# tests, so it isn't collected (the indirect parametrize of the backend fixture)
pytestmark = [
    pytest.mark.backends("subprocess", reason="hostDemo --ofx11: ofxh has no OFX 1.1 host profile"),
    pytest.mark.parametrize("backend", [b for b in selected_backends() if b == "subprocess"], indirect=True),
]


def test_tester_in_ofx11_host(render):
    plugin = "net.sf.openfx.GenericTestPlugin"
    default = render(plugin, inputs={"Source": SRC}, depth="float")
    ofx11 = render(plugin, inputs={"Source": SRC}, depth="float", extra_args=OFX11)
    assert default.report["host"]["profile"] == "default" and ofx11.report["host"]["profile"] == "ofx11"
    # the host kept its single PAR, and the render is the same (Tester's render is an invert)
    for r in (default, ofx11):
        assert r.report["clipPreferences"]["clips"]["Output"]["pixelAspectRatio"] == \
            r.report["clipPreferences"]["clips"]["Source"]["pixelAspectRatio"]
    assert_close(ofx11.output, default.output, atol=0)


def test_invert_same_in_both_hosts(render):
    default = render("net.sf.openfx.invertPlugin", inputs={"Source": SRC}, depth="byte")
    ofx11 = render("net.sf.openfx.invertPlugin", inputs={"Source": SRC}, depth="byte", extra_args=OFX11)
    assert ofx11.report["host"]["profile"] == "ofx11"
    assert_close(ofx11.output, default.output, atol=0)


TESTER = "net.sf.openfx.GenericTestPlugin"
TWICE = ["-s", "messageText=hi", "-s", "messageText=hi"]


def test_tester_refused_persistent_messages(render):
    r = render(TESTER, inputs={"Source": SRC}, depth="float", frames=[0], extra_args=OFX11 + TWICE)
    kinds = [m["kind"] for m in r.report["messages"]]
    assert kinds.count("message") == 2 and "persistentMessage" not in kinds

