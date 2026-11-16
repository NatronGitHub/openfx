"""pytest helpers for ofxhost.

    from ofxhost.testing import assert_close, hostdemo   # hostdemo is a pytest fixture

    def test_invert(hostdemo):
        r = ofxhost.render(bundles, "net.sf.openfx.invertPlugin", inputs={"Source": src},
                           host_binary=hostdemo)
        assert_close(r.outputs[0][..., :3], 1 - src, atol=1e-6)
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from . import HostDemoNotFound, find_hostdemo

__all__ = ["assert_close", "find_hostdemo_or_skip", "hostdemo"]


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
