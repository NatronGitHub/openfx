"""Image files for the sample host: PFM and NumPy .npy, as float32 arrays H x W x C, top-down.

- .pfm: PF (RGB) or Pf (grey, read as one component). As hostDemo writes them, the alpha of an RGBA
  image is in a separate Pf file next to it, <name>.alpha.pfm: write_image() writes it, and
  read_image() reads it back when it exists.
- .npy: H x W, or H x W x C with C = 1, 3 or 4; float32, float64, uint8 (/255) or uint16 (/65535).

Other formats (PNG, EXR...) would go here; the hosts and tests use these two, which keep the exact
float values.
"""

from __future__ import annotations

import os

import numpy as np

from .pfm import read_pfm, write_pfm

__all__ = ["read_image", "write_image", "alpha_path", "EXTENSIONS"]

EXTENSIONS = (".pfm", ".npy")


def alpha_path(path: str) -> str:
    """The separate alpha file of a PFM file: x.pfm -> x.alpha.pfm."""
    return path[:-4] + ".alpha.pfm"


def _ext(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext not in EXTENSIONS:
        raise ValueError(f"{path}: unsupported image format {ext or '(none)'} (supported: {', '.join(EXTENSIONS)})")
    return ext


def read_image(path: str | os.PathLike) -> np.ndarray:
    """A float32 array H x W x C (C = 1, 3 or 4), top-down."""
    path = os.fspath(path)
    if _ext(path) == ".pfm":
        a = read_pfm(path)
        if a.ndim == 2:
            return a[:, :, None]
        if os.path.isfile(alpha_path(path)):
            alpha = read_pfm(alpha_path(path))
            if alpha.shape != a.shape[:2]:
                raise ValueError(f"{alpha_path(path)}: its size isn't that of {path}")
            a = np.dstack([a, alpha])
        return np.ascontiguousarray(a, np.float32)
    a = np.load(path, allow_pickle=False)
    if a.ndim == 2:
        a = a[:, :, None]
    if a.ndim != 3 or a.shape[2] not in (1, 3, 4):
        raise ValueError(f"{path}: shape {a.shape} (expected H x W, or H x W x C with C = 1, 3 or 4)")
    scale = {np.dtype(np.uint8): 255., np.dtype(np.uint16): 65535.}.get(a.dtype)
    a = a.astype(np.float32)
    return a / np.float32(scale) if scale else a


def write_image(path: str | os.PathLike, pixels: np.ndarray) -> list[str]:
    """Write a top-down image (H x W x C); returns the files written (an RGBA PFM image has two)."""
    path = os.fspath(path)
    ext = _ext(path)
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    a = np.asarray(pixels, np.float32)
    if ext == ".npy":
        np.save(path, a)
        return [path]
    if a.ndim == 3 and a.shape[2] == 4:
        write_pfm(path, a[:, :, :3])
        write_pfm(alpha_path(path), a[:, :, 3])
        return [path, alpha_path(path)]
    write_pfm(path, a)
    return [path]
