# bzip2/gzip decode research: papers, inverse BWT, precode and Huffman caches

Read-only research. No tracked files were modified, nothing was built or benchmarked. Sources:
`knowledge/bzip2/*.pdf`, libsais 2.12.1 (`tmp/bzip2-refs/libsais`, submodule `c284ba8`), the GPU
bzip2 reference decoder (`tmp/bzip2-refs/bzip2gpu`), libdeflate/zlib-ng/ISA-L under `repos/`,
the earlier inverse-BWT investigation notes, and the current profiles.
Estimates below are explicitly marked.

## Executive summary

| rank | mechanism | where | estimated effect | risk | new dependency |
|---:|---|---|---|---|---|
| 1 | Cap concurrent libsais P tables at 4, or decode 4 blocks with 2 libsais threads each | `bzip2.hpp:1070`, `BZ2BlockFetcher.hpp:130` | P8 +10-25%; P1 0 | medium (scheduling; OpenMP for 2-thread groups) | OpenMP if grouping |
| 2 | Pack P as 28-bit `(next_position << 8) | second_byte`, move chain marks to a side bitmap | `libsais.c:12466-12560`, `bzip2.hpp:641` | P1 0-5%; P4/P8 +5-12% | medium (bit unpacking, marks) | none |
| 3 | Tune libsais chain count, chunk size and huge pages for a 900k block | `libsais.c:55-64`, `bzip2.hpp:641` | +/-5% P1, +2-8% P8 if TLB matters | low | none |
| 4 | Two-symbol bzip2 Huffman LUT (one lookup yields two literals) | `bzip2.hpp:416-420`, `:927-1035` | text +10-18%, base64 +3-8% single-core | medium | none |
| 5 | libdeflate-style register-held/preloaded first-level decode entry for custom gzip inflate | `repos/libdeflate/lib/decompress_template.h:342-430` | 1.2-1.5x on literal-heavy custom path | medium (register pressure) | none |

The two strongest factual conclusions are negative:

- **Precode/tree-shape caching cannot pay for bzip2 decode.** Header/selectors/tree construction is
  ~0.007 s of a 0.66-0.72 s decode (about 1%); tree construction itself is a small fraction of that.
  The hot cost is the 12-bit LUT lookup repeated for every symbol, not constructing the LUT. The
  remembered "1826 shapes" does not match the code: deflate precode has **1526** valid histograms in
  `precode.hpp:100`, and bzip2 has no precode at all.
- **The GPU inverse-BWT algorithm does not map to CPU SIMD.** The 2024 GPU iBWT is a stable sort plus
  Helman-Jajá list ranking plus scatter. It wins on a GPU by massive thread parallelism, but needs
  four n-sized 32-bit arrays plus two n-byte copies per block; on a 900k block that is ~14-18 MiB per
  block, worse than the 3.6 MB libsais P table. A CPU port would be memory-bound and slower than
  libsais.

Status: the ranked ideas above were later measured; Section 5 has the outcomes. Only the 8160-byte
libsais chunk size and per-worker inverse-BWT context reuse survived. For the aspirational targets:
single-core 4x is not reachable with this decoder design (the text phase floor is Huffman 0.25 s +
libsais 0.22 s + RLE/CRC 0.16 s). Full-core 4x over the new single-core would require better P8
scaling than the measured 3.0-3.3x, and the concurrency experiment did not provide it.

## 1. Papers and citations

### 1.1 What is directly about inverse BWT

**A. Weißenberger and B. Schmidt, "Massively Parallel Inverse Block-sorting Transforms for bzip2
Decompression on GPUs", ICPP 2024, pp. 856-865, DOI 10.1145/3673038.3673067.**
Source: `github.com/weissenberger/bzip2gpu` (cloned under `tmp/bzip2-refs/bzip2gpu`).

This is the only recent paper found that targets bzip2 *decompression* rather than BWT construction. It
is also the paper the CUDA-to-SIMD hypothesis should be tested against. The code shows what it actually
does:

- `src/ibwt.cu:203` `batch_iBWT`: for a segment of up to 800 blocks, copy the BWT bytes, stable-sort
  `(BWT byte, index)` pairs per block with CUB, mask the tail, run list ranking, scatter.
- `src/ibwt.cu:85` `helman_jaja_local`: each thread walks a sublist with stride `LIST_DISTANCE=32`,
  writing a local rank; `:139` `helman_jaja_reduce` composes the sublists; `:177` `add_permute`
  scatters each row to its final output position.
- `src/imtf.cu:192` `private_perm` / `:229` `private_perm_2`: inverse MTF on the GPU is not a list
  shift. Each block gets a private 256-byte permutation, and the block permutations are composed with
  a parallel prefix scan (`concat` at `:52`, up/down-sweep kernels).

Transfer assessment:

- The iBWT is a **sort + list-ranking + scatter**, not a faster LF chase. It removes the dependent
  load chain by computing all output positions in parallel. On CPU, the same decomposition needs
  `perm`, `perm_alt`, `sublist_ids`, `ranks`, `reduced_ranks` (5 x 3.6 MB) plus the text/output. That
  is 18 MB per block before T/U, so 8 workers would use ~150 MB of working set. This is strictly worse
  than libsais's one 3.6 MB P table for this machine. **Do not port the GPU list-ranking algorithm to
  a single CPU core.**
- The iMTF is even less suitable: updating a 256-byte private permutation per symbol is 4x the
  current 64-byte SSE2 front-list update, and composing permutations adds a prefix scan. It wins on a
  GPU because the permutations of thousands of blocks are independent, not because the per-symbol work
  is cheaper.
- The paper is still useful as evidence for *why* libsais's multi-chain approach is the right CPU
  shape: both exploit many independent successors, but libsais keeps one shared P table and 24 chain
  pointers instead of materializing a rank for every element.

The paper's forward citations (Semantic Scholar, 2026) are FPGA/ML-compression papers and do not
contain a new CPU inverse-BWT algorithm. Its backward citations include "Truly parallel
Burrows-Wheeler compression and decompression" (2013, DOI 10.1145/2486159.2486164), which is the
work-optimal PRAM formulation (decompression depth O(log n)) underlying list ranking, and "Massively
Parallel Huffman Decoding on GPUs" (2018, DOI 10.1145/3225058.3225076), which uses Huffman
self-synchronization, not a register table.

### 1.2 What is about BWT construction and does not transfer

**F. Kurpicz and M. Paris, "Engineering Lightweight Burrows-Wheeler Transform Construction on the GPU
via Prefix-Free Parsing", arXiv:2610.33889, 2026.**

Prefix-free parsing (PFP) replaces the suffix sort by a phrase dictionary and a parse. The paper's
contribution is a GPU formulation using only sorts/scans and a lower memory footprint. It says nothing
about inversion. Two implementation ideas are nevertheless relevant as validation of libsais's design:

- **MSD bucketing by the first two characters** (`Sec. 6.1`, around "presort by the first two
  characters (65,536 buckets)") bounds the largest bucket and improves locality. libsais's bi-gram
  P table (`libsais_unbwt_calculate_biPSI`, `libsais.c:10899`) is exactly this idea applied to
  inversion: P is grouped by the 256x256 bigram, so the 2-symbol decode can use `bucket2`/`fastbits`
  instead of a second random lookup.
- **Chunked in-place output** (`Sec. 6.2.1`): process `K=256` chunks of at most `max(bmax, s/K)`
  elements and apply a per-chunk permutation in place. libsais already does the analogous thing with
  `UNBWT_CHUNK_SIZE=16320` and `UNBWT_CHUNK_SIZE_SMALL=4080` (`libsais.c:64-65`). The transferable
  action is to tune those chunk sizes, not to add a new algorithm.

**J. Khan and P. Pandey, "Cascade: GPU-Accelerated Out-of-Core Suffix Array Construction for LLM
Corpora", arXiv:2610.15254, 2026.**

Cascade builds suffix arrays, not inverse BWT. Its ideas are output-range partitioning with sampled
splitters, "streamed rank exchange" (each rank has at most one reader, so send it instead of keeping a
rank table), and tiered residency across GPU/host/SSD. The analogy to inverse BWT would be to replace
the random P table with a sequential scan plus a blocked list ranking, but that is the GPU iBWT idea
above, with the same memory problem. The paper is useful only as a high-level confirmation that
"partition by output range, not by input range" can improve locality; it does not give a CPU
inverse-BWT algorithm.

**J. Khan, T. Rubel, E. Molloy, L. Dhulipala, R. Patro, "Fast, parallel, and cache-friendly suffix
array construction", Algorithms Mol Biol 19:16, 2024, DOI 10.1186/s13015-024-00263-5.**

CAPS-SA is a samplesort-style parallel suffix-array constructor with LCP-informed merging and AVX
vectorized LCP computation. It is a reference of both 2026 arXiv papers. Its lesson is that a simple,
cache-friendly partitioning (sorted subarrays, sampled pivots, merge with LCP) beats more complex
algorithms. For bzip2 inversion, the corresponding simple/cache-friendly structure is already libsais's
bigram bucketing; CAPS-SA offers no inversion mechanism.

**D. Díaz-Domínguez, L. Egidi, V. Guerrini, F. A. Louza, G. Rosone, "Algorithms for Computing Very Large
BWTs: A Short Survey", Manzini's Festschrift, OASIcs, 2026.**

The survey defines LF-mapping (`Sec. 2`, around line 236) and observes that the multi-string BWT's LF
permutation decomposes into k cycles, one per string (Example 2, around line 277). For a single bzip2
string the LF permutation is one cycle (or a few cycles for periodic data), which is why intra-block
parallelism needs checkpoints/marks. The survey covers external-memory and PFP construction, not
inversion; its future-work section explicitly lists GPU BWT construction and PFP, not inverse BWT.
Useful quote for the cache problem: the 2025 thesis "On Data Structures for Texts and Permutations"
states that "LF-steps are known to be not very cache-efficient due to unpredictable jumps in the data
structure" (line 1326 of the extracted text). That is the problem libsais's multi-chain bi-gram table
mitigates.

### 1.3 Papers that are about adjacent structures only

| paper | year / ID | relevance |
|---|---|---|
| J. Y. Xie, G. Nong, B. Lao, W. Xu, "Scalable Suffix Sorting on a Multicore Machine" | IEEE TC 2020, DOI 10.1109/TC.2020.2972546 | blockwise induced sorting, 4-stage pipeline, prefetching; supports a two-stage Huffman/libsais pipeline, no inversion |
| N. Timoshevskaya, W. Feng, "SAIS-OPT" | ICCABS 2014, DOI 10.1109/ICCABS.2014.6863917 | characterizes irregular memory access in SA-IS; libsais's README cites it; no inversion |
| J. Kärkkäinen, G. Manzini, S. J. Puglisi, "Permuted Longest-Common-Prefix Array" | CPM 2009, DOI 10.1007/978-3-642-02441-2_17 | Phi/PLCP linear scan; permutation traversal in position order, but no BWT inversion |
| "Lightweight LCP-Array Construction in Linear Time" | arXiv:1012.4263, 2010 | PLCP/LF-mapping definitions, no inversion |
| Z. Lipták, F. Masillo, S. J. Puglisi, "Suffix sorting via matching statistics" | Algorithms Mol Biol 2024, DOI 10.1186/s13015-023-00245-z | small working set / locality for construction, no inversion |
| G. Nong, S. Zhang, "Efficient Sorting Suffixes of Big Alphabets" | DCC 2024, DOI 10.1109/DCC58796.2024.00035 | O(n) time, O(sqrt n) workspace; no inversion |
| "Linear Time Suffix Array Construction Using D-Critical Substrings" | 2009 | SA-IS ancestry, no inversion |
| G. Nong, S. Zhang, W. H. Chan, "Two Efficient Algorithms for Linear Time Suffix Array Construction" | IEEE TC 2010, DOI 10.1109/TC.2010.188 | libsais ancestry, no inversion |
| "Building and Checking Suffix Array Simultaneously by Induced Sorting Method" | 2022 | construction/verification, no inversion |
| "Efficient Algorithms to Compute Closed Substrings" | 2025 | string combinatorics, no BWT decode |
| "Enhancing HDFS with a full-text search system for massive small files" | 2021 | systems paper, no BWT decode |

Citation-following result: the two 2026 arXiv papers have zero forward citations at the time of
writing, and their backward references are construction papers (PFP, CAPS-SA, libcubwt, matching
statistics). The 2024 GPU iBWT paper's forward citations are FPGA/ML compression, not CPU inverse BWT.
The CPU inverse-BWT literature is thin; libsais and the bzip2 reference decoders are ahead of it.

## 2. Precode Huffman tree caching (bzip2 and deflate)

### 2.1 Terminology

bzip2 has no precode. Its block header stores code lengths directly for 2-6 Huffman groups, each with
up to `symbolCount + 2` symbols (`Block::readTrees`, `bzip2.hpp:879-923`). The "precode" concept is
deflate: a 19-symbol code-length alphabet with maximum code length 7, used to encode the literal and
distance code lengths. The remembered "1826 architectures before alphabet translation" is therefore
most likely a deflate-precodes observation, not bzip2.

### 2.2 The 1826 number does not match the code

`src/rapidgzip/gzip/precode.hpp:100` has `static_assert( VALID_HISTOGRAMS_COUNT == 1526 )`. The
valid-histogram table counts full binary trees of depth <= 7 with <= 19 leaves, plus the single-symbol
case. Reproducing the count from first principles gives 1525 full histograms + 1 single-symbol = 1526.
The number 1826 is not produced by the obvious variants (all histograms with Kraft <= 1: 12191;
exactly 19 leaves: 271). It may be a misremembered earlier count. The code's 1526 is authoritative.

For bzip2, a shape cache is even less plausible: with maximum code length 20 and up to 258 symbols,
the number of valid full-tree length histograms is enormous (the exact-Kraft count with <= 258 leaves
is about 1.78e19). There is no small 1826-element universe to cache.

### 2.3 Quantified share

From the earlier inverse-BWT investigation notes:

| workload | decode total | header/selectors/trees | share |
|---|---:|---:|---:|
| base64 | 0.66 s | 0.007 s | 1.1% |
| text | 0.72 s | 0.007 s | 1.0% |

Tree construction is only a part of that 0.007 s. The `readTrees` timer (`bzip2.hpp:879`) includes
reading delta-coded code lengths and `HuffmanCoding::initializeFromLengths`, which fills a 12-bit LUT
of 4096 4-byte entries per group (`src/huffman/HuffmanCodingShortBitsCached.hpp:75-120`). Up to six
groups means up to 96 KiB of LUT writes per block, but that is amortized over ~18,000 Huffman groups
of 50 symbols. The scorep example in `bzip2.hpp:346-356` shows the scale: `readTrees` 0.0169 s,
`createHuffmanTable` 16.84 s (the timer name is misleading; it wraps the symbol decode loop), and
`burrowsWheelerPreparation` 1.09 s. Tree construction is ~0.1% of that header time, not a decode
lever.

### 2.4 Why caching shapes/subtrees cannot help the hot loop

- Caching the *shape* (length histogram) saves `initializeFromLengths`, but not the 96 KiB LUT fill,
  and the LUT fill is already the larger part of tree construction.
- Caching the *final LUT* keyed by (shape, symbol-to-byte translation) would need up to 16 KiB per
  entry. Even 64 entries is 1 MiB per worker; 8 workers is 8 MiB of L2/L3 pressure, for at most ~1%
  of runtime. The alphabet translation changes per block, so hit rate is corpus-dependent.
- Caching *subtrees* and applying translation after lookup adds a byte lookup and a branch to the
  hottest loop (one 12-bit LUT load per symbol). The LUT-size sweep (10/11/12/13 bits) already showed
  no measurable effect, so there is no headroom there.
- The bzip2 LUT is built once per block and read millions of times. The decode cost is the lookup and
  the MTF/run state machine, not construction.

Deflate is the same story: `readDynamicHuffmanCoding` is ~6.75% of gzip decode, 91% of that is ISA-L
literal-table construction, and precode construction is ~0.16% of total. A last-value code-length cache
never hit in prior measurements. The valid-histogram LUT already exists and is compile-time
run-length-encoded; more shape/subtree caching is a dead end.

### 2.5 The algorithmic trivial validity check exists

The remembered check is in the code and is already the cheap path:

- `src/huffman/HuffmanCodingBase.hpp:72-110` `checkCodeLengthFrequencies`: start with
  `unusedSymbolCount = 1 << minCodeLength`; for each length, reject if `frequency > unusedSymbolCount`,
  then `unusedSymbolCount -= frequency; unusedSymbolCount *= 2`. If `CHECK_OPTIMALITY` is true, require
  `unusedSymbolCount == 0` (or the single-symbol special case). This is a virtual-leaf count, no tree
  is built.
- `src/huffman/HuffmanCodingBase.hpp:219-240` `checkHuffmanCodeLengths` is the simpler variant used by
  the ISA-L wrapper: sum `1 << (MAX_CODE_LENGTH - codeLength)` and compare with `1 << MAX_CODE_LENGTH`,
  with a special case for a single code.
- `src/rapidgzip/blockfinder/precodecheck/CountAllocatedLeaves.hpp` and
  `src/rapidgzip/gzip/precode.hpp` implement the same idea for the deflate precode and use the 1526
  valid histograms.
- bzip2's `Block::HuffmanCoding` sets `CHECK_OPTIMALITY=false` (`bzip2.hpp:416-420`), so only the
  overflow check runs. It is already O(symCount) and is not worth simplifying further.

## 3. Inverse BWT: cache and SIMD ideas

### 3.1 Current implementation and working set

The project calls `libsais_unbwt_cyclic` at `bzip2.hpp:1070`:

```cpp
libsais_unbwt_cyclic( bwtData.get(), bwtData.get(),
                      reinterpret_cast<int32_t*>( dbuf.get() ),
                      static_cast<int32_t>( writeCount ), frequencies.data(),
                      static_cast<int32_t>( origPtr ) );
```

`dbuf` is `new uint32_t[MAX_BLOCK_SIZE + 1]` (`bzip2.hpp:641`), i.e. 3.6 MB for a 900k block;
`bwtData` is 0.9 MB (`:644`). For `n = 900k`, libsais uses the `symbol_rows` path because
`n <= UNBWT_SYMBOL_ROWS_MAX_SIZE` (8,388,607). In that path each P entry is a packed 32-bit value:

- bits 8..31: next position (20 bits used for 900k)
- bits 0..7: second output byte
- bit 31 is also used as a chain mark (`UNBWT_MARK`, `libsais.c:71`)

The decode loop is `libsais_unbwt_decode_chains_symbol_rows` (`libsais.c:12466`):
`q = P[p]`, write the first symbol from `first_symbols[p >> first_shift]`, write `(uint8_t)q`,
then `slot->p = q >> 8`. It prefetches `P[q >> 8]` one step ahead (`:12496`). There are
`UNBWT_CHAINS=24` slots (`libsais.c:56`) and a chunk size of 16,320 bytes (`:64`). For a 900k block
this is already a well-tuned 2-output-bytes-per-P-load kernel.

Measured phase cost: base64 0.157 s, text 0.314 s. At P8 the profile is roughly libsais setup 22% +
chain decode 18% = 40%. The P table is the largest per-worker working set.

### 3.2 Idea: cap concurrent P tables / two-thread block groups

This is the direct response to the documented P8 scaling limit. Eight workers each have a 3.6 MB P
table = 28.8 MB, plus T/U and libsais's `bucket2`/`fastbits`/marks, against a shared 16 MiB L3.
Current P8 scaling over the new single-core is ~3.0-3.3x (text 394.4 MB/s vs 121.35; base64 272.5 vs
89.67), not 4x.

Two variants:

1. **Semaphore gate (no new dependency).** Allow at most 4 concurrent `libsais_unbwt_cyclic` calls in
   `BZ2BlockFetcher::decodeBlock` (`BZ2BlockFetcher.hpp:130`) or in
   `Block::BurrowsWheelerTransformData::prepare` (`bzip2.hpp:1070`). The other 4 workers continue
   Huffman/RLE work. This tests whether fewer active P tables improve L3 hit rate. Risk: a worker that
   has finished Huffman and waits on the gate is idle; if the Huffman/RLE phases are not balanced, the
   gate can hurt. A global `std::counting_semaphore<4>` plus a counter of active libsais calls is a
   small, A/B-friendly change.
2. **Two threads per block (needs OpenMP).** Enable `LIBSAIS_OPENMP` in the libsais build (currently
   `src/CMakeLists.txt:148-156` compiles libsais without `LIBSAIS_OPENMP`; the submodule's CMake option
   is `LIBSAIS_USE_OPENMP`, default OFF). Run 4 blocks at a time with
   `libsais_unbwt_cyclic_omp(..., threads=2)`. Only 4 P tables are active (14.4 MB), and each block's
   construction and decode use 2 threads. This is the architecturally correct version of the same
   experiment, but it needs a two-stage pipeline or a 4-task wave to avoid oversubscription.

Expected: P1 neutral; P8 +10-25% if L3 capacity is the cause. If P8 improves less than ~10%, the
L3 hypothesis is weak and the remaining serial fraction is block finder/consumer overhead.

Composition with `ParallelBZ2Reader`: the semaphore version composes with the current worker pool
directly. The 2-thread version needs a pipeline split (Huffman workers -> libsais/RLE workers) to be
effective; simply nesting OpenMP inside 8 workers would oversubscribe.

### 3.3 Idea: 28-bit packed P with side marks

Because `n <= 900,000 < 2^20`, the 20-bit next position plus the 8-bit second byte is 28 bits. The
current entry uses 32 bits, with 4 unused bits and bit 31 reserved for marks. Move marks to a side
bitmap or a small mark array (there are only ~4096 marks) and store P as 28 bits:

- 8 entries = 28 bytes; 900k entries = 3.15 MB instead of 3.6 MB (12.5% smaller).
- Random access: load 8 bytes at byte offset `index * 7 / 2` (i.e. `index * 28 / 8`), shift by
  `(index * 28) & 63`, mask 28 bits. On x86 this is one unaligned 64-bit load, a shift and an AND;
  because 28 does not divide 64, an entry can straddle a cache line, but the load is still one or two
  cache lines at most (the same as a 4-byte load that straddles).
- Construction: `libsais_unbwt_calculate_biPSI_marks_symbol_rows` (`libsais.c:11263`) writes packed
  32-bit entries; a custom variant would pack 28 bits and record marks separately. This is a libsais
  patch or a small custom decoder.

Expected: P1 0-5% (one worker's 3.15 MB already fits 16 MiB; the extra shift may cancel the gain);
P4/P8 +5-12% (28.8 -> 25.2 MB for 8 workers). It does **not** cross the ~2 MB-per-worker threshold
needed to fit 8 workers in 16 MiB. The entropy limit for a general permutation of n elements is about
2.32 bytes/entry, so 28 bits is close to the limit without exploiting the sorted-bucket structure.

A more aggressive delta-coded P (16-20 bits/entry) could bring a 900k table to ~1.8 MB and fit 8
workers, but random access then needs a prefix sum or periodic anchors. For base64, a bigram bucket has
~14 entries spread over 900k positions, so deltas are ~64k (16 bits) and often need escapes; for text,
common bigrams have small deltas but rare ones do not. A checkpoint every 8 entries plus variable-width
deltas is possible, but the decode gains one dependent load/add per P access. This is speculative and
high risk; do not start here.

### 3.4 Idea: tune libsais chain count, chunk size, huge pages

These are cheap compile-time A/Bs on the existing kernel:

- `UNBWT_CHAINS` (24) and `UNBWT_CHAINS_LARGE` (64) at `libsais.c:56-57`. For n=900k the small path is
  used. Try 8/16/32/48 chains. More chains expose more memory-level parallelism but increase the number
  of concurrent random streams; fewer chains may improve locality at P8. Expected +/-5%.
- `UNBWT_CHUNK_SIZE` (16,320) and `UNBWT_CHUNK_SIZE_SMALL` (4,080) at `libsais.c:64-65`. The chunk is
  written sequentially and then copied; 16 KB is half of a 32 KB L1D. Try 8 KB/32 KB. Expected +/-3-5%.
- Huge pages on `dbuf`: `madvise( dbuf.get(), (MAX_BLOCK_SIZE + 1) * sizeof(uint32_t), MADV_HUGEPAGE )`
  after allocation at `bzip2.hpp:641`. The earlier `MADV_HUGEPAGE` test on the one-step LF `dbuf` was
  neutral, but that was a latency-bound chase. For libsais at P8, TLB pressure is more plausible:
  8 tables x 900 pages = 7200 pages, and the measured dTLB misses rose 2.1x at two blocks in the older
  interleave probe. Expected 0-2% P1, 2-8% P8. Low risk, cheap experiment.

Also: libsais allocates `bucket2` (256 KB) and `fastbits` (~225 KB for n=900k) for the setup, but the
`symbol_rows` decode path only needs `first_symbols`/`symbol_ends`. The setup tables are dead after
`libsais_unbwt_init`; freeing them before the chain decode would save ~0.5 MB per worker. This needs a
small libsais patch (free/`madvise(DONTNEED)` between init and decode) and is worth 2-5% at P8.

### 3.5 Idea: software prefetch

libsais already does `libsais_prefetchr(&P[q])` / `&P[q >> 8]` one step ahead
(`libsais.c:12496`, `:12542`, etc.). The address depends on the current load, so a deeper prefetch would
need the load itself. With 24 chains the hardware already has 24 independent loads; the older
prefetch experiments were neutral. Do not add another prefetch.

### 3.6 Idea: a different inversion algorithm (GPU-style, list ranking)

The only fundamentally different algorithm found is the GPU stable-sort + Helman-Jajá list ranking +
scatter. A CPU port would need, per block:

- a 900k-entry permutation (3.6 MB),
- a second permutation buffer (3.6 MB),
- sublist IDs (3.6 MB),
- ranks (3.6 MB),
- reduced lists/ranks (smaller),

plus the BWT bytes and output. That is ~18 MB per block, versus libsais's 3.6 MB P table. The
algorithm is work-efficient (O(n)) but its random accesses are to several arrays, and the sort itself
is multiple passes over n. On this CPU it would be memory-bound and lose to libsais. It could only be
interesting on a GPU or a very large many-core machine with more bandwidth than cache. **Not
recommended for single-core or P8 CPU.**

A cheaper theoretical variant is to compute the character of each row sequentially (one sequential P
read per row) and then use a blocked list ranking to place outputs. This removes the random P load but
adds several n-sized rank arrays. The same memory objection applies. If someone wants to explore it,
the first measurement should be a standalone 900k-block kernel, not an integration.

## 4. Gzip Huffman register cache

### 4.1 Capacity arithmetic

A DEFLATE literal/length first-level table with 11 bits has 2048 entries. With libdeflate's 4-byte
packed entry that is 8 KiB; the project's 12-bit bzip2 LUT is 16 KiB. Neither fits in registers (a
zmm register is 64 bytes; 32 zmm registers are 2 KiB total). A whole first-level table cannot be
register-resident on x86. What can be register-resident is:

- one **decode table entry** (4 bytes) for the current symbol, and the next one preloaded;
- a tiny **hot-symbol cache** of a handful of `(prefix, length, symbol)` triples;
- the **precode table**, which is only 19 symbols with max length 7: 128 entries x 2 bytes = 256 bytes,
  which fits in four zmm registers.

### 4.2 Concrete published/engineered design: libdeflate's preloaded first-level entry

`repos/libdeflate/lib/decompress_template.h:342-430` is the closest real design to the remembered
idea. The decode table is in L1 (`LITLEN_TABLEBITS=11`, `deflate_decompress.c:370-372`), but the
current entry is held in a register and the next entry is loaded before the current symbol is fully
consumed:

```c
entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
do {
    saved_bitbuf = bitbuf;
    bitbuf >>= (u8)entry;
    bitsleft -= entry;
    if (entry & HUFFDEC_LITERAL) {
        lit = entry >> 16;
        entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];  /* preload next */
        *out_next++ = lit;
        ...
    }
    ...
} while (...);
```

It also decodes up to two extra fast literals per iteration when enough bits remain, and preloads the
offset table entry before the match copy. zlib-ng's `inflate_fast` has the same shape with `here` and
`last` locals (`repos/zlib-ng/inflate.c:488-489`), and ISA-L uses a 12-bit `short_code_lookup` with a
packed `(symbol, length, count)` word (`repos/isa-l/igzip/igzip_inflate.c`, `HuffmanCodingISAL.hpp`).

What this buys: the L1 load (4-5 cycles) is overlapped with the previous symbol's output/backreference
work. It does not remove the load; it hides its latency. libdeflate and ISA-L are typically 1.2-1.5x
faster than a per-symbol `decode()` call on literal-heavy streams.

How to integrate in rapidgzip: the current custom loop is `readInternalCompressedMultiCached`
(`src/rapidgzip/gzip/deflate.hpp:1673`), which already decodes two symbols per lookup through
`HuffmanCodingShortBitsMultiCached::decode` but calls it once per loop iteration. A preload-only
variant would:

1. Add a `decodeFromPeeked(bitbuf)` that returns the entry without seeking the BitReader.
2. In the loop, keep `entry` in a local, consume the current symbol, and immediately preload
   `entry = table[next_index]` before doing the output/backreference work.
3. Keep the existing BitReader window/refill; do not introduce a second 56-bit caller-side window.

That last point matters: the previous fused caller-side-window experiment regressed by 67-70% on
literal-heavy streams because of register pressure (see the gzip optimization notes).
Preload-only is a smaller change and should be measured on the non-ISA-L path first, with
`benchmarkInflate` on base64, fixed-Huffman and backrefs cases.

### 4.3 The "table in registers" variants that are not worth it

- **Precode table in zmm registers.** 256 bytes fit in four zmm registers, and AVX-512 `vpermb`
  can index 64 bytes at a time (four selects for 256 bytes). But precode application is ~0.16% of
  total gzip decode; even a 10x speedup is invisible. Do not implement.
- **Hot-symbol cache in GPRs.** Keep the top 4-8 most frequent literals as `(12-bit prefix, length,
  symbol)` and compare the peeked window branchlessly, falling back to the LUT on a miss. This avoids
  the L1 load on hits, but costs 4-8 compares/selects per symbol. The L1 load is already only 4-5
  cycles and is overlapped by the preload in the design above. Likely neutral or negative; speculative.
- **A 16-entry `vpshufb` table in an XMM register.** Only works if the prefix is 4 bits, but DEFLATE
  minimum code lengths are 7-9 bits. Not applicable.
- **Hardware register-file Huffman decoders.** The 2019 ISSCC GZIP accelerator
  ("A 1.4GHz 20.5Gbps GZIP decompression accelerator...") uses an out-of-order speculative Huffman
  decoder and a multi-write register-file array. That is hardware, not a CPU technique; it is probably
  the source of the "register table" memory if it was not libdeflate's preload.

## 5. Outcome of the ranked ideas

The ideas in Sections 3 and 4 were later measured. This section replaces the original ranking and
"single best next experiment" lists; the detailed outcomes are in `OPTIMIZATIONS.md`.

| idea | outcome |
|---|---|
| Cap concurrent libsais P tables / two-thread block groups | **Negative.** Instrumentation found only 3-4 live P tables, not 8. Limit 2 cut cycles 14% but idled workers and lowered wall throughput (S3). |
| 28-bit packed P with side marks | **Bounded, not a win.** The packed read is within noise at P1/P8; chain decode is only 8.2% of the P8 profile, so the end-to-end bound is 0.8-2.5%. |
| libsais chain/chunk/huge-page tuning | **Negative or neutral.** Chains 8 and chunks 8192/32768 regress; huge pages were not backed and made faults worse; the 8160 chunk is the one retained change. |
| Two-symbol bzip2 Huffman LUT | **Negative.** 1.006x text / 1.007x base64 end to end; the second MTF update exposes the store-to-load chain. |
| Gzip preload-only first-level entry | **Not pursued on this branch.** The gzip work instead implemented AVX-512/AVX2 literal groups and a local bit window; see `../gzip/OPTIMIZATIONS.md`. |
| Delta-coded 16-bit P / GPU list ranking | **Not viable on this CPU.** The memory footprint or dependent loads dominate; see Sections 3.3 and 3.6. |

The P8 scaling limit is the all-core clock drop, wave quantization, and about 10% serial fraction, not
a fixable L3-capacity problem. The inverse-BWT context reuse (7-8% of libsais) was the durable
integration win; see `libsais-optimization.md`.

## Appendix: metadata of the directly relevant sources

- A. Weißenberger, B. Schmidt. "Massively Parallel Inverse Block-sorting Transforms for bzip2
  Decompression on GPUs." ICPP 2024, 856-865. DOI 10.1145/3673038.3673067.
- F. Kurpicz, M. Paris. "Engineering Lightweight Burrows-Wheeler Transform Construction on the GPU
  via Prefix-Free Parsing." arXiv:2610.33889, 2026.
- J. Khan, P. Pandey. "Cascade: GPU-Accelerated Out-of-Core Suffix Array Construction for LLM
  Corpora." arXiv:2610.15254, 2026.
- J. Khan, T. Rubel, E. Molloy, L. Dhulipala, R. Patro. "Fast, parallel, and cache-friendly suffix
  array construction." Algorithms Mol Biol 19:16, 2024. DOI 10.1186/s13015-024-00263-5.
- D. Díaz-Domínguez, L. Egidi, V. Guerrini, F. A. Louza, G. Rosone. "Algorithms for Computing Very
  Large BWTs: A Short Survey." Manzini's Festschrift, OASIcs, 2026.
- "Truly parallel Burrows-Wheeler compression and decompression." 2013. DOI 10.1145/2486159.2486164.
- J. Y. Xie, G. Nong, B. Lao, W. Xu. "Scalable Suffix Sorting on a Multicore Machine." IEEE TC 2020.
  DOI 10.1109/TC.2020.2972546.
- N. Timoshevskaya, W. Feng. "SAIS-OPT." ICCABS 2014. DOI 10.1109/ICCABS.2014.6863917.
- J. Kärkkäinen, G. Manzini, S. J. Puglisi. "Permuted Longest-Common-Prefix Array." CPM 2009.
  DOI 10.1007/978-3-642-02441-2_17.
- libsais 2.12.1, submodule `c284ba8`, `src/libsais.c`; `github.com/IlyaGrebnov/libsais`.
- bzip2gpu reference decoder, `github.com/weissenberger/bzip2gpu`, `src/ibwt.cu`, `src/imtf.cu`.
- libdeflate `lib/decompress_template.h`, `lib/deflate_decompress.c`; zlib-ng `inflate.c`;
  ISA-L `igzip/igzip_inflate.c`, `igzip/huffman.asm`.
