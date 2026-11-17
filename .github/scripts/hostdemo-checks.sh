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
