# AFL++ fuzz targets

These targets use AFL++ persistent mode and are built only when `LIBRAPIDARCHIVE_BUILD_FUZZERS=ON`.

The setup follows the usual AFL++ advice: build with ASan and UBSan, enable assertions, keep the
test input in a size-exact copy, and reset the reader for every iteration.

## Targets

- `fuzzRapidgzip` exercises `GzipReader` and `ParallelGzipReader` with small chunk sizes. The mode
  and chunk size are derived from the input length so that a single corpus covers the serial and
  parallel paths.
- `fuzzDeflate` calls `deflate::Block::readHeader` and `read` directly on the raw input, which reaches the
  Huffman and back-reference decode paths without the container overhead.
- `fuzzGzipIndex` feeds the input to `readGzipIndex`. It covers the gzip index parsers.
- `fuzzIndexedBzip2` exercises the sequential `indexed_bzip2::BZ2Reader`. The parallel reader is
  covered by TSan; using the sequential path here keeps AFL edge coverage deterministic.

## Running

Install AFL++ once, for example `sudo apt-get install afl++`. The CMake preset configures a fresh
`build-afl` directory with `afl-clang-fast`, ASan, and UBSan:

```bash
cmake --preset fuzz
cmake --build --preset fuzz
cmake --build build-afl --target run-fuzz-rapidgzip
```

Each fuzzer has its own target: `run-fuzz-rapidgzip`, `run-fuzz-deflate`, `run-fuzz-gzip-index`, and
`run-fuzz-indexed-bzip2`. To run all four, either start them individually or use:

```bash
cmake --build --preset run-fuzz -j 4
```

The corpus is copied from `src/tests/data` into `build-afl/fuzz-corpus/<target>` at configure time;
findings go to `build-afl/fuzz-findings/<target>`. The duration is the `AFL_FUZZ_SECONDS` cache
variable (default 60 seconds); set it to `0` to run until interrupted:

```bash
cmake --preset fuzz -DAFL_FUZZ_SECONDS=3600
```

The sanitizer configuration is built into the targets, following the
[AFL++ performance tips](https://nullprogram.com/blog/2025/02/05/).

 - `max_malloc_fill_size` makes ASan fill large allocations with a pattern, which exposes reads of
   uninitialized memory. `detect_stack_use_after_return` catches stale references to stack objects.
 - The harness copies the AFL buffer into a `std::vector` of exactly the reported length because the
   AFL buffer itself is larger than the test case, which would hide over-reads from ASan.

## Triage

A crash is written to `build-afl/fuzz-findings/<target>/default/crashes/`. Reproduce it with the
built harness and the sanitizer options above:

```bash
build-afl/src/tests/fuzz/fuzzRapidgzip < build-afl/fuzz-findings/rapidgzip/default/crashes/id:000000,*
```

For minimization, use `afl-tmin` with the same target, or reduce the input by hand. Treat
`std::bad_alloc` and `std::length_error` as findings: the harness aborts on them because untrusted
sizes must not drive allocations.
