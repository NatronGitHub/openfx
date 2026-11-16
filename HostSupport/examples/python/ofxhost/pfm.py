"""Read and write PFM (Portable Float Map) images as NumPy arrays.

PFM rows are stored "in left to right, bottom to top order"
(https://www.pauldebevec.com/Research/HDR/PFM/), and the sign of the scale gives
the byte order: negative is little-endian, positive is big-endian. "PF" has
three channels (RGB), "Pf" has one (grey).

The arrays are top-down (row 0 is the top row of the image), float32:
H x W x 3 for PF, H x W for Pf.
"""

from __future__ import annotations

import os

import numpy as np

__all__ = ["read_pfm", "write_pfm", "parse_pfm", "encode_pfm"]


def _token(data: bytes, pos: int) -> tuple[bytes, int]:
    """Return the next whitespace-separated header token and the position after it."""
    n = len(data)
    while pos < n and data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while pos < n and not data[pos:pos + 1].isspace():
        pos += 1
    if start == pos:
        raise ValueError("PFM: truncated header")
    return data[start:pos], pos


def parse_pfm(data: bytes) -> np.ndarray:
    """Decode a PFM file's bytes into a top-down float32 array."""
    ident, pos = _token(data, 0)
    if ident == b"PF":
        channels = 3
    elif ident == b"Pf":
        channels = 1
    else:
        raise ValueError(f"PFM: bad identifier {ident!r} (expected PF or Pf)")
    w_tok, pos = _token(data, pos)
    h_tok, pos = _token(data, pos)
    s_tok, pos = _token(data, pos)
    try:
        width, height, scale = int(w_tok), int(h_tok), float(s_tok)
    except ValueError as e:
        raise ValueError(f"PFM: bad header ({e})") from None
    if width <= 0 or height <= 0:
        raise ValueError(f"PFM: bad size {width}x{height}")
    if scale == 0.0 or not np.isfinite(scale):
        raise ValueError(f"PFM: bad scale {s_tok!r}")
    # exactly one whitespace character ends the header
    if pos >= len(data) or not data[pos:pos + 1].isspace():
        raise ValueError("PFM: truncated header")
    pos += 1
    dtype = np.dtype("<f4" if scale < 0 else ">f4")
    count = width * height * channels
    expected = count * dtype.itemsize
    if len(data) - pos != expected:
        raise ValueError(f"PFM: {len(data) - pos} bytes of pixel data, expected {expected}")
    pixels = np.frombuffer(data, dtype=dtype, count=count, offset=pos)
    shape = (height, width, channels) if channels == 3 else (height, width)
    # bottom-up in the file: flip to top-down, and convert to native float32
    return np.ascontiguousarray(pixels.reshape(shape)[::-1], dtype=np.float32)


def read_pfm(path: str | os.PathLike) -> np.ndarray:
    """Read a PFM file into a top-down float32 array (H x W x 3, or H x W for Pf)."""
    with open(path, "rb") as f:
        return parse_pfm(f.read())


def encode_pfm(image: np.ndarray) -> bytes:
    """Encode a top-down image as little-endian PFM bytes.

    H x W or H x W x 1 gives Pf, H x W x 3 gives PF.
    """
    a = np.asarray(image)
    if a.ndim == 3 and a.shape[2] == 1:
        a = a[:, :, 0]
    if a.ndim == 2:
        ident = b"Pf"
    elif a.ndim == 3 and a.shape[2] == 3:
        ident = b"PF"
    else:
        raise ValueError(f"PFM: can't write an image of shape {a.shape} "
                         "(expected H x W, H x W x 1 or H x W x 3)")
    height, width = a.shape[:2]
    if width == 0 or height == 0:
        raise ValueError("PFM: empty image")
    header = b"%s\n%d %d\n-1.0\n" % (ident, width, height)
    return header + np.ascontiguousarray(a[::-1], dtype="<f4").tobytes()


def write_pfm(path: str | os.PathLike, image: np.ndarray) -> None:
    """Write a top-down image as a little-endian PFM file (rows bottom-up)."""
    data = encode_pfm(image)
    with open(path, "wb") as f:
        f.write(data)
