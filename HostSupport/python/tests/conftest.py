"""pytest configuration of the ofxh tests (HostSupport/python/tests).

The ofxh module must be importable: set PYTHONPATH to the directory of the built module
(<build>/HostSupport/python). The sample plug-ins are found in $OFX_TEST_PLUGIN_PATH, else in a
CMake build tree of the repository (build*/Plugins); hostDemo (to compare with) with ofxhost's
rules ($OFXHOST_HOSTDEMO, then the build tree).
"""

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
# minimal_host.py and the ofxhost package
sys.path.insert(0, str(REPO_ROOT / "HostSupport" / "examples" / "python"))

import ofxh  # noqa: E402  (PYTHONPATH: the build directory of the module)
from ofxhost.testing import hostdemo  # noqa: E402,F401  (pytest fixture)


@pytest.fixture(scope="session")
def plugin_dir():
    """The directory of the sample bundles (with the Support/Plugins Invert)."""
    env = os.environ.get("OFX_TEST_PLUGIN_PATH")
    cands = [Path(d) for d in env.split(os.pathsep) if d] if env else sorted(REPO_ROOT.glob("build*/Plugins"))
    for d in cands:
        if (d / "example-Invert-support.ofx.bundle").is_dir():
            return str(d)
    pytest.skip("the sample plug-ins are not built (cmake -DBUILD_EXAMPLE_PLUGINS=ON), or set OFX_TEST_PLUGIN_PATH")


@pytest.fixture(autouse=True)
def _shutdown():
    """Each test starts and ends without a host."""
    ofxh.shutdown()
    yield
    ofxh.shutdown()
