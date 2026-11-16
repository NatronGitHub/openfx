#!/bin/bash
# Build everything with the Makefiles (what the consumers use) and check that
# the sample plug-ins load (cacheDemo). Used by the Linux and macOS jobs.
set -eo pipefail
cd "$(dirname "$0")/../.."

# git checkout gives the files arbitrary mtimes, which may make the bundled
# expat's Makefile try to re-run aclocal/automake
( cd HostSupport/expat-*/ && touch aclocal.m4 && sleep 1 && touch configure Makefile.in */Makefile.in expat_config.h.in )

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
