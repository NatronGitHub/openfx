"""Where the openfx builds are.

hostDemo, the ofxh module (the HostSupport Python bindings) and the sample plug-ins, in this openfx checkout's
Makefile or CMake build trees.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

__all__ = ["OfxhNotFound", "import_ofxh", "hostdemo_candidates", "sample_plugin_dirs"]

_REPO_ROOT = Path(__file__).resolve().parents[4]  # (this file is HostSupport/examples/python/ofxhost/build.py)
_EXAMPLES_DIR = _REPO_ROOT / "HostSupport" / "examples"
_HOSTDEMO = "hostDemo.exe" if sys.platform == "win32" else "hostDemo"


class OfxhNotFound(ImportError):
    """The ofxh module (the HostSupport Python bindings) can't be imported."""


def hostdemo_candidates() -> list[Path]:
    """The hostDemo binaries of the build trees, in order of preference.

    Returns:
        The Makefile builds' (HostSupport/examples/<OS>-release, then relwithdebinfo, debug), then
        the CMake builds' (build*/HostSupport/examples, possibly in a configuration directory).
    """
    found: list[Path] = []
    for config in ("release", "relwithdebinfo", "debug"):
        found += sorted(_EXAMPLES_DIR.glob(f"*-{config}/{_HOSTDEMO}"))
    for pattern in (f"build*/HostSupport/examples/{_HOSTDEMO}",
                    f"build*/HostSupport/examples/*/{_HOSTDEMO}",
                    f"build*/bin/{_HOSTDEMO}"):
        found += sorted(_REPO_ROOT.glob(pattern))
    return [p for p in found if p.is_file() and os.access(p, os.X_OK)]


def sample_plugin_dirs() -> list[Path]:
    """The directories of the Support/Plugins samples.

    $OFX_TEST_PLUGIN_PATH (os.pathsep-separated), else the Makefile build (make -C Support/Plugins CONFIG=release),
    else the CMake build trees' (build*/Plugins). They may hold no bundle yet.
    """
    env = os.environ.get("OFX_TEST_PLUGIN_PATH")
    if env:
        return [Path(d) for d in env.split(os.pathsep) if d]
    return sorted((_REPO_ROOT / "Support" / "Plugins").glob("*/*-release")) or sorted(_REPO_ROOT.glob("build*/Plugins"))


def _within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def import_ofxh(plugin_path: str | None = None, for_cli: bool = False):
    """Import the ofxh module.

    From sys.path (PYTHONPATH), else from an openfx CMake build tree (build*/HostSupport/python,
    built with -DOFX_PYTHON_BINDINGS=ON): the one that holds the plug-in directories, so that the
    module and the plug-ins come from the same configuration, else the only one there is.

    Args:
        plugin_path: the plug-in directories, os.pathsep-separated (None: $OFX_TEST_PLUGIN_PATH,
            the tests').
        for_cli: the error message also says to give -b a plug-in directory of a build tree.

    Returns:
        The ofxh module.

    Raises:
        OfxhNotFound: not importable, or several build trees have one and none holds the plug-ins.
    """
    try:
        import ofxh
        return ofxh
    except ImportError:
        pass
    trees = [d for d in sorted(_REPO_ROOT.glob("build*/HostSupport/python"))
             if any(d.glob("ofxh*.so")) or any(d.glob("ofxh*.pyd"))]
    paths = os.environ.get("OFX_TEST_PLUGIN_PATH", "") if plugin_path is None else plugin_path
    plugin_dirs = [Path(p).resolve() for p in paths.split(os.pathsep) if p]
    matching = [d for d in trees if any(_within(p, d.resolve().parents[1]) for p in plugin_dirs)]
    if len(trees) > 1 and not matching:
        hint = ", or give -b a plug-in directory of that build tree (<build>/Plugins)" if for_cli else ""
        raise OfxhNotFound("the ofxh module (HostSupport Python bindings) is not on the Python path, and several "
                           f"openfx build trees have one ({', '.join(str(d) for d in trees)}): set PYTHONPATH to "
                           "the one to use, <build>/HostSupport/python" + hint)
    for d in matching[:1] or trees:
        sys.path.insert(0, str(d))
        try:
            import ofxh
            return ofxh
        except ImportError:
            sys.path.remove(str(d))
    raise OfxhNotFound("the ofxh module (HostSupport Python bindings) is not importable: build it with "
                       "cmake -DOFX_PYTHON_BINDINGS=ON and set PYTHONPATH to <build>/HostSupport/python, "
                       "or pip install ./HostSupport/python")
