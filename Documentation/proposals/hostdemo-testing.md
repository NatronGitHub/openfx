# DRAFT: hostDemo as a plug-in test host, and a Python test wrapper (`ofxhost`)

Status: design proposal, revised after two independent reviews. For NatronGitHub/openfx; it doesn't change the OFX API.

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

## Part A: hostDemo as a test host (C++, HostSupport/examples only unless noted)

PRs in this order. Each adds a CI check to the existing per-PR jobs.

1. **Image model and deterministic runs:**
   - images are allocated with each clip's mapped depth (byte, short, float) and components (RGBA, RGB, Alpha), with the bounds the action asks for (RoI ∩ RoD) and the render scale;
   - the output image is created per render, and unwritten pixels are filled with a sentinel (NaN for float, 0xCD for integer depths);
   - inputs per clip: `-i Clip=solid:r,g,b,a | ramp | checker:N | none`, where `none` means disconnected; the content depends on time without wrapping;
   - `--no-std-paths` (only OFX_PLUGIN_PATH), `--no-cache` / `--cache PATH`, and `--quiet` (no cache dump, so stdout is free for results);
   - fix `delete[]`; clean exit codes;
   - CI: Invert on float RGBA, and a disconnected optional clip.
2. **Exact I/O:** PFM input and output, `--stats` (min, max and mean per channel, plus a checksum), and 16-bit PPM.
3. **Report:** `--report out.json` with the results of isIdentity, getRegionOfDefinition, getRegionsOfInterest, getFramesNeeded, getTimeDomain and getClipPreferences, plus messages and progress. `--describe` outputs JSON (params with type, default, range and options; clips; contexts; flags). Both share one JSON writer. Parameters can be set after createInstance, which fires `instanceChanged`.
4. **Host profiles:** `--host minimal | natron-like` sets the advertised capabilities: RGB, multi-resolution, multiple clip depths and PARs, contexts, and explicitly no GPU. Tests run under at least two profiles.
5. **Time and scale:** frame range, fractional times, render scale, render window, project size/PAR, and field.
6. **Animation:** keys with linear interpolation, plus getDerivative/integrate. Needed for Transform motion blur.
7. **Readers and writers:** reader and writer contexts, file paths, and the OCIO configuration (the `OCIO` environment variable). Covers openfx-io and Arena's readers.
8. **Later:**
   - multi-plane and multi-view (Nuke/Natron extensions);
   - concurrent host-side renders, honouring `kOfxImageEffectPluginRenderThreadSafety`. A real multithread suite needs `OFX_SUPPORTS_MULTITHREAD`, which changes HostSupport's `ImageEffect::Host` vtable (9 pure virtuals), so it waits for the suites proposal;
   - headless overlays: the DrawSuite plumbing in ofxhInteract.cpp, plus pen events and the cursor (#63);
   - host-side parametric parameters, which are a library change with their own proposal.

Builds use the same extension flags as the consumers (NATRON, NUKE...), so Natron-specific code paths (e.g. Transform's getTransform) are tested.

## Implementation status

- **PR 1 is implemented:** images follow the clip preferences, a new sentinel-filled output image per render, `-d byte|short|float`, `-i Clip=default|solid:r,g,b,a|ramp|checker:N|none`, `--no-std-paths`, `--no-cache`/`--cache PATH`, `--quiet`, and the CI checks (Invert on float RGBA, basic plug-in with its Mask disconnected).
- **PR 1 review fixes:**
  - a mandatory clip can't be disconnected (`checkClipConnectionStatus`): "Any clip that is not optional will *always* be connected during a render action";
  - a disconnected clip reports `kOfxBitDepthNone` and `kOfxImageComponentNone` (unmapped and mapped) and an empty RoD; with no connected input, the output gets the `-d` depth;
  - the output RoD is the plug-in's getRegionOfDefinition; the render window, the output image and the PPM are that RoD clipped to the project frame;
  - numbers must be finite decimals (no nan, inf, hexadecimal or ERANGE);
  - unwritten float pixels (NaN) fail the run, unless `--allow-unwritten`. The 0xCD sentinel of byte and short images can't be detected, so use `-d float` for that check;
  - RGB inputs are opaque, RGBA and Alpha inputs are unpremultiplied; the output premultiplication comes from the clip preferences;
  - PPM rows are written top row first, with the header comments before the maxval;
  - CI: exact values for byte, short and float Invert, the Mask (disconnected, alpha 0, checker), mandatory clips, number parsing, unwritten pixels (PropTester writes nothing), and the output size of a plug-in that sets its RoD (the dot example). The Windows job compiles the example hosts.

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
assert r.report["regionOfDefinition"] == r.report["inputs"]["Source"]["regionOfDefinition"]
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
- Reference images: committed in each consumer repo, or regenerated? Proposed: assertions and `--stats` first, small reference PFMs only where needed.
