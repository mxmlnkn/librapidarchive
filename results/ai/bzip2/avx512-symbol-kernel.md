# AVX-512 bzip2 symbol decoder

## Benchmark

Machine: AMD Ryzen 7 PRO 8840U (Zen 4), 8 physical / 16 logical cores, Linux, GCC 16.2, Release + LTO,
ISA-L off. Load average was 1.7-2.7 during the runs because other agents share the host. The benchmark pins
one process with `taskset`, alternates the two kernels per pair, and discards one warmup pair.

Input: `tmp/silesia/silesia.tar.bz2` (54.5 MB compressed, 212 MB uncompressed), output to `/dev/null`.

The A/B runs used a temporary in-tree kernel selection during development; the selection override and
benchmark harness are not part of this commit.

### 1 thread, two independent 24-pair sessions (48 pairs pooled)

| kernel | wall median [s] | wall min-max [s] | CPU median [s] |
|---|---:|---:|---:|
| AVX2 | 0.8766 | 0.7975-1.0698 | 0.8761 |
| AVX-512 | 0.8348 | 0.7464-1.0255 | 0.8344 |

Ratio of medians: **1.050x**. Paired per-pair ratio median: **1.0615x** (range 0.913-1.194);
AVX-512 was faster in **41 of 48** pairs. The two sessions separately measured 1.026x and 1.084x, so the
machine noise is larger than the effect, but both direction and magnitude are consistent.

### 4 threads (CPUs 2,4,6,8), 16 pairs

| kernel | wall median [s] | wall min-max [s] | CPU median [s] |
|---|---:|---:|---:|
| AVX2 | 0.4494 | 0.4210-0.5756 | 1.5261 |
| AVX-512 | 0.4291 | 0.4075-0.4741 | 1.5165 |

Wall ratio 1.047x, total CPU ratio 1.006x. The secondary check is compatible with a small win, but the
symbol phase is a smaller fraction of the parallel run.

### Hardware counters, 10 alternating pairs, whole process

`perf stat -e cycles,instructions,branches,branch-misses` on CPU 3:

| counter | AVX2 median | AVX-512 median | ratio |
|---|---:|---:|---:|
| cycles | 3,792,323,884 | 3,641,099,144 | 1.0415x |
| instructions | 9,993,775,572 | 8,794,177,681 | 1.1364x |
| branches | 955,378,744 | 856,812,893 | 1.1150x |
| branch misses | 21,488,727 | 19,988,984 | 1.0750x |

The counter result is the cleanest signal: 13.6% fewer instructions, 11.5% fewer branches, 7.5% fewer
branch misses, and 4.2% fewer end-to-end cycles. Isolating the `createHuffmanTable` timer from `-v` gave a
3.6% median improvement in the symbol phase, consistent with the whole-process numbers.

## Conclusion

The gain is real but modest: about 4-6% on the single-thread hot path, with a clear reduction in instruction
and branch counts. The front update is a serial dependency chain, and `vpermb zmm` latency on Zen 4 keeps the
cycle win smaller than the instruction win; the remaining benefit comes mostly from moving the in-register
front from 32 to 64 entries.

Follow-up ideas, in rough priority order:

- Measure the AVX-512 kernel on base64/random 0-255 alphabets, where the portable heuristic used to win for
  AVX2; the new 64-entry front should remove that reason, but this was not benchmarked here.
- The symbol loop is latency-bound on the serial Move-to-Front front update. Batching increasing MTF indices
  and gathering from the unchanged suffix remains the only promising way to break that chain.
- The Huffman lookup is still one scalar LUT load per symbol; decoding several dependent symbols in a wider
  vector would need a different representation, not just a wider front register.

## Additional follow-up measurements

- 64-symbol flat alphabet: AVX-512 is 1.61x faster than AVX2 and 1.41x faster than portable. Every literal
  is below list position 64, so the AVX-512 path has no deep-list branch. This confirms that skipping the
  portable near-tail heuristic for AVX-512 is correct.
- 256-symbol flat alphabet: AVX-512 and AVX2 are within noise; both are about 1.5x faster than portable.
  AVX-512 executes 44% fewer instructions but has 76% more branch misses because the deep-list branch is
  taken for 25% of literals instead of 12.5%.
- Last-to-first mapping builder: on Silesia the AVX-512 builder used 4.0% more cycles than AVX2
  (3.864e9 vs 3.717e9) because it writes 16 entries per run record, while Silesia runs average much
  shorter. The automatic choice remains AVX-512 until a broader long-run/high-expansion corpus is measured.
- MTF batching of strictly increasing positions: negative. Such runs cover about 0.02% of a flat 64-symbol
  input; the best-case batch-8 primitive is 10-15% faster, and batch-4 is 2.5x slower. Do not integrate.
