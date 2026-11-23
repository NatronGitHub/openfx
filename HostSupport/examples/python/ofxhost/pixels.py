"""Image arrays as hostDemo makes and reports them: depths, components, conversions, statistics.

Depths are ofxh's short names ("byte", "short", "half", "float"), components "rgba", "rgb" or
"alpha". The functions don't care whether rows are top-down or bottom-up.
"""

from __future__ import annotations

import zlib
from collections.abc import Sequence

import numpy as np

from .constants import kOfxImageOpaque, kOfxImagePreMultiplied, kOfxImageUnPreMultiplied

__all__ = ["BYTES", "NCOMPS", "check_array", "to_float", "to_rgba", "to_depth", "convert",
           "stats"]

BYTES = {"byte": 1, "short": 2, "float": 4}  # (the depths hostDemo makes: bytes per component)
NCOMPS = {"rgba": 4, "rgb": 3, "alpha": 1}
_DTYPES = {"byte": np.uint8, "short": np.uint16, "half": np.float16, "float": np.float32}
# the OFX premultiplication states and hostDemo's input spec suffixes (#premult...)
_PREMULTS = {kOfxImagePreMultiplied: "premult", kOfxImageUnPreMultiplied: "unpremult", kOfxImageOpaque: "opaque"}


def check_array(clip: str, pixels, bounds: Sequence[int] | None = None,
                premult: str | None = None) -> tuple[np.ndarray, tuple[int, int] | None, str | None]:
    """Check an array input of ofxhost.render(), as both backends take it.

    Args:
        clip: the clip name, for the messages.
        pixels: H x W, or H x W x C with C = 1, 3 or 4 (array-like).
        bounds: (x1, y1, x2, y2) of an ofxhost.Image, which must have the size of pixels, or None.
        premult: the OFX premultiplication state of an ofxhost.Image, or None.

    Returns:
        (pixels as float32, H x W or H x W x 3 or 4; the bottom-left corner (x1, y1) of bounds, or
        None; the premultiplication's short name, premult, unpremult or opaque, or None).

    Raises:
        ValueError: a bad shape, a non-finite value, an empty image, bounds of another size, or an
            unknown premultiplication.
    """
    a = np.asarray(pixels, dtype=np.float32)
    if a.ndim == 3 and a.shape[2] == 1:
        a = a[:, :, 0]
    if not (a.ndim == 2 or (a.ndim == 3 and a.shape[2] in (3, 4))):
        raise ValueError(f"input {clip}: shape {a.shape} (expected H x W, H x W x 1, H x W x 3 or H x W x 4)")
    if not np.all(np.isfinite(a)):
        raise ValueError(f"input {clip}: non-finite values")
    h, w = a.shape[:2]
    if w == 0 or h == 0:
        raise ValueError("PFM: empty image")  # (the subprocess backend's error: its PFM file can't be written)
    origin = None
    if bounds is not None:
        x1, y1, x2, y2 = (int(v) for v in bounds)
        if (x2 - x1, y2 - y1) != (w, h):
            raise ValueError(f"input {clip}: bounds {tuple(bounds)} don't match the size {w}x{h}")
        origin = (x1, y1)
    if premult is not None and premult not in _PREMULTS:
        raise ValueError(f"input {clip}: premultiplication {premult!r}, expected one of {sorted(_PREMULTS)}")
    return a, origin, None if premult is None else _PREMULTS[premult]


def to_float(pixels: np.ndarray) -> np.ndarray:
    """float32 values, as hostDemo's getComponentFloat: byte and short divided by 255 and 65535."""
    if pixels.dtype == np.uint8:
        return pixels.astype(np.float32) / np.float32(255)
    if pixels.dtype == np.uint16:
        return pixels.astype(np.float32) / np.float32(65535)
    return pixels.astype(np.float32)


def to_rgba(pixels: np.ndarray) -> np.ndarray:
    """Any image as RGBA, in float's range, as hostDemo's PFM inputs give it.

    Args:
        pixels: H x W, or H x W x C with C = 1, 3 or 4; byte, short, half or float.

    Returns:
        H x W x 4: RGB is opaque (alpha 1), one component v gives (v, v, v, v). float32 for a
        float32 image (the same values), else float64 (byte and short divided by their maximum).
    """
    if pixels.dtype == np.float32:
        a = pixels
    else:
        a = pixels.astype(np.float64)
        if pixels.dtype == np.uint8:
            a /= 255.
        elif pixels.dtype == np.uint16:
            a /= 65535.
    if a.ndim == 2:
        a = a[:, :, None]
    if a.shape[2] == 1:
        return np.repeat(a, 4, axis=2)
    if a.shape[2] == 3:
        return np.dstack([a, np.ones(a.shape[:2], a.dtype)])
    return a


def to_depth(rgba: np.ndarray, depth: str, comps: str) -> np.ndarray:
    """An RGBA image at a clip's depth and components, as hostDemo makes its images.

    Args:
        rgba: H x W x 4, in float's range.
        depth: "byte", "short" or "float" (BYTES).
        comps: "rgba", "rgb" (the alpha dropped) or "alpha" (the alpha kept).

    Returns:
        A new contiguous array: float unchanged; byte and short clamped to [0, 1] (NaN gives 0),
        scaled and rounded.
    """
    v = rgba[..., 3:4] if comps == "alpha" else rgba[..., :NCOMPS[comps]]
    if depth == "float":
        return np.ascontiguousarray(v, dtype=np.float32)
    v = np.asarray(v, np.float64)
    c = np.where(np.isnan(v) | (v <= 0), 0., np.minimum(v, 1.))
    if depth == "byte":
        return np.ascontiguousarray(np.floor(c * 255. + 0.5), dtype=np.uint8)
    return np.ascontiguousarray(np.floor(c * 65535. + 0.5), dtype=np.uint16)


def convert(pixels: np.ndarray, depth: str, comps: str) -> np.ndarray:
    """An image at a clip's depth (byte, short, half or float) and components; the same array if it
    already is."""
    if pixels.dtype == _DTYPES.get(depth) and pixels.ndim == 3 and pixels.shape[2] == NCOMPS.get(comps):
        return pixels
    if depth == "half":
        return np.ascontiguousarray(to_depth(to_rgba(pixels), "float", comps), np.float16)
    return to_depth(to_rgba(pixels), depth, comps)


def stats(pixels: np.ndarray, bounds: Sequence[int], files: Sequence[str] = ()) -> dict:
    """The statistics of an output image, as hostDemo's report gives them (computeStats, statsJson).

    Args:
        pixels: H x W x C, byte, short or float.
        bounds: its pixel bounds.
        files: the files it was written to.

    Returns:
        {w, h, bounds, depth, comps, min, max, mean (per component, NaN left out; None if all are
        NaN), nan (the pixels with a NaN), crc32 (of the little-endian pixels), files}.
    """
    h, w, nc = pixels.shape
    j: dict = {"w": bounds[2] - bounds[0], "h": bounds[3] - bounds[1], "bounds": list(bounds),
               "depth": {1: "byte", 2: "short", 4: "float"}[pixels.dtype.itemsize],
               "comps": {4: "RGBA", 3: "RGB", 1: "Alpha"}[nc]}
    v = pixels.reshape(-1, nc).astype(np.float64)
    nan = np.isnan(v)
    mins, maxs, means = [], [], []
    for c in range(nc):
        x = v[~nan[:, c], c]
        if x.size == 0:
            mins.append(None)
            maxs.append(None)
            means.append(None)
        else:
            mins.append(float(x.min()))
            maxs.append(float(x.max()))
            means.append(float(x.sum() / x.size))
    j["min"], j["max"], j["mean"] = mins, maxs, means
    j["nan"] = int(nan.any(axis=1).sum()) if nc else 0
    data = np.ascontiguousarray(pixels, dtype=pixels.dtype.newbyteorder("<"))
    j["crc32"] = f"{zlib.crc32(data.tobytes()) & 0xffffffff:08x}"
    j["files"] = list(files)
    return j
