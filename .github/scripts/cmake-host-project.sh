#!/bin/bash
# Build cmake/tests/host-project: a host using openfx's HostSupport with add_subdirectory(openfx),
# with the system expat, as Natron's CMake build does; then again with OFX_SUPPORTS_PARAMETRIC
# and the host's (stub) ofxhParametricParam.cpp added to OfxHost.
# usage: cmake-host-project.sh <work dir>
set -eo pipefail
work=${1:?usage: cmake-host-project.sh <work dir>}
src="$(cd "$(dirname "$0")/../.." && pwd)"
rm -rf "$work"
for v in OFF ON; do
  cmake -S "$src/cmake/tests/host-project" -B "$work/parametric-$v" -DOFX_SOURCE_DIR="$src" \
    -DOFX_TEST_PARAMETRIC=$v ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
  cmake --build "$work/parametric-$v" --parallel ${CMAKE_BUILD_PARALLEL_LEVEL}
done
echo "host-project: OK"
