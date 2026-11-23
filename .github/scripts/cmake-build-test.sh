#!/bin/bash
# Build everything with CMake (what the plug-in repositories use, Natron#1181), install it, run
# the hostDemo checks on the built plug-ins, and build a plug-in project against the installed
# package (cmake/tests/plugin-project). Used by the Linux and macOS jobs.
# The extensions are off, as in the Makefile build: the hostDemo checks expect one view per frame
# (with OFX_EXTENSIONS_NUKE or _VEGAS hostDemo renders two). The Windows job builds them on.
# OFX_CMAKE_EXTRA_ARGS: more (space-separated) arguments for the configure step, e.g.
# -DOFX_PYTHON_BINDINGS=ON (the Linux and macOS jobs).
set -eo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
build=$root/build-ci
# macOS has the OpenCL framework: build the OpenCL example there (Examples/OpenCL; Metal is built
# on macOS anyway). Linux would need the OpenCL headers and ICD loader packages.
opencl=OFF
if [ "$(uname -s)" = Darwin ]; then opencl=ON; fi
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLE_PLUGINS=ON -DBUILD_EXAMPLE_HOSTS=ON -DOFX_SUPPORTS_OPENCLRENDER=$opencl \
  -DOFX_EXTENSIONS_NATRON=OFF -DOFX_EXTENSIONS_NUKE=OFF -DOFX_EXTENSIONS_VEGAS=OFF \
  -DOFX_EXTENSIONS_TUTTLE=OFF -DOFX_EXTENSIONS_RESOLVE=OFF \
  -DPLUGIN_INSTALLDIR="$build/install/OFX/Plugins" ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"} \
  ${OFX_CMAKE_EXTRA_ARGS}
cmake --build "$build" --parallel
# the OpenGL DrawSuite core self-test (Support/tests/DrawSuiteOpenGLTest.cpp, a fake OpenGL table)
dst=$(find "$build" -name drawsuite-opengl-test -type f -perm -u+x | head -1)
test -n "$dst" || { echo "drawsuite-opengl-test was not built"; exit 1; }
"$dst"
# the Support library self-test that needs no host (Support/tests/SupportLibraryTest.cpp)
slt=$(find "$build" -name support-library-test -type f -perm -u+x | head -1)
test -n "$slt" || { echo "support-library-test was not built"; exit 1; }
"$slt"
# the Support self-test (Support/tests/DrawContextOpenGLTest.cpp): macOS with an offscreen CGL
# context, Linux without a context (run-time loading of libGL and of the context checks)
dct=$(find "$build" -name drawcontext-opengl-test -type f -perm -u+x | head -1)
test -n "$dct" || { echo "drawcontext-opengl-test was not built"; exit 1; }
(cd "$build" && "$dct")  # (writes ofxTestLog.txt)
cmake --install "$build" --prefix "$build/install"
# Linux: linuxCheckBundle.sh on the installed examples. They need only system libraries, so the
# full check (no system library bundled, no other library needed) applies; the symbol-version
# limits are disabled (99): the Ubuntu builder is newer than the release baseline, EL9, which is
# checked where the releases are built. Finding no bundle fails.
if [ "$(uname -s)" = Linux ]; then
  OFX_ABI_GLIBC=99 OFX_ABI_GLIBCXX=99 OFX_ABI_CXXABI=99 OFX_ABI_GCC=99 OFX_ABI_GOMP=99 \
    OFX_ABI_LIBATOMIC=99 OFX_ABI_ZLIB=99 \
    "$root/Support/include/linuxCheckBundle.sh" "$build/install/OFX/Plugins"
  # the build-tree plug-ins have the install rpath (OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH), not
  # CMake's build rpath padded with ':' (empty entries: the current directory)
  n=0
  while IFS= read -r -d '' f; do
    rp=$(readelf -d "$f" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p')
    [ -n "$rp" ] || { echo "build-tree $f: no RUNPATH"; exit 1; }
    case "$rp" in :*|*:|*::*) echo "build-tree $f: empty RUNPATH/RPATH entry: '$rp'"; exit 1 ;; esac
    i="$build/install/OFX/Plugins/${f#"$build/Plugins/"}"
    [ -f "$i" ] || { echo "build-tree $f: not installed as $i"; exit 1; }
    irp=$(readelf -d "$i" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p')
    [ "$rp" = "$irp" ] || { echo "build-tree $f: RUNPATH '$rp', installed: '$irp'"; exit 1; }
    n=$((n + 1))
  done < <(find "$build/Plugins" -path '*.ofx.bundle/Contents/Linux-*/*.ofx' -type f -print0)
  [ "$n" -gt 0 ] || { echo "no build-tree plug-in found in $build/Plugins"; exit 1; }
  echo "build-tree RUNPATH: $n plug-ins, no empty entry, same as installed"
fi
# one plug-in's install component installs that bundle only (cmake/OpenFX.cmake)
rm -rf "$build/component"
DESTDIR="$build/component" cmake --install "$build" --component example-Basic > /dev/null
bundles=$(find "$build/component" -name '*.ofx.bundle' -type d | sed 's|.*/||' | sort | tr '\n' ' ')
test "$bundles" = "example-Basic.ofx.bundle " || { echo "install --component example-Basic installed: $bundles"; exit 1; }

bin=$(dirname "$(find "$build" -name hostDemo -type f -perm -u+x | head -1)")
mkdir -p "$build/checks"
cd "$build/checks"
"$root/.github/scripts/hostdemo-checks.sh" "$bin" "$build/Plugins"
"$root/.github/scripts/cmake-plugin-project.sh" "$build/install" "$build/plugin project"  # a space: osxDeploy.sh and the helpers quote paths
"$root/.github/scripts/cmake-host-project.sh" "$build/host project"
