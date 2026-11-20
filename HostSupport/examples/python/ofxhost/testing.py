"""pytest helpers for ofxhost.

    from ofxhost.testing import assert_close, backend, pytest_configure   # backend: a fixture

    def test_invert(backend):       # runs once per backend: [subprocess] and [inprocess]
        r = ofxhost.render(bundles, "net.sf.openfx.invertPlugin", inputs={"Source": src})
        assert_close(r.outputs[0][..., :3], 1 - src, atol=1e-6)

The backend fixture runs a test on each backend (ofxhost.BACKENDS, or those of
$OFXHOST_TEST_BACKENDS, e.g. "subprocess"): it sets $OFXHOST_BACKEND for the test, so that
render(), describe() and ofxhost.graph use it. A backend that isn't available is skipped (hostDemo
not built, ofxh not importable), or fails with OFXH_REQUIRE=1 (CI). A test that depends on one
backend's behaviour says so, with the reason:

    @pytest.mark.backends("subprocess", reason="hostDemo's exit status after a crash")

(import pytest_configure in the conftest.py to register the marker). The hostdemo fixture is the
hostDemo binary, for the subprocess backend only.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

from . import BACKENDS, HostDemoNotFound, find_hostdemo

__all__ = ["assert_close", "find_hostdemo_or_skip", "hostdemo", "backend", "backend_available",
           "test_backends", "pytest_configure"]


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


def find_hostdemo_or_skip(host_binary=None) -> Path:
    """find_hostdemo(), but skip the test (or module) if hostDemo isn't built."""
    try:
        return find_hostdemo(host_binary)
    except HostDemoNotFound as e:
        pytest.skip(str(e))


@pytest.fixture(scope="session")
def hostdemo() -> Path:
    """The hostDemo binary (see find_hostdemo); skips the tests that use it if not built."""
    return find_hostdemo_or_skip()


def test_backends() -> list[str]:
    """The backends the tests run on: $OFXHOST_TEST_BACKENDS (comma-separated), else all."""
    env = os.environ.get("OFXHOST_TEST_BACKENDS")
    b = [x.strip() for x in env.split(",") if x.strip()] if env else list(BACKENDS)
    bad = [x for x in b if x not in BACKENDS]
    if bad:
        raise ValueError(f"OFXHOST_TEST_BACKENDS: unknown backends {bad} (expected {BACKENDS})")
    return b


test_backends.__test__ = False  # (not a test)


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
    """Registers the backends marker (import it in conftest.py)."""
    config.addinivalue_line(
        "markers", "backends(*names, reason): the test depends on these backends' behaviour (the reason "
        "says why); it is skipped on the others, with the reason")


@pytest.fixture(params=test_backends())
def backend(request, monkeypatch) -> str:
    """Each backend in turn ("subprocess", "inprocess"), as $OFXHOST_BACKEND for the test."""
    name = request.param
    m = request.node.get_closest_marker("backends")
    if m is not None and name not in m.args:
        reason = m.kwargs.get("reason")
        if not reason:
            pytest.fail("@pytest.mark.backends(...) needs a reason=")
        pytest.skip(f"only on the {', '.join(m.args)} backend: {reason}")
    why = backend_available(name)
    if why is not None:
        if os.environ.get("OFXH_REQUIRE") == "1":
            pytest.fail(f"backend {name}: {why} (OFXH_REQUIRE=1)")
        pytest.skip(f"backend {name}: {why}")
    monkeypatch.setenv("OFXHOST_BACKEND", name)
    return name
