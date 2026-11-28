#!/usr/bin/env python3
"""Lists the GPU renderers and devices that an OFX host on the HostSupport bindings (the ofxh module) can use.

For each GPU API (OpenGL, OpenCL, Metal, CUDA), it says whether ofxh was built with it
(ofxh.build_config) and lists its renderers or devices, each with the ofxh.init(host, ...)
arguments that choose it and its main facts. It needs no ofxh.init(): it loads no plug-in.

    PYTHONPATH=<build>/HostSupport/python python list_gpus.py [--json]

Metal is listed on macOS only, and CUDA everywhere but macOS (it has no CUDA driver: ofxh can be
built with OFX_HOSTSUPPORT_CUDA there, for the checks on a fake driver).

An empty list says why when ofxh knows it (the with_error=True of opencl_devices(), metal_devices()
and cuda_devices(); OpenGL's listing gives no reason).

--json prints one JSON object instead, with a key per API of the platform ("opengl", "opencl", and
"metal" on macOS or "cuda" elsewhere): {"built": bool, "devices": [...], "error": str}, the devices
as ofxh.opengl_renderers(), opencl_devices(), metal_devices() and cuda_devices() give them, and the
error why the list is empty ("" if it isn't; for OpenGL, null when it is built, as its listing
gives no reason).
hostDemo --gpu-devices prints the same lists, one line each.
"""

import argparse
import json
import sys

import ofxh

MIB = 1 << 20


def mib(n):
    """Returns n bytes in MiB, as text ("unknown" if n isn't positive)."""
    return f"{n // MIB} MiB" if n > 0 else "unknown"


# The describe_*() functions take a dict of ofxh's list of renderers or devices, and return its lines:
# the ofxh.init() arguments that choose it (host is the ofxh.Host), then its facts.

def describe_opengl(d):
    """Returns the lines of an OpenGL renderer of ofxh.opengl_renderers()."""
    return [f"init(host, opengl=True, opengl_backends=[{d['backend']!r}], opengl_renderer={d['id']})",
            f"{d['renderer']} ({d['vendor']}{', software' if d['software'] else ''})",
            f"OpenGL {d['version']}, GLSL {d['glsl'] or '-'}, video memory {mib(d['video_memory'])}"]


def describe_opencl(d):
    """Returns the lines of an OpenCL device of ofxh.opencl_devices()."""
    return [f"init(host, opencl=True, opencl_device=({d['platform']}, {d['device']}))",
            f"{d['name']} ({d['vendor']}{', GPU' if d['gpu'] else ', not a GPU'})",
            f"platform {d['platform_name']} ({d['platform_version']})",
            f"{d['version']}, driver {d['driver_version']}, global memory {mib(d['global_mem_size'])}, "
            f"{'images' if d['images'] else 'no images'}"]


def describe_metal(d):
    """Returns the lines of a Metal device of ofxh.metal_devices()."""
    flags = ((", unified memory" if d["unified_memory"] else "")
             + (", system default" if d["system_default"] else ""))
    return [f"init(host, metal=True, metal_device={d['index']})",
            f"{d['name']}{flags}",
            f"recommended working set {mib(d['recommended_max_working_set_size'])}, "
            f"max buffer {mib(d['max_buffer_length'])}"]


def describe_cuda(d):
    """Returns the lines of a CUDA device of ofxh.cuda_devices()."""
    v = d["driver_version"]
    major, minor = d["compute_capability"]
    return [f"init(host, cuda=True, cuda_device={d['index']}) or init(host, cuda=True, cuda_uuid={d['uuid']!r})",
            f"{d['name']} (compute capability {major}.{minor}, PCI {d['pci_bus_id']})",
            f"memory {mib(d['total_mem'])}, driver {v // 1000}.{v % 1000 // 10}"]


def list_opengl():
    """Returns (the OpenGL renderers, why the list is empty).

    Why is None when ofxh is built with OpenGL: GL's listing gives no reason for an empty list.
    """
    if not ofxh.build_config.get("OFX_HOSTSUPPORT_OPENGL"):
        return [], "ofxh was built without OFX_HOSTSUPPORT_OPENGL"  # (as opencl_devices(with_error=True)...)
    return ofxh.opengl_renderers(), None


# the APIs of this platform: (key, title, build option, the function that returns (the devices, why
# the list is empty), the function that describes one)
APIS = [
    ("opengl", "OpenGL", "OFX_HOSTSUPPORT_OPENGL", list_opengl, describe_opengl),
    ("opencl", "OpenCL", "OFX_HOSTSUPPORT_OPENCL", lambda: ofxh.opencl_devices(with_error=True), describe_opencl),
]
if sys.platform == "darwin":
    APIS.append(("metal", "Metal", "OFX_HOSTSUPPORT_METAL", lambda: ofxh.metal_devices(with_error=True),
                 describe_metal))
else:  # (macOS has no CUDA driver: ofxh may be built with CUDA there for its checks on a fake driver)
    APIS.append(("cuda", "CUDA", "OFX_HOSTSUPPORT_CUDA", lambda: ofxh.cuda_devices(with_error=True),
                 describe_cuda))


def main():
    """Prints the lists (see the module docstring)."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", action="store_true", help="print one JSON object")
    args = parser.parse_args()
    found = {}
    for key, _, option, list_devices, _ in APIS:
        devices, error = list_devices()
        found[key] = {"built": bool(ofxh.build_config.get(option)), "devices": devices, "error": error}
    if args.json:
        print(json.dumps(found, indent=2))
        return
    for key, title, option, _, describe in APIS:
        if not found[key]["built"]:
            print(f"{title}: not built (ofxh was built without {option})")
            continue
        devices = found[key]["devices"]
        if not devices:
            error = found[key]["error"]
            print(f"{title}: none ({error})" if error else f"{title}: none")
            continue
        kind = "renderer" if key == "opengl" else "device"
        print(f"{title}: {len(devices)} {kind}{'' if len(devices) == 1 else 's'}")
        for d in devices:
            first, *rest = describe(d)
            print(f"  {first}")
            for line in rest:
                print(f"    {line}")


if __name__ == "__main__":
    main()
