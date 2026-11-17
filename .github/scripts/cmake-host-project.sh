#!/bin/bash
# Build cmake/tests/host-project: a host using openfx's HostSupport with add_subdirectory(openfx),
# with the system expat, as Natron's CMake build does; and configure it with
# OFX_SUPPORTS_PARAMETRIC (the host provides ofxhParametricParam.h, so no build).
# usage: cmake-host-project.sh <work dir>
set -eo pipefail
work=$1
src="$(cd "$(dirname "$0")/../.." && pwd)"
rm -rf "$work"
cmake -S "$src/cmake/tests/host-project" -B "$work/build" -DOFX_SOURCE_DIR="$src" ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"}
cmake --build "$work/build" --parallel
cmake -S "$src/cmake/tests/host-project" -B "$work/parametric" -DOFX_SOURCE_DIR="$src" -DOFX_TEST_PARAMETRIC=ON ${CMAKE_GENERATOR:+-G "$CMAKE_GENERATOR"} > "$work/parametric.log"
echo "host-project: OK"
