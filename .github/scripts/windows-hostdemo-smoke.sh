#!/bin/bash
# Windows (MSYS2) smoke test of ofxhost.hostdemo, hostDemo's command line in-process, against
# hostDemo.exe: one --describe and one small render of Invert, with hostDemo.exe, with
# python -m ofxhost.hostdemo, and with the hostdemo-inprocess script (which converts its path for
# the native python.exe), then the same runs in the batch mode, with a worker killed by the test
# hook. All must give the same exit status, describe JSON, files and crc32.
# Run from the repository root, after the CMake build in build/ (build.yml).
set -eo pipefail
root=$PWD
hd=$root/$(find build -name hostDemo.exe -type f | head -1)
export OFX_PLUGIN_PATH="$(cygpath -w "$root/build/Plugins")"
export PYTHONPATH="$(cygpath -w "$root/build/HostSupport/python")"
args=(--no-std-paths --no-cache --quiet -p net.sf.openfx.invertPlugin -d float --frames 0
      --window 0,0,16,8 -i Source=ramp --format pfm -o out --report r.json)
for h in hd py sh; do
  mkdir -p "smoke/$h" && cd "smoke/$h"
  case $h in
    hd) run=("$hd") ;;
    py) run=(python -m ofxhost.hostdemo) ;;
    sh) run=(env OFXHOST_PYTHON=python "$root/HostSupport/examples/python/hostdemo-inprocess") ;;
  esac
  [ $h = hd ] || export PYTHONPATH="$PYTHONPATH;$(cygpath -w "$root/HostSupport/examples/python")"
  "${run[@]}" --no-std-paths --no-cache --quiet -p net.sf.openfx.invertPlugin --describe d.json
  "${run[@]}" "${args[@]}"
  ls out.0*.pfm
  cd "$root"
done
# the batch mode: the same two runs in two workers, then a worker killed by the test hook
# (the command is crashed, the worker replaced: the next command runs)
mkdir -p smoke/bt && cd smoke/bt
{ echo "--no-std-paths --no-cache --quiet -p net.sf.openfx.invertPlugin --describe d.json"
  echo "${args[*]}"
  echo "--env OFXHOST_BATCH_TEST_HOOK=kill --no-std-paths --no-cache --quiet --describe k.json"
  echo "--no-std-paths --no-cache --quiet --describe k2.json"; } > cmds.txt
st=0
python -m ofxhost.hostdemo --batch cmds.txt -j 2 --batch-report recs.jsonl || st=$?
test $st = 1
cd "$root"
python - <<'PY'
import glob, json, os
# the output files, byte for byte (MSYS2 has no cmp without diffutils)
files = sorted(glob.glob("smoke/hd/out.0*.pfm"))
assert files
for h in ("py", "sh", "bt"):
    for f in files:
        g = f"smoke/{h}/{os.path.basename(f)}"
        assert open(f, "rb").read() == open(g, "rb").read(), g
def load(p):
    return json.load(open(p), parse_constant=lambda c: (_ for _ in ()).throw(ValueError(c)))
def nopath(d):  # plugin.filePath: as found by hostDemo, absolute in ofxh (a documented difference)
    if isinstance(d, dict):
        return {k: nopath(v) for k, v in d.items() if k != "filePath"}
    return [nopath(v) for v in d] if isinstance(d, list) else d
for h in ("py", "sh", "bt"):
    assert nopath(load(f"smoke/{h}/d.json")) == nopath(load("smoke/hd/d.json")), h
    crc = lambda p: [r["output"]["crc32"] for r in load(p)["renders"]]
    assert crc(f"smoke/{h}/r.json") == crc("smoke/hd/r.json"), h
recs = [json.loads(x) for x in open("smoke/bt/recs.jsonl")]
assert [(r["ok"], r["crashed"]) for r in recs] == [(True, False)] * 2 + [(False, True), (True, False)], recs
assert "batch test hook: kill" in recs[2]["stdout"], recs[2]
print("ofxhost.hostdemo: same as hostDemo (also --batch)")
PY
