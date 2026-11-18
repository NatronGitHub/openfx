# hostDemo as a plug-in test host, and a Python test wrapper (`ofxhost`)

Status: implemented (NatronGitHub/openfx #111–#132). hostDemo is now frozen: new host features go to the Python sample hosts on HostSupport bindings ([#136](https://github.com/NatronGitHub/openfx/issues/136)). This document is the design as it was proposed; the code, `hostDemo --help` and `HostSupport/examples/python/README.md` are the reference. It doesn't change the OFX API.

## Goal

Let the plug-in repositories (openfx-misc, openfx-io, openfx-arena, openfx-gmic, and SupportExt-based plug-ins) test their plug-ins without Natron. A test should be able to:
- render with chosen inputs, parameters, times and scales;
- check pixels, RoD, RoI, frames needed and identity;
- run in CI within the frugal policy: three per-PR builds, a nightly extended run.

It already works for debugging: hostDemo found a real ReadPSD regression from openfx-io #65 (Opaque premultiplication), comparing two builds headlessly.

## Today's hostDemo, and what's wrong with it

After #96 and #99, hostDemo:
- picks a plug-in, a context and parameter values;
- renders 2 frames of a synthetic 720×576 clip (768×576 canonical, PAR 768/720);
- writes 8-bit PPM files.

The reviews found blocking problems:
- **Images ignore clip preferences.** `MyImage` always allocates RGBA bytes (`hostDemoClipInstance.cpp`), while the clip advertises the depth and components negotiated in getClipPreferences. A plug-in that maps a clip to float or Alpha (Merge's Mask) reads the wrong data, or past the buffer. `~MyImage` uses `delete` on `new[]` memory.
- **The output image** is created once, at time 0 with fixed bounds, pre-filled with the input pattern and never cleared. Unwritten pixels look valid, and render windows, render scales and concurrent renders can't work.
- **Every clip is connected** (`getConnected()` always returns true), so masks and optional inputs can't be tested.
- **The host capabilities are hard-coded:** RGBA and Alpha only (no RGB), `SupportsMultiResolution=0`, `SupportsMultipleClipDepths=0`, `SupportsMultipleClipPARs=0`, and no reader, writer or paint contexts. Plug-ins branch on these.
- **Plug-in loading isn't deterministic.** It scans the system folders (`/Library/OFX/Plugins`, `/usr/OFX/Plugins`), reads and writes `hostDemoPluginCache.xml` in the current directory (so parallel tests race, and a stale cache can hide a rebuilt bundle), and dumps the whole cache to stdout.
- **Parameters are set before createInstance,** so `instanceChanged` never fires.
- **No parametric parameters:** `newParam` returns no instance, and `ofxhParametricParam.h` lives in Natron, not HostSupport.
- **Windows CI doesn't build hostDemo** (`BUILD_EXAMPLE_HOSTS=OFF`).

## Design principle: hostDemo lean, orchestration in Python

- **hostDemo (C++) stays small and lean: one node per run.** It renders one effect with the given inputs, at the given times and render window, and reports the actions. It only grows for what must happen inside the plug-in's process, while the plug-in runs its actions: images and clip preferences, parameter values at any time (the plug-in may query other times, e.g. for motion blur), the host's advertised capabilities, and contexts. No graph, no curve types, no sequences, no comparisons.
- **`ofxhost` / `hostDemo.py` (Python) do the orchestration,** and may be a bit richer:
  - **graph execution:** a plug-in instance graph (a DAG: a node's output may feed several inputs), built in Python or loaded from a small JSON file. The executor sorts it topologically. From the requested node, time and window, it walks the graph backwards with `--query` (RoD, regions of interest, frames needed), merging what each node is asked for (the union of the windows at each time). Then it renders each node once per needed time, inputs first, and passes each output as a positioned PFM input. Kept simple: a graph builder and an executor, nothing else;
  - **animation:** curves sampled into the linear keys that hostDemo takes;
  - frame ranges, comparisons with references, and the pytest helpers.
- `hostDemo.py` is only a sample use of that API, e.g. running a small tree described in JSON.

## Part A: hostDemo as a test host (C++, HostSupport/examples only unless noted)

PRs in this order. Each adds a CI check to the existing per-PR jobs.

1. **Image model and deterministic runs:**
   - images are allocated with each clip's mapped depth (byte, short, float) and components (RGBA, RGB, Alpha), with the bounds the action asks for (RoI ∩ RoD) and the render scale;
   - the output image is created per render, and unwritten pixels are filled with a sentinel (NaN for float, 0xCD for integer depths);
   - inputs per clip: `-i Clip=solid:r,g,b,a | ramp | checker:N | none`, where `none` means disconnected; the content depends on time without wrapping;
   - `--no-std-paths` (only OFX_PLUGIN_PATH), `--no-cache`, and `--quiet` (no cache dump, so stdout is free for results);
   - fix `delete[]`; clean exit codes;
   - CI: Invert on float RGBA, and a disconnected optional clip.
2. **Exact I/O:** PFM input and output (little-endian), with the min, max and mean per channel and a checksum of each output image in the report.
3. **Report:** `--report out.json` with the results of isIdentity, getRegionOfDefinition, getRegionsOfInterest, getFramesNeeded, getTimeDomain and getClipPreferences, plus messages and progress. `--describe` outputs JSON (params with type, default, range and options; clips; contexts; flags). Both share one JSON writer. Parameters can be set after createInstance, which fires `instanceChanged`.
4. **Node primitives for the Python tree:** `--frames t0,t1,…` (any times, fractional too), `--window x1,y1,x2,y2`, a positioned PFM input (`file:x.pfm@x1,y1`, so a child's output keeps its bounds), and `--query` (the report without rendering: RoD, regions of interest, frames needed). Render scale comes with `--window`.
5. **Animation keys:** `name@t=value`, linear interpolation only, with getDerivative/integrate. Python samples any richer curve into these keys.
6. **Host profiles:** one `--host minimal|natron-like` flag for the advertised capabilities (RGB, multi-resolution, multiple clip depths and PARs, contexts, no GPU).
7. **Readers and writers:** the reader and writer contexts (file paths are just string params), and the `OCIO` environment variable passed through.
8. **Python (Part B):** `ofxhost.Node(plugin, context, params, inputs={clip: Node | array | pattern})` and an executor, `render(node, time, window)` (DAG execution, see above); a JSON graph loader; animation helpers that produce keys; `hostDemo.py` as a sample graph runner.
9. **Later:**
   - multi-plane and multi-view (Nuke/Natron extensions);
   - concurrent host-side renders, honouring `kOfxImageEffectPluginRenderThreadSafety`. A real multithread suite needs `OFX_SUPPORTS_MULTITHREAD`, which changes HostSupport's `ImageEffect::Host` vtable (9 pure virtuals), so it waits for the suites proposal;
   - headless overlays: the DrawSuite plumbing in ofxhInteract.cpp, plus pen events and the cursor (#63);
   - host-side parametric parameters, which are a library change with their own proposal.

Builds use the same extension flags as the consumers (NATRON, NUKE...), so Natron-specific code paths (e.g. Transform's getTransform) are tested.

## Implementation status

- **Step 5 (animation keys) is implemented:** `-s name@t=value` (instanceChanged at time t); linear interpolation (rounded for integers, held for booleans and choices); the keyframe functions (paramGetNumKeys, GetKeyTime, GetKeyIndex, DeleteKey, DeleteAllKeys), paramSetValueAtTime from the plug-in, exact paramGetDerivative and paramGetIntegral of a Double; paramSetValue on a parameter with keys sets the key at the current time (the timeline follows the action's time); push buttons (`-s name`); paramEditBegin/End succeed. The report has the keys of each numeric parameter and, per render, the values of the parameters with keys. ofxhost: `params={name: {t: v}}` for keys, `None` for a button. Tests: the Tester sample's `keysTest` button checks every keyframe function from the plug-in side; hostdemo-checks.sh and test_basic.py check interpolation, the per-render values and the instanceChanged times. Requested by openfx-misc (ImageStatistics' analysis, push buttons).
- **PR 1 is implemented:** images follow the clip preferences, a new sentinel-filled output image per render, `-d byte|short|float`, `-i Clip=default|solid:r,g,b,a|ramp|checker:N|none`, `--no-std-paths`, `--no-cache`, `--quiet`, and the CI checks (Invert on float RGBA, basic plug-in with its Mask disconnected).
- **PR 1 review fixes:**
  - a mandatory clip can't be disconnected (`checkClipConnectionStatus`): "Any clip that is not optional will *always* be connected during a render action";
  - a disconnected clip reports `kOfxBitDepthNone` and `kOfxImageComponentNone` (unmapped and mapped) and an empty RoD; with no connected input, the output gets the `-d` depth;
  - the output RoD is the plug-in's getRegionOfDefinition; the render window, the output image and the PPM are that RoD clipped to the project frame;
  - numbers must be finite decimals (no nan, inf, hexadecimal or ERANGE);
  - unwritten float pixels (NaN) fail the run, unless `--allow-unwritten`. The 0xCD sentinel of byte and short images can't be detected, so use `-d float` for that check;
  - RGB inputs are opaque, RGBA and Alpha inputs are unpremultiplied; the output premultiplication comes from the clip preferences;
  - PPM rows are written top row first, with the header comments before the maxval;
  - CI: exact values for byte and float Invert, an Alpha Mask, a mandatory clip, number parsing, unwritten pixels (PropTester writes nothing), and the output size of a plug-in that sets its RoD (the dot example). The Windows job compiles the example hosts.
- **PR 2 is implemented:** exact I/O.
  - `--format ppm|pfm`. PFM is little-endian (scale -1.0), with rows "in left to right, bottom to top order" ([Debevec's PFM page](https://www.pauldebevec.com/Research/HDR/PFM/)), the same order as OFX images and the opposite of PPM. RGB and RGBA images are written as PF (RGB), with the alpha of RGBA images in a separate `<prefix>.<t>.alpha.pfm` (Pf); Alpha images are Pf. Byte and short values are converted to float in [0,1].
  - `-i Clip=file:path.pfm`: PF or Pf, little-endian only (a positive scale is rejected), the same image at all times. Each file pixel is a clip pixel at render scale 1: the clip's RoD is the file size in pixels at the project PAR (hostDemo doesn't support multiple clip PARs). PF gives (r,g,b,1), Pf gives r=g=b=a. The values are converted to the clip's mapped depth and components. Malformed files (identifier, size, scale, truncated or trailing data) are rejected.
  - The output numbers (size, bounds, depth, components, per-component min, max and mean in the native range with NaN left out, the number of NaN pixels, and a CRC-32 (zlib's) of the pixel bytes within the bounds, row by row from the bottom, components little-endian) are in the report's `renders[].output`.
  - CI: a PFM round trip (the file orientation is checked at known pixels) and the rejection of a big-endian file.
- **Part B (Python wrapper) is implemented** in `HostSupport/examples/python`: the `ofxhost` package (`render()`, `describe()`, PFM read/write), `ofxhost.testing` (`assert_close`, a `hostdemo` fixture that skips when hostDemo isn't built), and a thin `hostDemo.py` command line. It doesn't use a JSON job file: `render()` builds the hostDemo command line (`--no-std-paths --no-cache --quiet --format pfm --report`, `-i Clip=file:` for NumPy inputs) and runs it in a temporary directory, with `OFX_PLUGIN_PATH` from the given bundles and a timeout; hostDemo renders frames 0 and 1, and the output numbers come from `.report`. ofxhost and hostDemo ship together, so there is no feature detection. `hostDemo.py` saves the outputs as `.npy`. hostDemo's PFM input has no alpha (PF gives alpha 1), so RGBA NumPy inputs must have alpha 1. The pytest suite covers only the wrapper (NumPy/PFM round trip, orientation, an error on failure, one Invert; hostDemo's semantics are checked by the CI script) and runs in the Linux CI job.

- **PR 3 is implemented:** the report.
  - `--report out.json`, written after the run even if it failed (`ok`, `exitStatus`): the plug-in (id, version, label), context, host name and profile, depth and inputs (`-i` spec, connected, optional, mask); the actions in order with their status (`ok`, and the status name, e.g. `kOfxStatReplyDefault`); the clip preferences (per clip depth, components, PAR and unmapped values, plus output premultiplication, fielding, frame rate, continuous samples and frame varying); the time domain (general context only: "Only effects instantiated in the General Context can have this called on them"); then one `renders` item per time and view, with the input RoDs, getRegionOfDefinition, the render window, isIdentity (identity, clip and time), getFramesNeeded (per input), getRegionsOfInterest (per input, for the render window), the render status and the output's numbers and files. At the end come the parameter values and enabled/secret states, the messages (message suite and persistent messages: type, id, text) and the progresses (start, end, update count). hostDemo now calls isIdentity and getFramesNeeded before each render; an identity effect is still rendered, so its output can be checked.
  - `--describe out.json`: no instance. For each context, the effect flags (supported depths, tiles, multi-resolution, temporal access, multiple clip depths/PARs, thread safety, GPU flags...), the clips (optional, mask, supported components, temporal access, tiles...) and the params in definition order (type, label, hint, default, min/max, display min/max, choice options, dimension, secret, parent group and page).
  - Both use one small C++98 JSON writer (`hostDemoJson.h`): strings are escaped (invalid UTF-8 becomes U+FFFD), numbers have 17 significant digits in the classic locale, NaN is null. Keys are camelCase; depths and components are OFX strings, except in the output numbers, which use short names (byte, RGBA...).
  - Command-line parameters are set after createInstance and the first getClipPreferences, one at a time, each followed by begin/instanceChanged/end (kOfxChangeUserEdited, time 0); getClipPreferences runs again if a slave param changed. Setting values before createInstance (as when a host loads a project) isn't offered: a user edit is what tests the changedParam logic, and no CI check needed the old order.
  - CI (checked with Python's standard `json` module): the Invert report (RoD = input RoD, not an identity, frames needed [t,t], clip preferences, exact output numbers), `--describe` of the basic plug-in (clips, mask, params, ranges, pages, groups), instanceChanged (the basic plug-in disables its component scales), the dot example's RoD and output size, a failed run's report, and string escaping. The checks were mutation-tested: a wrong RoD, a missing field, no instanceChanged, a wrong mask flag, a wrong identity, a missing quote escape and no report on failure are each caught.
- **Trimmed option set:** `-h -p -m -c -o --format ppm|pfm -d -i -s --report --describe --no-std-paths --no-cache --quiet --allow-unwritten`. `--stats`, `--format ppm16`, `--cache PATH`, big-endian PFM input, and ofxhost's `render(frames=)`, feature detection and `hostDemo.py --describe`/`-f`/`--save pfm` were removed: the report carries the output numbers, and the only external user (openfx-supportext's `tests/hostdemo/run.sh`) needs only `--no-std-paths --no-cache -p -c -o` and parameters. `hostDemo -h` has one line per option; the file formats and JSON schemas are in `HostSupport/examples/python/README.md`.

- **PR 4 is implemented:** the node primitives. `--frames` (any distinct times), `--scale` (render scale in (0,1]), `--window` (pixels at the render scale, clipped to the RoD), a positioned PFM input `file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]` (pixels at the render scale, with the alpha file and premultiplication state of an RGBA output, so an output with its bounds is the next node's input with the same RoD and alpha), and `--query` (RoD, identity, frames needed and regions of interest at each time; no begin/endSequenceRender, no render, no file). The report has the `frames`, `renderScale`, `window` and `query`. CI: an Invert at fractional times, scale 0.5 and a window (exact RoI and output), the same output as a positioned input (same RoD, inverted back), with its alpha file and premultiplication, a query run, and the option errors.

- **Part B, graph execution, is implemented:** `ofxhost.graph` (about 180 lines): `Node`, `topological_order` (with cycle detection), `load` (a JSON graph file) and `execute(output, bundles, frames, scale, window)`. Outputs first, `--query` gives each node's frames needed and regions of interest. Requests for the same node and time are merged (the union of the windows). Inputs first, each node is rendered once per time, and its output becomes an `ofxhost.Image` input (bounds, alpha, premultiplication). `render()` takes `frames`, `scale`, `window` and `query`, and returns `images`; RGBA array inputs may have any alpha. `hostDemo.py -g graph.json` runs a graph file. Limits: the queries use project-sized inputs, and a node can use only one frame of each input node. Tests: topological order and cycles, loading, Invert(Invert(x)) = x at a render scale and window, alpha included, and a diamond (one node feeding two inputs) rendered once.

- **Review of #117–#123 (two independent reviewers, after the merge), fixed:**
  - images larger than 2^28 pixels are refused instead of overflowing (a blocker: `--window` had no limit);
  - a clip given twice, times with the same file name, and NaN or infinite PFM input values are refused;
  - begin/endSequenceRender and the render action declare the real frame step and sequential state;
  - the Natron format of the default input is in pixels (720x576);
  - ofxhost resolves both paths of `file:rgb.pfm+alpha.pfm`;
  - the graph executor computes each node's RoD inputs first, so the queries see the real input RoDs (inputs outside the project were dropped or clipped), gives a transparent input with the right RoD when a node renders nothing, raises a clear error when the output renders nothing, and refuses a range of frames needed instead of collapsing it;
  - graph files with errors give messages instead of tracebacks.

## Part B: Python test wrapper (`ofxhost`), subprocess first

Each test must run in its own process anyway: HostSupport has process-wide singletons (`PluginCache::getPluginCache()`, `gImageEffectHost`), plug-ins can't be unloaded, and a crashing plug-in would take down the interpreter. So v1 is a pure-Python wrapper around the hostDemo CLI, not in-process bindings:
- **`ofxhost.py`** (~200 lines): writes a JSON job (plug-in, context, host profile, inputs, parameters, times), runs hostDemo with a timeout, and reads back the PFM images (`numpy.fromfile`) and the JSON report;
- **`ofxhost.testing`:** pytest helpers (`render(...)`, `assert_close(img, ref, atol)`, bundle fixtures);
- **image contract:** NumPy arrays are H×W×C and top-down; OFX images are bottom-up, with pixel bounds. The wrapper flips and records `bounds.x1, y1`. Generators get an explicit output window.

```python
import ofxhost, numpy as np
r = ofxhost.render("Misc/Misc.ofx.bundle", "net.sf.openfx.GammaPlugin", context="filter",
                   inputs={"Source": src}, params={"value": (2.2, 2.2, 2.2, 1.0)}, depth="float")
assert np.allclose(r.output[..., :3], src[..., :3] ** (1 / 2.2), atol=1e-5)
frame = r.report["renders"][0]
assert frame["regionOfDefinition"]["rod"] == frame["inputs"]["Source"]["regionOfDefinition"]
```

In-process bindings (nanobind) are deferred until file I/O is a measured bottleneck. Stable-ABI wheels would need Python ≥ 3.12.

## CI cost (frugal policy)

- **Per PR:** Part A adds about a minute to the existing Linux and macOS jobs. Turn on `BUILD_EXAMPLE_HOSTS` on Windows (MSYS2), so all three jobs run the checks. Python tests run in the Linux job only, which already sets up Python; no new jobs.
- **Nightly (extended.yml):** the Python tests on macOS (MacPorts Python and NumPy) and MSYS2, all depths, profiles and scales, and an ASan/UBSan run of hostDemo over the openfx-misc plug-ins.
- **Consumers:** they build HostSupport and hostDemo from their openfx submodule, cached per OS.

## Open questions
- Where do plug-in tests live: in each consumer repo (proposed), or a shared test repo?
- Minimum Python for the wrapper: 3.10 is proposed, the same as `scripts/check-props.py`.
- Is NumPy acceptable as a test-only dependency? Proposed: yes.
- Reference images: committed in each consumer repo, or regenerated? Proposed: assertions and the report's output numbers first, small reference PFMs only where needed.
