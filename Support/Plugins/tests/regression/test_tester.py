"""Tester (GenericTest): the progress calls of the Support library, from the host's report.

Pattern: what the host received, read from hostDemo's report (report["progress"]: one record
per progressStart, with the number of updates and of progressEnd calls). Each render of Tester
calls progressStart, progressUpdate(0.5), progressEnd, then progressUpdate(1) and progressEnd
again. The OFX progress suite says of progressEnd: "\\post - you can no longer call
progressUpdate on the instance", so the Support library must not pass the calls made after
progressEnd to the host. Rendering several frames also checks that a progressStart after a
progressEnd starts a new progress.
"""

import numpy as np

PLUGIN = "net.sf.openfx.GenericTestPlugin"


def test_no_progress_update_after_end(render):
    """Only the progressUpdate and progressEnd before the first progressEnd reach the host, and each render starts a new
    progress ("you can no longer call progressUpdate on the instance" after progressEnd, ofxProgress.h). Before #206,
    Support passed the calls made after progressEnd to the host, and didn't end a Vegas progress. (#206)"""
    src = np.full((8, 8, 3), 0.25, dtype=np.float32)
    r = render(PLUGIN, inputs={"Source": src}, frames=[0, 1, 2])
    progress = r.report["progress"]
    # one progress per render call (Tester supports tiles: count the renders, not the outputs)
    assert len(progress) == len(r.report["renders"]) >= 2
    for p in progress:
        assert (p["message"], p["id"]) == ("Tester render", "TesterRenderProgress")
        # only the update and the end before the first progressEnd reached the host
        assert (p["ended"], p["ends"]) == (True, 1)
        assert (p["updates"], p["lastValue"]) == (1, 0.5)
