# Inverse-BWT lane interleaving: attribution and history

The in-tree bzip2 decoder walks several marked chains of the last-to-first mapping at once. This note records
the primary source for that interleaving idea, how it maps onto the in-tree code, and why bzip2-specific
decoders only started using it in 2026. It is an attribution and timeline document, not a new measurement
report.

## Short version

- The primary source for interleaving several independent LF traversals on one thread is Juha Kärkkäinen,
  Dominik Kempa, and Simon J. Puglisi, *Slashing the Time for BWT Inversion*, DCC 2012, pp. 99-108,
  DOI `10.1109/DCC.2012.18`.
- libsais has implemented the paper's two constant-factor techniques since 2.4.0 (2021-07-14): a bi-gram LF
  table and eight interleaved block walks. libsais 2.12.0 (2026-10-18) adds the cyclic bzip2 BWT and the
  chunk/chain scheduler.
- The in-tree `src/indexed_bzip2/Bzip2InverseBWT.hpp` uses the interleaving idea with 8/24 lanes and a chain
  decomposition. It does not use the paper's super-alphabet/bi-gram or Precopy techniques. The chain
  decomposition is splitter-based list ranking (Helman and JáJá, 2001).
- pbz2 0.2.0 (2026-09-30) also uses start marks, pieces, and interleaved lanes. Its `NOTICE` does not cite
  the 2012 paper.
- mibz2 0.3.1 (2026-10-27..11-14) is a Rust bzip2 decoder whose `pyproject.toml` describes the inverse BWT
  as "walked as interleaved chains". Its inverse-BWT literature review explicitly records the 2012 paper
  as the root reference, and its 16-64 lane and L1-prefetch sweeps independently arrive at the same policy.
- The 2026 cluster is a bzip2-specific adaptation, not the first implementation of the paper. The paper's
  general inverse-BWT techniques have been in libsais since 2021.

## What the paper contains

The paper makes three contributions:

1. **Super-alphabet / bi-gram LF mapping.** Decode two characters at a time using an `LL` array and
   `LF2[i] = LF[LF[i]]`. This halves the number of dependent random loads and therefore the cache misses.
2. **Several traversals on one thread.** Split the text into `p` parts, reconstruct each part separately, and
   switch between the parts in one thread. While one load is outstanding, the out-of-order engine can execute
   the independent loads of the other parts. The paper uses `p = 8` and reports about a 2x speedup. The paper
   stores the L positions of the part ends in the compressed file.
3. **Precopy.** Detect repeated factors and copy them from an earlier occurrence during inversion. For highly
   repetitive strings this reduces the number of cache misses asymptotically.

The in-tree lane walk is contribution 2. It is not contribution 1 or 3.

## libsais already implements the first two techniques

`libsais` 2.4.0, tagged 2021-07-14, added the reverse BWT. Its source already contains:

- `libsais_unbwt_decode_8()` with eight independent walkers (`i0`...`i7`). The dispatch uses
  `libsais_unbwt_decode_8` while there are more than eight blocks, and the smaller variants for the remainder.
  This is the paper's `p = 8`.
- `libsais_unbwt_compute_bigram_histogram_single()`, `bucket2`, `fastbits`, and the packed `P` table. The
  library README describes this as a "bi-gram LF-mapping technique, which allows for the decoding of two
  symbols simultaneously" and says it reduces cache misses. This is the paper's super-alphabet.

libsais 2.12.0, tagged 2026-10-18, then added cyclic BWT and inverse BWT "e.g. as used by bzip2" and
significantly improved inverse BWT. That is the first version with the chunk/chain form:

- `UNBWT_CHAINS` (24, with a larger variant for big inputs), per-chunk links, and
  `libsais_unbwt_decode_chains()`.
- The inner loop iterates over the active `slots[]` so several chains are in flight in one thread, with
  `libsais_prefetchr(&P[q])` for the next dependent load.

libsais does not cite the 2012 paper. Its README cites Kärkkäinen, Manzini, and Puglisi, *Permuted
Longest-Common-Prefix Array* (2009), which is a different paper. The 2.4.0 code matches the 2012 techniques
closely enough that the 2012 paper is the best primary source, but the lineage is inferred from the
implementation, not from a citation.

## Mapping onto the in-tree decoder

`Bzip2InverseBWT.hpp` marks rows at regular distances, records the successor chain when a walk reaches a
mark, and walks the chains with `FEW_LANES = 8` or `MANY_LANES = 24` interleaved lanes. Each lane has its own
contiguous output buffer, and the loop prefetches the next mapping entry and byte. This is the paper's second
technique.

The differences from the paper are deliberate:

- The paper's `p`-part split needs the start positions stored in the compressed file. A bzip2 block only has
  `origPtr`, so the in-tree code creates independent work by marking rows in the LF table and linking the
  chain segments. That is splitter-based parallel list ranking (Helman and JáJá, 2001). libsais 2.12 and
  pbz2 use the same idea in different forms.
- The in-tree code expands the run records into a plain `(rank, byte)` table and decodes one byte per step.
  It does not build the paper's `LL`/`LF2` bi-gram table. The 8-lane small-block path matches the paper's
  `p = 8`; the 24-lane path matches libsais 2.12 rather than the paper.
- Precopy is not used. The chain decomposition is not repeated-factor copying; it is a way to expose
  independent LF walks without extra metadata.

The correct attribution is therefore a combination: Kärkkäinen/Kempa/Puglisi for the interleaving,
Helman/JáJá for the chain decomposition, Weißenberger/Schmidt (ICPP 2024) for the closest published GPU
splitter implementation, and libsais 2.12 for the closest CPU chunk/chain engineering.

## pbz2

pbz2 0.2.0 was published 2026-09-30. Its decoder in `pbz2-core/src/block.rs` uses `STARTS = 128`,
`START_FLAG`, `walk_in_pieces()`, `Piece`, pages, and a fixed number of lanes. It marks piece starts, walks
them interleaved, and copies the pieces in successor order. That is the same family as the in-tree and
libsais chain walks. pbz2's `NOTICE` credits libbzip2-rs, libdivsufsort, and the bzip2-tests corpus, but not
the 2012 paper.

## mibz2

mibz2 0.3.1 by lmmx is a Rust bzip2 decoder with block-parallel decoding and a multi-chain inverse BWT.
Its `pyproject.toml` describes it as "block-parallel, its inverse BWT walked as interleaved chains, SIMD
kernels chosen at run time". The journal under
`docs/journal/2026-10-27-bwt-inversion-literature/` is a literature review of BWT inversion, and it
records Kärkkäinen, Kempa, and Puglisi, *Slashing the Time for BWT Inversion*, DCC 2012, DOI
`10.1109/DCC.2012.18`, as the root paper (`kkp2012-slashing`). This is the only independent implementation
in the cluster that records the paper in its own literature review; the in-tree code comment now cites it too.

Its journal also reports the same engineering trade-offs as the in-tree decoder. It sweeps 16, 24, 32, 48,
and 64 lanes, keeps 32, and uses an L1 prefetch hint. Dropping the prefetch saves instructions but costs
about 6% wall time at one thread, and `prefetchnta` costs about 33% at four threads, so it keeps L1. That
is the same conclusion as the in-tree 8/24-lane and L1 policy, and the opposite of the NTA experiment.
mibz2 is therefore the fourth fast bzip2 implementation in the 2026 cluster, alongside pbz2, libsais 2.12,
and the in-tree decoder.

## Timeline and why bzip2 adoption was late

The important correction to "the paper was never implemented until now" is that the algorithm is not new to
libsais:

| date | event |
|---|---|
| 2012 | Kärkkäinen, Kempa, Puglisi publish the interleaving, super-alphabet, and Precopy techniques |
| 2021-07-14 | libsais 2.4.0 implements bi-gram and 8-way interleaving for general inverse BWT |
| 2026-09-30 | pbz2 0.2.0 is published with piece marks and lanes |
| 2026-10-18 | libsais 2.12.0 adds cyclic bzip2 BWT and the chunk/chain scheduler |
| 2026-10-27..11-14 | mibz2 0.3.1 is developed with interleaved chains and an explicit DCC 2012 citation |
| 2026-11-09 | libsais 2.12.2 further improves inverse BWT |
| 2026-11-12 | the in-tree lane-walking inverse BWT is added and libsais is replaced |

The bzip2-specific work arrived late for several reasons:

- **Cyclic BWT.** bzip2 has no `$` sentinel; it has `origPtr` and a cyclic block. libsais 2.4.0's inverse BWT
  works from a primary position/index, not from a bzip2 block. The cyclic variant only arrived in 2.12.0.
- **No stored part starts.** The paper's simplest multi-traversal scheme stores the `p` part-end positions in
  the compressed stream. bzip2 has no such field. The chain/mark decomposition (marks, successor links,
  assembly) is the engineering that makes the same latency hiding possible without changing the format.
- **Block size and cache residency.** bzip2 blocks are at most 900 KB. The LF table and output fit in L2/L3
  more easily than the large texts in the paper's benchmarks, so the cache-miss penalty is smaller. The
  interleaving still helps, but the end-to-end gain is diluted by Huffman, MTF, RLE, CRC, and I/O.
- **Amdahl.** bzip2 decoders optimized the entropy stages and block-level parallelism first. The inverse BWT
  becomes the remaining bottleneck only after those are SIMD-optimized, which is what happened in the 2.0.0
  work. The paper's kernel win matters more once the other phases shrink.
- **Citation gap and community separation.** The paper is in the string-algorithms/DCC community. libsais did
  not cite it, and bzip2 implementers traditionally used Seward's `tt`/`dbuf` chase. The idea was hard to
  find from the decoder side.
- **The 2026 cluster.** pbz2, mibz2, libsais 2.12, and the in-tree 2.0.0 landed within two months. This
  is consistent with AI-assisted code analysis and implementation lowering the cost of porting and tuning
  the scheme. pbz2 has an explicit AI policy, mibz2 ships agent-team docs and its own literature review,
  and the in-tree work is a 2026 agent-assisted commit series. This is a plausible accelerant, not a proven
  cause; the algorithmic seed is the 2012 paper, which mibz2 cites explicitly and the other implementations
  reproduce in code.
- **Many-core bandwidth.** The paper's technique is a single-thread latency fix, not a bandwidth fix.
  Interleaving many chains and prefetching their cache lines hides dependent-load latency at low thread
  counts, but at 104 cores the prefetch traffic can saturate memory bandwidth. lbzip2 scales further because
  it parallelizes independent blocks instead of multiplying prefetch streams inside one block. The libsais
  2.12 chain scheduler, the 8/24-lane choice, and the L1/NTA prefetch experiments are attempts to keep the
  paper's latency win without paying the bandwidth cost. This is also the likely explanation for the Xeon
  CTU-13 scaling wall at 8-16 threads.

## Attribution text for the code

The comment in `Bzip2InverseBWT.hpp` should cite the paper for the interleaving and keep the existing
Helman/JáJá citation for the chain decomposition:

```cpp
 * Interleaving several independent traversals of the last-to-first cycle on one thread follows Kärkkäinen,
 * Kempa, and Puglisi, "Slashing the Time for BWT Inversion", DCC 2012, pp. 99-108,
 * DOI 10.1109/DCC.2012.18. libsais uses their super-alphabet/bi-gram variant as well; the chain decomposition
 * below is the adaptation that does not need the traversal start rows stored in the stream.
```

The author's preprint is at `https://www.cs.helsinki.fi/u/tpkarkka/publications/dcc2012.pdf`.

## References

- J. Kärkkäinen, D. Kempa, S. J. Puglisi. *Slashing the Time for BWT Inversion.* DCC 2012, pp. 99-108.
  DOI `10.1109/DCC.2012.18`.
- D. R. Helman, J. JáJá. *Prefix Computations on Symmetric Multiprocessors.* JPDC 61(2), 2001.
- A. Weißenberger, B. Schmidt. *Massively Parallel Inverse Block-sorting Transforms for bzip2 Decompression
  on GPUs.* ICPP 2024. DOI `10.1145/3673038.3673067`.
- M. Burrows, D. J. Wheeler. *A block-sorting lossless data compression algorithm.* 1994.
- P. Ferragina, G. Manzini. *Opportunistic data structures with applications.* 2000.
- libsais 2.4.0 (2021-07-14) and 2.12.0 (2026-10-18); `src/external/libsais/src/libsais.c` and `CHANGES`.
- pbz2 0.2.0 (2026-09-30); `pbz2-core/src/block.rs` and `NOTICE`.
- mibz2 0.3.1 (2026-10-27..11-14); `https://github.com/lmmx/mibz2`, `pyproject.toml`, and
  `docs/journal/2026-10-27-bwt-inversion-literature/`.
