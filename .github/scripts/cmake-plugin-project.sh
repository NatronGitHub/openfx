#!/bin/bash
# Build cmake/tests/plugin-project against an installed openfx, the way the plug-in repositories
# use cmake/OpenFX.cmake, and check the bundles it makes.
# usage: cmake-plugin-project.sh <openfx install prefix> <work dir>
set -eo pipefail
prefix=$1 work=$2
src="$(cd "$(dirname "$0")/../.." && pwd)"
os=$(uname -s)
deploy=OFF; [ "$os" = Darwin ] && deploy=ON
rm -rf "$work"
run() { # mode: package | subdirectory
  local mode=$1 w="$work/$1" via=OFF
  [ "$mode" = subdirectory ] && via=ON
  cmake -S "$src/cmake/tests/plugin-project" -B "$w/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" -DOFX_SOURCE_DIR="$src" -DOFX_VIA_SUBDIRECTORY=$via \
    -DCMAKE_INSTALL_PREFIX="$w/install" -DOFX_DEPLOY_BUNDLES=$deploy ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
  cmake --build "$w/build" --parallel
  cmake --install "$w/build"
  # openfx's own libraries and headers aren't installed by a project that uses add_subdirectory
  if [ "$mode" = subdirectory ] && [ -e "$w/install/include" ]; then fail "add_subdirectory(openfx) installed openfx itself"; fi
  check "$w/build/Plugins" "$w/install/OFX/Plugins"
}
fail() { echo "plugin-project: FAILED: $*"; exit 1; }
check() {
built=$1 installed=$2
case $os in
  Darwin) arch=MacOS ;;
  *) arch=$(ls "$installed/InvertBundle.ofx.bundle/Contents" | grep -v -e Info.plist -e Resources | head -1) ;;
esac
for root in "$built" "$installed"; do
  b="$root/InvertBundle.ofx.bundle/Contents"
  test -f "$b/$arch/InvertBundle.ofx" || fail "no $b/$arch/InvertBundle.ofx"
  grep -q '<string>org.example.InvertBundle</string>' "$b/Info.plist" || fail "$b/Info.plist: identifier"
  grep -q '<string>2.3.4</string>' "$b/Info.plist" || fail "$b/Info.plist: version"
  grep -q '<string>InvertBundle.ofx</string>' "$b/Info.plist" || fail "$b/Info.plist: executable"
  test -f "$b/Resources/icon.txt" -a -f "$b/Resources/presets/preset.txt" || fail "$b/Resources"
  p="$root/TestPlist.ofx.bundle/Contents/Info.plist"
  grep -q '<string>TestPlist.ofx</string>' "$p" || fail "$p: @PLUGIN_EXE@ not substituted"
  grep -q 'org.example.OwnPlist' "$p" && grep -qF '${NOT_EXPANDED}' "$p" || fail "$p: not used as-is"
done
ofx="$installed/InvertBundle.ofx.bundle/Contents/$arch/InvertBundle.ofx"
case $os in
  Darwin) exports=$(nm -gU "$ofx" | awk '$2 == "T" { print $3 }' | sort -u | tr '\n' ' ')
          rpath=$(otool -l "$ofx" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }' | sort -u | tr '\n' ' ') ;;
  *) exports=$(nm -D --defined-only "$ofx" | awk '$2 == "T" { print $3 }' | sort -u | tr '\n' ' ')
     rpath=$(readelf -d "$ofx" | sed -n 's/.*R.*PATH.*\[\(.*\)\]/\1/p' | tr '\n' ' ') ;;
esac
case $os in Darwin) want="_OfxGetNumberOfPlugins _OfxGetPlugin " ;; *) want="OfxGetNumberOfPlugins OfxGetPlugin " ;; esac
[ "$exports" = "$want" ] || fail "exported: '$exports', expected '$want'"
case $os in
  Darwin) [ "$rpath" = "@loader_path/../Frameworks @loader_path/../Libraries " ] || fail "rpath: '$rpath'" ;;
  Linux|FreeBSD) [ "$rpath" = '$ORIGIN/../../Libraries ' ] || fail "rpath: '$rpath'" ;;
esac
if [ "$deploy" = ON ]; then
  b="$installed/InvertBundle.ofx.bundle"
  codesign --verify --strict "$b" || fail "codesign --verify --strict $b"
  if otool -L "$ofx" | grep -q libomp; then
    ls "$b/Contents/Libraries/" | grep -q libomp || fail "libomp not bundled"
  fi
  ! otool -L "$ofx" "$b"/Contents/Libraries/* 2>/dev/null | grep -E '^\s+/(opt/local|usr/local|opt/homebrew)/' || fail "references outside the bundle"
fi
echo "plugin-project ($installed): OK (exports: $exports; rpath: $rpath)"
}
run package
run subdirectory
