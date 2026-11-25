#!/usr/bin/env bash

# Checks the add_subdirectory(src) consumer configuration used by SortMeRNA. The public header check
# catches missing includes and platform macros; this check also catches CMake interface definitions that
# only work when the librapidarchive root CMakeLists.txt is evaluated, for example an empty
# LIBRAPIDARCHIVE_BZIP2_CRC_SLICES definition.

set -euo pipefail

repoRoot=$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )
tempDir=$( mktemp -d )
trap 'rm -rf "$tempDir"' EXIT

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=( -G Ninja )
fi

cat > "$tempDir/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.17)
project(consumer CXX)

find_package(Threads REQUIRED)

set(LIBRAPIDARCHIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_subdirectory("$repoRoot/src" librapidarchive)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE rapidgzip::rapidgzip)
EOF

# Use the same include order as SortMeRNA's readfeed.cpp, which includes ParallelGzipReader.hpp before
# gzip/definitions.hpp. ParallelGzipReader.hpp includes core/Memory.hpp first.
cat > "$tempDir/main.cpp" <<'EOF'
#include <filereader/BufferView.hpp>
#include <filereader/Standard.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>

int main()
{
    return sizeof( rapidgzip::ParallelGzipReader<> );
}
EOF

cmake -S "$tempDir" -B "$tempDir/build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build "$tempDir/build"
