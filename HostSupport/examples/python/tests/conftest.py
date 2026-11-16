import sys
from pathlib import Path

import pytest

# make the ofxhost package importable without installing it
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ofxhost.testing import hostdemo  # noqa: E402,F401  (pytest fixture)

REPO_ROOT = Path(__file__).resolve().parents[4]


@pytest.fixture(scope="session")
def sample_plugins():
    """The Support/Plugins sample bundles (make -C Support/Plugins CONFIG=release)."""
    dirs = sorted(p for p in (REPO_ROOT / "Support" / "Plugins").glob("*/*-release")
                  if any(p.glob("*.ofx.bundle")))
    if not dirs:
        pytest.skip("Support/Plugins samples not built (make -C Support/Plugins CONFIG=release)")
    return dirs
