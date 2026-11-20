"""pytest configuration of the ofxh tests (HostSupport/python/tests).

The ofxh module must be importable: set PYTHONPATH to the directory of the built module
(<build>/HostSupport/python); without it, the tests are skipped, unless OFXH_REQUIRE=1 (CI),
which makes them fail. The sample plug-ins and hostDemo
(to compare with) come from the same build directory as the module: <build>/Plugins and
<build>/HostSupport/examples/hostDemo, unless $OFX_TEST_PLUGIN_PATH or $OFXHOST_HOSTDEMO are set.
The tests don't read OFX_PLUGIN_PATH (ofxh.init(..., use_env_path=False)), and hostDemo gets the
same plug-in directory, so that both load the same binaries.
"""

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
# minimal_host.py and the ofxhost package
sys.path.insert(0, str(REPO_ROOT / "HostSupport" / "examples" / "python"))

# OFXH_REQUIRE=1 (CI): fail instead of skipping when the module, the plug-ins or hostDemo are missing
REQUIRE = os.environ.get("OFXH_REQUIRE") == "1"

try:
    import ofxh  # PYTHONPATH: the build directory of the module
except ImportError:
    if REQUIRE:
        raise
    ofxh = None


def _skip(message):
    if REQUIRE:
        pytest.fail(message + " (OFXH_REQUIRE=1)")
    pytest.skip(message)


def _build_dir():
    if ofxh is None:
        _skip("the ofxh module is not built or not on PYTHONPATH (-DOFX_PYTHON_BINDINGS=ON)")
    # <build>/HostSupport/python/ofxh.*.so, possibly in a configuration directory
    here = Path(ofxh.__file__).resolve().parent
    for d in (here, *here.parents):
        if (d / "CMakeCache.txt").is_file():
            return d
    return here.parents[1]


@pytest.fixture(scope="session")
def plugin_dir():
    """The directory of the sample bundles of the module's build (with the Support/Plugins Invert)."""
    env = os.environ.get("OFX_TEST_PLUGIN_PATH")
    cands = [Path(d) for d in env.split(os.pathsep) if d] if env else [_build_dir() / "Plugins"]
    for d in cands:
        if (d / "example-Invert-support.ofx.bundle").is_dir():
            return str(d)
    _skip("the sample plug-ins are not built (cmake -DBUILD_EXAMPLE_PLUGINS=ON), or set OFX_TEST_PLUGIN_PATH")


@pytest.fixture(scope="session")
def hostdemo():
    """hostDemo of the module's build (or $OFXHOST_HOSTDEMO)."""
    env = os.environ.get("OFXHOST_HOSTDEMO")
    if env:
        return Path(env)
    exe = "hostDemo.exe" if sys.platform == "win32" else "hostDemo"
    examples = _build_dir() / "HostSupport" / "examples"
    for p in [examples / exe, *sorted(examples.glob(f"*/{exe}"))]:
        if p.is_file() and os.access(p, os.X_OK):
            return p
    _skip("hostDemo is not built in the module's build directory (cmake -DBUILD_EXAMPLE_HOSTS=ON)")


@pytest.fixture(autouse=True)
def _shutdown():
    """Each test starts and ends without a host."""
    if ofxh is not None:
        ofxh.shutdown()
    yield
    if ofxh is not None:
        ofxh.shutdown()
