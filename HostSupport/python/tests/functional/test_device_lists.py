"""The device lists of the GPU components: opencl_devices(), metal_devices() and cuda_devices(), with
with_error=True (#389). Needs no device: on a machine without one, the lists are empty, with their reason."""

import pytest

ofxh = pytest.importorskip("ofxh")


@pytest.mark.parametrize("api", ["opencl", "metal", "cuda"])
def test_with_error(api):
    """ofxh's <api>_devices(with_error=True): (the same list, why it is empty), the reason a non-empty string
    when the list is empty (built or not: CI's Linux CUDA, built without a driver, gives cuInit's or the
    loader's error), "" when it isn't; a build without the component gives "ofxh was built without ...", so that
the reason a list is empty is always said (hostDemo --gpu-devices and list_gpus.py print it). (#389)"""
    devices = getattr(ofxh, f"{api}_devices")
    listed, why = devices(with_error=True)
    assert isinstance(why, str)
    assert len(listed) == len(devices())
    assert (why == "") == bool(listed), why
    if not ofxh.build_config[f"OFX_HOSTSUPPORT_{api.upper()}"]:
        assert why == f"ofxh was built without OFX_HOSTSUPPORT_{api.upper()}"
