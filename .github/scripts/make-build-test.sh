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

# hostDemo runs are deterministic: only the plug-ins in OFX_PLUGIN_PATH, no plug-in
# cache, nothing on stdout but the plug-in messages.
hostdemo() { OFX_PLUGIN_PATH="$dirs" ./hostDemo --no-std-paths --no-cache --quiet "$@"; }

# Render the invert plug-in with hostDemo. The input frames are black (t=0) and
# white (t=1), with digits drawn in the other colour: inverted, nearly every
# value of frame 0 is 255 and of frame 1 is 0.
hostdemo > hostdemo.log 2>&1 || { cat hostdemo.log; exit 1; }
for f in "Output.0.ppm 255" "Output.1.ppm 0"; do
  set -- $f
  sed 's/#.*//' "$1" | tr -s ' \t' '\n\n' | tail -n +5 | sort | uniq -c | sort -rn | head -1 |
    awk -v v="$2" -v f="$1" '{ print f": most frequent value", $2, "("$1"x)"; exit ($2 != v || $1 < 1200000) }'
done

# Invert on a float RGBA input: (0.25, 0.5, 1) gives (0.75, 0.5, 0), i.e. (191, 128, 0)
# in the 8-bit PPM, at every pixel (unwritten pixels would be NaN, written as 0).
hostdemo -d float -i Source=solid:0.25,0.5,1,1 -o InvertFloat > hostdemo-float.log 2>&1 || { cat hostdemo-float.log; exit 1; }
grep -q "OfxBitDepthFloat OfxImageComponentRGBA" InvertFloat.0.ppm || { echo "InvertFloat.0.ppm: not a float RGBA image"; head -4 InvertFloat.0.ppm; exit 1; }
awk 'NR > 4 { n++; if ($1 != 191 || $2 != 128 || $3 != 0) bad++ } END { print "InvertFloat.0.ppm:", n, "pixels,", bad+0, "wrong"; exit (n != 720*576 || bad > 0) }' InvertFloat.0.ppm

# A disconnected optional clip (the Mask of the basic plug-in, in the general
# context) must not crash; with the default scale of 1, the output is the input.
hostdemo -p net.sf.openfx.basicPlugin -c general -i Mask=none -o BasicNoMask > hostdemo-nomask.log 2>&1 || { cat hostdemo-nomask.log; exit 1; }
sed 's/#.*//' BasicNoMask.1.ppm | tr -s ' \t' '\n\n' | tail -n +5 | sort | uniq -c | sort -rn | head -1 |
  awk '{ print "BasicNoMask.1.ppm: most frequent value", $2, "("$1"x)"; exit ($2 != 255 || $1 < 1200000) }'
