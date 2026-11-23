import sys
from pathlib import Path

import pytest

# make the ofxhost package importable without installing it
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ofxhost.build import sample_plugin_dirs  # noqa: E402
from ofxhost.testing import backend, hostdemo, pytest_configure  # noqa: E402,F401  (pytest fixtures and hook)


@pytest.fixture(scope="session")
def sample_plugins():
    """The Support/Plugins sample bundles (ofxhost.build.sample_plugin_dirs())."""
    dirs = [p for p in sample_plugin_dirs() if any(p.glob("*.ofx.bundle"))]
    if not dirs:
        pytest.skip("Support/Plugins samples not built (make -C Support/Plugins CONFIG=release)")
    return dirs


@pytest.fixture
def tmp_cwd(tmp_path, monkeypatch):
    """Runs the test in a temporary directory, and shuts the ofxh host down after it.

    The plug-in scan makes PropTester write ofxTestLog.txt in the current directory, and the
    in-process tests' modules use the ofxh host.
    """
    monkeypatch.chdir(tmp_path)
    yield tmp_path
    import ofxh
    from ofxhost import inprocess
    inprocess.shutdown()
    ofxh.shutdown()
