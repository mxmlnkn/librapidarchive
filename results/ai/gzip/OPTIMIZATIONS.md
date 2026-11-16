# OPTIMIZATIONS.md

A summary of the single-threaded inflate performance work in `librapidarchive`: the gap to measure
against, the confirmed bottleneck, everything that was tried with its outcome, the techniques the
reference libraries use, and the ranked open ideas. It was kept current while the gzip performance
branch was active.

Status: most of this work is **not** merged into `master`. The 8x, AVX-512/AVX2/NEON literal decoders,
the local bit window, and the PCLMUL CRC live on `perf/gzip-inflate8x`, `perf/gzip-stream8x`,
`perf/gzip-mixed`, and `perf/arm-neon`. `master` contains the earlier compile-time peek count, the
true-size fixed/distance LUTs, the contiguous backreference fast path, and the one-chunk-per-worker
pipeline change. The commit hashes, test counts, and absolute rates below are branch-time evidence.

The related scoped reviews (`review-huffman-decoders.md`, `review-inflate-lz-backreferences.md`) were
kept on the local `docs/performance-reports` branch and are not part of this tree; this file is the
top-level digest.

## Target and current state

The custom inflate in `src/rapidgzip/gzip/deflate.hpp` is about **2x slower than ISA-L / libdeflate**
in raw single-threaded decode. References and prior measurements:

- Paper (HPDC'23), `results/paper/sections/6 - Evaluation.tex`: single-thread rapidgzip 0.153 GB/s vs
  bgzip 0.298 (1.9x) vs igzip 0.656 (4.3x) on a bounded workload; `:392-393` calls the gap "the custom
  deflate implementation" and lists further optimization as future work.
- In-source Huffman-decoder shootout `deflate.hpp:71-93` (parallel) and `:137-165` (sequential): ISA-L
  decoder `879 MB/s` vs `ShortBitsCached` LUT=11 `367 MB/s` on FASTQ single-thread (~2.4x); multi-thread
  is parity on FASTQ but still ~30-40% behind on base64.
- `benchmarkInflate.cpp` (aarch64): WITH_ISAL=OFF vs ON: Zeros 643->1142, Random 685->1240,
  Random Base64 121->167, Fixed-Huff Base64 111->121, Random Backrefs 445->600 MB/s. So ISA-L is
  ~1.7-1.8x on literals, ~1.35x on backrefs.
- `benchmarkHuffmanCoding` (this repo): all in-tree Huffman decoders are within ~15% of each other on
  raw decode throughput alone, so **the gap is NOT raw table lookup**; it is the per-symbol and
  per-backreference overhead around the table.

On this machine (Ryzen, load fluctuates due to CI compile jobs), `benchmarkInflate`'s random-backrefs
case is ~2490 MB/s after the September 2026 `resolveBackreference` fix. Measurements on this machine have
+/-5-10% noise; trust averages over 3+ runs or profile with `perf`.

A later 2026-11 session on a busier machine measured a lower absolute baseline but the same structure:
random-backrefs midpoints around 1.9-2.0 GB/s for both build flavors before row 12, and ~2.2 GB/s (ISA-L)
/ ~2.3 GB/s (no-ISA-L) after it. Use the alternating A/B method, not absolute numbers across days.

## Confirmed bottleneck: per-symbol and per-backreference overhead, not the table

Three independent pointers converge on this:

1. All Huffman variants decode within ~15% of each other (`benchmarkHuffmanCoding`), so the two-level
   table choice is a small slice.
2. The paper (`deflate.hpp` header, `results/benchmarks/`) reports the BitReader is not the bottleneck
   even at compression ratio 1 (`6 - Evaluation.tex:20-27`), and `peek<32>` measures fastest
   (`HuffmanCodingISAL.hpp:112-115`).
3. The remaining per-backreference work is: decode length symbol, read length extra bits, decode
   distance symbol, read distance extra bits, copy `length` bytes. Two of the five steps are
   **runtime-counted `bitReader.read(n)` calls** (`getLength`, `getDistance`) - the only non-templated
   reads in the hot loop.

This is exactly the spot the reference libraries remove. libdeflate and ISA-L/zlib-ng all **precompute
the extra-bit count into the Huffman table entry and consume code+extra bits in ONE bitbuffer shift**,
then re-extract the extra bits from the saved window (libdeflate) or from the packed literal run
(ISA-L). See "Reference techniques" below.

## What we have tried, with outcome

| # | Change | Outcome | When |
|---|--------|---------|------|
| 1 | `resolveBackreference` bulk copies: distance==1 fill (memset/std::fill), two-piece `memcpy` for source wrap-around, doubling self-copy for overlap. | +~14% on `benchmarkInflate` random-backrefs (3-run A/B, disjoint CIs); RLE/literal cases within noise (output memory-bound). Commit "[performance] Replace per-byte LZ match-copy loops with bulk copies". | 2026-09 |
| 2 | Literal-burst widening in `readInternalCompressedMultiCached`: write `symbolCount-1` literals with one `memcpy`. | Regression on literal-heavy base64 (~420 -> ~330 MB/s), likely the per-decode `std::array` init + extra branch. Reverted; documented in `review-inflate-lz-backreferences.md`. | 2026-09 |
| 3 | `HuffmanCodingDoubleLiteralCached` (double caching) | Faster single-thread, slower multi-thread (128 KiB LUT exceeds shared L1). Not selected. See `deflate.hpp:168-173`. | historical |
| 4 | `HuffmanCodingShortBitsCached` LUT sweep | LUT=10/11 optimal in this repo; 12 loses (L1). Selected. `deflate.hpp:137-165`. | historical |
| 5 | Distance decoder: own `HuffmanCodingReversedBitsCached` beats ISA-L by 1-2% (table too small for caching to pay). Selected. `deflate.hpp:349`. | historical | |
| 6 | Precode decoder: `ReversedBitsCachedCompressed` selected; precode table **construction** dominates `readDynamicHuffmanCoding` (~58% of header cost). | `deflate.hpp:308`. | historical |
| 7 | Compile-time peek count in `ShortBitsCached::decode`: `peek<LUT_BITS_COUNT>()` instead of the runtime `peek(m_lutBitsCount)` (the member is only smaller when the alphabet's maximum code length is below the LUT size). | +8% on the literal-heavy base64 case and ~+8% on backrefs (custom no-ISA-L path, `benchmarkInflate`, 5 interleaved runs in both orders, disjoint ranges on base64); direct application of libdeflate's "constant bit consumption" pattern to the per-symbol peek. Commit "[performance] Use a compile-time peek count in ShortBitsCached::decode". | 2026-09 |
| 8 | Standalone `getLength` LUT slice (precomputed extra-bits count + base instead of recompute/divide) | Sub-measurement-noise; reverted, folded into idea #1 (merged extra-bits consume). See "Tried and reverted" below. | 2026-09 |
| 9 | Compile-time peek + true-size LUT in the `ReversedBitsCached` family: `decode()` peeks `MAX_CODE_LENGTH` when the tree's actual max equals it (distance decode), and `FixedHuffmanCoding` uses a 9-bit LUT (fixed tree max is 9; LUT 128 Ki -> 2 KiB, binary 126 KiB smaller). | no-ISA-L custom path, 10 alternately-run base/opt pairs (load-drift-cancelling): random-backrefs faster in 9/10 pairs, fixed-Huffman in ~7/10; literal/memory-bound within noise. Behavior identical. Commit "[performance] Compile-time peek and true-size LUT for fixed/distance". | 2026-09 |
| 10 | Decode fixed-Huffman blocks with the batched ISAL multi-literal loop (static `createFixedHCISAL`), instead of the single-symbol `ReversedBitsCached` path. | ISA-L build, benchmarkInflate, 6 alternating pairs on a quiet machine: fixed-Huffman base64 ~166 -> ~297 MB/s (+79%, disjoint ranges); no change elsewhere. Non-ISA-L build unchanged. Commit "[performance] Decode fixed-Huffman blocks with the batched ISAL loop". | 2026-09 |
| 11 | Merged extra-bits consume for the distance path: `decodeWithoutConsuming<28>` peeks code+extra in one window, one seek. | **Regression** on no-ISA-L backrefs (~1380 -> ~1210 MB/s, 5-6/6 pairs): the wider peek<28> forces more frequent bit-buffer refills, and the optional/tuple return does not optimize as well. Reverted. Conclusion: merged-extra only pays off together with a caller-side local bit window (idea #2/libeeflate's local `bitbuf`), not on its own against the streaming BitReader. See "Tried and reverted". | 2026-09 |
| 12 | What row 1 called "bulk copies" is only partially present on this branch: `resolveBackreference` has the contiguous non-overlap `memcpy` and the `distance == 1` fill, but **the source-wrap case (distance > `m_windowPosition`, length <= distance) still used the per-byte loop**. Replace it with two `memcpy`s over the circular buffer boundary when the source range is disjoint from the destination (`length <= window.size() - distance`). | +12.9% on `benchmarkInflate` random-backreferences (ISA-L) and +16.8% (no-ISA-L), 6 alternating pairs each, all pairs positive. Note that `distance > m_windowPosition` is a coarse partition: it includes high-start sources that are still contiguous, so the direct source-contiguity test is `offset + length <= window.size()`, not `distance <= m_windowPosition`. Commit "[performance] Copy wrapped non-overlapping matches with two memcpys". Superseded by row 32: the contiguous-only fast path keeps the high-start win, while the two-memcpy wrap branch was dropped. | 2026-11 |
| 13 | Overlap (`length > distance`): copy one period, then double with `memcpy` (libdeflate shape). | **Regression** -8.6% on the new overlapping-backreferences benchmark case (6 pairs, mean 3133 -> 2863 MB/s). Small runtime-size `memcpy` calls cost more than the existing per-byte repetition loop, which the compiler handles well with store-to-load forwarding. Reverted. | 2026-11 |
| 14 | Overlap: replicate one period to a `uint64_t` and store whole 8-byte words (no libc calls). | **Still slower**: -4.4% on the overlap case (6 pairs). The per-byte loop is ~1 cycle/byte and hard to beat for the 2..31 byte periods used here. Reverted. | 2026-11 |
| 15 | `Block::getDistance`: replace the runtime `bitReader.read(extraBitsCount)` with a `switch` over counts 1..13 calling `read<N>()` (constant masks). | **Regression** -14.6% on random-backreferences (all pairs negative). The compiler's current runtime read is better than a jump table plus constant-mask variants. Reverted. | 2026-11 |
| 16 | Keep `m_windowPosition` in a local across a decoded literal batch in `readInternalCompressedMultiCached`, expecting the `uint8_t` store to force an aliasing reload of the member. | **Regression** -5.6% on base64 HUFFMAN_ONLY. The compiler already keeps the position effective; the surrounding code got worse. Reverted. | 2026-11 |
| 17 | Replace the `std::optional<size_t> availableBitsCount` in `HuffmanCodingISAL::decode` with a sentinel `size_t` to shorten the per-symbol EOF check. | No win (-2.8% mean, one noisy outlier); the optional's engaged test was already hoisted. Reverted. | 2026-11 |
| 18 | Direct packed multi-literal store in `readInternalCompressedMultiCached`: for `!containsMarkerBytes` and `symbolCount > 1`, store the guaranteed leading literals with one unaligned 2/3-byte store, leave a possible trailing length/EOB to the generic path, and wrap the window position once. | **Regression** on the target: base64 HUFFMAN_ONLY 423 -> 398 MB/s (-6%, 0/6 pairs), fixed-Huffman 313 -> 326 (+4%, layout only: the fixed tree produces no packed pairs at the 12-bit short table). Base `ab027b2` and `perf-dynamic-noinline` also lost 3-5% on HUFFMAN_ONLY. A packed entry may end in a length/EOB symbol, so the store cannot simply write `symbolCount` bytes. Reverted. | 2026-11 |
| 19 | Code-layout isolation for the fixed path: explicit `noinline` on the multi-cached loop, and separate template instantiations for fixed and dynamic blocks. | Neutral / no win: HUFFMAN_ONLY 431 -> 429 (separate instantiations) and 426 -> 418 (noinline), fixed within noise, backrefs moved +/-3-5% with overlapping ranges. The post-`c571199` clean build already measures HUFF ~425 and fixed ~310 MB/s here, above the reporting PC's target, so there is no layout penalty to recover on this machine. No code change. | 2026-11 |
| 20 | ISA-L-style 16-byte sliding overlap copy: reload/doubling from the source and 16-byte stores, guarded by 64 B of destination slack. | **Regression** -5.8% on overlapping-backreferences over 10 pairs (2/10 wins); the existing per-byte repetition loop's store-to-load forwarding is faster, as rows 13/14 already indicated. Reverted. | 2026-11 |
| 21 | Build `ReversedBitsCached` tables by canonical doubling: fill one entry per code, double with `memcpy` after each length level, and skip zeroing for complete codes. Incomplete and single-symbol codes still zero the table first; the constexpr fixed table keeps the strided fill. At runtime this only affects the per-block distance table, because literal tables use ISA-L or `ShortBitsCached` and the fixed table is static. | **Win**: `benchmarkHuffmanCoding` construction for a max-15 mixed tree drops 10.6 -> 1.9 us (~5x); a 32 MiB stream with 8192 small blocks decodes 120 -> 113 ms (~6%, 6/7 alternating pairs). Per-symbol decode is unchanged. Committed as "[performance] Build ReversedBitsCached tables by doubling". | 2026-11 |
| 22 | Marker-phase bulk copies: `std::fill` for `distance == 1`, two-piece source-wrap `memcpy`, and a trailing marker scan in the `uint16_t` window. | Within noise: 64 KiB chunk parallel decode 0.104 -> 0.103 s (7/9 pairs), 256 KiB chunks 1.002. The marker phase is too small in these workloads. Reverted. | 2026-11 |
| 23 | Root 9/10 + lazy-subtable decoder for the no-ISA-L litlen path. | Not implemented: the measured ceiling is below the current decoder. On the long-code-heavy distribution (all lengths 1-15), the full 15-bit `ReversedBitsCached` decodes a fixed trace in 2709 us vs 2656 us for `ShortBitsCached@11` with its bit-by-bit fallback, so a subtable decoder cannot beat the existing short LUT. libdeflate's root-11 litlen table is 2342 entries (9.4 KB) plus an 8 KB root, not smaller than our 8 KB short LUT. Skip. | 2026-11 |
| 24 | Slack-guaranteed wrap-free caller loop: split each `Block::read` into a chunk that cannot reach the window edge (no `%=` in `appendToWindow`) plus a short wrapping chunk at the edge. | **Regression**: ISA-L HUFFMAN_ONLY base64 425 -> 415 MB/s (-2.3%), no-ISA-L HUFFMAN_ONLY 156.6 -> 150.9 (-3.7%), no-ISA-L backreferences 2355 -> 2265 (-3.8%), 10 alternating pairs. The per-symbol wrap is a single AND with a power-of-two window size, while the extra dispatcher and the short wrapping chunk per window cost more. Reverted. | 2026-11 |
| 25 | `__restrict__` on the `BitReader` parameter of both decode loops (window and tables are members of the same `Block`, so the input cannot alias them). | Not measurable: ISA-L HUFFMAN_ONLY +0.8%, fixed -0.7%, backrefs +1.6%, all ranges overlapping; no-ISA-L fixed -1.8%, overlapping-backreferences +4% but with 300 MB/s spread. Reverted rather than claiming noise as a win. | 2026-11 |
| 26 | Merged extra-bit consumption with a new, non-persistent 16-bit caller window: `HuffmanCodingShortBitsCached::decodeWithWindow` returns the LUT peek and the caller takes deflate length extra bits from it, with no window state across symbols. | **Regression** on the no-ISA-L path: HUFFMAN_ONLY base64 156.6 -> 140 MB/s (-11%, 10 pairs), backreferences unchanged. The wider peek plus the optional/branch per literal costs more than the second `BitReader::read` that it removes. Reverted. Together with rows 11 and the 2026-09 fused loop, this closes caller-side bit windows for merged extra bits. | 2026-11 |
| 27 | Always peek the full template table width (`peek<MAX_CODE_LENGTH>`) in `HuffmanCodingReversedBitsCached` to get a compile-time index mask. | **Incorrect**: the construction only fills up to the tree's actual maximum code length, so a wider peek indexes zero entries for an incomplete tree and `zeros.deflate` fails. Padding every table to the template width would add a full doubling pass per distance-table build. Reverted. | 2026-11 |
| 28 | Header microbenchmark (task 4): `rapidgzip --analyze` on a 128 MiB HUFFMAN_ONLY stream with 4097 32 KiB blocks separates per-block header cost. | `readDynamicHuffmanCoding` is 6.75% of the decode time; 91% of that is ISA-L literal-table construction, 2.4% is precode construction (0.16% of the total). Precode or block-finder candidate work has no measurable headroom on this workload. No code change. | 2026-11 |
| 29 | Last-value Huffman code-length cache: skip `initializeFromLengths` when a block repeats the previous literal/distance code lengths. | **No hit rate**: even on the stationary HUFFMAN_ONLY stream, consecutive blocks alternate between a few code-length variants (debug first differences at indices 55-78, lengths 6<->7), so every block missed. A multi-entry cache would have to duplicate the 16 KiB ISA-L short table and the 64 KiB distance table per `Block`. Reverted. | 2026-11 |
| 30 | Extend the disjoint non-overlap `memcpy` path in `resolveBackreference` to a wrapping destination (two `memcpy`s; the source stays contiguous because both arcs cannot wrap at the same time). | **No win / slight regression**: `benchmarkInflate` random-backreferences, three binaries (base `91c415aa`, destination-wrap `11b2fb18`, final `5094f408`), 5 rotating rounds pinned to one core on a battery-powered `powersave` machine (load 2.4-3.0). Destination-wrap-only was 3.1% slower in all 5 rounds (median ratio 1.031; range 1.003-1.056). Destination wrap needs `m_windowPosition + length > 32768`, probability ~`length / 32768` (~0.4% for the generated 4-255 byte matches), while the extra branch runs for every backreference. The row 12 win is different: the old fast path only accepted `distance <= m_windowPosition`, so the common high-start half (~50%) fell into the per-byte loop, including contiguous sources that could use one `memcpy`. The direct source-contiguity test is `offset + length <= window.size()`; actual source-end wrap is only ~0.4%. Dropped from `security-audit`; measured commits preserved on `backup/security-audit-pre-drop-ab`. | 2026-11 |
| 31 | Extend the single-symbol fill (`memset` for `uint8_t`, `std::fill_n` for `uint16_t`, wrapping destination included) to all `nToCopyPerRepeat == 1` cases. | **Neutral**: zeros case (128 MiB, `distance == 1` runs) base 0.0444 s, final 0.0440 s (-0.9% combined), mid -> final +0.2% (i.e. no improvement from the fill-only step). The random-backreferences case cannot exercise it (`length >= 4`, so `nToCopyPerRepeat > 1`). The old code already used `memset` for non-wrapping `distance == 1`; the new code only covers the wrapping subset (~0.4%) and `uint16_t` marker windows, which the sequential benchmark does not use. Dropped from `security-audit`; measured commits preserved on `backup/security-audit-pre-drop-ab`. | 2026-11 |
| 32 | Split the row 12 source-wrap change: first use only the relaxed contiguous check (one `memcpy` for every non-wrapping source, fall back to the per-byte loop when the source crosses the buffer end), then add the two-memcpy wrap branch as a separate commit. | **Contiguous-only wins**: `benchmarkInflate` random-backreferences, contiguous-only vs contiguous + two-memcpy, 5 rotating rounds pinned to one core (power state Full, load ~2.5): two-memcpy/contiguous paired median 1.027 (2.7% slower), 4/5 rounds slower; controls within noise (zeros 0.992, base64 ~0.99, random-data 0.970 but unaffected by the branch). Actual source-end wrap is ~`length / 32768` (~0.4%), so the extra branch on every backreference costs more than it saves. Contiguous-only kept in `security-audit` (`dd562aa2`); the two-memcpy experiment is on `experiment/two-memcpy-wrap`. | 2026-11 |
| 33 | Wide-peek literal-run batching in the plain custom loop: decode a run of literals from one `peek<56>`, keep the window position local, and stop before length/EOB and long codes. | **Literal-heavy win, text regression**: `benchmarkCustomInflate` base64 168 -> 399 MB/s (2.38x, 8/8 alternating pairs), but callgrind on `text6m.gz` grew 190.7M -> 215.6M instructions (+13%) because short text runs pay a 56-bit peek each. Committed as `55cd7126`. | 2026-11 |
| 34 | Gate the literal-run decoder on dominant-code coverage (>=90% of LUT slots) plus a 4-literal scalar run, and dispatch once per block to a scalar or batched loop instantiation. | **Fixes the text regression and keeps the base64 win**: text6m 215.6M -> 197.2M instructions (base 190.7M; the batched loop is never entered for text). Committed as `8f3a9877`. | 2026-11 |
| 35 | Constant-stride loop for the dominant literal length: four independent table loads per iteration from a fixed stride, breaking out on a non-dominant code. | **Win**: tip -> final base64 window-full 1.53x (8/8 pairs). Against base in paired all-mode runs (8 pairs, both modes under the same load): no-window whole stream 3.46x (3.36-4.00), 128 KiB first chunk 3.48x, window-full 3.62x (3.15-4.12), 32 KiB marker-first 2.80x and usage 2.73x (header-bound); all 8/8. Full `benchmarkInflate` base64 2.91x (6/6). Committed as `8f3a9877`. | 2026-11 |
| 36 | AVX-512 VBMI 8-wide dominant literal decode: extract eight dominant-length code fields with one `vpmultishiftqb` and map them to bytes with one 64-byte `vpermb`, replacing eight LUT loads. Reject a group by finding the first 0xFF sentinel with a scalar borrow mask, emit its dominant prefix in bulk, and let one scalar step consume the non-dominant code before SIMD resumes. Compile the whole loop for AVX-512 with a target-attributed entry point so the intrinsics inline; an out-of-line helper cost ~60 cycles per group. | **Win**: tip -> final base64 no-window-full 1.97x -> 2.04x, window-full 2.06x -> 2.08x, first chunk 1.74x -> 1.84x in the first paired run. Committed as `a71f4a3a`. | 2026-11 |
| 37 | Decode custom-Huffman literals from a local 128-bit window over `BitReader::UnreadBitsView` (valid bit-buffer bits plus the unread byte buffer) and commit the consumed bit count once per view with `skipUnreadBits`, which never leaves the byte buffer and works on non-seekable input. Refills one 64-bit load at a time. | **Win**: tip -> final no-window-full 15% fewer cycles; combined with row 36 the full stream reached 1.27 GB/s and first/usage improved. Committed as `a71f4a3a`. | 2026-11 |
| 38 | Replace the marker-window conversion's per-element modulo loops and the zero-initialized 64 KiB conflated buffer with two contiguous fills and two contiguous 16-to-8-bit loops. Then conflate in one pass: zero the first `N - decoded` entries and map-and-narrow the rest directly into the destination. | **Win, first/usage**: conversion fell from ~32% of the 128 KiB first-chunk samples to ~24%; first chunk 1.84x -> 2.43x, 32 KiB first 1.27x -> 1.93x, usage 1.28x -> 1.58x vs the 3x tip. Committed as `0d7309b6` and `f2da3a90`. | 2026-11 |
| 39 | Split the wide-peek decoder out of the SIMD loop and select it at loop instantiation: add a `SIMD` template parameter to `readInternalCompressedLoopImpl`, instantiate it with `true` from the AVX-512 entry point and with `false` otherwise. | **Win and regression fix**: no-window-full +2.9%, first chunk +3.0%; `benchmarkInflate` Random recovered from ~1.1 GB/s to ~1.6 GB/s (the local-window loop was slower than the wide peek for dominant lengths above six). Committed as `761f7fbe`. | 2026-11 |
| 40 | Final paired result vs the original custom path `1f506834` (ISA-L off, base64, 64 MiB, CPU 15): no-window 128 KiB first chunk 7.95x (7.9-8.3 in the low-load run: 8.34x), no-window whole stream 7.65x, window-full 7.77x, gzip-reader 7.47x, 32 KiB first 6.16x, usage 5.72x; 6/6 pairs each. Full `benchmarkInflate` Random Base64 4.76x vs base and 1.61x vs the 3x tip. | **8x reached on the 128 KiB first chunk; whole stream at ~7.7x.** | 2026-11 |
| 41 | AVX2 fallback for the dominant-literal decoder: extract the eight fields with two `vpsrlvq` and map them through the same 64-byte table with four 16-byte `vpshufb` lookups plus three bit selects. Reuse the AVX-512 sentinel rejection and local unread-bit window; dispatch per read call to AVX-512, AVX2, or the wide-peek loop and allow `RAPIDGZIP_SIMD_LITERAL_DECODE=avx2|avx512|scalar` for A/B runs. | **Win on AVX2-only CPUs**: `benchmarkCustomInflate`, base64, 64 MiB, ISA-L off, CPU 15, 8 alternating pairs, 6 iterations, medians of per-pair ratios vs the 3x scalar tip / original `1f506834`: no-window-full 1.82x / 6.11x, chunk 1.81x / 6.35x, window-full 1.80x / 6.09x, first 2.04x / 5.80x, usage 1.61x / 4.09x, gzip-reader 1.74x / 6.01x; 8/8 each. AVX2 reaches 0.76-0.86x of the AVX-512 path, i.e., retains ~80% of the gain. Full `benchmarkInflate` Random Base64 4.97x base / 1.64x scalar (normal dispatch 5.22x / 1.72x) under load 6-10; other cases within noise. | 2026-11 |
| 42 | PCLMULQDQ folded CRC32 for the ISA-L-off path: four 128-bit accumulators, two-stage Barrett reduction, runtime dispatch on `pclmul`, slicing-by-16 kept below 64 B. | **Win**: isolated 32 KiB 4.35 -> 15.46 GB/s. `benchmarkGzipDecode`, 6 alternating pairs (perf cycles / wall, instructions): text P1 -5.2% / -5.0% (-14.1%), base64 P1 -4.8% / -6.5% (-10.8%), text P8 -7.1% / -7.7% (-12.6%). Committed as "[performance] Use PCLMULQDQ folding for the CRC32". | 2026-11 |
| 43 | Bulk overlapping backreference copy for the contiguous source case (`offset <= m_windowPosition`): drop the per-byte modulo loop, use a forward byte copy for `distance < 8` and 8-byte word copies for `distance >= 8`. No distance doubling is needed because the window write is already a plain forward self-copy. | **Negative for text**: match-path counters on `text.gz` P1 show 21.1M backreferences / 255 MB copied, but only 6.78 MB (2.7%) have `length > distance`; 1.12 MB of that is `distance == 1` and already uses `memset`. The change moved 5.66 MB off the per-byte fallback, but text P1 was instructions -1.5% and cycles +1.7% over 6 pairs, i.e., noise. Dropped; the patch is kept as `tmp/negative/overlap-backref.patch`. | 2026-11 |
| 44 | Inline word copies for disjoint matches of length <= 32: one to four 64-bit loads through a local, writing whole words past the match end into the 4x window slack. The guard requires 32 B of buffer slack on both sides; marker windows keep the byte-granular `memcpy` and scan. | **Win**: `benchmarkGzipDecode`, 6 alternating pairs vs row 42 (perf cycles / wall): text P1 -3.7% / -2.8% (instructions -2.8%), base64 P1 -1.4% / -1.2% (instructions -0.8%). The 21.1M text matches copy 255 MB, ~12 B each, so the libc call dominated. Committed as "[performance] Copy short matches with inline word stores". | 2026-11 |
| 45 | Extend row 44's inline copy to 64 bytes for matches <= 64. | **Neutral**: text P1 instructions +0.1%, cycles ratio 0.992 with overlapping ranges; wall was within load noise. Not included. | 2026-11 |
| 46 | Sequential CLI fast path: when one worker is requested and no index, ranges, line offsets, or statistics are used, decode with `GzipReader` instead of the block finder, chunk queue, and chunk windows. | **Win for the custom decoder**: CLI `-P 1`, 6 alternating pairs on 537 MB text / 544 MB base64, 1.658 -> 1.312 s (+26%) and 3.893 -> 1.813 s (+115%). The ISA-L build keeps the pipeline because its chunk decoder uses the full ISA-L inflate state machine on whole chunks, while `GzipReader` only reaches the per-symbol ISA-L Huffman path. Committed as "[performance] Skip the chunk pipeline for single-worker CLI decode". | 2026-11 |
| 47 | Start a SIMD literal run after one literal instead of four. The four-literal gate protects the wide-peek decoder, but the SIMD decoder amortizes its setup within the first group and every backreference resets the counter. | **Win**: 544 MB base64, sequential `GzipReader`, three perf-stat repetitions: instructions 25.91e9 -> 24.46e9 (-5.6%), cycles 13.21e9 -> 12.92e9 (-2.2%). Text does not use the SIMD decoder and keeps the four-literal gate. Committed as "[performance] Start SIMD literal runs after a single literal". | 2026-11 |
| 48 | On a rejected SIMD group, store the whole eight-byte group for the dominant prefix with one fixed-size store instead of a variable-size `memcpy`; the sentinel bytes after the prefix are overwritten by the symbols decoded next. | **Win**: 544 MB base64, sequential `GzipReader`: cycles 13.09e9 -> 12.04e9 (-8.0%), instructions 24.47e9 -> 22.79e9 (-6.9%); wall median ratio 1.095, 5/5 alternating pairs. Committed as "[performance] Store the SIMD literal prefix with one fixed-size store". | 2026-11 |
| 49 | Use the custom SIMD inflate for windowed chunks instead of ISA-L in the default build. | **Negative for the real corpora**: decode-only `-p 1` (no index, sparsity, window compression, or output copy), 5 pairs: ISA-L is 1.83x faster on text and 1.63x on base64. The custom path is 1.3-1.5x faster only on literal-only streams (uniform and recompressed base64), where its AVX-512 literal groups beat ISA-L's per-symbol literal path. A runtime selection would need both decoders in one binary and would pick ISA-L for the shipped corpora, so no selection was added. | 2026-11 |
| 50 | AVX-512 mixed group: when a group is rejected, look up the first sentinel's seven-bit literal in a 128-entry table, re-extract the following lanes with their one-bit shift, and store all eight literals if no later lane is non-dominant. A second exception or a non-literal still falls back to the prefix plus one scalar step. | **Win, but only ~2%**: `benchmarkGzipDecode -p 1 -i 8 --discard-output`, CPU 13, 8 alternating pairs, best-of-8 per run: `base64x4` 1.019 (8/8), `base64-huff-x4` 1.033 (5/5). On `base64x4`, 48% of the 99.5M rejected groups are absorbed, branch misses fall 166.5M -> 147.4M (-11.5%), instructions rise 24.13e9 -> 25.72e9 (+6.6%), cycles 12.97e9 -> 12.79e9 (-1.3%). Committed as "[performance] Absorb one seven-bit literal in the AVX-512 group". | 2026-11 |
| 51 | Isolate the backreference cost from the literal tree: compare `synth-uniform-def-x8` (uniform-ish six-bit literal tree, DEFAULT matches, 3.9-5.2% copied bytes) with `synth-uniform-huff-x8` (same data, literal-only) and with `base64x4` (mixed tree, 3.5-5.0% copied bytes). | **The mixed-length thesis is the smaller half of the gap**: with matches, the mostly-uniform tree reaches 386 MB/s and real base64 326 MB/s (1.20x); without matches they reach 845 and 728 MB/s. Removing the matches is 2.20x in both pairs, reducing literal exceptions is 1.20x. Backreferences, not mixed code lengths, dominate the real-base64 ceiling; no literal-table change can reach 1.1 GB/s. | 2026-11 |

### Backreference copy paths (2026-11)

Counting every `resolveBackreference` call on `text.gz` P1 (21.1M calls, 255 MB copied, i.e., 1.9x
the decoded size) gives the actual split: disjoint `memcpy` 248.1 MB, `distance == 1` `memset`
1.1 MB, non-disjoint contiguous forward copy 5.6 MB, per-byte fallback (wrapping source or
destination) 0.18 MB. Overlap (`length > distance`) is 6.78 MB / 2.7%, not "a large fraction".
The dominant cost is the 21.1M short disjoint copies (12 bytes on average), not overlap; row 44
attacks that call overhead. `base64.gz` has only 5.8 kB of overlap.

The synthetic overlap generators do not isolate `distance >= 8` overlap well either:
`benchmarkInflate` writes overlapping `std::memcpy` calls (undefined behavior), and a corrected
forward byte loop makes zlib encode 95% of the copied bytes as `distance == 1` runs. Treat rows
13/14/20 as measurements of those synthetic streams, not of text.

### Three-shape baseline on this machine (2026-11)

Unlike the reporting Zen 2 result (HUFF 174.5 / fixed 237.2 on `ab027b2`; 242 / 196 on `c571199`;
253 / 194 on `perf-dynamic-noinline`), this machine already has the intended split:

| build | HUFFMAN_ONLY base64 | fixed-Huffman base64 |
|---|---:|---:|
| `ab027b2` (9-bit fixed LUT) | 405-414 | 179-181 |
| `c571199` / `fa40b93` (fixed via ISA-L loop) | 423-431 | 305-314 |
| `ec56e63` (noinline, parent `ab027b2`) | 422-430 | 173-180 |

So the fixed ISA-L routing is a +72% win here, not a loss, and HUFFMAN_ONLY is already at the
noinline level. The PC layout target is not reproducible on this CPU; the packed-store and
layout-isolation experiments therefore have no positive headroom to exploit. All three shapes lose
3-6% on HUFFMAN_ONLY with the packed multi-literal store.

Known NEGATIVE / risky-to-repeat:

- `DoubleLiteralCached` blows L1 under multi-threading; 4th literal unroll in libdeflate's fastloop
  throttled (they measured a drop and removed the 4th literal - `lib/decompress_template.h:376-380`).
- LUT sizes matter a lot: `ShortBitsCached` LUT=12 (16 KiB) is *slower* than 11 (8 KiB). Any new table
  must fit L1 alongside the window.
- Overlapping matches are faster with the plain per-byte repetition loop than with either memcpy
  doubling or word stores on this machine (rows 13-14), and the 16-byte reload/doubling copy from
  ISA-L is also slower (row 20). Do not "fix" that loop without a benchmark that isolates overlap
  matches (`benchmarkInflate`'s overlapping-backreferences case).
- Bulk overlapping copies are not a text lever: overlap is 2.7% of copied bytes and mostly
  `distance == 1` (section "Backreference copy paths", row 43). The dominant cost is the 21.1M
  short disjoint matches, addressed by row 44.
- Do not extend the inline small-match copy (row 44) past 32 bytes (row 45): the extra branch and
  store traffic are neutral.
- The ISA-L caller's per-literal `appendToWindow` loop is already cheaper than a packed 2/3-byte
  store plus the needed trailing-symbol handling (row 18). Packed entries can end in a length or EOB
  symbol, so "store `symbolCount` literals" is wrong even though most entries are all literals.
- Layout isolation of the fixed path (noinline, separate instantiations) does not change HUFFMAN_ONLY
  on this machine (row 19); the pre-existing out-of-line dynamic loop is the whole effect.
- A root+subtable litlen decoder cannot beat `ShortBitsCached@11` when the full 15-bit LUT itself is
  not faster (row 23). The long-code fallback is not the bottleneck.
- Destination-wrapping non-overlap copies are too rare with a 32 KiB window to pay for a branch on
  every backreference (row 30). The row 12 win came from the previously rejected high-start half,
  not from actual source-end wrap; do not generalize the fast path to the destination without a
  workload where `length / window.size()` is significant.
- Do not add the two-memcpy source-wrap branch on top of the contiguous fast path (row 32): actual
  source-end wrap is only ~0.4%, and the extra branch measured 2.7% slower on random-backreferences.
  The contiguous-only check is the part that pays.

## End-to-end parallel chunk pipeline (2026-11)

The work above targets the single-threaded inflate loop. This section covers the surrounding chunk
pipeline: block finding, speculative chunk decoding, marker replacement, window compression, and the
output copy. It uses a new focused benchmark, `benchmarkGzipDecode`, because `benchmarkGzip`'s full
matrix is too coarse for alternating A/B runs. The corpora live in `tmp/gzip-corpus/` (not committed):
`text.gz` (30 MiB compressed / 134 MiB decoded), `text4.gz` (the same stream four times), `base64.gz`
(103 / 136 MiB), and `random.gz` (incompressible, stored blocks). Runs alternate A/B/A/B, are pinned to
CPU 6 for P1 and 6-13 for P8, and report the median of per-run medians. With a loaded machine the
binary-to-binary noise floor is about 2-3%; the unchanged-configuration controls below show it.

### Measured overheads

- **CRC32**: disabling checksum verification with `setCRC32Enabled(false)` on P1 gives 736.6 vs
  703.1 MB/s on `text.gz` (paired median 0.9465, 0/6 pairs faster with CRC) and 371.6 vs 368.5 MB/s on
  `base64.gz` (median 1.008, 3/6, i.e. noise). The new `ChecksumVerifier` itself takes ~1e-5 s per
  stream; the cost is the hardware CRC32 pass over the decoded bytes. There is no separate per-chunk
  verification overhead to optimize, and verification should not be disabled by default.
- **Output copy**: a no-op write functor (`--discard-output`) on P1 gives +8.4% on `text.gz` (6/6
  pairs) and +5.4% on `base64` (6/6); P8 gives +9.6% and +2.7%. This is the copy into the caller's
  buffer and is inherent to the memory API. The file-descriptor path already writes directly from the
  chunk buffers with `writev`.

### Accepted changes

1. **Only verify sparse windows in debug builds** (`getUsedWindowSymbols`). The verification decoded
   the next 32 KiB a second time. 8 alternating pairs: `text4.gz` P8 1685 -> 1745 MB/s (+4.1%,
   7/8), `base64.gz` P8 967 -> 1032 MB/s (+7.5%, 8/8). P1 was within noise.
2. **Compress seek windows with ISA-L zlib** instead of zlib level 6. A 32 KiB window probe shows
   ISA-L level 1 is 7-8x faster, at +13.6% bytes on text and -0.6% on base64. End to end: `text.gz`
   P1 +2.4%, P8 +2.0%, `base64` P8 +1.9% (5/8).
3. **One chunk per worker for medium files.** The constructor reduced chunks to ~3 per worker for
   files smaller than `2*P*chunkSize`; each extra chunk must validate its start and carries a window.
   `text.gz` (30 MiB) with P8: 1382 -> 1608 MB/s (+17.9%, paired median 1.1788, 8/8); P4 +5.6% (8/8).
   Files that already have at least P chunks keep the requested size; P2, `text6m`, and `text4`
   controls stayed within 2%.

### Negative results (do not retry without a new mechanism)

- **ISA-L inexact-stop for chunks with a known window**: calling `finishDecodeChunkWithInexactOffset`
  directly instead of the custom-inflate prefix was neutral (`text4` P8 0.996) and 1.8% slower on
  `random` (2/8). Most speculatively decoded chunks still have no window, so the prefix path was not
  the limiting factor.
- **Publishing raw windows from workers**: the main thread already publishes an uncompressed window in
  `queueChunkForPostProcessing` before it queues compression, so worker-side publication was
  redundant. 10 pairs: `text4` 0.993, `base64` 1.008, `random` 0.951 (0/10) - the extra 32 KiB copy
  and map insert hurt incompressible chunks.
- **Removing zero-initialization of the 128 KiB decode buffers**: a custom `DecodedVector` with
  `resizeUninitialized` reduced callgrind memset from 13.9% to 5.2% of instructions, but wall-clock was
  neutral (`text4` P1 1.0016, P8 0.9883, `base64` P1 1.0182, `random` P1 0.9963, 8 pairs each). The
  zeroing runs at L2 bandwidth and is not on the critical path; the buffer class and the change were
  reverted.
- **AVX2 gather for marker replacement**: the gather-based map was 0.89-0.99x of the scalar loop
  (Zen 4 gather is slow). The scalar in-place loop is already optimal.
- **Larger prefetch cache (3P vs 2P)**: neutral (`text4` 0.997, `base64` 1.014, `text` 1.018, 8 pairs),
  as expected because the thread pool already limits in-flight work to P.

### Final integration (`benchmarkGzip`, base 316e8453 vs branch)

5 alternating pairs, `taskset -c 6-13`, decoded bandwidth:
- `text.gz` parallel P8: 1132 -> 1360 MB/s (+16.2%, 5/5); P1 and serial within the ~3% control noise.
- `random.gz` P8: 3400 -> 3720 MB/s (+3.6%, 4/5); P1 +7.8% (4/5).
- `text4.gz` P1: 685 -> 709 MB/s (+4.0%, 4/5); P8 within noise (the large file keeps 4 MiB chunks).
- `base64.gz`: +1 to +2% at P1/P8/index, within noise.

Remaining ranked ideas: (a) avoid the caller-buffer copy by decoding directly into the destination for
sequential reads (API change, biggest remaining measured cost); (b) a track-only mode for the sparse
window lookahead that skips writing decoded bytes (small, needs a compile-time Block variant);
(c) 8 MiB default chunks for large files (P8 results were contradictory under load, and memory
doubles); (d) benchmark BGZF with larger allocation chunks before touching `ALLOCATION_CHUNK_SIZE`.

## Full-stream decode on 500 MB corpora (2026-11)

The microbenchmark wins were checked on full multi-block CLI decodes of 500 MB text/base64 corpora.
The complete tables, corpus definitions, P1/P8 results, the minigzip build-artifact correction, and
the 8x ceiling analysis are in `gzip-full-stream-report.md`. Summary:

- P1 against the last pre-optimization custom path `1f506834`: text 1.17x (custom) and 2.47x (ISA-L);
  base64 2.28x (custom) and 3.51x (ISA-L); literal-only streams 6.74x/5.36x with CRC.
- P8 against old: text 1.90x and base64 2.37x with ISA-L.
- `benchmarkGzip` integration: text P8 +16.2%, random P8 +3.6%, text4 P1 +4.0%, base64 within noise.
- The 8x target is above the reference-decoder ceiling on real corpora and is reachable only on
  literal-only streams; the remaining gap there is the CRC pass and machine load, not the pipeline.
- On real base64, removing backreferences is 2.20x while reducing literal exceptions is 1.20x, so
  backreferences dominate. Mixed-length decoding is worth at most ~15-20%, not the 2.9x to 1.1 GB/s.

## Reference library techniques (repos/{libdeflate,zlib-ng,isa-l,zlib})

Collected from the clones in `repos/` (libdeflate v1.26, zlib-ng 2.3.1+, ISA-L v2.32, zlib 1.3.2).
Full extracts: `/tmp/optimization-sources/{libdeflate,zlib,isa-l,our-findings}.md` (scratch).

### 1. Bit buffer / refill

- libdeflate: 64-bit branchless refill `bitbuf |= get_unaligned_leword(in_next) << (u8)bitsleft;`
  `in_next += WORDBYTES-1; in_next -= (bitsleft>>3)&7; bitsleft |= MAX_BITSLEFT & ~7;` -
  single OR for the refill, `MAX_BITSLEFT = BITBUF_NBITS-1` keeps the shift UB-free
  (`lib/deflate_decompress.c:206-212`, `:116-117`). `bitsleft` intentionally holds garbage in the high
  bits so consumption is one full-width `bitsleft -= entry` (`:83-91`).
- zlib-ng: essentially the same `hold |= load64(in)<<bits; in += (63^bits)>>3; bits |= 56;`
  (`inflate_p.h:104-108`), ~1.2x over zlib on x86_64.
- ISA-L: `read_in | temp << read_in_length` with a guard "always keep >= 8 valid bits" so symbol
  lookups never need a bounds check (`igzip/igzip_inflate.c:193-221`).
- Conclusion for us: our BitReader already does 64-bit unaligned load + shift refills and 64-bit
  buffers. **A 256-bit SIMD buffer is not worth it**: every reference library tops out at one
  64-bit machine word; the metric is refill frequency, which 64 bits already minimizes, and wider
  buffers would complicate the byte-aligned refill and the `<< bitsleft` OR-ing without adding
  throughput. Skip.

### 2. Two-level Huffman with merged extra bits (the biggest single technique)

- libdeflate: one packed `u32` per table slot carries symbol/base value, codeword length, extra-bit
  count, and type flags; the LOW BYTE is the total consume count = codeword len + extra bits, so a
  single `bitbuf >>= (u8)entry; bitsleft -= entry;` decodes a symbol AND consumes its extra bits; the
  value is then `base += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (entry>>8)` (saved_bitbuf captured
  before consumption, `_bzhi_u64` on BMI2). One load + one shift per symbol, no second bit read.
  Entry layout: `deflate_decompress.c:437-503`; decode: `lib/decompress_template.h:346-360`;
  `make_decode_table_entry(result, sym, len) = result[sym] + (len<<8) + len` (`:387-391`).
- ISA-L: two-level 12-bit short table + long table where the short entry packs up to **3 consecutive
  literals** and their total code length, so one shift + one store advances `next_out` 1-3 bytes
  (`include/igzip_lib.h:499-508`, `igzip/igzip_inflate.c:443-544`). Distance short table is 10-bit.
- zlib-ng: table entry `op` field bakes code_bits+extra; one `DROPBITS(here.bits)` + one
  `EXTRA_BITS(old,here,op)` (`inflate_p.h:152-178`).
- This addresses the long-standing question of whether a two-stage lookup can win: the win is not the second
  level per se, it is **pre-consuming the extra bits with the symbol** and eliminating the second
  `read(n)` from the loop. Our `ShortBitsCached`/`ReversedBitsCached` return only the symbol; the
  caller then does `getLength`/`getDistance` which re-read bits. Merging the extra-bit count and base
  value into the entry is the missing piece.

### 3. `__restrict__`

- libdeflate (C) marks `in`, `out`, and the decoder-state pointers `restrict`
  (`lib/decompress_template.h:44-48`, typedef `:1072-1076`).
- zlib-ng: in its template-based inflate it deliberately does **not** restrict the hot pointers because
  the match-copy source aliases the destination (self-modifying window) - only `Z_RESTRICT` on
  non-overlapping paths (`inffast_tpl.h:52`, `zbuild.h:195-199`).
- For us: restrict is a per-pointer hint. Marking the *input* cursor/bitreader buffer restrict is safe,
  but the match source (`window + mp - distance`) provably aliases the output, so only the input side
  (and tables) can be restrict. Expected effect is small (GCC/Clang already know these from locals);
  treat as an experiment, not a given. C++ supports `__restrict__` (GCC/Clang/MSVC) or
  `[[gnu::restrict]]`.

### 4. LZ / match copy

- libdeflate: 5 unconditional word copies + 5-words/iter loop, with intentional write-past-the-match of
  up to 5 words steered into reserved slack, so ~40-byte matches finish without a per-byte branch
  (`lib/decompress_template.h:590-622`, `FASTLOOP_MAX_BYTES_WRITTEN` `deflate_decompress.c:280-281`).
  RLE (`offset==1`) is a byte-broadcast word fill (`:623-648`); partial overlap (`offset < WORDBYTES`)
  walks stride-by-offset so each load precedes its store (`:649-663`). libdeflate has NO 32 KiB
  circular window (buffer-based) so wrap-around does not exist there.
- zlib-ng: `CHUNKUNROLL` doubling (`chunkset_tpl.h:24-63`) + arch `chunkmemset_1/2/4/8/16` broadcasts
  (`chunkset_{avx2,sse2,neon}.c`).
- ISA-L (asm): 16-byte XMM match copies via `MOVDQU`, with sliding-overlap "small_byte_copy" doubling
  until non-overlap, and `OUT_BUFFER_SLOP=16+258` reserved so the hot loop has no bounds checks
  (`igzip/igzip_decode_block_stateless.asm:29-32, 599-621`).
- Us: our `resolveBackreference` bulk-copy (commit "[performance] Replace per-byte LZ match-copy loops with bulk copies") already covers the memcpy/fill/marker
  cases. The remaining difference is (a) the marker-phase 16-bit window still pays per-element
  accounting (scan-after-copy is implemented), and (b) we do byte-granular copies while reference
  libraries write word/XMM-sized with slack; with a 4x-oversized window we could adopt the
  "slack overrun" trick safely.

### 5. Literal runs / epilogue

- libdeflate: literals stay byte-at-a-time but up to 3 are unrolled per fastloop iteration
  (`lib/decompress_template.h:363-435`); a 4th literal was measured slower (branch prediction).
- ISA-L: multi-literal packing (see item 2) is the winning form.
- FAST-loop guard pointers: `in_fastloop_end`/`out_fastloop_end` reserve slack so the hot loop body
  has zero per-iteration bounds checks (`decompress_template.h:52-59, 344-345`); the "generic loop"
  re-checks only near the buffer ends.
- For us: this maps to reserving near the 128 KiB window edge instead of `mp %= size` every symbol;
  our `mp % window.size()` is already an `&` because the size is a power of two, but folding the
  module into a rare guard branch would save the `&` on the hot path.

### 6. Other

- Fixed/static Huffman tables cached across blocks (`lib/decompress_template.h:305-308`; our
  `m_fixedHC` static already does this - `deflate.hpp:970`).
- Subtable pointer entries carry their own bit-count and index so the miss path is one indirect load.
- Decode-table construction: libdeflate fills the root table by doubling (`memcpy` of the table at each
  length step, `deflate_decompress.c:884-934`) - fast build AND fewer cache touches.
- Runtime dispatch of a BMI2-specialized whole-file template (`lib/x86/decompress_impl.h:34-58`).

## Ranked open ideas (estimate, risk)

1. **Merged extra-bits consume into the L/D table entries** (libdeflate-style, item 2). Targets the
   confirmed per-backreference overhead (remove `getLength`/`getDistance` second reads). Expected:
   largest single win for backref-heavy + mixed streams. Effort: high (decode-table + loop refactor,
   must keep all decoder variants building). Risk: medium (correctness surface). This is the
   "two-stage lookup that actually wins" that was being sought - the win is the merged consume,
   not the second level.
2. **Caller-side bit window / iterator over the BitReader buffer** (libdeflate-style local `bitbuf`).
   Fold refill + consume into the decode loop as locals, with a `saved_bitbuf` for extra bits; keep the
   `FileReader` interface. Expected: reduces per-call indirection and enables #1. Effort: medium.
   Risk: medium (must keep the EOF/trailer machinery correct).
3. **Slack-guaranteed fast loop** (item 5): reserve window-edge slack so the per-symbol `mp &= ...`
   and the marker/wrap checks become a rare guard branch. Expected: few %, combined with #1 likely
   necessary to see it. Effort: low-medium. Risk: low.
4. **`__restrict__` on the input cursor / table pointers** (item 3): cheap experiment; may be noise.
5. **Marker-phase distance-1 / overlap bulk path already done in "[performance] Replace per-byte LZ match-copy loops with bulk copies"**; further: word-sized
   (non-memcpy) fills for the marker window and the "slack overrun" reserved towards the 4x window.
6. **Two-level root+subtable with lazy sub-tables** - **closed by measurement (row 23)**. The full
   15-bit LUT is not faster than `ShortBitsCached@11` on the worst-case long-code distribution
   (2709 vs 2656 us), and the subtable form is larger than the 8 KB short LUT. The long-code fallback
   is not the bottleneck; the gap is caller-side.
7. **Skip/not-worth-it**: 256-bit SIMD bit buffer; literal-run widening (measured regression);
   `DoubleLiteralCached` at 128 KiB (L1 pressure); packed multi-literal stores (row 18).

Remaining ranked ideas after round 2 (2026-11-02), all previously ranked caller/header ideas are now
closed by measurement:

1. **Merged extra-bits consume with a caller-side bit window**: closed. The standalone wide peek (row 11),
   the persistent 56-bit fused loop (2026-09) and the non-persistent 16-bit window (row 26) all regress.
   The `BitReader` second read is cheaper than any window plumbing tried on this CPU.
2. **Slack-guaranteed caller loop**: closed (row 24). The wrap is an AND and the chunking overhead wins.
3. **`__restrict__` on the input cursor and table pointers**: closed (row 25), within noise.
4. **Precode construction / block-finder header cost**: closed (row 28). The header is 6.75% of decode on
   an extreme 32 KiB-block stream and 91% of it is ISA-L literal-table construction that cannot be reused
   because consecutive trees alternate (row 29). Precode work is 0.16% of the total.
5. **Remaining untested structural idea** (not attempted, high risk): replace the ISA-L short-table
   reconstruction per block with an incremental/two-generation table that only rewrites entries whose
   code lengths changed. The alternation in row 29 means most entries are stable, but the tables are
   large and the correctness surface is high; this is a separate project, not a micro-optimization.
6. **Bulk overlapping backreference copy**: closed by row 43 and the "Backreference copy paths"
   section. The reference report's premise that `distance < length` covers a large fraction of text
   matches does not hold for this corpus; short disjoint copies (row 44) are the actual lever.

## How we measure

- Primary: `benchmarkInflate` (self-generated 128 MiB inputs; 5 cases; report the `X <= mean+-err <= Y`
  midpoints) and `benchmarkHuffmanCoding`. Build with `LIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON`.
- The machine runs CI compile jobs sporadically; check `uptime` before trusting a single sample, run
  >= 3 interleaved samples, prefer `perf`/profiling over wall-clock when loaded.
- Wall-clock is unusable under sustained load (observed 2026-09-08: load avg 8.7 from a foreign
  `clang++` job, backrefs spread 953-2890 MB/s across otherwise-identical runs). When the machine is
  loaded, either measure with `perf stat -e cycles,instructions` (CPU cycles are preemption-robust) or
  defer the measurement until `uptime` is low. Do not label a change `[performance]` based on
  wall-clock data collected under load.
- On a quiet machine (load < 1, e.g. 2026-09-09), the stable method is: build both binaries, copy them
  to /tmp, then alternate them one-at-a-time for >= 6 pairs and compare per-case medians. This cancels
  slow-load drift within pairs and gives trustworthy relative numbers (+/- ~3% on cases that are not
  obviously noisy). Always verify a result in both build flavors (ISA-L and no-ISA-L) and always revert
  a measured regression even if the mechanism looked plausible.
- A "stands to reason but sub-measurement-noise" change is not a `[performance]` commit: either show
  cycles/duration improvement on a quiet machine or fold the idea into a larger refactor.
- Round 2 hygiene: rebuild the exact benchmark target from the current tree before every A/B. A stale
  `benchmarkGzip` from an older commit made a neutral change look ~30% faster. Also alternate the order
  within each pair, not only A/B/A/B with A always first: the first run of a pair was reproducibly
  1.5-3% slower (cold caches/frequency ramp) on this machine.
- Correctness gate: `testDeflate`, `testGzipChunk`, `testGzip`, `testParallelGzipReader`, `testCLI`
  (full ctest 29/29 on 2026-09-08). `[performance]` commits must be functionally identical.

## Tried and reverted (not enough signal)

- 2026-09: standalone `getLength` LUT slice (replace `code/4` + `calculateLength` recompute with
  `lengthLUT` + a precomputed extra-bits-count table; both `constexpr`, behavior-identical). Reverted:
  removes only a few ALU ops and the machine was under a sustained foreign compile job, so it could not
  be measured above noise. The real fix is not this slice but idea #1 (merged extra-bits consume), which
  eliminates the second bit read entirely; revisit this only as part of that.
- 2026-09: merged extra-bits consume for the dynamic distance decode: `ReversedBitsCached` got a
  `decodeWithoutConsuming<28>` that peeks code+extra in one window and the caller seeks once. Measured
  **regression** on the no-ISA-L backrefs case (~1380 -> ~1210 MB/s, 5-6/6 alternating pairs): the wider
  upfront `peek<28>` triggers more frequent bit-buffer refills than the old two-step (decode peeks at
  most the code length, then the extra read only asks what is needed), and the `optional<tuple>` return
  does not reduce to the same code. Also observed that a wider peek strips no extra ops from the path.
  Reverted. This is the concrete evidence that idea #1 does not pay on its own: libdeflate gets the win
  only because it keeps the window in a local with greedy refill (idea #2), so the "wider consumption"
  is already available for free.

## Tried and reverted (2026-11)

All of these started from the post-row-12 tree and were measured with alternating A/B binaries (6 pairs,
one process at a time, per-case midpoint of `benchmarkInflate`), the method described under "How we measure".
Each was reverted after the measurement:

- Overlap copy with `memcpy` doubling (row 13) and with 8-byte word stores (row 14): the per-byte
  repetition loop wins for distances 2..31. The benchmark case added for this
  (`benchmarkInflate`'s overlapping-backreferences case) should be kept for future attempts; the
  random-backreference generator explicitly rejects `length > distance`.
- `switch`-dispatched templated extra-bit reads in `Block::getDistance` (row 15): the runtime
  `bitReader.read(n)` with its mask LUT is faster than an indirect jump plus a constant-mask variant.
- Local `m_windowPosition` across a literal batch (row 16) and `optional` -> sentinel in
  `HuffmanCodingISAL::decode` (row 17): neither shortened the per-literal path measurably; both were
  within noise or worse.
- Packed multi-literal store in the ISA-L caller (row 18): -6% HUFFMAN_ONLY on the target build and
  -3 to -5% on `ab027b2` and `perf-dynamic-noinline`; fixed-Huffman's +4% is pure layout because its
  fixed tree generates no packed pairs. The first implementation also showed why the store must not
  assume `symbolCount` literals: a packed entry can end in a length symbol, and the window position
  must wrap with `%=` just like `appendToWindow`.
- Layout isolation via explicit `noinline` and separate fixed/dynamic instantiations (row 19): no
  HUFFMAN_ONLY gain; the clean `c571199` shape is already the best of the three on this machine.
- 16-byte sliding overlap copy (row 20): -5.8% on the overlapping-backreferences case (10 pairs,
  2/10 wins), consistent with rows 13/14.
- Marker-phase bulk copies (row 22): at best ~1% on a marker-heavy parallel workload and 0% at
  256 KiB chunks; not worth the extra branches.
- Slack-guaranteed wrap-free loop (row 24): consistently slower on both build flavors. The window wrap is
  an AND, not a branch, so there is nothing to hoist into a rare guard.
- `__restrict__` on the input reader (row 25): within the noise band in both flavors.
- Non-persistent 16-bit window for litlen length extra bits (row 26): -11% on literal-heavy no-ISA-L
  because the per-symbol window and optional plumbing outweigh one saved `read()`.
- Full template-width peek (row 27): an incorrect lookahead into unfilled incomplete-tree entries.
- Last-value code-length cache (row 29): consecutive blocks' trees differ even for stationary data, so a
  single-entry cache never hits. Multi-entry caching would duplicate the large tables.
- Wrapping-destination non-overlap `memcpy` (row 30): measured against the source-wrap fast path
  (`91c415aa`), the destination-wrap-only step was 3.1% slower on the random-backreferences case in all
  five alternating rounds (median ratio 1.031, range 1.003-1.056). Destination wrap is rare with the
  32 KiB window (`length / 32768`, ~0.4% for 4-255 byte matches), while the added branch and position
  bookkeeping run on every backreference. This is not the same situation as the row 12 win: the old
  `distance <= m_windowPosition` condition rejected the whole high-start half (~50%), including
  contiguous sources that could use one `memcpy`. The direct source-contiguity test is
  `offset + length <= window.size()`; actual source-end wrap is only ~0.4%. The row 12 gain came from
  covering the common half of the old partition, not from wrapping being common. Measured on a
  battery-powered `powersave` machine with load 2.4-3.0; the absolute noise band is 2-5%, but the sign
  was consistent across every round.
- Single-symbol fill for wrapping destinations and marker windows (row 31): the zeros case exercises
  `distance == 1` fills and was neutral (base 0.0444 s, final 0.0440 s; fill-only step +0.2%). The old
  path already used `memset` for non-wrapping `distance == 1`; only the wrapping subset (~0.4%) and
  `uint16_t` marker windows are new. The sequential `benchmarkInflate` uses a `uint8_t` window, so the
  marker part was not measured by this A/B. Both commits were dropped from `security-audit`; the
  measured commits are preserved in `backup/security-audit-pre-drop-ab`.
- Two-memcpy source wrap on top of the contiguous fast path (row 32): the contiguous-only version
  was compared with contiguous + two-memcpy over 5 rotating rounds. The two-memcpy version was 2.7%
  slower on random-backreferences (paired median ratio 1.027, 4/5 rounds slower); controls stayed
  within their noise bands. Actual source-end wrap is ~`length / 32768` (~0.4%), so the extra branch
  is not paid for. The contiguous-only version is kept in `security-audit` (`dd562aa2`); the
  two-memcpy experiment is preserved on `experiment/two-memcpy-wrap` and is not intended to merge.

Lesson: on this target the remaining per-symbol/per-copy code is already at a local optimum for
small re-arrangements. Measured reductions of actual work (row 32's contiguous-only part and row 21)
pay; instruction reshuffling and layout isolates do not.

## Profiling snapshot (callgrind, 2026-09-09)

Method that works on this box (no `perf`; `perf` needs `kernel.perf_event_paranoid` lowered at container
start): `sudo apt-get install -y linux-perf valgrind`, then
`valgrind --tool=callgrind --callgrind-out-file=x.out <bench>` and parse `x.out` yourself (the
`callgrind_annotate` flat list is unreliable when template code is inlined; the raw file is well-formed:
`fn=`/`cfn=` frames, `positions: line`, `events: Ir`, cost lines `N c` / `+N c` / `* c`). The parser
used for this analysis is not part of the repository.

Sequential `GzipReader` decode of a 48 MiB backrefs-heavy gzip (ratio 38, ~750 blocks), `-O2 -g`,
self-cost attribution (total 267M Ir):
- `__memcpy` 59.7M (22%) + `__memset` 3.8M + `resolveBackreference` self 5.7M -> the LZ match-copy
  (~26%) is the irreducible output-movement cost. This is the bulk-copy work from "[performance] Replace per-byte LZ match-copy loops with bulk copies".
- output-write path (std::function->writeAll->memcpy into the caller buffer) ~19%: benchmark harness
  overhead, not decoder.
- CRC32 (~37M inclusive) is on by default in `GzipReader` (`setCRC32Enabled(true)`, crc32.hpp
  `updateCRC32` -> `crc32_gzip_refl` on the ISAL build = hardware); the parallel reader's per-chunk
  CRC is separate. Consumers who verify CRC elsewhere / trust the stream can call
  `GzipReader::setCRC32Enabled(false)` to remove roughly this cost (the benchmark now does).
- per-symbol machinery is spread thin: mask LUT `N_LOWEST_BITS_SET_LUT` index ~3.6M (already one hot
  L1 load per access; a computed branchy mask is not clearly faster), optional/pair structured-binding
  returns ~2-3M each, bounds-checked `WeakArray::operator[]` ~2M, distance LUT ~0.9M. **No single
  fat per-symbol target remains**; the remaining lever is structural (idea #2: caller-side bit window
  + fused fast loop, which is what libdeflate/ISA-L do and what makes their per-symbol cost lower).
- Dynamic-header construction is not the bottleneck for the backrefs case (750 blocks decode at
  ~2.4 GB/s total, CLI `-v`: "custom inflate" 0.033 s of 0.024 s real, block finder tiny).

The base64 HUFFMAN_ONLY anomaly (see above) is the stored-blocks-vs-Huffman effect, confirmed by these
numbers too: the honest per-literal Huffman decode of this setup is ~385-390 MB/s (base64), which only
improves via the structural rewrite, not micro-tuning.

## Fused caller-side-window decode (idea #2) - attempted 2026-09-09, reverted

Implemented a libdeflate-style fused decoder, fixed its correctness bug, measured it, and reverted it
because it is a net regression on the default ISA-L build. The design and measured outcome are recorded
here so it is not retried blindly:

- **Design**: `HuffmanCodingISAL::decodeFromPeeked(uint32_t)` and `ReversedBitsCached::decodeFromPeeked`
  (window-based decode returning symbol/codeLen/consumedBits); a `readInternalCompressedMultiCachedFused`
  loop keeping a local 56-bit window, consuming length+distance codes and the distance extra bits with
  single shifts. Two semantic gotchas were pinned down:
  1. ISA-L *expands* length codes, so the decoded symbol is the full length (`symbol - 254`); the length
     extra bits are already inside the code length - only the distance has a separate extra read.
  2. **The correctness bug**: `BitReader::size()` returns *bits*, and a non-clamped refill lets `read()`
     cross the end of raw deflate streams (no footer), where `read()` consumes leftover bits *then* throws
     EndOfFileReached, silently desynchronizing the window. Fix: clamp the refill to the known stream size
     (`size()` already in bits) and gate the fused loop on a known size. With that fix, testGzip
     (21.7M checks) and full ctest 29/29 pass, including `.deflate`/`.zlib` raw streams.
- **Measured outcome (default ISA-L build, benchmarkInflate, 6 alternating pairs on a quiet machine)**:
  base64 ~388 -> ~128 MB/s (**-67%**) and fixed-Huffman ~267 -> ~81 MB/s (**-70%**), backrefs/random/zeros
  within noise. The caller-side 56-bit window is *slower* than the existing per-symbol ISAL multi-cached
  loop for literal-heavy streams (likely register pressure from the persistent window + refill
  bookkeeping). This refutes idea #2 for the ISAL path on this machine.
- **Not worth retrying** for the ISAL (default) path. The design could still be tried on the *custom*
  (no-ISA-L) decoder where per-symbol overhead is higher, but expect the same register-pressure problem;
  measure with `benchmarkInflate` before committing to it.

## Open questions for the bit reader specifically

- **base64 HUFFMAN_ONLY decode is ~7.5x slower than random HUFFMAN_ONLY (385 vs 2880 MB/s on the ISA-L
  build, quiet machine, 2026-09-09) - RESOLVED, not a bug.** callgrind (valgrind) shows random256 executes
  0.40B instructions vs 2.0B for base64 (~5x), and parsing the deflate block headers shows why: the
  incompressible random data (ratio 1.0) is emitted by zlib as 4097 *stored* (uncompressed) blocks, which
  decode through the memcpy fast path (~6 GB/s, `readInternalUncompressed`), whereas base64 (ratio 1.33)
  is 4 blocks of which 2 are real Huffman blocks decoded by the per-symbol path
  (`decode_huffman_code_block_stateless`, ISA-L). The gap is memcpy-vs-Huffman per-byte work, present in
  zlib too (714 vs 290 MB/s on the same streams); our Huffman decode is actually *faster* than zlib's on
  the base64 stream (385 vs 290 MB/s). No Huffman decoder can be made "as fast as memcpy"; the stored
  fast path already exists. Lesson for the benchmark: the "random data" case measures the stored-block
  path, not Huffman quality - use base64/FIXED/backrefs for decoder comparisons. (Investigated 2026-09-09:
  python-zipped test files + `valgrind --tool=callgrind` + `addr2line` + a tiny deflate block-type
  parser.)
- **Merged extra-bits consume** (idea #1) regressed on its own (see "Tried and reverted"): the wider
  upfront peek forces more refills. It can only pay off with a caller-side local bit window (idea #2),
  i.e., the libdeflate fast-loop shape, which is a substantial rewrite — do not retry idea #1 standalone.

## Open questions for the caller-side window

- Keep the `FileReader` interface? Yes - the streaming/seekable/mmap abstraction is core to the
  parallel reader and the Python bindings. The way to get "caller-side iteration" without abandoning it
  is a thin accessor that hands the decode loop a `(const uint8_t*, size_t availableBits, size_t
  consumedBits)` view plus a re-acquire on refill, or a `peekAndAdvance` that returns a consumed value
  AND the bit count in one call - the ISA-L model (`read_in` held in locals, refill fused).
- The ISA-L `decode_next_lit_len` already reads `peek<32>` + `seekAfterPeek(bit_count)` per symbol
  (ours), while libdeflate's per-symbol cost is one `bitbuf >>=` - the difference is that ours keeps
  the window in the class and reads via a method (albeit `forceinline`), libdeflate keeps it in a local
  and refills inline. This is a code-shape difference, not a fundamental one; see idea #2.

## Custom literal-run decoder (2026-11)

The plain no-ISA-L path (`HuffmanCodingShortBitsCached` in `readInternalCompressed`) is the custom inflate
used for chunks without a resolved window and for `getUsedWindowSymbols`. ISA-L and zlib were not changed;
they are the reference. The focused benchmark is `benchmarkCustomInflate` (`cefdd5af`, extended by
`28785b41`): it decodes the same raw deflate stream with a fresh `Block` and no window (32 KiB, 128 KiB,
and whole stream), with a real 32 KiB window, and with `setTrackBackreferences` (the usage shape), and
reports decoded bytes, CRC32, block count, and whether marker mode remains.

Measurement: CPU 15, ISA-L off, 64 MiB base64 and back-reference corpora, alternating A/B binaries,
4-12 pairs, 5-20 iterations, medians. The shared machine kept a load of 4-9 from the parallel bzip2
benchmark, so ranges are wide; callgrind instruction counts and best-of runs were used where wall-clock
was too noisy.

### Accepted

1. **Wide-peek literal runs** (`55cd7126`): one 56-bit peek decodes a whole run of short literals; the
   window position is local and the run stops before length/EOB symbols and long codes, which the scalar
   path handles. Base64 168 -> 399 MB/s (2.38x, 8/8).
2. **Coverage gate and scalar/batched dispatch** (`8f3a9877`): batch only when one literal code length
   covers at least 90% of the LUT and the scalar loop has seen four literals; dispatch once per block so
   non-batched trees execute the unchanged scalar loop.
3. **Constant-stride dominant-length loop** (`8f3a9877`): with a fixed stride, four table indices are
   arithmetic and the four loads are independent, which breaks the load -> code length -> next index
   chain. Paired all-mode runs (8 pairs, base64, both modes decoded in the same run so they see the
   same load) against base: whole no-window stream 3.46x (range 3.36-4.00), 128 KiB first chunk 3.48x,
   whole with-window stream 3.62x (range 3.15-4.12), 32 KiB marker-first 2.80x and 32 KiB usage 2.73x;
   all 8/8. The 32 KiB cases are header-bound (three block headers per 32 KiB), not
   literal-loop-bound. Full `benchmarkInflate` (GzipReader): base64 2.91x (6/6), Random 1.11x (6/6),
   random backreferences 1.08x (4/6), fixed Huffman 1.06x (4/6), overlapping backreferences 1.03x
   (4/6). Zeros is an RLE/memcpy case at 8-10 GB/s and stayed within its noise band.

### Negative: unconditional batching on text

Commit `55cd7126` batched every literal run. Callgrind on `text6m.gz` (`benchmarkGzipDecode --sequential`)
grew from 190.7M to 215.6M instructions (+13%) because text literal runs are short and each run paid the
56-bit peek. Wall-clock text medians were 0.96x (4/8 pairs) and 0.91x (3/8) in the two A/B runs. The
coverage gate + dispatch brings text6m to 197.2M instructions; the batched loop is never entered for these
trees. The residual +3.4% is the local-window scalar loop, which removes the per-literal
`m_windowPosition` member round-trip and was kept.

### Register-resident first-level table: not the right mechanism here

For random base64, `m_lutBitsCount` is 7 (63 six-bit symbols plus two seven-bit ones), so the full
first-level table is 128 entries (512 B) and already L1-resident. The profile showed the table load is not
the cost; the load -> code length -> next index dependency chain is. A 4/6/8-bit switch or a tiny
register cache cannot hold the 6-bit alphabet without a second level and adds a tag/branch per symbol.
Issuing the loads independently from a fixed stride is what recovered the throughput.

### Next bottleneck

After `8f3a9877`, on base64 the process split is about 57% in `readInternalCompressedLoop<true>`, 12% in
`BitReader::peek2` (bit-buffer refills), and 5% in block-header construction; the rest is benchmark input
generation/compression. For the full literal stream the next single idea is to carry the bit buffer across
`decodeLiteralRun` calls so the refill moves off the literal path, but earlier persistent caller-side
windows regressed (rows 11 and 26), so it should only be retried on top of the constant-stride loop.
For the 32 KiB first/usage cases, block-header and table construction are the next limit.

### Accepted: AVX-512 literal decode and the local bit window (2026-11)

The custom no-ISA-L literal path now decodes eight dominant-length literals per group with one
`vpmultishiftqb` and one `vpermb`, from a local 128-bit window over the BitReader's unread bytes. The
sentinel rejection path emits the group's dominant prefix in bulk, so one 7-bit code costs one scalar
step and the next group usually runs in SIMD again. The dispatch is per read call: the AVX-512 entry
point is a target-attributed wrapper, so the intrinsics inline into `readInternalCompressedLoopImpl`.

Final paired all-mode runs on CPU 15, ISA-L off, base64, 64 MiB, 6 alternating pairs, 6 iterations,
medians and ranges against the original custom path `1f506834`:
- no-window 128 KiB first chunk 7.95x (the low-load run gave 8.34x, range 7.9-8.7)
- no-window whole stream 7.65x
- 32 KiB window whole stream 7.77x
- GzipReader end-of-block-header 7.47x
- 32 KiB marker-first 6.16x and usage 5.72x (still conversion/header-bound)
- full `benchmarkInflate` Random Base64 4.76x vs base, 1.61x vs the 3x tip; Random 0.93-0.98x,
  fixed Huffman ~1.0x, random backreferences 1.05x, overlapping 1.06x, zeros ~1.0x.
The absolute base64 no-window-full throughput is 1.25-1.42 GB/s depending on the machine load (the
original custom path is 0.16-0.19 GB/s on the same load).

Negative experiments on top of this loop, all reverted and not kept in the branch:
- Two independent SIMD groups per iteration (16 literals, two chains from the same window): 0.85x on
  no-window-full. The extra `ensureBits(96)` refills and the second validation outweigh the overlap.
- Ten literals per group using `vpmultishiftqb` control bytes 8 and 9: 0.75x on no-window-full. The
  ninth/tenth byte extraction and validation cost more than the 20% fewer groups save.
- Sentinel detection through an AVX-512 mask register and a struct return: 0.58x. The vector compare
  plus mask materialization generated much worse code than the scalar borrow mask.
- Storing the group from the vector register (`movq`) instead of a GPR `vmovq`: 0.63x for the byte
  window (neutral for the 16-bit marker window). The vector store path did not shorten the branch chain.
- Removing the now-dead four-wide fallback from the SIMD loop: 0.975x. The shorter loop changed the
  compiler's scheduling for the worse.
- `__restrict__` on the window pointer and hoisting the table/control constants: neutral.
- Using the local-window loop for non-SIMD trees (dominant length above six): `benchmarkInflate` Random
  fell from 1.6 GB/s to 1.1 GB/s; row 39 restores the wide-peek decoder for that case.

### Accepted: AVX2 fallback (2026-11)

The AVX-512 path is unavailable on Zen 2, so the same eight-literal group is decoded with AVX2:
two `vpsrlvq` extract the fields from the 64-bit word (four lanes each), one `vpshufb` packs their
low bytes, and four 16-byte `vpshufb` lookups against the 64-byte symbol table are joined by three
`vpblendvb` selects using the index bits 4 and 5. The sentinel byte 0xFF, the four-wide fallback,
the local `UnreadBitsView` window, and the refill policy are unchanged. The loop is instantiated
twice, in target-attributed AVX-512 and AVX2 wrappers, and dispatched once per read call. Text and
other non-concentrated trees still use the wide-peek scalar loop.

Measurement: `benchmarkCustomInflate`, base64, 64 MiB, ISA-L off, CPU 15, 8 alternating pairs,
6 iterations, medians of per-pair ratios against the rebuilt commits `1f506834`, `6ba42ef8`, and
`65e6f9be` (the machine carried a load of 6-10; ranges were wide, all 8/8 wins):
- AVX2 vs scalar tip / original: no-window-first 2.04x / 5.80x, no-window-chunk 1.81x / 6.35x,
  no-window-full 1.82x / 6.11x, window-full 1.80x / 6.09x, usage 1.61x / 4.09x,
  gzip-reader 1.74x / 6.01x.
- AVX2 vs the AVX-512 path: 0.76-0.86x, so AVX2 keeps about 80% of the AVX-512 gain.
- The all-mode AVX-512 comparison was neutral within the noise band (ours-avx512/avx512
  0.92-1.06x); the AVX-512 loop only gained an `if constexpr` selection and one byte load.

`benchmarkInflate` with the same binaries, 2 alternating rounds under load 6-10 (medians,
Random Base64 is the only case that exercises the path): base 106.5, scalar 322.5, AVX-512 562.5,
normal dispatch 556.5, forced AVX2 529.5 MB/s. Random, fixed Huffman, backreferences, and
overlapping backreferences were within the reaction noise; the benchmark itself reports wide
`+-` bands at this load.

The AVX2 path uses `avx2,bmi2` and falls back to the wide-peek loop when either is missing (or when
the symbol table cannot represent literal 0xFF). `RAPIDGZIP_SIMD_LITERAL_DECODE=avx2|avx512|scalar`
forces a path for measurements only; unset means AVX-512, then AVX2, then scalar.

For a Zen 2 user the custom literal path should land around 4-6x the original custom inflate on
base64, versus the 3.46x scalar fallback; this test ran on Zen 4, whose 256-bit shuffle throughput
is higher, so the upper end is optimistic. AVX2 also avoids the AVX-512 frequency effects.

Next bottleneck: the sentinel check and the `vpermb -> vmovq -> borrow-mask` dependency chain still
account for about a third of the SIMD loop. A supergroup that decodes 32 symbols from three input
qwords (192 bits is the least common multiple of 6 and 64) would remove the per-group branch but needs
a cross-qword field fixup. For the 32 KiB first/usage cases, the remaining cost is block-header and
table construction, not the literal loop.
