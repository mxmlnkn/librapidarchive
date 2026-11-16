# bzip2 extreme-case corpus, baseline, and optimization tracks

The baseline and phase measurements were taken at `15c73ae5` unless a section says otherwise.
Benchmark binary: `src/benchmarks/benchmarkBzip2`: Release, `-O3 -DNDEBUG`, system zlib/libbz2, ISA-L off, rpmalloc off, tests/tools off, `LIBRAPIDARCHIVE_BZIP2_CRC_SLICES=8`.

Host: AMD Ryzen 7 PRO 8840U, 8 physical cores / 16 logical. SMT siblings are interleaved: CPUs 0/1,
2/3, ..., 14/15. The first baseline used `taskset -c 0-7` for P8, which is only four physical cores. The
integrated results use the correct eight physical cores `0,2,4,6,8,10,12,14`. The host was shared during
the early runs, so medians and ranges are reported. Every number below is measured; estimates are labelled
as estimates.

All data lives under `tmp/bzip2-extremes/`:

| path | content |
|---|---|
| `corpus/*.bin.bz2` | generated corpus |
| `corpus/manifest.json` | seed, source detail, raw/compressed size, ratio |
| `results/generateBzip2ExtremeCases.py` | generator |
| `baseline.log` | benchmark output for all cases |
| `stages.json`, `stage-*.log` | `ibzip2 -v` phase timings |
| `perf-*.txt`, `perf-*.data`, `perf-stat.log` | perf profiles and counters |
| `probes/crc_fill_probe.cpp` | CRC/fill ceiling probe |
| `probes/mtf_probe.cpp` | MTF update probe (current vs 256-byte vector shift) |
| `verify.json` | decoded-size and `bzip2 -dc` CRC verification |

## Results after integration

The performance commits on `perf/bzip2-extremes` were integrated into `master` with rewritten hashes:

| branch commit | change | master commit |
|---|---|---|
| `c4a2892d` | PCLMUL / VPCLMULQDQ non-reflected bzip2 CRC | `3ea75dba` |
| `7f49db2b` | exact decoded-size allocation instead of buffer doubling | `1e491bb2` |
| `8e08fbc9` | prefetch the last block once the block finder is finalized | `4dd3c6a7` |
| `5a84837c` | decode the next Huffman symbol ahead of the MTF update | `0a985900` |

Base is `15c73ae5`, change is the integrated tree. P1 is `taskset -c 0`; P8 is
`taskset -c 0,2,4,6,8,10,12,14`. Six alternating pairs, five iterations (three for `highratio`),
`--mode discard`. The columns are medians in MB/s (10^6 decoded bytes/s); the speedup is the median of the
paired change/base ratios, which is why it can differ slightly from change median divided by base median.

| case | raw MB | compressed B | ratio | P1 base | P1 change | P1 speedup | P8 base | P8 change | P8 speedup |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| zeros | 67.1 | 79 | 849479.29 | 2572.2 | 10709.5 | 4.166x | 763.1 | 2352.2 | 3.114x |
| constant-a | 67.1 | 83 | 808540.53 | 2491.3 | 10516.6 | 4.137x | 772.2 | 2303.7 | 2.990x |
| skewed-pair | 67.1 | 832604 | 80.60 | 983.8 | 1483.1 | 1.444x | 1083.3 | 3006.6 | 2.770x |
| two-symbol | 67.1 | 10744799 | 6.25 | 112.2 | 122.3 | 1.084x | 339.8 | 364.0 | 1.068x |
| dna | 67.1 | 18339189 | 3.66 | 102.2 | 112.5 | 1.102x | 323.8 | 344.5 | 1.049x |
| sixteen-symbol | 67.1 | 34159494 | 1.96 | 115.2 | 119.0 | 1.034x | 329.1 | 337.8 | 1.007x |
| random-256 | 67.1 | 67404026 | 1.00 | 47.5 | 51.3 | 1.082x | 154.3 | 169.8 | 1.098x |
| base64 | 67.1 | 50680897 | 1.32 | 106.8 | 108.6 | 1.018x | 296.7 | 302.1 | 1.026x |
| english | 67.1 | 11881609 | 5.65 | 149.4 | 164.2 | 1.096x | 417.7 | 476.9 | 1.143x |
| mixed-zero-spikes | 67.1 | 60227197 | 1.11 | 52.6 | 55.8 | 1.059x | 166.9 | 183.6 | 1.133x |
| rle1-boundaries | 67.1 | 4406 | 15231.24 | 2240.5 | 6699.4 | 2.952x | 1062.0 | 3883.7 | 3.537x |
| rle2-boundaries | 67.1 | 7822 | 8579.50 | 2145.4 | 5860.8 | 2.729x | 1028.1 | 4079.4 | 4.026x |
| small-blocks | 67.1 | 13441435 | 4.99 | 137.3 | 152.6 | 1.111x | 420.0 | 457.1 | 1.105x |
| many-streams | 67.1 | 12059119 | 5.56 | 150.5 | 163.5 | 1.089x | 423.4 | 449.7 | 1.080x |
| highratio | 1009.1 | 6030199 | 167.34 | 1504.8 | 2852.7 | 1.888x | 2628.9 | 4656.0 | 1.714x |

Observations:

- The constant/run extremes reach 4.14-4.17x at P1 and 3.0-4.0x at P8. P1 is now at the measured
  fill+PCLMUL floor rather than the slicing-by-8 CRC floor.
- `highratio` improves 1.89x at P1 and 1.71x at P8. It spends 57% in `decodeBlock`, so the CRC change is
  the main contributor; exact sizing and last-block prefetch add the parallel part.
- The balanced text/base64/random cases improve 1.02-1.14x at P1 and 1.01-1.14x at P8. `small-blocks`
  and `many-streams` improve 1.08-1.11x.
- P8 zeros is still 2352 MB/s versus 10709 MB/s at P1 (0.22x). The parallel reader materializes about
  64 MB of fresh anonymous pages per decode while the sequential reader expands into a 4 KB hot buffer.
  Streaming block output in `ParallelBZ2Reader` is the follow-up for that gap.
- The naive two-pass batch (decode 50 symbols into `uint16_t[50]`, then consume) was a measured negative:
  random-256 1.00x, mixed 1.00x, dna 0.91x, sixteen-symbol 0.84x, base64 0.79x, english 0.96x. The
  one-symbol lookahead is the version that won.
- The 256-byte SSE2 MTF shift was neutral (11.06 vs 11.27 ns/update), so MTF representation changes were
  not pursued.

Validation: the combined tree passes `testBZ2Reader`, `testParallelBZ2Reader`, `testCrc32`,
`testBzip2Compressor`, `testGzipChunk`, and `testParallelGzipReader` in Release. The bzip2 tests also pass
under ASAN+UBSAN, and all 15 corpus files were decoded at P1 and P8 under ASAN with the block and stream
CRC checks enabled.

## 1. Corpus

Generator command actually used:

```bash
python3 results/generateBzip2ExtremeCases.py \
    --output-dir tmp/bzip2-extremes/corpus \
    --size-mib 64
```

The generator is deterministic per case (`random.Random(seed ^ crc32(case name))`), uses only the
standard library, reduces alphabets with `bytes.translate`, compresses with `bzip2 -9` except the
small-block case (`-1`), and removes the uncompressed intermediate after compression. The text case
uses `tmp/ref-corpus/text.bin`; the generator falls back to a documented seeded English-like template
if that file is absent. The high-ratio case reuses `tmp/highratio-subset.bin.bz2` when present and
falls back to repeated text otherwise. All 15 outputs were decoded with `bzip2 -dc`; exit code was 0
and the decoded size matched the manifest for every case (`verify.json`). The decoder's own per-block
and stream CRC checks run during the benchmarks.

| case | description | stage/feature stressed |
|---|---|---|
| `zeros` | constant `0x00`, 64 MiB | longest RLE1/RLE2 runs, `decodeBlock` fill + CRC, single-symbol MTF |
| `constant-a` | constant `0x41` | same as zeros with a non-zero byte; symbol map path |
| `skewed-pair` | random A/B, about 98.8% A | long RLE1 runs, RUNA/RUNB accumulation, Huffman run symbols |
| `two-symbol` | random A/B, 50/50 | BWT, small MTF indices, RLE2, Huffman |
| `dna` | uniform ACGT | BWT, small MTF indices, RLE2, Huffman |
| `sixteen-symbol` | uniform 16 bytes | MTF positions mostly below 16 (SSE2 fast path), 4-bit entropy |
| `random-256` | uniform 256 bytes, ratio 1.00 | uniform MTF 0..255 (scalar tail), Huffman LUT, libsais |
| `base64` | base64 of seeded random bytes | 64-symbol alphabet, MTF 1..63 (SSE2), Huffman, libsais |
| `english` | first 64 MiB of `tmp/ref-corpus/text.bin` | balanced BWT/MTF/RLE2/output |
| `mixed-zero-spikes` | random-256 with deterministic long zero intervals | mixed random core and long run expansion |
| `rle1-boundaries` | run lengths around 3/4/255/259/260/516/517 | RLE1 4+count encoding and expansion boundaries |
| `rle2-boundaries` | single-byte separators between runs around powers of two | RLE2/RUNA/RUNB run lengths, RLE1 |
| `small-blocks` | English text compressed with `bzip2 -1` | per-block overhead: trees, libsais setup, scheduling |
| `many-streams` | 64 independently compressed streams concatenated | stream headers/EOS, block finder, stream CRC |
| `highratio` | `tmp/highratio-subset.bin.bz2` (6.03 MB -> 1009.1 MB) | RLE1 expansion, `decodeBlock` fill + CRC, high-ratio archives |

Note the special shape of the high-ratio case: a bzip2 block limit applies to the RLE1-encoded stream,
so a 900 kB BWT block can expand to many MB of original data. The 87 data blocks decode to 1009 MB,
about 11.6 MB per block. This is why the output/RLE1 path, not BWT or Huffman, dominates it.

## 2. Baseline

`benchmarkBzip2 --mode discard` keeps the compressed input in memory and does not copy decoded output.
This table is the original baseline: P1 is `taskset -c 0`, P8 is `taskset -c 0-7`, which is only four
physical cores on this SMT-interleaved host. The integrated A/B in "Results after integration" re-baselines
P8 on the eight physical cores. Three iterations per file, two for `highratio`; the
benchmark also performs one warm-up decode. MB/s is `10^6` decoded bytes per second. Ranges are the
minimum and maximum of the measured iterations. GNU bzip2 is 1.0.8 `-dc` pinned to CPU 0; lbzip2 is
2.5 `-dc -n 8` pinned to CPUs 0-7; both are best of 3 (2 for `highratio`), reading the file from the
page cache and writing to `/dev/null`.

| case | raw MB | compressed B | ratio | P1 MB/s median (range) | P8 MB/s median (range) | P8/P1 | GNU bzip2 MB/s | lbzip2 -n8 MB/s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| zeros | 67.1 | 79 | 849479.29 | 2455.4 (2285-2506) | 766.9 (744-768) | 0.31 | 528.3 | 767.3 |
| constant-a | 67.1 | 83 | 808540.53 | 2444.9 (2147-2448) | 721.3 (716-730) | 0.30 | 511.1 | 723.4 |
| skewed-pair | 67.1 | 832604 | 80.60 | 946.6 (944-1002) | 960.0 (919-1107) | 1.01 | 359.6 | 797.0 |
| two-symbol | 67.1 | 10744799 | 6.25 | 107.7 (103-108) | 324.6 (312-325) | 3.01 | 45.1 | 121.1 |
| dna | 67.1 | 18339189 | 3.66 | 99.3 (97-100) | 250.7 (240-278) | 2.53 | 40.8 | 111.7 |
| sixteen-symbol | 67.1 | 34159494 | 1.96 | 110.5 (108-111) | 261.1 (257-262) | 2.36 | 31.9 | 115.8 |
| random-256 | 67.1 | 67404026 | 1.00 | 45.8 (45-48) | 124.8 (118-126) | 2.72 | 24.3 | 94.2 |
| base64 | 67.1 | 50680897 | 1.32 | 105.7 (105-106) | 228.0 (225-232) | 2.16 | 27.4 | 102.4 |
| english | 67.1 | 11881609 | 5.65 | 142.3 (119-146) | 326.0 (285-348) | 2.29 | 66.1 | 254.0 |
| mixed-zero-spikes | 67.1 | 60227197 | 1.11 | 50.0 (50-51) | 141.6 (133-146) | 2.83 | 27.2 | 110.9 |
| rle1-boundaries | 67.1 | 4406 | 15231.24 | 2261.5 (1996-2290) | 1046.5 (1008-1084) | 0.46 | 489.9 | 861.6 |
| rle2-boundaries | 67.1 | 7822 | 8579.50 | 2121.6 (1850-2171) | 1038.2 (1034-1052) | 0.49 | 464.5 | 1035.2 |
| small-blocks | 67.1 | 13441435 | 4.99 | 139.7 (139-141) | 354.9 (342-359) | 2.54 | 77.0 | 455.0 |
| many-streams | 67.1 | 12059119 | 5.56 | 141.2 (138-142) | 377.4 (368-387) | 2.67 | 62.8 | 284.5 |
| highratio | 1009.1 | 6030199 | 167.34 | 1526.4 (1383-1526) | 2670.3 (2667-2670) | 1.75 | 372.5 | 1270.0 |

Observations:

- The run-heavy extremes are much faster than GNU bzip2: zeros/constant-a 4.6-4.8x at P1 and
  rle1/rle2 boundaries 4.6x. They are, however, at the CRC floor (Section 4).
- `random-256` is the slowest P1 case at 45.8 MB/s, 1.9x faster than GNU bzip2 and 2.7x slower than
  the P8 result. Its compressed ratio is 1.00, as intended.
- The balanced text/base64 cases are 2.2x (english) and 3.9x (base64) faster than GNU bzip2 at P1.
  At P8, lbzip2 is 1.3x (english) and 2.2x (base64) behind ours. lbzip2 wins only on `small-blocks`
  (455 vs 355 MB/s P8), where its block scheduler is better at many 100 kB blocks; it ties ours on
  zeros and rle2-boundaries.
- The few-block cases are pathological for P8: `zeros` and `constant-a` are 3.1-4.0x slower with 8
  workers than with 1. `skewed-pair` (7 blocks) is flat, and `rle1/rle2` are about 2x slower. This is
  not a CPU-work problem: Section 4 shows the parallel path spending measurable time in buffer growth.
- `highratio` already scales 1.75x at P8 and reaches 2.67 GB/s because it has 87 large-output blocks.
  Its P1 rate is the second slowest per raw byte after `random-256`.

## 3. Phase breakdown

`ibzip2 -v -P 1 -d -o /dev/null -f <file>` on CPU 0, one decode. The four timers sum to the decode
time (the wall time also includes setup and the block finder). Timer semantics in the current code:

- `decodeBlock`: inverse-BWT output scan, RLE1 expansion, output stores, and the slicing-by-8 CRC over
  the decoded bytes.
- `createHuffmanTable` (misleading name): the Huffman + MTF + RLE2 symbol loop in `readBlockData`,
  filling the RLE1-encoded `bwtData`.
- `burrowsWheelerPreparation`: `prepare()`, i.e. libsais histogram/P-table construction and chain
  decode.
- `readBlockHeader`: symbol maps, selectors, and Huffman tree construction; it is not the symbol loop.

| case | timer sum s | decodeBlock % | Huffman+MTF % | libsais prep % | header % |
|---|---:|---:|---:|---:|---:|
| zeros | 0.028 | 88.3 | 2.2 | 9.6 | 0.0 |
| constant-a | 0.028 | 88.6 | 2.2 | 9.1 | 0.0 |
| skewed-pair | 0.075 | 52.2 | 21.2 | 26.2 | 0.4 |
| two-symbol | 0.601 | 18.3 | 47.6 | 33.4 | 0.6 |
| dna | 0.658 | 6.9 | 61.3 | 30.7 | 1.1 |
| sixteen-symbol | 0.636 | 4.7 | 59.6 | 34.1 | 1.6 |
| random-256 | 1.518 | 1.9 | 75.7 | 21.6 | 0.8 |
| base64 | 0.762 | 4.1 | 53.6 | 40.8 | 1.6 |
| english | 0.501 | 9.8 | 46.4 | 43.0 | 0.9 |
| mixed-zero-spikes | 1.322 | 2.2 | 75.6 | 21.4 | 0.8 |
| rle1-boundaries | 0.034 | 82.2 | 2.8 | 14.8 | 0.1 |
| rle2-boundaries | 0.039 | 77.0 | 4.4 | 18.5 | 0.1 |
| small-blocks | 0.517 | 9.2 | 47.4 | 40.8 | 2.6 |
| many-streams | 0.469 | 10.0 | 47.3 | 41.7 | 1.0 |
| highratio | 0.736 | 56.8 | 17.0 | 25.7 | 0.4 |

The `ibzip2` block finder reported 3 blocks for zeros/constant-a, 7 for skewed-pair, 76-77 for the
64 MiB random/Alphabet cases, 69 for english, 4-5 for the rle boundary cases, 613 for small-blocks,
182 for many-streams, and 87 for highratio. CRC is not a separate timer; it is inside `decodeBlock`.
The probe in Section 4 shows that the CRC, not the output fill, is the bound: fill alone is 18.3 GB/s,
slicing-by-8 CRC is 2.81 GB/s, and fill+CRC is 2.51 GB/s, close to the measured 2.70 GB/s `decodeBlock`
rate for zeros. For a normal text block the CRC is hidden under the MTF/libsais stalls.

The cases split into three clear regimes:

1. **Output/RLE1-bound**: zeros, constant-a, rle1/rle2 boundaries (77-89% `decodeBlock`), and
   highratio (57%). The BWT and Huffman phases are small because the data is repetitive.
2. **Huffman/MTF-bound**: random-256 and mixed-zero-spikes (76%), dna/sixteen-symbol (60-61%),
   two-symbol (48%), base64 (54%), english (46%). libsais is 21-43% and is the second cost.
3. **Per-block/scheduling-bound**: small-blocks (613 found blocks, header 2.6%, libsais 41%) and
   many-streams (182 found blocks, header 1.0%). At P8 these are dominated by block-level parallelism.

## 4. Hotspots and measured ceilings

### perf profiles (P1, CPU 0)

`perf record -e cycles:u` over one warm-up plus one measured decode. Percentages are of user cycles.

| case | decodeBlock | readBlockData | libsais init | libsais decode | histogram | peek2 | readBlockHeader | other |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| zeros | 92.1 | 1.6 | 1.2 | 0.9 | | | | 4.2 |
| two-symbol | 18.8 | 47.9 | 17.5 | 9.0 | 2.8 | 2.1 | | 1.9 |
| random-256 | 1.9 | 69.9 | 14.8 | 3.2 | | 7.3 | | 2.9 |
| small-blocks | 10.2 | 45.9 | 22.8 | 7.8 | 4.3 | 3.5 | 2.5 | 3.0 |
| many-streams | 10.1 | 46.9 | 23.1 | 9.1 | 3.9 | 3.2 | | 3.7 |
| base64 | 4.4 | 50.7 | 23.5 | 8.3 | | 7.2 | | 5.9 |
| english | 10.3 | 45.2 | 24.1 | 10.8 | | 3.5 | | 6.1 |
| highratio | 57.7 | 17.1 | 9.6 | 8.7 | 2.0 | | | 4.9 |

Empty cells are zero. `other` is the unlisted remainder, so each row sums to 100% up to rounding.
`peek2` is `BitReader::peek2`.

`perf stat`, one warm-up plus one decode (counts therefore cover two decodes):

| case | cycles | instructions | IPC | cache misses | branches | branch misses |
|---|---:|---:|---:|---:|---:|---:|
| zeros | 0.277e9 | 0.720e9 | 2.60 | 0.36e6 | 45.3e6 | 0.58e6 (1.29%) |
| two-symbol | 5.50e9 | 14.50e9 | 2.64 | 60.6e6 | 1.694e9 | 71.9e6 (4.24%) |
| random-256 | 13.28e9 | 40.59e9 | 3.06 | 194.8e6 | 4.406e9 | 178.9e6 (4.06%) |
| small-blocks | 4.64e9 | 12.21e9 | 2.63 | 8.3e6 | 1.414e9 | 52.8e6 (3.74%) |
| many-streams | 4.35e9 | 11.13e9 | 2.56 | 47.2e6 | 1.269e9 | 45.9e6 (3.62%) |
| base64 | 6.00e9 | 23.21e9 | 3.87 | 69.2e6 | 2.386e9 | 19.8e6 (0.83%) |
| english | 4.26e9 | 11.00e9 | 2.58 | 50.1e6 | 1.254e9 | 45.2e6 (3.61%) |
| highratio | 6.27e9 | 17.84e9 | 2.84 | 72.2e6 | 1.521e9 | 27.5e6 (1.81%) |

`random-256` executes 40.6e9 instructions for 134 MB of output over two decodes, about 300
instructions per decoded byte. The scalar MTF tail is the mechanism: with a uniform permutation, 75%
of the 67M literal updates take the `position >= 64` word loop, whose trip count is the MTF position.

### CRC and fill ceiling probe

`probes/crc_fill_probe.cpp`, 64 MiB, CPU 0, best of 7. It copies the exact slicing-by-8 CRC from
`bzip2.hpp`.

| operation | ns/byte | GB/s |
|---|---:|---:|
| slicing-by-8 CRC (current) | 0.356 | 2.81 |
| `std::memset` / byte fill loop | 0.055 | 18.3 |
| fill + slicing-by-8 CRC | 0.398 | 2.51 |

The measured `zeros` `decodeBlock` phase is 24.8 ms for 67.1 MB, i.e. 0.370 ns/B or 2.70 GB/s, which
is the fill+CRC floor within noise. A prior project measurement of ISA-L `crc32_ieee_avx2` (PCLMUL) on
this host gives 25.9 GB/s (0.039 ns/B), against 2.19 GB/s for slicing-by-8 in that session. Using the
fresh fill number and that PCLMUL number, the ideal combined floor is
`1 / (1/18.3 + 1/25.9) = 10.7 GB/s`, about 4.3x the current fill+CRC throughput. This is a component
ceiling, not an end-to-end claim; it is the basis for Track A.

### MTF update probe

`probes/mtf_probe.cpp` runs the current `moveToFront` (SSE2 for positions < 64, scalar packed-word loop
above) and a prototype that shifts all 256 bytes with 16 SSE2 lanes for positions >= 64. Uniform random
positions 0..255, 20M updates, CPU 0, best of 5, checksums equal:

| implementation | ns/update |
|---|---:|
| current | 11.27 |
| 256-byte SSE2 shift for position >= 64 | 11.06 |

The 1.9% difference is inside the noise and does not reproduce the hoped-for tail win. The scalar loop
is already close to the cost of a full 256-byte store-to-load round trip. This new negative result
means that a simple ISA-level MTF widening is not a worthwhile track. A fundamentally different
representation (two-level list or order-statistics structure) would be required, and none has a
measured win yet.

### Parallel scaling and the few-block pathology

`benchmarkBzip2 --mode discard`, medians of 3 iterations (2 for highratio):

| case | P1 | P2 | P4 | P8 |
|---|---:|---:|---:|---:|
| zeros | 2506 | 773 | 766 | 633 |
| constant-a | 2335 | 761 | 782 | 752 |
| skewed-pair | 946 | 884 | 1138 | 1071 |
| rle1-boundaries | 2281 | 960 | 1076 | 1030 |
| rle2-boundaries | 2040 | 1022 | 911 | 1058 |
| highratio | 1442 | 1538 | 2152 | 2632 |
| random-256 | 45.9 | 55.0 | 92.2 | 122.8 |
| small-blocks | 136 | 162 | 239 | 375 |
| many-streams | 151 | 165 | 257 | 378 |

The `zeros` P2/P8 profile explains part of the gap: `decodeBlock` 66%, `__memmove_avx512` 18.8%,
`__memset_avx512` 5.2%. The P1 profile has no `memmove`/`memset` entry. In `BZ2BlockFetcher`, the
output buffer starts at 900 kB and doubles until the block fits. A 900 kB BWT block of zeros expands
to about 33 MB, so each block is copied through six doublings and each new allocation is
value-initialized. The measured 24% of cycles is only part of the 3-4x wall-time gap; the remainder is
thread-pool/block-finder waiting, which needs a separate measurement. Note that the benchmark constructs
a fresh reader for every decode, so the P8 number also includes worker-pool startup and teardown; for a
25 ms P1 decode this fixed cost is significant. A long-lived reader would amortize it, but the
adaptive/lazy-start track targets exactly this cost. The P1/P8 ratios bound the achievable win for the
few-block extremes at about 3.1-4.0x.

## 5. Candidate optimization tracks

Tracks A, B, and C were implemented; their measured results are in "Results after integration". Track D
(BitReader peek/refill, an estimated 1.03-1.07x) was not pursued because the other tracks had larger wins.

### Track A: PCLMUL non-reflected bzip2 CRC

- **Files/functions owned**: `src/indexed_bzip2/bzip2.hpp`, CRC section (`CRC32_TABLE`,
  `createCRC32SliceTables`, `updateCRC32SliceByN`, `updateCRC32`). Optionally add a new
  `src/core/Crc32NonReflected.hpp`.
- **Proposed change**: add a runtime-dispatched PCLMUL (CLMUL instruction, with a scalar/SSE
  fallback) implementation of the MSB-first CRC32 (polynomial `0x04C11DB7`) for buffers above a few
  KiB, keeping the slicing-by-8 code for tails,
  non-x86 targets, and CPUs without PCLMUL. Preserve the raw CRC state (no final XOR) and the existing
  `updateCRC32` signature. A hand-written version avoids making ISA-L a hard dependency; the prior
  ISA-L probe is the performance reference.
- **Affected cases**: zeros, constant-a, rle1/rle2 boundaries, highratio, mixed-zero-spikes, and every
  high-ratio archive. Text/base64/random have `decodeBlock` shares of 2-10%, where the CRC is already
  hidden, so they should be neutral.
- **Estimated ceiling (measured components)**: current fill+CRC 2.51 GB/s; fill alone 18.3 GB/s;
  PCLMUL 25.9 GB/s (prior project measurement). Combined floor 10.7 GB/s, 4.3x the current CRC path.
  End-to-end: zeros about 4x (decodeBlock 88%); highratio about 1.7x (Amdahl with 57% decodeBlock);
  rle1/rle2 about 2.5-3.5x. These are ceilings, not achieved results.
- **Risks**: the CRC is a correctness and integrity check, so any bit-order or final-XOR mistake must
  fail tests loudly; feature dispatch and ARM (PMULL) coverage; PCLMUL is only a win for large
  contiguous buffers and must not regress the many 4 KiB calls of the sequential reader; ISA-L is not
  installed in this environment, so a hand-written kernel or a vendored small implementation is likely
  needed.
- **A/B protocol**: rebuild `benchmarkBzip2` in Release; 8 alternating base/change pairs, best of 10
  per invocation, P1 CPU 0 and P8 CPUs 0-7; files zeros, highratio, rle1-boundaries, rle2-boundaries,
  mixed-zero-spikes, plus english, base64, random-256 as regression guards; compare medians and ranges,
  not single runs; record `ibzip2 -v` `decodeBlock` time and `perf stat` instructions/cycles; run
  `testBZ2Reader`, `testParallelBZ2Reader`, and decode md5/size checks.

### Track B: Few-block and high-expansion parallel path

- **Files/functions owned**: `src/indexed_bzip2/BZ2BlockFetcher.hpp` (`decodeBlock`, buffer growth),
  `src/indexed_bzip2/ParallelBZ2Reader.hpp` (worker-count policy), `src/core/BlockFetcher.hpp` and
  `src/core/ThreadPool.hpp` (lazy worker start / small-pool behaviour). `src/indexed_bzip2/bzip2.hpp`
  is touched only to expose a decoded-size accessor.
- **Proposed change**: two independent sub-tasks.
  1. After `readBlockData()`/`prepare()`, scan the RLE1-encoded `bwtData` once to compute the exact
     decoded size (`sum 1 or 4 + count`), expose it on `Block`, and have `BZ2BlockFetcher` allocate the
     final `BlockDataBuffer` once. Replace the value-initializing `make_unique<uint8_t[]>` with an
     uninitialized allocation, since every byte is overwritten before it is exposed.
  2. For inputs with fewer data blocks than workers, cap the thread pool or run the first block on the
     caller thread. The block finder knows when it is finalized; a small compressed-size heuristic can
     be used before that.
- **Affected cases**: zeros, constant-a, rle1/rle2 boundaries, highratio at P8; any few-block file.
  Normal many-block files should see only the small benefit of removing the doubling copies.
- **Estimated ceiling**: the dedicated scaling run gives P1/P8 ratios of 3.96x for zeros, 3.10x for
  constant-a, 2.21x for rle1, 1.93x for rle2 (the baseline run gives 3.2-3.4x for the constants, so
  treat these as 2-4x). Removing the 24% memmove/memset share alone is bounded at about 1.3x; the
  thread-pool/finder part must be measured separately. The conservative target is to match the P1
  rate for few-block inputs (up to 4x on zeros P8); the optimistic ceiling is 2x P1 with two blocks
  in parallel, but that has not been demonstrated.
- **Risks**: the exact-size scan adds O(block) work; a single allocation can be up to about 46 MB per
  worker for pathological runs, which interacts with the buffer pool; changing worker startup or pool
  size can regress many-block P8 throughput; the RLE1 boundary scan must treat a run of exactly four
  identical bytes plus count correctly and must not read past `writeCount`.
- **A/B protocol**: P8 CPUs 0-7 on zeros, constant-a, rle1/rle2, highratio, small-blocks,
  many-streams, random-256; P1 regression check on the same; 3-5 iterations, 6 alternating pairs;
  record RSS and minor faults; `perf stat` memmove/memset and cycles; verify decoded bytes and sizes.
  Test both the exact-size and worker-policy sub-tasks separately so their contributions are known.

### Track C: Two-block Huffman/libsais software pipeline

- **Files/functions owned**: `src/indexed_bzip2/BZ2Reader.hpp` (`decodeStream`, block lifecycle) and
  `src/indexed_bzip2/bzip2.hpp` (`readBlockData`, `prepare`, per-block state). A new helper class for
  the pipeline is preferable to spreading state through `BZ2Reader`.
- **Proposed change**: split `readBlockData()` into a symbol pass (Huffman + MTF + RLE2) and the
  `prepare()` inversion call, then keep two `Block` objects. Read block N+1's header and symbol pass
  into the second block while block N's libsais inverse BWT, RLE1 expansion, CRC, and output run. The
  bit reader is sequential, so the read-ahead must advance past block N before decoding block N; the
  pipeline then overlaps the branch-heavy symbol loop with the memory-latency-bound inversion. This is
  the structural idea already nominated in `OPTIMIZATIONS.md`; it has not been implemented.
- **Affected cases**: english, base64, two-symbol, dna, sixteen-symbol (balanced phases), and
  small-blocks/many-streams (per-block pipeline). random-256 is 76% Huffman and 22% libsais, so its
  ceiling is lower. Zeros/highratio will not benefit.
- **Estimated ceiling (Amdahl from measured phases)**: english about 1.8x, base64 about 1.7x,
  two-symbol about 1.5x, random-256 about 1.3x (ideal overlap of the symbol pass and libsais, with
  `decodeBlock` serial). These are ideal overlaps; memory-bandwidth and L3 contention will reduce them.
- **Risks**: doubles the per-block working set (two 4.5 MB blocks per sequential reader), which is
  significant on the shared 16 MiB L3 and may negate the overlap; exception and reset state must not
  leak between blocks; the sequential CRC/output ordering must be preserved; the code becomes harder
  to reason about. This is the highest-risk, highest-structural-payoff track.
- **A/B protocol**: P1 CPU 0 on english, base64, two-symbol, dna, random-256, small-blocks; 8
  alternating pairs, best of 10; `ibzip2 -v` phase timers (the overlap should show the sum falling
  while individual phases stay similar); `perf stat` cycles, IPC, cache misses; full tests and
  decoded-size/CRC verification. Only after a P1 win should P2/P4 be measured.

### Track D: BitReader peek/refill for high symbol rates

- **Files/functions owned**: `src/filereader/BitReader.hpp` (the `peek`/`peek2`/refill path used by
  `bzip2::BitReader`).
- **Proposed change**: make the 12-bit bzip2 peek and the refill boundary cheaper, e.g. a 64-bit
  double-word buffer with refill only when fewer than the peek length remains, and a fully inlined
  bzip2-specific peek that does not branch through the generic path. The profile shows `peek2` at
  7.2-7.3% on random-256/base64, 3.2-3.5% on text/many-streams, and 2.1% on two-symbol.
- **Affected cases**: all high-symbol-rate cases, especially random-256, base64, dna/two-symbol.
- **Estimated ceiling**: about 1.03-1.07x on the affected cases if the whole `peek2` share were
  removed; a more realistic partial win is 1.01-1.04x. This is a small, broad track.
- **Risks**: `BitReader` is shared with gzip and other formats; EOF and block-boundary semantics must
  be preserved; prior work found that hoisting the reader state into locals was byte-identical, so
  target the refill and branch structure rather than local caching.
- **A/B protocol**: P1 CPU 0 on random-256, base64, english, two-symbol, dna; 8 alternating pairs,
  best of 10; `perf stat` instructions/cycles and `perf record` `peek2` share; run the bit-reader and
  gzip test suites.

## 6. Independence and conflicts

| track | primary files | conflicts with | can run in parallel with |
|---|---|---|---|
| A CRC | `bzip2.hpp` CRC region, optional new header | C (same file, different region), B only if B adds a `Block` accessor | D, B (if interface is agreed) |
| B parallel path | `BZ2BlockFetcher.hpp`, `ParallelBZ2Reader.hpp`, `core/BlockFetcher.hpp`, `core/ThreadPool.hpp`, small `bzip2.hpp` accessor | C (`bzip2.hpp`/`BZ2Reader` block lifecycle), A only at the accessor | A, D |
| C pipeline | `BZ2Reader.hpp`, `bzip2.hpp` block lifecycle | A and B on `bzip2.hpp`, B on the sequential/parallel split | D only, or run alone |
| D BitReader | `filereader/BitReader.hpp` | none of the above | A, B, C |

Recommended execution:

1. Run A and D in parallel now: they own disjoint files and have clear measured ceilings.
2. Run B in parallel with A/D after agreeing on a single `Block::decodedSize()` interface; keep B out
   of `BZ2Reader.hpp`.
3. Run C last, or in a separate branch that rebases onto A/B. C and B both need the block lifecycle;
   assigning both to one agent would also be reasonable, but C's pipeline is easier to review on top
   of B's size accessor.

The main file contention is `src/indexed_bzip2/bzip2.hpp`: A owns the CRC block near the top, C owns
`readBlockData`/`prepare` near the bottom, and B needs a small accessor there. They are different
hunks and merge cleanly in practice, but they should not be developed in the same branch without a
merge order.

## 7. Reproduction

```bash
# corpus (deterministic, removes raw intermediates)
python3 results/generateBzip2ExtremeCases.py \
    --output-dir tmp/bzip2-extremes/corpus --size-mib 64

# decode verification (CRC errors make bzip2 exit non-zero)
for f in tmp/bzip2-extremes/corpus/*.bin.bz2; do bzip2 -dc "$f" | wc -c; done

# baseline
taskset -c 0   tmp/build-bzip2-extremes/src/benchmarks/benchmarkBzip2 \
    --iterations 3 --parallel 1 --mode discard tmp/bzip2-extremes/corpus/random-256.bin.bz2
taskset -c 0-7 tmp/build-bzip2-extremes/src/benchmarks/benchmarkBzip2 \
    --iterations 3 --parallel 8 --mode discard tmp/bzip2-extremes/corpus/random-256.bin.bz2

# phase split
taskset -c 0 build/src/tools/ibzip2 \
    -v -P 1 -d -o /dev/null -f tmp/bzip2-extremes/corpus/highratio.bin.bz2

# counters and hotspots
perf stat -e cycles,instructions,cache-misses,branches,branch-misses \
    taskset -c 0 tmp/build-bzip2-extremes/src/benchmarks/benchmarkBzip2 \
    --iterations 1 --parallel 1 --mode discard tmp/bzip2-extremes/corpus/random-256.bin.bz2
perf record -o tmp/bzip2-extremes/perf-random.data -e cycles:u \
    taskset -c 0 tmp/build-bzip2-extremes/src/benchmarks/benchmarkBzip2 \
    --iterations 1 --parallel 1 --mode discard tmp/bzip2-extremes/corpus/random-256.bin.bz2
perf report --stdio -i perf-random.data --sort symbol,dso
```

## 8. Caveats

- The host was shared during the baseline and phase runs (load average about 1.6-2.2). Absolute MB/s in
  the baseline table are medians of a few runs; use paired A/B ratios for any optimization decision.
- The stage timings come from `ibzip2` at `9cfcda7e`; the baseline benchmark and
  perf profiles come from the `perf/bzip2-extremes` branch at `15c73ae5`. The integrated A/B uses the
  performance commits listed in "Results after integration". Re-run the stage timers after each change.
- The 25.9 GB/s PCLMUL reference was a prior measurement from `OPTIMIZATIONS.md`; the committed
  kernel was measured fresh at 25.2 GB/s (AVX2/VPCLMULQDQ) and 10.2 GB/s (SSE PCLMUL) on this host.
- The high-ratio raw size (1009.1 MB) makes it the most expensive case. The integrated A/B uses three
  iterations, but its medians are still less stable than the 67 MiB cases.
- `skewed-pair` and `sixteen-symbol` have wide per-pair ranges; their medians are reported, not a single
  best run.
- The corpus, generator, and report are committed. The generated corpus, perf data, probes, and A/B logs
  stay under `tmp/bzip2-extremes/` and are not committed.
