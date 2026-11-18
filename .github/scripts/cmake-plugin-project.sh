#!/bin/bash
# Build cmake/tests/plugin-project against an installed openfx, the way the plug-in repositories
# use cmake/OpenFX.cmake, and check the bundles it makes.
# usage: cmake-plugin-project.sh <openfx install prefix> <work dir>
set -eo pipefail
prefix=$1 work=$2
src="$(cd "$(dirname "$0")/../.." && pwd)"
os=$(uname -s)
deploy=OFF; osxtarget=""
# macOS: deploy, with a deployment target older than MacPorts' libomp (built for the runner's
# macOS): libomp must get the minos warning, the plug-in itself must not. (14.0, the plug-ins'
# target: VFX Reference Platform CY2027. The runner must be newer than macOS 14, so that MacPorts'
# libomp, built for the runner's macOS, is newer than the target)
[ "$os" = Darwin ] && deploy=ON osxtarget=-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
# OpenMP must be found: MacPorts libomp on macOS, gcc's libgomp on Linux and MSYS2 (the
# mingw-w64-ucrt-x86_64-libgomp package)
openmp=ON
rm -rf "$work"
# macOS: a library with an @rpath install name in an "SDK" prefix (as built by CMake's defaults)
sdkopt=()
if [ "$os" = Darwin ]; then
  mkdir -p "$work/sdk/lib"
  echo 'int ofx_test_sdk_dep(void) { return 1; }' > "$work/sdk/dep.c"
  cc -dynamiclib -mmacosx-version-min=14.0 -arch "$(uname -m)" -install_name @rpath/libsdkdep.dylib -o "$work/sdk/lib/libsdkdep.dylib" "$work/sdk/dep.c"
  sdkopt=(-DOFX_TEST_SDK_LIBRARY="$work/sdk/lib/libsdkdep.dylib")
fi
run() { # mode: package | subdirectory
  local mode=$1 w="$work/$1" via=OFF
  [ "$mode" = subdirectory ] && via=ON
  cmake -S "$src/cmake/tests/plugin-project" -B "$w/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" -DOFX_SOURCE_DIR="$src" -DOFX_VIA_SUBDIRECTORY=$via -DBUILD_EXAMPLE_HOSTS=$via -DOFX_TEST_REQUIRE_OPENMP=$openmp \
    -DCMAKE_INSTALL_PREFIX="$w/install" -DOFX_DEPLOY_BUNDLES=$deploy $osxtarget "${sdkopt[@]}" ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
  cmake --build "$w/build" --parallel
  # with add_subdirectory(openfx), BUILD_EXAMPLE_HOSTS=ON still gives the hostDemo target
  if [ "$mode" = subdirectory ]; then cmake --build "$w/build" --parallel --target hostDemo; fi
  cmake --install "$w/build" 2>&1 | tee "$w/install.log"
  if [ "$os" = Darwin ]; then
    # installing again: the plug-in binaries are copied again (not "Up-to-date"), so that the
    # install rpaths aren't added twice (install_name_tool: "would duplicate path"). The race
    # (signed within the second of the link: the same mtime as the built file) is forced here.
    for f in "$w/install/OFX/Plugins"/*.ofx.bundle/Contents/MacOS/*.ofx; do
      touch -r "$w/build/Plugins/${f#"$w/install/OFX/Plugins/"}" "$f"
    done
    cmake --install "$w/build" > "$w/install2.log" 2>&1 || { cat "$w/install2.log"; fail "second install"; }
    if grep -E "Up-to-date: .*\.ofx$|would duplicate" "$w/install2.log"; then fail "second install: binaries not copied again"; fi
  fi
  if [ "$deploy" = ON ]; then
    warned=$(tr -s ' \n' ' ' < "$w/install.log" | grep -oE '[A-Za-z]+\.(ofx|dylib) \([a-z0-9_]+\) requires macOS' | sort -u | tr '\n' ' ' || true)
    echo "minos warnings: $warned"
    case $warned in *InvertBundle.ofx*) fail "minos warning on the plug-in itself: $warned" ;; esac
    case $warned in *libomp.dylib*) ;; *) fail "no minos warning on libomp: $warned" ;; esac
  fi
  # openfx's own libraries and headers aren't installed by a project that uses add_subdirectory
  # (nor the example hosts, nor expat)
  if [ "$mode" = subdirectory ] && [ "$(ls "$w/install")" != OFX ]; then fail "add_subdirectory(openfx) installed openfx itself: $(ls "$w/install")"; fi
  check "$w/build/Plugins" "$w/install/OFX/Plugins"
  # Linux: OFX_LINUX_CHECK_BUNDLE (the installed script with find_package, the source tree's with
  # add_subdirectory) passes on the installed bundles, with the build machine's limits
  if [ "$os" = Linux ]; then
    lcb=$(cat "$w/build/ofx_linux_check_bundle.txt")
    case $mode in
      package) [ "$lcb" = "$prefix/lib/cmake/openfx/linuxCheckBundle.sh" ] || fail "OFX_LINUX_CHECK_BUNDLE: $lcb" ;;
      subdirectory) [ "$lcb" = "$src/Support/include/linuxCheckBundle.sh" ] || fail "OFX_LINUX_CHECK_BUNDLE: $lcb" ;;
    esac
    # the test plug-ins have absolute RPATH entries on purpose (add_ofx_plugin(RPATH)): they must be
    # reported, and accepted once listed in OFX_ABI_RPATH_ALLOW
    local limits="OFX_ABI_GLIBC=99 OFX_ABI_GLIBCXX=99 OFX_ABI_CXXABI=99 OFX_ABI_GCC=99 OFX_ABI_GOMP=99 OFX_ABI_LIBATOMIC=99 OFX_ABI_ZLIB=99"
    local out
    out=$(env $limits "$lcb" "$w/install/OFX/Plugins") && fail "linuxCheckBundle.sh: absolute RPATH entries not reported"
    case "$out" in
      *"PATH entry /opt/ofx-test-rpath,"*"PATH entry /usr/local/ofx-test-keep,"*|*"PATH entry /usr/local/ofx-test-keep,"*"PATH entry /opt/ofx-test-rpath,"*) ;; # RUNPATH or RPATH
      *) echo "$out"; fail "linuxCheckBundle.sh: unexpected report" ;;
    esac
    env $limits OFX_ABI_RPATH_ALLOW="/opt/ofx-test-rpath /usr/local/ofx-test-keep" \
      "$lcb" "$w/install/OFX/Plugins" || fail "linuxCheckBundle.sh"
  fi
}
fail() { echo "plugin-project: FAILED: $*"; exit 1; }
check() {
built=$1 installed=$2
case $os in
  Darwin) arch=MacOS ;;
  *) arch=$(ls "$installed/InvertBundle.ofx.bundle/Contents" | grep -v -e Info.plist -e Resources | head -1)
     case $os in
       Linux) [ "$arch" = "Linux-$(uname -m | sed 's/^x86_64$/x86-64/')" ] || fail "arch folder $arch" ;;
       MINGW*|MSYS*) [ "$arch" = Win64 ] || fail "arch folder $arch" ;;
     esac ;;
esac
for root in "$built" "$installed"; do
  b="$root/InvertBundle.ofx.bundle/Contents"
  test -f "$b/$arch/InvertBundle.ofx" || fail "no $b/$arch/InvertBundle.ofx"
  grep -q '<string>org.example.InvertBundle</string>' "$b/Info.plist" || fail "$b/Info.plist: identifier"
  grep -q '<string>2.3.4</string>' "$b/Info.plist" || fail "$b/Info.plist: version"
  grep -q '<string>InvertBundle.ofx</string>' "$b/Info.plist" || fail "$b/Info.plist: executable"
  test -f "$b/Resources/icon.txt" -a -f "$b/Resources/presets/preset.txt" -a -f "$b/Resources/shaders/a.glsl" || fail "$b/Resources"
  p="$root/TestPlist.ofx.bundle/Contents/Info.plist"
  grep -q '<string>TestPlist.ofx</string>' "$p" || fail "$p: @PLUGIN_EXE@ not substituted"
  grep -q 'org.example.OwnPlist' "$p" && grep -qF '${NOT_EXPANDED}' "$p" || fail "$p: not used as-is"
done
ofx="$installed/InvertBundle.ofx.bundle/Contents/$arch/InvertBundle.ofx"
case $os in
  Darwin) exports=$(nm -gU "$ofx" | awk '$2 == "T" { print $3 }' | sort -u | tr '\n' ' ')
          rpath=$(otool -l "$ofx" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }' | sort -u | tr '\n' ' ') ;;
  MINGW*|MSYS*|CYGWIN*)
     # the export table of the DLL: "[ordinal] name" lines
     # ("[   0] +base[   1]  0000 OfxGetNumberOfPlugins" with recent binutils): the last field
     exports=$(objdump -p "$ofx" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' | awk '/^[[:space:]]*\[ *[0-9]+\]/ { print $NF }' | sort -u | tr '\n' ' ')
     rpath="" ;;
  # binutils >= 2.42 prints versioned names (OfxGetPlugin@@OFX_1.0)
  *) exports=$(nm -D --defined-only "$ofx" | awk '$2 == "T" { print $3 }' | sed 's/@.*//' | sort -u | tr '\n' ' ')
     rpath=$(readelf -d "$ofx" | sed -n 's/.*R.*PATH.*\[\(.*\)\]/\1/p' | tr '\n' ' ') ;;
esac
case $os in Darwin) want="_OfxGetNumberOfPlugins _OfxGetPlugin " ;; *) want="OfxGetNumberOfPlugins OfxGetPlugin " ;; esac
[ "$exports" = "$want" ] || fail "exported: '$exports', expected '$want'"
case $os in
  Darwin) [ "$rpath" = "@loader_path/../Frameworks @loader_path/../Libraries " ] || fail "rpath: '$rpath'" ;;
  Linux|FreeBSD) [ "$rpath" = '$ORIGIN/../../Libraries ' ] || fail "rpath: '$rpath'" ;;
esac
# the installed binaries are executable (Debian's CMake strips the execute bit of modules; the CI
# runners have Kitware's CMake, so this only guards against regressions)
for f in "$installed"/*.ofx.bundle/Contents/"$arch"/*.ofx; do
  [ -x "$f" ] || fail "$f is not executable"
done
# the extra rpath of the non-deployed TestPlist
tp="$installed/TestPlist.ofx.bundle/Contents/$arch/TestPlist.ofx"
case $os in
  Darwin) otool -l "$tp" | grep -q ' path /opt/ofx-test-rpath ' || fail "TestPlist: no RPATH /opt/ofx-test-rpath"
          # a non-deployed plug-in with OpenMP and an @rpath library of an SDK prefix: Makefile.master's
          # two, the SDK's lib and its explicit RPATH (no libomp directory: MacPorts libraries have
          # absolute install names)
          # (one per line: the work directory may contain spaces)
          op=$(otool -l "$installed/TestOmpNoDeploy.ofx.bundle/Contents/MacOS/TestOmpNoDeploy.ofx" | sed -n 's/^ *path \(.*\) (offset [0-9]*)$/\1/p' | LC_ALL=C sort)
          [ "$op" = "$(printf '%s\n' "$work/sdk/lib" @loader_path/../Frameworks @loader_path/../Libraries /usr/local/ofx-test-keep | LC_ALL=C sort)" ] || fail "TestOmpNoDeploy rpath: '$op'" ;;
  Linux|FreeBSD) readelf -d "$tp" | grep -q '/opt/ofx-test-rpath' || fail "TestPlist: no RPATH /opt/ofx-test-rpath" ;;
esac
if [ "$os" = Darwin ]; then
  # a bundle that isn't deployed, with resources: its signature is valid too
  codesign --verify --strict "$installed/TestPlist.ofx.bundle" || fail "codesign --verify --strict TestPlist (not deployed)"
fi
if [ "$deploy" = ON ]; then
  b="$installed/InvertBundle.ofx.bundle"
  codesign --verify --strict "$b" || fail "codesign --verify --strict $b"
  otool -L "$ofx" | grep -q '@rpath/libomp' || fail "the plug-in doesn't link the bundled libomp"
  ls "$b/Contents/Libraries/" | grep -q libomp || fail "libomp not bundled"
  ! otool -L "$ofx" "$b"/Contents/Libraries/* 2>/dev/null | grep -E '^\s+/(opt/local|usr/local|opt/homebrew)/' || fail "references outside the bundle"
fi
echo "plugin-project ($installed): OK (exports: $exports; rpath: $rpath)"
}
run package
run subdirectory
