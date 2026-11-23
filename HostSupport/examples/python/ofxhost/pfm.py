"""Read and write PFM (Portable Float Map) images as NumPy arrays.

PFM rows are stored "in left to right, bottom to top order"
(https://www.pauldebevec.com/Research/HDR/PFM/), and the sign of the scale gives
the byte order: negative is little-endian, positive is big-endian. "PF" has
three channels (RGB), "Pf" has one (grey). hostDemo keeps the alpha of an RGBA
image in a separate Pf file, <name>.alpha.pfm (alpha_path()).

The arrays are top-down (row 0 is the top row of the image), float32:
H x W x 3 for PF, H x W for Pf.
"""

from __future__ import annotations

import os

import numpy as np

__all__ = ["read_pfm", "write_pfm", "parse_pfm", "encode_pfm", "alpha_path", "split_alpha", "merge_alpha",
           "write_pfm_files"]

# hostDemo's readPFM limits (strict=True)
_MAX_SIDE = 65536
_MAX_PIXELS = 1 << 24
_MAX_TOKEN = 64


def _token(data: bytes, pos: int) -> tuple[str, int]:
    """The next whitespace-separated header token (at most 64 bytes, as hostDemo's), and the position after it."""
    n = len(data)
    while pos < n and data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while pos < n and not data[pos:pos + 1].isspace() and pos - start < _MAX_TOKEN:
        pos += 1
    return data[start:pos].decode("latin-1"), pos


def parse_pfm(data: bytes, *, strict: bool = False) -> np.ndarray:
    """Decode a PFM file's bytes into a top-down float32 array.

    Args:
        data: the file's bytes.
        strict: also apply hostDemo's readPFM rules: little-endian only, finite values only, at
            most 65536 pixels wide or high and 2^24 pixels in all.

    Returns:
        H x W x 3 for PF, H x W for Pf, float32, top-down.

    Raises:
        ValueError: not a valid PFM file (hostDemo's messages).
    """
    from .specs import parse_double, parse_int  # (hostDemo's number parsing; specs imports this module)
    if len(data) < 3 or data[0:1] != b"P" or data[1:2] not in (b"F", b"f") or not data[2:3].isspace():
        raise ValueError("not a PFM file (it must start with PF or Pf)")
    channels = 3 if data[1:2] == b"F" else 1
    ws, pos = _token(data, 2)
    hs, pos = _token(data, pos)
    w, h = parse_int(ws), parse_int(hs)
    if w is None or h is None or w <= 0 or h <= 0 or (
            strict and (w > _MAX_SIDE or h > _MAX_SIDE or w * h > _MAX_PIXELS)):
        raise ValueError(f'invalid PFM size "{ws} {hs}" (the width and height must be positive'
                         + (", at most 65536, and at most 2^24 pixels)" if strict else ")"))
    ss, pos = _token(data, pos)
    scale = parse_double(ss)
    if scale is None or scale == 0 or (strict and scale > 0):
        raise ValueError(f'invalid PFM scale "{ss}" (it must be '
                         + ("negative: only little-endian PFM files are supported)" if strict else
                            "a non-zero decimal number, not subnormal)"))
    # exactly one whitespace character ends the header
    if pos >= len(data) or not data[pos:pos + 1].isspace():
        raise ValueError("no whitespace after the PFM scale")
    pos += 1
    expected = w * h * channels * 4
    found = len(data) - pos
    if found != expected:
        raise ValueError(f"a {w}x{h} {'PF' if channels == 3 else 'Pf'} file has {expected} bytes of pixel data, "
                         f"but there are {found} {'(truncated)' if found < expected else '(trailing data)'}")
    pixels = np.frombuffer(data, dtype="<f4" if scale < 0 else ">f4", count=w * h * channels, offset=pos)
    if strict:
        bad = np.flatnonzero(~np.isfinite(pixels))
        if bad.size:
            i = int(bad[0])
            pixel = i // channels
            raise ValueError(f"the value of channel {i % channels} of pixel ({pixel % w},{pixel // w}) (x,y from "
                             "the bottom left) is not finite")
    shape = (h, w, channels) if channels == 3 else (h, w)
    # bottom-up in the file: flip to top-down, and convert to native float32
    return np.ascontiguousarray(pixels.reshape(shape)[::-1], dtype=np.float32)


def read_pfm(path: str | os.PathLike, *, strict: bool = False) -> np.ndarray:
    """Read a PFM file into a top-down float32 array.

    Args:
        path: the file.
        strict: as parse_pfm().

    Returns:
        H x W x 3 for PF, H x W for Pf.

    Raises:
        OSError: the file can't be read.
        ValueError: not a valid PFM file ("<path>: <reason>").
    """
    with open(path, "rb") as f:
        data = f.read()
    try:
        return parse_pfm(data, strict=strict)
    except ValueError as e:
        raise ValueError(f"{os.fspath(path)}: {e}") from None


def encode_pfm(image: np.ndarray) -> bytes:
    """Encode a top-down image as little-endian PFM bytes.

    Args:
        image: H x W or H x W x 1 (written as Pf), or H x W x 3 (PF).

    Returns:
        The file's bytes.
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
    """Write a top-down image as a little-endian PFM file (see encode_pfm())."""
    data = encode_pfm(image)
    with open(path, "wb") as f:
        f.write(data)


def alpha_path(path: str) -> str:
    """The separate alpha file of a PFM file, as hostDemo names it: x.pfm -> x.alpha.pfm."""
    return path[:-4] + ".alpha.pfm"


def split_alpha(image: np.ndarray) -> tuple[np.ndarray, np.ndarray | None]:
    """Split an image into what hostDemo's PFM files hold.

    Args:
        image: H x W, or H x W x C with C = 1, 3 or 4.

    Returns:
        (colour, alpha): the PF (H x W x 3) or Pf (H x W) image, and the alpha (H x W) of an RGBA
        image, else None.
    """
    a = np.asarray(image)
    if a.ndim == 3 and a.shape[2] == 1:
        return a[:, :, 0], None
    if a.ndim == 3 and a.shape[2] == 4:
        return a[:, :, :3], a[:, :, 3]
    return a, None


def merge_alpha(colour: np.ndarray, alpha: np.ndarray | None) -> np.ndarray:
    """The inverse of split_alpha(): H x W x C, with C = 1 (Pf), 3 (PF), or 4 (PF and an alpha)."""
    if colour.ndim == 2:
        return colour[:, :, None]
    return colour if alpha is None else np.dstack([colour, alpha])


def write_pfm_files(path: str | os.PathLike, image: np.ndarray) -> list[str]:
    """Write an image as hostDemo writes it: PF or Pf, and the alpha of RGBA in alpha_path(path).

    Args:
        path: the colour file.
        image: top-down, H x W, or H x W x C with C = 1, 3 or 4.

    Returns:
        The files written.
    """
    path = os.fspath(path)
    colour, alpha = split_alpha(image)
    write_pfm(path, colour)
    if alpha is None:
        return [path]
    write_pfm(alpha_path(path), alpha)
    return [path, alpha_path(path)]
