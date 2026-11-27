"""pytest helpers for ofxhost: assert_close(), and the backend fixture and marker.

    from ofxhost.testing import assert_close, backend, pytest_configure   # in conftest.py

    def test_invert(backend):       # runs once per backend: [subprocess] and [inprocess]
        r = ofxhost.render(bundles, "net.sf.openfx.invertPlugin", inputs={"Source": src})
        assert_close(r.outputs[0][..., :3], 1 - src, atol=1e-6)

The backend fixture sets $OFXHOST_BACKEND for the test (each of ofxhost.BACKENDS, or those of
$OFXHOST_TEST_BACKENDS); an unavailable backend is skipped, or fails with OFXH_REQUIRE=1 (CI). A test
that depends on one backend's behaviour says so: @pytest.mark.backends("subprocess", reason="...").
Get them with pytest_plugins = ["ofxhost.testing"] in the top-level conftest.py, or the import
above in any conftest.py (one with its own pytest_configure calls ofxhost.testing.pytest_configure()).
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path

import numpy as np
import pytest

from . import BACKENDS, HostDemoNotFound, find_hostdemo

__all__ = ["assert_close", "suite_function", "find_hostdemo_or_skip", "hostdemo", "backend", "backend_available",
           "selected_backends", "check_backend", "pytest_configure"]


def assert_close(actual, expected, atol: float = 0.0, rtol: float = 0.0, *,
                 equal_nan: bool = True, max_report: int = 5, msg: str = "") -> None:
    """Assert |actual - expected| <= atol + rtol * |expected| elementwise.

    NaN matches NaN if equal_nan (a NaN on one side only always fails). expected may be a
    scalar or broadcastable to actual's shape. On failure, the message gives the number
    of mismatching values, the largest difference and where it is, and the first few
    mismatches as (index): actual vs expected.
    """
    a = np.asarray(actual, dtype=np.float64)
    b = np.asarray(expected, dtype=np.float64)
    try:
        b = np.broadcast_to(b, a.shape)
    except ValueError:
        raise AssertionError(f"{msg + ': ' if msg else ''}shape mismatch: actual "
                             f"{a.shape}, expected {np.shape(expected)}") from None
    nan_a, nan_b = np.isnan(a), np.isnan(b)
    with np.errstate(invalid="ignore"):
        diff = np.abs(a - b)
        ok = diff <= atol + rtol * np.abs(b)
    ok |= (nan_a & nan_b) if equal_nan else False
    ok &= ~(nan_a ^ nan_b)
    if ok.all():
        return
    bad = np.argwhere(~ok)
    finite = np.where(np.isfinite(diff), diff, -1.0)
    worst = np.unravel_index(int(np.argmax(finite)), a.shape) if a.size else ()
    lines = [f"{msg + ': ' if msg else ''}{len(bad)} of {a.size} values differ "
             f"(atol={atol}, rtol={rtol}), shape {a.shape}"]
    if finite.max(initial=-1.0) >= 0:
        lines.append(f"max |difference| = {diff[worst]:.9g} at {tuple(int(i) for i in worst)}"
                     f": {a[worst]:.9g} vs {b[worst]:.9g}")
    if (nan_a ^ nan_b).any():
        lines.append(f"NaN in actual only: {int((nan_a & ~nan_b).sum())}, "
                     f"in expected only: {int((nan_b & ~nan_a).sum())}")
    for idx in bad[:max_report]:
        i = tuple(int(x) for x in idx)
        lines.append(f"  {i}: {a[i]:.9g} vs {b[i]:.9g}")
    if len(bad) > max_report:
        lines.append(f"  ... and {len(bad) - max_report} more")
    raise AssertionError("\n".join(lines))


def suite_function(name: str, version: int, index: int, prototype):
    """Function index of the host's suite (ofxh.fetch_suite(name, version)), as a ctypes prototype: to
    call it as a plug-in does."""
    import ofxh  # (not at the top: ofxhost.testing doesn't need ofxh)

    suite = ofxh.fetch_suite(name, version)
    assert suite, f"no suite {name} {version}"
    return prototype(ctypes.c_void_p.from_address(suite + index * ctypes.sizeof(ctypes.c_void_p)).value)


def find_hostdemo_or_skip(host_binary=None) -> Path:
    """find_hostdemo(), but skip the test (or module) if hostDemo isn't built."""
    try:
        return find_hostdemo(host_binary)
    except HostDemoNotFound as e:
        pytest.skip(str(e))


@pytest.fixture
def hostdemo(backend) -> Path | None:
    """The hostDemo binary on the subprocess backend, None on the in-process one.

    Skips the test if hostDemo isn't built. render(..., host_binary=hostdemo) then runs on both
    backends (the backend fixture).
    """
    return find_hostdemo_or_skip() if backend == "subprocess" else None


def selected_backends() -> list[str]:
    """The backends the tests run on: $OFXHOST_TEST_BACKENDS (comma-separated), else all."""
    env = os.environ.get("OFXHOST_TEST_BACKENDS")
    b = [x.strip() for x in env.split(",") if x.strip()] if env else list(BACKENDS)
    bad = [x for x in b if x not in BACKENDS]
    if bad:
        raise ValueError(f"OFXHOST_TEST_BACKENDS: unknown backends {bad} (expected {BACKENDS})")
    return b


def backend_available(name: str) -> str | None:
    """None if the backend can run here, else why not."""
    if name == "subprocess":
        try:
            find_hostdemo()
        except HostDemoNotFound as e:
            return str(e)
        return None
    try:
        from . import inprocess  # noqa: F401  (imports ofxh)
    except ImportError as e:
        return str(e)
    return None


def pytest_configure(config):
    """Registers the backends and opencl markers (import it in conftest.py)."""
    config.addinivalue_line(
        "markers", "backends(*names, reason): the test depends on these backends' behaviour (the reason "
        "says why); it is skipped on the others, with the reason")
    config.addinivalue_line(
        "markers", "opencl: the test needs an OpenCL device and the sample plug-ins built with OpenCL "
        "(OFX_SUPPORTS_OPENCLRENDER); CI selects them (-m opencl) where it has both, and deselects them "
        "(-m 'not opencl') elsewhere")


def check_backend(name: str, marker=None) -> None:
    """Skip (or fail) a test, or a whole test module (at its top level), on the backend name.

    Args:
        name: "subprocess" or "inprocess". An unavailable backend is skipped, or fails with
            OFXH_REQUIRE=1.
        marker: the test's backends marker (a pytest Mark), or None: the other backends are skipped
            with its reason (a marker without reason= fails).
    """
    if marker is not None:
        reason = marker.kwargs.get("reason")
        if not reason:
            pytest.fail("@pytest.mark.backends(...) needs a reason=")
        if name not in marker.args:
            pytest.skip(f"only on the {', '.join(marker.args)} backend: {reason}")
    why = backend_available(name)
    if why is not None:
        if os.environ.get("OFXH_REQUIRE") == "1":
            pytest.fail(f"backend {name}: {why} (OFXH_REQUIRE=1)", pytrace=False)
        pytest.skip(f"backend {name}: {why}", allow_module_level=True)


@pytest.fixture(params=selected_backends())
def backend(request, monkeypatch) -> str:
    """Each backend in turn ("subprocess", "inprocess"), as $OFXHOST_BACKEND for the test."""
    name = request.param
    check_backend(name, request.node.get_closest_marker("backends"))
    monkeypatch.setenv("OFXHOST_BACKEND", name)
    return name
