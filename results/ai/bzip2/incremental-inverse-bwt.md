# Incremental cyclic inverse BWT and the two-block pipeline

Investigation branch `perf/bzip2-incremental`, based on `5704e3bc`.
Host load was 2.0-4.5 during the measurements (shared with the editor and another agent).

## Result in one paragraph

The incremental libsais API is implemented and behaves correctly. `libsais_unbwt_cyclic_yield_ctx`
runs the same decoder as the whole-block call and invokes a callback at bounded intervals; its
overhead with a no-op callback is within measurement noise (about -1% to +1% paired, well below the
2% stop gate). The two-block pipeline on top of it does **not** clear the 1.2x gate. At P1 the best
configuration is 0.968x (base64) and 0.972x (text64); the intended finer interleave is worse, 0.868x
(base64) and 0.940x (text64). The reason is structural: a callback alternates two multi-thousand
instruction streams on one hardware thread, and in-order retirement only overlaps the boundary
window of the out-of-order buffer. The time is the sum of both phases plus cache and state-switch
overhead, not their maximum. The 1.75-1.88x ceiling in the overnight report is a simultaneous-execution
bound, and the only realization observed so far is SMT (1.12-1.23x, two logical CPUs). No further
polish of this path is justified.

## libsais API (committed)

Commit `1c14185` on `perf/unbwt-2x`, on top of `712ee99`:

```c
typedef void (*libsais_unbwt_yield_callback)(void * user_data);

int32_t libsais_unbwt_cyclic_yield_ctx(
    const void * ctx, const uint8_t * T, uint8_t * U, int32_t * A,
    int32_t n, const int32_t * freq, int32_t i,
    libsais_unbwt_yield_callback callback, void * user_data, int32_t interval);
```

* Same output as `libsais_unbwt_cyclic_ctx`; `interval <= 0` is an error.
* Yields at 16-symbol boundaries in the biPSI scatter and once per decode chunk in the symbol-rows
  chain decoder. The packed decoder used for `n <= 65535` does not yield.
* All decoder state stays on the stack; no separate state object is needed. The callback must not
  touch `T`, `U`, `A`, or the context and must not throw or `longjmp`.
* The existing whole-block entry points pass a `NULL` yield pointer and take the same code paths as
  before. Their output is unchanged.
* The two hot functions are duplicated into `_yield` variants so the default paths keep their
  original loop bodies and code generation. This is about 400 added lines in `libsais.c`.

Correctness of the API was checked without the bzip2 reader: a 900000-symbol text-like and
base64-like string was BWT-transformed with `libsais_bwt_cyclic` and inverted with both the
whole-block and the yielding call for intervals 1, 16, 100, 1024, 8192, and 65536. All outputs matched
the original input byte for byte.

## Stepped-API overhead

`tmp/libsais_yield_test.cpp` (not committed): 900000 symbols, paired whole-block vs yielding call with
a no-op callback, alternating order, 15 rounds x 3 repeats, `taskset -c 14`.

| input | interval | whole | yield | ratio median | range |
|---|---:|---:|---:|---:|---|
| text-like | 4096 | 3.741 ms | 3.710 ms | 0.9956 | 0.848..1.230 |
| base64-like | 4096 | 3.100 ms | 3.133 ms | 1.0112 | 0.862..1.101 |

Earlier runs at intervals 512-8192 were between 0.92 and 1.01. The paired spread is host noise, not a
systematic cost. The stop gate ("step overhead > 2%") is not triggered.

## Two-block pipeline (implemented, not committed)

`src/indexed_bzip2/BZ2Reader.hpp` holds the experiment. It was left uncommitted
because it is the losing path; the reusable `Block` split is committed separately.

* Two `Block` slots. While slot N is inverted by libsais, the callback decodes slot N+1's Huffman/MTF
  data in `RAPIDGZIP_BZIP2_PIPELINE_STEP`-symbol groups.
* The next block's header is parsed into the free slot during `prepare()`, but its global effects
  (block-offset map insertion, stream CRC check, EOF flags) are deferred until it becomes current.
  Failures during the prefetch are stored and rethrown when the next block is needed, so the current
  block is still output first.
* The callback catches exceptions and stores them, because throwing through the C decoder frame is
  not safe.
* `RAPIDGZIP_BZIP2_PIPELINE=1` enables the path; `0` or unset keeps the existing one-block path.
  `RAPIDGZIP_BZIP2_PIPELINE_STEP` and `_INTERVAL` override the tuning constants (4096 symbols and 8192
  work units by default).
* Output order, block CRC, stream CRC, EOS handling, concatenated streams and seek were checked by
  `testBZ2Reader` with the switch on (232/232). `testParallelBZ2Reader` is unaffected (278/278,
  because it uses `BZ2BlockFetcher` and `readBlockData()`).

## P1 measurements

`benchmarkBzip2 --iterations 8 --parallel 1 --mode discard`, `taskset -c 14`, alternating base
(pipeline off) and change, 6 rounds. `ratio = pipeline / base`. Load 2.1-2.5 for most rounds, up to
4.5 later.

### text64 (73,702,464 B decoded)

| config | base median (range) | pipeline median (range) | ratio median (range, wins) |
|---|---|---:|---:|---|
| step 4096 / interval 8192 | 142.97 (136.12..149.83) MB/s | 136.37 (116.02..141.46) MB/s | **0.9400** (0.8250..1.0282, 1/6) |
| step 900000 / interval 8192 | 142.58 (128.13..144.71) MB/s | 136.55 (124.61..142.01) MB/s | **0.9720** (0.8780..0.9978, 0/6) |

### base64 (33,554,432 B decoded)

| config | base median (range) | pipeline median (range) | ratio median (range, wins) |
|---|---|---:|---:|---|
| step 4096 / interval 8192 | 91.31 (76.19..97.10) MB/s | 79.75 (74.72..93.95) MB/s | **0.8677** (0.7695..1.2330, 2/6) |
| step 900000 / interval 8192 | 96.30 (94.58..104.36) MB/s | 93.21 (85.11..107.53) MB/s | **0.9684** (0.8573..1.1115, 3/6) |

The coarse configuration (`step 900000`) decodes the whole next block in the first callback, which is
close to the old serial order and therefore only pays the prefetch/state overhead. The intended
fine interleave is worse, especially on base64: 0.87 vs 0.94 on text64. A granularity sweep on
text64 (two rounds per point) gave 0.82-0.93 for 32/64, 0.90-1.02 for 128/256, 0.90 for 512/1024,
and 0.92 for 2048/4096. A no-op callback (prefetch only, Huffman finished serially) ranged 0.87-1.15
with a median near 0.96, i.e. even the pipeline mechanics alone do not pay for themselves.

### P8 and P16 (text64)

`--parallel 8` and `--parallel 16` on CPUs `0-15`, 5 iterations per invocation, 3 rounds. These use
`ParallelBZ2Reader`, which does not take the pipeline path:

| threads | ratios (pipeline on/off) |
|---:|---|
| 8 | 0.991, 1.113, 0.969 |
| 16 | 0.998, 0.979, 1.007 |

Within noise. No parallel regression.

## Why the one-thread pipeline cannot reach the gate

The two phases are separate serial streams. A callback switches between them at chunk boundaries of
thousands of instructions. The CPU retires in order, so it can only overlap the tail of the outgoing
chunk with the head of the incoming chunk inside one reorder buffer (a few hundred instructions).
With about 110-220 boundaries per 900k block and about 20 billion instructions per file, the overlap
is negligible: total time is the sum of the phases, not the maximum. Measured ratios confirm this:
the best is 0.97, and every finer granularity is worse because of state save/restore, instruction
cache churn, and two block working sets (two 900 KiB BWT buffers plus the 3.6 MiB P table) competing
for cache.

The overnight report's `1 / max(libsais, huffman + rle)` ceiling assumes simultaneous execution. The
SMT P2 result (1.12x base64, 1.23x text64) is a second hardware thread, not instruction-level
parallelism available to one thread. A genuine one-thread version would require fusing the Huffman
decoder into the libsais chain-decode loop at instruction granularity (manual software pipelining of
two dependent chains), which is a rewrite of both hot loops, not an incremental API. That is out of
scope for this task and was not attempted.

## Tests and byte comparisons

Final committed tree (`bc015bdd`, pipeline path not committed; the numbers below are from that tree):

* `testBZ2Reader`: 232/232, exit 0.
* `testParallelBZ2Reader`: 278/278, exit 0.
* `testHuffmanCoding`: 3712/3712, exit 0.
* `ibzip2 -d -c -P 1` md5 equals `bzip2 -dc`:
  * base64 `96da987fb724edb67395454f196e539a`
  * text64 `721d158c3a1fe1de074a80c0f098e9af`
  * Silesia `1bd862b2e81e1122da403a7019ea7634`
* The same tree with the uncommitted pipeline enabled also passed `testBZ2Reader` 232/232 and
  `testParallelBZ2Reader` 278/278, and the three md5s matched for pipeline on and off. Silesia at P8
  with the pipeline enabled also matched.

## Commits and submodule caveat

* libsais: `1c14185` "Add a yielding cyclic inverse BWT" on `perf/unbwt-2x`, on top of `712ee99`.
* librapidarchive: `bc015bdd` `[refactor] Make the bzip2 Huffman decode resumable`.
* The report commit follows on `perf/bzip2-incremental`.

In the investigation tree `src/external/libsais` is a symbolic link to a local libsais fork checkout, so the
build used `perf/unbwt-2x@1c14185`. The branch is not in the pinned submodule. `master` now pins
`48cac4d` on the fork's `ibzip2` branch, which carries the scatter/chunk commits and the `#ifndef`
guard but not the yielding API. The reusable `Block` split (`bc015bdd`) is also not in `master`. Do not
bump the gitlink to an unpushed commit; the yielding API would need a separate fork branch and a new
measurement before it is worth shipping. The `git status` warning about `src/external/bzip2` being a
symbolic link was already present in the investigation setup.

## Next action

Stop the callback-based one-thread pipeline. The incremental libsais API and the resumable
`Block::readBlockDataStep()` are available and correct on the experiment branch, but the only way to use
them for a one-thread win is a fused inner loop (a rewrite), not scheduling. The scatter/chunk/guard
part of `perf/unbwt-2x` is already in the pinned fork and `master`; the yielding API and the pipeline
were not merged. The SMT two-thread ceiling remains the only realized overlap.
