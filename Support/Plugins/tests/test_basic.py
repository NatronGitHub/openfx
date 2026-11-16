"""Basic (Gain): parameters, an optional mask clip, and instanceChanged.

Patterns:
- parameters change the output (params={...}), and the default parameters make the effect
  an identity (isIdentity in the report);
- a mask clip in the general context: an image (here a checkerboard), or disconnected
  ("none", the mask is optional);
- an instanceChanged effect: unchecking scaleComponents disables the per-component scales,
  which the report shows in params[name]["enabled"].
"""

import numpy as np

from ofxhost.testing import assert_close

PLUGIN = "net.sf.openfx.basicPlugin"

SRC = np.random.default_rng(0).random((16, 24, 3), dtype=np.float32)


def test_params_change_output(render):
    # r is scaled by scale * scaleR, g and b by scale, alpha by scaleA
    r = render(PLUGIN, inputs={"Source": SRC}, params={"scale": 0.5, "scaleR": 2.0})
    out = r.output
    assert_close(out[..., 0], SRC[..., 0], atol=1e-6)
    assert_close(out[..., 1:3], 0.5 * SRC[..., 1:3], atol=1e-6)
    assert_close(out[..., 3], 1.0)


def test_default_is_identity(render):
    r = render(PLUGIN, inputs={"Source": SRC})
    identity = r.report["renders"][0]["isIdentity"]
    assert identity["identity"] and identity["clip"] == "Source"
    # hostDemo renders identity effects anyway: the output is the input
    assert_close(r.output[..., :3], SRC)


def test_checker_mask(render):
    # the effect (here scale 0: black) applies where the mask is 1, not where it is 0.
    # A 2D array is a grey PFM: hostDemo gives it to the Alpha mask clip.
    y, x = np.mgrid[0:16, 0:24]
    mask = ((x // 4 + y // 4) % 2).astype(np.float32)
    r = render(PLUGIN, context="general", inputs={"Source": SRC, "Mask": mask},
               params={"scale": 0.0})
    assert r.report["inputs"]["Mask"]["isMask"]
    assert_close(r.output[..., :3], SRC * (1 - mask[..., None]), atol=1e-6)


def test_disconnected_mask(render):
    # without a mask, the effect applies everywhere
    r = render(PLUGIN, context="general", inputs={"Source": SRC, "Mask": "none"},
               params={"scale": 0.25})
    assert not r.report["clipPreferences"]["clips"]["Mask"]["connected"]
    assert_close(r.output[..., :3], 0.25 * SRC, atol=1e-6)


def test_instance_changed_disables_params(render):
    # changedParam("scaleComponents") calls setEnabled on scaleR..scaleA, and render then
    # ignores scaleR
    r = render(PLUGIN, inputs={"Source": SRC},
               params={"scaleComponents": False, "scaleR": 2.0})
    params = r.report["params"]
    assert not any(params[p]["enabled"] for p in ("scaleR", "scaleG", "scaleB", "scaleA"))
    assert params["scale"]["enabled"]
    assert ("instanceChanged", "scaleComponents") in [
        (a["action"], a.get("param")) for a in r.report["actions"]]
    assert_close(r.output[..., :3], SRC)
