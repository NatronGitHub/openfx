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
make -C Examples

dirs=$(find "$PWD/Support/Plugins" -maxdepth 2 -type d -name '*-release' | tr '\n' ':')
cd HostSupport/examples/*-release
OFX_PLUGIN_PATH="$dirs" ./cacheDemo > cachedemo.log 2>&1 || { cat cachedemo.log; exit 1; }
n=$(grep -c "Contexts:" cachedemo.log)
echo "cacheDemo: $n plug-ins loaded"
test "$n" -ge 5

# Render the invert plug-in with hostDemo. The input frames are black (t=0) and
# white (t=1), with digits drawn in the other colour: inverted, nearly every
# value of frame 0 is 255 and of frame 1 is 0.
OFX_PLUGIN_PATH="$dirs" ./hostDemo > hostdemo.log 2>&1 || { cat hostdemo.log; exit 1; }
for f in "Output.0.ppm 255" "Output.1.ppm 0"; do
  set -- $f
  sed 's/#.*//' "$1" | tr -s ' \t' '\n\n' | tail -n +5 | sort | uniq -c | sort -rn | head -1 |
    awk -v v="$2" -v f="$1" '{ print f": most frequent value", $2, "("$1"x)"; exit ($2 != v || $1 < 1200000) }'
done
