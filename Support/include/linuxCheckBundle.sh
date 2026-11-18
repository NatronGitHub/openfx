#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Check that Linux OFX plug-in bundles run on the baseline Linux system (default: EL9, i.e. Rocky
# Linux 9 / VFX Reference Platform CY2027). Run it on the installed bundles: in the plug-in
# repositories' CI, and in the installers after the libraries were bundled.
#
#   linuxCheckBundle.sh [-v] [--no-needed] <bundle.ofx.bundle | file | directory>...
#
# A directory that is not a bundle is searched for *.ofx.bundle directories (finding none is a
# problem). A file is checked alone; if it is inside a bundle, with that bundle's context.
#
# Every ELF file of a bundle is checked: Contents/Linux-*/ (which must contain one), Libraries/
# (where Natron's installer bundles libraries, possibly as symlinks), Contents/Libraries/, any
# other place, and their subdirectories. A symlink to an ELF file outside the bundle is checked
# too, as if it were in the link's directory (that is what the loader does with $ORIGIN).
#
# The libraries fall into three classes:
# - system libraries, which must come from the system and must never be bundled (a file with
#   such a name or soname anywhere in the bundle is a problem, versioned or not, e.g.
#   libstdc++.so): glibc (libc, libm, libpthread, libdl, librt, libutil, libresolv, libmvec,
#   libanl, libthread_db, ld-linux), libcrypt (libxcrypt, in EL9's minimal install), libstdc++,
#   libgcc_s, libgomp, libatomic, GLVND (libGL, libGLX, libEGL, libGLdispatch, libOpenGL), and the
#   driver-provided libOpenCL and libcuda, plus the sonames listed in OFX_ABI_SYSTEM_LIBS;
# - optional libraries, which may come from the system or be bundled (OFX_ABI_OPTIONAL_LIBS,
#   default: libz.so.1);
# - all the other libraries (OIIO, OCIO, FFmpeg...), which the installer must bundle: each
#   DT_NEEDED must exist (symlinks are followed) in one of the file's DT_RUNPATH (or DT_RPATH)
#   directories that are inside the bundle, $ORIGIN being the file's own directory. The usual
#   rpath is $ORIGIN/../../Libraries from Contents/Linux-*/, i.e. <bundle>/Libraries, or $ORIGIN
#   for libraries next to the plug-in. A bundled library that needs another bundled library must
#   have its own rpath (e.g. $ORIGIN). --no-needed (or OFX_ABI_CHECK_NEEDED=0) skips this rule,
#   for CI builds against the distribution's libraries; the other checks still apply.
#
# DT_RUNPATH and DT_RPATH: every entry must start with $ORIGIN (or ${ORIGIN}). An absolute entry
# (e.g. a build-machine path such as /opt/rh/gcc-toolset-14/..., /__w/..., or an SDK prefix added
# by CMake's INSTALL_RPATH_USE_LINK_PATH), a relative one or an empty one (both are relative to the
# current directory) is a problem, unless it is listed in OFX_ABI_RPATH_ALLOW (default: none;
# whitespace-separated, exact entries). The loader would search it on the user's machine. An
# $ORIGIN entry that leads outside the bundle is a warning.
#
# Symbol versions: every version that a file needs (readelf -V, .gnu.version_r) must exist on the
# baseline. The limits can be set in the environment; the defaults are EL9's, measured with
# readelf -V on rockylinux/rockylinux:9 (glibc 2.34-290.el9_9, GCC 11.5.0-17.el9_9, zlib 1.2.11):
#   OFX_ABI_GLIBC      2.34    glibc (libc, libm, ld-linux...)
#   OFX_ABI_GLIBCXX    3.4.29  libstdc++
#   OFX_ABI_CXXABI     1.3.13  libstdc++
#   OFX_ABI_GCC        7.0.0 for x86_64 files, 11.0 for aarch64 files (libgcc_s)
#   OFX_ABI_GOMP       5.0.1   libgomp (GOMP_ and OMP_ versions)
#   OFX_ABI_LIBATOMIC  1.2     libatomic
#   OFX_ABI_ZLIB       1.2.9   zlib
# Set a limit to 99 to disable it.
# - GLIBC_PRIVATE and the GLIBC_ABI_* markers (e.g. GLIBC_ABI_DT_RELR, from glibc 2.36's
#   -z pack-relative-relocs, the default on some newer toolchains) are problems; the
#   GLIBC_ABI_* names listed in OFX_ABI_GLIBC_ABI_ALLOW (default: none) are allowed.
# - GLIBC_2.35 is a problem too, even though EL9 backported a few GLIBC_2.35 symbols: the other
#   glibc 2.34 systems don't have them. OFX_ABI_GLIBC_EXTRA (default: none) lists the symbols
#   allowed at exactly GLIBC_2.35, checked symbol by symbol (readelf --dyn-syms). EL9's list is
#   OFX_ABI_GLIBC_EXTRA="__memcmpeq epoll_pwait2 __rseq_flags __rseq_offset __rseq_size
#   _dl_find_object posix_spawn_file_actions_addtcsetpgrp_np"
#
# Warnings (not problems): a file needs libOpenGL.so.0 (EL9 has it in libglvnd-opengl, which
# isn't always installed: link libGL.so.1 instead; CMake's OpenGL::GL is libOpenGL.so.0 unless
# OpenGL_GL_PREFERENCE is LEGACY), or libcrypt.so.1 (EL9's libxcrypt is libcrypt.so.2;
# libcrypt.so.1 needs libxcrypt-compat).
#
# Exit status: 0 if there is no problem, 1 if there are problems (each one is printed, then
# their number), 2 on a usage error or a missing tool.
# Requires bash, readelf (binutils) and GNU coreutils (sort -V, realpath).

set -o pipefail

usage() {
  echo "usage: $0 [-v] [--no-needed] <bundle.ofx.bundle | file | directory>..." >&2
  exit 2
}

verbose=0
check_needed=${OFX_ABI_CHECK_NEEDED:-1}
while [ $# -gt 0 ]; do
  case "$1" in
    -v) verbose=1 ;;
    --no-needed) check_needed=0 ;;
    --) shift; break ;;
    -*) usage ;;
    *) break ;;
  esac
  shift
done
[ $# -gt 0 ] || usage
for tool in readelf sort realpath od; do
  if ! command -v "$tool" > /dev/null; then
    echo "$0: $tool is needed" >&2
    exit 2
  fi
done
if [ "$(printf '1.10\n1.9\n' | sort -V 2>/dev/null | head -1)" != 1.9 ]; then
  echo "$0: sort -V (GNU coreutils) is needed" >&2
  exit 2
fi

OFX_ABI_GLIBC=${OFX_ABI_GLIBC:-2.34}
OFX_ABI_GLIBCXX=${OFX_ABI_GLIBCXX:-3.4.29}
OFX_ABI_CXXABI=${OFX_ABI_CXXABI:-1.3.13}
OFX_ABI_GCC=${OFX_ABI_GCC-}
OFX_ABI_GOMP=${OFX_ABI_GOMP:-5.0.1}
OFX_ABI_LIBATOMIC=${OFX_ABI_LIBATOMIC:-1.2}
OFX_ABI_ZLIB=${OFX_ABI_ZLIB:-1.2.9}
OFX_ABI_GLIBC_EXTRA=${OFX_ABI_GLIBC_EXTRA-}
OFX_ABI_GLIBC_ABI_ALLOW=${OFX_ABI_GLIBC_ABI_ALLOW-}
OFX_ABI_SYSTEM_LIBS=${OFX_ABI_SYSTEM_LIBS-}
OFX_ABI_OPTIONAL_LIBS=${OFX_ABI_OPTIONAL_LIBS-libz.so.1}
OFX_ABI_RPATH_ALLOW=${OFX_ABI_RPATH_ALLOW-}

# word_in WORD LIST: WORD is one of the whitespace-separated words of LIST
word_in() {
  [[ " ${2//[$'\t\n']/ } " == *" $1 "* ]]
}

# the libraries that must come from the system (never bundled), versioned or not
system_lib() {
  case "$1" in
    libc.so|libc.so.*|libm.so|libm.so.*|libpthread.so|libpthread.so.*|libdl.so|libdl.so.*) return 0 ;;
    librt.so|librt.so.*|libutil.so|libutil.so.*|libresolv.so|libresolv.so.*) return 0 ;;
    libmvec.so|libmvec.so.*|libanl.so|libanl.so.*|libthread_db.so|libthread_db.so.*) return 0 ;;
    libcrypt.so|libcrypt.so.*) return 0 ;;
    ld-linux*.so*|ld64.so*|ld.so|ld.so.*) return 0 ;;
    libstdc++.so|libstdc++.so.*|libgcc_s.so|libgcc_s.so.*) return 0 ;;
    libgomp.so|libgomp.so.*|libatomic.so|libatomic.so.*) return 0 ;;
    libGL.so|libGL.so.*|libGLX.so|libGLX.so.*|libEGL.so|libEGL.so.*) return 0 ;;
    libGLdispatch.so|libGLdispatch.so.*|libOpenGL.so|libOpenGL.so.*) return 0 ;;
    libGLESv1_CM.so|libGLESv1_CM.so.*|libGLESv2.so|libGLESv2.so.*) return 0 ;;
    libnsl.so.*|libnss_*.so.*|libBrokenLocale.so.*|libc_malloc_debug.so.*) return 0 ;;
    libOpenCL.so|libOpenCL.so.*|libcuda.so|libcuda.so.*) return 0 ;;
  esac
  word_in "$1" "$OFX_ABI_SYSTEM_LIBS"
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
warn() {
  echo "$1: warning: $2"
}

is_elf() {
  [ "$(head -c 4 "$1" 2>/dev/null | od -An -c | tr -d ' ')" = '177ELF' ]
}

# inside PATH ROOT: the canonical PATH is ROOT or below it (ROOT is canonical)
inside() {
  local p
  p=$(realpath -m "$1" 2>/dev/null) || return 1
  [ "$p" = "$2" ] || [[ "$p" == "$2"/* ]]
}

# the undefined dynamic symbols of file F at version VER (readelf --dyn-syms: the name is the
# last field once a trailing "(n)" is dropped, and the section index is the field before it)
undefined_symbols_at() {
  readelf --dyn-syms -W "$1" 2>/dev/null | awk -v ver="$2" '
    $1 ~ /^[0-9]+:$/ {
      n = NF
      if ($n ~ /^\([0-9]+\)$/) n--
      if (n < 2 || $(n - 1) != "UND") next
      name = $n
      at = index(name, "@")
      if (at == 0) next
      v = substr(name, at + 1); sub(/^@/, "", v)
      if (v == ver) print substr(name, 1, at - 1)
    }' | sort -u
}

# check_elf FILE ROOT: check one ELF file. ROOT is the canonical bundle directory (the scope of
# "bundled"), or the file's directory for a file outside any bundle.
check_elf() {
  local f=$1 root=$2
  local header dyn machine soname
  # readelf prints "readelf: <file>: Error: ..." on a truncated file, but may still exit with 0
  if ! header=$(readelf -h -W "$f" 2>&1) || [[ "$header" == *": Error: "* ]] ||
     ! dyn=$(readelf -d -W "$f" 2>&1) || [[ "$dyn" == *": Error: "* ]]; then
    report "$f" "readelf can't read it (truncated or corrupt ELF file?): $(printf '%s\n%s\n' "$header" "$dyn" | grep -m1 'Error: ' | sed 's/.*Error: //')"
    return
  fi
  machine=$(echo "$header" | sed -n 's/^ *Machine: *//p')
  local gcc_max=$OFX_ABI_GCC
  if [ -z "$gcc_max" ]; then
    case "$machine" in
      *AArch64*) gcc_max=11.0 ;;
      *) gcc_max=7.0.0 ;;
    esac
  fi
  soname=$(echo "$dyn" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
  if [ -n "$soname" ] && system_lib "$soname" && ! system_lib "$(basename "$f")"; then
    report "$f" "is the system library $soname: it must not be bundled"
  fi

  # DT_NEEDED, resolved through the DT_RUNPATH (or DT_RPATH) directories inside the bundle
  local dir runpath entry d
  dir=$(dirname "$f")
  runpath=$(echo "$dyn" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')
  [ -n "$runpath" ] || runpath=$(echo "$dyn" | sed -n 's/.*(RPATH).*\[\(.*\)\]/\1/p')
  local -a entries=() dirs=()
  local rpathtag=RUNPATH
  [ -n "$(echo "$dyn" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')" ] || rpathtag=RPATH
  if echo "$dyn" | grep -q '(RUNPATH)\|(RPATH)'; then
    # read -a drops a trailing empty entry: check "a:" and "" by hand
    case "$runpath" in
      ""|:*|*:|*::*) report "$f" "has an empty $rpathtag entry ($runpath), which the loader takes as the current directory" ;;
    esac
  fi
  IFS=: read -r -a entries <<< "$runpath"
  for entry in "${entries[@]}"; do
    case "$entry" in
      '') ;; # reported above
      '$ORIGIN'|'$ORIGIN/'*|'${ORIGIN}'|'${ORIGIN}/'*) ;;
      *)
        if ! word_in "$entry" "$OFX_ABI_RPATH_ALLOW"; then
          report "$f" "has the $rpathtag entry $entry, which doesn't start with \$ORIGIN (a build-machine path?): the loader would search it on the user's machine (see OFX_ABI_RPATH_ALLOW)"
        fi ;;
    esac
    d=${entry//\$\{ORIGIN\}/"$dir"} # (quoted: bash 5.2 expands & in a replacement)
    d=${d//\$ORIGIN/"$dir"}
    if [ -n "$d" ] && inside "$d" "$root"; then
      dirs+=("$d")
    elif [ -n "$d" ] && [ "$d" != "$entry" ]; then
      warn "$f" "its $rpathtag entry $entry leads outside the bundle"
    fi
  done
  # DT_RELR (e.g. -z pack-relative-relocs) needs glibc 2.36's loader, even without a version need
  if echo "$dyn" | grep -q '(RELR)'; then
    report "$f" "uses DT_RELR relocations (-z pack-relative-relocs), which the baseline's loader doesn't support"
  fi
  local lib found
  for lib in $(echo "$dyn" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    case "$lib" in
      libOpenGL.so.0) warn "$f" "needs libOpenGL.so.0, which EL9 has in libglvnd-opengl (not always installed): link libGL.so.1 instead (CMake: OpenGL_GL_PREFERENCE=LEGACY)" ;;
      libcrypt.so.1) warn "$f" "needs libcrypt.so.1, which EL9 has in libxcrypt-compat (not always installed; EL9's libxcrypt is libcrypt.so.2)" ;;
    esac
    if system_lib "$lib" || word_in "$lib" "$OFX_ABI_OPTIONAL_LIBS" || [ "$check_needed" = 0 ]; then
      continue
    fi
    found=0
    for d in "${dirs[@]}"; do
      if [ -e "$d/$lib" ]; then
        found=1
        break
      fi
    done
    if [ "$found" = 0 ]; then
      report "$f" "needs $lib, which is neither a system library nor in the bundle (RUNPATH: ${runpath:-none})"
    fi
  done

  # needed symbol versions (.gnu.version_r)
  local file ver tag num max syms bad s
  while read -r file ver; do
    [ -n "$ver" ] || continue
    case "$ver" in
      GLIBC_ABI_*)
        if ! word_in "$ver" "$OFX_ABI_GLIBC_ABI_ALLOW"; then
          report "$f" "needs $ver (from $file), which the baseline glibc doesn't have (see OFX_ABI_GLIBC_ABI_ALLOW)"
        fi
        continue ;;
      GLIBC_PRIVATE)
        report "$f" "needs GLIBC_PRIVATE (from $file): it only works with the glibc build it was linked with"
        continue ;;
    esac
    tag=${ver%_*} num=${ver##*_}
    case "$tag" in
      GLIBC) max=$OFX_ABI_GLIBC ;;
      GLIBCXX) max=$OFX_ABI_GLIBCXX ;;
      CXXABI) max=$OFX_ABI_CXXABI ;;
      GCC) max=$gcc_max ;;
      GOMP|OMP) max=$OFX_ABI_GOMP ;;
      LIBATOMIC) max=$OFX_ABI_LIBATOMIC ;;
      ZLIB) max=$OFX_ABI_ZLIB ;;
      *) continue ;; # other libraries' versions, CXXABI_TM_1, GLIBCXX_LDBL_3.4, GOMP_PLUGIN_1.0...
    esac
    case "$num" in
      [0-9]*) ;;
      *) continue ;;
    esac
    version_le "$num" "$max" && continue
    syms=$(undefined_symbols_at "$f" "$ver" | tr '\n' ' ')
    syms=${syms% }
    if [ "$ver" = GLIBC_2.35 ] && [ -n "$syms" ]; then
      bad=""
      for s in $syms; do
        word_in "$s" "$OFX_ABI_GLIBC_EXTRA" || bad="$bad $s"
      done
      if [ -z "$bad" ]; then
        [ "$verbose" = 1 ] && echo "$f: GLIBC_2.35 symbols allowed by OFX_ABI_GLIBC_EXTRA: $syms"
        continue
      fi
      syms=${bad# }
    fi
    report "$f" "needs $ver (from $file), newer than the baseline $tag $max${syms:+: $syms}"
  done < <(readelf -V -W "$f" 2>/dev/null | awk '
    /^Version needs section/ { inr = 1; next }
    /^Version .* section/ { inr = 0; next }
    inr {
      for (i = 1; i < NF; i++) {
        if ($i == "File:") file = $(i + 1)
        else if ($i == "Name:") print file, $(i + 1)
      }
    }' | sort -u)
  [ "$verbose" = 1 ] && echo "checked $f"
  return 0
}

# the canonical bundle directory containing PATH, if any
bundle_of() {
  local p
  p=$(realpath -m "$1")
  while [ "$p" != / ] && [ -n "$p" ]; do
    if [[ "$p" == *.ofx.bundle ]] && [ -d "$p" ]; then
      echo "$p"
      return 0
    fi
    p=$(dirname "$p")
  done
  return 1
}

check_bundle() {
  local b=${1%/}
  local root
  root=$(realpath "$b") || { report "$b" "can't resolve the bundle path"; return; }
  local f target nbin=0
  local -A seen=()
  while IFS= read -r -d '' f; do
    if system_lib "$(basename "$f")"; then
      report "$f" "is a system library: it must not be bundled"
      continue
    fi
    if [ -L "$f" ]; then
      if [ ! -e "$f" ]; then
        warn "$f" "dangling symlink"
        continue
      fi
      [ -f "$f" ] || continue
      target=$(realpath "$f")
      # a target inside the bundle is checked as a file of its own
      inside "$target" "$root" && continue
      [ -n "${seen[$target]}" ] && continue
      seen[$target]=1
    fi
    is_elf "$f" || continue
    case "$f" in
      "$b"/Contents/Linux-*/*) nbin=$((nbin + 1)) ;;
    esac
    check_elf "$f" "$root"
  done < <(find -H "$b" \( -type f -o -type l \) -print0 2>/dev/null | sort -z)
  if [ "$nbin" = 0 ]; then
    report "$b" "has no ELF file in Contents/Linux-*/"
  fi
}

for arg in "$@"; do
  if [ -d "$arg" ] && [[ "${arg%/}" == *.ofx.bundle ]]; then
    check_bundle "$arg"
  elif [ -d "$arg" ]; then
    n=0
    while IFS= read -r -d '' b; do
      check_bundle "$b"
      n=$((n + 1))
    done < <(find -H "$arg" -type d -name '*.ofx.bundle' -prune -print0 | sort -z)
    if [ "$n" = 0 ]; then
      report "$arg" "contains no *.ofx.bundle"
    fi
  elif [ -f "$arg" ]; then
    if ! is_elf "$arg"; then
      report "$arg" "is not an ELF file"
    elif system_lib "$(basename "$arg")"; then
      report "$arg" "is a system library: it must not be bundled"
    else
      root=$(bundle_of "$arg") || root=$(realpath "$(dirname "$arg")")
      check_elf "$arg" "$root"
    fi
  else
    report "$arg" "not found"
  fi
done
limits="GLIBC $OFX_ABI_GLIBC${OFX_ABI_GLIBC_EXTRA:+ (and at 2.35: $OFX_ABI_GLIBC_EXTRA)}, GLIBCXX $OFX_ABI_GLIBCXX, CXXABI $OFX_ABI_CXXABI, GCC ${OFX_ABI_GCC:-7.0.0 (x86_64) or 11.0 (aarch64)}, GOMP $OFX_ABI_GOMP, LIBATOMIC $OFX_ABI_LIBATOMIC, ZLIB $OFX_ABI_ZLIB"
[ "$check_needed" = 0 ] && limits="$limits; needed libraries not checked"
if [ "$problems" -eq 0 ]; then
  echo "linuxCheckBundle: OK ($limits)"
  exit 0
fi
echo "linuxCheckBundle: $problems problem(s) ($limits)"
exit 1
