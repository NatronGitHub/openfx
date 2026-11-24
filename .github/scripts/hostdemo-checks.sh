#!/bin/bash
# The hostDemo and cacheDemo checks, for a Make or a CMake build (make-build-test.sh,
# cmake-build-test.sh). Outputs are written in the current directory.
# usage: hostdemo-checks.sh <directory of hostDemo and cacheDemo> <OFX_PLUGIN_PATH>
set -eo pipefail
bindir=$(cd "$1" && pwd)
dirs=$2
hostDemo="$bindir/hostDemo"
cacheDemo="$bindir/cacheDemo"
OFX_PLUGIN_PATH="$dirs" "$cacheDemo" > cachedemo.log 2>&1 || { cat cachedemo.log; exit 1; }
n=$(grep -c "Contexts:" cachedemo.log)
echo "cacheDemo: $n plug-ins loaded"
test "$n" -ge 5

# The param, progress, timeline, interact and draw suites reject NULL and descriptor handles, NULL
# names and NULL return pointers (suiteHandleChecks, built with hostDemo; not a hostDemo feature).
# The checks on an effect instance use the Invert plug-in of OFX_PLUGIN_PATH, which must be there.
SUITE_HANDLE_CHECKS_REQUIRE_PLUGIN=1 OFX_PLUGIN_PATH="$dirs" "$bindir/suiteHandleChecks" > suitehandlechecks.log 2>&1 || { cat suitehandlechecks.log; exit 1; }
tail -1 suitehandlechecks.log

# hostDemo runs are deterministic: only the plug-ins in OFX_PLUGIN_PATH, no plug-in
# cache, nothing on stdout but the plug-in messages.
hostdemo() { OFX_PLUGIN_PATH="$dirs" "$hostDemo" --no-std-paths --no-cache --quiet "$@"; }
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

# Invert on a solid RGBA input, at float and byte depths: (0.25, 0.5, 1) gives (0.75, 0.5, 0),
# i.e. (191, 128, 0) in the 8-bit PPM for float; in bytes, 64 and 128 invert to 191 and 127.
for f in "float 191 128 0" "byte 191 127 0"; do
  set -- $f
  run hostdemo-$1.log -d $1 -i Source=solid:0.25,0.5,1,1 -o Invert-$1
  grep -q "OfxBitDepth$(echo ${1:0:1} | tr a-z A-Z)${1:1} OfxImageComponentRGBA" Invert-$1.0.ppm || { echo "Invert-$1.0.ppm: not a $1 RGBA image"; head -4 Invert-$1.0.ppm; exit 1; }
  expect Invert-$1.0.ppm "$(hist Invert-$1.0.ppm)" "720 576
414720 $2 $3 $4"
done

# The DepthConverter example (Examples/DepthConverter, CMake builds only) must address whole RGBA
# pixels, not components: on the ramp (x + 0.5) / 720 it gives back the ramp, i.e.
# round((2x + 1) * 255 / 1440) in the 8-bit PPM, at every depth (hostDemo has no multiple clip
# depths: the output depth is the input's)
if grep -q "^Plug-in:uk.co.thefoundry.DepthConverterExample$" cachedemo.log; then
  for d in byte short float; do
    run hostdemo-depthconverter-$d.log -p uk.co.thefoundry.DepthConverterExample -d $d -i Source=ramp --frames 0 -o DepthConverter-$d
    bad=$(ppm DepthConverter-$d.0.ppm | awk 'NR > 1 { x = (NR - 2) % 720; v = int(((2 * x + 1) * 255 + 720) / 1440)
      if (!bad && ($1 != v || $2 != v || $3 != v)) bad = "pixel (" x "," 575 - int((NR - 2) / 720) "): " $0 ", expected " v }
      END { print bad }')
    expect "DepthConverter-$d.0.ppm: the input ramp" "$bad" ""
  done
fi

# An Alpha clip: the basic plug-in (general context) scales the Source by scale=2, modulated by
# its Mask, an Alpha clip. A Mask whose alpha is 0 (and RGB is 1: the Alpha image must take the
# alpha) scales no pixel. --quiet prints nothing on stdout.
basic="-p net.sf.openfx.basicPlugin -c general -d float -i Source=solid:0.5,0.25,0.125,1 scale=2"
test -z "$(hostdemo $basic -i Mask=solid:1,1,1,0 -o BasicMask0 2>/dev/null)" || { echo "hostDemo --quiet: output on stdout"; exit 1; }
expect BasicMask0.0.ppm "$(hist BasicMask0.0.ppm)" "720 576
414720 128 64 32"

# A mandatory clip can't be disconnected ("Any clip that is not optional will *always* be
# connected during a render action")
fails hostdemo-nosource.log "not optional.*Source" -i Source=none

# Numbers must be finite decimals
fails hostdemo-nan.log "invalid input" -i Source=solid:nan,0,0,1

# pfmpixel FILE X Y N: the N floats of the pixel (X,Y) of a little-endian PFM file with a 16-byte
# header ("PF\n720 576\n-1.0\n" or "Pf\n..."), Y up: PFM rows are bottom to top
# (http://www.pauldebevec.com/Research/HDR/PFM/). od reads floats in the host's byte order, which
# is little-endian on the CI machines, and prints them in various formats: %g normalizes them.
pfmpixel() { od -A n -t f4 -j $((16 + ($3 * 720 + $2) * $4 * 4)) -N $(($4 * 4)) "$1" |
  awk '{ for (i = 1; i <= NF; i++) printf "%s%g", (n++ ? " " : ""), $i } END { print "" }'; }

# PFM round trip. RT1 = Invert(default pattern), in float: white with the time drawn in black
# from (50,50). The pixel (57,52) is in the first digit, so it is black, and the pixel at the
# same place from the top (57,523) is white: this checks that the PFM rows are bottom to top.
run hostdemo-rt1.log -d float --format pfm -o RT1
expect "RT1.0.pfm: header" "$(head -c 16 RT1.0.pfm | tr '\n' ' ')" "PF 720 576 -1.0 "
expect "RT1.0.pfm: size" "$(wc -c < RT1.0.pfm | tr -d ' ')" "$((16 + 720 * 576 * 12))"
expect "RT1.0.pfm: pixels (57,52) and (57,523)" "$(pfmpixel RT1.0.pfm 57 52 3) $(pfmpixel RT1.0.pfm 57 523 3)" "0 0 0 1 1 1"
# the alpha of the RGBA output (1 inverted: 0) goes to a separate Pf file
expect "RT1.0.alpha.pfm: header" "$(head -c 16 RT1.0.alpha.pfm | tr '\n' ' ')" "Pf 720 576 -1.0 "
expect "RT1.0.alpha.pfm: size and pixel (57,52)" "$(wc -c < RT1.0.alpha.pfm | tr -d ' ') $(pfmpixel RT1.0.alpha.pfm 57 52 1)" "$((16 + 720 * 576 * 4)) 0"
# RT2 = Invert(RT1.0.pfm) is the original pattern: black with the time in white
run hostdemo-rt2.log -d float -i Source=file:RT1.0.pfm --format pfm -o RT2
expect "RT2.0.pfm: pixels (57,52) and (57,523)" "$(pfmpixel RT2.0.pfm 57 52 3) $(pfmpixel RT2.0.pfm 57 523 3)" "1 1 1 0 0 0"
# only little-endian PFM files (negative scale) are read
{ printf 'Pf\n1 1\n1.0\n'; printf '\0\0\0\0'; } > big-endian.pfm
fails hostdemo-pfm-scale.log "only little-endian" -i Source=file:big-endian.pfm

# the JSON files are removed first, so that a check never reads the file of an earlier run
rm -f report.json describe.json changed.json dot.json fail.json escape.json keys.json buttons.json render-keys.json

# jcheck NAME FILE CODE: run the Python statements CODE (assertions) with r = the JSON document in
# FILE, using only the standard library. Any failed assertion or missing key fails the check.
jcheck() { python3 -I -c 'import json, math, sys
name, path, code = sys.argv[1:4]
try:
    with open(path) as f:
        r = json.load(f)
    exec(code)
except Exception as e:
    print("%s: FAILED (%s: %s)" % (name, type(e).__name__, e))
    sys.exit(1)
print(name + ": OK")' "$@"; }

# --report on Invert (filter context): the RoD is the input RoD, Invert is not an identity, the
# frames needed are [t,t], the clip preferences map both clips to float RGBA, and the output
# numbers are exact (Invert also inverts alpha)
run hostdemo-report.log -d float -i Source=solid:0.25,0.5,1,1 -o Report --report report.json
jcheck "--report: Invert" report.json '
assert r["schema"] == "hostDemo.report" and r["schemaVersion"] == 1
assert r["ok"] is True and r["exitStatus"] == 0
assert r["plugin"]["id"] == "net.sf.openfx.invertPlugin" and r["context"] == "OfxImageEffectContextFilter"
assert r["depth"] == "OfxBitDepthFloat" and r["inputs"]["Source"]["spec"] == "solid:0.25,0.5,1,1"
assert [a["action"] for a in r["actions"]] == ["createInstance", "getClipPreferences", "beginSequenceRender", "endSequenceRender"]
assert all(a["ok"] for a in r["actions"])
cp = r["clipPreferences"]
for c in ("Source", "Output"):
    assert (cp["clips"][c]["depth"], cp["clips"][c]["components"]) == ("OfxBitDepthFloat", "OfxImageComponentRGBA"), c
    assert abs(cp["clips"][c]["pixelAspectRatio"] - 768. / 720.) < 1e-12
assert cp["outputPremultiplication"] == "OfxImageAlphaUnPremultiplied" and cp["outputFrameRate"] == 25
assert cp["outputFielding"] == "OfxFieldNone" and cp["frameVarying"] is False and cp["continuousSamples"] is False
assert r["timeDomain"] is None  # only called in the general context
assert [(x["time"], x["view"]) for x in r["renders"]] == [(0, 0), (1, 0)]
for x in r["renders"]:
    t = x["time"]
    assert x["regionOfDefinition"]["ok"] and x["regionOfDefinition"]["rod"] == x["inputs"]["Source"]["regionOfDefinition"] == [0, 0, 768, 576]
    assert x["renderWindow"] == [0, 0, 720, 576]
    assert x["isIdentity"]["ok"] and x["isIdentity"]["identity"] is False and x["isIdentity"]["clip"] is None
    assert x["framesNeeded"]["ok"] and x["framesNeeded"]["clips"] == {"Source": [[t, t]]}
    assert x["regionsOfInterest"]["clips"]["Source"] == [0, 0, 768, 576]
    assert x["render"] == {"ok": True, "status": "kOfxStatOK", "imageFetchErrors": 0}
    o = x["output"]
    assert (o["w"], o["h"], o["bounds"], o["depth"], o["comps"]) == (720, 576, [0, 0, 720, 576], "float", "RGBA")
    assert o["min"] == o["max"] == o["mean"] == [0.75, 0.5, 0, 0] and o["nan"] == 0
    assert o["files"] == ["Report.%d.ppm" % t] and len(o["crc32"]) == 8
assert r["messages"] == [] and r["progress"] == []
'

# --describe on the basic plug-in: no instance, no render; the clips and params of the general context
rm -f Describe.*
run hostdemo-describe.log -p net.sf.openfx.basicPlugin -o Describe --describe describe.json
test -z "$(ls Describe.* 2>/dev/null)" || { echo "hostDemo --describe: rendered"; exit 1; }
jcheck "--describe: basic plug-in" describe.json '
assert r["schema"] == "hostDemo.describe" and r["ok"] is True
assert r["plugin"]["id"] == "net.sf.openfx.basicPlugin" and r["plugin"]["versionMajor"] == 1
assert {"OfxImageEffectContextFilter", "OfxImageEffectContextGeneral"} <= set(r["contexts"])
g = r["contexts"]["OfxImageEffectContextGeneral"]
assert g["ok"] and [c["name"] for c in g["clips"]] == ["Source", "Mask", "Output"]
mask = g["clips"][1]
assert mask["optional"] is True and mask["isMask"] is True and mask["supportedComponents"] == ["OfxImageComponentAlpha"]
assert g["clips"][0]["optional"] is False and "OfxImageComponentRGBA" in g["clips"][0]["supportedComponents"]
f = g["flags"]
assert set(f["supportedPixelDepths"]) == {"OfxBitDepthByte", "OfxBitDepthShort", "OfxBitDepthFloat"}
assert len(f["supportedPixelDepths"]) == 3, f["supportedPixelDepths"]  # described once: no duplicates
assert f["supportsTiles"] is True and f["renderThreadSafety"] == "OfxImageEffectRenderInstanceSafe"
p = dict((q["name"], q) for q in g["params"])
assert [q["name"] for q in g["params"]] == ["Controls", "componentScales", "scale", "scaleComponents", "scaleR", "scaleG", "scaleB", "scaleA"]
s = p["scale"]
assert (s["type"], s["dimension"], s["default"], s["min"], s["max"], s["displayMin"], s["displayMax"]) == ("OfxParamTypeDouble", 1, 1, 0, 10, 0, 10)
assert s["page"] == "Controls" and s["parent"] == "" and s["hint"] == "Scales all component in the image"
assert p["scaleComponents"]["type"] == "OfxParamTypeBoolean" and p["scaleComponents"]["default"] == 1
assert p["scaleR"]["parent"] == "componentScales" and p["scaleR"]["label"] == "red"
assert p["Controls"]["type"] == "OfxParamTypePage" and p["Controls"]["children"][0] == "scale"
assert [c["name"] for c in r["contexts"]["OfxImageEffectContextFilter"]["clips"]] == ["Source", "Output"]
'

# command-line parameters are set after createInstance, each followed by instanceChanged
# (kOfxChangeUserEdited): the basic plug-in disables the component scales when scaleComponents is
# off (in its changedParam), so the report shows them disabled
run hostdemo-changed.log -p net.sf.openfx.basicPlugin -c general -i Mask=none scaleComponents=0 --report changed.json
jcheck "--report: instanceChanged" changed.json '
a = [(x["action"], x.get("param")) for x in r["actions"]]
assert a[:5] == [("createInstance", None), ("getClipPreferences", None), ("beginInstanceChanged", "scaleComponents"),
                 ("instanceChanged", "scaleComponents"), ("endInstanceChanged", "scaleComponents")], a
assert all(x["ok"] for x in r["actions"])
assert r["params"]["scaleComponents"]["value"] is False and r["params"]["scale"]["value"] == 1
assert [r["params"][n]["enabled"] for n in ("scale", "scaleR", "scaleG", "scaleB", "scaleA")] == [True, False, False, False, False]
assert r["timeDomain"]["ok"] and r["inputs"]["Mask"]["connected"] is False
'

# The output RoD comes from the plug-in: the dot example's RoD is position +- radius, in canonical
# coordinates, and the output covers it in pixels (PAR 768/720). A disconnected clip has no RoD.
run hostdemo-dotreport.log -p net.sf.openfx.dotexample -c general -i Source=none radius=50 position=100,100 -o DotReport --report dot.json
jcheck "--report: dot example RoD" dot.json '
par = 768. / 720.
for x in r["renders"]:
    rod = x["regionOfDefinition"]["rod"]
    assert x["regionOfDefinition"]["status"] == "kOfxStatOK" and rod == [50, 50, 150, 150], rod
    o = x["output"]
    px = [math.floor(rod[0] / par), math.floor(rod[1]), math.ceil(rod[2] / par), math.ceil(rod[3])]
    assert o["bounds"] == x["renderWindow"] == px, (o["bounds"], px)
    assert (o["w"], o["h"]) == (px[2] - px[0], px[3] - px[1]) == (95, 100)
    assert x["inputs"]["Source"] == {"connected": False, "regionOfDefinition": None}
assert len(r["renders"]) == 2
'

# Degenerate inputs must not make NaNs (NaN audit): the gamma example, where the mask is 0, gives
# the source unchanged, even if the gamma overflows (1e30^10 is infinite, and inf*0 was NaN). The
# NaN count must also work with -ffast-math, where std::isnan and x != x are folded to false.
rm -f gamma0.json
run hostdemo-gamma0.log -p net.sf.openfx.gammaexample -c general -d float --frames 0 -i Source=solid:1e30,1e30,1e30,1 -i Mask=solid:0,0,0,0 scale=10 -o Gamma0 --report gamma0.json
jcheck "gamma example, mask 0, overflowing gamma: the source" gamma0.json '
o = r["renders"][0]["output"]
assert o["nan"] == 0 and o["min"] == o["max"] and all(abs(v - 1e30) < 1e24 for v in o["min"][:3]), o
'

# Unwritten float pixels fail the run: the property tester fetches its output image, but doesn't
# write it. The report is still written, with ok false and the exit status. --allow-unwritten
# turns the failure into a warning.
fails hostdemo-unwritten.log "414720 of 414720 pixels were not written" -p net.sf.openfx.propertyTester -d float -o PropTester --report fail.json
jcheck "--report: failed run" fail.json '
assert r["ok"] is False and r["exitStatus"] == 1 and r["renders"][0]["render"]["ok"] is True
'
run hostdemo-unwritten2.log -p net.sf.openfx.propertyTester -d float -o PropTester --allow-unwritten
echo "hostDemo --allow-unwritten: OK"

# strings are escaped: a file name with a quote, a backslash, a tab and a non-ASCII (UTF-8) letter
esc=$(printf 'esc"a\\b\tc\303\251.pfm'); cp RT1.0.alpha.pfm "$esc"
run hostdemo-escape.log -d float -i "Source=file:$esc" -o Escape --report escape.json
jcheck "--report: string escaping" escape.json '
assert r["inputs"]["Source"]["spec"] == "file:esc\"a\\b\tcé.pfm", r["inputs"]["Source"]["spec"]
assert r["renders"][0]["output"]["w"] == 720
'

# Node primitives, for a Python graph executor: any times, a render scale and a render window
# (pixels at that scale). Invert of the default pattern at t=0.5 is 0.5 (and alpha 0) outside the
# digits. Then the output, positioned with its bounds (file:...@x1,y1, pixels at the render scale),
# is an input with the same RoD, and Invert gives back the pattern. --query renders nothing.
rm -f node1.json node2.json query.json Node1.* Node2.* Query.*
run hostdemo-node1.log -d float --format pfm --frames 0.5,2 --scale 0.5 --window 10,20,110,70 -o Node1 --report node1.json
jcheck "--frames, --scale, --window" node1.json '
assert r["frames"] == [0.5, 2] and r["renderScale"] == [0.5, 0.5] and r["window"] == [10, 20, 110, 70]
assert [x["time"] for x in r["renders"]] == [0.5, 2]
par = 768. / 720.
for x in r["renders"]:
    assert x["renderScale"] == [0.5, 0.5] and x["renderWindow"] == x["output"]["bounds"] == [10, 20, 110, 70]
    assert x["regionsOfInterest"]["clips"]["Source"] == [10 / 0.5 * par, 40, 110 / 0.5 * par, 140]
o = r["renders"][0]["output"]
assert (o["min"], o["max"]) == ([0.5, 0.5, 0.5, 0], [0.5, 0.5, 0.5, 0]), o
assert r["renders"][0]["output"]["files"][0] == "Node1.0.5.pfm"
'
run hostdemo-node2.log -d float --format pfm --frames 0.5 --scale 0.5 -i Source=file:Node1.0.5.pfm@10,20 -o Node2 --report node2.json
jcheck "positioned PFM input" node2.json '
par = 768. / 720.
x = r["renders"][0]
assert x["inputs"]["Source"]["regionOfDefinition"] == [10 / 0.5 * par, 40, 110 / 0.5 * par, 140]
assert x["renderWindow"] == x["output"]["bounds"] == [10, 20, 110, 70]
assert x["output"]["min"][:3] == x["output"]["max"][:3] == [0.5, 0.5, 0.5]
'
# with its alpha file and premultiplication state: Node1's alpha is 0, and Invert gives alpha 1
run hostdemo-node3.log -d float --format pfm --frames 0.5 --scale 0.5 -i "Source=file:Node1.0.5.pfm+Node1.0.5.alpha.pfm@10,20#premult" -o Node3 --report node3.json
jcheck "PFM input with alpha and premultiplication" node3.json '
x = r["renders"][0]
assert x["output"]["min"] == x["output"]["max"] == [0.5, 0.5, 0.5, 1], x["output"]
assert r["inputs"]["Source"]["spec"].endswith("#premult")
'
fails hostdemo-alpha.log "the alpha file must be a Pf file" -i "Source=file:Node1.0.5.pfm+Node1.0.5.pfm"
run hostdemo-query.log -d float --frames 3 --query -o Query --report query.json
test -z "$(ls Query.* 2>/dev/null)" || { echo "hostDemo --query: rendered"; exit 1; }
jcheck "--query" query.json '
assert r["query"] is True and [a["action"] for a in r["actions"]] == ["createInstance", "getClipPreferences"]
x = r["renders"][0]
assert x["time"] == 3 and x["render"] is None and x["output"] is None
assert x["regionOfDefinition"]["rod"] == [0, 0, 768, 576] and x["framesNeeded"]["status"]
'
fails hostdemo-frames.log "invalid frames" --frames 0,0
fails hostdemo-scale.log "invalid render scale" --scale 1.5
fails hostdemo-window.log "invalid render window" --window 5,5,5,9

# Review fixes (#117-#123): huge windows, a clip given twice, times with the same file name and
# non-finite PFM inputs are refused; a non-sequential --frames list is rendered as such
fails hostdemo-bigwindow.log "at most 1000000 pixels" -p net.sf.openfx.noisePlugin -c general --frames 0 --window 0,0,268435457,1
fails hostdemo-twice.log "given twice" -i Source=solid:1,0,0,1 -i Source=ramp
fails hostdemo-samename.log "given twice to 12 significant digits" --frames 0.1234567890123,0.1234567890124
printf 'Pf\n1 1\n-1.0\n\000\000\300\177' > nan.pfm
fails hostdemo-nanpfm.log "is not finite" -d float -i Source=file:nan.pfm
run hostdemo-unsorted.log -d float --frames 2,0,1 -o Unsorted
echo "hostDemo --frames 2,0,1: OK"
# rod:x1,y1,x2,y2: a transparent placeholder input with a given RoD (used by ofxhost.graph)
run hostdemo-rod.log -d float --frames 0 -i Source=rod:-100.5,0,2000,300 --query --report rod.json
jcheck "rod: input" rod.json '
x = r["renders"][0]
assert x["inputs"]["Source"]["regionOfDefinition"] == [-100.5, 0, 2000, 300], x["inputs"]
assert x["regionOfDefinition"]["rod"] == [-100.5, 0, 2000, 300]
'
fails hostdemo-badrod.log "invalid RoD" -i Source=rod:1,0,0,1

# kOfxImageEffectPropSupportsTiles: "If a clip or plugin does not support tiled images, then the
# host should supply full RoD images to the effect whenever it fetches one". Invert's supportsTiles
# parameter sets it on the instance (OFX 1.4); setting it also calls paramGetNumKeys and
# paramDeleteAllKeys, which must succeed on a parameter without keys.
run hostdemo-tiles.log -d float --frames 0 -i Source=solid:0.25,0.5,0,1 -s supportsTiles=1 --window 100,100,164,148 --report tiles.json -o Tiles
run hostdemo-notiles.log -d float --frames 0 -i Source=solid:0.25,0.5,0,1 -s supportsTiles=0 --window 100,100,164,148 --report notiles.json -o NoTiles
jcheck "supportsTiles=1" tiles.json '
x = r["renders"][0]
assert x["supportsTiles"] == {"effect": True, "clips": {"Output": True, "Source": True}}, x["supportsTiles"]
assert x["renderWindow"] == [100, 100, 164, 148], x["renderWindow"]
'
jcheck "supportsTiles=0: the whole RoD" notiles.json '
x = r["renders"][0]
assert x["supportsTiles"]["effect"] is False, x["supportsTiles"]
assert x["renderWindow"] == x["output"]["bounds"] == [0, 0, 720, 576], (x["renderWindow"], x["output"])
'
grep -q "doesn.t support tiles: rendering the whole RoD" hostdemo-notiles.log || { echo "supportsTiles=0: no warning"; cat hostdemo-notiles.log; exit 1; }
# whole-RoD input images: Invert's testSmallRoI asks for the centre of the region it needs only, so
# a solid input gives a solid output only if hostDemo fetches the whole RoD (no tiles); with tiles,
# the output has other values outside the small RoI. Two frames: the --window warning only once.
run hostdemo-smallroi.log --frames 0,1 -i Source=solid:0.25,0.5,0,1 -s supportsTiles=0 -s testSmallRoI=1 --window 100,100,164,148 -o SmallRoI
expect "supportsTiles=0 testSmallRoI=1: whole-RoD input" "$(hist SmallRoI.0.ppm)" "720 576
414720 191 127 255"
[ "$(grep -c "rendering the whole RoD" hostdemo-smallroi.log)" = 1 ] || { echo "the --window warning isn't printed once"; cat hostdemo-smallroi.log; exit 1; }
run hostdemo-smallroi-tiles.log --frames 0 -i Source=solid:0.25,0.5,0,1 -s testSmallRoI=1 -o SmallRoITiles
[ "$(hist SmallRoITiles.0.ppm | wc -l)" -gt 2 ] || { echo "testSmallRoI=1 with tiles: the output should not be solid"; hist SmallRoITiles.0.ppm; exit 1; }
# an input RoD too large for hostDemo: the RoI is fetched instead (with a warning)
run hostdemo-hugerod.log -d float --frames 0 -i Source=rod:0,0,40000,40000 -s supportsTiles=0 --window 0,0,64,64 -o HugeRoD
grep -q "is too large for hostDemo: rendering --window" hostdemo-hugerod.log &&
  grep -q "is too large for hostDemo: fetching the region of interest" hostdemo-hugerod.log ||
  { echo "huge RoD without tiles: no fallback warnings"; cat hostdemo-hugerod.log; exit 1; }
echo "huge RoD without tiles: OK"

# Support's sendMessage passes the text as a "%s" argument of the message suite's printf format: a
# '%' in a message (a file name, an error text) is shown as is
run hostdemo-percent.log -p net.sf.openfx.GenericTestPlugin -s 'messageText=100% done: %d %s %x %%' --frames 0 --query --report percent.json
jcheck "a message with %" percent.json '
t = "100% done: %d %s %x %%"
assert [(m["kind"], m["text"]) for m in r["messages"]] == [("message", t), ("persistentMessage", t)], r["messages"]
'


# Animation keys (-s name@t=value): linear interpolation, the values at each render, instanceChanged
# at the key's time, the keys and the value at time 0 in the report
run hostdemo-keys.log -p net.sf.openfx.basicPlugin -c general -i Source=solid:0.5,0.5,0.5,1 -i Mask=none -d float \
  -s scale@0=1 -s scale@10=2 -s scale@4=1.2 --frames 0,5,10,12 --report keys.json -o Keys
jcheck "keys" keys.json '
p = r["params"]["scale"]
assert p["keys"] == [[0, 1], [4, 1.2], [10, 2]] and p["value"] == 1, p
v = [x["params"]["scale"] for x in r["renders"]]
assert v[0] == 1 and abs(v[1] - 4 / 3) < 1e-12 and v[2] == 2 and v[3] == 2, v
m = [x["output"]["mean"][0] for x in r["renders"]]
assert abs(m[1] - 0.5 * 4 / 3) < 1e-6 and abs(m[2] - 1) < 1e-6, m
assert [a["time"] for a in r["actions"] if a.get("param") == "scale" and a["action"] == "instanceChanged"] == [0, 10, 4]
'
# push buttons (-s name), paramSetValueAtTime from instanceChanged (Tester's analyseButton keys
# analysisParam at each frame), and the keyframe functions (Tester's keysTest checks them all)
# (keysTest pressed at 0 and at 3.5: the plug-in checks the time it gets and the timeline)
run hostdemo-buttons.log -p net.sf.openfx.GenericTestPlugin -s pbButton -s analyseButton -s keysTest -s keysTest@3.5 --frames 0 --query --report buttons.json
jcheck "push buttons and keys from the plug-in" buttons.json '
assert [m["text"] for m in r["messages"]] == ["Push Button Pressed - TestPassed!", "Keys test passed", "Keys test passed"], r["messages"]
k = r["params"]["analysisParam"]["keys"]
assert len(k) == 25 and all(t == v for t, v in k), k
'
# paramGetValue during a render is the value at the render's time (Tester checks it, with an error
# message otherwise)
run hostdemo-renderkeys.log -p net.sf.openfx.GenericTestPlugin -s keysDouble@0=1 -s keysDouble@10=3 --frames 5,10 -d float --report render-keys.json -o RenderKeys
jcheck "paramGetValue in render" render-keys.json '
assert r["ok"] and r["messages"] == [], r["messages"]
assert [x["params"]["keysDouble"] for x in r["renders"]] == [2, 3]
'
fails hostdemo-keynoanim.log "doesn't animate" -p net.sf.openfx.GenericTestPlugin -s animateDbl@1=2
fails hostdemo-keytime.log "invalid key time" -s supportsTiles@x=1
fails hostdemo-nobutton.log "invalid parameter assignment" -s supportsTiles

# kOfxActionLoad and kOfxActionDescribe once per load of the binary (ofxCore.h: Load "will not be
# called again while the binary containing the plug-in remains loaded"), and kOfxActionUnload
# before the binary is unloaded. The property tester prints these actions with
# OFX_PROPTESTER_TRACE=1. By default the scan sends Load, Describe and Unload and unloads the
# binary, and the first use (--describe) loads it again. With OFX_HOSTDEMO_KEEP_LOADED=1
# (Host::keepPluginsLoadedAfterScan()) the scanned plug-in stays loaded: its first use sends no
# second Load or Describe, and Unload comes once, at exit. A run that uses the plug-in cache loads
# the binary only at the first use, in both modes.
lifecycle_trace() { grep -o 'propTester: OfxAction[A-Za-z]*' "$1" | sed 's/^propTester: OfxAction//' | tr '\n' ' '; }
# lifecycle NAME EXPECTED CACHE KEEP: --describe of the property tester, without the plug-in cache
# (CACHE=0), or in the directory lifecycle-cache (CACHE=1, which keeps its cache file)
lifecycle() { local name=$1 expected=$2 cache=$3 keep=$4 log=$PWD/lifecycle-$1.log
  if [ "$cache" = 1 ]; then
    (mkdir -p lifecycle-cache && cd lifecycle-cache &&
     OFX_PROPTESTER_TRACE=1 OFX_HOSTDEMO_KEEP_LOADED=$keep OFX_PLUGIN_PATH="$dirs" "$hostDemo" --no-std-paths --quiet \
       -p net.sf.openfx.propertyTester --describe lifecycle.json) > "$log" 2>&1
  else
    OFX_PROPTESTER_TRACE=1 OFX_HOSTDEMO_KEEP_LOADED=$keep hostdemo -p net.sf.openfx.propertyTester --describe lifecycle.json > "$log" 2>&1
  fi || { echo "lifecycle $name: hostDemo failed"; cat "$log"; exit 1; }
  expect "Load/Describe/Unload: $name" "$(lifecycle_trace "$log")" "$expected"; }
lifecycle "scan, then first use" "Load Describe Unload Load Describe Unload " 0 0
lifecycle "scan, kept loaded" "Load Describe Unload " 0 1
rm -rf lifecycle-cache
lifecycle "scan that writes the cache" "Load Describe Unload Load Describe Unload " 1 0
lifecycle "from the cache" "Load Describe Unload " 1 0
lifecycle "from the cache, keep mode" "Load Describe Unload " 1 1
rm -rf lifecycle-cache
lifecycle "scan that writes the cache, kept loaded" "Load Describe Unload " 1 1

# A plug-in cache that is still valid for its binary (same modification time and size) may hold a
# label, grouping or contexts that the plug-in no longer describes (openfx-gmic's plug-ins come
# from user data): --describe and --report give what the describe action set, not the cache.
stale() { (cd stale-cache && OFX_PLUGIN_PATH="$dirs" "$hostDemo" --no-std-paths --quiet -p net.sf.openfx.basicPlugin "$@") \
  > stale-cache.log 2>&1 || { echo "stale cache: hostDemo $* failed"; cat stale-cache.log; exit 1; }; }
rm -rf stale-cache && mkdir stale-cache
stale --describe stale.json  # writes the cache
perl -0pi -e 's{<plugin name="net\.sf\.openfx\.basicPlugin".*?</plugin>}{ my $b = $&;
  $b =~ s/(name="OfxPropLabel"[^\n]*\n\s*<value index="0" value=)"Gain"/$1"Stale"/;
  $b =~ s/(name="OfxImageEffectPluginPropGrouping"[^\n]*\n\s*<value index="0" value=)"OFX"/$1"Stale"/;
  $b =~ s/\n\s*<value index="2" value="OfxImageEffectContextPaint" \/>//; $b }se' stale-cache/hostDemoPluginCache.xml
expect "stale cache: cache edited" "$(perl -0ne 'my ($b) = /(<plugin name="net\.sf\.openfx\.basicPlugin".*?<\/plugin>)/s;
  my $n = () = $b =~ /value="Stale"/g; print $n, $b =~ /ContextPaint/ ? " paint" : " no paint"' stale-cache/hostDemoPluginCache.xml)" "2 no paint"
stale --describe stale.json
stale -c general -i Mask=none --frames 0 --report stale-report.json
jcheck "stale cache: --describe" stale-cache/stale.json '
assert (r["plugin"]["label"], r["plugin"]["grouping"]) == ("Gain", "OFX"), r["plugin"]
assert sorted(r["contexts"]) == ["OfxImageEffectContextFilter", "OfxImageEffectContextGeneral", "OfxImageEffectContextPaint"], list(r["contexts"])
'
jcheck "stale cache: --report" stale-cache/stale-report.json 'assert (r["plugin"]["label"], r["plugin"]["grouping"]) == ("Gain", "OFX"), r["plugin"]'
rm -rf stale-cache stale-cache.log

# OFX 1.4 hosts: every plug-in must keep working in a host that implements only OFX 1.4. hostDemo
# --ofx14 has only the OFX 1.4 host properties (a plug-in reading another one gets
# kOfxStatErrUnknown) and suites (no DrawSuite, OpenCL program, plane or Vegas suites), doesn't
# call OfxSetHost, refuses StrChoice and Bytes params (kOfxStatErrUnknown) and renders one view.
# The ColourSpace example (CMake builds only) reads the OFX 1.5 colour management style and the
# StrChoice support, and logs that the host has neither.
if grep -q "^Plug-in:io.aswf.openfx.example.ColourspacePluginCore$" cachedemo.log; then
  OFX_COLOURSPACE_EXAMPLE_VERBOSE=1 run hostdemo-ofx14-colour.log --ofx14 -p io.aswf.openfx.example.ColourspacePluginCore --describe ofx14-colour.json
  grep -q "host does not support colour management (err=ErrUnknown)" hostdemo-ofx14-colour.log &&
    grep -q "host does not support StrChoice params" hostdemo-ofx14-colour.log ||
    { echo "--ofx14: the ColourSpace example found OFX 1.5 host properties"; cat hostdemo-ofx14-colour.log; exit 1; }
  echo "--ofx14: no OFX 1.5 host properties: OK"
fi

# --opengl-describe is for --describe only: hostDemo can't render with OpenGL (test_hostdemo_cli.py
# checks what the OpenGL example sees with it)
fails hostdemo-opengl-render.log "needs --describe" --opengl-describe -p net.sf.openfx.invertPlugin
# The plug-in cache stays valid while a binary's mtime and size don't change, but the binary's list
# of plug-ins may change (e.g. openfx-gmic's depends on a user file): a cached plug-in must be the
# one with its identifier and version, at another index if it moved, or none. The cache of the
# ColourSpace example (Basic, Core, Full) is edited as if Basic had been at index 7 (out of range:
# the bounds check, then the scan), and Core had been another plug-in, Gone. (The describe output
# can't tell these plug-ins apart: it has the cached label.)
if grep -q "^Plug-in:io.aswf.openfx.example.ColourspacePluginCore$" cachedemo.log; then
  cs=io.aswf.openfx.example.ColourspacePlugin
  stale() { (cd stale-cache && OFX_PLUGIN_PATH="$dirs" "$hostDemo" --no-std-paths --quiet "$@"); }
  rm -rf stale-cache && mkdir stale-cache
  stale -p ${cs}Basic --describe scan.json > stale-cache.log 2>&1 || { echo "stale cache: hostDemo failed"; cat stale-cache.log; exit 1; }
  perl -pi -e "s/name=\"${cs}Basic\" index=\"0\"/name=\"${cs}Basic\" index=\"7\"/;
               s/name=\"${cs}Core\"/name=\"${cs}Gone\"/" stale-cache/hostDemoPluginCache.xml
  grep -q "name=\"${cs}Basic\" index=\"7\"" stale-cache/hostDemoPluginCache.xml &&
    grep -q "name=\"${cs}Gone\"" stale-cache/hostDemoPluginCache.xml || { echo "stale cache: the cache wasn't edited"; exit 1; }
  stale -p ${cs}Basic --describe moved.json > stale-cache.log 2>&1 || { echo "stale cache: a moved plug-in was not found"; cat stale-cache.log; exit 1; }
  grep -q "stale plug-in cache: .* has ${cs}Basic 1.0 at another index; clear the OpenFX plug-in cache" stale-cache.log ||
    { echo "stale cache: no message for a moved plug-in"; cat stale-cache.log; exit 1; }
  if stale -p ${cs}Gone --describe gone.json > stale-cache.log 2>&1; then echo "stale cache: another plug-in was used for one the binary no longer has"; exit 1; fi
  grep -q "stale plug-in cache: .* no longer has ${cs}Gone 1.0; clear the OpenFX plug-in cache" stale-cache.log ||
    { echo "stale cache: no message for a plug-in the binary no longer has"; cat stale-cache.log; exit 1; }
  echo "stale plug-in cache: a moved plug-in is found by identifier, a removed one is not replaced: OK"
fi
# Every plug-in that cacheDemo loaded is described (by the default host), then rendered at t=0 in
# the filter context (else the general one) by the default host and by the OFX 1.4 host. Both
# hosts must give the same results: render status and crc32 of the output, except for the
# plug-ins in $differs, whose OFX 1.4 fallback changes the output (ChoiceParams-support: no
# StrChoice param, blue scale 1). A plug-in with no context to render is described by both hosts
# instead. All must succeed, except the plug-ins in $needs_more, which need a feature hostDemo
# doesn't have and fail in both hosts: an overlay host (the Overlay and Custom examples: describe
# fails), OpenGL (the OpenGL example: render fails), the effect's identity (the DrawSuite
# example's render writes nothing: it relies on isIdentity), an output clip in the filter context
# (the Test example).
needs_more=" uk.co.thefoundry.BasicOverlayPlugin uk.co.thefoundry.CustomParamPlugin com.genarts:OpenGLSamplePlugin uk.co.thefoundry.BasicDrawSuitePlugin net.sf.openfx:PropertyTestPlugin "
differs=" org.openeffects.support.choiceParamsPlugin "
# sweep_render MODE ID CONTEXT [ARGS...]: render, writing sweep-MODE.log and sweep-MODE.json
sweep_render() { local mode=$1 id=$2 ctx=$3; shift 3; rm -f "sweep-$mode.json"
  hostdemo "$@" -p "$id" -c "$ctx" --frames 0 -o "Sweep-$mode" --report "sweep-$mode.json" >> "sweep-$mode.log" 2>&1; }
# sweep_json FILE...: one line per file: for a describe, the number of contexts that work and the
# context to render (filter, else general, else -); for a report, the host profile and the crc32
# of the first render's output (- if none). A missing or unreadable file gives "-".
sweep_json() { python3 -I -c 'import json, sys
for path in sys.argv[1:]:
    try:
        with open(path) as f:
            r = json.load(f)
        if r["schema"] == "hostDemo.describe":
            ok = [k for k, v in r["contexts"].items() if v["ok"]]
            print(len(ok), "filter" if "OfxImageEffectContextFilter" in ok else "general" if "OfxImageEffectContextGeneral" in ok else "-")
        else:
            o = r["renders"][0]["output"] if r["renders"] else None
            print(r["host"]["profile"], o["crc32"] if o else "-")
    except Exception:
        print("- -")' "$@"; }
start=$SECONDS
nplugins=0
for id in $(sed -n 's/^Plug-in://p' cachedemo.log | sort -u); do
  rm -f sweep-default.log sweep-ofx14.log sweep-describe.json sweep-describe14.json
  ds=0; hostdemo -p "$id" --describe sweep-describe.json > sweep-default.log 2>&1 || ds=$?
  set -- $(sweep_json sweep-describe.json); nctx=$1 ctx=$2
  if [ "$ctx" = - ]; then
    # nothing to render: the OFX 1.4 host must describe it the same way
    ds14=0; hostdemo --ofx14 -p "$id" --describe sweep-describe14.json > sweep-ofx14.log 2>&1 || ds14=$?
    default="describe=$ds contexts=$nctx"
    ofx14="describe=$ds14 contexts=$(sweep_json sweep-describe14.json | cut -d' ' -f1)"
  else
    rs=0; sweep_render default "$id" "$ctx" || rs=$?
    rs14=0; sweep_render ofx14 "$id" "$ctx" --ofx14 || rs14=$?
    { read -r p crc; read -r p14 crc14; } < <(sweep_json sweep-default.json sweep-ofx14.json)
    case "$differs" in *" $id "*) crc14=$crc ;; esac  # the documented fallback output
    [ "$rs14" != 0 ] || [ "$p14" = ofx14 ] || rs14="0 (profile $p14)"
    default="describe=$ds contexts=$nctx render=$rs crc32=$crc"
    ofx14="describe=$ds contexts=$nctx render=$rs14 crc32=$crc14"
  fi
  case "$needs_more" in *" $id "*) known=1 ;; *) known=0 ;; esac
  if [ "$ofx14" != "$default" ]; then
    echo "--ofx14: $id: $ofx14 in the OFX 1.4 host, $default in the default host"
    echo "--- default host:"; cat sweep-default.log; echo "--- OFX 1.4 host:"; cat sweep-ofx14.log; exit 1
  fi
  works=0; case "$default" in "describe=0 contexts="[1-9]*" render=0 crc32="[0-9a-f]*) works=1 ;; esac
  if [ $known = 0 ] && [ $works = 0 ]; then
    echo "--ofx14: $id: $default"; cat sweep-default.log; exit 1
  fi
  if [ $known = 1 ] && [ $works = 1 ]; then
    echo "--ofx14: NOTICE: $id now works in hostDemo: remove it from needs_more in $0"
  fi
  echo "--ofx14: $id: $ofx14$([ $known = 1 ] && [ $works = 0 ] && echo " (needs a feature hostDemo doesn't have, in both hosts)")"
  nplugins=$((nplugins + 1))
done
echo "--ofx14: $nplugins plug-ins work in the OFX 1.4 host as in the default host ($((SECONDS - start)) s)"
