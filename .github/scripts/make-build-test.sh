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

# --stats prints one line per output image on stdout: grep it from the log
stats() { grep '^stats ' "$1"; }
# pfmpixel FILE X Y N: the N floats of the pixel (X,Y) of a little-endian PFM file with a 16-byte
# header ("PF\n720 576\n-1.0\n" or "Pf\n..."), Y up: PFM rows are bottom to top
# (http://www.pauldebevec.com/Research/HDR/PFM/). od reads floats in the host's byte order, which
# is little-endian on the CI machines, and prints them in various formats: %g normalizes them.
pfmpixel() { od -A n -t f4 -j $((16 + ($3 * 720 + $2) * $4 * 4)) -N $(($4 * 4)) "$1" |
  awk '{ for (i = 1; i <= NF; i++) printf "%s%g", (n++ ? " " : ""), $i } END { print "" }'; }

# --stats on a solid float input through Invert: exact values (Invert also inverts alpha)
run hostdemo-stats.log -d float -i Source=solid:0.25,0.5,1,1 -o Stats --stats
expect "--stats: solid float through Invert" "$(stats hostdemo-stats.log | sed 's/ crc32=.*//')" "stats frame=0 view=0 w=720 h=576 bounds=0,0,720,576 depth=float comps=RGBA min=0.75,0.5,0,0 max=0.75,0.5,0,0 mean=0.75,0.5,0,0 nan=0
stats frame=1 view=0 w=720 h=576 bounds=0,0,720,576 depth=float comps=RGBA min=0.75,0.5,0,0 max=0.75,0.5,0,0 mean=0.75,0.5,0,0 nan=0"
test -z "$(hostdemo -d float -i Source=solid:0.25,0.5,1,1 -o Stats --stats 2>/dev/null | grep -v '^stats ')" || { echo "hostDemo --quiet --stats: other output on stdout"; exit 1; }

# a deterministic crc32: Invert on the default byte pattern (computed once, checked in). The
# crc32 of a byte image doesn't depend on the byte order.
run hostdemo-crc.log -o Crc --stats
expect "--stats: crc32 of Invert on the default pattern" "$(stats hostdemo-crc.log)" "stats frame=0 view=0 w=720 h=576 bounds=0,0,720,576 depth=byte comps=RGBA min=0,0,0,0 max=255,255,255,0 mean=254.49273,254.49273,254.49273,0 nan=0 crc32=6e0ce805
stats frame=1 view=0 w=720 h=576 bounds=0,0,720,576 depth=byte comps=RGBA min=0,0,0,0 max=255,255,255,0 mean=0.476526331,0.476526331,0.476526331,0 nan=0 crc32=52af513c"

# 16-bit PPM: P6, maxval 65535, big-endian, top row first. Invert of (0.25,0.5,1) in float is
# (0.75,0.5,0), i.e. 49151 (0xbfff), 32768 (0x8000) and 0.
run hostdemo-ppm16.log -d float -i Source=solid:0.25,0.5,1,1 -o P16 --format ppm16
expect "P16.0.ppm: header" "$(head -c 2 P16.0.ppm; sed -n 4,5p P16.0.ppm | tr '\n' ' ')" "P6720 576 65535 "
expect "P16.0.ppm: size" "$(wc -c < P16.0.ppm | tr -d ' ')" "$(( $(head -5 P16.0.ppm | wc -c) + 720 * 576 * 6 ))"
expect "P16.0.ppm: last pixel" "$(tail -c 6 P16.0.ppm | od -A n -t x1 | tr -d ' \n')" "bfff80000000"

# PFM round trip. RT1 = Invert(default pattern), in float: white with the time drawn in black
# from (50,50). The pixel (57,52) is in the first digit, so it is black, and the pixel at the
# same place from the top (57,523) is white: this checks that the PFM rows are bottom to top.
run hostdemo-rt1.log -d float --format pfm -o RT1 --stats
expect "RT1.0.pfm: header" "$(head -c 16 RT1.0.pfm | tr '\n' ' ')" "PF 720 576 -1.0 "
expect "RT1.0.pfm: size" "$(wc -c < RT1.0.pfm | tr -d ' ')" "$((16 + 720 * 576 * 12))"
expect "RT1.0.pfm: pixels (57,52) and (57,523)" "$(pfmpixel RT1.0.pfm 57 52 3) $(pfmpixel RT1.0.pfm 57 523 3)" "0 0 0 1 1 1"
# the alpha of the RGBA output (1 inverted: 0) goes to a separate Pf file
expect "RT1.0.alpha.pfm: header" "$(head -c 16 RT1.0.alpha.pfm | tr '\n' ' ')" "Pf 720 576 -1.0 "
expect "RT1.0.alpha.pfm: size and pixel (57,52)" "$(wc -c < RT1.0.alpha.pfm | tr -d ' ') $(pfmpixel RT1.0.alpha.pfm 57 52 1)" "$((16 + 720 * 576 * 4)) 0"
# RT2 = Invert(RT1.0.pfm) is the original pattern: black with the time in white; the PF input has
# alpha 1, which Invert makes 0
run hostdemo-rt2.log -d float -i Source=file:RT1.0.pfm --format pfm -o RT2 --stats
expect "RT2.0.pfm: pixels (57,52) and (57,523)" "$(pfmpixel RT2.0.pfm 57 52 3) $(pfmpixel RT2.0.pfm 57 523 3)" "1 1 1 0 0 0"
expect "RT2: --stats" "$(stats hostdemo-rt2.log | sed -n 1p | sed 's/ mean=.*//')" "stats frame=0 view=0 w=720 h=576 bounds=0,0,720,576 depth=float comps=RGBA min=0,0,0,0 max=1,1,1,0"
# RT3 = Invert(RT2.0.pfm) is RT1 again, exactly: same stats and crc32 as RT1's frame 0
run hostdemo-rt3.log -d float -i Source=file:RT2.0.pfm --format pfm -o RT3 --stats
expect "PFM round trip: --stats of Invert(Invert(RT1)) = RT1" "$(stats hostdemo-rt3.log | sed -n 1p)" "$(stats hostdemo-rt1.log | sed -n 1p)"
# a PFM read into a byte clip is converted (and clamped) to 8 bits
run hostdemo-rt4.log -i Source=file:RT1.0.pfm -o RT4
expect "RT4.0.ppm: pixels (57,52) and (57,523)" "$(pixel RT4.0.ppm 57 52) $(pixel RT4.0.ppm 57 523)" "255 255 255 0 0 0"
# a Pf file gives r=g=b=a; its size is the clip's RoD (in pixels, at the project PAR)
printf 'Pf\n3 2\n-1.0\n' > small.pfm; printf '\0\0\0\0\0\0\200\76\0\0\0\77\0\0\100\77\0\0\200\77\0\0\0\0' >> small.pfm
run hostdemo-small.log -d float -i Source=file:small.pfm -o Small --stats
expect "Pf input: --stats" "$(stats hostdemo-small.log | sed -n 1p | sed 's/ crc32=.*//')" "stats frame=0 view=0 w=3 h=2 bounds=0,0,3,2 depth=float comps=RGBA min=0,0,0,0 max=1,1,1,1 mean=0.583333333,0.583333333,0.583333333,0.583333333 nan=0"
# the same file in big-endian (positive scale) gives the same image
printf 'Pf\n3 2\n1\n' > small-be.pfm; printf '\0\0\0\0\76\200\0\0\77\0\0\0\77\100\0\0\77\200\0\0\0\0\0\0' >> small-be.pfm
run hostdemo-small-be.log -d float -i Source=file:small-be.pfm -o SmallBE --stats
expect "Pf big-endian input: --stats" "$(stats hostdemo-small-be.log | sed -n 1p)" "$(stats hostdemo-small.log | sed -n 1p)"

# malformed PFM files are rejected
head -c 1000 RT1.0.pfm > truncated.pfm
fails hostdemo-pfm-truncated.log "truncated" -i Source=file:truncated.pfm
{ cat small.pfm; printf 'x'; } > trailing.pfm
fails hostdemo-pfm-trailing.log "trailing data" -i Source=file:trailing.pfm
fails hostdemo-pfm-magic.log "not a PFM file" -i Source=file:P16.0.ppm
printf 'PF\n0 2\n-1.0\n' > zero.pfm
fails hostdemo-pfm-zero.log "invalid PFM size" -i Source=file:zero.pfm
printf 'PF\n1 1\n0\n\0\0\0\0\0\0\0\0\0\0\0\0' > scale0.pfm
fails hostdemo-pfm-scale.log "invalid PFM scale" -i Source=file:scale0.pfm
fails hostdemo-pfm-missing.log "cannot open" -i Source=file:missing.pfm
