"""Tester (GenericTest): the progress calls of the Support library, from the host's report.

Pattern: what the host received, read from hostDemo's report (report["progress"]: one record
per progressStart, with its updates and whether it was ended). Tester's render calls
progressStart, progressUpdate(0.5), progressEnd, then progressUpdate(1) and progressEnd again.
The OFX progress suite says of progressEnd: "\\post - you can no longer call progressUpdate on
the instance", so the Support library must not pass the calls made after progressEnd to the
host.
"""

import numpy as np

PLUGIN = "net.sf.openfx.GenericTestPlugin"


def test_no_progress_update_after_end(render):
    src = np.full((8, 8, 3), 0.25, dtype=np.float32)
    r = render(PLUGIN, inputs={"Source": src})
    progress = r.report["progress"]
    assert len(progress) == len(r.outputs) > 0  # one progress per rendered frame
    for p in progress:
        assert (p["message"], p["id"]) == ("Tester render", "TesterRenderProgress")
        assert p["ended"]
        # only the update before progressEnd reached the host
        assert (p["updates"], p["lastValue"]) == (1, 0.5)
