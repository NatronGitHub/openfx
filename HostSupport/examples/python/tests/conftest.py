import os
import sys
from pathlib import Path

import pytest

# make the ofxhost package importable without installing it
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ofxhost import find_hostdemo  # noqa: E402
from ofxhost.testing import backend, pytest_configure  # noqa: E402,F401  (pytest fixture and hook)


@pytest.fixture
def hostdemo(backend):
    """The tests that render run on both backends (the backend fixture): this is hostDemo for the
    subprocess one, None in-process (render() ignores host_binary there)."""
    return find_hostdemo() if backend == "subprocess" else None

REPO_ROOT = Path(__file__).resolve().parents[4]


@pytest.fixture(scope="session")
def sample_plugins():
    """The Support/Plugins sample bundles: $OFX_TEST_PLUGIN_PATH, else the Make build
    (make -C Support/Plugins CONFIG=release), else a CMake build tree (build*/Plugins)."""
    env = os.environ.get("OFX_TEST_PLUGIN_PATH")
    cands = ([Path(d) for d in env.split(os.pathsep) if d] if env else
             sorted((REPO_ROOT / "Support" / "Plugins").glob("*/*-release"))
             or sorted(REPO_ROOT.glob("build*/Plugins")))
    dirs = [p for p in cands if any(p.glob("*.ofx.bundle"))]
    if not dirs:
        pytest.skip("Support/Plugins samples not built (make -C Support/Plugins CONFIG=release)")
    return dirs
