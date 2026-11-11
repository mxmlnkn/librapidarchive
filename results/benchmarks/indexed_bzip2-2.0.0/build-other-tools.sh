#!/usr/bin/env bash
# Build the newest available external bzip2 tools into build-other-tools-<cpu>/bin.
#
# Rerunning is cheap: existing downloads, source trees, and installed binaries are reused. Remove a
# tool's binary or source directory to force that part to be rebuilt. The 1.7.0 ibzip2 reference is
# built from a temporary worktree of this repository so that the current checkout is not touched.
set -euo pipefail

jobs=${JOBS:-$(nproc)}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cpu_model=$( ( lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1 ) || true )
if [[ -z "$cpu_model" ]]; then
    cpu_model=$(uname -m)
fi
cpu_suffix=$(printf '%s' "$cpu_model" | tr -cs 'A-Za-z0-9' '-' | sed 's/^-//;s/-$//')
cpu_suffix=${cpu_suffix%-Core-Processor}
cpu_suffix=${cpu_suffix%-Processor}
out=${BUILD_OTHER_TOOLS_DIR:-$root/build-other-tools${cpu_suffix:+-$cpu_suffix}}
if [[ -n "${SEVEN_ZIP_MARCH:-}" ]]; then
    seven_zip_march=$SEVEN_ZIP_MARCH
elif [[ "$cpu_model" == *8470* ]]; then
    # Barnard's binutils cannot assemble the AVX-512 FP16/SHA-512 code selected by -march=native.
    seven_zip_march=icelake-server
else
    seven_zip_march=native
fi
src=$out/src
bin=$out/bin
mkdir -p "$src" "$bin"
cd "$src"

download() {
    local url=$1
    local output=$2
    if [[ ! -s "$output" ]]; then
        curl -L --fail -o "$output" "$url"
    fi
}

# 7-Zip 26.03 (Ubuntu 24.04 has 23.01)
if [[ ! -x "$bin/7zz" ]]; then
    download https://www.7-zip.org/a/7z2603-src.tar.xz 7z.tar.xz
    if [[ ! -d 7z/CPP ]]; then
        mkdir -p 7z
        tar -xf 7z.tar.xz -C 7z
    fi
    # 7-Zip enables its SHA-512 target code for GCC >= 14 even when the assembler is too old for
    # the SHA-512 instructions. That code is only used by xz, not by the bzip2 decoder, so replace
    # it with the scalar stub. This keeps the 7-Zip bzip2 baseline buildable on Binutils 2.42.
    sha512_source=7z/C/Sha512Opt.c
    chmod u+w "$sha512_source" 2>/dev/null || true
    sed -i 's|^// #define Z7_USE_HW_SHA_STUB.*|#define Z7_USE_HW_SHA_STUB /* z7: disable HW SHA512 */|' \
        "$sha512_source"
    sed -i 's/^[[:space:]]*#define USE_HW_SHA[[:space:]]*$/\/\/ #define USE_HW_SHA z7: disable HW SHA512/' \
        "$sha512_source"

    # Override CFLAGS_BASE_LIST instead of CXXFLAGS: the latter would drop the makefile's -c, include,
    # and platform flags. GCC 16 also reports a false-positive -Warray-bounds in 7zDecode.cpp.
    build_7z() {
        make -C 7z/CPP/7zip/Bundles/Alone2 -f makefile.gcc -j"$jobs" \
            CFLAGS_BASE_LIST="-c -O3 -march=$1" CFLAGS_WARN="-Wno-error"
    }
    if ! build_7z "$seven_zip_march"; then
        echo "7-Zip failed with -march=$seven_zip_march; retrying with x86-64-v3" >&2
        rm -rf 7z/CPP/7zip/Bundles/Alone2/_o
        build_7z x86-64-v3
    fi
    install -m755 7z/CPP/7zip/Bundles/Alone2/_o/7zz "$bin/7zz"
fi
ln -sf 7zz "$bin/7z"

# pbzip2 1.1.13 (still the latest release)
if [[ ! -x "$bin/pbzip2" ]]; then
    download https://deb.debian.org/debian/pool/main/p/pbzip2/pbzip2_1.1.13.orig.tar.gz pbzip2.tar.gz
    if [[ ! -d pbzip2-1.1.13 ]]; then
        tar -xzf pbzip2.tar.gz
    fi
    make -C pbzip2-1.1.13 -j"$jobs" CXXFLAGS="-O3 -march=native"
    install -m755 pbzip2-1.1.13/pbzip2 "$bin/pbzip2"
fi

# lbzip2 master (2.5 release plus recent fixes). Master switched from autotools to CMake.
if [[ ! -x "$bin/lbzip2" ]]; then
    lbzip2_revision=$(git ls-remote https://github.com/kjn/lbzip2 refs/heads/master | cut -f1)
    download https://codeload.github.com/kjn/lbzip2/tar.gz/refs/heads/master lbzip2.tar.gz
    if [[ ! -d lbzip2-master ]]; then
        tar -xzf lbzip2.tar.gz
    fi
    # The CMake build hardcodes PACKAGE_VERSION to "devel". Stamp the fetched revision instead so
    # --version and the benchmark metadata identify the source tree.
    sed -i "s/PACKAGE_VERSION=\"devel\"/PACKAGE_VERSION=\"${lbzip2_revision:0:7}\"/" \
        lbzip2-master/CMakeLists.txt
    cmake -S lbzip2-master -B lbzip2-master/build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_FLAGS_RELEASE="-O3 -march=native -DNDEBUG"
    cmake --build lbzip2-master/build -j"$jobs" --target lbzip2
    install -m755 lbzip2-master/build/lbzip2 "$bin/lbzip2"
    echo "lbzip2 ${lbzip2_revision:0:7}" > "$bin/lbzip2.version"
fi

# GNU bzip2 1.0.8 (still the latest release)
if [[ ! -x "$bin/bzip2" ]]; then
    download https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz bzip2.tar.gz
    if [[ ! -d bzip2-1.0.8 ]]; then
        tar -xzf bzip2.tar.gz
    fi
    make -C bzip2-1.0.8 -j"$jobs" CFLAGS="-O3 -march=native"
    install -m755 bzip2-1.0.8/bzip2 "$bin/bzip2"
fi
ln -sf bzip2 "$bin/bunzip2"
ln -sf bzip2 "$bin/bzcat"

# libarchive / bsdcat: a common libbz2-based single-core baseline.
if command -v bsdcat >/dev/null 2>&1; then
    ln -sf "$(command -v bsdcat)" "$bin/bsdcat"
else
    echo "Skipping libarchive: install libarchive-tools for bsdcat"
fi

# seek-bzip2's micro-bunzip: the full-stream decoder from the seek-bzip2 repository. The
# seek-bunzip binary only decodes one block starting at a bit offset, so it is not a comparable
# full-file decoder. micro-bunzip reads the compressed stream from stdin and writes stdout.
if [[ ! -x "$bin/micro-bunzip" || ! -s "$bin/micro-bunzip.version" ]]; then
    seek_bzip2_revision=$(git ls-remote https://github.com/galaxyproject/seek-bzip2 refs/heads/main | cut -f1)
    download https://codeload.github.com/galaxyproject/seek-bzip2/tar.gz/refs/heads/main seek-bzip2.tar.gz
    if [[ ! -d seek-bzip2-main ]]; then
        tar -xzf seek-bzip2.tar.gz
    fi
    make -C seek-bzip2-main -j"$jobs" micro-bunzip CC="gcc -m64" CFLAGS="-O3 -march=native"
    install -m755 seek-bzip2-main/micro-bunzip "$bin/micro-bunzip"
    echo "seek-bzip2 ${seek_bzip2_revision:0:7}" > "$bin/micro-bunzip.version"
fi

# STREAM: a canonical, short memory-bandwidth probe for interpreting the decode rates. This is
# metadata only; a failure must not prevent building the actual comparison tools.
if [[ "${BUILD_STREAM:-1}" == 1 && ! -x "$bin/stream" ]]; then
    if download https://www.cs.virginia.edu/stream/FTP/Code/stream.c stream.c; then
        stream_array_size=${STREAM_ARRAY_SIZE:-80000000}
        if ! ${CC:-gcc} -O3 -march=native -fopenmp -DSTREAM_ARRAY_SIZE="$stream_array_size" \
                -o "$bin/stream" stream.c; then
            echo "Skipping STREAM: compilation failed" >&2
        fi
    else
        echo "Skipping STREAM: download failed" >&2
    fi
fi

# Rust CLIs: bzip2-cli is the drop-in CLI around libbz2-rs-sys; pbz2 is a separate
# parallel implementation with its own pbz2-core engine. lbzip2-rs is Ignalina's pure-Rust
# decoder (the installed binary is lbunzip2), not the C lbzip2 built above. fbz is
# AnswerDotAI's multi-format CLI. crabz2 has no installed CLI, so helpers/crabz2-bench
# exposes its parallel whole-buffer API; mibz2's PyPI wheel lags crates.io, so
# helpers/mibz2-bench wraps the audited 0.3.1 streaming API.
if command -v cargo >/dev/null 2>&1; then
    if [[ ! -x "$bin/bzip2-rs-cli" ]]; then
        CARGO_TARGET_DIR="$out/cargo-target" RUSTFLAGS="-C target-cpu=native" \
            cargo install --root "$out/cargo" bzip2-cli
        install -m755 "$out/cargo/bin/bzip2" "$bin/bzip2-rs-cli"
    fi
    if [[ ! -x "$bin/pbz2" ]]; then
        CARGO_TARGET_DIR="$out/cargo-target" RUSTFLAGS="-C target-cpu=native" \
            cargo install --root "$out/cargo" pbz2
        install -m755 "$out/cargo/bin/pbz2" "$bin/pbz2"
    fi
    if [[ ! -x "$bin/lbzip2-rs" || ! -s "$bin/lbzip2-rs.version" ]]; then
        CARGO_TARGET_DIR="$out/cargo-target" RUSTFLAGS="-C target-cpu=native" \
            cargo install --root "$out/cargo" lbzip2@0.5.13
        install -m755 "$out/cargo/bin/lbunzip2" "$bin/lbzip2-rs"
        echo "lbzip2-rs 0.5.13" > "$bin/lbzip2-rs.version"
    fi
    if [[ ! -x "$bin/fbz" ]]; then
        CARGO_TARGET_DIR="$out/cargo-target" RUSTFLAGS="-C target-cpu=native" \
            cargo install --root "$out/cargo" fbz@0.1.15
        install -m755 "$out/cargo/bin/fbz" "$bin/fbz"
    fi
    if [[ ! -x "$bin/crabz2" || ! -s "$bin/crabz2.version" ]]; then
        CARGO_TARGET_DIR="$out/cargo-target" RUSTFLAGS="-C target-cpu=native" \
            cargo install --locked --root "$out/cargo" \
            --path "$root/results/benchmarks/indexed_bzip2-2.0.0/helpers/crabz2-bench"
        install -m755 "$out/cargo/bin/crabz2" "$bin/crabz2"
        echo "crabz2 0.7.0" > "$bin/crabz2.version"
    fi
    if [[ ! -x "$bin/mibz2" || ! -s "$bin/mibz2.version" ]]; then
        CARGO_TARGET_DIR="$out/cargo-target" RUSTFLAGS="-C target-cpu=native" \
            cargo install --locked --root "$out/cargo" \
            --path "$root/results/benchmarks/indexed_bzip2-2.0.0/helpers/mibz2-bench"
        install -m755 "$out/cargo/bin/mibz2" "$bin/mibz2"
        echo "mibz2 0.3.1" > "$bin/mibz2.version"
    fi
else
    echo "Skipping Rust CLIs: cargo not found" >&2
fi

# librapidarchive itself: build ibzip2 and rapidgzip and install the Python module from the current
# checkout. If this script is not inside a librapidarchive checkout, clone the repository and check out
# LIBRAPIDARCHIVE_COMMIT (default: origin/master). ISA-L is enabled for rapidgzip's gzip decoder;
# ibzip2 does not link it.
if [[ -f "$root/python/indexed_bzip2/pyproject.toml" ]] && \
   git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    librapidarchive_source=$root
else
    librapidarchive_source=$out/src/librapidarchive
    if [[ ! -d "$librapidarchive_source/.git" ]]; then
        git clone --recursive https://github.com/mxmlnkn/librapidarchive.git "$librapidarchive_source"
    fi
    git -C "$librapidarchive_source" fetch --all --tags
    librapidarchive_ref=${LIBRAPIDARCHIVE_COMMIT:-origin/master}
    if [[ "$librapidarchive_ref" == "master" || "$librapidarchive_ref" == "origin/master" ]]; then
        git -C "$librapidarchive_source" checkout -B master origin/master
    else
        git -C "$librapidarchive_source" checkout "$librapidarchive_ref"
    fi
fi
git -C "$librapidarchive_source" submodule update --init --recursive

librapidarchive_commit=$(git -C "$librapidarchive_source" rev-parse HEAD)
librapidarchive_dirty=$(
    git -C "$librapidarchive_source" status --porcelain --untracked-files=no | sha256sum | cut -c1-16
)
librapidarchive_isal=${LIBRAPIDARCHIVE_WITH_ISAL:-ON}
if [[ "$librapidarchive_isal" == ON ]] && ! command -v nasm >/dev/null 2>&1; then
    echo "NASM not found; building librapidarchive without ISA-L" >&2
    librapidarchive_isal=OFF
fi
librapidarchive_stamp="$librapidarchive_commit-$librapidarchive_dirty-isal-$librapidarchive_isal"

librapidarchive_build=$out/librapidarchive-build
if [[ ! -x "$bin/ibzip2" || ! -x "$bin/rapidgzip" ||
      "$(cat "$bin/ibzip2.commit" 2>/dev/null || true)" != "$librapidarchive_stamp" ]]; then
    cmake -S "$librapidarchive_source" -B "$librapidarchive_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBRAPIDARCHIVE_BUILD_TESTS=OFF \
        -DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=OFF \
        -DLIBRAPIDARCHIVE_BUILD_TOOLS=ON \
        -DLIBRAPIDARCHIVE_WITH_ISAL="$librapidarchive_isal" \
        -DLIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION=ON \
        -DLIBRAPIDARCHIVE_ENABLE_PIE="${LIBRAPIDARCHIVE_ENABLE_PIE:-ON}" \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"
    cmake --build "$librapidarchive_build" --target ibzip2 rapidgzip -j"$jobs"
    install -m755 "$librapidarchive_build/src/tools/ibzip2" "$bin/ibzip2"
    install -m755 "$librapidarchive_build/src/tools/rapidgzip" "$bin/rapidgzip"
    echo "$librapidarchive_stamp" > "$bin/ibzip2.commit"
    echo "$librapidarchive_stamp" > "$bin/rapidgzip.commit"
fi

librapidarchive_python_stamp=$out/python-indexed_bzip2.commit
if [[ "$(cat "$librapidarchive_python_stamp" 2>/dev/null || true)" != "$librapidarchive_stamp" ]]; then
    if python3 -c "import Cython" >/dev/null 2>&1; then
        python3 -m pip install --force-reinstall --no-build-isolation \
            "$librapidarchive_source/python/indexed_bzip2"
    else
        python3 -m pip install --force-reinstall "$librapidarchive_source/python/indexed_bzip2"
    fi
    echo "$librapidarchive_stamp" > "$librapidarchive_python_stamp"
fi

# ibzip2 1.7.0: build the old tag as a separate reference binary. The worktree is removed afterwards
# so that checking out the tag does not disturb the current checkout or its submodules.
old_tag=v1.7.0
if git -C "$librapidarchive_source" rev-parse --verify --quiet "${old_tag}^{commit}" >/dev/null; then
    old_commit=$(git -C "$librapidarchive_source" rev-parse "${old_tag}^{commit}")
    if [[ ! -x "$bin/ibzip2-1.7.0" || "$(cat "$bin/ibzip2-1.7.0.commit" 2>/dev/null || true)" != "$old_commit" ]]; then
        old_worktree=$src/librapidarchive-1.7.0
        old_build=$out/librapidarchive-build-1.7.0
        remove_old_worktree() {
            if [[ -e "$old_worktree" ]]; then
                if ! git -C "$librapidarchive_source" worktree remove --force "$old_worktree" >/dev/null 2>&1; then
                    rm -rf "$old_worktree"
                    git -C "$librapidarchive_source" worktree prune
                fi
            fi
        }
        cleanup_old_worktree() {
            if [[ -n "${old_worktree:-}" ]]; then
                remove_old_worktree
            fi
        }
        trap cleanup_old_worktree EXIT
        remove_old_worktree
        git -C "$librapidarchive_source" worktree add --detach "$old_worktree" "$old_tag"
        git -C "$old_worktree" submodule update --init --recursive
        if [[ "${LIBRAPIDARCHIVE_ENABLE_PIE:-ON}" == OFF ]]; then
            # v1.7.0 has no PIE option and hardcodes -pie, which fails on the Romeo toolchain.
            sed -i 's/-fpie;//' "$old_worktree/CMakeLists.txt"
            sed -i 's/-Wl,-pie,/-Wl,/' "$old_worktree/CMakeLists.txt"
        fi
        cmake -S "$old_worktree" -B "$old_build" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DWITH_ISAL=OFF \
            -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native" \
            -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"
        cmake --build "$old_build" --target ibzip2 -j"$jobs"
        install -m755 "$old_build/src/tools/ibzip2" "$bin/ibzip2-1.7.0"
        echo "$old_commit" > "$bin/ibzip2-1.7.0.commit"
        cleanup_old_worktree
        old_worktree=
        trap - EXIT
    fi
else
    echo "Skipping ibzip2 1.7.0: tag ${old_tag} not found (run git fetch --tags)"
fi

echo "Built: $(find "$bin" -maxdepth 1 -type f -printf '%f ')"
echo "Existing tools are reused. Remove $bin/<tool> or its source directory to rebuild."
echo "Use: PATH=$bin:\$PATH python results/benchmarks/indexed_bzip2-2.0.0/benchmarkIndexedBzip2.py <file> \\"
echo "       --ibzip2 $bin/ibzip2 --tool-suffix=-native \\"
echo "       --suffix-tools 'GNU bzip2' pbzip2 lbzip2 lbzip2-rs pbz2 fbz mibz2 crabz2 7z"
