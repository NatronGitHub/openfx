import sys
from pathlib import Path

import pytest

# make the ofxhost package importable without installing it
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ofxhost import find_hostdemo  # noqa: E402
from ofxhost.build import sample_plugin_dirs  # noqa: E402
from ofxhost.testing import backend, pytest_configure  # noqa: E402,F401  (pytest fixture and hook)


@pytest.fixture
def hostdemo(backend):
    """The tests that render run on both backends (the backend fixture): this is hostDemo for the
    subprocess one, None in-process (render() ignores host_binary there)."""
    return find_hostdemo() if backend == "subprocess" else None


@pytest.fixture(scope="session")
def sample_plugins():
    """The Support/Plugins sample bundles (ofxhost.build.sample_plugin_dirs())."""
    dirs = [p for p in sample_plugin_dirs() if any(p.glob("*.ofx.bundle"))]
    if not dirs:
        pytest.skip("Support/Plugins samples not built (make -C Support/Plugins CONFIG=release)")
    return dirs
