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
  frames=None, scale=None, window=None, query=False, host_binary=None, timeout=60,
  extra_args=(), check=True)` runs hostDemo (by default at frames 0 and 1) with
  `--no-std-paths --no-cache --quiet --format pfm --report`, and `OFX_PLUGIN_PATH` set from
  `bundle_paths` (directories, or `.ofx.bundle` paths, which stand for their parent
  directory). `frames`, `scale`, `window` and `query` are hostDemo's `--frames`, `--scale`,
  `--window` and `--query`.
  - `inputs`: clip name to a NumPy array (H×W, H×W×1, H×W×3 or H×W×4; top-down; written to
    PFM files, at pixel 0,0), an `ofxhost.Image` (pixels, bounds at the render scale, and
    premultiplication: e.g. an output of another render), a `pathlib.Path` or
    `"file:x.pfm"`, or a hostDemo spec: `"none"` (disconnected), `"default"`,
    `"solid:r,g,b,a"`, `"ramp"`, `"checker:N"`.
  - `params`: name to value (number, bool, string, or a sequence), passed as `-s name=value`;
    or keys, `{time: value}` (numeric parameters that animate), passed as `-s name@time=value`;
    or None, to press a push button (`-s name`).
  - A non-zero exit or a timeout raises `HostDemoError` (with the command, stdout and stderr),
    unless `check=False`.
- The result has `.outputs` (frame to float32 H×W×C, top-down; RGBA outputs merge the
  separate alpha file; stereo builds key by `(frame, "l"|"r")`), `.output` (the first frame),
  `.alpha` (frame to H×W), `.report` (the `--report` JSON: `renders[i].output` has the
  size, min, max, mean, NaN count and crc32 of each output image), `.images` (the same keys
  to `Image`, with the bounds and the output premultiplication), `.returncode`, `.stdout`,
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

`minimal_host.py` is not a hostDemo wrapper: it is a minimal OFX host in Python, in-process, on
the HostSupport Python bindings (the `ofxh` module, built with `-DOFX_PYTHON_BINDINGS=ON`;
NatronGitHub/openfx#136). It renders one frame of a plug-in on one input, and is a template:
which classes to subclass, and what each override does
(`PYTHONPATH=<build>/HostSupport/python python minimal_host.py <plugin dir> <plugin id> in.npy out.npy`).
The module can also be installed alone, with `pip install ./HostSupport/python`
(`HostSupport/python/pyproject.toml`, scikit-build-core). Its tests are in `HostSupport/python/tests`.

## hostDemo file formats

- `--format ppm` (default): plain PPM (P3), 8 bits, top row first. Short and float images are
  converted to 8 bits (float is clamped to [0,1]), Alpha images are written as grey, and
  unwritten (NaN) pixels as 0.
- `--format pfm`: PFM, little-endian (scale -1.0), bottom row first (as in OFX images; PFM
  rows are "in left to right, bottom to top order"). Byte and short values are converted to
  float in [0,1], float values (NaN included) are kept. RGB and RGBA images are written as
  PF (RGB): the alpha of an RGBA image goes to `<prefix>.<t>[l|r].alpha.pfm` (Pf). Alpha
  images are Pf.
- `-i Clip=file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]`: a little-endian PFM (PF or Pf, negative scale; a positive
  scale is rejected), the same at all times. Each file pixel is a clip pixel at the render
  scale (`--scale`, default 1), the bottom-left one at pixel (x1,y1) (default 0,0): the clip's
  RoD is the file's pixel bounds divided by the render scale, at the project PAR (768/720),
  i.e. x1/sx*PAR, y1/sy, (x1+width)/sx*PAR, (y1+height)/sy. `+alpha.pfm` gives the alpha of
  a PF file (a Pf file of the same size, as written for RGBA outputs), and `#premult`,
  `#unpremult` or `#opaque` the premultiplication state (the suffixes in this order; the pixel
  values must be finite) (default: unpremultiplied, opaque for
  RGB). So the output of a render, with its `bounds` and the `outputPremultiplication` of its
  clip preferences, can be the input of the next one at the same scale:
  `file:Out.0.pfm+Out.0.alpha.pfm@x1,y1#premult`. PF gives (r,g,b,1), Pf gives r=g=b=a=value; the values are converted
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
  index or label, String/StrChoice/Custom the whole value. `-s name` presses a push button
  (instanceChanged only).
- Keys: `-s name@t=value` sets a key at time t, followed by instanceChanged at time t; numeric
  parameters that animate only (kOfxParamPropAnimates). Between keys, values are interpolated
  linearly (rounded for integers), and held for booleans and choices; before the first key and
  after the last one, they are those of the first and last keys. A plug-in may set keys too
  (paramSetValueAtTime), and use paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex (keys
  within 1e-4 of a time match it), paramDeleteKey, paramDeleteAllKeys, and paramGetDerivative
  and paramGetIntegral of a Double (exact, piecewise linear). paramSetValue on a parameter with
  keys sets the key at the current time: the timeline (timeLineGetTime) is at the time of the
  current action (the render's time, or the time of the instanceChanged). String, Custom,
  StrChoice and Bytes parameters don't animate. paramEditBegin and paramEditEnd succeed.
- `--ofx14` makes hostDemo an OFX 1.4 host, to check that a plug-in still works in hosts
  without OFX 1.5 and the extensions: the host descriptor has only the OFX 1.4 host properties
  (reading another one, e.g. kOfxParamHostPropSupportsStrChoice, kOfxImageEffectPropCPURenderSupported,
  the colour management or Natron properties, gives kOfxStatErrUnknown), fetchSuite gives only
  the OFX 1.4 suites (no DrawSuite, OpenCL program suite, plane or Vegas suites),
  OfxSetHost is not called, paramDefine of a StrChoice or Bytes param returns
  kOfxStatErrUnknown, and one view is rendered: even in stereo (Nuke/Vegas extension) builds,
  the output files have no l/r suffix (`<prefix>.<t>`), and the keys of `RenderResult.outputs`
  are plain times, not `(t, "l"|"r")`. The report's `host.profile` is "ofx14" (else
  "default"). It uses its own plug-in cache file, hostDemoPluginCache-ofx14.xml. In Python:
  `extra_args=["--ofx14"]`.
- The environment variable `OFX_HOSTDEMO_KEEP_LOADED=1` makes hostDemo keep the plug-ins that
  the scan found loaded (`Host::keepPluginsLoadedAfterScan()`): they get one kOfxActionLoad and
  kOfxActionDescribe, and kOfxActionUnload at exit. By default (as in a run that uses the plug-in
  cache), the scan sends kOfxActionLoad, kOfxActionDescribe and kOfxActionUnload, unloads the
  binary, and the first use of the plug-in loads it again.
- Times, scale and window: `--frames t0,t1,...` (default 0,1; any distinct finite times,
  fractional too, distinct to 12 significant digits) renders each time, in the given order, into
  `<prefix>.<t>[l|r]` (12 significant digits). begin/endSequenceRender declare a sequential render
  with the frames' step if they increase with a constant step, else a non-sequential one with step 1.
  The render window is at most 1000000 pixels wide or high and 2^28 pixels in all.
  `--scale s[,sy]` is the render scale, in (0,1]. `--window x1,y1,x2,y2` is the render window in
  pixels at the render scale, clipped to the output RoD; without it, the render window is the
  RoD clipped to the project. `--query` calls only the actions that query the effect at each
  time (getRegionOfDefinition, isIdentity, getFramesNeeded, getRegionsOfInterest), with no
  begin/endSequenceRender and no render: the `render` and `output` of each `renders` item are
  null, and no file is written.
- The images have the depth and components that getClipPreferences maps each clip to. The
  output image is filled with NaN (float) or 0xCD bytes before each render; unwritten float
  pixels fail the run unless `--allow-unwritten`.

## hostDemo JSON

Numbers have 17 significant digits; NaN (no value) is null. Rectangles are `[x1,y1,x2,y2]`,
in canonical coordinates, except `renderWindow` and `bounds` (pixels).

`--report file.json`, written after the run even if it failed (but not if the command line
is invalid: hostDemo then exits with status 1 before running anything): `schema`
("hostDemo.report"), `schemaVersion` (1), `ok`, `exitStatus`, `plugin` {id, versionMajor,
versionMinor, label, grouping, filePath}, `context`, `host` {name, profile ("default", or "ofx14"
with `--ofx14`)}, `depth`,
`frames`, `renderScale` [sx, sy], `window` (`--window`, or null), `query`,
`inputs` {Clip: {spec, connected, optional, isMask, unmappedDepth, unmappedComponents}},
`actions` [{action, ok, status[, param, value, time]}] (value null for a button press) (createInstance, getClipPreferences,
begin/end/instanceChanged, begin/endSequenceRender), `clipPreferences` {clips {Clip: {depth,
components, pixelAspectRatio, unmappedDepth, unmappedComponents, connected}},
outputPremultiplication, outputFielding, outputFrameRate, continuousSamples, frameVarying},
`timeDomain` {ok, status, range} (null outside the general context), `renders`, `params`
{name: {type, value, enabled, secret[, keys]}} (after the run; value at time 0, keys
[[time, value]...] for the numeric parameters), `messages` [{kind, type, id, text}],
`progress` [{message, id, started, ended, updates, lastValue}].

Each `renders` item is one time and view: {time, view, renderScale, params {name: value} (at
that time, for the parameters with keys), inputs {Clip: {connected,
regionOfDefinition}} (null for a disconnected clip), regionOfDefinition {ok, status, rod},
supportsTiles {effect, clips {Clip: bool}} (the instance's values, which a plug-in may change in
instanceChanged: without tiles, the render window is the whole RoD and the input images are
whole-RoD, if hostDemo can make images that large), renderWindow, isIdentity {ok, status, identity, clip, time}, framesNeeded {ok, status, clips
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

## Graphs

`ofxhost.graph` builds and executes plug-in instance graphs (DAGs: an output may feed several
inputs). A `Node` has a plug-in, a context, parameters and inputs; an input is another
`Node`, or anything `render()` takes.

```python
from ofxhost.graph import Node, execute, load
src = Node("net.sf.openfx.invertPlugin", inputs={"Source": "ramp"})
out = Node("net.sf.openfx.basicPlugin", "general", {"scale": 2},
           inputs={"Source": src, "Mask": src})
images = execute(out, bundles, frames=[0, 1], scale=0.5, window=(0, 0, 100, 80))
images[0].pixels, images[0].bounds   # top-down array, pixel bounds at the render scale
```

`load("graph.json")` reads the same graph from a file, and returns the output node and the
nodes by name:

```json
{"output": "b",
 "nodes": {"a": {"plugin": "net.sf.openfx.invertPlugin", "inputs": {"Source": "ramp"}},
           "b": {"plugin": "net.sf.openfx.basicPlugin", "context": "general",
                 "params": {"scale": 2},
                 "inputs": {"Source": {"node": "a"}, "Mask": {"node": "a"}}}}}
```

`execute()` sorts the graph topologically. It runs hostDemo `--query` on each node, outputs
first, to get the frames and regions it needs from its inputs. The requests for the same node
and time are merged; an input of a node or clip without tiles is asked for its whole RoD (as
hostDemo gives whole-RoD images then). Then it renders each node once per time, inputs first,
and passes each output on with its bounds, alpha and premultiplication. Limits:
- the queries use project-sized inputs, since the real ones aren't rendered yet;
- a node can use only one frame of each input node, since hostDemo's inputs are the same
  image at all times.

`python hostDemo.py -b DIR -g graph.json --frames 0,1 --scale 0.5 -o out` runs a graph file.

## OFX constants and property metadata

`ofxhost.constants` has every `#define` of `include/` (strings, status codes, key symbols...),
with its C name. `ofxhost.props` has the `@propdef` metadata of every property: type,
dimension (0 means any number), valid values of enums (including extension and deprecated
values), and the extension it belongs to (`None` for standard properties).

```python
from ofxhost import constants as k
from ofxhost.props import PROPS
p = PROPS[k.kOfxImageEffectPropSupportedPixelDepths]
p.type, p.dimension, p.values   # ('enum', 0, ('OfxBitDepthNone', 'OfxBitDepthByte', ...))
```

Both are generated from the headers by `scripts/gen-ofxhost-py.py` (needs PyYAML); run it after
changing a header. CI fails if they are out of date, and `scripts/check-props.py` fails if a
`#define` of `include/` is neither a property with a `@propdef` nor classified in
`scripts/ofx_fork_props.py` (enum value, alias, or name).

## Tests

```sh
make -C HostSupport && make -C HostSupport/examples && make -C Support/Plugins CONFIG=release
python3 -m venv /tmp/ofxhost-venv && /tmp/ofxhost-venv/bin/pip install numpy pytest
/tmp/ofxhost-venv/bin/python -m pytest -q -p no:cacheprovider HostSupport/examples/python/tests
```

Support/Plugins/tests has tests of the sample plug-ins, meant as templates for plug-in repos
(see its README.md). CI runs both in the Linux job.
