#!/usr/bin/env bash
# Run clang-tidy through the CMake option used by CI.
#
# clang-tidy needs clang compile commands. The defaults match CI; override
# CC, CXX, or CLANG_TIDY_EXECUTABLE if clang is installed under another name.
#
# Usage:
#   bash src/tests/run-clang-tidy.sh
#
# compile_commands.json is written to the build directory for clang-tidy-diff
# or other tools.

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
build_dir="${BUILD_DIR:-${repo_root}/build-clang-tidy}"

for i in $( seq 30 -1 15 ); do
    if command -v clang-$i &>/dev/null && clang-$i --version &>/dev/null; then
        export CC="${CC:-clang-$i}"
        export CXX="${CXX:-clang++-$i}"
    fi
done

cmake_args=(
    --fresh
    -S "${repo_root}"
    -B "${build_dir}"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DLIBRAPIDARCHIVE_RUN_CLANG_TIDY=ON
    -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=ON
    -DLIBRAPIDARCHIVE_WITH_ISAL=OFF
    -DLIBRAPIDARCHIVE_BUILD_TESTS=ON
)
if [[ -n "${CLANG_TIDY_EXECUTABLE:-}" ]]; then
    cmake_args+=("-DCLANG_TIDY_EXECUTABLE=${CLANG_TIDY_EXECUTABLE}")
fi
cmake "${cmake_args[@]}"

cmake --build "${build_dir}" -j "${JOBS:-4}"
