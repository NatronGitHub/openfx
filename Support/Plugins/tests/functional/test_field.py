"""Field: what a plug-in does on unfielded material, and its stdout.

hostDemo's clips are progressive (unfielded), and it renders whole frames (field "none"),
so the field-specific branches of the sample (red set to 1 on the lower field, blue on the
upper one) can't run: it simply inverts all components. This checks that, and that
plug-in output on stdout is available (r.stdout).
"""

import numpy as np

from ofxhost.testing import assert_close

PLUGIN = "net.sf.openfx.fieldPlugin"


def test_unfielded(render):
    """On unfielded frames (hostDemo's), Field inverts all components and says it renders (r.stdout has the plug-in's
    output); its output fielding is none. (#120)"""
    src = np.random.default_rng(4).random((16, 24, 3), dtype=np.float32)
    r = render(PLUGIN, inputs={"Source": src})
    assert r.report["clipPreferences"]["outputFielding"] == "OfxFieldNone"
    assert_close(r.output[..., :3], 1 - src, atol=1e-6)
    assert_close(r.output[..., 3], 0.0)
    assert "Rendering at time 0" in r.stdout  # the sample prints it in render
