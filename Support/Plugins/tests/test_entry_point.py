"""The C entry point of a Support plug-in (OfxPlugin::mainEntry), called directly with ctypes.

Pattern: a call that no host makes, and that could crash the plug-in: it runs in a subprocess, so
that a crash fails the test instead of killing pytest.
- a null action returns kOfxStatFailed (the Support library turned it into a std::string,
  undefined behaviour, before checking it);
- an action the plug-in doesn't know returns kOfxStatReplyDefault: "the value kOfxStatReplyDefault
  is returned if the plug-in does not trap the action" (Reference/ofxCoreAPI.rst).
Neither needs OfxSetHost or the load action: the Support library checks the action first.
"""

import subprocess
import sys

import pytest

PLUGIN = "net.sf.openfx.basicPlugin"

kOfxStatFailed = 1
kOfxStatReplyDefault = 14

# run in a subprocess: argv[1] is the plug-in identifier, argv[2:] the binaries that may hold it
SCRIPT = r"""
import ctypes, sys

class OfxPlugin(ctypes.Structure):
    _fields_ = [("pluginApi", ctypes.c_char_p), ("apiVersion", ctypes.c_int),
                ("pluginIdentifier", ctypes.c_char_p), ("pluginVersionMajor", ctypes.c_uint),
                ("pluginVersionMinor", ctypes.c_uint), ("setHost", ctypes.c_void_p),
                ("mainEntry", ctypes.c_void_p)]

Entry = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
for binary in sys.argv[2:]:
    lib = ctypes.CDLL(binary)
    lib.OfxGetNumberOfPlugins.restype = ctypes.c_int
    lib.OfxGetPlugin.restype = ctypes.POINTER(OfxPlugin)
    lib.OfxGetPlugin.argtypes = [ctypes.c_int]
    for i in range(lib.OfxGetNumberOfPlugins()):
        plugin = lib.OfxGetPlugin(i).contents
        if plugin.pluginIdentifier.decode() == sys.argv[1]:
            entry = Entry(plugin.mainEntry)
            print(binary, entry(None, None, None, None), entry(b"NotAnOfxAction", None, None, None))
            sys.stdout.flush()
            sys.exit(0)
sys.exit("no plug-in " + sys.argv[1] + " in " + " ".join(sys.argv[2:]))
"""


def _binaries(bundles, name):
    """The binaries (Contents/<arch>/<name>.ofx) of the bundles whose name contains name."""
    binaries = [str(b) for d in bundles for bundle in sorted(d.glob("*.ofx.bundle"))
                if name in bundle.name.lower() for b in sorted(bundle.glob("Contents/*/*.ofx"))]
    if not binaries:
        pytest.skip(f"no {name} bundle in {bundles}")
    return binaries


def test_null_and_unknown_action(bundles, tmp_path):
    # cwd: a debug build of the plug-in writes ofxTestLog.txt in the current directory
    r = subprocess.run([sys.executable, "-c", SCRIPT, PLUGIN, *_binaries(bundles, "basic")],
                       capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
                       cwd=tmp_path)
    assert r.returncode == 0, f"exit status {r.returncode}\nstdout:\n{r.stdout}\nstderr:\n{r.stderr}"
    null_status, unknown_status = (int(s) for s in r.stdout.split()[-2:])
    assert null_status == kOfxStatFailed
    assert unknown_status == kOfxStatReplyDefault
