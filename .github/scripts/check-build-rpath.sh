#!/bin/bash
# ELF platforms: every build-tree plug-in has the install RUNPATH (OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH),
# not CMake's build rpath padded with ':' (empty entries: the current directory), and is installed
# with the same RUNPATH. Fails if no plug-in is found.
# usage: check-build-rpath.sh <build-tree plug-in dir> <installed plug-in dir> <message prefix>
set -eo pipefail
built=$1 installed=$2 label=$3
fail() { echo "$label: FAILED: $*"; exit 1; }
runpath() { readelf -d "$1" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p'; }
n=0
while IFS= read -r -d '' f; do
  rp=$(runpath "$f")
  [ -n "$rp" ] || fail "build-tree $f: no RUNPATH"
  case "$rp" in :*|*:|*::*) fail "build-tree $f: empty RUNPATH/RPATH entry: '$rp'" ;; esac
  i="$installed/${f#"$built/"}"
  [ -f "$i" ] || fail "build-tree $f: not installed as $i"
  irp=$(runpath "$i")
  [ "$rp" = "$irp" ] || fail "build-tree $f: RUNPATH '$rp', installed: '$irp'"
  n=$((n + 1))
done < <(find "$built" -path '*.ofx.bundle/Contents/*/*.ofx' -type f -print0)
[ "$n" -gt 0 ] || fail "no build-tree plug-in in $built"
echo "$label: build-tree RUNPATH of $n plug-ins: no empty entry, same as installed"
