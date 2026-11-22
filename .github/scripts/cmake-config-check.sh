#!/bin/bash
# Build cmake/tests/config-check: the link-time check of the OFX_EXTENSIONS_*/OFX_SUPPORTS_*
# macros (NatronGitHub/openfx#226). A plug-in on openfx::Support and a host on
# openfx::HostSupport compiled with the libraries' definitions must link (and the plug-in must
# not export the configuration object); compiled with any covered macro flipped, they must fail
# to link, with an error naming the configuration object.
# usage: cmake-config-check.sh <work dir>  (CMAKE_GENERATOR: the generator, also multi-config)
set -eo pipefail
work=${1:?usage: cmake-config-check.sh <work dir>}
src="$(cd "$(dirname "$0")/../.." && pwd)"
rm -rf "$work"
build=$work/build
osxtarget=""
[ "$(uname -s)" = Darwin ] && osxtarget=-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake -S "$src/cmake/tests/config-check" -B "$build" -DOFX_SOURCE_DIR="$src" \
  -DCMAKE_BUILD_TYPE=Release ${osxtarget} ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
cmake --build "$build" --config Release --parallel 4
plugin=$(find "$build" -name plugin.ofx -type f -not -path '*/openfx/*' | head -1)
test -n "$plugin" || { echo "config-check: plugin.ofx was not built"; exit 1; }
# the host constructs its ImageEffect::Host and runs (not on Windows: expat's DLL may not be in PATH)
case "$(uname -s)" in
  Darwin|Linux)
    host=$(find "$build" -name host -type f -perm -u+x -not -path '*/openfx/*' | head -1)
    test -n "$host" || { echo "config-check: host was not built"; exit 1; }
    "$host" || { echo "config-check: host failed"; exit 1; } ;;
esac
# the configuration object is hidden: not exported by the plug-in
case "$(uname -s)" in
  Darwin) exports=$(nm -gU "$plugin") ;;
  Linux) exports=$(nm -D --defined-only "$plugin") ;;
  *) exports=$(objdump -p "$plugin") ;;
esac
# (a here-string: under pipefail, grep -q could make the writer of a pipe fail with SIGPIPE)
if grep -q 'Configuration_' <<< "$exports"; then
  echo "config-check: the plug-in exports its configuration object:"
  grep 'Configuration_' <<< "$exports"
  exit 1
fi
fail=0 n=0
while read -r target symbol; do
  [ -n "$target" ] || continue
  n=$((n + 1))
  log="$work/$target.log"
  if cmake --build "$build" --config Release --target "$target" > "$log" 2>&1; then
    echo "config-check: $target linked (a configuration mismatch must fail to link)"; fail=1
  elif grep -Eq '\.(h|cpp)(:[0-9]+)+: (fatal )?error|\.(h|cpp)\([0-9]+\) ?: (fatal )?error' "$log"; then
    # (GCC/Clang file:line: error, MSVC file(line): error)
    echo "config-check: $target does not compile (it must compile, and fail to link):"; tail -n 30 "$log"; fail=1
  elif ! grep -q "$symbol" "$log"; then
    echo "config-check: $target failed, but not by an undefined $symbol*:"; tail -n 30 "$log"; fail=1
  else
    echo "config-check: $target fails to link: $(grep -o "${symbol}[A-Za-z0-9_]*" "$log" | head -1)"
  fi
done < "$build/mismatch-targets.txt"
[ "$n" -gt 0 ] || { echo "config-check: no mismatch target"; exit 1; }
[ "$fail" = 0 ] || exit 1
echo "config-check: OK ($n mismatches fail to link)"
