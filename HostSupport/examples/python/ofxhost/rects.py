"""Rectangles (x1, y1, x2, y2), as hostDemo computes them: canonical coordinates and pixels.

Pixel rectangles are at the render scale; canonical x is in pixels times the pixel aspect ratio
(par), at scale 1. The functions take and give tuples.
"""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence

import numpy as np

__all__ = ["INT_MIN", "INT_MAX", "MAX_IMAGE_PIXELS", "scale_pair", "to_pixels", "to_canonical", "intersect", "union",
           "contains", "is_empty", "fits"]

INT_MIN, INT_MAX = -2**31, 2**31 - 1
_EPS = 1e-6  # (hostDemo's canonicalToPixelEnclosing)
MAX_IMAGE_PIXELS = 1 << 28  # (hostDemo's kMaxImagePixels)


def _clamp_int(v: float, rounding: Callable[[float], int]) -> int:
    """Round v to an int, clamped to the int range (NaN gives 0), as hostDemo's clampToInt.

    Clamps before rounding: math.floor and math.ceil raise on an infinite or NaN value.
    """
    if math.isnan(v):
        return 0
    if v <= INT_MIN:
        return INT_MIN
    if v >= INT_MAX:
        return INT_MAX
    return rounding(v)


def scale_pair(scale) -> tuple[float, float]:
    """A render scale as (sx, sy) floats.

    Args:
        scale: s (a number, or a 0-d array), which is both, or (sx, sy) (a sequence or an array).

    Returns:
        (sx, sy). Their range isn't checked.

    Raises:
        ValueError: not one number or two.
    """
    try:
        v = np.asarray(scale, dtype=np.float64)
    except (TypeError, ValueError):
        v = None
    if v is None or v.shape not in ((), (2,)):
        raise ValueError(f"render scale {scale!r}: expected s or (sx, sy)")
    return (float(v), float(v)) if v.ndim == 0 else (float(v[0]), float(v[1]))


def to_pixels(r: Sequence[float], rs: Sequence[float], par: float) -> tuple:
    """The smallest pixel rectangle that encloses a canonical one, as hostDemo's canonicalToPixelEnclosing.

    Args:
        r: the canonical rectangle; an infinite or NaN bound (a plug-in's RoD or RoI) is clamped to
            the int range, or to 0 for NaN.
        rs: the render scale (sx, sy).
        par: the pixel aspect ratio.

    Returns:
        The pixel rectangle, ints.
    """
    return (_clamp_int(r[0] * rs[0] / par + _EPS, math.floor), _clamp_int(r[1] * rs[1] + _EPS, math.floor),
            _clamp_int(r[2] * rs[0] / par - _EPS, math.ceil), _clamp_int(r[3] * rs[1] - _EPS, math.ceil))


def to_canonical(px: Sequence[float], rs: Sequence[float], par: float) -> tuple:
    """A pixel rectangle at the render scale rs, in canonical coordinates."""
    return (px[0] * par / rs[0], px[1] / rs[1], px[2] * par / rs[0], px[3] / rs[1])


def intersect(a: Sequence[float], b: Sequence[float]) -> tuple:
    """The intersection of two rectangles. A NaN bound of a gives b's, as hostDemo's maxD/minD."""
    def hi(x, y):
        return x if x > y else y

    def lo(x, y):
        return x if x < y else y
    return (hi(a[0], b[0]), hi(a[1], b[1]), lo(a[2], b[2]), lo(a[3], b[3]))


def union(a: Sequence[float] | None, b: Sequence[float]) -> tuple:
    """The bounding box of two rectangles; a may be None (nothing yet): b."""
    if a is None:
        return tuple(b)
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def contains(a: Sequence[float], b: Sequence[float]) -> bool:
    """Is b inside a?"""
    return a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]


def is_empty(r: Sequence[float]) -> bool:
    return r[2] <= r[0] or r[3] <= r[1]


def fits(b: Sequence[int], bytes_per_pixel: int = 16) -> bool:
    """Can hostDemo make an image of these pixel bounds (imageFits)? At most 2^28 pixels, and rows
    of less than 2^31 bytes (default: float RGBA)."""
    w, h = b[2] - b[0], b[3] - b[1]
    return (w <= MAX_IMAGE_PIXELS and h <= MAX_IMAGE_PIXELS and w * h <= MAX_IMAGE_PIXELS
            and w * bytes_per_pixel <= 0x7fffffff)
