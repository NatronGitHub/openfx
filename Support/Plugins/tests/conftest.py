"""pytest fixtures for the plug-in tests: where hostDemo, ofxhost and the plug-in bundles are.

This is the file to adapt when copying the tests to a plug-in repo: set OPENFX (the openfx
checkout, usually a submodule) and BUNDLE_DIRS (the directories holding your .ofx.bundle
directories). Tests that need hostDemo or the bundles are skipped if they aren't built, or fail
with OFXH_REQUIRE=1 (CI).

The render and describe fixtures run each test on both ofxhost backends (ofxhost.testing's backend
fixture): hostDemo in a subprocess, and in-process on the HostSupport Python bindings (the ofxh
module: PYTHONPATH=<openfx build>/HostSupport/python). A backend that isn't built is skipped, or
fails with OFXH_REQUIRE=1 ($OFXHOST_TEST_BACKENDS=subprocess runs only hostDemo). render and
describe are function-scoped fixtures (the backend fixture is): a fixture that uses them must be
function-scoped too.
"""

import functools
import os
import sys
from pathlib import Path

import pytest

# --- adapt these to your repo ----------------------------------------------------------------
REPO = Path(__file__).resolve().parents[3]  # the repo root (these tests are in Support/Plugins/tests)
OPENFX = REPO  # the openfx checkout; in a plug-in repo: REPO / "openfx"
# the directories that contain the .ofx.bundle directories: $OFX_TEST_PLUGIN_PATH, else the Make
# build (make -C Support/Plugins CONFIG=release), else a CMake build tree (build*/Plugins)
BUNDLE_DIRS = ([Path(d) for d in os.environ["OFX_TEST_PLUGIN_PATH"].split(os.pathsep) if d]
               if os.environ.get("OFX_TEST_PLUGIN_PATH") else
               sorted(REPO.glob("Support/Plugins/*/*-release")) or sorted(REPO.glob("build*/Plugins")))
# ---------------------------------------------------------------------------------------------

# ofxhost is a plain directory in openfx, not an installed package
sys.path.insert(0, str(OPENFX / "HostSupport" / "examples" / "python"))

import ofxhost  # noqa: E402
from ofxhost.testing import backend, pytest_configure  # noqa: E402,F401  (each backend in turn; the marker)


@pytest.fixture(scope="session")
def bundles():
    """The plug-in directories (OFX_PLUGIN_PATH); skips the test if no bundle is built (fails with OFXH_REQUIRE=1)."""
    dirs = [d for d in BUNDLE_DIRS if any(d.glob("*.ofx.bundle"))]
    if not dirs:
        why = (f"no .ofx.bundle in {REPO}/Support/Plugins/*/*-release or {REPO}/build*/Plugins: "
               "build them with make -C Support/Plugins CONFIG=release, or CMake, or set OFX_TEST_PLUGIN_PATH")
        if os.environ.get("OFXH_REQUIRE") == "1":  # (CI: a missing build fails the tests)
            pytest.fail(why + " (OFXH_REQUIRE=1)")
        pytest.skip(why)
    return dirs


@pytest.fixture
def render(backend, bundles):  # noqa: F811  (the backend fixture imported above)
    """ofxhost.render with our bundles, on each backend: render(plugin_id, inputs=..., params=...)."""
    return functools.partial(ofxhost.render, bundles, backend=backend)


@pytest.fixture
def describe(backend, bundles):  # noqa: F811  (the backend fixture imported above)
    """ofxhost.describe with our bundles, on each backend: describe(plugin_id)."""
    return functools.partial(ofxhost.describe, bundles, backend=backend)
