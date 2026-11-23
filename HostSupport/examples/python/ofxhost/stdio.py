"""The process's stdout and stderr at the file-descriptor level, where plug-ins write (printf,
std::cerr): flushing the Python and C buffers, and redirecting the descriptors 1 and 2.

The descriptors and the C buffers are process-wide: a redirection also catches what other threads
write meanwhile.
"""

from __future__ import annotations

import contextlib
import ctypes
import functools
import os
import sys
from collections.abc import Iterator, Mapping

__all__ = ["flush", "write", "redirect"]


@functools.cache
def _c_fflush():
    """The C runtime's fflush (ctypes), or None if there is none."""
    try:
        lib = ctypes.CDLL("ucrtbase") if sys.platform == "win32" else ctypes.CDLL(None)
        f = lib.fflush
    except (OSError, AttributeError):
        return None
    f.argtypes = [ctypes.c_void_p]
    return f


def flush() -> None:
    """Flush sys.stdout and sys.stderr, then the C stdio buffers (fflush(NULL)): the plug-ins'
    output, and ours, goes out in order before a descriptor moves."""
    for f in (sys.stdout, sys.stderr):
        try:
            f.flush()
        except (AttributeError, ValueError, OSError):
            pass
    fflush = _c_fflush()
    if fflush is not None:
        fflush(None)


def write(fd: int, text: str) -> None:
    """Write text to a file descriptor, after the buffers (in the order hostDemo's output has)."""
    flush()
    data = text.encode("utf-8", "replace")
    while data:
        n = os.write(fd, data)
        data = data[n:]


@contextlib.contextmanager
def redirect(targets: Mapping[int, int]) -> Iterator[None]:
    """Redirect file descriptors (e.g. 1 and 2) to others for the duration, flushing before and after.

    Args:
        targets: descriptor -> the open descriptor it writes to meanwhile.
    """
    saved: dict[int, int] = {}
    try:
        flush()
        for fd, target in targets.items():
            saved[fd] = os.dup(fd)
            os.dup2(target, fd)
        yield
    finally:
        flush()
        for fd, d in saved.items():
            os.dup2(d, fd)
            os.close(d)
