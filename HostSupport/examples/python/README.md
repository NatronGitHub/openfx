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
  host_binary=None, timeout=60, extra_args=(), check=True)` runs hostDemo (which renders
  frames 0 and 1) with `--no-std-paths --no-cache --quiet --format pfm --report`, and
  `OFX_PLUGIN_PATH` set from `bundle_paths` (directories, or `.ofx.bundle` paths, which stand
  for their parent directory).
  - `inputs`: clip name to a NumPy array (H×W, H×W×1, H×W×3, or H×W×4 with alpha 1; top-down;
    written to a PFM file), a `pathlib.Path` or `"file:x.pfm"`, or a hostDemo spec:
    `"none"` (disconnected), `"default"`, `"solid:r,g,b,a"`, `"ramp"`, `"checker:N"`.
    hostDemo's PFM input is RGB with alpha 1, or grey with r=g=b=a.
  - `params`: name to value (number, bool, string, or a sequence), passed as `-s name=value`.
  - A non-zero exit or a timeout raises `HostDemoError` (with the command, stdout and stderr),
    unless `check=False`.
- The result has `.outputs` (frame to float32 H×W×C, top-down; RGBA outputs merge the
  separate alpha file; stereo builds key by `(frame, "l"|"r")`), `.output` (the first frame),
  `.alpha` (frame to H×W), `.report` (the `--report` JSON: `renders[i].output` has the
  size, min, max, mean, NaN count and crc32 of each output image), `.returncode`, `.stdout`,
  `.stderr` and `.cmd`.
- `describe(bundle_paths, plugin_id)` returns the `--describe` JSON.
- `read_pfm` / `write_pfm`: top-down float32 arrays; files are bottom-up, little-endian
  when written, either byte order when read.
- hostDemo is found from the `host_binary` argument, then `$OFXHOST_HOSTDEMO`, then the build
  tree (`HostSupport/examples/*-release/hostDemo`, then CMake `build*/HostSupport/examples`).

`ofxhost.testing` has `assert_close(actual, expected, atol, rtol)`, which reports the number
of mismatches, the largest difference and where, and a `hostdemo` pytest fixture that skips
the tests when hostDemo isn't built.

`hostDemo.py` is a small command line wrapper: it saves the outputs as
`<prefix>.<frame>.npy`, and prints the report as JSON (`python hostDemo.py -h`).

## hostDemo file formats

- `--format ppm` (default): plain PPM (P3), 8 bits, top row first. Short and float images are
  converted to 8 bits (float is clamped to [0,1]), Alpha images are written as grey, and
  unwritten (NaN) pixels as 0.
- `--format pfm`: PFM, little-endian (scale -1.0), bottom row first (as in OFX images; PFM
  rows are "in left to right, bottom to top order"). Byte and short values are converted to
  float in [0,1], float values (NaN included) are kept. RGB and RGBA images are written as
  PF (RGB): the alpha of an RGBA image goes to `<prefix>.<t>[l|r].alpha.pfm` (Pf). Alpha
  images are Pf.
- `-i Clip=file:path.pfm`: a little-endian PFM (PF or Pf, negative scale; a positive scale
  is rejected), the same at all times. Each file pixel is a clip pixel at render scale 1: the
  clip's RoD is the file size in pixels at the project PAR (768/720), i.e.
  0,0,width*PAR,height. PF gives (r,g,b,1), Pf gives r=g=b=a=value; the values are converted
  to the clip's depth (clamped to [0,1] for byte and short) and components. Malformed files
  are rejected.
- The other `-i` specs: `default` (a grey level t: black at t=0, white at t=1, with the time
  drawn in the reverse colour), `solid:r,g,b,a`, `ramp` (0 at the left to 1 at the right),
  `checker:N` (N-pixel squares of 0 and 1), `none` (disconnected, optional clips only).
  Numbers, in `-i` and in parameter values, must be finite decimals.
- Parameters (`-s name=value` or `name=value`) are set in order after createInstance and
  getClipPreferences, each one between begin/end instanceChanged (kOfxChangeUserEdited, time
  0); the clip preferences are run again if a clip preferences slave param changed.
  Multi-dimensional values are comma-separated; booleans take 0/1/true/false, Choice the
  index or label, String/StrChoice/Custom the whole value. Parameters don't animate: the
  derivative of a Double is 0, its integral from t1 to t2 is value * (t2 - t1).
- The images have the depth and components that getClipPreferences maps each clip to. The
  output image is filled with NaN (float) or 0xCD bytes before each render; unwritten float
  pixels fail the run unless `--allow-unwritten`.

## hostDemo JSON

Numbers have 17 significant digits; NaN (no value) is null. Rectangles are `[x1,y1,x2,y2]`,
in canonical coordinates, except `renderWindow` and `bounds` (pixels).

`--report file.json`, written after the run even if it failed: `schema`
("hostDemo.report"), `schemaVersion` (1), `ok`, `exitStatus`, `plugin` {id, versionMajor,
versionMinor, label, grouping, filePath}, `context`, `host` {name, profile}, `depth`,
`inputs` {Clip: {spec, connected, optional, isMask, unmappedDepth, unmappedComponents}},
`actions` [{action, ok, status[, param, value]}] (createInstance, getClipPreferences,
begin/end/instanceChanged, begin/endSequenceRender), `clipPreferences` {clips {Clip: {depth,
components, pixelAspectRatio, unmappedDepth, unmappedComponents, connected}},
outputPremultiplication, outputFielding, outputFrameRate, continuousSamples, frameVarying},
`timeDomain` {ok, status, range} (null outside the general context), `renders`, `params`
{name: {type, value, enabled, secret}} (after the run), `messages` [{kind, type, id, text}],
`progress` [{message, id, started, ended, updates, lastValue}].

Each `renders` item is one time and view: {time, view, renderScale, inputs {Clip: {connected,
regionOfDefinition}} (null for a disconnected clip), regionOfDefinition {ok, status, rod},
renderWindow, isIdentity {ok, status, identity, clip, time}, framesNeeded {ok, status, clips
{Clip: [[min,max]...]}}, regionsOfInterest {ok, status, regionOfInterest, clips {Clip:
rect}}, render {ok, status, imageFetchErrors}, output}. `output` is null if nothing was
written, else {w, h, bounds, depth (byte|short|float), comps (RGBA|RGB|Alpha), min, max,
mean (per component, in the native range of the depth, NaN left out), nan (the number of
pixels with a NaN component), crc32 (zlib's CRC-32 of the pixel bytes within the bounds, row
by row from the bottom, components little-endian), files}. Actions run in this order, and
one that fails stops the run: the later items are missing or null. `ok` is true for
kOfxStatOK and kOfxStatReplyDefault (only kOfxStatOK for render); `status` is the status
name (null for getClipPreferences, whose status HostSupport doesn't return). An identity
effect is rendered anyway.

`--describe file.json` describes the plug-in in all its contexts, without creating an
instance: {schema ("hostDemo.describe"), schemaVersion (1), ok, plugin, contexts {context:
{ok, flags, clips, params}}}. flags: supportedPixelDepths, supportsTiles,
supportsMultiResolution, temporalClipAccess, supportsMultipleClipDepths,
supportsMultipleClipPARs, fieldRenderTwiceAlways, singleInstance, hostFrameThreading,
renderThreadSafety, sequentialRender, clipPreferencesSlaveParams, openGLRenderSupported,
openCLRenderSupported, cudaRenderSupported, cudaStreamSupported, metalRenderSupported,
cpuRenderSupported. clips: {name, label, hint, optional, isMask, supportedComponents,
temporalClipAccess, supportsTiles, fieldExtraction, secret}. params, in definition order:
{name, type, label, hint, scriptName, secret, enabled, parent, page, dimension, default, min,
max, displayMin, displayMax, doubleType, choiceOptions, animates, evaluateOnChange,
children}; a property that the type doesn't have is null, one-dimensional values are
scalars.

## Tests

```sh
make -C HostSupport && make -C HostSupport/examples && make -C Support/Plugins CONFIG=release
python3 -m venv /tmp/ofxhost-venv && /tmp/ofxhost-venv/bin/pip install numpy pytest
/tmp/ofxhost-venv/bin/python -m pytest -q -p no:cacheprovider HostSupport/examples/python/tests
```

Support/Plugins/tests has tests of the sample plug-ins, meant as templates for plug-in repos
(see its README.md). CI runs both in the Linux job.
