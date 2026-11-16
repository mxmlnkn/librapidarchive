# bzip2 SIMD-gather multi-block LF chase: measured dead end

Scratch experiment only. No tracked files were modified. All sources, binaries, and raw runs are under
`tmp/bzip2-simd-batch/` (ignored tree).

## Question and verdict

Can AVX2 `_mm_i32gather_epi32` advance 2/4 independent 900k-entry BWT LF chains faster than
(a) an unrolled scalar interleave of the same blocks and (b) libsais's intra-block multi-chain inverse BWT?

**Verdict: measured dead end, do not integrate.**

- Gather is **1.5-2.0x slower** than the equivalent unrolled scalar interleave for 2/4/8 independent blocks.
- Gather is **2.5-3.5x slower** than libsais `libsais_unbwt_cyclic` on one block, and the best multi-block
  scalar interleave is still **1.8-2.4x slower** than libsais.
- Best gather configuration (8 blocks) reaches only 41-48 MB/s of *chase-only* throughput, versus
  **93 MB/s base64 / 126 MB/s text for the current `perf/bzip2-fast` full decoder** (Huffman + libsais +
  RLE + CRC). The chase alone is already 2.2-2.7x slower than the whole current decoder.
- The gather hypothesis (one instruction hides several chains' latency) does not hold on Zen 4: the OoO
  core already overlaps independent scalar loads, while `vpgatherdd` has microcode/latency overhead and is
  a slower way to issue the same loads. Gather only wins when the table is L3-resident **and** the baseline
  is a poorly compiled array-based interleave, not a register-resident unrolled one.

## Harness

The paths below are local investigation paths. Replace `tmp/bzip2-*` with your own work directory and
`worktrees/bzip2-huffman` with the repository checkout.

- Source: `tmp/bzip2-simd-batch/simd_batch.cpp`
- Binary: `tmp/bzip2-simd-batch/simd_batch`
- Raw runs: `tmp/bzip2-simd-batch/runs/{base64,text64}-r{1,2,3}.txt`
- Third-party: libsais 2.12.1 header/source from `tmp/bzip2-refs/libsais`;
  `Block::readBlockData()` and `BurrowsWheelerTransformData` from
  `src/indexed_bzip2/bzip2.hpp`.

The harness parses the first 8 blocks of each corpus with the real `bzip2::Block` reader (so `bwtData`,
`origPtr`, `writeCount`, and the frequency table come from the actual decoder), builds the classic packed
LF table itself, and times only the dependent LF chase. It does not time Huffman, table construction, or
RLE/CRC. Output bytes are accumulated into a checksum (no per-byte store) so the variants differ only in how
the next pointer is loaded. The scalar reference chase was verified byte-for-byte against
`libsais_unbwt_cyclic` on block 0 of both corpora (`scalarMatchesLibsais=1`, see raw runs).

The gather variants place all tables in one contiguous, page-aligned allocation with a padded element stride
(`901120` elements = 220 pages), use absolute per-lane table offsets, and do one gather per step:

```c
g   = _mm_i32gather_epi32(base, index, 4);   /* 4 independent blocks       */
acc = _mm_xor_si128(acc, g);
index = _mm_add_epi32(_mm_srli_epi32(g, 8), laneBase);
```

2-block gather uses a masked 4-lane gather (AVX2 has no 2-lane 32-bit gather). 8-block gather uses
`_mm256_i32gather_epi32`. The scalar baseline is fully unrolled with one pointer/accumulator pair per chain
in registers; this matters, because a generic runtime loop over the table array was 1.5-2x slower and made
gather look artificially good.

### Commands

```bash
cd <workdir>
clang-21 -O3 -march=znver4 -I <libsais>/include \
  -c <libsais>/src/libsais.c -o libsais.o
clang++-21 -O3 -march=znver4 -std=c++17 -pthread \
  -I <repo>/src -I <libsais>/include \
  simd_batch.cpp libsais.o -o simd_batch

taskset -c 15 ./simd_batch <corpus>/base64.bin.bz2 5
taskset -c 15 ./simd_batch <corpus>/text64.bin.bz2  5
taskset -c 15 ./simd_batch tmp/bzip2-corpus/base64.bin.bz2 5 n1     # n2, n4, n8, intra, prefetch, libsais
taskset -c 15 perf stat -e cycles,instructions,l2_cache_req_stat.ls_rd_blk_c,ls_dmnd_fills_from_sys.all,dTLB-load-misses,dTLB-loads \
  ./simd_batch tmp/bzip2-corpus/base64.bin.bz2 3 n4
```

All runs were pinned to CPU 15. Caveat: CPU 14 is the SMT sibling of CPU 15 and the AVX2 benchmark
(`bench-N4`/`bench-N8`, 67-100% busy) plus the parallel-reader benchmark on CPUs 1-8 share the same 16 MiB
L3. Absolute numbers are therefore inflated and drift; the three rounds were taken in detected gaps when no
`bench-N*` was running. The verdict rests on same-process, same-footprint ratios, which are contention-robust.

## Results (ns per output byte)

Medians over 3 independent rounds, each round best-of-5 alternating scalar/gather. `n=899981` per block,
first 8 blocks; both corpora have identical block sizes. `vsScalar1` >1 means faster than one block;
`vsLibsais` >1 means faster than the full libsais inverse of one block.

### base64.bin.bz2

| variant | ns/B | vsScalar1 | vsLibsais |
|---|---:|---:|---:|
| scalar1 (baseline) | 29.01 | 1.00x | 0.31x |
| scalar interleave 2 | 25.02 | 1.16x | 0.35x |
| gather 2 (masked) | 50.47 | 0.57x | 0.18x |
| scalar interleave 4 | 23.80 | 1.22x | 0.37x |
| gather 4 | 40.29 | 0.72x | 0.22x |
| scalar interleave 8 | 16.07 | 1.80x | 0.55x |
| gather 8 (AVX2) | 24.17 | 1.20x | 0.37x |
| scalar intra-block 4 chains (synthetic) | 15.06 | 1.93x | 0.59x |
| gather intra-block 4 chains (synthetic) | 12.13 | 2.39x | 0.73x |
| libsais `unbwt_cyclic`, 1 block, full | 8.87 | 3.27x | 1.00x |

### text64.bin.bz2

| variant | ns/B | vsScalar1 | vsLibsais |
|---|---:|---:|---:|
| scalar1 (baseline) | 18.54 | 1.00x | 0.32x |
| scalar interleave 2 | 17.01 | 1.09x | 0.35x |
| gather 2 (masked) | 28.39 | 0.65x | 0.21x |
| scalar interleave 4 | 17.86 | 1.04x | 0.33x |
| gather 4 | 28.47 | 0.65x | 0.21x |
| scalar interleave 8 | 14.12 | 1.31x | 0.42x |
| gather 8 (AVX2) | 21.04 | 0.88x | 0.28x |
| scalar intra-block 4 chains (synthetic) | 8.54 | 2.17x | 0.69x |
| gather intra-block 4 chains (synthetic) | 7.64 | 2.43x | 0.77x |
| libsais `unbwt_cyclic`, 1 block, full | 5.91 | 3.14x | 1.00x |

Gather vs scalar at the same block count (median):

| blocks | base64 | text64 |
|---:|---:|---:|
| 2 | 0.50x | 0.60x |
| 4 | 0.59x | 0.63x |
| 8 | 0.66x | 0.67x |
| intra-block 4 | 1.24x | 1.12x |

The intra-block rows are **synthetic**: starts are index-spaced nodes on the LF cycle, found by walking the
chain (that walk is excluded from the timing). They are not a valid full-coverage partition and include no
mark/P-table construction. That is exactly the hard part libsais already solves.

## End-to-end comparison (experiment 2 not built)

RLE/CRC was not implemented because the chase alone already loses. Current `perf/bzip2-fast` single-core
full decoder (from `tmp/bzip2-2x-state.md`):

| corpus | current full decoder | best gather8 chase only | best scalar8 chase only | libsais inverse only |
|---|---:|---:|---:|---:|
| base64 | 93.09 MB/s (10.75 ns/B) | 41.4 MB/s (24.17 ns/B) | 62.2 MB/s (16.07 ns/B) | 112.7 MB/s (8.87 ns/B) |
| text64 | 126.15 MB/s (7.93 ns/B) | 47.5 MB/s (21.04 ns/B) | 70.8 MB/s (14.12 ns/B) | 169.2 MB/s (5.91 ns/B) |

Adding RLE+CRC can only widen the gap (the branch's text RLE+CRC pass alone is ~2.1 ns/B). A gather
decoder would need to beat a *chase-only* budget of ~10.75 / ~7.93 ns/B, and it is at 24.2 / 21.0 ns/B.

## Alignment, padding, prefetch (experiment 3)

- Page-aligned per-table stride vs cache-line-packed stride: differences are within run-to-run noise for
  the scalar interleave. Gather with packed stride was worse on base64 and statistically unchanged on text.
  No reliable padding win.
- Prefetch of the next gather indices (`_mm_prefetch(T0)` after extracting them) was neutral:
  base64 39.98 -> 39.15 ns/B (within noise), text 28.08 -> 27.88 ns/B. The address of the next load is the
  result of the current load, and the next gather is issued immediately after it is computed, so there is
  nothing to prefetch ahead of.
- 2-lane 64-bit gather is not useful for this payload: a 64-bit gather would require widening each 32-bit
  LF entry to 8 bytes, doubling the per-block footprint to 7.2 MiB and making the L3/TLB problem worse.
  The masked 4-lane 32-bit gather is the best available 2-block form and was the slowest variant per byte.

## Cache/TLB evidence

`perf stat` with AMD Zen 4 events, base64, reps=3, one configuration per process. "per chase load" divides
by the known number of LF loads performed (warm-up plus timed chases).

| blocks | table footprint | 4 KiB pages | L2 data-read block misses / chase load | dTLB-load-misses / chase load |
|---:|---:|---:|---:|---:|
| 1 | 3.6 MiB | 900 | 2.09* | 3.6% |
| 2 | 7.2 MiB | 1800 | 1.20 | 2.0% |
| 4 | 14.4 MiB | 3600 | 1.05 | **20.1%** |
| 8 | 28.8 MiB | 7200 | 0.98 | **50.8%** |
| intra 4, one table | 3.6 MiB | 900 | 1.25 | 1.4% |

\* n1 is inflated by table construction and the cold first pass.

Every LF load misses L2 once the table exceeds the 1 MiB per-core L2, so L2 misses are not the differentiator;
the dTLB miss rate is. Two tables (1800 pages) still fit the ~3072-entry L2 TLB. Four tables (3600 pages)
cross it and eight tables (7200 pages) miss on half of all loads. This is the same capacity cliff reported
before and it is not addressed by gather; the gather variant uses the same pages and the same random order.
Packing the tables contiguously with a page-aligned stride actually makes the per-table page offsets large
multiples of the page size, but the page count, and therefore the TLB pressure, is unchanged.

`ls_dmnd_fills_from_sys.all` grows from 29.6M (1 block) to 36.4M (2), 47.6M (4), and 70.0M (8) for a fixed
per-chase load count that grows as N; the extra demand fills confirm that the multi-block working set is
being served below L2, not hidden.

## Why gather loses

- The multi-block chase is latency-bound, not issue-bound. Four independent scalar loads per iteration
  already fill the load pipes and overlap their L3/DRAM latencies; the unrolled scalar loop reaches
  1.0-1.8x over one chain.
- Zen 4 `vpgatherdd` expands into per-element loads with gather bookkeeping and a long completion latency.
  It waits for the whole vector and cannot be pipelined as freely as four independent scalar loads.
- In the only L3-resident case (intra-block, one 3.6 MiB table), gather is only 1.1-1.25x faster than the
  scalar 4-chain chase, still 1.3-1.4x slower than libsais's full 2-step multi-chain inversion, and the
  marks needed to make those chains valid are exactly what libsais already computes during its P scatter.

## Residual idea (low value)

Applying gather to libsais's 2-step `P` table (one dependent gather per 2 output bytes, with libsais's
already-computed marks) could shave a small amount from its multi-chain chase, but the measured intra-block
gather advantage over scalar is only 1.1-1.25x, and libsais is already 1.3-1.4x ahead of even that synthetic
chase. Expected gain is under ~10-20% of the inverse-BWT phase with substantial complexity, not worth
pursuing relative to the existing libsais integration.

## Recommendation

Stop here. Keep the libsais intra-block path (`perf/bzip2-fast`). Do not add a multi-block gather or scalar
interleave decoder. The measured dead end is robust across both corpora and is explained by the L3/TLB
capacity cliff plus Zen 4 gather throughput, not by harness noise.
