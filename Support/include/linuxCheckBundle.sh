#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Check that Linux OFX plug-in bundles run on the baseline Linux system (default: EL9, i.e. Rocky
# Linux 9 / VFX Reference Platform CY2027): run it on the installed bundles, e.g. in CI or in an
# installer, after deployment.
#
#   linuxCheckBundle.sh [-v] <bundle.ofx.bundle | file.ofx | directory>...
#
# For each ELF file of the bundles (the plug-ins in Contents/Linux-*/, and the libraries in
# Contents/Libraries/), it checks that:
# - the system libraries are not bundled: glibc (libc, libm, libpthread, libdl, librt, libutil,
#   ld-linux), libstdc++, libgcc_s, libgomp and libGL/libOpenGL/libGLX/libEGL (GLVND) must come from
#   the system, and every other DT_NEEDED library is in the bundle's Contents/Libraries;
# - every versioned symbol it needs exists on the baseline system: GLIBC, GLIBCXX, CXXABI, GCC (libgcc_s)
#   and GOMP/OMP versions are at most the baseline's.
#
# The baseline limits can be set in the environment (the defaults are EL9's, measured with
# `readelf -V` on rockylinux/rockylinux:9, glibc 2.34-290.el9_9, GCC 11.5.0-17.el9_9):
#   OFX_ABI_GLIBC      2.34    (plus the GLIBC_2.35 symbols that EL9 backported, OFX_ABI_GLIBC_EXTRA)
#   OFX_ABI_GLIBCXX    3.4.29
#   OFX_ABI_CXXABI     1.3.13
#   OFX_ABI_GCC        7.0.0 on x86_64, 11.0 on aarch64 (libgcc_s)
#   OFX_ABI_GOMP       5.0.1   (GOMP_ and OMP_ versions of libgomp)
# The exit status is the number of problems (0: all bundles are OK). Needs readelf (binutils).

set -o pipefail

verbose=0
if [ "$1" = "-v" ]; then verbose=1; shift; fi
if [ $# -eq 0 ]; then
  echo "usage: $0 [-v] <bundle.ofx.bundle | file.ofx | directory>..." >&2
  exit 1
fi
if ! command -v readelf > /dev/null; then
  echo "$0: readelf (binutils) is needed" >&2
  exit 1
fi

arch=$(uname -m)
OFX_ABI_GLIBC=${OFX_ABI_GLIBC:-2.34}
OFX_ABI_GLIBC_EXTRA=${OFX_ABI_GLIBC_EXTRA:-"__memcmpeq epoll_pwait2 __rseq_flags __rseq_offset __rseq_size"}
OFX_ABI_GLIBCXX=${OFX_ABI_GLIBCXX:-3.4.29}
OFX_ABI_CXXABI=${OFX_ABI_CXXABI:-1.3.13}
if [ -z "$OFX_ABI_GCC" ]; then
  case "$arch" in
    aarch64|arm64) OFX_ABI_GCC=11.0 ;;
    *) OFX_ABI_GCC=7.0.0 ;;
  esac
fi
OFX_ABI_GOMP=${OFX_ABI_GOMP:-5.0.1}

# the libraries that must come from the system (never bundled)
system_lib() {
  case "$1" in
    libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libutil.so.*|libresolv.so.*|libmvec.so.*) return 0 ;;
    ld-linux*.so.*|ld64.so.*) return 0 ;;
    libstdc++.so.*|libgcc_s.so.*|libgomp.so.*|libatomic.so.*) return 0 ;;
    libGL.so.*|libOpenGL.so.*|libGLX.so.*|libEGL.so.*|libGLdispatch.so.*) return 0 ;;
  esac
  return 1
}

# version_le A B: A <= B (dotted versions)
version_le() {
  [ "$1" = "$2" ] || [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$1" ]
}

problems=0
report() {
  echo "$1: $2"
  problems=$((problems + 1))
}

check_elf() {
  local f=$1 libdir=$2
  # DT_NEEDED
  local needed
  needed=$(readelf -d -W "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
  for lib in $needed; do
    if system_lib "$lib"; then
      continue
    fi
    if [ -z "$libdir" ] || [ ! -e "$libdir/$lib" ]; then
      report "$f" "needs $lib, which is neither a system library nor in the bundle's Contents/Libraries"
    fi
  done
  # versioned undefined symbols, e.g. "memcpy@GLIBC_2.14 (2)"
  readelf --dyn-syms -W "$f" 2>/dev/null | awk '$7 == "UND" && $8 ~ /@/ {print $8}' | sed 's/ (.*//' | sort -u |
  while IFS= read -r sv; do
    local sym=${sv%%@*} ver=${sv#*@}
    ver=${ver#@}
    local tag=${ver%%_*} num=${ver#*_} max=""
    case "$tag" in
      GLIBC) max=$OFX_ABI_GLIBC ;;
      GLIBCXX) max=$OFX_ABI_GLIBCXX ;;
      CXXABI) max=$OFX_ABI_CXXABI ;;
      GCC) max=$OFX_ABI_GCC ;;
      GOMP|OMP) max=$OFX_ABI_GOMP ;;
      *) continue ;;
    esac
    # skip the non-numeric tags (e.g. GLIBC_PRIVATE, CXXABI_TM_1, GLIBCXX_LDBL_*)
    case "$num" in
      [0-9]*) ;;
      *) continue ;;
    esac
    if version_le "$num" "$max"; then
      continue
    fi
    if [ "$tag" = GLIBC ] && version_le "$num" 2.35 && [[ " $OFX_ABI_GLIBC_EXTRA " == *" $sym "* ]]; then
      continue # backported in EL9
    fi
    echo "$f: needs $sym@$ver, newer than the baseline $tag $max"
  done > "$tmp"
  if [ -s "$tmp" ]; then
    cat "$tmp"
    problems=$((problems + $(wc -l < "$tmp")))
  fi
  [ "$verbose" = 1 ] && echo "checked $f"
}

check_bundle() {
  local b=$1
  local libdir="$b/Contents/Libraries"
  [ -d "$libdir" ] || libdir=""
  if [ -n "$libdir" ]; then
    for l in "$libdir"/*; do
      [ -e "$l" ] || continue
      if system_lib "$(basename "$l")"; then
        report "$l" "is a system library: it must not be bundled"
      fi
    done
  fi
  while IFS= read -r -d '' f; do
    if system_lib "$(basename "$f")"; then
      continue # already reported above
    fi
    if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
      check_elf "$f" "$libdir"
    fi
  done < <(find "$b/Contents" -type f \( -path '*/Contents/Linux-*/*' -o -path '*/Contents/Libraries/*' \) -print0 2>/dev/null)
}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
for arg in "$@"; do
  if [ -d "$arg" ] && [[ "$arg" == *.ofx.bundle || "$arg" == *.ofx.bundle/ ]]; then
    check_bundle "${arg%/}"
  elif [ -d "$arg" ]; then
    while IFS= read -r -d '' b; do
      check_bundle "$b"
    done < <(find "$arg" -type d -name '*.ofx.bundle' -prune -print0)
  elif [ -f "$arg" ]; then
    check_elf "$arg" ""
  else
    echo "$arg: not found" >&2
    problems=$((problems + 1))
  fi
done
if [ "$problems" -eq 0 ]; then
  echo "linuxCheckBundle: OK (GLIBC $OFX_ABI_GLIBC, GLIBCXX $OFX_ABI_GLIBCXX, CXXABI $OFX_ABI_CXXABI, GCC $OFX_ABI_GCC, GOMP $OFX_ABI_GOMP)"
fi
exit $((problems > 255 ? 255 : problems))
