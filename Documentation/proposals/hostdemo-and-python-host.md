# hostDemo and the Python host: what to do about #136 part 5b

Status: decided by a vote of the plug-in repositories (2026-11-23): **B**, unanimously.

## Problem

#136 planned to turn the C++ `hostDemo` back into a simple example once the plug-in repositories' tests had moved to the Python host (`ofxhost` on the `ofxh` bindings; parts 1 to 5a are merged). Part 5b, "strip hostDemo", is the last step. But the plug-in tests depend on hostDemo as it is:

- **Its command line**, in every repository: misc's rendertest and smoketest (which also run openfx-supportext's per-PR CI, with and without `--ofx14`), io's `io-hostdemo` tests (`tests/hostdemo/run.sh`, the `--report` JSON), arena's ctests, gmic's describe and render checks.
- **Its `MyHost::*` classes** (the four `HostSupport/examples/hostDemo*.{h,cpp}` files), compiled into C++ test hosts: arena `mt_unload` and `reader_window`; io `tests/hostdemo/reader_window.cpp`, `raw_unload` and `exit_without_unload`; openfx's `propsetDump` and `suiteHandleChecks`. Some can't move to Python: `raw_unload` checks that nothing keeps OpenMP loaded after unload, which a Python process with NumPy can't check.
- **Its platforms:** gmic runs hostDemo on Linux, macOS and Windows x64 with MSVC, and weekly on Windows on Arm (ARM64 and Arm64EC hosts). `ofxh` has never been built with MSVC (Arm64EC would need an emulated x64 CPython: untested). misc, io and arena test Windows with MSYS2 UCRT64, where `ofxh` already builds.

Moving to the Python host would add nanobind, NumPy (where not already used), the Python ≥ 3.10 floor (Rocky 9 ships 3.9) and an `ofxh` build to the consumers' CI. It is also slower per process: misc's rendertest (236 command lines, macOS) takes 46.9 s on `python -m ofxhost.hostdemo` against 35.4 s on hostDemo serially, and 14.9 s against 13.0 s at four at a time (`HostSupport/examples/python/README.md`). The batch mode is faster, but no consumer uses it, and moving smoketest to it would be a rewrite. gmic also keeps its own C++ describe host (`describe_plugins.cpp`, on HostSupport), whatever the option.

## Options

**A. Migrate everything, then strip hostDemo.** One test host, eventually. But the C++ test hosts and gmic's Windows/ARM coverage can't move, the Python host's subprocess backend and its comparison tests run hostDemo, and every consumer's CI gets heavier.

**B. Keep hostDemo as the frozen test host; the Python host is optional.** No change for anyone; every platform keeps working. Host-side fixes keep landing in both hosts.

**C. Split instead of strip:** a small `OfxTestHost` library for the C++ test hosts, a separate one-page C++ example host, and only openfx-misc (and so openfx-supportext's CI) moves to the Python host. For io and arena it only replaces four source files with a library to link.

## Decision: B

"Frozen" means no new hostDemo features; bug fixes and spec-compliance fixes still land (as in #209 and #213). What stays stable for the consumers:

- the `hostDemo` CMake target (`BUILD_EXAMPLE_HOSTS`, `OFX_BUILD_HOSTSUPPORT`) and the Makefile build;
- the command-line options and environment variables the plug-in tests use: `--describe`, `--report` (JSON fields such as `renders[].output.files` and `.nan`), `--ofx14`, `--no-std-paths`, `--no-cache`, `--quiet`, `--allow-unwritten`, `--format pfm`, `-d`, `-p`, `-c`, `-s`, `--scale`, `--frames`, `--window`, `-o`, `-i Clip=solid|ramp|checker|none|rod:…|file:…`, PFM inputs and output, both stereo views, the output file naming, a non-zero exit on render errors, `OFX_HOSTDEMO_KEEP_LOADED`;
- the `MyHost::*` classes for the C++ test hosts: the four `hostDemo*` files and `MyHost::Host`, `MyEffectInstance`, `MyClipInstance::getOutputImage`, `MyImage`, `canonicalToPixelEnclosing` and `gImageErrors` stay source-compatible (this gives the test hosts what C's library would, without a library);
- HostSupport's comment anchors that gmic's describe host patches (`ofxhParam.cpp` "kOfxStatErrExists - if a parameter of that name exists already in this plugin", `ofxhImageEffect.cpp` "// clip names are unique").

The Python host stays for the tests hostDemo can't do. Known gap: hostDemo can't give a plug-in padded or negative `kOfxImagePropRowBytes` (openfx-misc#232 was tested with the `ofxh` bindings); a small hostDemo option is the cheap fix if CI ever needs it.

Revisit A or C only if a repository needs something hostDemo can't give it.
