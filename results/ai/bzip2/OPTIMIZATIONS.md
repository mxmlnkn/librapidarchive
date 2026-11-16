# OPTIMIZATIONS.md

A summary of the single-core bzip2 decode performance work in `librapidarchive`. The inflate work is
documented separately in `../gzip/OPTIMIZATIONS.md`; this file follows the same rules: every hypothesis that was
tested is recorded here with its outcome, positive or negative, so that it is not retried blindly.

Status: these changes are merged into `master`. The branch commits named below were rewritten during
integration; use the subjects as the stable reference. Changes that landed after these measurements are
listed in the next paragraph, so a "not committed" note is a statement about the branch at the time of
the experiment, not about the current tree.

Subsequent `master` work includes the exact-size block allocation (`1e491bb2`), last-block prefetch
(`4dd3c6a7`), next-symbol lookahead (`0a985900`), the PCLMUL/VPCLMUL CRC (`3ea75dba`), per-worker
libsais context reuse (`f1190e0a`), vectorized MTF above index 63 (`6ff22145`), 64 KiB sequential
chunks (`b3f2b4d1`), incremental run emission (`efef924a`), the AVX-512 RLE scan (`bb8c0de5`),
one-word short-run stores (`fa42ee48`), NEON MTF/RLE (`49a659b9`), and the 64-bit MSB refill
(`04716e81`). The numbers and test counts below are branch-time measurements and are not re-baselined for
all of those later changes.

## Target and current state

`BZ2Reader` decodes about **40 MB/s for random base64** and **78 MB/s for source text** (block size 900k,
`bzip2 -9`, Ryzen 7 PRO 8840U, a quiet core, best of many runs). For comparison, GNU `bzip2 -dc` 1.0.8
reaches about 28 MB/s and 60 MB/s on the same files, so the current decoder is already about 1.3-1.5x
faster than the reference implementation. A 2x improvement over the current decoder therefore needs a
structural change, not a few percent of instruction polishing. bzip2's decode is dominated by the
inverse Burrows-Wheeler transform, which is a single dependent chain of random memory accesses. The
measured phase split for the text workload (one 73 MB file, 82 blocks, from `Block::Statistics`) is:

| phase | time | share |
|---|---:|---:|
| inverse BWT + RLE + CRC + output (`decodeBlock`) | 0.704 s | 63% |
| Huffman + MTF + run decoding (`readBlockData`) | 0.333 s | 30% |
| counting-sort scatter (`prepare`) | 0.079 s | 7% |
| block header, selectors, trees | 0.005 s | 0.4% |

The BWT output loop is at the dependent-load latency floor: per output byte it follows one random
`dbuf` index with no way to issue the next load earlier. Removing the per-byte CRC computation does not
change its cycle count, which confirms that the CRC and the output work are already hidden under that
latency.

After the libsais change below, the quiet-core state on the same files is **55.4 MB/s base64** and
**116.4 MB/s text**, i.e. 1.45x and 1.37x over the decoder above. The phase split has moved to:

| phase | base64 | text |
|---|---:|---:|
| Huffman + MTF + run decoding (`readBlockData`) | 0.419 s | 0.365 s |
| libsais bWT construction and inversion | 0.157 s | 0.314 s |
| RLE expansion + CRC + output (`decodeBlock`) | 0.074 s | 0.171 s |
| block header, selectors, trees | 0.007 s | 0.007 s |

The Huffman/MTF phase is now the largest single cost, and it is compute-bound rather than latency-bound.
`perf` on base64 attributes 39% to `readBlockData` and 23% to the MTF `memmove`; libsais accounts for
23%, and the exposed RLE pass for 12%. The measured expansion pass with and without CRC is the same
(1.35 vs 1.37 ns per input byte on text, 0.98 vs 1.02 on base64), so the CRC is still hidden.

## How we measure

- Tool: `src/benchmarks/benchmarkBzip2.cpp` (`benchmarkBzip2`). It keeps the compressed input in memory,
  so no file I/O is measured, and supports sequential (`--parallel 1`) and parallel readers as well as a
  `discard` mode that does not copy the output into a caller buffer. `discard` is used for all decoder
  A/B runs. The benchmark prints every iteration so a max-of-iterations estimator is possible.
- The machine shares its 16 MiB L3 with other agent processes, so naive wall-clock or `perf cycles` runs
  vary by up to 2x. Two complementary methods are used:
  1. **Alternating A/B pairs with max-of-N:** `taskset -c 1 benchmarkBzip2 --iterations 10 --mode
     discard <file>`, take the best iteration per invocation, alternate base/change and flip the order
     every pair, report per-file medians and ranges of `change/base`. This is the primary required
     method; a contaminated pair (any run far below the others) is discarded and rerun.
  2. **Concurrent cycles:** run both binaries at the same time on two cores with `perf stat -e cycles`,
     cycling launch order and core assignment in a Latin square. Both processes see the same external
     load, which cancels drift. The base-vs-base noise floor with 16 repetitions was median 1.007 on
     base64 with 10/16 wins, so effects below about 2% are not resolvable.
- Always rebuild the exact benchmark target against the current tree, and never build or fuzz while
  measuring.
- Correctness gate: `testBZ2Reader`, `testParallelBZ2Reader`, and the per-block CRC check performed by
  the decoder itself.

## Committed change: libsais inverse BWT

The primary source and history of the interleaving and chain form are summarized in
[inverse-bwt-attribution.md](inverse-bwt-attribution.md).

| # | Change | Outcome |
|---|--------|---------|
| 0 | Replace the one-step LF chase by libsais 2.12.1 `libsais_unbwt_cyclic`, with the BWT bytes in a contiguous buffer and an incremental RLE/CRC/output pass. | **Kept.** Base64 1.455x, text 1.374x end to end. Isolated first-block prototype: build-LF + chase + RLE 14.36 ns/B vs libsais cyclic + RLE 4.84 ns/B, byte-exact; on `base64-256KiB.bz2` 10.05 vs 5.40 ns/B (1.86x). Raw inverse output was byte-exact against the LF chase on `base64-256KiB.bz2`, `dolorem-ipsum.txt.bz2`, both corpora and a 2 MiB random-text file. The LF build remains as a fallback when libsais cannot allocate its temporary tables. |

Measurement, `benchmarkBzip2 --parallel 1 --mode discard`, CPU 2, 8 alternating pairs, best of 10
iterations per invocation, base and branch binaries rebuilt from their own trees:

| file | base | change | ratio (median, range) |
|---|---:|---:|---|
| `base64.bin.bz2` | 38.07 MB/s | 55.37 MB/s | 1.4546 (1.4025..1.4846) |
| `text64.bin.bz2` | 84.72 MB/s | 116.37 MB/s | 1.3744 (1.3588..1.3779) |

The text number is lower than the base64 number because the fixed libsais table construction (4.3 ns/B)
is a larger fraction of a phase that was less dominant to begin with. Small blocks are slower with
libsais than with the chase (866 B block: 3.0 vs 4.9 ns/B), so the fallback matters for tiny inputs.

The gain is L3-sensitive. When a second single-core benchmark runs on another physical core of the same
CCX, the same 8-pair method gives a base64 ratio of 1.23-1.24 (base 32.9-34.0 MB/s, branch
40.4-41.9 MB/s), and one pair set measured 1.236 median. The libsais path needs its 3.6 MB P table plus
the 0.9 MB BWT buffer concurrently, whereas the chase only streams `dbuf`, so it loses more of its gain
to a contended L3. Quiet-core numbers are the meaningful ones for the 2x comparison; under external load
the improvement is about 1.23x.

## Dropped change

| # | Change | Outcome |
|---|--------|---------|
| 1 | Inline the MTF shift for positions <= 16 and cache the `std::array`/vector `data()` pointers and `dbuf` size in locals. | **Dropped as a negative result.** User measurements showed a slowdown. Local A/B with best-of-8 and 8 alternating pairs gave text 1.0053 (6/8) and base64 1.0361 (8/8), but that is at or below the noise floor and the commit makes the loop harder to read. The arrays have `constexpr` sizes, so the cached `data()` pointers and size are invariant; they should not be needed and may hinder alias analysis. Reverted with this branch. The only part with a plausible mechanism is the inline shift, and it is not big enough to keep on its own. |

The accompanying pointer/size locals (`dbufData`, `byteCountData`, `symbolToByteData`, `mtfSymbolData`,
`dbufSize`) are not worth keeping independently of this experiment.

## Tried and rejected

All changes were behavior-preserving and passed the bzip2 tests unless stated otherwise.

| # | Change | Outcome |
|---|--------|---------|
| 2 | Keep the inverse-BWT decoder state (`writePos`, `writeCurrent`, `writeRun`, `writeCount`, CRC) in locals across the loop instead of in members. | **No win:** instructions fell 12.9% on text and 7.1% on base64, but cycles and wall time did not improve (6 alternating wall pairs within +/-2%). The loop is latency-bound, not instruction-bound. Reverted. |
| 3 | Two-step jump table for the inverse BWT: one 64-bit entry per position holding both bytes and the positions after one and two steps (7.2 MiB per block). | **Regression:** readBlockData +39% and decodeStream +20% samples, about +10% cycles. The larger table and its construction cost more than the halved dependent-load count. Reverted; a two-array variant (4-byte `next2` + 2-byte byte pair, 5.4 MiB) was not better in-tree. |
| 4 | Two-phase decode: run the inverse BWT into a linear `bwtData` buffer in `prepare()`, then do RLE, CRC and output in `decodeBlock()` (2-step table, split arrays). | **About +3%, not worth it:** chase 0.562 s -> 0.446 s (1.26x), but table construction 0.079 s, extra buffer zeroing and the now-exposed RLE pass cancel most of it. Total 1.121 s -> 1.089 s while retaining ~5.4 MiB extra per block, which also multiplies in the parallel reader. Reverted. |
| 5 | Skip the per-byte CRC update (temporary, CRC check disabled) to measure its ceiling. | **Hidden:** instructions -11%, but cycles were neutral on text and the concurrent test gave text 1.008 and base64 0.987. The CRC is covered by the BWT load latency. Reverted. |
| 6 | LUT sizes 10, 11, 13 instead of 12 for `HuffmanCodingShortBitsCached`. | **No effect:** instruction counts within +/-0.6% on both inputs, so the long-code fallback frequency barely changes. The earlier 3900X numbers that selected 12 still apply. No change. |
| 7 | MTF shift loop only for `ii <= 8` (instead of 16). | No better than #1; the 16-entry inline path also lets the compiler emit its 2/4/8/16-byte short-copy idiom. Not committed separately. |
| 8 | Hoist `BitReader` state into a local `LocalReader`, refill 32 bits at a time. | **No-op:** GCC/LTO already keeps `m_bitBuffer`, `m_bitBufferFree` and the input position in registers across the inlined Huffman decode. A fresh build of the hoisted tree produced a **byte-identical** binary (md5 equal, `readBlockData` size 0x3d7a in both) and A/B gave base64 1.0004 and text 0.9978. The research report's 5.21 -> 2.49 ns/symbol harness was compiled without this hoisting, so the in-tree win is zero. Not committed. |
| 9 | Packed 64-bit MTF list (32 words, 8 symbols each). | **Regression:** base64 A/B 0.7738 (6 pairs); `readBlockData` own share rose from 41.5% to 67.8%. The variable word-shift loop is slower than the AVX-512 `memmove`. Not committed. |
| 10 | Word-wise MTF shift with overlapping 8-byte moves, scalar below 8. | **Regression:** ~54 vs 64 MB/s base64 in single runs after the unit test passed. The overlapping stores add a store-forwarding chain. Not committed. |
| 11 | Inline masked 8-byte shift for MTF indices < 8 (and later <= 15). | **Regression:** ~62 vs 64 MB/s base64, ~110 vs 116 MB/s text in single runs. The index branch and mask arithmetic cost more than the short `memmove`. Not committed. |
| 12 | `std::memset` for the RLE2 run flush in `readBlockData`. | **Regression:** base64 A/B 0.9281 (6 pairs, range 0.9176..0.9366). Base64 runs are mostly one byte, so the call overhead dominates. Not committed. |
| 13 | Slicing-by-8 tables for the non-reflected bzip2 CRC, applied to the decoded chunk in one pass. | **Committed later; the first run was neutral.** The first implementation measured 5.1x in isolation but base64 0.9465 (noisy, 6 pairs) and text 1.0117 (6 pairs), so it was dropped as cache pressure without visible gain. The configurable version below re-measured on a different load and was kept because no slice count regresses and N=8 reduces instructions by 1.6% (base64) and 3.4% (text). |
| 14 | Batch four literals in the RLE expansion using a zero-byte mask. | **Regression:** base64 0.9147 and text 0.9140 (6 pairs each). The existing scalar check short-circuits after one byte comparison, while the batch adds a load and ~10 ALU ops per four bytes. Not committed. |
| 15 | Invert in place (`U == T`) to drop the separate 900 KiB inverse buffer. | **Neutral:** base64 1.0065, text 0.9986 (6 pairs each, under external load and noisy). Saves 0.9 MiB per block but does not change throughput, and it makes the fallback depend on libsais not touching T before reporting an error. Not committed. |

The two-step table result (#4) is the useful negative: halving the dependent loads helps the isolated
walk by 1.6x in a microbenchmark, but in the real decoder the chase only improves 1.26x and the table
build plus cache pressure consume the difference. The inverse BWT is close to its practical single-core
floor with a 900k-entry table on this CPU.

## Ranked open ideas (post-libsais)

The libsais change removed most of the inverse-BWT latency, so the previous ranking is obsolete. Measured
single-core ceiling with the current design is about **1.45x base64 and 1.37x text** over the pre-libsais
decoder. To reach 2x, the Huffman/MTF phase has to shrink; the inverse BWT is no longer the lever.

1. **A fundamentally cheaper MTF.** MTF is now the largest avoidable cost: 33M literal symbols on the
   33.5 MB base64 file, an almost uniform index distribution 1..63 (516k occurrences each), and about
   23% of cycles in the MTF `memmove`. Every list-based variant tried here lost to glibc's AVX-512
   `memmove` (see negatives 9-11). The AVX2/AVX-512 front-list permute was tried in November 2026 and
   is negative end to end; see the section below. The remaining option is a representation where the
   front list never round-trips through memory, which needs the decoder loop itself to be restructured
   around a different MTF/RLE pipeline rather than another instruction-level variant.
2. **Overlap the Huffman phase with the inverse BWT of the previous block.** This is the software-pipeline
   idea (formerly idea 2). It cannot be done by interleaving instructions in one call, but a coarser
   2-block pipeline in `BZ2Reader` would let the branch-heavy Huffman pass and the memory-latency-bound
   libsais pass share one core. It is the only remaining structural idea that composes with libsais and
   does not require a faster MTF.
3. **Reduce libsais table construction.** It is 0.157 s base64 and 0.314 s text. `libsais_unbwt_ctx`
   reuses allocations but allocates for `SAINT_MAX` and is therefore a poor fit for the parallel reader;
   a per-thread context would need a size-aware replacement. Expected gain is a few percent.
4. **Increase `BZ2Reader::IOBUF_SIZE`.** The 4 KiB output buffer makes `decodeBlock` run its setup and
   CRC pass 18k times for the text file. This is shared with the baseline, so it is not the reason for
   the gap, but it may remove a few percent of the exposed RLE time. Measure separately.

The single next experiment is (2), because the AVX2/AVX-512 MTF direction below is now measured negative.

## Negative: AVX2 / AVX-512 MTF update

The SSE2 branchless MTF update (commit `df27e9b7`) shifts four 16-byte lanes and blends the prefix for
positions < 64. Five instruction-level variants aimed to beat it on the same 7-Zip packed list. All were
behavior-preserving and passed a randomized comparison (200k updates per implementation, including the
0/63/64/255 boundaries) against a plain 256-byte list, plus `testBZ2Reader` and the ASAN+UBSAN build for
the AVX-512 intrinsic paths. They are performance negatives only and were not committed.

Isolated microbenchmark, 33.5M updates with the uniform 1..63 base64 distribution, CPU 14, best of 5.
The "inlined" column builds the same benchmark with `-mavx512vbmi` so the intrinsic functions are not
called through a dispatch boundary:

| implementation | called (ns/update) | fully inlined (ns/update) |
|---|---:|---:|
| scalar 64-bit word loop | 6.31 | 6.38 |
| SSE2, four 16-byte lanes (current) | 3.81 | 4.16 |
| AVX2, two 32-byte halves with `vperm2i128` + `vpalignr` | 4.38 | 4.38 |
| AVX-512 VBMI, one `vpermb` on the memory list | 5.70 | 3.76 |

AVX2 does not beat SSE2 even when inlined. The 32-byte half shift needs a `vperm2i128`, a `vpalignr`,
and a carry-byte insert per half, and the dynamic head extraction is a `vpermd`+`vpshufb` pair. The
update is bound by the 64-byte store-to-load round-trip of the list, so fewer vector instructions do not
translate into cycles. AVX-512 `vpermb` reaches only about 10% inlined, which is below the noise of the
real phase.

The natural next idea is to keep the first 64 entries in a zmm register across updates (`vpermb` is both
the shift and the dynamic extraction), with the packed list as the >= 64 tail:

```cpp
__m512i front;  // list[0..63]
for ( every literal ) {
    const auto idx = /* [position, 0, 1, ..., 62] */;
    front = _mm512_mask_permutexvar_epi8( front, ( 1ULL << ( position + 1 ) ) - 1, idx, front );
    const auto head = static_cast<uint8_t>( _mm_cvtsi128_si32( _mm512_castsi512_si128( front ) ) );
}
```

In a standalone tight loop this is **1.17 ns/update**, 3.3x faster than SSE2. In the real
`readBlockData` loop GCC spills the zmm at every update:

```asm
vmovdqa64 (%rsp), %zmm2          # reload front
vpermb    %zmm2, %zmm1, %zmm0{%k2}
vmovdqa64 %zmm0, (%rsp)          # spill front for the next iteration
```

The 64-byte spill restores the store-to-load forwarding cost, and `perf stat` then shows the same
instructions as SSE2 (4.997e9 vs 4.997e9 on base64) with cycles +2.3%. A per-position index/mask table
version (4.5 KiB of read-only tables) cut instructions by 1.9% but raised cycles by 16% (Huffman phase
0.202 -> 0.266 s): the table loads and the spill are on the critical path.

The final attempt decoded a whole 50-symbol Huffman group into a temporary array and processed the group
afterwards, so the front list could stay in a register for 50 updates instead of one. The regrouping
itself is the problem: the Huffman decode loop and the serial MTF/RLE chain stop overlapping, and the
Huffman phase regresses from 0.203 s to 0.384 s (cycles +40%, branch misses +10%). AVX-512 grouped and
SSE2 grouped measured identically, so the register residency did not recover the loss.

Conclusion: leave the SSE2 branchless update in place. A faster MTF needs a different loop structure
that keeps the list in a register while preserving Huffman/MTF overlap, not another ISA variant.

## Negative: two-symbol bzip2 Huffman LUT

Hypothesis: one 12-bit LUT lookup could yield two literal symbols whenever both codes fit into the same
peek, removing one Huffman lookup, one group-boundary check, and one run/EOB/bounds dispatch per pair.
The research estimate was text +10-18% and base64 +3-8% end to end.

Two implementations were measured and both were dropped. The code was reverted; the complete patches are
kept outside the repository for reference.

### Variant A: paired cache entry

A 32-bit entry holds `symbol1` (9 bits), `length1` (5), `symbol2` (9), `length2` (5), and a
`hasSecond` flag. `initializeFromLengths` derives the second symbol from the already filled LUT by
aligning the remaining prefix bits to the top of the next 12-bit window; only literal-literal pairs are
marked. The loop consumes the first code, applies the first MTF update, and only then consumes the
second code and its MTF update. A second symbol never crosses a 50-symbol group boundary and run/EOB
symbols always use the scalar path.

Measured pair coverage, counted over all decoded symbols (a pair covers two symbols):

| LUT bits | text64 pairs / symbols | text64 pair fraction | base64 pairs / symbols | base64 pair fraction |
|---:|---:|---:|---:|---:|
| 12 | 9.97M / 55.26M | **36.10%** | 2.78M / 67.09M | **8.29%** |
| 13 | 10.42M / 55.26M | 37.73% | 2.78M / 67.09M | 8.30% |
| 14 | 10.77M / 55.26M | 38.99% | 2.78M / 67.09M | 8.30% |

Widening the LUT barely helps text and does not help base64 at all: the base64 code-length sums jump
from <= 12 straight past 14, so a larger first-level window cannot reach them. Coverage is also capped
by the literal-only restriction and by the group boundary (one in 50 symbols).

Phase measurement with `ibzip2 -v -d -o /dev/null -f`, `createHuffmanTable` timer (the complete
Huffman + MTF + run loop), 5 alternating pairs, CPU 14, best of each binary:

| file | baseline | two-symbol | ratio |
|---|---:|---:|---:|
| `text64.bin.bz2` | 0.354 s | 0.348 s | 0.982 (noisy, 0.95..1.03) |
| `base64.bin.bz2` | 0.323 s | 0.353 s | 1.094 |

End to end, `benchmarkBzip2 --iterations 10 --mode discard`, 8 alternating pairs, best of 10, CPU 14:

| file | base median | change median | ratio median (range, wins) |
|---|---:|---:|---|
| `text64.bin.bz2` | 78.52 MB/s | 79.41 MB/s | 1.0058 (0.9445..1.1318, 4/8) |
| `base64.bin.bz2` | 47.65 MB/s | 50.87 MB/s | 1.0068 (0.9204..1.5979, 4/8) |

The absolute rates are lower than the quiet-machine numbers in this file because the host was shared
with the parallel and gzip agents; the paired ratios are the evidence.

### Variant B: wide peek plus second lookup

The same 12-bit LUT was reused for both symbols. A 24-bit peek gives the first code (top 12 bits) and
the second code (the next 12 bits, shifted by `length1`). Only literal pairs are consumed; long second
codes and run/EOB symbols consume the first code only. This removes the paired cache construction and
raises potential coverage to every literal-literal pair with both codes <= 12 bits, at the cost of a
dependent second LUT load.

It was also negative: the Huffman phase was 0.367 s -> 0.403 s on text (+9.7%) and 0.330 s -> 0.348 s on
base64 (+5.5%) in the same 5-pair protocol. The 24-bit peek and the dependent second lookup cost more
than the saved group/dispatch work.

### Why it does not pay

- The Huffman LUT load is a small part of the ~15-19 cycles per symbol. The MTF update, the serial
  run/MTF state, and the output store dominate.
- In variant A the second `moveToFront` immediately follows the first, so its load waits on the first
  update's 64-byte store-to-load round trip. Scalar iterations hide that latency behind the next Huffman
  decode. The pair saves one decode but exposes the MTF dependency.
- The `hasSecond`/group-boundary checks and the second bounds check are paid by every literal, while
  only 36% (text) or 8% (base64) of symbols benefit. The overhead is visible on base64, where pairs are
  rare.

The experiment is preserved in the untracked `strict-two-symbol.patch` and
`wide-two-symbol.patch` files.

Both variants passed `testBZ2Reader` (215/215), `testParallelBZ2Reader` (232/232), and
`testHuffmanCoding` (3712/3712); decoded md5 matched the original corpora. The conclusion is that the
Huffman LUT is not the lever left in the single-core decoder. A paired decode only pays if the MTF update
is fused, and that requires a representation that updates two positions in one pass; the existing AVX-512
experiments show the list is bound by its store-to-load round trip, so this is not a local rewrite.

## Committed change: slicing-by-N bzip2 CRC

The per-byte CRC update uses the non-reflected (MSB-first) polynomial 0x04C11DB7, so the hardware CRC32
instruction does not apply. The decoded bytes of one `decodeBlock` call are contiguous, which allows the
CRC to be computed in one pass after the RLE expansion instead of interleaved with it. `N` is a CMake
cache variable `LIBRAPIDARCHIVE_BZIP2_CRC_SLICES` (1, 4, or 8, default 8); N=1 is the byte-wise table.

Isolated benchmark (`benchmarkBzip2CRC`, 128 MiB, min of 5, CPU 14), checksums identical for all N:

| slices | table size | GB/s | speedup |
|---:|---:|---:|---:|
| 1 | 1 KiB (existing table) | 0.436 | 1.00x |
| 4 | 4 KiB | 1.355 | 3.11x |
| 8 | 8 KiB | 2.193 | 5.03x |

End to end, `benchmarkBzip2 --iterations 10 --mode discard`, CPU 14, 8 alternating pairs, best of 10
per invocation. The baseline is `perf/bzip2-fast` (no slicing). The four-way run put all N in the same
pair rotation; the two-way runs alternate only the named binaries and flip their order every pair. The
host was shared with another agent's benchmark, so the ranges are wide:

| comparison | base64 ratio (median, range) | wins | text64 ratio (median, range) | wins |
|---|---|---:|---|---:|
| base -> N1 | 0.9730 (0.8136..1.4782) | 2/8 | 0.8973 (0.7800..1.6436) | 3/8 |
| base -> N8 | 1.0682 (0.8554..1.2273) | 7/8 | 1.0480 (1.0194..1.0766) | 8/8 |
| N1 -> N8 | 1.0741 (0.5661..1.1279) | 7/8 | 1.1892 (0.6086..1.6142) | 7/8 |
| N4 -> N8 | 1.0546 (0.8817..1.2377) | 6/8 | 1.0432 (0.8021..1.2526) | 6/8 |
| base -> N4 (four-way) | 1.2086 (1.0164..1.4291) | 8/8 | 1.1596 (0.9922..1.2603) | 7/8 |

`perf stat` over 3 iterations (instructions are deterministic and do not depend on the external cache
load):

| N | base64 instructions | text64 instructions |
|---:|---:|---:|
| base | 26.146e9 | 28.524e9 |
| 1 | 26.550e9 | 29.328e9 |
| 4 | 25.970e9 | 28.102e9 |
| 8 | 25.720e9 | 27.544e9 |

N=1 pays about 1.5-2.8% extra instructions for the separate pass over the output buffer. N=8 recovers
that and removes another 0.4-0.6% versus the interleaved baseline, mostly from the shorter CRC
dependency chain. The end-to-end effect is therefore small, but it is not a regression.

Cache-pressure trade-off: slicing-by-8 needs 8 KiB of read-only tables instead of 1 KiB, and N=4 needs
4 KiB. Both stay L1-resident next to the 4 KiB output buffer and the hot Huffman/MTF state; the
measured N=8-vs-N=4 difference favors N=8 (isolated 1.62x, end to end 1.04-1.05x median). The earlier
neutral result (#13) came from a run under heavier L3 contention, where the tables and the extra pass
cost as much as the CRC saved. Smaller N is the safer choice only on a very tight data cache; the
configure-time switch exists for that case. With N>1 the byte-wise table handles the `size % N` tail.

The parallel reader uses `BZ2BlockFetcher`, which can hand `decodeBlock` a whole ~900 KiB block, so
the second pass over the output is larger than in the 4 KiB sequential buffer case. A 2-thread A/B
(`benchmarkBzip2 --parallel 2`, CPUs 14-15, 4 alternating pairs) still gave N=8 vs base 1.0224
(1.0133..1.0553, 4/4) on base64 and 1.0249 (1.0099..1.0688, 4/4) on text, so the larger buffer does
not turn the neutral sequential result into a regression.

## Benchmark commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIBRAPIDARCHIVE_BUILD_TESTS=ON \
    -DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON -DLIBRAPIDARCHIVE_WITH_ISAL=ON
cmake --build build --target benchmarkBzip2 -j 8

# sequential decoder, no output copy; prints per-iteration MB/s and the decoded size
taskset -c 1 build/src/benchmarks/benchmarkBzip2 --iterations 10 --mode discard file.bz2

# parallel reader
taskset -c 1 build/src/benchmarks/benchmarkBzip2 --iterations 5 --parallel 8 file.bz2

# isolated non-reflected CRC throughput for 1/4/8 slices
taskset -c 1 build/src/benchmarks/benchmarkBzip2CRC

# end-to-end A/B for one slice count
cmake -S . -B build -DLIBRAPIDARCHIVE_BZIP2_CRC_SLICES=4
cmake --build build --target benchmarkBzip2 -j 8
```

## Huffman/MTF phase after libsais

After the inverse BWT was replaced with `libsais_unbwt_cyclic` (`2ab882a4`), the Huffman/MTF pass in
`Block::readBlockData` became the largest single phase. Measured with `ibzip2 -v -P 1 -d -c` on the
corpora and cross-checked with the phase timers: for base64 it was about 0.42-0.55 s of a roughly
0.66 s decode, for text about 0.31-0.45 s of a roughly 0.72 s decode. The accepted changes target the
literal path, which dominates both files.

### Accepted

| # | Change | Outcome |
|---|--------|---------|
| 8 | Store the Huffman LUT entry as one `uint32_t` (`length << 16 | symbol`) and add a sentinel-returning `decodePacked` instead of `std::optional<Symbol>`. Replace the block-data MTF with 7-Zip's packed 64-bit list (`Mtf8.h`) so the literal path no longer calls `memmove`. | **Committed.** A/B against `2ab882a4` (8 alternating pairs, CPU 3, `benchmarkBzip2 --iterations 10 --mode discard`, best-of-10 per invocation): base64 median 0.9979 (range 0.9849..1.0073, 3/8 wins), text median 1.0747 (range 0.9886..1.0798, 7/8 wins). Base64 is neutral; text gains about 7%. |
| 9 | Make the packed MTF update branchless with SSE2 for positions < 64: shift all four 16-byte lanes and blend the prefix with a byte mask. The scalar word loop's trip count is the MTF index, which is uniform for base64 and mispredicts on almost every literal. | **Committed.** Incremental A/B against #8 (same method): base64 median 1.2751 (range 1.2622..1.7506, 8/8), text median 1.0166 (range 0.9989..1.0826, 7/8). Combined against `2ab882a4`: base64 median 1.5121 (range 1.3751..1.5660, 8/8), text median 1.1179 (range 1.0688..1.1690, 8/8). Phase timers (min of 5): base64 Huffman 0.419 -> 0.211 s (1.99x), text 0.308 -> 0.256 s (1.21x). `perf stat` over 3 iterations: base64 cycles 10.59e9 -> 7.31e9 and branch-misses 161M -> 22M; text cycles 12.46e9 -> 11.15e9 and branch-misses 155M -> 115M. |

The base64 branch-miss reduction is the mechanism: instructions increased 19% while cycles fell 31%,
so the loop was bound by the data-dependent MTF loop-exit branch, not by issue throughput.

### Rejected for this path

| # | Change | Outcome |
|---|--------|---------|
| 10 | Keep `m_bitBuffer`/`m_bitBufferFree` in caller-owned locals across symbols, synchronizing only on refill and the long-code path (the standalone harness measured 5.21 -> 2.49 ns/symbol). | **Dropped.** In-tree, against the #8 variant (8 alternating pairs): base64 0.967 (range 0.7914..1.4285, 1/8), text 1.028 (range 0.6958..1.0352, 7/8). The standalone result does not translate because the real literal path already has enough independent work (MTF and the run/literal state machine) to hide the memory-resident BitReader state, while the extra live values spill around calls. Before the branchless MTF it was worse: base64 0.868, text 1.043 against the libsais base. |
| 11 | Add a `position < 8` single-word branch before the vector MTF path to exploit the skewed text distribution (74% of text literals have an index below 8). | **Dropped.** Against #9 (8 alternating pairs): base64 0.934 (range 0.8613..1.0508, 1/8), text 1.018 (range 0.9951..1.3663, 7/8). The uniform base64 index makes that branch a 12.5% random event and its mispredictions cost more than the text-side saving. |

### Remaining text ceiling

At this point the text Huffman phase is about 0.25 s while the libsais inverse BWT is about 0.22 s and
RLE+CRC about 0.16 s, both owned by a different branch. Even reducing the Huffman phase to zero would
only reach about 1.6x over the libsais base, so the 2x text target is not reachable by changing only
`readBlockData`. The remaining text branch misses (115M over 4 decodes) are mostly outside the MTF
loop, in libsais and in the run/literal branch. The next experiment inside this scope is a two-symbol
Huffman LUT (one 12-bit lookup yielding two symbols when both codes fit) or a branchless run/literal
classification; both would need to recover a large fraction of the remaining 0.25 s to matter.

## Combined result on `perf/bzip2-fast`

`perf/bzip2-fast` combines the libsais inverse BWT (`2ab882a4`) with the packed Huffman LUT/MTF and the
branchless SSE2 MTF update (`fcbf6ff5`, `df27e9b7`). All numbers below are against the pre-libsais decoder
(`1a565e7d`/`663cb8c7`), with both benchmark binaries rebuilt from their own trees.

Single-core, `benchmarkBzip2 --iterations 10 --parallel 1 --mode discard`, 8 alternating pairs, best of 10
per invocation, CPU 3:

| file | old | new | ratio (median, range) | wins |
|---|---:|---:|---|---:|
| `base64.bin.bz2` | 44.74 MB/s | 93.09 MB/s | **2.077** (1.749..2.137) | 8/8 |
| `text64.bin.bz2` | 86.50 MB/s | 126.15 MB/s | **1.459** (1.452..1.465) | 8/8 |

The base64 single-core target is met. Text is not: after libsais + Huffman/MTF the text phases are Huffman
0.249 s, libsais 0.213 s, RLE+CRC 0.155 s. Even a zero-cost Huffman phase leaves 0.368 s, above the
~0.29 s that 2x text requires, so text 2x needs a fundamentally faster libsais construction/RLE or
multi-core execution, not another Huffman micro-optimization.

For completeness, the same combined binary with 4 worker threads (`taskset -c 3-6`) against the old
single-core decoder (`taskset -c 3`), 6 alternating pairs, best of 5:

| file | old P1 | new P4 | ratio (median, range) | pairs > 2x |
|---|---:|---:|---|---:|
| `text64.bin.bz2` | 84.28 MB/s | 219.39 MB/s | **2.616** (2.569..2.675) | 6/6 |
| `base64.bin.bz2` | 42.98 MB/s | 127.24 MB/s | **2.971** (2.940..3.030) | 6/6 |

The parallel scaling itself is sublinear (the libsais tables add L3 pressure between workers), but the
combined decoder clears 2x end-to-end on both workloads with four threads.

Remaining single-core text ideas, in order: a two-symbol Huffman LUT (must approach 3x on the remaining
Huffman phase), a cheaper libsais construction/reused context, and the 4x RLE/CRC FSM (only ~3-4% total).
A 2-block Huffman/libsais software pipeline would use a second core; `ParallelBZ2Reader` already provides
that shape.

### Negative: interleaving independent blocks (`perf/bzip2-decode-2`)

Interleaving the inverse-BWT chains of 2/4/8 independent 900k blocks in one thread was measured end to end
at 0.657x (text) and 0.700x (base64) on the batch branch. A synthetic pointer-chase probe showed a 1.77x
ceiling for two blocks, but the real decoder adds output buffers and the two `dbuf` tables exceed the
effective shared L3; `perf stat` showed cycles +40%, cache misses +34% and dTLB misses 2.1x at N=2.
`MADV_HUGEPAGE` and a split `uint16_t` table layout were neutral or slower. Do not retry block
interleaving without a cache-blocked traversal or a much smaller per-block working set; libsais keeps one
block's working set and gets the parallelism inside the block instead.

## Parallel bzip2 reader (`perf/bzip2-parallel`)

Base: `perf/bzip2-fast` at `a2a73faf`. All numbers below use
`benchmarkBzip2 --iterations 10 --parallel N --mode discard`, best of 10 per invocation, 4-6 alternating
base/change pairs, `taskset -c 1-N`, with both binaries rebuilt from their own trees. The machine shares its
16 MiB L3, so absolute rates drifted by up to 25% between sessions; the paired ratios are the evidence.

### Accepted

| # | Change | Outcome |
|---|--------|---------|
| P1 | Allocate the 135 KiB `bzip2::Block` on the heap in `BZ2BlockFetcher::decodeBlock` instead of on the worker stack (`950b9a83`). | **Kept.** Base64 ratios 1.0147 / 1.4365 / 1.2676 / 1.2091 (P1/P2/P4/P8), text 0.9956 / 1.0755 / 1.0295 / 1.0184, with 6/6 wins at P2. This turned base64 P2 from ~1.0x into ~1.7x over P1. |
| P2 | Invert the BWT in place (`U == T`) and free the 3.6 MiB LF table after libsais succeeds (`6f271c96`). | **Kept.** Ratios base64 1.0134 / 1.0398 / 1.0401 (P1/P4/P8), text 1.0230 / 1.0722 / 1.0870, 5-6 of 6 wins. Removes 0.9 MiB per worker and another 3.6 MiB before the expansion pass. libsais validates and allocates before writing, so the chase fallback still sees the unmodified input after an error. |
| P3 | Do not zero-initialize `bwtData` and the LF table; both are fully overwritten before they are read (`53ded860`). | **Kept.** Ratios base64 1.0236 / 1.0584 / 1.0572 (P1/P4/P8), text 0.9896 / 1.0900 / 1.1183, with 6/6 wins at P4 and P8. Valgrind memcheck is clean on one- and multi-block files with two workers. |
| P4 | Parse the next block header in the worker instead of constructing a full `bzip2::Block` on the consumer thread (`40b825b8`). | **Kept.** Only the 48 magic bits are needed to detect the EOS block; the full parse allocated the per-block buffers and read all Huffman trees on the serial path. Ratios base64 1.0094 / 1.0165 / 1.0477 (P2/P4/P8), text 1.0159 / 0.9885 / 1.0276. |

Final same-thread-count ratios against `a2a73faf` (4 alternating pairs, best of 10):

| file | P2 | P4 | P8 |
|---|---:|---:|---:|
| `text64.bin.bz2` | 1.1723 (1.131..1.188) | 1.2022 (1.193..1.207) | 1.2121 (1.196..1.262) |
| `base64.bin.bz2` | 1.5600 (1.346..1.600) | 1.3885 (1.366..1.442) | 1.2871 (1.224..1.315) |

Against the pre-libsais single-core reference used for the 2x work (`44.74 MB/s` base64, `86.50 MB/s`
text), the final P8 medians were `244/349 MB/s` in one session (5.45x / 4.03x) and `284/420 MB/s` in a
quieter session (6.35x / 4.85x). The same-session `old-P1` denominator is 10-25% higher than the stored
reference, so a paired old-P1-vs-new-P8 run reports 3.1-3.4x even though the historical-reference ratio is
4.0x or better.

### The worker-stack pathology

Before P1, two workers of one `ParallelBZ2Reader` scaled at ~1.0x on base64 while two independent readers
reached ~1.8x. Per-block timers inside the worker showed that only the Huffman/MTF phase was inflated, from
~8 to ~20 ms per block; libsais and the RLE pass were unchanged. Compiling the same source as `-O2` or moving
the `Block` off the worker stack removed the regression. The likely mechanism is an address-dependent
store/load stall: the `Block`'s low address bits relative to the separately stacked `BitReader` and the hot
MTF/LUT state select a pathological aliasing pattern in the symbol loop. Do not put this object back on the
caller's stack without re-measuring parallel scaling.

### Tried and rejected

| # | Change | Outcome |
|---|--------|---------|
| P5 | Parameterize the `BlockFetcher` cache capacities and test `m_cache` 2/4/8/16 with `m_prefetchCache` P..2P. | **Neutral.** Best case text P8 cache=8/prefetch=16 gave 1.0112 (6/6, 320 -> 328 MB/s); base64 P8 and all P4/P2 settings were within noise or negative. The thread pool already caps in-flight prefetches at P-1, so the caches never grow to their capacity. Reverted. |
| P6 | Skip the consumer-side EOS header read entirely (temporary `SKIP_EOS`). | **Neutral at P2** (58.9 vs 59.7 MB/s). The useful part was moving the cheap magic parse to the worker (P4 above), which helps only at high worker counts. |
| P7 | Hoist `inverseBWTPosition`, `repeatByte` and `repeatCount` into locals across the RLE expansion. | **Neutral.** Base64 P8 1.012, text P8 0.999, P1 1.016, 4 pairs each. The expansion loop is bound by the CRC table-load chain, not by the member stores. Reverted. |
| P8 | Give the main thread and block finder one or two extra CPUs at P8 (mask 1-9 or 1-10 instead of 1-8). | **Shows the remaining serial overhead:** text P8 278 -> 290 -> 298 MB/s and base64 232 -> 237 -> 248 MB/s. The finder and the polling consumer cost about 5-7% at P8. Not fixed; removing them needs a condition-variable handoff or worker-driven prefetching. |

### Remaining bottleneck

The P8 text profile after P1-P4 is approximately: libsais setup 22%, libsais chain decode 18%, Huffman/MTF
27%, RLE/CRC output 16%, with the rest in the bit reader, memmove and scheduling. Removing the CRC entirely
raises P8 by 7.5% (base64) and 5.1% (text, 6/6), which motivated the slicing-by-8 and later PCLMUL CRC
work; both are in `master` now. The PCLMUL path mainly helps high-expansion blocks, where the CRC is
exposed, and is neutral on balanced corpora. The per-worker 3.6 MiB libsais P table remains the scaling
limit: eight workers need ~29 MiB against a shared 16 MiB L3. A per-worker context reuses allocations but
the public context allocates for `SAINT_MAX`; `master` keeps one context per worker anyway because the
reduced page-fault rate wins in practice (`f1190e0a`).

## Final combined branch `perf/bzip2-final`

`perf/bzip2-final` merges `perf/bzip2-parallel` (heap `Block`, in-place libsais, no zero-init, worker
EOS parse) with `perf/bzip2-avx2` (slicing-by-N CRC). All numbers below are against the stored
pre-libsais decoder (`1a565e7d`/`663cb8c7`), with both binaries rebuilt from their own trees.

Single-core, 8 alternating pairs, best of 10, CPU 3:

| file | old | final | ratio (median, range) | wins |
|---|---:|---:|---|---:|
| `base64.bin.bz2` | 38.04 MB/s | 89.67 MB/s | **2.343** (2.323..2.388) | 8/8 |
| `text64.bin.bz2` | 71.89 MB/s | 121.35 MB/s | **1.690** (1.647..1.729) | 8/8 |

Parallel, same-session paired against the old single-core decoder, 6 alternating pairs, best of 5:

| file | old P1 | final P2 | final P4 | final P8 |
|---|---:|---:|---:|---:|
| `base64.bin.bz2` | 36-38 MB/s | 144.0 (**3.84x**) | 198.6 (**5.19x**) | 272.5 (**7.44x**) |
| `text64.bin.bz2` | 70-83 MB/s | 230.4 (**2.76x**) | 322.8 (**3.82x**) | 394.4 (**5.64x**) |

The 4x target is met with 4 threads on base64 and 8 threads on both workloads; text P4 is 3.82x, just
below 4x, and text P2 is 2.76x. On a quiet machine the parallel agent measured final P8 at 398 MB/s text
and 245 MB/s base64.

Same-thread-count comparison with the reference lbzip2 built under `tmp/lbzip2-build` (best of 3,
`taskset` on the same core sets):

| file | P1 | P2 | P4 | P8 |
|---|---:|---:|---:|---:|
| text ours | 117 | 198 | 271 | 398 |
| text lbzip2 | 68 | 125 | 203 | 220 |
| base64 ours | 81 | 142 | 166 | 245 |
| base64 lbzip2 | 35 | 62 | 90 | 94 |

We beat lbzip2 at every thread count by 1.3-2.6x. The widest gain is base64 P2 (142 vs 62).

### Parallel findings

- The largest scaling fix was keeping the 135 KiB `bzip2::Block` off the worker stack. At -O3 its stack
  address aliased the hot BitReader/MTF/LUT state; the Huffman/MTF phase went from 8 to 20 ms per block
  with two workers. Heap allocation restored near-linear scaling (base64 P2 1.56x, P4 1.39x, P8 1.29x
  over the previous final).
- In-place libsais (`U == T`) plus freeing the 3.6 MiB LF table after success and avoiding zero-init of
  the per-block buffers removed 4.5 MiB of per-worker working set.
- Parsing the next EOS header in the worker removed a consumer-thread stall.
- Effective serial fraction P1->P8 is still ~20-24%: block finder + polling consumer, and shared-L3
  pressure from eight 3.6 MiB libsais P tables against a 16 MiB L3. Removing the CRC raises P8 by 7.5%
  (base64) and 5.1% (text), which is why slicing-by-N CRC is kept.

### SIMD block-chase negative

A separate prototype (`tmp/bzip2-simd-batch-report.md`) tested AVX2 gather across 2/4/8 independent
blocks. Gather was 1.5-2.0x slower than the same unrolled scalar interleave and 2.5-3.5x slower than
libsais. The differentiator is TLB: dTLB misses per chase load rise from ~2% at 2 blocks to 20% at 4 and
51% at 8; gather does not reduce the page footprint, and Zen 4 `vpgatherdd` is slower than independent
scalar loads for a latency-bound chain. Prefetching and padded stride were neutral. This closes the
SIMD block-batching direction on this CPU; libsais's intra-block multichain remains the right shape.

### AVX2/AVX-512 MTF negative

AVX2 `vperm2i128`/`vpalignr` was slower than the current SSE2 path (4.38 vs 3.81 ns/update isolated);
AVX-512 VBMI `vpermb` was only ~10% better in the best inlined case and spilled in the real loop. The
MTF update is bound by the 64-byte store-to-load round-trip. Details are in the AVX2 section above.

## Full-core scaling experiments (2026-11-05)

Goal: raise P8/P1 from about 3.25x to 4.0x. Baseline, `benchmarkBzip2 --iterations 10 --mode discard`,
`taskset -c 1-N`, best of 10 in a low-load window: base64 P1 76.2, P8 247.4 (3.25x), P12 283.6 (3.72x),
P16 302.8 (3.97x); text64 P1 115.7, P8 380.6 (3.29x), P12 428.5 (3.70x), P16 426.7 (3.69x).

A P8 `perf record` attributes 47.7% to `readBlockData` (Huffman/MTF/run), 21.0% to
`libsais_unbwt_init_single` (P-table construction), 9.6% to our RLE+CRC `decodeBlock`, 8.2% to
`libsais_unbwt_decode_chains_symbol_rows`, 6.7% to `BitReader::peek2`, and the rest to the histogram
and `memmove`. P8 executes 1.51x the cycles of P1 for the same output (7.78e9 vs 5.16e9), with the same
instruction count (26.2e9), i.e., the loss is memory/clock stalls, not extra work.

All experiments below were behavior-preserving and passed `testBZ2Reader` (215/215),
`testParallelBZ2Reader` (232/232), and `testHuffmanCoding` (3712/3712) unless noted.

| # | Change | Outcome |
|---|--------|---------|
| S1 | Per-worker reuse of the 135 KiB `bzip2::Block` and its 4.5 MiB buffers via `thread_local`, keeping `dbuf` instead of freeing it after libsais. | **Neutral; reverted.** Minor faults fell 25% (base64 P1 16012 -> 10109 per 4 decodes), but cycles and task-clock were unchanged: P1 5.16 -> 5.19e9 (+0.6%), P8 7.78 -> 7.82e9 (+0.5%), task-clock P8 medians 3409 -> 3418 ms. glibc already recycles the mappings; the fault cost was not on the critical path. |
| S2 | 2 MiB-aligned `aligned_alloc` + `MADV_HUGEPAGE` for `dbuf`/`bwtData`. | **Negative; reverted.** THP is not backed in this environment: `/proc/<pid>/smaps_rollup` kept `AnonHugePages: 0` during a run. The advice made the kernel retry promotion on every fault: minor faults 16012 -> 109402 and dTLB-load-misses 0.75e6 -> 3.6e6 per 4 P1 decodes, cycles +10%. |
| S3 | Global `std::counting`-style semaphore around `libsais_unbwt_cyclic`, limits 2/4/6/8, to cap concurrent 3.6 MiB P tables. | **Negative; reverted.** The premise was wrong: instrumented acquire-time concurrency at P8 was 1:18, 2:27, 3:45, 4:41, 5:16, 6:4, 7:1 (152 inversions), i.e. typically 3-4 tables, not 8. A limit of 2 reduced total P8 cycles 14% (6.01 -> 5.17e9) but wall throughput fell (base64 216-226 -> 192-200 MB/s, text 366-386 -> 295-338) because waiting workers idle. Limit 4: P8 wall ratio 0.968 base64 / 0.965 text (1/4 wins); limit 8 at P12/P16: 0.986 / 1.018. |
| S4 | Completion notification: `ThreadPool` notifies a condition variable after each task's future is ready, and `BlockFetcher::get` waits on it instead of polling `wait_for(1ms)`. | **Neutral; reverted.** P8 ratios 0.984 base64 (3/8) and 1.003 text (6/8), later 0.987 (1/6) and 0.993 (2/6). The 1 ms poll was not on the critical path because the prefetch queue is deep enough. |
| S5 | Block-finder pool `ceilDiv(P, 64) = 1` -> 2/4/8. | **Neutral; reverted.** P8 base64 206-228 MB/s and text 295-306 MB/s across all values, no ordering trend. The in-memory `BufferViewFileReader` finder scans far faster than the decoders consume blocks. |
| S6 | Skip the `BlockFinder::get(nextBlockIndex)` infinite wait in `BlockFetcher::get`/`prefetchNewBlocks` for bzip2, which does not use `nextBlockOffset`. | **Neutral; reverted.** P8 0.964 base64 (1/5) and 1.014 text (4/5); the finder is normally already ahead, so the wait was almost never taken. |

### The frequency wall

The all-core clock drop accounts for most of the apparent serial fraction. A dependent-multiply loop
runs 5.664 s on one core and 7.910 s with seven more instances pinned to the other cores (the same
20.7e9 cycles in both, so this is frequency, not work): **1.40x**. During the actual decoders,
`scaling_cur_freq` averages 2.71 GHz for P1 and 1.91 GHz for P8 in the same loaded window (1.42x).
Modelling `T8/T1 = f * (s + (1-s)/8)` with `f = 1.4` gives `s` about 10%, and reaching `T8/T1 = 0.25`
needs `s` about 6.3%. Since 37-82 blocks over 8 workers already costs 7-8% wave quantization, 4x at P8
is not reachable on this APU with this block granularity even if the consumer and finder became free.
P16 reaches about 4x because SMT adds throughput; the same model with the measured P16 numbers gives
`f` about 1.4 and a small remaining serial part.

The measured scaling is therefore close to the hardware limit: eliminating all software serial overhead
would still leave P8/P1 at about 4.0-4.3x only in the most optimistic case, and the remaining lever is
reducing P-table memory traffic (28-bit packing or a smaller inversion working set), which requires
changing libsais. The concurrency experiment also refutes the "eight simultaneous P tables" premise:
the workers are naturally spread across Huffman and libsais phases, so at most 3-4 tables are live.

lbzip2 2.5 at the same thread counts (best of 5, interleaved with ours in the loaded window): ours is
2.3-2.7x faster at every P; e.g. P8 base64 211 vs 87 MB/s, P8 text 310 vs 168 MB/s.

## libsais cache and traffic tuning (2026-11-05)

Goal: reduce per-block memory traffic and cycles so the existing block-level worker pool spends less time
in the all-core low-clock state. No production change survived. All numbers below are relative A/B from
`benchmarkBzip2 --iterations 10 --mode discard`, `taskset -c 1-N`, alternating base and variant, with the
host at load average 5-7 (the gzip agent was running on CPUs 14/15). Absolute rates were therefore low
(P1 63-120 MB/s, P8 200-340 MB/s); the ratios and the `perf stat` work counters are the evidence.

The branch build is byte-identical to the `perf/bzip2-final` benchmark binary (`md5 b29c6478...`), so the
baseline is the same code, not a re-implementation.

### 1. Chain count and chunk size (negative)

`UNBWT_CHAINS` (24) and `UNBWT_CHUNK_SIZE` (16320) are plain `#define`s in the libsais submodule, not
overridable from the command line. They were wrapped in `#ifndef` for the sweep; the guard alone produces
a byte-identical binary at the default values. The exact patch is
`tmp/libsais-chains-chunk-guard.patch`. Each variant was built by changing the default and rebuilding only
`libsais` plus `benchmarkBzip2`.

| variant | P1 base64 | P1 text | P8 base64 | P8 text | verdict |
|---|---:|---:|---:|---:|---|
| chains 8 | 0.904 | 0.932 | 0.968 | 0.885 | clear regression |
| chains 16 | 1.019 | 1.013 | 1.011 | 0.987 | neutral |
| chains 32 | 1.004 | 0.984 | 1.047 | 1.023 | neutral after cycles |
| chains 48 | 1.043 | 1.028 | 1.023 | 0.993 | neutral, noisy |
| chunk 8192 | 0.955 | 0.917 | 0.933 | 0.937 | clear regression |
| chunk 32768 | 0.961 | 0.920 | 0.984 | 0.955 | clear regression |

`chains 32` was the only variant with a P8 throughput signal, so it was re-measured with `perf stat` over
three alternating pairs at P8: cycles ratio 1.0097 base64 and 0.9917 text, instructions 1.0004 and 1.0020,
dTLB-load-misses 1.048 and 1.053. The throughput gain was clock/load noise, not less work; more chains also
add TLB pressure. `chains 24` and `chunk 16320` remain the local optimum. The P-table construction, not the
chain decode, dominates libsais time at P8 (21% init versus 8.2% decode in the earlier profile), so this
knob cannot move the total much.

### 2. Free `bucket2` and the unused `fastbits` tail after init (negative)

For a 900k block the symbol-rows path reads only `first_symbols` and `symbol_ends`, both at the start of
`fastbits`; `bucket2` (256 KiB) and the rest of `fastbits` (about 228 KiB) are dead after the setup. A local
patch called `madvise(MADV_DONTNEED)` on both regions between `libsais_unbwt_init` and
`libsais_unbwt_decode_split`. Throughput ratio at P8 was 0.974 base64 and 0.963 text (1/4 wins).
`perf stat` over three P8 pairs: cycles 1.0025 base64 and 1.0261 text, cache-misses 1.024 and 1.016,
dTLB-load-misses 1.465 and 1.396. Dropping the pages does not remove the cache lines from the active decode
working set and it adds TLB shootdowns; the pages were already cold. Reverted.

### 3. Per-worker `libsais_unbwt_cyclic_ctx` reuse (negative)

A `thread_local` `libsais_unbwt_create_ctx()` was used for every block instead of the per-call context in
`bzip2.hpp`. This removes the per-block allocation of the chain/slack/link work block (about 450 KiB) but
the public context allocates for `SAINT_MAX` (about 2.4 MiB: 64-slot slack, large links and chain arrays),
and the thread-local copy stays resident on every worker between blocks. Throughput at P8 was 0.984 base64
and 0.987 text. `perf stat` over three pairs: P8 cycles 1.041 base64 and 0.998 text, cache-misses 1.051 and
0.999, dTLB-load-misses 1.61 and 1.05; P1 cycles 0.986 and 1.014. The larger resident footprint hurts
base64 at P8 and the P1 allocation saving is inside the noise. Reverted.

### 4. 28-bit packed P with side marks: bounded and not a win

The decode table is a 32-bit entry `(next_position << 8) | second_byte`, using 20 bits for the position and
8 for the byte. Packing it to 28 bits shrinks a 900k table from 3.6 to 3.15 MiB (12.5%). A standalone
microbenchmark (`tmp/lf-table-microbench.cpp`) decoded the same random permutation with the same 24-chain,
two-output-bytes, one-prefetch loop for both layouts, including the unaligned 64-bit load and a shift by
`bit & 7` for the packed read. Five alternating pairs, 100 rounds, checksums equal:

| threads | 32-bit ns/step | 28-bit ns/step | ratio (32/28) |
|---:|---:|---:|---:|
| 1 | 20.04 | 21.15 | 1.022 |
| 4 | 31.75 | 35.57 | 0.891 |
| 8 | 75.07 | 74.17 | 1.024 |

The ranges were 0.74-1.21, so the packed read is within noise at P1/P8 and possibly slower at P4; the extra
shift/AND and the 11% line-straddling 8-byte loads offset the smaller footprint. The end-to-end bound is
also decisive: the chain decode is only 8.2% of the P8 profile (and roughly a fifth of P1), so even a 10%
decode win is 0.8-2.5% end-to-end, below the 5% gate. P-table construction (21% at P8) still writes 32-bit
entries; packing during the random bucket scatter would add a read-modify-write per entry. A narrower
20-bit next-position table plus a `T[next-1]` lookup would save 37.5% of P but adds a dependent load and the
same construction problem, and remains bounded by the decode share. Not implemented.

### 5. Block scheduling (not retried)

The prior loop already measured per-worker `Block`/buffer reuse, huge pages, libsais concurrency gates,
completion notification, block-finder parallelism, oversubscription, and skipping the next-offset wait as
neutral or worse. The instrumented libsais concurrency was 3-4 tables, not 8, so the L3-capacity premise of
another gate or a deeper queue is already refuted. No cheap scheduling change has a plausible mechanism
left within the block-level design.

### Conclusion

None of the remaining per-block cache or traffic levers produces a repeatable end-to-end win. The P8
scaling (P8/P1 3.25 base64, 3.29 text) is set by the 1.40x all-core clock drop, 7-8% wave quantization, and
about 10% serial fraction, not by a fixable cache bottleneck: 3-4 live 3.6 MiB tables already fit the
16 MiB L3, and the dominant phase at P8 is Huffman/MTF (47.7%) followed by libsais construction (21%),
neither of which is a P-footprint problem. The existing block-level parallelization has no remaining
software headroom above 5% on this APU within the current constraints. The only measured large cycle
reduction (the limit-2 concurrency gate, -14% cycles) comes from idling workers and lowers throughput.

Remaining options are outside the current design constraints: 4 blocks x 2 libsais OpenMP threads
(intra-block parallelism, explicitly excluded), a two-symbol Huffman LUT (already measured neutral), or a
different inversion algorithm (the GPU sort + list-ranking needs 5x the working set per block and is not
viable on CPU).


## gzip SIMD ideas applied to bzip2 (2026-11-05)

Base: `perf/bzip2-final` at `8dfb9e5e`, branch `perf/bzip2-simd`. The A/B protocol is
`benchmarkBzip2 --iterations 10 --mode discard`, CPU 13, both binaries rebuilt from their own trees, 6
alternating base/change pairs, best of 10 per invocation. The host was shared with other agents, so the
absolute rates are low; the paired ratios and the deterministic instruction counts are the evidence.

### Accepted: vectorized RLE expansion scan (`73f335c3`)

The post-libsais expansion scanned the inverse-BWT bytes one at a time, loading the next byte for every
literal and short-circuiting the four-byte run test on the first mismatch. The committed change loads 16
bytes and compares four shifted copies with SSE2, finds the first run start with `movemask` + `ctz`, and
copies the literal prefix with one 16-byte load and store. Runs and the last unaligned bytes keep the
scalar path. The compiler already vectorizes the repeat fill itself (16-byte stores above length 14), so
this only changes the scan.

| file | base MB/s | change MB/s | ratio median (range, wins) |
|---|---:|---:|---|
| `base64.bin.bz2` | 77.98 | 84.95 | **1.036** (0.650..1.215, 5/6) |
| `text64.bin.bz2` | 97.97 | 115.20 | **1.153** (1.048..1.324, 6/6) |

A first base64 session mixed in two low outliers (base 29.78, change 32.42) and gave 1.001 (3/6).
The final run above has one low change outlier (48.82 MB/s) and is 5/6; excluding it the median is
1.043. Text is 6/6 in both sessions. The code was re-measured after the ASAN fix described below. Isolated phase, `ibzip2 -v -P 1`, CPU 13, min/median of 5 alternating
runs, `decodeBlock` timer (RLE scan + slicing-by-8 CRC + output):

| file | base | change | speedup |
|---|---:|---:|---:|
| `base64.bin.bz2` | 0.0574 / 0.0586 s | 0.0196 / 0.0204 s | about 2.9x |
| `text64.bin.bz2` | 0.1504 / 0.1507 s | 0.0758 / 0.0815 s | about 1.9x |

`testBZ2Reader` 215/215, `testParallelBZ2Reader` 232/232, `testHuffmanCoding` 3712/3712; decoded md5
matches the corpora. ASAN+UBSAN `testBZ2Reader` initially found a one-byte overflow when the 16-byte loop
filled the caller buffer exactly; the scalar tail now rechecks the bound before writing. The fixed tip
passes all three sanitizer tests and decodes both corpora with md5 equal. The phase is on the critical path, which is why the win survives end to end, unlike
the CRC experiment below.

### Negative: AVX-512 Huffman symbol extraction

Hypothesis: decode 4/8 Huffman symbols from one wide peek with `vpmultishiftqb` + `vpermb`, as the gzip
literal decoder does, and process the MTF/run state afterwards. A probe over all groups found:

| file | symbols | runs | dominant-length literals | eligible (coverage >= 0.90, length <= 6) |
|---|---:|---:|---:|---:|
| `base64` | 33.55M | 0.51M | 31.87M (95%) | 33.44M |
| `text64` | 27.63M | 11.74M | 6.69M | **0** |

Base64 has a single 6-bit dominant literal length covering more than 90% of every LUT, so the gate opens
for almost all symbols. Text fails the gate completely because runs occupy the LUT slots. The experiment
therefore targeted base64. The bzip2 table maps the 6-bit code to the MTF position (`symbol - 1`) and uses
`0xFF` for runs, EOB and non-dominant codes; a group with a sentinel still emits its dominant prefix in
bulk before the scalar step.

The experiment binary compiled the whole target with `-mavx512f -mavx512bw -mavx512vbmi` so the
intrinsics inline; the control used the same flags with the batch path disabled. This isolates the batch
decoder from the compiler flags. It was **15% slower** on base64:

| file | control MB/s | SIMD MB/s | ratio median (range, wins) |
|---|---:|---:|---|
| `base64.bin.bz2` | 58.48 | 49.00 | **0.849** (0.588..0.953, 0/6) |

The full experimental patch is preserved in `tmp/bzip2-simd-huffman.diff`.

Why it loses: the Huffman LUT load is already overlapped with the serial MTF chain. Decoding eight symbols
first and then walking them serially exposes the full 64-byte store-to-load round trip of the packed MTF
list, which is the same mechanism that made the earlier grouped-50-symbol experiment regress. Removing the
lookup chain does not help when the lookup was not on the critical path. The two-symbol LUT negative and
this result agree; no local variant of the batch decoder is worth pursuing.

### Negative: SIMD RLE fill and four-literal scan

The RLE composition was measured with a probe (one iteration, all blocks):

| file | literal bytes | runs | run bytes | runs <= 8 | 9..16 | 17..32 | 33..64 | 65..128 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `base64` | 33.55M | 121 | 487 | 121 | 0 | 0 | 0 | 0 |
| `text64` | 61.89M | 1.33M | 11.81M | 906k | 325k | 80k | 15k | 2k |

The average text run is 8.9 bytes, and 98.8% of runs are at most 32 bytes. GCC already emits a 16-byte
vector fill for the repeat loop (the `cmp $0xe` threshold and `movups` loop are visible in the
disassembly), so a hand-written long-run fill cannot improve it. The scan is the only remaining part,
and the existing four-literal batch in the "Tried and rejected" table (#14) already regressed because the
scalar four-byte test short-circuits after one mismatch. The 16-byte scan that was kept is a different
shape: it copies the literal prefix in bulk and only pays the per-run path at a run start, which is why it
wins.

### Historical end-to-end negative: PCLMUL CRC via ISA-L

Status: the negative below applies to normal text/base64 corpora, where the CRC is hidden. A
hand-written PCLMUL/VPCLMUL kernel without an ISA-L dependency was later committed (`3ea75dba`) and
integrated with exact-size allocation and last-block prefetch; it is a large win on high-expansion data
(zeros 4.0x, `highratio` 1.9x). See `extreme-case-results.md`.

The non-reflected bzip2 CRC cannot use the hardware CRC32 instruction, so the current path uses
slicing-by-8 tables. ISA-L's `crc32_ieee_avx2` implements the normal (non-reflected) polynomial with
PCLMUL. Isolated, 128 MiB, CPU 13, best of 7: `crc32_ieee_avx2` **25.9 GB/s** and
`crc32_ieee_by16_10` 23.4 GB/s, versus 2.19 GB/s for slicing-by-8. Both match the byte-wise normal CRC.

The drop-in is `crc = ~crc32_ieee_avx2( ~crc, data, size )` because ISA-L returns
`~rawCRC( init = ~seed, data )` while the bzip2 loop keeps the raw value. It was correct on the corpora
and all tests. The `decodeBlock` phase fell substantially:

| file | slicing-by-8 | PCLMUL | speedup |
|---|---:|---:|---:|
| `base64` | 0.0246..0.0395 s | 0.0064..0.0104 s | 3-4x |
| `text64` | 0.0817..0.1075 s | 0.0510..0.0818 s | about 1.6x |

End to end it was **not** a win. Text64 instructions fell 5.0% (11.79e9 -> 11.20e9) and were stable, but
cycles were within noise (base 5.00..5.50e9, change 5.11..5.19e9) and the alternating wall-time ratios
were 1.047 (4/6, one contaminated pair) in one session and 0.972 in another with a strong frequency ramp.
Base64 was 0.991 (2/6). The CRC work was hidden under the inverse-BWT/MTF stalls, the same pattern as the
"skip CRC" experiment (#5); removing instructions did not remove the critical path on those corpora. The
ISA-L drop-in also required an optional dependency and a non-public variant symbol, so it was not committed
at that point. The patch is in `tmp/bzip2-simd-crc-isal.diff`. The later self-contained kernel avoids both
problems and is exposed on the extreme cases instead of normal corpora.

### Not retried: local bit window

The gzip `UnreadBitsView` removes per-group `BitReader` state updates. The bzip2 `BitReader` hoist was
already tested (#8/#10): GCC/LTO keeps the state in registers and a fresh build was byte-identical, so the
in-tree win was zero. The SIMD Huffman experiment above also consumed a 48-bit peek per batch, which is the
same local-window shape, and was negative for the same reason. No new variant is warranted.
