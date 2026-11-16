# ofxhost: render OFX plug-ins from Python

A pure-Python test wrapper around the demo host `hostDemo` (HostSupport/examples), run as a
subprocess. Each render runs in its own process and temporary directory: HostSupport has
process-wide singletons, plug-ins can't be unloaded, and a crashing plug-in must not take
down the interpreter. Images are exchanged as PFM files. Needs Python 3.10 or later and
NumPy (and pytest for the tests). See Documentation/proposals/hostdemo-testing.md, Part B.

```python
import numpy as np, ofxhost
src = np.random.default_rng(0).random((48, 64, 3), dtype=np.float32)
r = ofxhost.render("Support/Plugins/Invert/Linux-64-release", "net.sf.openfx.invertPlugin",
                   inputs={"Source": src}, depth="float")
assert np.allclose(r.outputs[0][..., :3], 1 - src, atol=1e-6)
```

- `render(bundle_paths, plugin_id, context="filter", inputs=None, params=None, depth="float",
  frames=None, host_binary=None, timeout=60, extra_args=(), check=True)` runs hostDemo with
  `--no-std-paths --no-cache --quiet --format pfm --stats`, and `OFX_PLUGIN_PATH` set from
  `bundle_paths` (directories, or `.ofx.bundle` paths, which stand for their parent directory).
  - `inputs`: clip name to a NumPy array (H×W, H×W×1, H×W×3, or H×W×4 with alpha 1; top-down;
    written to a PFM file), a `pathlib.Path` or `"file:x.pfm"`, or a hostDemo spec:
    `"none"` (disconnected), `"default"`, `"solid:r,g,b,a"`, `"ramp"`, `"checker:N"`.
    hostDemo's PFM input is RGB with alpha 1, or grey with r=g=b=a.
  - `params`: name to value (number, bool, string, or a sequence), passed as `-s name=value`.
  - `frames`: keep only these frames (hostDemo renders frames 0 and 1).
  - A non-zero exit or a timeout raises `HostDemoError` (with the command, stdout and stderr),
    unless `check=False`.
- The result has `.outputs` (frame to float32 H×W×C, top-down; RGBA outputs merge the
  separate alpha file; stereo builds key by `(frame, "l"|"r")`), `.output` (the first frame),
  `.alpha` (frame to H×W), `.stats` (the parsed `--stats` lines), `.report` (the
  `--report` JSON, or None), `.returncode`, `.stdout`, `.stderr` and `.cmd`.
- `describe(bundle_paths, plugin_id)` returns the `--describe` JSON.
- `--report` and `--describe` are used only if `hostDemo -h` lists them
  (`host_features()`); otherwise `.report` is None and the `--stats` lines are the results.
- `read_pfm` / `write_pfm`: top-down float32 arrays; files are bottom-up, little-endian
  when written, either byte order when read.
- hostDemo is found from the `host_binary` argument, then `$OFXHOST_HOSTDEMO`, then the build
  tree (`HostSupport/examples/*-release/hostDemo`, then CMake `build*/HostSupport/examples`).

`ofxhost.testing` has `assert_close(actual, expected, atol, rtol)`, which reports the number
of mismatches, the largest difference and where, and a `hostdemo` pytest fixture that skips
the tests when hostDemo isn't built.

`hostDemo.py` is a small command line wrapper: it saves the outputs as `.npy` or PFM, and
prints the stats and report as JSON (`python hostDemo.py -h`).

## Tests

```sh
make -C HostSupport && make -C HostSupport/examples && make -C Support/Plugins CONFIG=release
python3 -m venv /tmp/ofxhost-venv && /tmp/ofxhost-venv/bin/pip install numpy pytest
/tmp/ofxhost-venv/bin/python -m pytest -q -p no:cacheprovider HostSupport/examples/python/tests
```

CI runs them in the Linux job.
