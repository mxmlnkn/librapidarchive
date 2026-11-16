# AI-assisted performance and hardening work: overview

Source material: `results/ai/` (reports dated 2026-09 to 2026-11-17). This is a release-note input,
not a maintained ledger. "Accepted" means kept on the named performance branch or in `master` at the
time of the report. The bzip2 changes are merged into `master`; the gzip 8x/SIMD work, the ARM NEON
work, and the C ABI shims remain on their branches.

## 1. Workstreams

- **gzip inflate.** Scalar literal-run batching, constant-stride table loads, table construction by
  doubling, short-match word copies, PCLMUL CRC, AVX-512/AVX2/NEON literal decoders, local bit window,
  and the chunk-pipeline changes. The 8x result is specific to the custom no-ISA-L path; the default
  ISA-L build keeps the chunk pipeline and is at parity with, or slightly ahead of, zlib-ng minigzip on
  the shipped corpora.
- **bzip2 single-core.** Replaced the LF chase with libsais, then attacked Huffman/MTF/RLE: packed
  32-bit Huffman entries, 7-Zip packed MTF list, branchless SSE2 MTF, word MSB refill, short RLE1
  stores, incremental run emission, and next-symbol lookahead. The base64 2x target was met; text
  stayed below it.
- **bzip2 parallel/buffers.** Heap-allocated 135 KiB `Block` (the worker-stack aliasing pathology),
  in-place libsais, removal of zero-initialization, worker-side EOS parse, reusable block buffers,
  allocated-once BWT scratch, exact-size output allocation, and last-block prefetch. P8/P1 is
  hardware-bound at about 3.25-3.29x.
- **libsais inverse BWT.** `libsais_unbwt_cyclic` integration, pipelined scatter, smaller decode
  chunks, and per-worker `libsais_unbwt_ctx` reuse. The 2x libsais target was not reached; the
  integration and context changes were the durable wins.
- **SIMD (RLE/CRC/MTF/literal).** SSE2 branchless MTF and 16-byte RLE scan, slicing-by-N and
  PCLMUL/VPCLMUL CRC, AVX-512 eight-literal decode, AVX2 fallback, and ARM NEON MTF/RLE and literal
  decoding. The recurring negative is that the MTF list is bound by its store-to-load round trip.
- **I/O and file readers.** Reusable buffer pool, BWT scratch allocation, one-chunk-per-worker gzip
  scheduling, the single-worker CLI `GzipReader` fast path, the C-shim sequential path, and the
  file-reader null-buffer fixes. Larger I/O buffers were marginal or workload-dependent.
- **Hardening/security.** Python extension packaging, Huffman-cache merge adaptation, null-buffer
  crashes in both file readers, fuzz-harness fixes, and differential probes for PCLMUL CRC, packed
  Huffman entries, the inline short-match copy, and the SSE2 RLE scan. No production bug was found in
  the new hot paths after the audits.

### Late additions (2026-11-11 to 2026-11-17)

These reports arrived after the 2026-11-10 cut used for the counts below. They are included in the
release notes but not in the 170-experiment totals.

- **Inverse-BWT attribution (`results/ai/bzip2/inverse-bwt-attribution.md`).** Traces the interleaved inverse-BWT
  walk to Kärkkäinen, Kempa, and Puglisi, *Slashing the Time for BWT Inversion*, DCC 2012. libsais
  2.4.0/2.12, pbz2, and mibz2 use the same family; the in-tree 8/24-lane walk adds a splitter-based chain
  decomposition. This is history and attribution, not a new measurement. The 2.0.0 decoder replaces the
  libsais inverse BWT described in the earlier workstreams.
- **AVX-512 bzip2 symbol kernel (`results/ai/bzip2/avx512-symbol-kernel.md`).** A 64-entry in-register MTF front
  and `vpermb` symbol decoder. At one thread: 1.050x wall median, 1.0615x paired ratio median (41/48
  pairs), 4.2% fewer cycles, and 13.6% fewer instructions. At four threads: 1.047x wall and 1.006x total
  CPU. A flat 64-symbol alphabet is 1.61x over AVX2 and 1.41x over portable; 256 symbols is within noise.
  The AVX-512 last-to-first builder used 4.0% more cycles than AVX2 on Silesia, and batched increasing
  MTF positions was negative.

## 2. How many approaches?

Counting basis: one experiment is one distinct hypothesis with an explicit outcome in a ledger or
report. Numbered `OPTIMIZATIONS.md` rows count once even when repeated in a state file or another
report. Variants with separate measurements count separately. Where a change spans categories, it is
assigned to the workstream of the measured mechanism (for example, packing to single-core, the SSE2
update to SIMD). Pure measurements, open ideas, and never-implemented proposals are excluded.
Accepted = retained on a branch/release; negative = measured regression or a behavior-preserving
change explicitly reverted as not worth keeping; inconclusive = the report calls it neutral,
noise-limited, or not resolvable. The negative/inconclusive boundary is sometimes a judgment call;
treat the counts as +/-2 per row and do not sum them as commits.

| Workstream | Accepted | Negative/rejected | Inconclusive/noise | Total |
|---|---:|---:|---:|---:|
| gzip inflate (scalar, structural, pipeline) | 18 | 20 | 9 | 47 |
| bzip2 single-core (Huffman/MTF/RLE scalar) | 5 | 14 | 9 | 28 |
| bzip2 parallel/buffers | 6 | 3 | 9 | 18 |
| libsais inverse BWT | 4 | 13 | 6 | 23 |
| SIMD (RLE/CRC/MTF/literal, x86 + NEON) | 16 | 14 | 3 | 33 |
| I/O and file readers | 5 | 1 | 2 | 8 |
| Hardening/security | 6 | 3 | 4 | 13 |
| **Total** | **60** | **68** | **42** | **170** |

The 170 total covers the reports through 2026-11-10. The two late additions above are not included: the
AVX-512 symbol-kernel report adds two accepted and two negative SIMD experiments plus one inconclusive
flat-alphabet check, while the inverse-BWT attribution report is not a measurement.

Report disagreements and ambiguities:

- **libsais context reuse.** The 2026-11-05 P8 experiment measured per-worker context reuse as negative
  (0.984 base64, 0.987 text) in the libsais cache-tuning section of `bzip2/OPTIMIZATIONS.md`. The
  2026-11-09 paired real-block measurement found +7-8% of libsais and integrated it
  (`bzip2/libsais-optimization.md`, context reuse). The later measurement and `master` commit
  `f1190e0a` (release branch `6dc46b5a`) are the current evidence.
- **bzip2 PCLMUL CRC.** `bzip2/OPTIMIZATIONS.md` records the ISA-L PCLMUL drop-in as end-to-end neutral
  on text/base64 and not committed at that time. The extreme-case branch added a hand-written
  PCLMUL/VPCLMUL CRC as one of four changes and measured 4.166x on the zeros case
  (`bzip2/extreme-case-results.md`). It is now in `master` (`3ea75dba`). This is workload-dependent,
  not a contradiction: CRC is hidden on normal corpora and exposed on high-expansion data.
- **Packed MTF.** An early variable-word-shift packed list regressed (the packed-MTF rows in
  `bzip2/OPTIMIZATIONS.md`); the later 7-Zip packed list plus branchless SSE2 update was kept. The
  update shape, not packing alone, decided the result.
- **Targets.** bzip2 base64 reached 2.08x, then 2.34x single-core; text reached 1.46x, then 1.69x, so
  the 2x target was met only on base64. libsais itself did not reach 2x (`bzip2/libsais-optimization.md`).
- **Corrected number.** The earlier gzip claim that 16% of literals decoded scalar was wrong; the
  instrumented value is 6.4% (`gzip/gzip-mixed-length-report.md`, corrected count).
- **ARM NEON.** The literal decoder gains 1.68x on synthetic base64 but its gate does not fire on the
  real `gzip -9` base64 corpus, so the realistic `.gz` corpus shows no NEON gain (`arm/arm-report.md`,
  NEON gating).

## 3. Methodology

The reliable method was same-session paired A/B, not absolute rates. Both binaries were rebuilt from
their own trees, pinned with `taskset` (commonly `-c 14`, also 1, 3, 13, 15, or 1-8 for parallel
runs), and alternated base/change with the order flipped each pair. Each invocation reported its own
iterations; the analysis used best-of-N, medians of per-pair ratios, ranges, and win counts. Warm-up
iterations were discarded, and the first run of a pair was known to be 1.5-3% slower. `perf stat -e
cycles,instructions` was the primary counter source, with `minor-faults`, cache misses, dTLB misses,
and branch misses where they explained the mechanism. Callgrind was used when `perf` was unavailable
(the ARM Pi had no `perf`). Isolated microbenchmarks established ceilings; real-block drivers decided
libsais changes (64 real BWT blocks, base and change linked into one binary with renamed symbols,
alternating calls, byte-equality check, A/A control within 0-1.5%). Shared hosts were the main caveat:
the 16 MiB L3 is shared, absolute rates drifted by 20-30% between sessions, and a stale benchmark
binary once made a neutral change look ~30% faster. A result below roughly 2% was not treated as a
win. Correctness gates were the gzip/bzip2 test suites, forced AVX-512/AVX2/scalar paths, ASAN+UBSAN,
TSan for the parallel reader, fuzz iterations, and md5/CRC comparison against the reference tools.

## 4. Headline results

| Result | Source |
|---|---|
| libsais inverse BWT: base64 1.455x, text 1.374x end to end; isolated 1.86x on `base64-256KiB.bz2` | `results/ai/bzip2/OPTIMIZATIONS.md` (libsais inverse BWT) |
| libsais context reuse: 7-8% of libsais, ~3.5% end to end; minor faults -55.2% Silesia, -44.4% text64 | `results/ai/bzip2/libsais-optimization.md` |
| bzip2 combined single-core: base64 2.077x then 2.343x; text 1.459x then 1.690x vs pre-libsais | `results/ai/bzip2/OPTIMIZATIONS.md` (combined result) |
| bzip2 SSE2 MTF: base64 1.275x incremental, 1.512x combined; Huffman phase 1.99x base64 / 1.21x text; base64 branch misses 161M -> 22M | `results/ai/bzip2/OPTIMIZATIONS.md` (Huffman/MTF phase) |
| bzip2 AVX-512 symbol kernel: 1.050x wall, 1.0615x paired; 4.2% fewer cycles, 13.6% fewer instructions; 64-symbol flat 1.61x over AVX2 | `results/ai/bzip2/avx512-symbol-kernel.md` |
| bzip2 16-byte RLE scan: 1.036x base64, 1.153x text end to end; `decodeBlock` phase about 2.9x / 1.9x | `results/ai/bzip2/OPTIMIZATIONS.md` (vectorized RLE scan) |
| bzip2 slicing-by-8 CRC: 5.03x isolated; 1.048x text / 1.068x base64 end to end; N=8 beats N=4 | `results/ai/bzip2/OPTIMIZATIONS.md` (slicing-by-N CRC) |
| bzip2 extreme-case integration (PCLMUL CRC, exact-size allocation, last-block prefetch, next-symbol lookahead): zeros P1 4.166x, english 1.096x | `results/ai/bzip2/extreme-case-results.md` |
| bzip2 parallel: final P8 vs old P1 7.44x base64 / 5.64x text; P8/P1 3.25x / 3.29x | `results/ai/bzip2/OPTIMIZATIONS.md` (parallel result) |
| gzip custom inflate: 7.95x 128 KiB first chunk, 7.65x whole stream, 7.77x window-full, 7.47x gzip-reader, 6.16x marker-first, 5.72x usage | `results/ai/gzip/OPTIMIZATIONS.md` (row 40) |
| gzip AVX2 fallback: 1.80-2.04x over the scalar 3x tip, about 80% of the AVX-512 gain | `results/ai/gzip/OPTIMIZATIONS.md` (row 41) |
| gzip PCLMUL CRC: isolated 4.35 -> 15.46 GB/s; P1 text wall -5.0%, base64 -6.5% | `results/ai/gzip/OPTIMIZATIONS.md` (row 42) |
| gzip full-stream CLI P1: text 2.47x, base64 3.51x (ISA-L); custom path 1.17x / 2.28x; P8 1.90x / 2.37x | `results/ai/gzip/gzip-full-stream-report.md` |
| ARM NEON: bzip2 base64 P1 1.44x, text P1 1.11x; gzip NEON literal 1.68x custom, 1.45x benchmark | `results/ai/arm/arm-report.md` |

## 5. What did not work

- **AVX2/AVX-512 MTF updates.** AVX2 was 4.38 vs SSE2 3.81 ns/update; AVX-512 `vpermb` was only
  ~10% faster inlined and spilled in the real loop. The MTF list is bound by its 64-byte store-to-load
  round trip, so fewer vector instructions do not help (`results/ai/bzip2/OPTIMIZATIONS.md` (MTF section)).
- **A 2x libsais kernel.** The committed libsais changes are 2-5% (up to ~7% on text); at libsais's
  ~45% share, 2x on libsais would only be 1.25x end to end (`results/ai/bzip2/libsais-optimization.md`).
- **Interleaving independent BWT blocks.** 0.657x text and 0.700x base64 end to end; two `dbuf` tables
  exceed the effective L3 and dTLB misses rise sharply (`results/ai/bzip2/OPTIMIZATIONS.md` (block interleaving)).
- **AVX2 gather for the block chase.** 1.5-2.0x slower than the unrolled scalar interleave and
  2.5-3.5x slower than libsais; dTLB misses are ~2% at 2 blocks, 20% at 4, and 51% at 8
  (`results/ai/bzip2/bzip2-simd-batch-report.md`).
- **Alternative Huffman LUTs.** The two-symbol variants were negative (text 1.006, base64 1.007, 4/8
  wins) and the AVX-512 batch was 0.849x on base64; LUT sizes 10/11/13 changed instructions by less
  than 0.6% (`results/ai/bzip2/OPTIMIZATIONS.md` (Huffman LUT negatives)).
- **SIMD RLE fill and four-literal batch.** The existing scalar four-byte test short-circuits after one
  mismatch; the four-literal batch regressed base64/text by about 8.6%, and the `memset`-based RLE2
  flush by about 7.2% (`results/ai/bzip2/OPTIMIZATIONS.md` (RLE negatives)).
- **PCLMUL CRC on normal corpora.** It cut CRC instructions but not cycles; the CRC is hidden under
  inverse-BWT/MTF stalls. The committed kernel is still used for its high-expansion win, but on normal
  text/base64 it is cycle-neutral (`results/ai/bzip2/OPTIMIZATIONS.md`, PCLMUL history).
- **Huge pages and libsais concurrency gates.** THP was not backed and made faults worse; a semaphore
  limiting concurrent P tables reduced cycles but lowered wall throughput by idling workers
  (`results/ai/bzip2/OPTIMIZATIONS.md` (huge pages and concurrency gates)).
- **Fused caller-side gzip window.** The libdeflate-style local window regressed literal-heavy ISA-L
  decoding by 67-70% from register pressure (`results/ai/gzip/OPTIMIZATIONS.md` (fused caller window)).
- **More gzip micro-reshuffling.** Packed multi-literal stores (-6%), destination-wrap copies (-3.1%),
  source-wrap two-memcpy (-2.7%), and slack-free wrap loops (-2.3 to -3.8%) all lost because the
  added branch or store handling ran on every symbol (`results/ai/gzip/OPTIMIZATIONS.md` (micro-reshuffling rows)).
- **ARM NEON literal gate on real streams.** The 8-wide NEON decoder does not fire on the real `gzip -9`
  base64 corpus, so the synthetic 1.45-1.68x does not translate to that corpus
  (`results/ai/arm/arm-report.md` (NEON gating)).
