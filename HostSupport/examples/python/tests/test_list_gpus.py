"""list_gpus.py: it runs without a GPU (the lists may be empty), and --json has the APIs of the platform, as ofxh lists
them."""

import json
import os
import subprocess
import sys
from pathlib import Path

from ofxhost.testing import check_backend

SCRIPT = Path(__file__).resolve().parents[1] / "list_gpus.py"
# the APIs of the platform: Metal on macOS only, CUDA everywhere but macOS (it has no CUDA driver)
APIS = {"opengl": "OFX_HOSTSUPPORT_OPENGL", "opencl": "OFX_HOSTSUPPORT_OPENCL",
        **({"metal": "OFX_HOSTSUPPORT_METAL"} if sys.platform == "darwin" else {"cuda": "OFX_HOSTSUPPORT_CUDA"})}
TITLES = {"opengl": "OpenGL", "opencl": "OpenCL", "metal": "Metal", "cuda": "CUDA"}

check_backend("inprocess")  # (the ofxh module; OFXH_REQUIRE=1: fail without it)


def _run(*args):
    import ofxh
    env = dict(os.environ, PYTHONPATH=str(Path(ofxh.__file__).resolve().parent))
    p = subprocess.run([sys.executable, str(SCRIPT), *args], env=env, capture_output=True, text=True, timeout=120,
                       stdin=subprocess.DEVNULL)
    assert p.returncode == 0, (p.returncode, p.stderr)
    return p.stdout


def test_text():
    import ofxh
    out = _run()
    titles = [line.split(":")[0] for line in out.splitlines() if not line.startswith(" ")]
    assert titles == [TITLES[key] for key in APIS]
    for key, option in APIS.items():
        line = next(x for x in out.splitlines() if x.startswith(f"{TITLES[key]}: "))
        assert ("not built" in line) == (not ofxh.build_config[option]), line


def test_json():
    import ofxh
    found = json.loads(_run("--json"))
    assert list(found) == list(APIS)
    listed = {"opengl": ofxh.opengl_renderers, "opencl": ofxh.opencl_devices, "metal": ofxh.metal_devices,
              "cuda": ofxh.cuda_devices}
    for key, option in APIS.items():
        assert found[key]["built"] is ofxh.build_config[option], key
        assert len(found[key]["devices"]) == len(listed[key]()), key
        error = found[key]["error"]
        if key == "opengl" and found[key]["built"]:
            assert error is None, error  # (GL's listing gives no reason)
        else:
            assert (error == "") == bool(found[key]["devices"]), (key, error)
