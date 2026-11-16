"""pytest fixtures for the plug-in tests: where hostDemo, ofxhost and the plug-in bundles are.

This is the file to adapt when copying the tests to a plug-in repo: set OPENFX (the openfx
checkout, usually a submodule) and BUNDLE_DIRS (the directories holding your .ofx.bundle
directories). Tests that need hostDemo or the bundles are skipped if they aren't built.
"""

import functools
import sys
from pathlib import Path

import pytest

# --- adapt these to your repo ----------------------------------------------------------------
REPO = Path(__file__).resolve().parents[3]  # the repo root (these tests are in Support/Plugins/tests)
OPENFX = REPO  # the openfx checkout; in a plug-in repo: REPO / "openfx"
# the directories that contain the .ofx.bundle directories (make -C Support/Plugins CONFIG=release)
BUNDLE_DIRS = sorted(REPO.glob("Support/Plugins/*/*-release"))
# ---------------------------------------------------------------------------------------------

# ofxhost is a plain directory in openfx, not an installed package
sys.path.insert(0, str(OPENFX / "HostSupport" / "examples" / "python"))

import ofxhost  # noqa: E402
from ofxhost.testing import hostdemo  # noqa: E402,F401  (the hostDemo fixture: skips if not built)


@pytest.fixture(scope="session")
def bundles():
    """The plug-in directories (OFX_PLUGIN_PATH); skips the test if no bundle is built."""
    dirs = [d for d in BUNDLE_DIRS if any(d.glob("*.ofx.bundle"))]
    if not dirs:
        pytest.skip(f"no .ofx.bundle in {REPO}/Support/Plugins/*/*-release: "
                    "build them with make -C Support/Plugins CONFIG=release")
    return dirs


@pytest.fixture(scope="session")
def render(hostdemo, bundles):
    """ofxhost.render with our bundles and hostDemo: render(plugin_id, inputs=..., params=...)."""
    return functools.partial(ofxhost.render, bundles, host_binary=hostdemo)


@pytest.fixture(scope="session")
def describe(hostdemo, bundles):
    """ofxhost.describe with our bundles and hostDemo: describe(plugin_id)."""
    return functools.partial(ofxhost.describe, bundles, host_binary=hostdemo)
