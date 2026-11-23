# hostDemo and the Python host: what to do about #136 part 5b

Status: proposal, for a vote of the plug-in repositories (openfx-supportext, -misc, -io, -arena, -gmic).

## Problem

#136 planned to turn the C++ `hostDemo` back into a simple example once the plug-in repositories' tests had moved to the Python host (`ofxhost` on the `ofxh` bindings; parts 1 to 5a are merged). Part 5b, "strip hostDemo", is the last step. But the plug-in tests still depend on things only hostDemo or C++ provide:

- C++ test hosts built on hostDemo's `MyHost::*` classes: arena `mt_unload` and `reader_window`, io `reader_window`, `raw_unload` and `exit_without_unload`, and openfx's `propsetDump` and `suiteHandleChecks`.
- gmic runs hostDemo on MSVC x64, ARM64 and Arm64X. `ofxh` has never been built there (and may not be possible for Arm64EC).
- Moving to the Python host adds nanobind, NumPy and Python ≥ 3.10 to every consumer's CI.
- One process per command is slower than hostDemo (misc's rendertest: 77 s vs 56 s on macOS); the batch mode is faster but no consumer uses it yet.

## Options

**A. Migrate everything, then strip hostDemo.** One test host. But the C++ test hosts and gmic's Windows/ARM coverage can't move, every CI gets heavier, and the bindings lose their reference implementation. About 3–5 weeks, with an uncertain MSVC/ARM part.

**B. Keep hostDemo as the frozen test host; the Python host stays optional.** No change for anyone, every platform keeps working. #136's "simple example" goal is dropped, and host-side fixes keep landing in both hosts.

**C. Split instead of strip.** Keep hostDemo frozen as the test host and reference; move the `MyHost::*` classes into a small test-host library (`OfxTestHost`) that hostDemo and the consumers' C++ test hosts link; add a separate one-page C++ example host. Repositories move to the Python host only where it helps (openfx-misc first; gmic, io and arena stay on hostDemo). Adds a library and a third host program to maintain.

## Recommendation

**B**, under the "no over-engineering" rule: it costs nothing, loses no coverage, and nothing today needs a simpler hostDemo. A small "minimal host" already exists in Python (`HostSupport/examples/python/minimal_host.py`). Revisit A or C only if a repository needs something hostDemo can't give it.

(C was the earlier recommendation of the comparison study; it is kept as an option for the vote.)

## Votes

Each plug-in repository reviews this proposal twice (two independent reviewers) and votes A, B or C, with any requested changes. The results are summarized in the PR that adds this file.
