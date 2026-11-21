"""File names with frame-number patterns, for image sequences (as Natron reads and writes them).

    expand("render.####.pfm", 7)                 # "render.0007.pfm"
    expand("shot_%03d_%V.npy", 12, "left")       # "shot_012_left.npy"
    expand("100%%_%d.pfm", 3)                     # "100%_3.pfm"
    has_pattern("a.%%d.pfm")                      # False: "%%" is a literal "%"
    frames("in.####.pfm")                         # [1, 2, 3]: the frames of the files that exist

The patterns are those of Natron's SequenceParsing::generateFileNameFromPattern():
- a run of N "#": the frame number, zero-padded to N digits;
- "%d": the frame number; "%0Nd": zero-padded to N digits;
- "%V": the view name; "%v": its first letter, upper case (as Natron);
- "%%": a literal "%" (any other "%" is kept as it is).
Negative frames are formatted as printf does: "####" and "%04d" of -7 give "-007" (Natron's
gives "00-7"). The frame of a time t is floor(t + 0.5), as Natron's paramGetValueAtTime of a
file parameter (OfxStringInstance::get(time, str)).
"""

from __future__ import annotations

import math
import os
import re

__all__ = ["has_pattern", "expand", "frame_of", "frames"]

# the tokens, in the order they are tried at each position
_TOKEN = re.compile(r"%%|%0(\d+)d|%d|%v|%V|#+")


def frame_of(time: float) -> int:
    """The frame number of a time: floor(t + 0.5)."""
    return int(math.floor(float(time) + 0.5))


def _tokens(pattern: str):
    """(kind, text, digits) for each part of the pattern: kind is "text", "frame" or "view"."""
    pos = 0
    for m in _TOKEN.finditer(pattern):
        if m.start() > pos:
            yield "text", pattern[pos:m.start()], 0
        t = m.group(0)
        if t == "%%":
            yield "text", "%", 0
        elif t.startswith("#"):
            yield "frame", t, len(t)
        elif t == "%d":
            yield "frame", t, 0
        elif t in ("%v", "%V"):
            yield "view", t, 0
        else:
            yield "frame", t, int(m.group(1))
        pos = m.end()
    if pos < len(pattern):
        yield "text", pattern[pos:], 0


def has_pattern(pattern: str) -> bool:
    """Does the name have a frame number or a view in it?"""
    return any(kind != "text" for kind, _, _ in _tokens(pattern))


def _view(token: str, view_name: str) -> str:
    return view_name if token == "%V" else view_name[:1].upper()


def expand(pattern: str, frame: int, view_name: str = "main") -> str:
    """The file name of a frame (an int, see frame_of()) and view."""
    out = []
    for kind, text, digits in _tokens(pattern):
        if kind == "frame":
            out.append(f"{int(frame):0{digits}d}" if digits else str(int(frame)))
        elif kind == "view":
            out.append(_view(text, view_name))
        else:
            out.append(text)
    return "".join(out)


def frames(pattern: str, view_name: str = "main") -> list[int]:
    """The frames of the existing files of a sequence, in order ([] if there are none; [0] for a
    name without a frame number, if the file exists). Only the file name may have frame numbers,
    not the directory (ValueError)."""
    directory, name = os.path.split(pattern)
    if any(kind == "frame" for kind, _, _ in _tokens(directory)):
        raise ValueError(f"{pattern}: frame numbers are only supported in the file name, not the directory")
    directory = expand(directory, 0, view_name)
    if not any(kind == "frame" for kind, _, _ in _tokens(name)):
        return [0] if os.path.isfile(os.path.join(directory, expand(name, 0, view_name))) else []
    regex = "".join(re.escape(_view(text, view_name)) if kind == "view" else
                    r"(-?\d+)" if kind == "frame" else re.escape(text)
                    for kind, text, _ in _tokens(name))
    found = set()
    try:
        names = os.listdir(directory or ".")
    except OSError:
        return []
    for n in names:
        m = re.fullmatch(regex, n)
        if not m:
            continue
        numbers = {int(g) for g in m.groups()}
        # the same frame everywhere, written as the pattern writes it (e.g. not 07 for ####)
        if len(numbers) == 1:
            f = numbers.pop()
            if expand(name, f, view_name) == n:
                found.add(f)
    return sorted(found)
