# DRAFT: hostDemo as a plug-in test host, and Python bindings (`ofxhost`)

Status: design proposal, not implemented. For NatronGitHub/openfx; it doesn't change the OFX API.

## Goal

Let the plug-in repositories (openfx-misc, openfx-io, openfx-arena, openfx-gmic, and SupportExt-based plug-ins) test their plug-ins without Natron:
- render with chosen inputs, parameters, times and scales;
- check pixels, RoD, RoI, frames needed and identity;
- run in CI on the three per-PR builds.

Today, after #96 and #99, hostDemo can:
- choose the plug-in, context and parameter values;
- render 2 frames of a synthetic 720×576 RGBA 8-bit clip;
- write PPM files.

Its gaps for testing:
- only one input type (synthetic RGBA byte) and one size;
- no float/short depths, Alpha or RGB components;
- no animation, time ranges, render scale or render window;
- it doesn't call isIdentity, getRegionOfDefinition, getFramesNeeded, getTimeDomain or instanceChanged;
- the multithread suite runs on one thread (HostSupport's fallback without `OFX_SUPPORTS_MULTITHREAD`), so thread-safety bugs never show;
- output is 8-bit text PPM, and there are no machine-readable results.

## Part A: small hostDemo capabilities (C++, one PR each)

| # | Capability | Options (sketch) | Tests what |
|---|---|---|---|
| A1 | **Real inputs** per clip: PPM/PGM and PFM files, or patterns (`solid:r,g,b,a`, `ramp`, `checker:N`, `noise:seed`), with a size and RoD | `-i Source=ramp:1920x1080`, `-i Mask=file.pfm` | filters, generators, transitions, masks, odd RoDs |
| A2 | **Pixel depths and components**: byte, short, float; RGBA, RGB, Alpha. Follows the clip preferences the plug-in asks for (`getClipPreferences`) | `--depth float --components RGB` | per-depth code paths, clip preference negotiation |
| A3 | **Exact output**: PFM (float) and 16-bit PPM. `--stats` prints min/max/mean per channel and a checksum, so CI can assert without reference files | `-o out.pfm --stats` | regressions, bit-exactness |
| A4 | **Time and scale**: frame range, frame rate, render scale, render window, project size and PAR, field | `-t 0-24 --scale 0.5 --window 0,0,256,256` | multi-resolution, tiles, temporal plug-ins |
| A5 | **Animated parameters**: keys with linear interpolation, plus getDerivative/integrate | `blur@0=1 blur@24=10` | animation-dependent code, frames needed |
| A6 | **Action report**: call and print, as JSON, isIdentity, RoD, RoI, frames needed, time domain, clip preferences, and instanceChanged when a parameter is set after create | `--report actions.json` | the non-render actions, often the buggiest |
| A7 | **Messages and progress**: print persistent messages, progress and abort. `--fail-on-error-message` gives a non-zero exit | — | error paths |
| A8 | **Threads**: a real multithread suite (N std::thread workers), plus `--threads N` and `--repeat N` for timing | `--threads 8 --repeat 10` | thread safety, performance |
| A9 | **Describe as JSON**: params (type, default, range, choices), clips, contexts and flags | `--describe` | generating test matrices, docs |
| A10 *(later)* | **Headless overlays**: a DrawSuite (OFX 1.5) that records draw calls, plus scripted pen and key events. Also checks the interact cursor (#63) | `--overlay events.txt` | interacts without a GUI |

A1–A3 and A6 matter most. Each is a few hundred lines in `HostSupport/examples/` and doesn't touch the library.

## Part B: Python bindings

### Layering
1. **`ofxDemoHost` library**: move the hostDemo classes (host descriptor, effect, clip and param instances) out of `hostDemo.cpp`, behind a small C++ façade:
   ```cpp
   Host host; host.scan(paths);              // the plug-in cache
   Effect fx = host.create(id, context);      // describe + create instance
   fx.setParam("value", {0.5,0.5,0.5,0.5});
   fx.setInput("Source", Image{w,h,depth,comps,data,rowBytes,rod});
   Image out = fx.render(time, window, scale);
   Report r = fx.rod(time); fx.isIdentity(...); fx.framesNeeded(...)
   ```
   hostDemo.cpp becomes a thin CLI over it, so the CLI and Python use the same code. HostSupport itself stays C++98. The façade and bindings are C++17 and optional.
2. **Python module `ofxhost`**: bindings over the façade. Images are NumPy arrays (H×W×C; uint8/uint16/float32), with zero-copy input through the buffer protocol. Parameters are a dict-like view. Each action is a method, and the reports come back as dicts.
3. **`hostDemo.py`**: the Python CLI (same options as Part A), plus `ofxhost.testing` pytest helpers:
   - `render(...)`;
   - `assert_close(img, ref, atol)`;
   - fixtures to load a bundle;
   - running each test in a subprocess, so a crashing plug-in doesn't take down pytest.

```python
import numpy as np, ofxhost
host = ofxhost.Host(["Gamma/Darwin-64-release"])
fx = host.create("net.sf.openfx.GammaPlugin", "filter")
fx.params["value"] = (2.2, 2.2, 2.2, 1.0)
src = np.random.rand(256, 256, 4).astype(np.float32)
out = fx.render(time=0, inputs={"Source": src})
assert np.allclose(out[..., :3], src[..., :3] ** (1 / 2.2), atol=1e-5)
```

### Binding technology
| Option | Pros | Cons |
|---|---|---|
| **nanobind** (recommended) | small and fast binaries, NumPy ndarray support, stable ABI wheels (Python ≥ 3.12), CMake support | C++17, Python ≥ 3.8 |
| pybind11 | most widespread, mature | bigger and slower builds; no stable ABI |
| C ABI + ctypes/cffi | no build-time Python, one binary for every Python | more hand-written glue, so it's more work to keep in sync |

Build: CMake option `BUILD_PYTHON_BINDINGS` (default OFF). The Makefiles, which the consumers use, are unchanged. `pip install .` via scikit-build-core. Python comes from:
- MacPorts on macOS (policy: no Homebrew);
- the system on Linux;
- the MSYS2 UCRT64 package on Windows.

CI: in the existing three per-PR jobs (no new job, per the policy), build the module and run a pytest smoke test: Invert, MultiplyGPU at 0.5, an RoD check.

### Constraints and risks
- **One host per process.** HostSupport uses process-wide singletons (`PluginCache::getPluginCache()`, `gImageEffectHost`), and plug-in binaries can't be unloaded safely. `ofxhost.Host` is therefore a singleton. Tests that need a fresh host run in a subprocess.
- **Crashes and exceptions.** A crashing plug-in kills the interpreter, hence the subprocess helper. C++ exceptions are translated to Python exceptions with the OFX status.
- **GIL.** The GIL is released during actions, so `--threads` and Python threads work. Host callbacks (messages, progress) take it back.
- **Scope.** This is a test host, not a production host. No GPU (OpenGL, CUDA, Metal, OpenCL) render paths at first: plug-ins get CPU rendering only.

## Plan
1. Part A, PRs A1, A2, A3, A6, A4, then A5, A7, A8, A9. hostDemo CLI only; each PR adds a CI check.
2. Extract `ofxDemoHost` and its façade; hostDemo moves onto it with no change in behaviour.
3. `ofxhost` bindings, `hostDemo.py` and `ofxhost.testing`; a smoke test in CI.
4. Consumers adopt it: e.g. openfx-misc adds pytest render tests for a few plug-ins (Gamma, Grade, Transform).
5. A10, headless overlays.

## Open questions
- nanobind or pybind11? (Proposed: nanobind.)
- Minimum Python version. Proposed: 3.10, the same as `scripts/check-props.py`.
- Is NumPy acceptable as a dependency? (Proposed: yes, as the image type.)
- Where do plug-in tests live: in each consumer repo (proposed), or a shared test repo?
- Do we also publish `ofxhost` wheels, or only build it from source?
