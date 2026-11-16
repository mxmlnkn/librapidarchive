# gzip mixed-length literal decoding: result and the real ceiling

Date: 2026-11-06. Branch: `perf/gzip-mixed` @ `4eb77839`, based on
`perf/gzip-stream8x` @ `90695804`. The branch was not merged into `master`. The detailed ledger is
`OPTIMIZATIONS.md` rows 50-51 on that branch; this report keeps the measured result and the corrected
interpretation.

## Commits

- `725fcacb` [performance] Absorb one seven-bit literal in the AVX-512 group.
- `4eb77839` [doc] Record the mixed-length and backreference split.

## What the mixed group does

The equal-length AVX-512 group rejects a group when it hits a non-dominant code. The mixed group
handles the common case of exactly one seven-bit literal:

1. Find the first sentinel in the group.
2. Look up that literal in a 128-entry seven-bit table.
3. Re-extract the following lanes with their one-bit shift.
4. Store all eight literals if no later lane is non-dominant.

A second exception, or a non-literal symbol, still falls back to the dominant prefix plus one scalar
step. The commit is small; it does not replace the equal-length decoder.

## Measurement

`benchmarkGzipDecode -p 1 -i 8 --discard-output`, CPU 13, 8 alternating pairs, best-of-8 per run.
The corpora are `base64x4.gz` (544 MB decoded, real mixed tree) and `base64-huff-x4.gz`
(544 MB, literal-only recompression). Both are under `tmp/gzip-corpus-large/` and are not committed.

| corpus | paired ratio | wins |
|---|---:|---:|
| `base64x4` | 1.019 | 8/8 |
| `base64-huff-x4` | 1.033 | 5/5 |

The gain is about 2%, not a step change. On `base64x4`, the mixed group absorbs 47.7M of the 99.5M
pre-change rejected groups, or 48%. Branch misses fall 166.5M -> 147.4M (-11.5%), instructions rise
24.13e9 -> 25.72e9 (+6.6%), and cycles fall 12.97e9 -> 12.79e9 (-1.3%). The extra instructions buy
fewer branch misses and fewer scalar literals; the net is the small wall-clock win.

## Corrected scalar-literal count

Instrumentation on `base64x4` before the change:

- 1.030B literals; 964M dominant, 6.4% decoded scalar.
- 99.75% of the exceptions are seven-bit codes, 0.25% eight-bit, none nine-bit.
- 188.2M SIMD groups, 99.5M rejected (52.9%), 88.7M accepted.

Earlier text said 16% of literals decoded scalar. That was wrong. The instrumented value is 6.4%;
the 16% figure should not be reused. After the change: 168.3M groups, 51.8M rejected (30.8%),
47.7M mixed successes, and 1.8% scalar literals.

## The 2x2 split: backreferences dominate

Holding the data and match rate fixed while varying the literal exception rate separates the two
costs:

| corpus | backrefs | literal exceptions | P1 | MB/s |
|---|---|---:|---:|---:|
| `synth-uniform-huff-x8` | no | ~1.4% | 0.636 s | 845 |
| `synth-uniform-def-x8` | yes | ~1-2% | 1.392 s | 386 |
| `base64-huff-x4` | no | 2.7% | 0.747 s | 728 |
| `base64x4` | yes | 6.4% | 1.666 s | 326 |

One session, medians of three best-of-4 runs per corpus.

- Removing the matches is **2.20x** in both pairs.
- Reducing the literal exceptions from 6.4% to about 1.5% is **1.20x**.

The real-base64 ceiling is therefore dominated by the backreference decode/copy path, not by mixed
code lengths. Mixed-length literal decoding can recover at most about 15-20%, not the 2.9x needed to
reach 1.1 GB/s. The reference decoders confirm the ceiling: 366 MB/s (libdeflate), 370 MB/s (ISA-L),
and 343 MB/s (zlib-ng) on the same corpus, all close to the current full-stream number.

This also corrects the earlier full-stream conclusion. The 53% group rejection rate is real, but it
is the smaller half of the gap. The next real-base64 lever is the backreference path.

## Tests

Release: `testGzip` 21717652/21717652, `testGzipChunk` 1194/1194,
`testParallelGzipReader` pass, `testHuffmanCoding` 433325/433325. ASAN+UBSAN (Sanitize, ISA-L off)
passes all four with the project's sanitizer environment. `rapidgzip -d -c -P 1` matches `gzip -dc`
on `base64.gz`, `text.gz`, and `base64x4.gz`.

## Artifacts

- State ledger: `tmp/gzip-mixed-state.md`.
- Builds: `tmp/build-gzip-mixed` (Release, ISA-L off) and `tmp/build-gzip-mixed-asan`.
- Corpora: `tmp/gzip-corpus-large/`.
