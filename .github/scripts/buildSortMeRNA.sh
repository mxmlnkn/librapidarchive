#!/usr/bin/env bash

# Builds SortMeRNA v8.0.2 against a librapidarchive checkout and decompresses one gzipped reads file.
# The pinned SortMeRNA dependencies predate GCC 14, so RocksDB needs an explicit <cstdint> include and
# the parasail tests need incompatible-pointer warnings downgraded. Only the parasail library is built
# because its bundled tests do not compile with those warnings enabled.

set -euo pipefail

repoRoot=${1:?usage: $0 LIBRAPIDARCHIVE_ROOT SORTMERNA_DIR}
sortmernaDir=${2:?usage: $0 LIBRAPIDARCHIVE_ROOT SORTMERNA_DIR}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

cd -- "$sortmernaDir"
export CMAKE_BUILD_PARALLEL_LEVEL=$jobs

# zlib and RocksDB are built by SortMeRNA's setup.py. Keep the existing build trees when the CI cache
# restored them.
if [ ! -f build/3rdparty/zlib/dist/lib/libz.a ]; then
    python3 setup.py zlib
fi

if [ ! -f build/3rdparty/rocksdb/dist/lib/librocksdb.a ]; then
    CFLAGS="-include stdint.h ${CFLAGS:-}" CXXFLAGS="-include cstdint ${CXXFLAGS:-}" \
        python3 setup.py rocksdb
fi

python3 setup.py bbhash
python3 setup.py cxxopts

parasailDist=build/3rdparty/parasail/dist
if [ ! -f "$parasailDist/lib/libparasail.a" ]; then
    if [ ! -d 3rdparty/parasail ]; then
        git clone --depth 1 https://github.com/jeffdaily/parasail.git 3rdparty/parasail
        git -C 3rdparty/parasail fetch --depth 1 origin fb985ee4f2302c72c87d7d56443712b2d920b106
        git -C 3rdparty/parasail checkout fb985ee4f2302c72c87d7d56443712b2d920b106
    fi

    cmake -S 3rdparty/parasail -B build/3rdparty/parasail -G Ninja \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build build/3rdparty/parasail --target parasail -j "$jobs"

    mkdir -p "$parasailDist/include" "$parasailDist/lib"
    cp 3rdparty/parasail/parasail.h "$parasailDist/include/"
    cp build/3rdparty/parasail/libparasail.a "$parasailDist/lib/"
fi

cmake -S . -B build-ci -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_TESTS=OFF \
    -DROCKSDB_USE_STATIC_LIBS=ON \
    -DZLIB_USE_STATIC_LIBS=ON \
    -DZLIB_ROOT="$PWD/build/3rdparty/zlib/dist" \
    -DROCKSDB_DIST="$PWD/build/3rdparty/rocksdb/dist" \
    -DPARASAIL_DIST="$PWD/$parasailDist" \
    -DINDEXED_BZIP2_HOME="$repoRoot/src"
cmake --build build-ci --target sortmerna -j "$jobs"

smokeDir=$( mktemp -d "${RUNNER_TEMP:-${TMPDIR:-$sortmernaDir}}/smr-smoke.XXXXXX" )
trap 'rm -rf "$smokeDir"' EXIT

gzip -c test_data/set4_mate_pairs_metatranscriptomics.fastq > "$smokeDir/reads.fastq.gz"
if ! build-ci/src/sortmerna/sortmerna \
    --ref test_data/silva-arc-16s-database-id95.fasta \
    --reads "$smokeDir/reads.fastq.gz" \
    --workdir "$smokeDir/work" \
    --fastx \
    --aligned "$smokeDir/aligned" \
    --other "$smokeDir/other" \
    --threads 1 > "$smokeDir/run.log" 2>&1
then
    cat "$smokeDir/run.log" >&2
    exit 1
fi

for output in aligned other; do
    if [ ! -s "$smokeDir/$output.fq.gz" ]; then
        echo "SortMeRNA did not write $output.fq.gz" >&2
        cat "$smokeDir/run.log" >&2
        ls -l "$smokeDir" >&2
        exit 1
    fi
done
