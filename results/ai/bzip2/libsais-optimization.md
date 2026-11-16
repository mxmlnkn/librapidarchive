# libsais inverse BWT optimization for indexed_bzip2

Date: 2026-11-09. Branch: `perf/unbwt-2x` in the libsais fork, tip `6c73fe5`.
Base: upstream `c284ba8`. Nothing was pushed.

## Summary

Two libsais commits speed up `libsais_unbwt_cyclic` by about 2-5% on real bzip2
blocks (Silesia, text, base64) and by 5-7% on text-like data. The end-to-end P1
bzip2 decode changes by roughly 1-2%, which is at or below the measurement noise
of this shared host. 2x was not reached; with libsais at roughly 45% of P1
runtime, 2x on libsais would only be 1.25x end-to-end anyway.

The largest remaining libsais win is an integration change, not a libsais
kernel change: the bzip2 block should reuse a `libsais_unbwt_create_ctx()`
context instead of calling the allocating `libsais_unbwt_cyclic` per block.
On 64 real bzip2 blocks that is consistently **7-8%** of libsais time on all
three corpora. The patch is included below; it was tested in the experiment
branch and later integrated as `f1190e0a` in `master`.

## Measurement method

The host is shared (16 logical CPUs, other agents active, SMT sibling 15 of
CPU 14) and wall-clock A/B at the few-percent level is not reproducible, even
in-process and CPU-pinned. The reliable metric used for the libsais results is
a paired benchmark on real workload data:

1. The in-tree decoder dumps the first 64 BWT blocks (n, primary index, 256
   frequencies, BWT bytes) for a corpus. 64 blocks x ~900k symbols, ~55 MiB.
2. A standalone driver links the base and the changed libsais into one binary
   with renamed symbols, and calls both per block in alternating order, on the
   same buffers, verifying byte equality. It reports per-variant total and
   median time. The A/A control (same source twice) is within 0-1.5%.

The block sizes are representative: Silesia is 456 blocks with median
n = 899981 and 900k-ish blocks for text64 and base64 too. Per block, libsais
takes about 4 ms, of which init is ~2.0-2.7 ms (scatter ~1.5-2.2 ms,
bigram histogram ~0.3-0.5 ms) and the chain decode ~1.2-1.5 ms.

In-tree verification used `benchmarkBzip2 --iterations N --parallel 1 --mode
discard` with `perf stat -e cycles`, and a temporary `clock_gettime` around the
libsais call to isolate libsais time (values printed per block and summed).

## Commits

Branch `perf/unbwt-2x` (clean on top of `c284ba8`):

- `81c73e6` Improved inverse BWT scatter by overlapping the bi-gram table stores.
  The fast 16-element scatter group now computes all destinations first, issues
  `prefetchw` for each `P` destination, and delays the stores by half a group.
  Destinations and values are unchanged, so `P` is bit-identical.
- `6c73fe5` Improved inverse BWT performance by reducing the decode chunk size.
  `UNBWT_CHUNK_SIZE` 16320 -> 8160. Halves the per-chain slack (391 KiB ->
  196 KiB) and the decode working set; 24 concurrent output streams remain.

The experiment history (chunk commit, revert, pipeline commit) was folded into
these two commits. The pre-fold history is preserved in
`backup/perf-unbwt-2x-preclean` = `b0984bc`.

## Results

### libsais-only, 64 real blocks, alternating paired calls

`total` is the sum over blocks; `med` is the per-block median. Both are ratios
base/change, so > 1 means the change is faster. A/A control is the same base
source linked twice.

| corpus  | control total | pipeline total | chunk total | pipeline+chunk total | final med |
|---|---:|---:|---:|---:|---:|
| Silesia | 1.012 | 1.041 | 1.032 | 1.047 | 1.024 |
| text64  | 0.998 | 1.072 | 1.033 | 1.068 | 1.083 |
| base64  | 0.995 | 1.004 | 1.025 | 1.029 | 1.021 |

Control-corrected, the combination is about +3% Silesia, +5-7% text, +2%
base64. The two changes overlap (both reduce memory stalls), so the sum is not
additive.

### In-tree libsais share (perf, Silesia, P1)

| build | init | chain decode | histogram | libsais total |
|---|---:|---:|---:|---:|
| base | 28.10% | 15.11% | 3.18% | 46.4% |
| pipeline | 27.02% | 13.75% | 3.19% | 44.0% |

The isolated libsais timer over whole Silesia runs gives 1.61-1.65 s for base
vs 1.53-1.59 s for the change in the cleanest runs (~+4%), but individual
alternating runs scatter by +-5% on this host.

### End-to-end P1 (cycles, 6 alternating pairs, 3 iterations)

| corpus | base cycles | final cycles | speedup |
|---|---:|---:|---:|
| Silesia | 24.98 G | 24.66 G | 1.013 |
| text64 | 8.16 G | 8.51 G | 0.959 |
| base64 | 5.56 G | 5.60 G | 0.992 |

The expected end-to-end effect of a 3-5% libsais change at a 45% share is 1.5-2%,
which is below this metric's noise; the text number is a noise/placement
artifact (the libsais-only measurement for text is positive).

## Recommended integration: reuse the libsais context

`libsais_unbwt_cyclic` allocates its ~1 MB working set (`bucket2`, `fastbits`,
`marks`, `chains`, slack, links) and frees it for every block. The freed blocks
are mmap'd/trimmed by glibc, so every block re-faults its pages: about 130
minor faults and ~0.2-0.3 ms per call. `libsais_unbwt_cyclic_ctx` with a context
created once per block worker removes that cost.

Paired measurement on the same 64 real blocks (plain API called first half the
time, same output verified):

| corpus | plain | ctx | speedup (total) | speedup (median) |
|---|---:|---:|---:|---:|
| Silesia | 1.515 s | 1.415 s | 1.071 | 1.083 |
| text64 | 1.603 s | 1.479 s | 1.084 | 1.090 |
| base64 | 1.712 s | 1.595 s | 1.073 | 1.070 |

That is a consistent **7-8% of libsais**, i.e. about 3.5% end-to-end, with no
libsais source change. Test patch for `src/indexed_bzip2/bzip2.hpp` (do not
commit the test patch; apply it in the real repository):

```c++
/* in BurrowsWheelerTransformData, next to inverseBWT */
void * unbwtContext{ nullptr };

/* in prepare(), replacing the libsais_unbwt_cyclic call */
if ( unbwtContext == nullptr ) {
    unbwtContext = libsais_unbwt_create_ctx();
}
const auto error = unbwtContext != nullptr
    ? libsais_unbwt_cyclic_ctx(
          unbwtContext, bwtData.data.get(), bwtData.data.get(),
          reinterpret_cast<int32_t*>( dbuf.data.get() ), static_cast<int32_t>( writeCount ),
          frequencies.data(), static_cast<int32_t>( origPtr ) )
    : libsais_unbwt_cyclic(
          bwtData.data.get(), bwtData.data.get(),
          reinterpret_cast<int32_t*>( dbuf.data.get() ), static_cast<int32_t>( writeCount ),
          frequencies.data(), static_cast<int32_t>( origPtr ) );

/* free it in the BurrowsWheelerTransformData destructor */
libsais_unbwt_free_ctx( unbwtContext );
```

The context must be owned by the block worker (one per `Block`), not shared
between worker threads. It is safe with `T == U`, as used by the bzip2 path.
`testBZ2Reader` (226/226) and `testParallelBZ2Reader` (272/272) pass with the
patch, and per-block output was verified byte-identical in the paired driver.

Note: the default API path also need not get worse. The committed chunk-size
change halves the slack allocation and is measurable without the context.

## Dead ends

- **Grouped scatter by current symbol (3-pass staging, or linked lists).**
  The P layout is ordered by the *preceding* symbol (bucket high byte), not the
  current one, so grouping by current symbol leaves the stores scattered. A
  position-staging pass plus a per-symbol redistribution measured 2-3x slower
  than the one-pass scatter; the extra passes cost more than the misses they
  remove.
- **Transposing the P layout to make it current-symbol-major.** Breaks the
  row-number identities used by `first_symbols`/`marks`; the chain decode does
  not terminate.
- **32-element pipelined groups** (two 16-groups batched): 9-10% slower than
  the 16-element, lead-8 group on all corpora (register pressure and longer
  flush).
- **Other prefetch leads** (4, 6, 12, 16): lead 8 is the optimum; lead 16 is a
  full batch and is 2-4% slower.
- **More chains** (`UNBWT_CHAINS` 32): neutral to 4% slower.
- **Smaller chunks** (2040, 4080) on top of 8160: neutral or up to 1% slower.
- **Avoiding the per-call allocation by allocating less** (smaller `fastbits`,
  one combined allocation): untouched pages do not fault, so this only removes
  mmap syscalls; the context is the effective fix.
- A pure sequential-store ablation shows the scatter could be ~1.5 ms faster if
  the writes were perfectly local, but every restructuring that achieves that
  needs an extra n-element scratch and at least one extra pass, which costs
  more than it saves on this CPU.

## Honest conclusion

2x was not reached. The committed changes are a clean ~2-5% on libsais
(up to ~7% on text), and a further ~7-8% of libsais is available from switching
the bzip2 block to the existing context API. End-to-end P1 gains are about
1-2% from the libsais commits and would be about 4-5% with the context change;
the remaining P1 time is Huffman/MTF/output, not libsais.

## librapidarchive integration validation

The recommended context-reuse patch was applied on the release branch and is now in `master` as
`f1190e0a [performance] bzip2: Reuse the libsais inverse-BWT context per block worker` (the release
branch carried the equivalent commit `6dc46b5a`). It uses the pinned libsais `c284ba8`, so it does not
depend on the fork branch.

Pinned single-core A/B, 5 alternating pairs, 5 iterations, `taskset -c 14`,
`benchmarkBzip2 --parallel 1 --mode discard`, base vs context:

| Corpus | Minor faults | Instructions | Wall |
|---|---:|---:|---|
| Silesia | 41717 -> 18678 (-55.2%) | -0.14% | load-dominated |
| text64 | 15007 -> 8342 (-44.4%) | -0.15% | paired MB/s median 1.029 |
| base64 | 12413 -> 11459 (-7.7%) | -0.03% | paired MB/s median 1.076 |

The minor-fault reduction is the robust effect; Silesia wall had two slow change runs and is not
conclusive. The libsais kernel commits are now in the fork's `ibzip2` branch (`48cac4d`, based on
`c284ba8`), which is the pinned submodule in `master`; the context win is independent of them.

Raw log was kept in the untracked investigation tree.
