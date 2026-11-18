#!/bin/bash
# Build everything with CMake (what the plug-in repositories use, Natron#1181), install it, run
# the hostDemo checks on the built plug-ins, and build a plug-in project against the installed
# package (cmake/tests/plugin-project). Used by the Linux and macOS jobs.
# The extensions are off, as in the Makefile build: the hostDemo checks expect one view per frame
# (with OFX_EXTENSIONS_NUKE or _VEGAS hostDemo renders two). The Windows job builds them on.
set -eo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
build=$root/build-ci
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLE_PLUGINS=ON -DBUILD_EXAMPLE_HOSTS=ON \
  -DOFX_EXTENSIONS_NATRON=OFF -DOFX_EXTENSIONS_NUKE=OFF -DOFX_EXTENSIONS_VEGAS=OFF \
  -DOFX_EXTENSIONS_TUTTLE=OFF -DOFX_EXTENSIONS_RESOLVE=OFF \
  -DPLUGIN_INSTALLDIR="$build/install/OFX/Plugins" ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
cmake --build "$build" --parallel
cmake --install "$build" --prefix "$build/install"
# Linux: linuxCheckBundle.sh on the installed examples. They need only system libraries, so the
# full check (no system library bundled, no other library needed) applies; the symbol-version
# limits are disabled (99): the Ubuntu builder is newer than the release baseline, EL9, which is
# checked where the releases are built. Finding no bundle fails.
if [ "$(uname -s)" = Linux ]; then
  OFX_ABI_GLIBC=99 OFX_ABI_GLIBCXX=99 OFX_ABI_CXXABI=99 OFX_ABI_GCC=99 OFX_ABI_GOMP=99 \
    OFX_ABI_LIBATOMIC=99 OFX_ABI_ZLIB=99 \
    "$root/Support/include/linuxCheckBundle.sh" "$build/install/OFX/Plugins"
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
