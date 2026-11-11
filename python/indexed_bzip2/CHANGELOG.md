# Version 2.0.0 built on 2026-11-??

This major release introduces some small Python wrapper API breaks,
comes with major 2-8x performance improvements, and is the first release using AI assistance.

## Features

 - Add parallel bzip2 compression to the `ibzip2` CLI.
 - Add `--count`, `--count-lines`, byte `--ranges`, and `--import-index`/`--export-index` to the `ibzip2` CLI.
 - Stream the bzip2 block offset index through `-L`/`--export-index` while decoding and compressing.
   `--list-offsets` remains an alias for compatibility. The compression-side scan works from the in-memory chunk,
   so it also supports writing compressed data to stdout. Empty streams get an EOS-plus-sentinel index,
   and chunks whose inner block offsets cannot be derived are rejected instead of writing an incomplete index.
 - Add `import_index` and `export_index` to `IndexedBzip2File` for the plain text bzip2 block offset index.

## Performance

 - Reuse the bzip2 block decoder state and `BlockData` buffers across blocks
   (1.01x single-core, 1.03x text64 at P4).
 - Decode run records and invert the BWT with interleaved chains instead of libsais
   (1.32x Silesia single-core, Huffman 0.74 s -> 0.64 s, and inverse BWT 0.64 s -> 0.41 s).
 - Add SSE4.1 and AVX2 symbol decoding kernels, selected per block from the Huffman code lengths
   (1.23x Silesia and 1.26x text64 single-core).
 - Dispatch the last-to-first mapping builder to AVX2 or AVX-512 at runtime so prebuilt wheels use the
   wider stores on capable x86 CPUs.
 - Pack the Huffman LUT entry and MTF list (1.07x text decode).
 - Make the MTF update branchless (1.28x base64 decode).
 - Compute the bzip2 CRC with slicing-by-N tables, and with PCLMUL/VPCLMULQDQ where available
   (4.2x on constant data, 2.9-3.0x on RLE boundary data, 1.9x on high-ratio files).
 - Allocate parallel bzip2 blocks at their exact decoded size and prefetch the last block
   (3.1x on constant data and 4.0x on RLE boundary data with eight physical cores).
 - Decode the next Huffman symbol ahead of the MTF update (1.05-1.10x on text, DNA, and random data).
 - Use NEON for the bzip2 MTF and RLE expansion, symbol kernel, and last-to-first mapping builder on ARM.
 - Decode the sequential reader in 64 KiB chunks, emit RLE1 runs incrementally, store short runs with
   one word write, and add a 64-byte AVX-512 RLE scan.
 - Keep the block decoder state off the worker stack, avoid zero-initializing and reuse the per-block
   BWT buffers, and parse the next block header in the worker.
 - Avoid `RawIOBase.readall`'s 8 KiB chunks and reallocations in the Python wrapper.
 - Fix a performance regression when repeatedly reading less than the chunk size per call.
 - Use the physical core count for automatic `ibzip2` parallelization because it scales not well beyond.

## Fixes

 - Return `None` from `IndexedBzip2File.size()` when the uncompressed size is
   not known yet instead of returning 0.
 - Make `ibzip2 --test` work without also passing `--decompress`.
 - Reject bzip2 run lengths that overflow the block size and data blocks with an empty symbol map.
 - Distinguish empty from truncated bzip2 streams and verify the bzip2 stream CRC in the parallel reader.
   Accept EOF immediately after a complete EOS on non-seekable inputs instead of reporting the next stream as truncated.
 - Report a zero compressed offset from `ParallelBZ2Reader::tellCompressed`
   before the first read.
 - Decode concatenated bzip2 streams and verify the CRCs of imported indexes
   while decoding.
 - Reject imported bzip2 block indexes whose last block offsets are incomplete, whose decoded block
   sizes disagree with the data, or that cover only the first stream of a concatenated file.
 - Share the file-reader and core fixes from the rapidgzip core, including `BufferedFileReader` seek and
   cache behavior, non-seekable stream sizes, null-buffer skips, out-of-range `BufferViewFileReader` seeks,
   and resetting the eviction strategy in `clear()`.
 - Fix reads from Python file-like objects returning more data than requested
   and require writable C-contiguous buffers for Python `readinto`.
 - Support non-seekable Python file objects without `fileno`, keep caller-owned file descriptors open while detecting
   the input type, release callback references, propagate Python exceptions from file-like objects, make reader
   construction and close exception-safe, and release the GIL while joining the block finder.
 - Accept path-like and bytes objects for the file name in the standalone and parallel readers.
 - Fix writing to `/dev/null` when the file size cannot be determined.
 - Treat `-` as stdin or stdout in the `ibzip2` CLI, and avoid decoding the
   whole input just to print verbose range information.
 - Retry interrupted writes and avoid a lost reader-thread wakeup for non-seekable inputs. Check the compressor
   chunk length before scanning for a block magic and restrict the packed-MTF NEON path to little-endian targets.
 - Validate parallel bzip2 block offsets before updating the block finder, reject invalid Huffman codes
   and block selectors with descriptive errors, and do not require the decoded block to end before the next candidate.
 - Fix block-zero prefetching, keep rethrown worker exceptions alive in the shared block fetcher, and
   keep the parallel readers usable after joining threads.

## API

 - Add type annotations to the Python wrappers using the `FileOrPath`, `IndexFile`, and `WritableBuffer` aliases.
 - Remove the `fileno` method from the Python readers.


# Version 1.7.0 built on 2025-07-21

 - This update now uses the same code base as the planned rapidgzip 0.15.0 and shares many of the fixes.
 - Fix a bug on native Windows with files > 4 GiB because `fstat` fails to return the file size for those.
 - Fix a bug on native Windows with files > 2 GiB because the `std::fseek` offset argument is signed 32-bit.
 - Increases minimum macOS requirement to 10.15.

# Version 1.6.0 built on 2024-02-22

 - Version 1.5.0 was based on the same code base as rapidgzip 0.6.0.
   This update now uses the same code base as the planned rapidgzip 0.13.0 and shares many of the fixes.
 - Fix a segmentation fault that only happens with Python 3.12 when recursively mounting nested bzip2
   files with ratarmount.
 - Suppress `Python memory allocator called without holding the GIL` errors with `PYTHONDEVMODE=1`.
 - Reuse the cached Huffman decoder from rapidgzip for ~25% speedup with a single core and ~10% speedup
   with full parallelization over all virtual cores. The parallel version profits less because SMT
   already did a good job hiding the slowness of the decoder.

# Version 1.5.0 built on 2023-05-29

 - Fix wrong version number returned by `ibzip2 --version`.
 - Use the number of cores the process has affinity for as default parallelism.
 - Write verbose output enabled with the `-v` option to stderr instead of stdout.
 - Allow decompressing to `/dev/null` with ibzip2 for benchmarking purposes.
 - Add `--oss-attributions` to `ibzip2`.
 - Quit when SIGINT has been sent when called from Python.
 - Spawn threads only when needed instead of all at once at startup.
 - Avoid `std::regex` initialization during dlopen.

# Version 1.4.0 built on 2022-11-12

 - Add command line CLI entrypoint ibzip2 to be used as a standalone parallel bzip2 decoder as replacement for bzip2.
 - Add a `BZ2Reader::read` overload that takes a callback functor.
 - Add `io.SEEK_SET` as default value for the `seek` method's `whence` argument.
 - Fix segmentation fault when IndexedBzip2File failed to construct, e.g., because of a wrong argument type given.
 - Speed up magic bit string finder 9-fold.
 - Refactored a lot of code in order to build pragzip on top of the indexed_bzip2 backend.
 - Migrate to pyproject.toml installation method.

# Version 1.3.1 built on 2021-12-23

 - Fix `std::logic_error` exception for certain files when using the parallel decoder.

# Version 1.3.0 built on 2021-09-15

 - Add support and wheels for macOS and Windows additionally to Linux.
 - Build and upload Conda packages for Linux, Windows, and macOS.
 - Add `open` method for compatibility to `bzip2`, `gzip`, and other similar modules.
 - Add support for pure Python file-like objects without a fileno like BytesIO.

# Version 1.2.0 built on 2021-06-27

 - Fix corrupted data for concatenated bzip2 streams in one bzip2 file in serial BZ2Reader.
 - Add C++17 threaded parallel bzip2 block decoder which is used when constructing
   `IndexedBzip2File` with argument `parallelization` != 1.
 - Provide `readline`, `readlines`, `peek` methods by inheriting from `BufferedReader`.
 - Add `available_block_offsets`, `block_offsets_complete`, and `size` method.
 - Do not build index when seeking to current position.

# Version 1.1.2 built on 2020-07-04

 - Fix `tell` method.

# Version 1.1.1 built on 2020-04-12

 - Refactor code.
 - Fix endless loop caused by failing stream CRC check after using `set_block_offsets` and seeking to the end.

# Version 1.1.0 built on 2019-12-08

 - Fix broken `set_block_offsets, `block_offsets`.
 - Add forgotten `tell_compressed`.

# Version 1.0.0 built on 2019-12-08

 - First hopefully stable version.
