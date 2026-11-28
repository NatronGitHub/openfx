"""pytest configuration of the ofxhost tests (HostSupport/examples/python/tests).

functional/ has the tests of ofxhost's contracts (its API, the hostDemo command line of python -m
ofxhost.hostdemo, the sample host); regression/ the tests of past bugs, each naming its issue or PR. The
helpers that both use are in ofxhost_helpers.py, next to this file. Fixtures: sample_plugins (the
Support/Plugins sample bundles: skipped if they aren't built, failed with OFXH_REQUIRE=1), tmp_cwd, both (both
backends), ofxh_dir and env (the batch commands'), and ofxhost.testing's backend and hostdemo fixtures and
markers.
"""

import os
import sys
from pathlib import Path

import pytest

# make the ofxhost package importable without installing it, and the shared helpers (ofxhost_helpers)
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import ofxhost  # noqa: E402
from ofxhost.build import sample_plugin_dirs  # noqa: E402
from ofxhost.testing import _skip_or_fail, backend, check_backend, hostdemo, pytest_configure  # noqa: E402,F401
from ofxhost_helpers import HOOK, PYDIR  # noqa: E402


@pytest.fixture(scope="session")
def sample_plugins():
    """The Support/Plugins sample bundles (ofxhost.build.sample_plugin_dirs()); skips the test if none is built,
    or fails with OFXH_REQUIRE=1 (CI), as Support/Plugins/tests' bundles fixture."""
    dirs = [p for p in sample_plugin_dirs() if any(p.glob("*.ofx.bundle"))]
    if not dirs:
        _skip_or_fail("Support/Plugins samples not built (make -C Support/Plugins CONFIG=release)")
    return dirs


@pytest.fixture
def tmp_cwd(tmp_path, monkeypatch):
    """Runs the test in a temporary directory, and shuts the ofxh host down after it.

    The plug-in scan makes PropTester write ofxPluginLog.txt in the current directory, and the
    in-process tests' modules use the ofxh host.
    """
    monkeypatch.chdir(tmp_path)
    yield tmp_path
    import ofxh
    from ofxhost import inprocess
    inprocess.shutdown()
    ofxh.shutdown()


@pytest.fixture
def both(sample_plugins):
    """The sample plug-ins, with both backends available (OFXH_REQUIRE=1: fail if one isn't)."""
    for b in ofxhost.BACKENDS:
        check_backend(b)
    return sample_plugins


@pytest.fixture(scope="module")
def ofxh_dir(sample_plugins):
    """The directory of the ofxh module (the in-process backend must be available)."""
    check_backend("inprocess")
    import ofxh
    return Path(ofxh.__file__).resolve().parent


@pytest.fixture(scope="module")
def env(sample_plugins, ofxh_dir):
    """The environment of the batch commands: the sample plug-ins, ofxhost and ofxh on PYTHONPATH, no test hook,
    no kept host and no capture setting inherited."""
    e = dict(os.environ)
    e["OFX_PLUGIN_PATH"] = os.pathsep.join(str(p) for p in sample_plugins)
    e["PYTHONPATH"] = os.pathsep.join([str(PYDIR), str(ofxh_dir)])
    for k in ("OFX_HOSTDEMO_KEEP_LOADED", HOOK, "OFXHOST_CAPTURE"):
        e.pop(k, None)
    return e
