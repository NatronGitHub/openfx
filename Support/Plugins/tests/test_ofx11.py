"""OFX 1.1 hosts such as VEGAS Pro: the Support library works without the OFX 1.2+ properties.

Pattern: the same render with and without hostDemo --ofx11 (extra_args=OFX11), the OFX 1.1 host
profile ("An OFX 1.1 Host" in Documentation/sources/Guide/ofxDemoHosts.rst). Tester uses what
such a host lacks (Tester.cpp says where), and none of it may fail. --ofx11 is a hostDemo test
hook, which the in-process backend (ofxh) doesn't have: these tests run on hostDemo only.
"""

import numpy as np
import pytest

from ofxhost.testing import assert_close, selected_backends

OFX11 = ["--ofx11"]
TESTER = "net.sf.openfx.GenericTestPlugin"
INVERT = "net.sf.openfx.invertPlugin"
TWICE = ["-s", "messageText=hi", "-s", "messageText=hi"]
SRC = np.random.default_rng(11).random((16, 24, 4), dtype=np.float32)
# hostDemo --ofx11 only, the backends marker says why; the in-process backend can never run these
# tests, so it isn't collected (the indirect parametrize of the backend fixture)
pytestmark = [
    pytest.mark.backends("subprocess", reason="hostDemo --ofx11: ofxh has no OFX 1.1 host profile"),
    pytest.mark.parametrize("backend", [b for b in selected_backends() if b == "subprocess"], indirect=True),
]


def test_tester_in_ofx11_host(render):
    default = render(TESTER, inputs={"Source": SRC}, depth="float")
    ofx11 = render(TESTER, inputs={"Source": SRC}, depth="float", extra_args=OFX11)
    assert default.report["host"]["profile"] == "default" and ofx11.report["host"]["profile"] == "ofx11"
    # the host kept its single PAR, and the render is the same (Tester's render is an invert)
    for r in (default, ofx11):
        assert r.report["clipPreferences"]["clips"]["Output"]["pixelAspectRatio"] == \
            r.report["clipPreferences"]["clips"]["Source"]["pixelAspectRatio"]
    assert_close(ofx11.output, default.output, atol=0)


def test_invert_same_in_both_hosts(render):
    default = render(INVERT, inputs={"Source": SRC}, depth="byte")
    ofx11 = render(INVERT, inputs={"Source": SRC}, depth="byte", extra_args=OFX11)
    assert ofx11.report["host"]["profile"] == "ofx11"
    assert_close(ofx11.output, default.output, atol=0)


def test_tester_refused_persistent_messages(render):
    """The profile refuses persistent messages, as DaVinci Resolve and Fusion 2026 do."""
    r = render(TESTER, inputs={"Source": SRC}, depth="float", frames=[0], extra_args=OFX11 + TWICE)
    kinds = [m["kind"] for m in r.report["messages"]]
    assert kinds.count("message") == 2 and "persistentMessage" not in kinds

