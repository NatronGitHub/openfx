# ofxhost: render OFX plug-ins from Python

A test wrapper around the demo host `hostDemo` (HostSupport/examples), with two backends that
have the same API and the same JSON report:

- `"subprocess"` (the default): hostDemo, run as a subprocess. Each render runs in its own
  process and temporary directory: HostSupport has process-wide singletons, plug-ins can't be
  unloaded, and a crashing plug-in must not take down the interpreter. Images are exchanged as
  PFM files.
- `"inprocess"`: `ofxhost.inprocess`, hostDemo's test host ported to Python on the HostSupport
  bindings (the `ofxh` module, built with `-DOFX_PYTHON_BINDINGS=ON`;
  `PYTHONPATH=<build>/HostSupport/python`, else ofxhost looks for it in `build*/HostSupport/python`:
  the build tree of the plug-ins of `$OFX_TEST_PLUGIN_PATH`, or the only one; with several and none
  of them the plug-ins', set `PYTHONPATH`). Same inputs, actions, render windows, errors and
  report, without files: each fetch of an input gives the plug-in a new contiguous NumPy array at
  the clip's depth, made from the input (as hostDemo makes a new image per fetch; ofxh passes that
  array itself, without another copy), and outputs are the arrays it rendered into.

Choose with `backend=` (`render()`, `describe()`) or `$OFXHOST_BACKEND`. Needs Python 3.10 or later
and NumPy (and pytest for the tests).

```python
import numpy as np, ofxhost
src = np.random.default_rng(0).random((48, 64, 3), dtype=np.float32)
r = ofxhost.render("Support/Plugins/Invert/Linux-64-release", "net.sf.openfx.invertPlugin",
                   inputs={"Source": src}, depth="float")
assert np.allclose(r.outputs[0][..., :3], 1 - src, atol=1e-6)
```

- `render(bundle_paths, plugin_id, context="filter", inputs=None, params=None, depth="float",
  frames=None, scale=None, window=None, query=False, host_binary=None, timeout=60,
  extra_args=(), check=True, backend=None, components=None)` runs hostDemo (by default at frames 0 and 1) with
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
  - no files unless `extra_args` asks for them (`--format`, `-o`, `--report`, below): the report's
    `output.files` is `[]`, and an array input's `spec` is `"array:WxHxC[@x1,y1][#premult]"`; the
    report's `host.name` is ofxh's (`net.sf.openfx.ofxh`);
  - ofxh's string choice parameters refuse a value outside their enums (`could not set parameter`),
    hostDemo's accept it;
  - `timeout` is ignored (a hang hangs the caller), and a plug-in crash kills the interpreter: large
    test runs keep the subprocess backend, or run in worker processes. The crash's last output (an
    assert message, for example) is then lost with the captured stdout and stderr, which are
    unlinked temporary files: `OFXHOST_CAPTURE=0` leaves the file descriptors 1 and 2 alone (then
    `RenderResult.stdout` and `.stderr` are empty, and hostDemo's messages go to the real stderr);
  - `host_binary` is hostDemo's: with it, a call without `backend=` uses the subprocess backend
    whatever `$OFXHOST_BACKEND` says, and `backend="inprocess"` with it is a `ValueError`;
  - `extra_args`: only `--ofx14`, `--no-normalised-defaults`, `--allow-unwritten`, `-m MAJOR` and `-s ASSIGNMENT` (as
    hostDemo's, before those of `params`; e.g. to press a button twice), `--format ppm|pfm`,
    `-o PREFIX` and `--report FILE` (and the no-ops `--quiet`, `--no-cache`, `--no-std-paths`);
    anything else raises `ValueError`. With `--format` or `-o`, the outputs are also written as
    hostDemo writes them (`<prefix>.<t>[l|r].ppm`, PPM by default as hostDemo's; or `.pfm` and
    `.alpha.pfm`; `-o` defaults to `Output`), relative to the caller's current directory, and the
    report's `output.files` lists them; `--report FILE` writes the report there too, also after a
    failure. The subprocess backend refuses these three (it passes its own to hostDemo);
  - stdout and stderr are captured from the file descriptors 1 and 2 during the call (the
    plug-ins' `printf` and `std::cerr` too), and the call runs in a temporary current directory, as
    hostDemo does (relative paths in the inputs are resolved before). Both are the process's: during
    the call, what other threads write to them is captured too, and their relative paths are
    relative to the temporary directory. `render()`, `describe()` and `shutdown()` calls from several
    threads run one at a time;
  - ofxh has one host per process. The backend keeps its host between the renders with the same
    plug-in directories, profile (`--ofx14`) and options (`--no-normalised-defaults`), so the
    plug-ins stay loaded (hostDemo loads them in each run); other directories, another profile or
    other options make a new host. It refuses to run
    while another ofxh host is initialized (RuntimeError). `ofxhost.inprocess.shutdown()` releases
    its host.
  - the template of a host on ofxh is the sample host, `ofxhost/graphhost.py` (below), and
    `minimal_host.py` the minimal one; `ofxhost.inprocess` is a port of hostDemo.
- **Per-input components** (both backends): `render(..., components={"Source": "rgb"})` (`"rgb"`,
  `"alpha"`, `"rgba"`, or the `kOfxImageComponent*` names; a clip that isn't in `inputs` gets the
  default pattern), or hostDemo's `#rgb`, `#alpha` or `#rgba` suffix at the end of a spec string
  (`"ramp#rgb"`, `"checker:8#alpha"`, `"file:x.pfm#premult#rgb"`; `components=` overrides it).
  `ofxhost.split_components(spec)` splits the suffix. See `-i` below.
- Beyond hostDemo, in-process only (hostDemo is frozen):
  - **worker threads**: a plug-in's worker thread (e.g. an OpenMP render loop) may be the first to
    fetch the output image of a render, or ask for the output's RoD during it. During a render, the
    output clip's `region_of_definition()` at the render's time and view returns the RoD computed
    before the render action, so no RoD action runs on the worker (ofxh refuses actions on a worker
    thread). Another time or view still runs the action, and is refused on a worker thread. Several
    worker threads may first-fetch the output at once: the bindings keep one output image per
    render (a `get_image()` that returns after another thread made the image is dropped), so no
    image is leaked and every thread writes into the same pixels. The sample
    host does the same; a host on ofxh should too (the default `ClipInstance.region_of_definition()`
    of the output runs the action).
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
the reason; without `reason=`, the test fails); `check_backend("inprocess")`, which skips a test
or, at its top level, a whole module (or fails with `OFXH_REQUIRE=1`); and a `hostdemo` fixture
(with `backend`: the hostDemo binary on the subprocess backend, skipped if it isn't built, None
in-process, for `host_binary=`). To get them: `pytest_plugins = ["ofxhost.testing"]` in
the top-level conftest.py (pytest refuses `pytest_plugins` in the others), or
`from ofxhost.testing import backend, pytest_configure` in any conftest.py (a conftest.py with its
own `pytest_configure` calls `ofxhost.testing.pytest_configure(config)` from it instead). The
fixtures that depend on `backend` must be function-scoped (as `render` and `describe` in
Support/Plugins/tests/conftest.py).

`minimal_host.py` is not a hostDemo wrapper: it is a minimal OFX host in Python, in-process, on
the HostSupport Python bindings (the `ofxh` module, built with `-DOFX_PYTHON_BINDINGS=ON`). It renders one frame of a plug-in on one input, and is a template:
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
- Any `-i` spec may end with `#rgb`, `#alpha` or `#rgba` (after `#premult`...): the input's
  components, i.e. the clip's unmapped components (what is connected to it; default RGBA). The
  clip preferences then give the clip's components: HostSupport's default is the closest one the
  clip supports (an RGB input of an RGBA-only clip gets RGBA), and the plug-in may ask for
  another; each fetch converts the input to them. An RGB input is opaque (alpha 1 in an RGBA
  clip, premultiplication opaque); an Alpha input is the alpha of the pattern or file (a Pf
  file's value, 1 for a PF file without `+alpha.pfm`), and gives (a, a, a, a) to an RGBA clip, as
  Pf files. The report's input `unmappedComponents` and `spec` (with the suffix) say what was
  given. This lets a test cover the RGB-source code paths of a plug-in.
- `--no-normalised-defaults` makes hostDemo a host without normalised parameter defaults (a host
  from before OFX 1.2, or one that ignores them): the double param descriptors have no
  kOfxParamPropDefaultCoordinateSystem, so a plug-in's set fails (kOfxStatErrUnknown), Support's
  `supportsDefaultCoordinateSystem()` is false, and the defaults are taken as canonical
  coordinates. The report's `params` then show the values such a host gives. In Python:
  `extra_args=["--no-normalised-defaults"]` (`ofxh.init(normalised_defaults=False)`).
- `--components c,...` sets the host descriptor's kOfxImageEffectPropSupportedComponents ("the
  components supported by a clip or host"): distinct `rgba`, `rgb` or `alpha`, in order (default
  `rgba,alpha`). Only what a plug-in reads changes: the inputs and outputs may have any of the
  three, whatever it says. A plug-in that offers an RGB output only to a host that supports RGB
  (e.g. SupportExt's generators) then gets an RGB output, which the report's
  `clipPreferences.clips.Output.components` shows, and which is written as a PF file (PFM) or a
  PPM.
- With other `--components` than the default, hostDemo uses its own plug-in cache file, e.g.
  hostDemoPluginCache-components-rgba-rgb-alpha.xml (a plug-in may describe itself differently to
  such a host). In Python: `extra_args=["--components", "rgba,rgb,alpha"]`, on both backends.
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
- `--ofx11` (not with `--ofx14`, `--multiplane` or `--opengl-describe`) makes hostDemo an OFX 1.1
  host modelled on VEGAS Pro (openfx-misc's README-hosts.txt), to check that a plug-in still
  works there: the host descriptor has only the OFX 1.1 host properties, and kOfxPropAPIVersion
  1.1 (no kOfxPropVersion, kOfxImageEffectInstancePropSequentialRender or kOfxPropHostOSHandle),
  no tiles, no multi-resolution (every plug-in's inputs are cropped and padded to the project
  extent, as those of a plug-in that declares kOfxImageEffectPropSupportsMultiResolution 0 on
  both backends), no overlays or custom interacts, byte and float depths only
  (`-d short` is an error), and kOfxImageEffectPropSupportsMultipleClipPARs 0; fetchSuite gives
  the OFX 1.1 suites but the interact suite, and the message suite V2, which VEGAS Pro 12 lists
  (no progress suite V2, parametric parameter, OpenGL render or dialog suite); its persistent
  messages are refused (kOfxStatFailed, not in the report), as DaVinci Resolve and Fusion 2026
  refuse them; OfxSetHost is not called; the plug-in descriptors and
  instances have no kOfxImageEffectInstancePropSequentialRender, and the double params no
  kOfxParamPropDefaultCoordinateSystem (as `--no-normalised-defaults`); one view is rendered, as
  with `--ofx14`. As in every profile, a plug-in may only set the clip depths and PARs in its clip
  preferences if both the host and the plug-in support multiple clip depths and PARs: otherwise
  hostDemo refuses the write. The report's `host.profile` is "ofx11", and it uses its own plug-in
  cache file, hostDemoPluginCache-ofx11.xml. The in-process backend has no such profile: `python
  -m ofxhost.hostdemo --ofx11` is an error, and `ofxhost.render(backend="inprocess")` doesn't take
  it in `extra_args`.
- `--multiplane` (hostDemo built with the Nuke extension, `OFX_EXTENSIONS_NUKE`; not with
  `--ofx14`) makes hostDemo a multi-plane host, as Nuke is: the host descriptor has
  kFnOfxImageEffectPropMultiPlanar 1, with the plane suite (V1 and V2) that HostSupport
  implements. `-i Clip@Plane=spec` gives an input an extra plane: `Plane` is `ForwardMotionVector`,
  `BackMotionVector`, `StereoDisparityLeft` or `StereoDisparityRight` (the kFnOfxImagePlane*
  names without `uk.co.thefoundry.OfxImagePlane`), and `spec` any `-i` spec but `none`: the
  plane's two components (u, v or x, y) are the first two of the spec's pattern or file, at the
  clip's depth, over the clip's RoD. A plug-in fetches it with clipGetImagePlane (an image of
  kFnOfxImageComponentMotionVectors or kFnOfxImageComponentStereoDisparity components, opaque);
  a plane that isn't given gives no image. kFnOfxImageEffectPropComponentsPresent lists "the
  planes present" (nuke/fnOfxExtensions.h): the colour plane, then the given ones. Before each
  render, hostDemo calls the plug-in's GetClipComponents action, and reports the planes it needs
  and its pass-through clip; it still renders the colour plane only (the output has no other
  plane). It uses its own plug-in cache file, hostDemoPluginCache-multiplane.xml. The in-process
  backend's command line (`python -m ofxhost.hostdemo`, below) does the same, on the bindings'
  multi-plane support (Nuke extension builds): a host on `ofxh` lists an input's planes in
  `ClipInstance.components_present`, gets `get_image(..., plane="BackMotionVector")` (short names,
  as `-i`; `ofxh.ofx_name("plane", ...)`) for those, and returns (H, W, 2) arrays, and
  `ImageEffectInstance.get_clip_components_action()` runs the GetClipComponents action
  (`ofxhost.render(backend="inprocess")` doesn't take `--multiplane` in `extra_args`). No dynamic
  choices (kNatronOfxParamHostPropSupportsDynamicChoices stays 0): SupportExt's multi-plane
  plug-ins see a "V1" host, with the built-in planes only.
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
With `--multiplane`, also: `inputs` {Clip: {planes {Plane: spec}}}, `clipPreferences` {clips
{Clip: {planesPresent}}}, and in each `renders` item `isIdentity` {identityPlane} and
`clipComponents` {ok, status, clips {Clip: [plane...]}, passThroughClip, passThroughTime,
passThroughView} (the GetClipComponents action, after isIdentity).

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
them to JSON and loads them; the sample host (`ofxhost.graphhost`, below) runs them. A `Node` has a plug-in (and
optionally its `version`, `(major, minor)`), a context, parameters and inputs; an input is another
`Node`, a `Files` image sequence, or anything `render()` takes; an input clip that the node
doesn't give is disconnected. A parameter is a value, `Keys` (or a `{time: value}` dict), a
`FilePath` (the value of a file parameter), or `None` to press a push button. A `Graph` names the nodes, says
which ones are its outputs (and the file each one is written to), and holds the project and the
render settings.

```python
from ofxhost import graphhost
from ofxhost.graph import Files, Graph, Keys, Node, load
src = Node("net.sf.openfx.invertPlugin", inputs={"Source": Files("in.####.pfm")})
out = Node("net.sf.openfx.basicPlugin", "general", {"scale": Keys({0: 1., 10: 2.})},
           inputs={"Source": src, "Mask": src})
g = Graph.of(out, frames=[0, 1], scale=0.5)   # names the nodes ("invertPlugin", "basicPlugin")
g.outputs["basicPlugin"] = "out.####.pfm"     # where the sample host writes it
g.save("graph.json")                          # load("graph.json") gives an equivalent Graph
images = graphhost.run(g, bundles).images["basicPlugin"]   # {0.0: Image, 1.0: Image}
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
                  "params": {"scale": {"keys": [[0.0, 1.0], [10.0, 2.0]]},
                             "scaleComponents": false},
                  "inputs": {"Source": {"node": "invertPlugin"}, "Mask": {"node": "invertPlugin"}}}}}
```

- `project`: the project (the defaults are hostDemo's, above; the pixel aspect ratio is positive).
- `render`: `frames` (null: every frame of the outputs' time domains), `renderScale`, `window`
  (pixels at the render scale; null: the project), and `depth` (of the patterns and unconnected
  inputs: byte, short or float).
- `outputs`: each output node, and the file name pattern it is written to by the sample host
  (null: rendered only; e.g. a writer plug-in, which writes its own files).
- `params`: values as `ofxh.param_template()` gives them (numbers, booleans, strings, lists; a
  choice's index, or the label of one of its options), keys, `{"file": path}` (a file parameter's
  value, which `save()` rewrites as it does the file inputs), or null (press a push button).
  Keys are interpolated as hostDemo and ofxh do: linearly, rounded for integers, held for booleans
  and choices.
- `inputs`: `{"node": name}`, `{"file": pattern}` (an image sequence, or a still image; a
  `#premult`, `#unpremult` or `#opaque` suffix gives its premultiplication), or a `render()` string:
  a test pattern (`"default"`, `"ramp"`, `"checker:16"`, `"solid:r,g,b,a"`, `"rod:..."`), `"none"`,
  or `"file:x.pfm"`. **An input clip that the graph doesn't give is disconnected**, as in a real host:
  the test patterns must be asked for.
- Relative paths (file inputs, outputs and file parameters) are relative to the graph file's
  directory (`Graph.base`). `load()` keeps them as they are, so that saving the loaded graph gives
  the same bytes; `save()` to another directory rewrites them for that directory (absolute when
  there is no relative path, and `"file:"` specs absolute), so that the saved graph still finds its
  files.
- `version` is `[major, minor]`: the latest version of that major is used; without it, the latest.
- Unknown keys are refused (in the nodes and at the top level).
- A graph built in Python, saved, then loaded, is equivalent (the same nodes, versions,
  connections, values and keys, outputs and settings), and saving it again gives the same bytes.
  Arrays and `Image` inputs can't be saved (`ValueError`): write them to files.
- **Trust:** a graph is like a script: it chooses the plug-ins that run, with their parameters,
  and the files they read and write, anywhere you can. Run only the graphs you trust.

`python -m ofxhost render graph.json` runs a graph file (see below).

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
  increase by a constant step, and `endSequenceRender` if its begin succeeded (an input's frames
  are planned over the regions of interest of the nodes that use it). A render action is
  sequential if its node's sequence is and it comes after the node's last render. A render outside
  a begun sequence that covers its time (for a push button pressed during the setup, or an
  unplanned fetch) gets its own begin and end over [t, t], not sequential, as Natron's analysis
  renders, since the render action requires "kOfxImageEffectActionBeginSequenceRender has been
  called on the instance". These may nest: an unplanned fetch at a time outside the node's open
  [first, last] sequence gets its [t, t] begin and end inside the open one (the spec says nothing
  about nested sequence renders; Natron's analysis renders nest the same way). A time inside
  [first, last] but off the step counts as covered. Each render is also in the node's `actions` of
  the report. The planning keeps the union of the windows asked of each node at each time, and
  plans a node again only when that union grows, so deep diamond-shaped graphs whose regions of
  interest differ along each path don't plan a node once per path.
- **Renders:** before each render action, the frames needed and the regions of interest of its
  inputs are rendered (in memory, kept for the current frame), so temporal effects (e.g. Retimer)
  work. A node without tiles renders its whole RoD, from whole-RoD inputs (a RoD too large for an
  image: the window, or the region of interest, as hostDemo). An identity (isIdentity) isn't rendered: its
  output is the image of the clip and time it gives. Each input gets its image at its clip's
  depth and components.
- **Image sequences and file name patterns** (`ofxhost.sequences`, as Natron): `####` (the frame,
  zero-padded to the number of `#`), `%d`, `%0Nd`, `%V` (the view name), `%v` (its first letter,
  upper case), `%%` (a literal `%`). The frame of a time is floor(t + 0.5). They apply to the
  `Files` inputs (held at the first and last frames outside the sequence), to the output files
  (two times that give the same file name fail the run), and to the plug-ins' file parameters: the
  output file paths (string parameters in file path mode with `kOfxParamPropStringFilePathExists` =
  0), and the `filename` and `proxy` parameters of a reader or writer (a plug-in with the reader or
  writer context). Those hold the pattern; paramGetValue
  returns the name at the current time, paramGetValueAtTime at its time (Natron's
  `OfxStringInstance::get`), made absolute against the graph's directory, as Natron resolves them
  against the project's path. A reader's `originalFrameRange` (Integer2D), if the graph doesn't set
  it, is set from the files (by the same rule), as Natron's `Node::computeFrameRangeForReader`.
  `%V` is ofxh's name of view 0, `main` (Natron's is `Main`), and `%v` its first letter, `M`.
- **Images:** `ofxhost.imageio`: only PFM (an RGBA image is written as RGB and
  `<name>.alpha.pfm`, as hostDemo, and read back) and NumPy `.npy`; no PNG or JPEG yet. `Files`
  inputs are given to the plug-ins as float RGBA (an RGB file is opaque, a grey one r = g = b = a),
  at the project's PAR (HostSupport doesn't let the clip preferences change an input's PAR), unless
  the input has a components suffix: `{"file": "in.####.pfm#rgb"}` (`Files("in.####.pfm#rgb")`,
  after an optional `#premult`...) or `"ramp#alpha"` gives the clip RGB or Alpha as its unmapped
  components, as in the in-process backend. The output files cover the window (the project by
  default), with the image at its place.
- **Report** (`schema` "ofxhost.graphReport", `schemaVersion` 1): `ok`, `error` (the message of
  the failure: a bad graph, an unknown plug-in, parameter or clip, a missing or unreadable file, an
  unsupported output format, a failed action; the run stops there, and `run()` doesn't raise), `host`, `project`, `frames`, `renderScale`, `nodes` {name: {plugin {id, version,
  label}, context, actions [{action, ok, status[, param, time]}] (createInstance,
  getClipPreferences, instanceChanged for the presses, begin/endSequenceRender[, first, last, step,
  sequential], render [, time, sequential]),
  paramChangedCount, clipPreferences {clips {Clip: {depth, components, pixelAspectRatio,
  connected}}, outputPremultiplication, frameRate}, timeDomain {action (its status, null outside
  the general context), range (null: infinite)}, renders [{time, renderWindow, identity (null, or
  {clip, time}), ok, status}], messages [{kind, type, id, text}], params (after the run, in the
  graph's form: they can be pasted back into a graph)}}, `outputs` {node: [{time, output (w, h,
  bounds, depth, comps, min, max, mean, nan, crc32, as hostDemo's report), files}]}.
- Limits: one view (view 0), the colour plane, single-threaded renders (a plug-in that fetches its inputs on
  its own worker threads gets only those that were rendered before its render action; its worker
  threads may fetch the output image first, see the in-process backend), no interacts. ofxh has one host per process: `graphhost.run()` shuts down the in-process backend's
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

## hostDemo's command line, in-process (`ofxhost.hostdemo`)

All the plug-in repositories drive hostDemo through its command line, from shell scripts, CMake
scripts or Python subprocesses. `ofxhost.hostdemo` is that command line on the in-process
backend, a drop-in for the hostDemo binary:

```sh
python -m ofxhost.hostdemo [hostDemo's options] [[-s] name=value]...
HostSupport/examples/python/hostdemo-inprocess [hostDemo's options]...    # the same, as a script
```

It takes all of hostDemo's options (`-h` lists them) with the same meaning and errors, reads the
plug-ins of `$OFX_PLUGIN_PATH` (and the standard locations without `--no-std-paths`), keeps them
loaded after the scan with `OFX_HOSTDEMO_KEEP_LOADED=1` (`ofxh.init(keep_loaded=True)`), writes the
same output files (`<prefix>.<t>[l|r].ppm`, or `.pfm` and `.alpha.pfm`) and the same `--report`
and `--describe` JSON, prints the plug-ins' messages on stdout as hostDemo (`<type> <id> <text>`,
also with `--quiet`), and has the same exit status (0, or 1 on failure). Like hostDemo, it runs in
the current directory, one run per process (so a harness keeps its crash isolation, timeouts and
parallel jobs; the batch mode below runs many in one Python), and unloads the plug-ins at the end, after writing the report. `tests/test_hostdemo_cli.py`
runs the consumers' command lines with both and compares the files byte for byte, the reports and
the error messages. The `ofxh` module is that of `PYTHONPATH`, else of the build tree that holds
the `$OFX_PLUGIN_PATH` directories.

What differs from hostDemo (the complete list; `tests/test_hostdemo_cli.py` checks the rest):
- the report's `host.name` is ofxh's (`net.sf.openfx.ofxh`);
- the report's `plugin.filePath` is absolute (ofxh's plug-in paths are; hostDemo's is the path as
  found in `$OFX_PLUGIN_PATH`);
- numbers in the JSON are written by Python: `0.0`, not `0`, and the shortest repr instead of
  `%.17g`. NaN and infinities are `null`, as in hostDemo, and the JSON is strict;
- the `mean` statistics can differ from hostDemo's in their last bits (NumPy's pairwise summation;
  `min`, `max`, `nan`, `crc32` and the files are the same);
- during a render, an output image fetch, and the output's RoD asked for at the render's time, run
  no nested RoD action: the host uses the RoD it computed before the render action (see the worker
  threads above). hostDemo runs the plug-in's RoD action again, so a plug-in that traces its actions
  (e.g. the property tester with `OFX_PROPTESTER_TRACE=1`) prints fewer RoD actions;
- `--no-cache` is a no-op (ofxh has no cache file), and without `--quiet` the list of the plug-ins
  is shorter than hostDemo's dump;
- ofxh's string choice parameters refuse a value outside their enums (hostDemo's accept it);
- `--ofx11` is an error (ofxh has no OFX 1.1 host profile);
- an error in the Python host itself (a bug: an uncaught Python exception) exits with status 1 and a
  traceback on stderr, and writes no report;
- beyond hostDemo: `$OFXHOST_PRELOAD` lists libraries (`os.pathsep`-separated) to load with
  `RTLD_GLOBAL` before the plug-in scan: e.g. MacPorts' `libtbbmalloc` for IO.ofx, since oneTBB
  2023.2 can crash at exit when OpenImageIO loaded it.

As hostDemo, it reads the whole command line before it imports `ofxh`: `-h` and the usage errors
behave the same where `ofxh` isn't built. Only `--multiplane` imports it at the option, to refuse it
there, as hostDemo does, when `ofxh` has no Nuke extension.

**Windows** (MSYS2 UCRT64, a native `python.exe`): run `python -m ofxhost.hostdemo ARGS...` with
Windows paths in `PYTHONPATH`, separated by `;`:
`PYTHONPATH="$(cygpath -w <build>/HostSupport/python);$(cygpath -w <openfx>/HostSupport/examples/python)"`,
and `OFX_PLUGIN_PATH` as for hostDemo (`cygpath -w -p`). From an MSYS2 shell script, the
`hostdemo-inprocess` script works too (`OFXHOST_PYTHON=python`): it converts its own directory with
`cygpath -w` and joins `PYTHONPATH` with `;` when `cygpath` exists. A native program (CMake's
`execute_process`, Python's `subprocess`) can't run the sh script: give it the
`python -m ofxhost.hostdemo` command. CI runs a describe and a small render both ways on UCRT64,
and compares them with hostDemo.exe.

### Batch mode: many command lines in one Python

Each run of `python -m ofxhost.hostdemo` starts Python and imports NumPy and `ofxh` (0.1 to 0.2 s
here) before it scans the plug-ins. The batch mode pays that once per worker process instead of
once per command line:

```sh
python -m ofxhost.hostdemo --batch cmds.txt -j 4 --timeout 120 > records.jsonl   # or --batch - (stdin)
```

```python
from ofxhost.hostdemo import run_batch
records = run_batch(["--quiet -p net.sf.openfx.invertPlugin -o a --report a.json",
                     ["--quiet", "-p", "net.sf.openfx.invertPlugin", "--describe", "d.json"]],
                    jobs=4, timeout=120)          # host="fresh" (the default), "per-plugin" or "reuse"
```

- **The commands**: one hostDemo command line per line, without the program name, split as a POSIX
  shell does (`shlex`, on every platform: on Windows write the paths with `/`, or quote them, since
  `\` escapes; `#` is literal inside a line, as in `-i Source=ramp#rgb`). Blank lines and lines
  whose first non-blank character is `#` are ignored. A line may start with `--cwd DIR` (its
  directory: a native path, relative to the batch's), `--env NAME=VALUE` (`--env NAME` unsets it),
  e.g. `--env OFX_PLUGIN_PATH=...`, and `--timeout SECONDS` (its own timeout). No shell syntax:
  `cd DIR && ...`, pipes and redirections are not supported. `run_batch()` also takes argument
  lists, `Command` objects and dicts `{"args", "cwd", "env", "timeout"}`.
- **Each command runs as a separate run would**: the same output files and `--report` and
  `--describe` JSON, relative to its directory, the same exit status, and its stdout and stderr
  captured at the file-descriptor level (the plug-ins' `printf` and `std::cerr` too), into files
  that survive the death of the worker.
- **The records**: one JSON line per command, in the order of the commands, on stdout (or in
  `--batch-report FILE`), and the list that `run_batch()` returns:
  `{"line", "args", "cwd", "env", "exitStatus", "ok", "stdout", "stderr", "seconds", "crashed",
  "timedOut", "worker", "seq"}`.
  - `crashed`: the worker died during the command (a signal, or a plug-in that called `exit()`), and
    the record has what the command printed before. `timedOut`: it ran longer than its timeout; the
    worker first printed the stacks of its Python threads (faulthandler) to the command's stderr,
    then it was killed, with its process group. In both cases the worker is replaced and the batch
    goes on. **Match `crashed` and `timedOut`, not `exitStatus`**, which depends on the platform: on
    POSIX minus the signal (-11 for SIGSEGV, -9 for a timeout), on Windows the NTSTATUS of a crash
    (e.g. 3221225477, an access violation) and 1 for a timeout.
  - `seconds`: the command's time in its worker, without the start of the worker (Python, NumPy);
    a worker's first command still includes the import of `ofxh` (0.1 to 0.2 s), which depends on
    its `$OFX_PLUGIN_PATH`. `worker`:
    `[slot, generation]` (the pool's slot, 0 to N-1, and which of its workers: 2 after the first was
    replaced...); `seq`: the command's position in that worker (the commands before it in the same
    process). `env`: its `--env` changes. `workerStderr` (crashed and timed-out commands): what the
    worker printed outside of the commands, e.g. while it shut a host down. `internalError` (a bug
    of the batch; the command may not have run): the traceback.
  - Worker events follow the command records, with `"line": null` and `"event"`: `"died"` (a worker
    died or hung after its previous command and before the next one started, e.g. in a plug-in's
    unload action when its host was shut down: the command runs again in a new worker, once),
    `"exit"` (a worker failed when it shut down at the end of the batch), `"start"` (a worker didn't
    start). They have the worker's stderr, and the `seq` of the last command that the worker ran.
  - A summary goes to stderr. The exit status is 0 if every command succeeded and no worker event
    happened, 1 otherwise, 2 for a usage error of the batch options (also an unopenable
    `--batch-report`), 3 for an internal error (e.g. the records can't be written: `| head`).
- **`-j N`** worker processes (default 1, `0`: one per CPU) take the commands in order, each the
  next one when it is free. They are subprocesses that speak JSON lines over pipes (the same on
  macOS, Linux and Windows), run in the batch's temporary directory with ofxhost's directory first
  on `sys.path` (a file of the batch's directory can't shadow a module; `PYTHONPATH` still applies),
  in their own process group (POSIX), which the batch kills when a worker is replaced, and when the
  batch gets SIGTERM or SIGHUP. A worker also ends if the batch dies without killing it (SIGKILL):
  it watches its parent (and Linux sends it SIGKILL, `PR_SET_PDEATHSIG`). On Windows, closing the
  console ends the workers too (they share it); a batch killed by `TerminateProcess` leaves its
  workers to end at their next read of a request, after the command that runs.
- **The host's lifetime** (isolation against speed):
  - default (`host="fresh"`): the host is shut down after each command, as at the end of a separate
    run (unload action, binaries unloaded): each command gets its own scan, load and describe, as
    with hostDemo. What survives the unloading of a binary is still shared by a worker's commands:
    the libraries it loaded, and the globals of the binaries that the OS doesn't really unload
    (`dlclose()` may keep a binary loaded: on Linux one with GNU unique C++ symbols, on macOS one
    with Objective-C code);
  - `--fresh-host-per-plugin` (`host="per-plugin"`): the host is kept while the plug-in (`-p`) is
    the same as the worker's previous command's;
  - `--reuse-host` (`host="reuse"`): a worker keeps its host while the plug-in directories, the
    profile (`--ofx14`), `OFX_HOSTDEMO_KEEP_LOADED` and `--no-std-paths` stay the same (the
    in-process backend starts a new host when they change). The fastest, but the commands of a
    worker share the plug-ins' globals, caches and static data. The plug-in scan, which sends every
    plug-in its load, describe and unload actions, runs once per host, so what plug-ins print
    during it (e.g. a describe that fails) is only in the stderr of the host's first command; a
    plug-in's first use in the worker loads it again (load and describe), and it gets its unload
    action when the host shuts down;
  - in the per-plugin and reuse modes, a host is shut down before the next command starts, on the
    worker's own stdout and stderr: what the unload actions print, or a crash there, is not the
    next command's (the crash is a `"died"` event);
  - one process per command line (`python -m ofxhost.hostdemo`, or hostDemo) isolates the runs
    completely.

  With `-j N` the commands that share a worker are not known in advance (`worker` and `seq` say
  which they were), so a test whose result depends on what ran before it in the same process is a
  bug that the batch can show.
- Also shared by a worker: the `ofxh` module, imported once (the first command's `$OFX_PLUGIN_PATH`
  chooses its build tree, see above), and the `$OFXHOST_PRELOAD` libraries.
- Test hook, for ofxhost's tests only: `--env OFXHOST_BATCH_TEST_HOOK=segv|kill|exit|hang|raise|host|unload|unload-crash`
  (the command's own `--env`, not an inherited variable) makes the worker crash, hang... after the
  command (`ofxhost/_batch.py`).

## OFX constants

`ofxhost.constants` has every `#define` of `include/` (strings, status codes, key symbols...),
with its C name:

```python
from ofxhost import constants as k
k.kOfxImageEffectPropSupportedPixelDepths   # 'OfxImageEffectPropSupportedPixelDepths'
```

It is generated from the headers by `scripts/gen-ofxhost-py.py` (needs PyYAML); run it after
changing a header. CI fails if it is out of date, and `scripts/check-props.py` fails if a
`#define` of `include/` is neither a property with a `@propdef` nor classified in
`scripts/ofx_fork_props.py` (enum value, alias, or name).

## Tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLE_PLUGINS=ON -DBUILD_EXAMPLE_HOSTS=ON \
    -DOFX_PYTHON_BINDINGS=ON && cmake --build build -j4      # (or the Makefiles: no ofxh module)
OFX_TEST_PLUGIN_PATH=build/Plugins PYTHONPATH=build/HostSupport/python OFXHOST_HOSTDEMO=build/HostSupport/examples/hostDemo \
    python3 -m pytest -q -rs -p no:cacheprovider HostSupport/examples/python/tests Support/Plugins/tests
```

Support/Plugins/tests has tests of the sample plug-ins, meant as templates for plug-in repos (see
its README.md). Both directories run on the two backends (without the ofxh module, the in-process
tests are skipped; `OFXH_REQUIRE=1` makes them fail instead, as in CI), and `tests/test_backends.py`
checks that the backends give the same pixels and reports. `tests/test_hostdemo_cli.py` runs
hostDemo and `ofxhost.hostdemo` on the same command lines (those of the plug-in repositories) and
compares their files, reports and errors; `tests/test_beyond_hostdemo.py` covers per-input
components, the files of `extra_args`, a worker thread that fetches the output image first, and the
sample host's planning; `tests/test_graph.py` and `tests/test_graphhost.py` the graphs and the
sample host; `tests/test_hostdemo_batch.py` the batch mode (its parser, the same files, reports and
output as separate runs, the host's lifetime, and workers that crash or hang). CI runs them in the
Linux job, on both backends.

## Developer notes

- Python code follows the [Google Python Style Guide](https://google.github.io/styleguide/pyguide.html),
  with lines of up to 120 columns ([line length](https://google.github.io/styleguide/pyguide.html#s3.2-line-length)
  is 80 there). `HostSupport/python/pyproject.toml` says so to ruff (`line-length`, and E501, which
  ruff doesn't check by default; the generated `constants.py` is excluded). The
  Linux CI job runs `ruff check --config HostSupport/python/pyproject.toml` on these directories,
  `Support/Plugins/tests` and the fork's own scripts (see `.github/workflows/build.yml`).
- The helpers shared by the backends and the sample host are public modules of the package:
  `ofxhost.rects` (rectangles), `ofxhost.pixels` (arrays, depths, statistics), `ofxhost.specs`
  (hostDemo's input specs and patterns), `ofxhost.pfm`, `ofxhost.status` (status names),
  `ofxhost.stdio` (the file descriptors 1 and 2) and `ofxhost.build` (the build trees). OFX names
  come from `ofxhost.constants`.
