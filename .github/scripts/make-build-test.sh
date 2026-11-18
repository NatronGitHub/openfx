#!/bin/bash
# Build everything with the Makefiles (what the consumers use) and check that
# the sample plug-ins load (cacheDemo). Used by the nightly Makefile jobs (extended.yml); the
# per-PR jobs use cmake-build-test.sh.
set -eo pipefail
cd "$(dirname "$0")/../.."

make -C Support/Library
make -C HostSupport
make -C HostSupport/examples
make -C Support/Plugins CONFIG=debug
make -C Support/Plugins CONFIG=release
make -C Support/PropTester CONFIG=release
make -C Examples

dirs=$(find "$PWD/Support/Plugins" "$PWD/Support/PropTester" -maxdepth 2 -type d -name '*-release' | tr '\n' ':')
bin=$(echo "$PWD"/HostSupport/examples/*-release)
root=$PWD
cd "$bin"
"$root/.github/scripts/hostdemo-checks.sh" "$bin" "$dirs"
