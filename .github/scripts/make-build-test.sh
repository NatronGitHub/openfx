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
cd HostSupport/examples/*-release
OFX_PLUGIN_PATH="$dirs" ./cacheDemo > cachedemo.log 2>&1 || { cat cachedemo.log; exit 1; }
n=$(grep -c "Contexts:" cachedemo.log)
echo "cacheDemo: $n plug-ins loaded"
test "$n" -ge 5

# hostDemo runs are deterministic: only the plug-ins in OFX_PLUGIN_PATH, no plug-in
# cache, nothing on stdout but the plug-in messages.
hostdemo() { OFX_PLUGIN_PATH="$dirs" ./hostDemo --no-std-paths --no-cache --quiet "$@"; }
# run hostDemo, which must succeed; the log is printed on failure
run() { local log=$1; shift; hostdemo "$@" > "$log" 2>&1 || { echo "hostDemo $*: failed"; cat "$log"; exit 1; }; }
# run hostDemo, which must fail with a message matching the regexp $2
fails() { local log=$1 msg=$2; shift 2
  if hostdemo "$@" > "$log" 2>&1; then echo "hostDemo $*: succeeded, but should fail"; cat "$log"; exit 1; fi
  grep -q -e "$msg" "$log" || { echo "hostDemo $*: no message \"$msg\""; cat "$log"; exit 1; }
  echo "hostDemo $*: fails as expected"; }

# ppm FILE: the width and height of a plain PPM on the first line, then one "r g b" line
# per pixel, in file order (top row first)
ppm() { awk '{ sub(/#.*/, ""); for (i = 1; i <= NF; i++) {
    if (k < 4) { h[k++] = $i; if (k == 4) { if (h[0] != "P3" || h[3] != 255) exit 1; print h[1], h[2] } }
    else { p[m++] = $i; if (m == 3) { print p[0], p[1], p[2]; m = 0 } } } }
  END { if (k < 4) exit 1 }' "$1"; }
# hist FILE: the size, then "count r g b" for each pixel value, most frequent first
hist() { ppm "$1" | { read -r w h; echo "$w $h"; sort | uniq -c | sort -k1,1nr -k2,2n -k3,3n -k4,4n | awk '{ print $1, $2, $3, $4 }'; }; }
# pixel FILE X Y: the value of the pixel at (X,Y) in OFX pixel coordinates (relative to the
# bottom-left corner of the image, y up)
pixel() { ppm "$1" | awk -v x="$2" -v y="$3" 'NR == 1 { n = 2 + ($2 - 1 - y) * $1 + x } NR == n { print; exit }'; }
# expect NAME GOT EXPECTED
expect() { if [ "$2" != "$3" ]; then printf '%s: FAILED\nexpected:\n%s\ngot:\n%s\n' "$1" "$3" "$(echo "$2" | head -5)"; exit 1; fi; echo "$1: OK"; }

# Render the invert plug-in with hostDemo. The input frames are black (t=0) and
# white (t=1), with digits drawn in the other colour (from (50,50), y up): inverted,
# nearly every pixel of frame 0 is white and of frame 1 is black. The pixel (57,52) is
# in the first digit, so it checks that the PPM rows are written top row first.
run hostdemo.log
for f in "Output.0.ppm 255 0" "Output.1.ppm 0 255"; do
  set -- $f
  hist "$1" | sed -n 2p | awk -v v="$2" -v f="$1" '{ print f": most frequent pixel", $2, $3, $4, "("$1"x)"; exit ($2 != v || $3 != v || $4 != v || $1 < 400000) }'
  expect "$1: digit pixel (57,52)" "$(pixel "$1" 57 52)" "$3 $3 $3"
done

# Invert on a float RGBA input: (0.25, 0.5, 1) gives (0.75, 0.5, 0), i.e. (191, 128, 0)
# in the 8-bit PPM, at every pixel. Unwritten pixels would be NaN, which fails the run.
run hostdemo-float.log -d float -i Source=solid:0.25,0.5,1,1 -o InvertFloat
grep -q "OfxBitDepthFloat OfxImageComponentRGBA" InvertFloat.0.ppm || { echo "InvertFloat.0.ppm: not a float RGBA image"; head -4 InvertFloat.0.ppm; exit 1; }
expect InvertFloat.0.ppm "$(hist InvertFloat.0.ppm)" "720 576
414720 191 128 0"

# Invert on short and byte inputs: 0.25 and 0.5 are 16384 and 32768 (64 and 128 in bytes),
# inverted 49151 and 32767 (191 and 127), i.e. (191, 127, 0) in the PPM.
for d in short byte; do
  run hostdemo-$d.log -d $d -i Source=solid:0.25,0.5,1,1 -o Invert-$d
  grep -q "OfxBitDepth$(echo ${d:0:1} | tr a-z A-Z)${d:1} OfxImageComponentRGBA" Invert-$d.0.ppm || { echo "Invert-$d.0.ppm: not a $d RGBA image"; head -4 Invert-$d.0.ppm; exit 1; }
  expect Invert-$d.0.ppm "$(hist Invert-$d.0.ppm)" "720 576
414720 191 127 0"
done

# The basic plug-in (general context) scales the Source by scale=2, modulated by its Mask
# (an Alpha clip): with a disconnected Mask, every pixel is scaled; with a Mask whose alpha is
# 0 (and RGB is 1: the Alpha image must take the alpha), none is; with an N=16 checker,
# alternate squares are. (0,0) is in a 0 square, and the top-left pixel (0,575) in a 1 square.
basic="-p net.sf.openfx.basicPlugin -c general -d float -i Source=solid:0.5,0.25,0.125,1 scale=2"
test -z "$(hostdemo $basic -i Mask=none -o BasicNoMask 2>/dev/null)" || { echo "hostDemo --quiet: output on stdout"; exit 1; }
run hostdemo-nomask.log $basic -i Mask=none -o BasicNoMask
expect BasicNoMask.0.ppm "$(hist BasicNoMask.0.ppm)" "720 576
414720 255 128 64"
run hostdemo-mask0.log $basic -i Mask=solid:1,1,1,0 -o BasicMask0
expect BasicMask0.0.ppm "$(hist BasicMask0.0.ppm)" "720 576
414720 128 64 32"
run hostdemo-checker.log $basic -i Mask=checker:16 -o BasicChecker
expect BasicChecker.0.ppm "$(hist BasicChecker.0.ppm)" "720 576
207360 128 64 32
207360 255 128 64"
expect "BasicChecker.0.ppm: pixels (0,0) and (0,575)" "$(pixel BasicChecker.0.ppm 0 0) $(pixel BasicChecker.0.ppm 0 575)" "128 64 32 255 128 64"

# A mandatory clip can't be disconnected ("Any clip that is not optional will *always* be
# connected during a render action")
fails hostdemo-nosource.log "not optional.*Source" -i Source=none
fails hostdemo-nosource2.log "not optional.*Source" -p net.sf.openfx.basicPlugin -c general -i Source=none

# Numbers must be finite decimals
fails hostdemo-nan.log "invalid input" -i Source=solid:nan,0,0,1
fails hostdemo-hex.log "invalid input" -i Source=checker:0x10
fails hostdemo-hexf.log "invalid input" -i Source=solid:0x1p-1,0,0,1
fails hostdemo-inf.log "invalid value" -p net.sf.openfx.basicPlugin -c general scale=inf
fails hostdemo-erange.log "invalid value" -p net.sf.openfx.basicPlugin -c general scale=1e999

# Unwritten float pixels fail the run: the property tester fetches its output image, but
# doesn't write it. --allow-unwritten turns that into a warning.
fails hostdemo-unwritten.log "414720 of 414720 pixels were not written" -p net.sf.openfx.propertyTester -d float -o PropTester
run hostdemo-unwritten2.log -p net.sf.openfx.propertyTester -d float -o PropTester --allow-unwritten
echo "hostDemo --allow-unwritten: OK"

# The output RoD comes from the plug-in: the dot example's RoD is position +- radius, in
# canonical coordinates. (100,100) +- 50 is x in [46.875, 140.625] and y in [50, 150] in
# pixels (PAR 768/720), i.e. 95x100 pixels; (0,0) +- 60 is clipped to the project, 57x60.
# With no input connected, the output still has the -d depth (byte).
run hostdemo-dot.log -p net.sf.openfx.dotexample -c general -i Source=none radius=50 position=100,100 -o Dot
expect "Dot.0.ppm: size" "$(ppm Dot.0.ppm | head -1)" "95 100"
grep -q "OfxBitDepthByte" Dot.0.ppm || { echo "Dot.0.ppm: not a byte image"; head -4 Dot.0.ppm; exit 1; }
run hostdemo-dot2.log -p net.sf.openfx.dotexample -c general -i Source=none radius=60 position=0,0 -o DotClipped
expect "DotClipped.0.ppm: size" "$(ppm DotClipped.0.ppm | head -1)" "57 60"
