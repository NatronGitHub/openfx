# Tests of the sample plug-ins

pytest tests of the Support/Plugins samples, rendered by the demo host `hostDemo` through the
`ofxhost` Python wrapper (see HostSupport/examples/python/README.md for its API and for
hostDemo's inputs, report and describe JSON). Each test runs on both ofxhost backends (the
`render` and `describe` fixtures of `conftest.py`): hostDemo in a subprocess, and in-process on
the HostSupport Python bindings (`PYTHONPATH=<build>/HostSupport/python`; skipped if `ofxh` isn't
built, and `OFXHOST_TEST_BACKENDS=subprocess` runs only hostDemo). Each file shows one testing pattern, named in
its docstring:

| File | Pattern |
|---|---|
| test_invert.py | exact pixels at byte, short and float depth (`assert_close`); the report |
| test_basic.py | parameters; a mask clip (an image, or disconnected); instanceChanged enabling params |
| test_multibundle.py | two plug-ins in a bundle: a formula (gamma), an output RoD that is not the input's (dot) |
| test_generator.py | no input; an infinite RoD; deterministic output |
| test_retimer.py | a temporal effect: frames needed, output depending on time and speed, time domain |
| test_transition.py | two inputs and a transition value |
| test_field.py | unfielded rendering; the plug-in's stdout |
| test_multiplygpu.py | the CPU path of a GPU-capable plug-in, with rounding at byte depth |
| test_gpugain.py | the CPU path of a plug-in with Metal, CUDA and OpenCL render, float only; an identity |
| test_choiceparams.py | Choice values are indices whatever the option order; a StrChoice value is its enum |
| test_describe.py | contexts, clips, parameters and flags, from `describe()` |
| test_tester.py | the host's report: the progress calls that reached the host |
| test_ofx14.py | an OFX 1.4 host (hostDemo `--ofx14`): the plug-in's fallbacks for the OFX 1.5 features |

The Tester sample only has describe checks (its render is an invert, and its push buttons
need an interactive host), and a check of the progress calls of its render (test_tester.py).
Support/PropTester isn't tested: it writes no pixels (hostDemo would fail on the unwritten
output), and it only dumps properties to its log.

Run them (after `make -C HostSupport && make -C HostSupport/examples && make -C
Support/Plugins CONFIG=release`):

```sh
python3 -m venv /tmp/venv && /tmp/venv/bin/pip install numpy pytest
/tmp/venv/bin/python -m pytest -q -p no:cacheprovider Support/Plugins/tests
```

`__init__.py` makes these modules a package, so that they don't clash with
HostSupport/examples/python/tests/test_basic.py when both directories are run together.

## Using these as templates for your plug-in repo

1. Copy `conftest.py` and the test file closest to your plug-in into a `tests/` directory of
   your repo (openfx-misc, openfx-io, openfx-arena...), and replace the plug-in identifier,
   clip and parameter names, and the expected images.
2. Edit the three settings at the top of `conftest.py`. With `tests/` at the root of a repo
   that has openfx as a submodule, and the Makefile builds:
   ```python
   REPO = Path(__file__).resolve().parents[1]
   OPENFX = REPO / "openfx"
   BUNDLE_DIRS = sorted(REPO.glob("*/*-release"))  # e.g. Misc/Linux-64-release/Misc.ofx.bundle
   ```
3. Build hostDemo from your openfx submodule:
   `make -C openfx/HostSupport && make -C openfx/HostSupport/examples`. ofxhost finds it in
   `openfx/HostSupport/examples/*-release/`; set `OFXHOST_HOSTDEMO=/path/to/hostDemo` to use
   another one.
4. Run: `pip install numpy pytest` (Python 3.10 or later), then `python -m pytest -q tests`.
   The tests are skipped if hostDemo or the bundles aren't built.

Use a few small images (hostDemo writes and reads them as PFM files), and compare with a
formula or with another render rather than with stored reference images.

CI: don't add a job. Add a step to the existing Linux job, after the plug-ins are built:

```yaml
      - name: Plug-in tests (hostDemo)
        run: |
          make -C openfx/HostSupport && make -C openfx/HostSupport/examples
          python3 -m venv /tmp/venv && /tmp/venv/bin/pip install -q numpy pytest
          /tmp/venv/bin/python -m pytest -q -p no:cacheprovider tests
```
