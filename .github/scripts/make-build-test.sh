#!/bin/bash
# Build everything with the Makefiles (what the consumers use) and check that
# the sample plug-ins load (cacheDemo). Used by the weekly Makefile jobs (extended.yml); the
# per-PR jobs use cmake-build-test.sh.
set -eo pipefail
cd "$(dirname "$0")/../.."

make -C Support/Library
make -C HostSupport
make -C HostSupport/examples
make -C Support/Plugins CONFIG=debug
make -C Support/Plugins CONFIG=release
make -C Support/PropTester CONFIG=release
# macOS: also the OpenCL example (the OpenCL framework; Metal is built there by default)
if [ "$(uname -s)" = Darwin ]; then make -C Examples HAVE_OPENCL=1; else make -C Examples; fi

dirs=$(find "$PWD/Support/Plugins" "$PWD/Support/PropTester" -maxdepth 2 -type d -name '*-release' | tr '\n' ':')
bin=$(echo "$PWD"/HostSupport/examples/*-release)
root=$PWD
cd "$bin"
"$root/.github/scripts/hostdemo-checks.sh" "$bin" "$dirs"
