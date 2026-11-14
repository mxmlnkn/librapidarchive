# `[performance] BitStringFinder: Add word and SIMD fast path` commit

## Change

`BitStringFinder::findBitStrings` used to run eight byte-aligned `std::string_view::find` passes, one
per bit shift. The new path scans the buffer once, filters on the first byte of the byte-aligned middle
of the pattern for each shift, and verifies a candidate with one unaligned 64-bit load. Portable, SSE2,
AVX2, and AVX-512 kernels are compiled with function target attributes and selected at runtime.

The portable kernel is slower than the eight-pass reference on hosts without x86 SIMD, so automatic
dispatch keeps the reference path unless an x86 SIMD kernel is available. It can be forced for A/B
runs and tests with `RAPIDGZIP_BIT_STRING_FINDER_SIMD=portable|sse2|avx2|avx512` when the build is
configured with `-DLIBRAPIDARCHIVE_ENABLE_KERNEL_OVERRIDE=ON`. The option is OFF by default and the
production build uses automatic dispatch only.

## Method

- `src/benchmarks/benchmarkBitStringFinder.cpp`, built with `-DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON`.
- Build: `RelWithDebInfo`, GCC 16.2.0, ISA-L off, rpmalloc off, system zlib, no LTO changes.
- Hardware: AMD Ryzen 7 PRO 8840U (Zen 4, AVX-512BW), 16 visible CPUs, shared host.
- Pinned with `taskset -c 14`. Base and change binaries were rebuilt from the same tree with only
  `src/core/BitStringFinder.hpp` switched. Runs alternate base/change; the first pair is discarded.
- The host was shared with other agents (load average 2-5), so the tables below show ranges and two
  sessions. The combined rows merge all valid pairs.
- Inputs: 1 MiB and 64 MiB of `/dev/urandom`; a real `base64-256KiB.bz2` was used for offset
  validation.

The harness reports the average of four timed iterations and the maximum bandwidth. The tables use the
average unless marked "max bandwidth".

## Isolated finder, 1 MiB

| session | pairs | base (GB/s) | change (GB/s) | speedup |
|---|---:|---:|---:|---:|
| A | 6 | 1.470 | 3.310 | 2.095x |
| B (after final tweak) | 4 | 1.845 | 3.355 | 1.781x |
| combined | 10 | 1.635 (ratio of medians) | 3.355 | 1.97x median per pair |

- Combined average bandwidth: base 1.64 GB/s, change 3.36 GB/s, about **2.05x** by ratio of medians.
- Combined per-pair speedup: median **1.97x**, range **1.68-2.55x**.
- Max-bandwidth pairs: median **2.07x**, range **1.76-2.69x**.

The 1 MiB buffer is cache-resident, so this is the cleanest measure of the finder itself.

## Isolated finder, 64 MiB

| session | pairs | base (GB/s) | change (GB/s) | speedup |
|---|---:|---:|---:|---:|
| A | 6 | 1.705 | 2.500 | 1.496x |
| B | 4 | 1.730 | 2.715 | 1.569x |
| combined | 10 | 1.730 (ratio of medians) | 2.695 | 1.54x median per pair |

- Combined average bandwidth: base 1.73 GB/s, change 2.70 GB/s, about **1.56x** by ratio of medians.
- Combined per-pair speedup: median **1.54x**, range **0.98-2.03x**.
- Max-bandwidth pairs: median **1.52x**, range **0.98-1.80x**.

One pair measured slightly below 1x while another agent was active. The 64 MiB case is closer to the
memory system and is more sensitive to host noise than the 1 MiB case.

## Kernel scaling, 1 MiB, 5 runs each

| kernel | average median (GB/s) | range (GB/s) | relative to portable |
|---|---:|---:|---:|
| portable | 1.52 | 1.41-1.59 | 1.00x |
| SSE2 | 2.11 | 1.94-2.17 | 1.39x |
| AVX2 | 2.67 | 2.30-2.91 | 1.75x |
| AVX-512 | 3.22 | 3.02-3.28 | 2.12x |

The reference measured 1.6-1.9 GB/s across the sessions. AVX2, the relevant path on Rome (Zen 2), is
about 1.4-1.6x the reference; AVX-512 is about 1.7-2.0x. The portable kernel is slower than the
reference, which is why automatic dispatch falls back to the reference without x86 SIMD.

## End-to-end `ibzip2`

Input: 85 MB `mixed-128MiB.bin.bz2` (128 MiB uncompressed). Base and change output md5 were identical.
Timing is wall clock to `/dev/null`, alternating runs, first pair discarded.

| config | base median | change median | speedup |
|---|---:|---:|---:|
| `-P 1` | 0.785 s | 0.814 s | 0.99x (range 0.84-1.61x) |
| `-P 8` | 0.340 s | 0.344 s | 0.99x (range 0.98-1.19x) |
| `-P 16 -p 8` | 0.322 s | 0.314 s | 1.01x (range 0.96-1.06x) |

End-to-end is neutral within noise on this host and data. The block finder is prefetched while the
decoder runs, and decompression dominates at these thread counts. The isolated finder is about 2x
faster, which is the useful result for the high-thread-count case where the finder becomes a
co-limiter.

## Correctness evidence

- `testBitStringFinder` includes a differential test that compares each kernel with the reference
  implementation for sizes 16, 24, 32, 40, 48, and 56 bit, random buffers, buffer lengths around the
  SIMD widths and needle length, an embedded pattern at every bit position of small buffers, and the
  bzip2 magic at all eight bit shifts. All 29,196 checks pass for each of portable, SSE2, AVX2, and
  AVX-512, both directly and through the automatic dispatch.
- Existing tests pass: `testBitStringFinder` 30/30, `testBlockFinder` 5/5,
  `testParallelBitStringFinder` 228/228.
- ASAN+UBSAN (`CMAKE_BUILD_TYPE=Sanitize`, C and C++ instrumented, `-fno-sanitize-recover=undefined`)
  passes for all four kernels, including the parallel and block finder tests.
- The benchmark on real `base64-256KiB.bz2` re-reads every reported offset with `BitReader` and
  compares results across implementations without error.
- The end-to-end `ibzip2` md5 of base and change are identical.

## Follow-ups

- The first-byte filter has an 8/256 false-positive rate. A 16-bit pair filter or a second-byte check
  could cut candidate verification further; the current AVX-512 path is already limited partly by
  candidate checks.
- A NEON kernel would extend the fast path to ARM; until then automatic dispatch keeps the reference
  path there.
