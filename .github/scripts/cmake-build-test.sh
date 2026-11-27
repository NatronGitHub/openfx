#!/bin/bash
# Build everything with CMake (what the plug-in repositories use, Natron#1181), install it, run
# the hostDemo checks on the built plug-ins, and build a plug-in project against the installed
# package (cmake/tests/plugin-project). Used by the Linux and macOS jobs.
# The extensions are off, as in the Makefile build: the hostDemo checks (hostdemo-checks.sh) expect
# one view per frame (with OFX_EXTENSIONS_NUKE or _VEGAS hostDemo renders two). The extensions-on
# step of the Linux job (with the Python tests) and the Windows job build them on.
# OFX_CMAKE_EXTRA_ARGS: more (space-separated) arguments for the configure step, e.g.
# -DOFX_PYTHON_BINDINGS=ON (the Linux and macOS jobs).
set -eo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
build=$root/build-ci
# macOS has the OpenCL framework: build the OpenCL example there (Examples/OpenCL; Metal is built
# on macOS anyway). Linux would need the OpenCL headers and ICD loader packages.
# openfx::HostSupportMetal is macOS only.
opencl=OFF
metal=OFF
if [ "$(uname -s)" = Darwin ]; then opencl=ON; metal=ON; fi
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLE_PLUGINS=ON -DBUILD_EXAMPLE_HOSTS=ON -DOFX_SUPPORTS_OPENCLRENDER=$opencl \
  -DOFX_HOSTSUPPORT_OPENGL=ON -DOFX_HOSTSUPPORT_OPENCL=ON -DOFX_HOSTSUPPORT_METAL=$metal \
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
# every example plug-in binary is really unloaded by dlclose(), and on macOS has no thread-local
# variables, which dyld never unloads (Support/tests/PluginUnloadTest.cpp, #385)
put=$(find "$build" -name plugin-unload-test -type f -perm -u+x | head -1)
test -n "$put" || { echo "plugin-unload-test was not built"; exit 1; }
plugins=()
while IFS= read -r f; do plugins+=("$f"); done < <(find "$build/Plugins" -path '*.ofx.bundle/Contents/*' -name '*.ofx' -type f | sort)
test ${#plugins[@]} -gt 0 || { echo "no plug-in binary found in $build/Plugins"; exit 1; }
# and a binary that uses ThreadScope, twice: unloaded, then loaded again with new keys; built
# with hidden visibility as the plug-ins, and with default visibility
tst=$(find "$build" -name thread-scope-test.ofx -type f | head -1)
test -n "$tst" || { echo "thread-scope-test.ofx was not built"; exit 1; }
tsd=$(find "$build" -name thread-scope-test-default.ofx -type f | head -1)
test -n "$tsd" || { echo "thread-scope-test-default.ofx was not built"; exit 1; }
"$put" "${plugins[@]}" "$tst" "$tst" "$tsd" "$tsd"
# the Support self-test (Support/tests/DrawContextOpenGLTest.cpp): macOS with an offscreen CGL
# context, Linux without a context (run-time loading of libGL and of the context checks)
dct=$(find "$build" -name drawcontext-opengl-test -type f -perm -u+x | head -1)
test -n "$dct" || { echo "drawcontext-opengl-test was not built"; exit 1; }
(cd "$build" && "$dct")  # (writes ofxPluginLog.txt)
# openfx::HostSupportOpenGL (HostSupport/examples/openglRenderChecks.cpp): Linux with EGL
# (Mesa llvmpipe in CI), macOS with CGL. It skips its OpenGL checks without a context, unless
# OFXH_REQUIRE=1, which build.yml sets for all jobs: the macOS job (not required for merging)
# then needs a CGL context too (the runners have Apple's software renderer)
grc=$(find "$build" -name openglRenderChecks -type f -perm -u+x | head -1)
test -n "$grc" || { echo "openglRenderChecks was not built"; exit 1; }
(cd "$build" && OFX_PLUGIN_PATH="$build/Plugins" "$grc")
cmake --install "$build" --prefix "$build/install"
# Linux: linuxCheckBundle.sh on the installed examples. They need only system libraries, so the
# full check (no system library bundled, no other library needed) applies; the symbol-version
# limits are disabled (99): the Ubuntu builder is newer than the release baseline, EL9, which is
# checked where the releases are built. Finding no bundle fails.
if [ "$(uname -s)" = Linux ]; then
  OFX_ABI_GLIBC=99 OFX_ABI_GLIBCXX=99 OFX_ABI_CXXABI=99 OFX_ABI_GCC=99 OFX_ABI_GOMP=99 \
    OFX_ABI_LIBATOMIC=99 OFX_ABI_ZLIB=99 \
    "$root/Support/include/linuxCheckBundle.sh" "$build/install/OFX/Plugins"
  "$root/.github/scripts/check-build-rpath.sh" "$build/Plugins" "$build/install/OFX/Plugins" cmake-build-test
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
