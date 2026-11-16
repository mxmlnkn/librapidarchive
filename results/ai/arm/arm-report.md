# aarch64 / Raspberry Pi 5 performance report

Status note: measurements were taken on the Pi at `perf/arm-neon` @ `0c1616aa`, based on
`integration/perf-final` @ `ebb76b8a`. The final integration tip is `5ad0061d`; it includes this
branch. `master` now contains the NEON bzip2 MTF/RLE change (`49a659b9`), but not the NEON gzip
literal decoder. Paths beginning with `tmp/` or `worktrees/` are local investigation paths and are
not part of the repository.

## 1. System

| Item | Value |
|---|---|
| Host | Raspberry Pi 5, aarch64 |
| CPU | 4 x Cortex-A76 (CPU part 0xd0b), 1 thread/core, 2.4 GHz max |
| ISA features | fp, asimd (NEON), aes, pmull, sha1, sha2, crc32, asimddp, asimdrdm; no SVE |
| RAM | 7.7 GiB usable, 1 GiB swap |
| Disk | 917 GB NVMe, 855 GB free at the end |
| OS | Ubuntu 26.04.1 LTS |
| Kernel | 7.0.0-1020-raspi |
| Compiler | gcc/g++ 15.2.0 (aarch64-linux-gnu) |
| CMake / Ninja / Git | 4.2.3 / 1.13.2 / 2.53.0 |
| Governor / temperature | `ondemand`, 1.5-2.4 GHz observed, 49-56 C during runs |

`perf` is **not** available: it is not installed, `perf_event_paranoid=4`, and there is no
passwordless sudo. `valgrind` and `ccache` are also absent. Measurements therefore use the
benchmark binaries' own timers and the `ibzip2 -vv` phase timers. The governor makes absolute
MB/s vary by up to ~30% between runs, so all A/B results below are medians of alternating
pairs after a warm-up spin; ratios are more trustworthy than absolute values.

## 2. Repository setup

The ARM branches were transferred with `git bundle` and cloned into
`$HOME/librapidarchive-arm` on the Pi. `origin/master` is `4f7966e5`; the tested branches
are `perf/bzip2-final` (`d18fb4d3`), `perf/gzip-inflate8x` (`4d8f4b81`; the final focused tip
is `f2b1fcf9`, which only adds the aarch64 test guard), and `integration/perf-final`
(`ebb76b8a`; the final integration tip is `5ad0061d`). Submodules (zlib-ng, zlib, rpmalloc, cxxopts, ISA-L)
were fetched from GitHub; the Pi reaches GitHub without problems.

Build configuration used for all numbers:

```bash
cd "$HOME/librapidarchive-arm"
cmake -S . -B build-<name> -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBRAPIDARCHIVE_WITH_ISAL=OFF \
  -DLIBRAPIDARCHIVE_BUILD_TESTS=ON \
  -DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON \
  -DLIBRAPIDARCHIVE_BUILD_TOOLS=ON
TMPDIR=$HOME/tmp cmake --build build-<name> -j4 --target \
  testGzip testGzipChunk testParallelGzipReader testHuffmanCoding \
  testBZ2Reader testParallelBZ2Reader \
  benchmarkGzipDecode benchmarkCustomInflate benchmarkBzip2 benchmarkInflate \
  rapidgzip ibzip2
```

zlib-ng is ON and rpmalloc is ON; ISA-L is OFF. The project already enables IPO/LTO in Release
and uses `-O3` by default, so no build-flag change was needed.

Corpora, generated on the Pi under `$HOME/tmp/corpus`:

| Corpus | Decoded | gzip -9 | bzip2 -9 |
|---|---|---|---|
| `base64` (32 MiB `/dev/urandom` piped through `base64`) | 45,327,919 B | 34,446,469 B | 34,374,837 B |
| `text` (`/usr/share/doc` + Python stdlib) | 67,242,806 B | 19,548,348 B | 16,587,995 B |

## 3. Build and test results

All branches built on aarch64 with the configuration above, except for one test source:

* `perf/gzip-inflate8x` and `integration/perf-final` **do not compile** `testHuffmanCoding` on
  aarch64. The test calls `detail::decodeEightLiteralsAvx2/Avx512` unconditionally, but those
  helpers are declared only when `RAPIDGZIP_HAS_AVX2/AVX512_LITERAL_DECODE` is 1. This is an
  architecture-specific build break, not a runtime failure. It is fixed on `perf/arm-neon` by
  commit `7e1ea796`, which is also the only change in `perf/gzip-inflate8x` @ `f2b1fcf9`.

Test results (all on the Pi, exit code 0, no `FAIL` lines):

| Test | master | bzip2-final | gzip-inflate8x | integration | perf/arm-neon |
|---|---|---|---|---|---|
| `testGzip` | 21717652/21717652 | pass | pass | pass | 21717652/21717652 |
| `testGzipChunk` | 1194/1194 | pass | pass | pass | 1194/1194 |
| `testParallelGzipReader` | 23893/23893 | - | pass | 23893/23893 | 23893/23893 |
| `testHuffmanCoding` | 3712/3712 | pass | **build failure** | **build failure** | 415133/415133 |
| `testBZ2Reader` | 215/215 | 215/215 | pass | 215/215 | 215/215 |
| `testParallelBZ2Reader` | 232/232 | 232/232 | pass | 232/232 | 232/232 |

The initial `testParallelGzipReader` run failed only because the Python modules `pgzip` and
`indexed_gzip` were missing; `python3 -m pip install --user pgzip indexed_gzip` fixed it.
The higher `testHuffmanCoding` count on `perf/arm-neon` is the NEON helper test plus the
literal-run tests, which now run because a SIMD path is active on ARM.

Decoded output was additionally verified by `md5sum` against the integration binary for both
corpora and both codecs; all match.

## 4. Baseline vs. optimized numbers

MB/s on decoded data. `master` = `origin/master`; `integration` = `integration/perf-final`
(bzip2 + gzip branch work); `arm-neon` = `integration` + this work. Pn is the number of decoder
workers; `benchmarkBzip2 --mode discard`, `benchmarkGzipDecode` with CRC on, chunk size 4 MiB.

### bzip2

| Corpus | Workers | master | integration | arm-neon | arm-neon / integration |
|---|---|---|---|---|---|
| base64 | P1 | 12.6 | 25.1 | **36.2** | 1.44x |
| base64 | P3 | - | 57.3 | **67.0** | 1.17x |
| base64 | P4 | 27.9 | 62.5 | 61.0 | ~0.98x |
| text | P1 | 27.4 | 37.9 | **42.0** | 1.11x |
| text | P3 | - | 66.8 | **68.4** | 1.02x |
| text | P4 | 51.0 | 62.0 | 61.0 | ~0.98x |

P3 is the best worker count on a 4-core Pi: the consumer thread is an extra runnable thread, so
P4 is five threads on four cores and is consistently ~10% slower than P3. The NEON gain is
clearly visible at P1 and P3 on base64; it disappears at P4, where the pipeline is limited by
something other than the decoder (see section 6).

Sequential phase profile (`ibzip2 -vv -P 1`, base64 corpus, 52 blocks):

```
decodeBlock                   0.0427 s   (RLE expansion + CRC + output)
readBlockHeader               0.0136 s
    readSymbolMaps            0.00004 s
    readSelectors             0.0125 s
    readTrees                 0.0010 s
    createHuffmanTable        0.5926 s
    burrowsWheelerPreparation 0.6839 s
```

After the NEON MTF/RLE work, only ~3% of the sequential time is in `decodeBlock`.
`createHuffmanTable` (per-block 12-bit LUT construction for up to six groups) and
`burrowsWheelerPreparation` (inverse BWT via libsais plus preparation) now dominate. Both are
architecture-independent bottlenecks.

### gzip

| Corpus | Mode | master | integration | arm-neon |
|---|---|---|---|---|
| base64 | sequential `GzipReader` | 99.5 | 159.8 | 161.5 |
| base64 | parallel P1 (chunk pipeline) | - | 139.3 | 140.0 |
| base64 | parallel P4 | 300 | 405 | 405 |
| text | sequential `GzipReader` | 243 | 260.9 | 260.9 |
| text | parallel P4 | 500 | 520 | 525 |

The gzip branch already gives a large win over master on literal-heavy input. On these corpora
the NEON literal decoder does not fire (see below), so `arm-neon` matches `integration`.

`benchmarkInflate` (synthetic 128 MiB inputs, CRC on):

| Data type | master | integration | arm-neon |
|---|---|---|---|
| Random Base64 (Huffman Only) | 98.52 | 229.28 | **331.1** |
| Random Base64 (Fixed Huffman) | 103.50 | 102.28 | 102.5 |
| Random (stored) | 816 | 834 | ~816 |
| Random Backreferences | 677 | 681 | ~678 |
| Random Overlapping Backreferences | 815 | 787 | ~790 |

`benchmarkCustomInflate --data base64`, same binary with
`RAPIDGZIP_SIMD_LITERAL_DECODE=scalar` vs. default:

| Mode | scalar | NEON | ratio |
|---|---|---|---|
| `no-window-full` | 297.4 | 499.8 | 1.68x |
| `gzip-reader` | 295.0 | 489.7 | 1.66x |
| `window-full` | 147.5 | 247.9 | 1.68x |

`window-full` is slower in absolute terms because it uses the real 32 KiB initial window and a
different window type; the NEON ratio is the same.

### Approximate cycles/byte

`perf` is unavailable, so these are derived from MB/s at the 2.4 GHz maximum
(`2400 / MB/s`), i.e. an upper bound if a core ran below 2.4 GHz:

| Workload | MB/s | approx. cycles/B |
|---|---|---|
| bzip2 base64 P1, master | 12.6 | 190 |
| bzip2 base64 P1, arm-neon | 36.2 | 66 |
| gzip base64 sequential, integration | 159.8 | 15.0 |
| gzip base64 custom block, arm-neon | 499.8 | 4.8 |
| gzip base64 P4, integration | 405 | 5.9 (aggregate) |

## 5. Implemented changes (`perf/arm-neon`, based on `integration/perf-final`)

Branch `perf/arm-neon` is at `0c1616aa` (also on the Pi in
`$HOME/librapidarchive-arm`). It contains three commits:

1. `7e1ea796 [fix] Guard x86-only literal-decode test helpers`
   Makes `testHuffmanCoding` compile on aarch64 by guarding the AVX2/AVX-512 helper calls with
   `RAPIDGZIP_HAS_*_LITERAL_DECODE`.
2. `d084acda [performance] Use NEON for bzip2 MTF and RLE expansion`
   * NEON branchless MTF update for list positions < 64: four `vextq_u8` shifts build the
     shifted 64-byte list, a `vcgtq_u8` index mask blends old and new bytes.
   * NEON 16-byte RLE run scan: `vceqq_u8` on four overlapping loads, a nibble-narrowed
     `vshrn_n_u16` movemask, and `vst1q_u8` for all-literal blocks.
   * Measured: bzip2 base64 P1 25.1 -> 36.2 MB/s, text P1 37.9 -> 42.0 MB/s; P3 base64
     57.3 -> 67.0 MB/s. Output md5 matches.
3. `0c1616aa [performance] Decode eight literals at once with NEON`
   * 8-wide dominant-literal decoder using four `vshlq_u64`, `vuzp1q_u32`/`vuzp1q_u16`
     packing, and one `vqtbl4q_u8` 64-byte table lookup. Same 0xFF sentinel protocol and
     partial-prefix handling as AVX2.
   * Selected through `selectedLiteralSimdPath()`; `RAPIDGZIP_SIMD_LITERAL_DECODE=scalar`
     forces the fallback for A/B.
   * Measured: custom inflate base64 297 -> 500 MB/s, `benchmarkInflate` Random Base64
     227.7 -> 331.1 MB/s (1.45x). Fixed Huffman is unchanged because the literal-run batching
     gate does not fire.

## 6. aarch64 bottlenecks and negative results

* **NEON literal decoder gating.** The 8-wide path requires a dominant literal length <= 6 and
  that length covering >= 90% of the LUT. `base64`-like Huffman-only data passes; the zlib -9
  compressed base64 corpus does not, and Fixed Huffman data (lengths 8/9) does not. That is why
  the realistic `.gz` corpus shows no NEON gain even though the synthetic Huffman-only corpus
  gains 1.45-1.68x. Relaxing the gate is architecture-independent and was not attempted here;
  it would need its own text-vs-base64 A/B.
* **Parallel bzip2 scaling.** P1 -> P4 is only ~1.7x despite ~380% CPU utilization. P3 beats P4
  on this 4-core machine because the consumer is a fifth thread. `time` shows 1.26 s user at P1
  but 2.92 s user at P4 for the same file, so the parallel run does substantially more CPU work
  per byte (per-block setup/allocation and cache/memory contention). The NEON decoder win is
  therefore visible at P1/P3 but hidden at P4.
* **bzip2 per-block setup.** `createHuffmanTable` + `burrowsWheelerPreparation` are ~96% of the
  sequential time after the NEON work. These are not ARM-specific, but they cap further
  single-core gains from RLE/MTF work.
* **CRC32.** Parallel decode with CRC is only ~5% slower than `--no-crc` (base64 404 vs 429 MB/s,
  text 515 vs 553 MB/s). The project's custom CRC32 uses PCLMULQDQ on x86 and slicing-by-N
  elsewhere; zlib-ng is built without its `arch/arm` sources, so there is no free PMULL path.
  A PMULL port could recover at most ~4-5% and was not attempted.
* **`-mcpu=native`.** A separate build with `-mcpu=native` (C and C++ flags) gave +0.7% on
  custom inflate, no consistent bzip2 change, and noise-level gzip P4 changes. Not added.
* **`-O3`/LTO.** Already active in the Release build; no change needed.

## 7. I/O, thermal, and network

* GitHub was reachable; `git ls-remote` and all submodule clones succeeded.
* Temperature stayed at 49-56 C; no throttling was observed and cores reached 2.4 GHz.
  The `ondemand` governor is the main source of run-to-run variance. No sudo means it cannot
  be pinned to `performance`.
* No background processes were left on the Pi; all runs used `timeout` and foreground
  execution.

## 8. Reproduce

```bash
ssh "$PI_HOST"
cd "$HOME/librapidarchive-arm"
git checkout perf/arm-neon

cmake -S . -B build-armneon -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBRAPIDARCHIVE_WITH_ISAL=OFF \
  -DLIBRAPIDARCHIVE_BUILD_TESTS=ON \
  -DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON \
  -DLIBRAPIDARCHIVE_BUILD_TOOLS=ON
TMPDIR=$HOME/tmp cmake --build build-armneon -j4 --target \
  testGzip testGzipChunk testParallelGzipReader testHuffmanCoding \
  testBZ2Reader testParallelBZ2Reader \
  benchmarkGzipDecode benchmarkCustomInflate benchmarkBzip2 benchmarkInflate \
  rapidgzip ibzip2

# tests (run from the binary directories because the tests use relative paths)
cd build-armneon/src/tests/rapidgzip
./testGzip && ./testGzipChunk && ./testParallelGzipReader && ./testHuffmanCoding
cd ../indexed_bzip2
./testBZ2Reader && ./testParallelBZ2Reader

# corpora
mkdir -p $HOME/tmp/corpus && cd $HOME/tmp/corpus
head -c 33554432 /dev/urandom | base64 > base64.txt
python3 - <<'EOF'
import glob
parts = []
for pattern in ["/usr/lib/python3*/**/*.py", "/usr/share/doc/**/*", "/usr/share/common-licenses/*"]:
    parts += glob.glob(pattern, recursive=True)
with open("text.txt", "wb") as out:
    size = 0
    for path in parts:
        data = open(path, "rb").read()
        out.write(data)
        size += len(data)
        if size >= 64 * 1024 * 1024:
            break
EOF
gzip -9 -k -f base64.txt text.txt
bzip2 -9 -k -f base64.txt text.txt

# bzip2 A/B (P3 is fastest on 4 cores)
cd "$HOME/librapidarchive-arm"
taskset -c 0-3 build-armneon/src/benchmarks/benchmarkBzip2 \
  --iterations 5 --parallel 3 --mode discard $HOME/tmp/corpus/base64.txt.bz2

# gzip literal decoder A/B, same binary
cd build-armneon/src/benchmarks
RAPIDGZIP_SIMD_LITERAL_DECODE=scalar taskset -c 1 ./benchmarkCustomInflate \
  --data base64 --mode no-window-full --size 64Mi --iterations 5
taskset -c 1 ./benchmarkCustomInflate \
  --data base64 --mode no-window-full --size 64Mi --iterations 5

# end-to-end gzip
taskset -c 0-3 ./benchmarkGzipDecode $HOME/tmp/corpus/base64.txt.gz -p 4 -c 4M -i 5
```

## 9. Artifacts

* Branch: `perf/arm-neon` at `0c1616aa`
* Pi branch: `perf/arm-neon` in `$HOME/librapidarchive-arm` (same commits)
* Bundles: `tmp/raspi/perf.bundle`, `tmp/raspi/arm-neon.bundle`
* Patches used during development: `tmp/raspi/arm-neon-v1.patch`,
  `tmp/raspi/gzip-neon-header.patch`, `tmp/raspi/gzip-neon-test.patch`
* Raw benchmark logs remain on the Pi under `$HOME/tmp/results/`.
