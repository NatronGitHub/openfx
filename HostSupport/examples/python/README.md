# ofxhost: render OFX plug-ins from Python

A test wrapper around the demo host `hostDemo` (HostSupport/examples), with two backends that
have the same API and the same JSON report (NatronGitHub/openfx#136):

- `"subprocess"` (the default): hostDemo, run as a subprocess. Each render runs in its own
  process and temporary directory: HostSupport has process-wide singletons, plug-ins can't be
  unloaded, and a crashing plug-in must not take down the interpreter. Images are exchanged as
  PFM files.
- `"inprocess"`: `ofxhost.inprocess`, hostDemo's test host ported to Python on the HostSupport
  bindings (the `ofxh` module, built with `-DOFX_PYTHON_BINDINGS=ON`; `PYTHONPATH=<build>/HostSupport/python`,
  else ofxhost looks for it in `build*/HostSupport/python`: the build tree of the plug-ins of
  `$OFX_TEST_PLUGIN_PATH`, or the only one; with several and none of them the plug-ins', set
  `PYTHONPATH`). Same inputs, actions, render windows,
  errors and report, without files: each fetch of an input gives the plug-in a new contiguous NumPy
  array at the clip's depth, made from the input (as hostDemo makes a new image per fetch; ofxh
  passes that array itself, without another copy), and outputs are the arrays it rendered into.

Choose with `backend=` (`render()`, `describe()`, and `ofxhost.graph.execute()` through its
render arguments) or `$OFXHOST_BACKEND`. Needs Python 3.10 or later and NumPy (and pytest for the
tests). See Documentation/proposals/hostdemo-testing.md, Part B.

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
- The in-process backend differs only where it must:
  - no files: the report's `output.files` is `[]`, and an array input's `spec` is
    `"array:WxHxC[@x1,y1][#premult]"`; the report's `host.name` is ofxh's (`net.sf.openfx.ofxh`);
  - ofxh's string choice parameters refuse a value outside their enums (`could not set parameter`),
    hostDemo's accept it;
  - `timeout` is ignored (a hang hangs the caller), and a plug-in crash kills the interpreter: large
    test runs keep the subprocess backend, or run in worker processes. The crash's last output (an
    assert message, for example) is then lost with the captured stdout and stderr, which are
    unlinked temporary files: `OFXHOST_CAPTURE=0` leaves the file descriptors 1 and 2 alone (then
    `RenderResult.stdout` and `.stderr` are empty, and hostDemo's messages go to the real stderr);
  - `host_binary` is hostDemo's: with it, a call without `backend=` uses the subprocess backend
    whatever `$OFXHOST_BACKEND` says, and `backend="inprocess"` with it is a `ValueError`;
  - a plug-in whose worker thread (e.g. OpenMP) is the first to fetch the output image of a render
    gets no image: that fetch asks for the output clip's RoD, which runs the RoD action, and ofxh
    refuses to run an action from a worker thread (part 2);
  - `extra_args`: only `--ofx14`, `--allow-unwritten`, `-m MAJOR` and `-s ASSIGNMENT` (as
    hostDemo's, before those of `params`; e.g. to press a button twice) (and the no-ops `--quiet`,
    `--no-cache`, `--no-std-paths`); anything else raises `ValueError`;
  - stdout and stderr are captured from the file descriptors 1 and 2 during the call (the
    plug-ins' `printf` and `std::cerr` too), and the call runs in a temporary current directory, as
    hostDemo does (relative paths in the inputs are resolved before);
  - ofxh has one host per process. The backend keeps its host between the renders with the same
    plug-in directories and profile (`--ofx14`), so the plug-ins stay loaded (hostDemo loads them in
    each run); another set of directories or another profile makes a new host. It refuses to run
    while another ofxh host is initialized (RuntimeError). `ofxhost.inprocess.shutdown()` releases
    its host.
  - `ofxhost.inprocess` is also an example of a full host on ofxh (Host, ImageEffectInstance and
    ClipInstance subclasses, the actions in a host's order), as `minimal_host.py` is a minimal one.
- `read_pfm` / `write_pfm`: top-down float32 arrays; files are bottom-up, little-endian
  when written, either byte order when read.
- hostDemo is found from the `host_binary` argument, then `$OFXHOST_HOSTDEMO`, then the build
  tree (`HostSupport/examples/*-release/hostDemo`, then CMake `build*/HostSupport/examples`).

`ofxhost.testing` has `assert_close(actual, expected, atol, rtol)`, which reports the number
of mismatches, the largest difference and where; a `backend` pytest fixture, which runs a test on
each backend (`test_x[subprocess]`, `test_x[inprocess]`: it sets `$OFXHOST_BACKEND` for the test,
skips a backend that isn't built, or fails with `OFXH_REQUIRE=1`, and `$OFXHOST_TEST_BACKENDS`
restricts them, e.g. `subprocess`); the `backends` marker, for a test that depends on one
backend's behaviour (`@pytest.mark.backends("subprocess", reason="...")`: skipped on the others with
the reason; without `reason=`, the test fails); and a `hostdemo` fixture (the hostDemo binary; it
skips the tests when hostDemo isn't built). To get them: `pytest_plugins = ["ofxhost.testing"]` in
the top-level conftest.py (pytest refuses `pytest_plugins` in the others), or
`from ofxhost.testing import backend, pytest_configure` in any conftest.py (a conftest.py with its
own `pytest_configure` calls `ofxhost.testing.pytest_configure(config)` from it instead). The
fixtures that depend on `backend` must be function-scoped (as `render` and `describe` in
Support/Plugins/tests/conftest.py).

`hostDemo.py` is a small command line wrapper: it saves the outputs as
`<prefix>.<frame>.npy`, and prints the report as JSON (`python hostDemo.py -h`; `--backend inprocess` for the in-process backend).

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

`--report file.json` (also the report of the in-process backend), written after the run even if it failed (but not if the command line
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
`progress` [{message, id, started, ended, ends (the number of progressEnd calls), updates, lastValue}].

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

`ofxhost.graph` builds plug-in instance graphs (DAGs: an output may feed several inputs), saves
them to JSON and loads them, and executes them with hostDemo. A `Node` has a plug-in (and
optionally its `version`, `(major, minor)`), a context, parameters and inputs; an input is another
`Node`, a `Files` image sequence, or anything `render()` takes; an input clip that the node
doesn't give is disconnected. A parameter is a value, `Keys` (or a `{time: value}` dict), a
`FilePath` (the value of a file parameter), or `None` to press a push button. A `Graph` names the nodes, says
which ones are its outputs (and the file each one is written to), and holds the project and the
render settings.

```python
from ofxhost.graph import Files, Graph, Keys, Node, execute, load
src = Node("net.sf.openfx.invertPlugin", inputs={"Source": Files("in.####.pfm")})
out = Node("net.sf.openfx.basicPlugin", "general", {"scale": Keys({0: 1., 10: 2.})},
           inputs={"Source": src, "Mask": src})
g = Graph.of(out, frames=[0, 1], scale=0.5)   # names the nodes ("invertPlugin", "basicPlugin")
g.outputs["basicPlugin"] = "out.####.pfm"     # where the sample host writes it
g.save("graph.json")                          # load("graph.json") gives an equivalent Graph
images = execute(g, bundles)                  # with hostDemo (either backend): {0.0: Image, 1.0: Image}
images[0].pixels, images[0].bounds            # top-down array, pixel bounds at the render scale
```

The JSON format (`schema` "ofxhost.graph", `schemaVersion` 1; `save()` writes the keys in this
order, `Graph.dumps()` gives the text; everything but `nodes` and `outputs` is optional):

```json
{"schema": "ofxhost.graph", "schemaVersion": 1,
 "project": {"size": [768.0, 576.0], "offset": [0.0, 0.0], "pixelAspectRatio": 1.0666666666666667,
             "frameRate": 25.0, "frameRange": [0.0, 24.0]},
 "render": {"frames": [0.0, 1.0], "renderScale": [0.5, 0.5], "window": null, "depth": "float"},
 "outputs": {"basicPlugin": "out.####.pfm"},
 "nodes": {
  "invertPlugin": {"plugin": "net.sf.openfx.invertPlugin", "version": [1, 0], "context": "filter",
                   "params": {}, "inputs": {"Source": {"file": "in.####.pfm#premult"}}},
  "basicPlugin": {"plugin": "net.sf.openfx.basicPlugin", "context": "general",
                  "params": {"scale": {"keys": [[0.0, 1.0], [10.0, 2.0]], "interpolation": "linear"},
                             "scaleComponents": false},
                  "inputs": {"Source": {"node": "invertPlugin"}, "Mask": {"node": "invertPlugin"}}}}}
```

- `project`: the sample host's project (the defaults are hostDemo's, above). `execute()` renders in
  hostDemo's fixed project, and refuses a graph with one.
- `render`: `frames` (null: the sample host renders the outputs' time domain; `execute()` renders
  frame 0), `renderScale`, `window` (pixels at the render scale; null: the RoD clipped to the
  project), and `depth` (of the patterns and unconnected inputs: byte, short or float).
- `outputs`: each output node, and the file name pattern it is written to by the sample host
  (null: rendered only; e.g. a writer plug-in, which writes its own files). `"output": "b"`, the
  format before, is read as `{"b": null}`.
- `params`: values as `ofxh.param_template()` gives them (numbers, booleans, strings, lists; a
  choice's index, or the label of one of its options), keys, `{"file": path}` (a file parameter's
  value, which `save()` rewrites as it does the file inputs), or null (press a push button).
  `"interpolation"` is `"linear"`, hostDemo's and ofxh's only one: linear, rounded for integers,
  held for booleans and choices. Another one is refused.
- `inputs`: `{"node": name}`, `{"file": pattern}` (an image sequence, or a still image; a
  `#premult`, `#unpremult` or `#opaque` suffix gives its premultiplication), or a `render()` string:
  a test pattern (`"default"`, `"ramp"`, `"checker:16"`, `"solid:r,g,b,a"`, `"rod:..."`), `"none"`,
  or `"file:x.pfm"`. **An input clip that the graph doesn't give is disconnected**, as in a real host
  (`execute()` passes it as `"none"` to hostDemo): the test patterns must be asked for.
- Relative paths (file inputs, outputs and file parameters) are relative to the graph file's
  directory (`Graph.base`). `load()` keeps them as they are, so that saving the loaded graph gives
  the same bytes; `save()` to another directory rewrites them for that directory (absolute when
  there is no relative path, and `"file:"` specs absolute), so that the saved graph still finds its
  files.
- Unknown keys are refused (in the nodes and at the top level), and so are `"output"` and
  `"outputs"` together.
- A graph built in Python, saved, then loaded, is equivalent (the same nodes, versions,
  connections, values and keys, outputs and settings), and saving it again gives the same bytes.
  Arrays and `Image` inputs can't be saved (`ValueError`): write them to files.
- **Trust:** a graph is like a script: it chooses the plug-ins that run, with their parameters,
  and the files they read and write, anywhere you can. Run only the graphs you trust.

`execute()` sorts the graph topologically. It runs hostDemo `--query` on each node, outputs
first, to get the frames and regions it needs from its inputs. The requests for the same node
and time are merged; an input of a node or clip without tiles is asked for its whole RoD (as
hostDemo gives whole-RoD images then). Then it renders each node once per time, inputs first,
and passes each output on with its bounds, alpha and premultiplication. A `Files` input gives
each render the file of its frame (as a hostDemo `file:` input, whose pixels are at the render
scale), and a node's `version` gives hostDemo `-m MAJOR`. Limits:
- the queries use project-sized inputs, since the real ones aren't rendered yet;
- a node can use only one frame of each input node, since hostDemo's inputs are the same
  image at all times.

`python hostDemo.py -b DIR -g graph.json --frames 0,1 --scale 0.5 -o out` runs a graph file.

## The sample host and its command line

`ofxhost.graphhost` is the full sample host on the ofxh bindings: it runs a `Graph` over a frame
sequence in this process, with the images in memory between the nodes. It is the template for a
real host (`minimal_host.py` is the minimal one).

```python
from ofxhost import graph, graphhost
r = graphhost.run(graph.load("graph.json"), ["/path/to/plugins"])
r.ok, r.report, r.images["basicPlugin"][0.0], r.files
```

What it does (see its docstring):
- **Project load:** each node's values and keys are set before `kOfxActionCreateInstance`, with no
  instanceChanged (ofxCore.h: "if the values are being loaded from an old setup, that load should
  have taken place before the create instance action is called"; the report's
  `paramChangedCount` is 0). Then the clip preferences, and the push buttons (null values) are
  pressed as a user would, at the project's first frame.
- **Time domain:** the frames are the graph's (or `--frames`), else every frame of the outputs'
  time domains. The spec says of `kOfxImageEffectActionGetTimeDomain`: "Only effects instantiated in
  the General Context can have this called on them. In all other the host is in strict control
  over the temporal duration of the effect." This host's choice, following Natron
  (`OfxEffectInstance::getFrameRange`), is to call it in the general, reader, writer and generator
  contexts. Otherwise, or when the plug-in replies default, it uses the spec's default, "the union
  of all the frame ranges of the non optional input clips, infinite if there are no non optional
  input clips" (infinite: the project's frame range). An infinite or huge (beyond ±1e7) end is
  clamped to the project's frame range; NaN fails the run. An input clip's frame range is its input
  node's time domain, or the frames of its image sequence.
- **Sequence renders:** each node gets `beginSequenceRender` with the frames it will render
  (planned from the outputs' frames, the frames needed and the identities), sequential if they
  increase by a constant step, and `endSequenceRender` if its begin succeeded. A render action is
  sequential if its node's sequence is and it comes after the node's last render.
- **Renders:** before each render action, the frames needed and the regions of interest of its
  inputs are rendered (in memory, kept for the current frame), so temporal effects (e.g. Retimer)
  work. A node without tiles renders its whole RoD. An identity (isIdentity) isn't rendered: its
  output is the image of the clip and time it gives. Each input gets its image at its clip's
  depth and components.
- **Image sequences and file name patterns** (`ofxhost.sequences`, as Natron): `####` (the frame,
  zero-padded to the number of `#`), `%d`, `%0Nd`, `%V` (the view name), `%v` (its first letter,
  upper case), `%%` (a literal `%`). The frame of a time is floor(t + 0.5). They apply to the
  `Files` inputs (held at the first and last frames outside the sequence), to the output files, and
  to the plug-ins' file parameters: the output file paths (string parameters in file path mode with
  `kOfxParamPropStringFilePathExists` = 0), and the `filename` and `proxy` parameters of a reader
  or writer (a plug-in with the reader or writer context). Those hold the pattern; paramGetValue
  returns the name at the current time, paramGetValueAtTime at its time (Natron's
  `OfxStringInstance::get`), made absolute against the graph's directory, as Natron resolves them
  against the project's path. A reader's `originalFrameRange` (Integer2D), if the graph doesn't set
  it, is set from the files (by the same rule), as Natron's `Node::computeFrameRangeForReader`.
  `%V` is ofxh's name of view 0, `main` (Natron's is `Main`), and `%v` its first letter, `M`.
- **Images:** `ofxhost.imageio`: only PFM (an RGBA image is written as RGB and
  `<name>.alpha.pfm`, as hostDemo, and read back) and NumPy `.npy`; no PNG or JPEG yet. `Files`
  inputs are given to the plug-ins as float RGBA (an RGB file is opaque, a grey one r = g = b = a),
  at the project's PAR (HostSupport doesn't let the clip preferences change an input's PAR). The
  output files cover the window (the project by default), with the image at its place.
- **Report** (`schema` "ofxhost.graphReport", `schemaVersion` 1): `ok`, `error` (the message of
  the failure: a bad graph, an unknown plug-in, parameter or clip, a missing or unreadable file, an
  unsupported output format, a failed action; the run stops there, and `run()` doesn't raise), `host`, `project`, `frames`, `renderScale`, `nodes` {name: {plugin {id, version,
  label}, context, actions [{action, ok, status[, param, time]}] (createInstance,
  getClipPreferences, instanceChanged for the presses, begin/endSequenceRender),
  paramChangedCount, clipPreferences {clips {Clip: {depth, components, pixelAspectRatio,
  connected}}, outputPremultiplication, frameRate}, timeDomain {action (its status, null outside
  the general context), range (null: infinite)}, renders [{time, renderWindow, identity (null, or
  {clip, time}), ok, status}], messages [{kind, type, id, text}], params (after the run, in the
  graph's form: they can be pasted back into a graph)}}, `outputs` {node: [{time, output (w, h,
  bounds, depth, comps, min, max, mean, nan, crc32, as hostDemo's report), files}]}.
- Limits: one view (view 0), the colour plane, single-threaded renders (a plug-in that fetches its inputs on
  its own worker threads gets only those that were rendered before its render action), no
  interacts. ofxh has one host per process: `graphhost.run()` shuts down the in-process backend's
  host, and refuses to run while another one is initialized.

The command line (`python -m ofxhost`, `ofxhost/cli.py`, after pyofx's), with `-b DIR` (repeatable;
`$OFX_PLUGIN_PATH` is scanned too) and `--loglevel debug|info|warning|error` (the plug-ins'
messages are logged at info, their log messages at debug):

```sh
python -m ofxhost -b build/Plugins list [TEXT]                 # the plug-ins: id, version, label, contexts
python -m ofxhost -b build/Plugins params PLUGIN [-c CTX]       # its clips and parameters
python -m ofxhost -b build/Plugins params PLUGIN -j > g.json    # a one-node graph with the defaults
python -m ofxhost -b build/Plugins filter PLUGIN in.####.pfm out.####.pfm -s scale=2 [--save-graph g.json]
python -m ofxhost -b build/Plugins render g.json [--frames 1-10] [--scale 0.5] [--report r.json]
python -m ofxhost render g.json --dump                          # print the graph (normalised), don't render
```

`filter` renders a plug-in on an image or a sequence, in a project of the input's size with
square pixels, over the sequence's frames; `-s NAME=VALUE` takes JSON values (`1.5`, `true`,
`[1, 2]`) or strings (a choice's option label), and `-s NAME` presses a button. `params -j` writes
the template of a graph: the parameter defaults (`ofxh.param_template()`; file parameters as
`{"file": ...}`), the mandatory input clips on hostDemo's default pattern (`"default"`; the optional
ones left out, so disconnected), and the output written to
`<name>.####.pfm`; it renders as it is. Frames: `1-10`, `0-20x2`, `1,3,5`. The exit status is 0,
1 if it failed (`error: ...` on stderr: a plug-in not found, a bad graph, a missing file, a failed
render...), 2 for a usage error (the command line itself, e.g. `--scale 0`). The `ofxh` module is
imported after the command line is read: without `PYTHONPATH`, it is that of the openfx build tree
that holds the `-b` directories.

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
(see its README.md). Both directories run on the two backends (`PYTHONPATH=<build>/HostSupport/python`
for the in-process one, which a Make build doesn't make: without it, those tests are skipped), and
`tests/test_backends.py` checks that the backends give the same pixels and reports. CI runs both
in the Linux job, on both backends.
