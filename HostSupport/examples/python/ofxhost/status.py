"""OFX status codes (ofxh.Stat) as hostDemo reports them."""

from __future__ import annotations

from .build import import_ofxh

__all__ = ["name", "succeeded"]

ofxh = import_ofxh()


def name(st) -> str:
    """The status's C name, as HostSupport's StatStr: "kOfxStatOK"..., or "(unknown error code)".

    StatStr names the codes of OFX 1.4 only (kOfxStatOK to kOfxStatReplyDefault).
    """
    v = int(st)
    if ofxh.Stat.OK <= v <= ofxh.Stat.ReplyDefault:
        return "kOfxStat" + ofxh.Stat(v).name
    return "(unknown error code)"


def succeeded(st) -> bool:
    """Did an action succeed? OK, or ReplyDefault (the plug-in doesn't trap the action)."""
    return int(st) in (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)
