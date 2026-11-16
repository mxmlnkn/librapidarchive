# gzip full-stream results and the 8x ceiling

Date: 2026-11-05. Branch: `perf/gzip-stream8x` @ `90695804`, based on
`perf/gzip-inflate8x` @ `4d8f4b81`. Merged into `integration/perf-final` at `36bdc287`, but not into
`master`; the local branch is still available. The detailed optimization ledger is `OPTIMIZATIONS.md`
on the gzip branch; this report keeps only the full-stream evidence and the ceiling analysis.

## Commits

- `a97fa88b` [performance] Skip the chunk pipeline for single-worker CLI decode.
- `91147de6` [performance] Start SIMD literal runs after a single literal.
- `ad8b2a29` [performance] Store the SIMD literal prefix with one fixed-size store.
- `90695804` [doc] Record the full-stream results and the 8x ceiling.

The first commit is a CLI scheduling change for the custom decoder only. The next two are small
SIMD-loop changes. The last commit is documentation.

## Method and corpora

Times are CLI wall clock, `rapidgzip -d -c -P N --verify FILE > /dev/null`, medians of 5-6
alternating pairs. P1 is pinned to CPU 13; P8 uses CPUs 1-8. The host was shared (load 4-7), so
paired ratios are the result and absolute MB/s is context. Byte rates use `1e6` bytes.

| corpus | compressed | decoded | content |
|---|---:|---:|---|
| `text4.gz` | 122.5 MB | 537.0 MB | four concatenated `text.gz` members |
| `base64x4.gz` | 413.8 MB | 543.9 MB | four concatenated `base64.gz` members; 98.8% literals |
| `synth-uniform-huff-x8.gz` | 404.3 MB | 536.9 MB | uniform 64-symbol base64, `Z_HUFFMAN_ONLY` |
| `base64-huff-x4.gz` | 411.4 MB | 543.9 MB | decoded base64 recompressed with `Z_HUFFMAN_ONLY` |

The large corpora are under `tmp/gzip-corpus-large/` and are not committed.

Builds: `old` is `1f506834` (last pre-optimization gzip branch, ISA-L off), `new-custom` is the
branch tip with ISA-L off, and `new-isal` is the branch tip with ISA-L on. All are Release with
zlib-ng.

## P1 full-stream results

| corpus | build | median | MB/s | paired ratio vs old |
|---|---|---:|---:|---:|
| text | old | 1.256 s | 427 | - |
| text | new-custom | 1.043 s | 515 | 1.17x |
| text | new-isal | 0.654 s | 821 | 2.47x |
| base64 | old | 3.982 s | 137 | - |
| base64 | new-custom | 1.732 s | 314 | 2.28x |
| base64 | new-isal | 1.004 s | 542 | 3.51x |
| synth-uniform-huff | old | 4.019 s | 134 | - |
| synth-uniform-huff | new-custom | 0.605 s | 888 | 6.74x (6.23-7.14) |
| base64-huff | old | 4.073 s | 134 | - |
| base64-huff | new-custom | 0.727 s | 748 | 5.36x (4.93-5.66) |

With `--no-verify` (no CRC32, ISIZE still checked) the two literal-only ratios rise to 7.48x and
6.04x; one pair of the uniform stream crossed 8.05x. The CRC32 pass and the shared-machine load are
the difference between the 6-7x and 8x range.

## P8 full-stream results

`taskset -c 1-8`, `-P 8`, five alternating pairs:

| corpus | old | new-isal | ratio |
|---|---:|---:|---:|
| text | 0.362 s (1.48 GB/s) | 0.194 s (2.77 GB/s) | 1.90x |
| base64 | 0.774 s (703 MB/s) | 0.312 s (1.75 GB/s) | 2.37x |

The ISA-L build keeps the chunk pipeline. The custom decoder wins only on literal-only streams;
on the real corpora ISA-L is 1.83x faster on text and 1.63x on base64. A runtime custom/ISA-L
selection was therefore not added.

## Why real base64 is not at the literal-only speed

`base64.gz` is literal-dominated but not literal-only. Its 65-symbol literal alphabet plus EOB
cannot be represented by 62 six-bit codes alone: the block tables use 62 codes at length 6, 3 at
length 7, 1 at length 8, and 2 at length 9. The SIMD group decoder extracts eight equal-length
fields, so any non-dominant symbol rejects the group.

Instrumented counters on the 544 MB stream measured 53% rejected groups and **6.4%** of literals
decoded scalar (99.75% of the exceptions are seven-bit codes). The literal-only recompression
measured 19% rejected groups and 2.7% scalar literals. An earlier draft said 16% scalar literals;
that was wrong. The instrumented value is 6.4%.

The rejection rate is real, but it is not the dominant gap. Holding the data and match rate fixed
while varying the exception rate gives a 2x2 split: removing the matches is **2.20x**, while
reducing the literal exceptions from 6.4% to about 1.5% is **1.20x**. Backreferences, not mixed code
lengths, dominate the real-base64 ceiling.

Mixed-length literal decoding is worth at most about 15-20%, not the 2.9x needed to reach
1.1 GB/s. The AVX-512 mixed group on `perf/gzip-mixed` absorbs 48% of the rejected groups and
gives about 2% (`base64x4` 1.019, `base64-huff` 1.033). See `gzip-mixed-length-report.md`.

The reference decoders confirm the ceiling: on the same corpus, in-memory one-shot decode is
366 MB/s (libdeflate), 370 MB/s (ISA-L), and 343 MB/s (zlib-ng). Those are close to the current
full-stream number and below the literal-only custom path.

## minigzip build artifact

The earlier reference comparison showed `zlib-ng minigzip` far ahead on text (1171 MB/s versus
317 MB/s). That was a build-configuration artifact: the rapidgzip number was the ISA-L-off custom
build, not the default build. On the same 537/544 MB corpora, six alternating pairs:

| corpus | zlib-ng minigzip | rapidgzip -P 1 ISA-L | ratio |
|---|---:|---:|---:|
| text | 0.480 s (1.12 GB/s) | 0.504 s (1.07 GB/s) | 0.95 |
| base64 | 1.179 s (461 MB/s) | 1.084 s (503 MB/s) | 1.06 |

The default build is at parity with `minigzip` on text and slightly ahead on base64. There is no
separate output-buffer or ISIZE trick left to port.

## The 8x target

The honest old baseline on these corpora is `1f506834`: 427 MB/s on text and 137 MB/s on base64.
Eight times that is 3.4 GB/s and 1.1 GB/s. The best reference decoders reach 917/366 MB/s
(libdeflate), 837/370 (ISA-L), and 838/343 (zlib-ng) on the same streams, so 8x is above the
reference ceiling by 3.7x on text and 3x on base64.

The 8x target is reachable only on the literal-only streams the microbenchmark uses:

- old decoder: 134 MB/s on both literal-only corpora;
- no-window microbenchmark: 1.25-1.42 GB/s on a quiet machine (9.3-10.6x);
- full-stream CLI: 888/748 MB/s (6.7x/5.4x) with CRC under a shared load.

The remaining gap to 8x on those streams is the CRC pass and machine load, not the pipeline. On
the real corpora, the remaining lever is the backreference decode/copy path, not mixed-length
literal decoding. The mixed-length work is a measured ~2% gain with a ~15-20% ceiling; see
`gzip-mixed-length-report.md`.

## Tests

Release build: `testGzip` 21717652/21717652, `testGzipChunk` 1194/1194,
`testParallelGzipReader` pass, `testHuffmanCoding` 433325/433325, `testCLI` 962/962 on the `-P 1`
combinations. ASAN+UBSAN (Sanitize, ISA-L off) passes all four gzip tests with the project's
sanitizer environment. CLI output matches `gzip -dc` on text, base64, random/stored, zeros, and
concatenated members; CRC catches a flipped byte; `--count`, `--count-lines`, `-t`,
`--export-index`, and `--ranges` fall back correctly.
