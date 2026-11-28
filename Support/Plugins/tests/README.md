# Tests of the sample plug-ins

pytest tests of the Support/Plugins samples, rendered by the demo host `hostDemo` through the
`ofxhost` Python wrapper (see HostSupport/examples/python/README.md for its API and for
hostDemo's inputs, report and describe JSON). Each test runs on both ofxhost backends (the
`render` and `describe` fixtures of `conftest.py`): hostDemo in a subprocess, and in-process on
the HostSupport Python bindings (`PYTHONPATH=<build>/HostSupport/python`; skipped if `ofxh` isn't
built, and `OFXHOST_TEST_BACKENDS=subprocess` runs only hostDemo). Each file shows one testing pattern, named in
its docstring, and each test says what it checks, why, and the pull request or issue (`#N`: NatronGitHub/openfx)
that added it. `functional/` has the tests of what the samples and the Support library must do; `regression/` the
tests of past bugs, each naming its issue or pull request (a regression test stays as long as the bug could come
back):

| File | Pattern |
|---|---|
| functional/test_invert.py | exact pixels at byte, short and float depth (`assert_close`); the report |
| functional/test_basic.py | parameters; a mask clip (an image, or disconnected); instanceChanged enabling params |
| functional/test_multibundle.py | two plug-ins in a bundle: a formula (gamma), an output RoD that is not the input's (dot) |
| functional/test_generator.py | no input; an infinite RoD; deterministic output |
| functional/test_retimer.py | a temporal effect: frames needed, output depending on time and speed, time domain |
| functional/test_transition.py | two inputs and a transition value |
| functional/test_field.py | unfielded rendering; the plug-in's stdout |
| functional/test_gpugain.py | the CPU path of a plug-in with Metal, CUDA, OpenCL and OpenGL render: rounding and clamping at byte and short depth; RGB and Alpha inputs (`components=`); an identity |
| functional/test_choiceparams.py | Choice values are indices whatever the option order; a StrChoice value is its enum |
| functional/test_describe.py | contexts, clips, parameters and flags, from `describe()` |
| regression/test_tester.py | the host's report: the progress calls that reached the host |
| functional/test_ofx14.py | an OFX 1.4 host (hostDemo `--ofx14`): the plug-in's fallbacks for the OFX 1.5 features |
| functional/test_ofx11.py | an OFX 1.1 host such as VEGAS Pro (hostDemo `--ofx11`, subprocess only: `@pytest.mark.backends("subprocess", reason=...)`): the Support library without the OFX 1.2+ properties, with a single clip PAR |
| regression/test_entry_point.py | the C entry point called directly (ctypes, in a subprocess, so that a crash fails the test): a null action, an unknown action |

The Tester sample only has describe checks (its render is an invert, and its push buttons
need an interactive host), and a check of the progress calls of its render (regression/test_tester.py).
Support/PropTester isn't tested: it writes no pixels (hostDemo would fail on the unwritten
output), and it only dumps properties to its log.

Run them (after `make -C HostSupport && make -C HostSupport/examples && make -C
Support/Plugins CONFIG=release`):

```sh
python3 -m venv /tmp/venv && /tmp/venv/bin/pip install numpy pytest
/tmp/venv/bin/python -m pytest -q -p no:cacheprovider Support/Plugins/tests
```

The `__init__.py` files make these modules a package (`tests.functional.test_describe`...). pytest imports a test
module of a directory without `__init__.py` by its basename, so two such modules with the same basename can't be
collected in one run: here `functional/test_describe.py` would clash with
HostSupport/python/tests/functional/test_describe.py when the three openfx suites run together. In a plug-in repo,
keep them if your tests may run with other suites that have test modules of the same names.

## Using these as templates for your plug-in repo

1. Copy `conftest.py` and the test file closest to your plug-in into a `tests/` directory of
   your repo (openfx-misc, openfx-io, openfx-arena...), and replace the plug-in identifier,
   clip and parameter names, and the expected images. Keep the layout: `conftest.py` in `tests/`,
   the tests in `tests/functional/` and `tests/regression/` (with an `__init__.py` in each, if
   their names could clash with other tests run in the same pytest).
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
