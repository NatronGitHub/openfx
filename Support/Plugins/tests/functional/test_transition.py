"""Cross Fade: a transition, with two inputs and the transition value.

Pattern: two inputs (SourceFrom, SourceTo) and the Transition parameter: 0 gives the first
input, 1 the second, in between a linear mix. hostDemo has no transition context, so the
plug-in runs in the general context, where it has the same clips and parameter.
"""

import numpy as np
import pytest

from ofxhost.testing import assert_close

PLUGIN = "net.sf.openfx.crossFade"

FROM = np.random.default_rng(2).random((16, 24, 3), dtype=np.float32)
TO = np.random.default_rng(3).random((16, 24, 3), dtype=np.float32)


@pytest.mark.parametrize("value, identity", [(0.0, "SourceFrom"), (0.5, None), (1.0, "SourceTo")],
                         ids=["from", "mix", "to"])
def test_cross_fade(render, value, identity):
    """The output is (1 - Transition) * SourceFrom + Transition * SourceTo; at the ends (0 and 1) the plug-in is an
    identity of the input it gives (isIdentity). (#120)"""
    r = render(PLUGIN, context="general", inputs={"SourceFrom": FROM, "SourceTo": TO},
               params={"Transition": value})
    assert_close(r.output[..., :3], (1 - value) * FROM + value * TO, atol=1e-6)
    if identity is not None:
        result = r.report["renders"][0]["isIdentity"]
        assert result["identity"] and result["clip"] == identity
