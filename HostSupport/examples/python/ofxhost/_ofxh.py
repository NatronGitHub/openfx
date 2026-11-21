"""Find and import the ofxh module (the HostSupport Python bindings) for ofxhost.inprocess, ofxhost.graphhost
and the command line."""

from __future__ import annotations

import os
import sys
from pathlib import Path

__all__ = ["import_ofxh", "OfxhNotFound"]


class OfxhNotFound(ImportError):
    """The ofxh module (the HostSupport Python bindings) can't be imported."""


_REPO_ROOT = Path(__file__).resolve().parents[4]


def _within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def import_ofxh(plugin_path: str | None = None, for_cli: bool = False):
    """Import ofxh: from sys.path (PYTHONPATH); else from an openfx CMake build tree
    (build*/HostSupport/python, built with -DOFX_PYTHON_BINDINGS=ON): the one that holds the
    plug-in directories of plugin_path (os.pathsep-separated; default $OFX_TEST_PLUGIN_PATH, the
    tests'), so that the module and the plug-ins come from the same configuration, else the only
    one there is. With several trees and none of them the plug-ins', it raises OfxhNotFound, which
    says to set PYTHONPATH (for_cli: or to give -b a directory of one of them)."""
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
        raise OfxhNotFound("the ofxh module (HostSupport Python bindings) is not on the Python path, and several "
                           f"openfx build trees have one ({', '.join(str(d) for d in trees)}): set PYTHONPATH to "
                           "the one to use, <build>/HostSupport/python"
                           + (", or give -b a plug-in directory of that build tree (<build>/Plugins)" if for_cli else ""))
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
