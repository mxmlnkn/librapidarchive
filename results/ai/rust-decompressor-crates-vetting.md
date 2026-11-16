# Rust decompressor crates: supply-chain, activity, seekability, and benchmark notes

Date: 2026-11-15. Scope: 20 crates selected for a benchmark comparison plus a short list of
additional crates.io candidates. Method: direct crates.io `.crate` tarballs and git clones, no `cargo`
commands, no builds, no crate execution. Every non-test source file in each published tarball
was read; build scripts, manifests, licenses, and repository history were checked separately.
The seekability section also reviews the public `Read + Seek`/index APIs against ratarmount's
file-object interface. See "Limitations" at the end.

## Executive summary

- **No build-time download, environment exfiltration, or prebuilt binary was found** in the
  published tarballs. There is also no `include_bytes!` payload, encoded blob, proc-macro crate,
  or git/path dependency in a normalized manifest. OSV returned zero advisories for all queried
  crate names on this date.
- **The observed issues are mainly release provenance, licensing, and decoder-integrity checks.**
  The most consequential observations are:
  1. `gzippy` 1.0.0 was packaged from a dirty tree that is not its `v1.0.0` tag, its `build.rs`
     can write into the enclosing repository's git hooks path, and the parallel BGZF/multi-member
     paths do not verify CRC32.
  2. `lgz` and `lbzip2` published source cannot be tied to any public tagged commit; the current
     forge is SSH-only, and the author documents same-version source drift.
  3. `parallel_bzip2_decoder` accepts truncated/corrupt streams and does not check the stream
     CRC. This matters for untrusted input.
- **Name collisions matter when selecting a crate.** The crates.io `rapidgzip`,
  `rapidgzip-sys`, and `rapidgzip-rs-cli` are `alekseizarubin`'s FFI wrapper around a vendored
  librapidarchive 0.16.0, not COMBINE-lab's pure-Rust project. COMBINE-lab publishes
  `rapidgzip-core` and `rapidgzip-rust-cli`. The crates.io `lbzip2` is a Rust decoder, not the
  well-known C `lbzip2`.
- **Seekability is the main differentiator for ratarmount.**
  `rapidgzip-core` (gzip) and `fbz` (bzip2) are the strongest genuine `Read + Seek`
  random-access readers with a persistent block index and bounded block cache.
  `rapidgzip`/`rapidgzip-sys` also implement `Read + Seek`, but through the older C++ 0.16.0
  backend and without `SeekFrom::End`. `mibz2` has block-level random access but no reader or
  persistent index. Everything else is streaming-only or only has block-scanning primitives.
  See Section 4.
- **Benchmark candidates:** `rapidgzip-core`/`rapidgzip-rust-cli` (independent Rust peer, 10
  reverse dependencies), `gzippy` (note the packaging and integrity caveats), `pbz2` (bzip2 CLI),
  `crabz2` (permissive pure-Rust bzip2 library), and `lbzip2` (parallel bzip2 library/CLI, with a
  provenance caveat).
- **Publication dates.** 13 of the 20 crates were first published in 2026; only `deacon` predates
  2026. Download counts are low except for the two rapidgzip families and `oxiarc-bzip2`; counts
  can include CI and mirror traffic.

## 1. Identity map and name collisions

| Name | What it actually is | Maintainer | Important note |
|---|---|---|---|
| `rapidgzip` 1.2.1 | Safe Rust wrapper over `rapidgzip-sys` (C++ librapidarchive 0.16.0) | `alekseizarubin` | Not COMBINE-lab, despite the name |
| `rapidgzip-sys` 1.2.1 | Raw FFI + vendored librapidarchive + CMake build | `alekseizarubin` | 18.5 MB source package, 536 C/C++ files |
| `rapidgzip-rs-cli` 1.2.1 | CLI for the above, gzip/BGZF | `alekseizarubin` | Binary name `rapidgzip-rs-cli` |
| `rapidgzip-core` 0.3.1 | Independent pure-Rust marker/window decoder | `rob-p` / COMBINE-lab | The independent COMBINE-lab project |
| `rapidgzip-rust-cli` 0.3.1 | CLI for `rapidgzip-core` | `rob-p` / COMBINE-lab | Binary name `rapidgzip-rust` |
| `rusty-rapidgzip` 0.2.2 | Independent streaming/speculative gzip decoder | `TyberiusPrime` | Manifest repo URL is 404; actual repo is `rusty_rapidgzip` (underscore) |
| `lbzip2` 0.5.13 | Pure-Rust parallel bzip2 decoder | `Ignalina` | Not the C `lbzip2`; repository URL is a moved/SSH-only forge |
| `lzip-parallel` (candidate) | Parallel ZIP decoder | `Ignalina` | Ships a binary named `lzip`, another collision |

## 2. Capability matrix: gzip, zlib, DEFLATE, BGZF

Legend: "P" = parallel decompression, "C" = compression, "Idx" = index/random access.
"Stream" = streaming API/CLI. "CLI" is the installed binary, not an example.

| Crate (version) | Kind / CLI | Formats | P | C | Idx / seek | Stream | License |
|---|---|---|---|---|---|---|---|
| `rapidgzip` 1.2.1 | library; no binary | gzip, BGZF | yes | no | indexed_gzip GZIDX import/export; `Read + Seek` (no `SeekFrom::End`) | yes | MIT OR Apache-2.0 |
| `rapidgzip-sys` 1.2.1 | FFI library; no binary | gzip, BGZF | yes | no | same as wrapper | yes | MIT OR Apache-2.0 |
| `rapidgzip-rs-cli` 1.2.1 | binary `rapidgzip-rs-cli` | gzip, BGZF | yes | no | index import/export for full-stream decode; no range/seek CLI | yes | MIT OR Apache-2.0 |
| `rapidgzip-core` 0.3.1 | library; no binary | gzip, BGZF, zlib, raw DEFLATE | yes | no | RGZIDX01, GZIDX, htslib `.gzi`, gztool; byte/line seek | yes | BSD-3-Clause AND MIT |
| `rapidgzip-rust-cli` 0.3.1 | binary `rapidgzip-rust` | gzip, BGZF, zlib, raw DEFLATE | yes | no | index import/export, byte/line ranges | yes | BSD-3-Clause AND MIT |
| `rusty-rapidgzip` 0.2.2 | library; no binary | gzip, multi-member, BGZF, raw DEFLATE primitives | yes | no | no index/seek | path/channel streaming | MIT (no license text shipped) |
| `rusty-rapidgzip-bin` 0.1.0 | binaries `rusty-rapidgzip-rs`, `bench-inflate` | gzip | yes, via 0.1.0 library | no | no | stdout streaming | MIT OR Apache-2.0 (no text shipped) |
| `gzippy` 1.0.0 | binary `gzippy` + library | gzip, BGZF, raw DEFLATE, Deflate64 | yes, single/multi-member | yes, gzip | custom GZIDX (single-member only) + `--seek` | yes | Zlib |
| `lgz` 0.3.17 | binary `lgz` + library | gzip, multi-member, BGZF/flush, raw DEFLATE segments | yes | no | no index/seek | yes | MIT OR Apache-2.0 (MIT text only) |
| `rsomics-igzip` 0.1.0 | library; no binary | gzip, concatenated members | no | no | no | `Read` streaming | MIT OR Apache-2.0 |
| `fbz` 0.1.15 | binary `fbz` + library | bzip2, gzip, raw DEFLATE, LZ4, tar, ZIP | yes | yes | bzip2 `.fbz2i` index + `Read + Seek` | yes | Apache-2.0 |
| `deacon` 0.19.1 | binary `deacon` | FASTQ/FASTA plain, gz, zst, xz; BINSEQ | gzip input: no; output: yes | gzip output via `gzp` | no | yes | MIT |
| `rdstream` 0.1.0 | binary `rdstream` | R `.rds`/`.RData` with gzip/bzip2/xz/zstd layers | optional, via `rapidgzip-core`/`rusty-rapidgzip` | no | no | yes | MIT OR Apache-2.0 |

Notes:

- `rapidgzip-core` and `rapidgzip-rust-cli` are the only ones here that handle zlib and raw
  DEFLATE as user-facing containers in addition to gzip/BGZF.
- `gzippy` handles Deflate64 (ZIP method 9) and a single-member custom GZIDX index, but its
  `-z` (zlib) and `-K` (single-entry ZIP) claims are rejected by the shipped CLI. Its library
  API is gzip/raw DEFLATE/Deflate64 only.
- `lgz` advertises tar extraction and rayon in the README, but the shipped CLI only writes a
  decompressed file and the manifest has no rayon dependency.
- `deacon` and `rdstream` are not codec benchmark harnesses. `deacon` is a DNA search tool that
  consumes compressed FASTX; `rdstream` is an R data reader that peels nested compression layers.
  Both are useful downstream integration tests, not A/B drivers.

## 3. Capability matrix: bzip2 and multi-format

| Crate (version) | Kind / CLI | Formats | Parallel decode | Compression | Index / seek | Stream | License |
|---|---|---|---|---|---|---|---|
| `bzip2-turbo` 0.2.1 | library; no binary | bzip2 | yes, block workers | yes, parallel | no | `Read`/`Write` | LGPL-2.1-or-later AND MIT |
| `oxiarc-bzip2` 0.4.2 | library; no binary | bzip2 | no (parallel is compression only) | yes, rayon | no | `Read`/`Write` | Apache-2.0 |
| `crabz2` 0.7.0 | library + examples; no installed CLI | bzip2 | yes, rayon whole-buffer | yes | no | serial streaming; `no_std + alloc` core | MIT |
| `parallel_bzip2_decoder` 0.2.1 | library; no binary | bzip2 | yes, Rayon + scan pool + driver thread | no | no | `Read` streaming | MIT (no license file shipped) |
| `decode-bzip2` 0.0.4 | library; no binary | bzip2 | no (serial) | no | checkpoint/resume between blocks, not arbitrary seek | sans-I/O streaming; `no_std + alloc` | Unlicense (no license text shipped) |
| `mibz2` 0.3.1 | library + optional Python extension + examples | bzip2 | yes, block-parallel custom teams | no | block scan + `decode_blocks` random access | `BzReader` streaming | MIT OR Apache-2.0 |
| `lbzip2` 0.5.13 | library + binary `lbunzip2` | bzip2 | yes, `rotaryengine`/`gatling` | no | block-scan primitives; no index/seek | `Read` streaming | MIT OR Apache-2.0 AND bzip2-1.0.6 (precedence issue) |
| `fbz` 0.1.15 | binary `fbz` + library | bzip2, gzip, raw DEFLATE, LZ4, tar, ZIP | yes | yes | bzip2 index + `Read + Seek` | yes | Apache-2.0 |
| `pbz2` 0.2.0 (candidate) | binary `pbz2` + library | bzip2 | yes | yes | no | `Read`/`Write` | MIT |
| `parallel-bz2-redux` 1.1.1 (candidate) | library | bzip2 | yes | yes | no | `Read`/`Write` | CC0-1.0; owner name and release cadence warrant verification |

## 4. Seekability and random access for ratarmount

This is the dimension that matters most for ratarmount. A codec can be fast and still be
useless as a mount backend if it cannot answer `read` at an arbitrary decompressed offset.

### 4.1 What ratarmount actually needs

`ratarmountcore` consumes a Python file-like object with `seekable()`, `seek()`, `read()`, and
`tell()`. Two interfaces go beyond that:

- gzip: `import_index()` and `export_index()`, because the SQLite index stores the gzip index
  as a blob. `rapidgzip.RapidgzipFile` also exports `IndexFormat.GZTOOL` when available.
- bzip2/zstd: `set_block_offsets()` and `block_offsets()`, so the block boundaries can be
  persisted in SQLite. `tell_compressed()` is used for progress and archive-offset mapping;
  for bzip2 it returns a bit offset and is divided by 8.

The mount path also needs multi-member/BGZF gzip support, because tar.gz is normally a single
gzip member but BGZF and concatenated members are common in bioinformatics. A backend that only
indexes single-member gzip is not a general tar.gz replacement.

A Rust crate therefore needs either a Python/PyO3 wrapper exposing that interface or a small
helper binary; spawning a CLI per FUSE read is not viable.

### 4.2 gzip random access

| Crate | Decompressed `Read + Seek` | Index | Index formats | Granularity / memory | Ratarmount fit |
|---|---|---|---|---|---|
| `rapidgzip-core` 0.3.1 | yes, `IndexedReader<R: ReadAt>` | built or imported; `DeflateIndex` | native RGZIDX01, indexed_gzip GZIDX v0/1, htslib `.gzi`, gztool v0/1 (with optional line metadata) | checkpoint + stored window; reads source through `ReadAt`; no whole-file heap copy | **Best Rust gzip candidate.** Supports multi-member/BGZF, byte and line seek, and `SeekFrom::End` when the index carries uncompressed size. Needs a PyO3 wrapper. |
| `rapidgzip` 1.2.1 / `rapidgzip-sys` 1.2.1 | yes, `Seek` on `Reader`; `SeekFrom::Start/Current`, **not `End`** | native index import/export; `keep_index` | indexed_gzip GZIDX by C++ default; no format selection exposed | C++ 0.16.0 block index and window; path or callback `Read + Seek`; no whole-file heap copy | **Second choice.** Practical if the C++ engine is required behind Rust, but it vendors 0.16.0, has no format selection, and cannot seek from end. |
| `rapidgzip-rust-cli` 0.3.1 | no `Seek` object, but `--ranges 'size@offset'` | import/export | same as `rapidgzip-core` | CLI-level range extraction | Useful for testing/index generation, not a FUSE backend. |
| `rapidgzip-rs-cli` 1.2.1 | no range/seek CLI | import/export for parallel decode | C++ indexed_gzip GZIDX | full-stream output only | Not a ratarmount backend. |
| `gzippy` 1.0.0 | no `Seek` trait; `seek_decompress` function and `--seek` | custom `GZIDX\x01/\x02` | not interoperable with indexed_gzip/rapidgzip | whole `gzip_data: &[u8]`; 4 MiB checkpoints with a 32 KiB window each; `--seek` materializes the tail with `max_output = len*32` when `max_bytes=u64::MAX` | **Not suitable for ratarmount.** Rejects multi-member/BGZF indexing, needs the whole compressed file in memory, and the CLI seek path can allocate the whole output. |
| `rusty-rapidgzip` 0.2.2 | no | none | none | streaming/path/channel; internal block finder only | Streaming only. |
| `rusty-rapidgzip-bin` 0.1.0 | no | none | none | stdout | No. |
| `lgz` 0.3.17 | no | none | none | streaming; internal speculative/flush finders | Streaming only. |
| `rsomics-igzip` 0.1.0 | no | none | none | `Read` streaming | No. |
| `fbz` 0.1.15 | bzip2 only | bzip2 index | custom `.fbz2i` | see 4.3 | gzip stream is streaming-only; use it for bzip2. |
| `deacon`, `rdstream` | not codec backends | n/a | n/a | n/a | Downstream workloads, not mount backends. |

Important details:

- `rapidgzip-core` is not just a full-stream decoder. `Decoder::decode_with_index` builds an
  index, `DeflateIndex` serializes/deserializes it, and `IndexedReader` implements
  `Read + Seek` over `ReadAt`. This is the right shape for a ratarmount backend.
- `rapidgzip`'s `seek(SeekFrom::End(_))` returns `Unsupported` (`native_impl.rs:805`). If
  ratarmount calls `seek(0, SEEK_END)` to determine the size, this wrapper needs a size shim.
- `gzippy`'s index magic looks like indexed_gzip's but the layout is different: after the
  6-byte magic it writes a `u32` deflate offset, then total uncompressed size and a point
  count. It is not the indexed_gzip header (compressed size, uncompressed size, spacing,
  window size, checkpoint count). It is not compatible with ratarmount's `gzipindex` blob.
- `rapidgzip-rs-cli` imports/exports indexes to speed up parallel full-stream decode; it does
  not expose an offset/range option in the CLI.

### 4.3 bzip2 random access

| Crate | Decompressed `Read + Seek` | Index | Index formats | Granularity / memory | Ratarmount fit |
|---|---|---|---|---|---|
| `fbz` 0.1.15 | yes, `IndexedReader` (`Read + Seek`) | built, loaded, saved; per-block cache (64 MiB default) | custom `FBZ2IDX\0` with BLAKE3 source hash and per-block CRC | block-aligned; `Source` is a `Vec<u8>` or mmap; `seek` works at any decompressed byte offset; `SeekFrom::End` supported | **Best bzip2 candidate.** It has a Rust `IndexedReader` and a Python `fbz.open_indexed` / `IndexedBzip2File` with `seek/tell`. Caveats: custom index, path/bytes-only `Source` (no arbitrary Python file object), no `tell_compressed`/`set_block_offsets` methods yet. |
| `mibz2` 0.3.1 | no `Seek` object | `scan()` yields `BlockSpan`s; `decode_blocks()` decodes arbitrary block ranges | no persistent index | `scan`/`decode_blocks` take `&[u8]`; caller would mmap and build a block index/cache; block CRC checked | **Good raw material, not a backend.** Block-level random access is exactly what ratarmount needs, but there is no reader, index format, or block cache. |
| `lbzip2` 0.5.13 | no | `block_scan`/`chunk` primitives only | none | whole-buffer or streaming; block boundaries private to the reader | **Primitives only.** Could build an index, but no public `Read + Seek` or block cache. |
| `decode-bzip2` 0.0.4 | no | `Decoder::checkpoint()` between blocks | none | caller must track compressed and decoded positions | **Low-level checkpoint only.** Not a persistent index and not a seekable reader. |
| `bzip2-turbo` 0.2.1 | no | none | none | `Read`/`Write` | Streaming only. |
| `oxiarc-bzip2` 0.4.2 | no | none | none | `Read`/`Write` | Streaming only. |
| `crabz2` 0.7.0 | no | none | none | serial `reader`; `decompress_parallel` is whole-buffer | Not a ratarmount backend. |
| `parallel_bzip2_decoder` 0.2.1 | no | none | none | `Read` streaming; scanner internals | No, and it also fails open on truncation. |
| `pbz2` 0.2.0 (candidate) | no | none | none | `Read`/`Write` | Streaming only. |

Important details:

- `fbz` is the only bzip2 crate here that already exposes a Python seekable file object:
  `fbz.open_indexed(path_or_bytes)` returns an `IndexedBzip2File` (`io.RawIOBase`), and
  `fbz.build_index()` returns the encoded `.fbz2i` index. That is the shortest path to a
  ratarmount bzip2 backend.
- Ratarmount's SQLite persistence expects `set_block_offsets`/`block_offsets`, which `fbz`
  does not expose. It does expose the index and block table, so a thin wrapper can add the
  methods. Without them, ratarmount will rebuild or warn about slow subsequent loads.
- `mibz2`'s `scan(data: &[u8])` and `decode_blocks(data: &[u8], ...)` require the entire
  compressed input as a contiguous slice. An mmap of a local file works; a remote or nested
  Python file object does not. That is a real limitation for ratarmount's layered mounts.
- `lbzip2` and `decode-bzip2` are useful if a custom index is needed, but they do not remove the
  work of implementing the block-offset table, cache, and `seek` semantics.

### 4.4 Practical recommendation for ratarmount

1. **gzip:** prototype a PyO3 wrapper around `rapidgzip-core::IndexedReader` over a `ReadAt`
   adapter for the Python file object. Expose `seekable/seek/read/tell`, `seek_to_line`,
   `import_index`/`export_index`, and the index-format selection. This is the only Rust
   implementation with multi-member/BGZF support, standard GZIDX/.gzi/gztool interop, and
   line-annotated indexes. Keep the existing C++ `rapidgzip` Python module as the fallback
   until the wrapper passes the ratarmount test suite.
2. **bzip2:** use `fbz`'s `IndexedBzip2File` as the first prototype. Add a small wrapper for
   `tell_compressed` and `set_block_offsets`/`block_offsets`, and either persist the `.fbz2i`
   blob or map its block table into ratarmount's SQLite schema. `mibz2` is relevant only if its
   block decoding speed justifies the extra index/caching work.
3. **`gzippy` is not a suitable tar.gz backend for ratarmount.** Multi-member/BGZF indexes are
   rejected, the index is private, and the seek path is whole-buffer. It may still be useful as a
   throughput benchmark.
4. **Streaming-only crates** (`lgz`, `rusty-rapidgzip`, `rsomics-igzip`, `bzip2-turbo`,
   `oxiarc-bzip2`, `crabz2`, `parallel_bzip2_decoder`, `pbz2`) remain benchmark candidates
   and possible full-extraction backends, not random-access mount backends.
5. **The C++ `rapidgzip` wrapper is not the COMBINE-lab project.** `rapidgzip` 1.2.1 is a
   Rust wrapper over vendored librapidarchive 0.16.0; it is seekable, but it is not the
   current C++ rapidgzip and it exports indexed_gzip GZIDX without format selection.

## 5. Health and activity

Age is days since first publication. "Last" is the latest publish. "DL" is total/recent
downloads from crates.io on 2026-11-15. "Rev" is crates.io reverse-dependency count.

| Crate (version) | Owner | First -> last publish | Age | Versions | DL (recent) | Rev | Repo activity / status |
|---|---|---|---:|---:|---|---:|---|
| `rapidgzip` 1.2.1 | `alekseizarubin` | 2026-03-25 -> 2026-04-01 | 235 | 5 | 9,151 (6,033) | 1 | 15 commits, 2026-03-25..04-01; dormant 7.5 months |
| `rapidgzip-sys` 1.2.1 | `alekseizarubin` | 2026-03-25 -> 2026-04-01 | 235 | 5 | 9,294 (6,109) | 1 | same repo; dormant |
| `rapidgzip-rs-cli` 1.2.1 | `alekseizarubin` | 2026-03-26 -> 2026-04-01 | 234 | 4 | 83 (6) | 0 | 10 commits, 2026-03-24..04-01; dormant |
| `rapidgzip-core` 0.3.1 | `rob-p` / COMBINE-lab | 2026-08-01 -> 2026-08-10 | 106 | 5 | 7,443 (6,743) | 10 | 97 commits in 11 days; dormant 3 months; 23 stars |
| `rapidgzip-rust-cli` 0.3.1 | `rob-p` / COMBINE-lab | 2026-08-01 -> 2026-08-10 | 106 | 5 | 123 (27) | 0 | same repo; dormant 3 months |
| `rusty-rapidgzip` 0.2.2 | `TyberiusPrime` | 2026-06-18 -> 2026-11-06 | 150 | 4 | 770 (518) | 2 | actual repo `rusty_rapidgzip` created 2026-05-26, pushed 2026-11-06; 0 stars |
| `rusty-rapidgzip-bin` 0.1.0 | `TyberiusPrime` | 2026-06-18 -> 2026-06-18 | 150 | 1 | 19 (1) | 0 | no README, no license text, pins library 0.1.0; single release |
| `fbz` 0.1.15 | `jph00` / AnswerDotAI | 2026-08-25 -> 2026-11-03 | 82 | 8 | 1,505 (1,505) | 0 | 51 commits in 10 weeks, one author, CI; active |
| `gzippy` 1.0.0 | `JackDanger` | 2026-05-14 -> 2026-10-01 | 185 | 3 | 200 (79) | 0 | 2,908 commits, 175 PRs, 4 CI workflows; pushed 2026-11-12; 1 human contributor |
| `lgz` 0.3.17 | `Ignalina` | 2026-05-21 -> 2026-11-03 | 178 | 15 | 3,982 (2,605) | 7 | Codeberg mirror stops 2026-10-15 at 0.3.14; new forge is SSH-only; no CI in mirror |
| `deacon` 0.19.1 | `bede` | 2025-03-14 -> 2026-10-26 | 611 | 24 | 6,354 (368) | n/a | 445 commits, 8 contributors, pushed 2026-11-11, 128 stars; mature |
| `rdstream` 0.1.0 | `HiroGitea` | 2026-10-15 -> 2026-10-15 | 31 | 1 | 139 (139) | n/a | 6 commits in 40 minutes, one author; one release |
| `rsomics-igzip` 0.1.0 | `Bengerthelorf` | 2026-05-16 -> 2026-05-16 | 183 | 1 | 423 (3) | 0 | 2,859 commits in 6 months; crate path removed from HEAD; frozen |
| `bzip2-turbo` 0.2.1 | `NZBMan` / scryer-media | 2026-11-06 -> 2026-11-11 | 9 | 4 | 3,593 (3,593) | 1 | 78 commits in 6 days; 4 CI workflows; very active |
| `oxiarc-bzip2` 0.4.2 | `cool-japan` | 2026-01-12 -> 2026-09-12 | 307 | 20 | 22,374 (11,365) | 3 | 20 snapshot commits; no CI in clone; monorepo pushed 2026-11-13 |
| `crabz2` 0.7.0 | `jwmurray` | 2026-07-01 -> 2026-08-24 | 137 | 9 | 8,517 (7,610) | 1 | 51 commits, one author, 4 CI workflows; dormant 3 months |
| `parallel_bzip2_decoder` 0.2.1 | `kassoulet` | 2025-12-06 -> 2026-03-05 | 344 | 3 | 1,167 (93) | 1 | 50 commits, last push 2026-05-30; unpublished fixes; dormant |
| `decode-bzip2` 0.0.4 | `artob` | 2026-11-04 -> 2026-11-05 | 11 | 3 | 87 (87) | 1 | 340 commits in 14 days in `filer` monorepo; active |
| `mibz2` 0.3.1 | `lmmx` | 2026-10-27 -> 2026-11-14 | 19 | 19 | 879 (879) | 0 | 501 commits in 19 days, 18 workflows incl. Miri; very active |
| `lbzip2` 0.5.13 | `Ignalina` | 2026-05-15 -> 2026-10-09 | 184 | 16 | 5,067 (3,650) | 3 | 467 commits, last push 2026-10-15; repo says 0.5.12; no CI in mirror |

Activity summary:

- **No recent pushes:** `rapidgzip`, `rapidgzip-sys`, `rapidgzip-rs-cli`,
  `rapidgzip-core`, `rapidgzip-rust-cli`, `crabz2`, `parallel_bzip2_decoder`.
- **Recently active:** `rusty-rapidgzip`, `fbz`, `gzippy`, `lgz`, `bzip2-turbo`,
  `decode-bzip2`, `mibz2`, `lbzip2`.
- **Single release or frozen:** `rusty-rapidgzip-bin`, `rdstream`, `rsomics-igzip`.
- **Long-running:** `deacon` only.

### 5.1 Public AI-related repository artifacts

The following are public repository artifacts, not a quality judgment. They are listed because they
are relevant when estimating review effort, not because AI involvement is itself a problem. For the
"agent team" pattern referenced by several projects, see Answer.AI's description of its agent
workflow: <https://www.answer.ai/posts/2023-12-12-launch.html>. The `mibz2` README also describes
its agent-team workflow: <https://github.com/lmmx/mibz2#working-with-the-agent-team>.

| Crate | Public artifacts |
|---|---|
| `rapidgzip-core` / `rapidgzip-rust-cli` | 16 commits with Claude co-author trailers and 4 Copilot bot commits in the public history; 6,900-line initial commit. |
| `gzippy` | 59.7% of 2,908 commits carry Claude/Cursor co-author trailers or Claude-Session links; `CLAUDE.md` and `.cursorrules` are shipped. |
| `lgz` | `CLAUDE.md` and `.nornir/` agent documentation; one commit message mentions "pre token-reset". |
| `rusty-rapidgzip` | The repository description describes the project as "Vibed rapidgzip in rust" (self-description). |
| `rsomics-igzip` | `CLAUDE.md`, `AGENTS.md`, and `.autopilot/` are present; commit cadence peaks above 200 commits/day. |
| `bzip2-turbo` | A 15,510-line root commit; an `AGENTS.md` model-reviewer line was removed later. |
| `oxiarc-bzip2` | A 1M-line snapshot commit with one author. |
| `crabz2` | Five commits titled "Code commit". |
| `parallel_bzip2_decoder` | Jules/Sentinel bot commits in the public history. |
| `decode-bzip2` | `AGENTS.md` names GPT-5.6 Sol/Terra/Luna. |
| `mibz2` | Agent-team documentation, `CLAUDE.md`, and `CHATGPT.md`; the README links the agent-team workflow. |
| `lbzip2` | "Ideas from Claude" in `design.md`; `CLAUDE.md` is present. |
| `fbz` | A 4,497-line single commit by one author; no agent files found. |
| `rdstream` | Six commits in 40 minutes; no agent files found. |
| `rapidgzip` / `rapidgzip-sys` / `rapidgzip-rs-cli` | A vendor import and a small wrapper; no AI artifacts found. |
| `deacon` | No AI artifacts found. |

## 6. Supply-chain and provenance matrix

Risk ratings are for the published crate as is. "Supply-chain" covers build-time behavior,
provenance, and unexpected native code. "Integrity/security" covers decoder correctness and
untrusted-input behavior found statically.

| Crate | Build/native surface | Published vs repository | Supply-chain risk | Integrity/security notes |
|---|---|---|---|---|
| `rapidgzip` | no build.rs; links `rapidgzip-sys` | `.cargo_vcs_info` matches `v1.2.1`; only normalized `Cargo.toml` differs | Medium (inherits `-sys`) | safe wrapper; no known malware |
| `rapidgzip-sys` | `build.rs` + CMake; 18.5 MB vendored C/C++; no prebuilt files shipped | matches `v1.2.1`; vendored librapidarchive is 0.16.0 | Medium | `RAPIDGZIP_USE_PREBUILT` can link `prebuilt/<target>` if added; CMake can run `git submodule update` if `src/external` is stripped; 4 undocumented local patches |
| `rapidgzip-rs-cli` | no build.rs; runtime HTTP for URLs | matches `v1.2.1` | Low | only crate in this family with runtime network (user-supplied HTTP/HTTPS) |
| `rapidgzip-core` | no build.rs; pure Rust; zlib-rs via `libz-rs-sys`; 61 `unsafe` sites | matches `v0.3.1` | Low | 10 reverse deps; static review clean |
| `rapidgzip-rust-cli` | no build.rs; zero `unsafe` in CLI source | matches `v0.3.1` | Low | static review clean |
| `rusty-rapidgzip` | `build.rs` only for optional ISA-L; hard-coded `/nix/store` path + rpath; C shim; vendored C via `libdeflate-sys` default | VCS SHA matches public underscore repo; manifest repo URL is 404; content diff not done | Medium/High | BGZF ISIZE can reserve GiBs; streaming path can buffer whole stream; fast kernel reads uninitialized spare capacity; no license text |
| `rusty-rapidgzip-bin` | no build.rs; no embedded data | direct publish; no repo diff | Medium | pins library 0.1.0, not 0.2.2; no README; no license text |
| `fbz` | no build.rs; no crate-local C; `blake3` etc. may compile native code | VCS SHA = `v0.1.15`; `src/` = HEAD | Low | `--no-verify` publish; ZIP extraction preserves setuid/setgid/sticky bits; output inherits input mode |
| `gzippy` | `build.rs` runs `git rev-parse`; can install git hooks; optional Zopfli C only under `oracle` | packaged from dirty commit `37da02b`, not tag `b9bf30e`; 93 extra files outside include list | Medium | parallel BGZF/multi-member skip CRC32; `--seek` can OOM; docs contradict flags |
| `lgz` | no build.rs; no crate-local C; 38 `unsafe` sites | VCS SHA `03ff44e` absent from public mirror; new forge is SSH-only | High | no malware found; source provenance unverifiable; in-memory API silently single-member |
| `deacon` | no build.rs; no crate-local FFI; large native dependency graph | not applicable | Medium (runtime trust) | index download has no checksum/signature and is bincode-decoded; local server has no auth |
| `rdstream` | no build.rs; no crate-local FFI | one release; repo pushed same day | Low | very young; R fixture test disabled unless generated |
| `rsomics-igzip` | no build.rs; depends on `isal-sys` build.rs (not audited) | VCS SHA = tag; source identical; crate path removed from HEAD | Medium | silent truncation when a member ends at the 4 MiB read boundary; `ISAL_END_INPUT` accepted as valid header; frozen |
| `bzip2-turbo` | no build.rs; no C; mmap/madvise unsafe only | VCS SHA = HEAD | Low (supply chain); High (license) | LGPL-2.1-or-later codec; not a permissive `bzip2` drop-in |
| `oxiarc-bzip2` | no build.rs; pure Rust; 0 unsafe | VCS SHA = `origin/0.4.2`; content identical | Medium | encoder is a declared faithful libbz2 port but crate is Apache-2.0 only with no bzip2 notice; no CI |
| `crabz2` | no build.rs; no runtime deps by default; `forbid(unsafe_code)` | VCS SHA = release commit; no `v0.7.0` tag | Low | macOS library path spawns `/usr/sbin/sysctl`; benign, failure-tolerant |
| `parallel_bzip2_decoder` | no build.rs; C libbz2 via `bzip2-sys`; mmap unsafe | VCS SHA = `v0.2.1`; repo has unpublished fixes | Medium | **High integrity:** discards worker errors, `Ok(0)` on channel close, accepts `UnexpectedEof`, no stream CRC |
| `decode-bzip2` | no build.rs; zero unsafe; `forbid(unsafe_code)` | VCS SHA = tag 0.0.4; HEAD 9 days newer | Low | 0.0.x; published version lacks HEAD `settle()` fix; no license text |
| `mibz2` | no build.rs; std-only default; ~230 unsafe SIMD/asm sites; optional pyo3 | VCS SHA = release bump; HEAD adds `phase.rs` | Medium | research features shipped; PCLMUL CRC folding has a patent note; 237 tests incl. Miri |
| `lbzip2` | no build.rs; 15 unsafe sites; `rotaryengine`/`gatling` deps | VCS SHA `ef366d3b` absent; repo says 0.5.12; no 0.5.13 tag | Medium/High | code matches master; release identity broken; license expression precedence issue; no CI in mirror |

## 7. Detailed observations

### 7.1 `gzippy`: packaging, build hook, and CRC observations

- **Not the tagged release.** `.cargo_vcs_info.json` records
  `37da02ba...` with `"dirty": true`; tag `v1.0.0` is `b9bf30ed...`. `git diff --stat` is 50
  files, +663/-1069. The tarball also contains 93 files outside the declared `include` list,
  including `rapidgzip/**`, `vendor/zlib-ng/**`, `vendor/isa-l/**`, and `libdeflate/README.md`.
  This is consistent with packaging from a worktree with adjacent checkouts, not from the tag.
- **Build-time hook writes.** `build.rs:201-246` runs `git rev-parse --git-path hooks`, then
  `create_dir_all` and writes `scripts/pre-commit`, `scripts/pre-push`, `scripts/commit-msg`,
  `chmod 0755`. The published crate does not ship `scripts/`, so a registry build is a no-op
  after `create_dir_all`; a git/path/vendored build writes into the enclosing repository and
  honours `core.hooksPath`. The write targets the enclosing repository; it is not exfiltration.
- **No CRC32 on parallel BGZF/multi-member.** `src/decompress/bgzf/mod.rs` and
  `src/decompress/bgzf/multi_member.rs` compare only decoded length against ISIZE. The error
  strings still say "CRC32 or size mismatch". A corrupt block that preserves ISIZE is written
  and reported as success. The single-member driver, stored-split path, and sequential
  multi-member walk do check CRC.
- **`--seek` can OOM.** `decompress.rs:405-446` sets `max_output = deflate_data.len() * 32`
  and allocates `vec![0u8; max(8 MiB, max_output + window)]`, materializing the tail instead of
  seeking.

### 7.2 The rapidgzip names are split between two unrelated projects

- `rapidgzip`, `rapidgzip-sys`, and `rapidgzip-rs-cli` are `alekseizarubin` crates. Their
  repository is `github.com/alekseizarubin/rapidgzip-rs`, not COMBINE-lab.
- `rapidgzip-sys` vendors librapidarchive **0.16.0**, not the crate's 1.2.1 version. 95 of 99
  first-party C++ files are byte-identical to upstream 0.16.0. Four files differ: a CMake
  header-output fix, a 1 MiB `BitReader` refill, a `ChunkData` include, and an undocumented
  `ParallelGzipReader` buffer-lifetime fix.
- `rapidgzip-sys/build.rs` has a `RAPIDGZIP_USE_PREBUILT` path that links
  `prebuilt/<target>/librapidgzip-capi.a` (plus rpmalloc/ISA-L/zlib) instead of compiling. The
  published crate has no `prebuilt/`, and the repo gitignores it, so the tarball is safe; a
  modified checkout or repackager could bypass source review.
- CMake is hermetic in the published tarball: `FetchContent` uses a local `SOURCE_DIR` and all
  external sources are present. A gated branch runs `git submodule update --recursive --init`
  only if `src/external` is stripped.
- COMBINE-lab's `rapidgzip-core` is the independent pure-Rust implementation. It is the one
  with 10 reverse dependencies (`rdstream`, `salmon-*`, `piscem-rs`, `cuttlefish-rs`, etc.).

### 7.3 `lgz` and `lbzip2`: provenance observations

- `lgz` 0.3.17: `.cargo_vcs_info` SHA `03ff44e8...` is absent from the public Codeberg mirror
  (`git cat-file` / remote returns "not our ref"). The mirror stops at 0.3.14 (2026-10-15);
  0.3.15-0.3.17 add whole modules and tests that are not in any clone ref. The crate links to
  `git.gunnar.rs`, which serves git over SSH only.
- `lbzip2` 0.5.13: `.cargo_vcs_info` SHA `ef366d3b...` is absent; repo says 0.5.12; tags stop
  at `lbzip2-v0.5.12`. The source tree matches master, so the code is reviewable, but the
  release identity is not reproducible.
- The author's own `Cargo.toml.orig` for `lgz` states that local source "DRIFTED from the
  published 0.3.13 at the SAME version" and that "ALL ELEVEN crates here were drifted". That is
  a release-hygiene red flag independent of the 0.3.17 mismatch.

### 7.4 `rusty-rapidgzip`: repository URL and license metadata

- Manifest and README point to `github.com/TyberiusPrime/rusty-rapidgzip` (hyphen), which
  returns HTTP 404. The actual public repo is `TyberiusPrime/rusty_rapidgzip` (underscore),
  created 2026-05-26, 0 stars, license `NOASSERTION`.
- The 0.2.2 VCS SHA matches the trusted-publishing SHA in the underscore repo, so the release
  commit is tied to a public repository; a content-level diff was not performed.
- Neither `rusty-rapidgzip` nor its bin crate ships a `LICENSE`/`COPYING`/`NOTICE` despite
  declaring MIT / MIT OR Apache-2.0. The README links a missing license and the zlib-rs-derived
  `src/deflate` attribution is absent.
- Static integrity issues: BGZF `ISIZE` is reserved without a 64 KiB cap; the streaming path can
  buffer an entire stored/fixed-only stream; the fast kernel can read uninitialized spare
  capacity; Miri only covers `safe_inflate`; there is no fuzz target in the tarball.

### 7.5 `parallel_bzip2_decoder`: error and CRC handling

- `src/decoder.rs:162` discards the whole `try_for_each` result (`let _ = ...`), so every
  worker `Bz2Error` is lost.
- `src/decoder.rs:260-262` returns `Ok(0)` when the result channel closes.
- `src/lib.rs:311` maps `UnexpectedEof` to `Ok(())` with the comment that it is expected for the
  last block. Any earlier truncation takes the same path.
- There is no stream CRC check; `src/lib.rs:294-295` prepends a synthetic `BZh9` header and
  decodes a block slice, so the combined stream CRC is never verified.
- The repo has fixes from 2026-04-29/2026-05-29, but 0.2.1 (2026-03-05) is the latest publish.
  It is suitable for trusted input only.

### 7.6 Licensing and provenance issues in bzip2 crates

- `bzip2-turbo` is a translation of 7-Zip's LGPL-2.1-or-later bzip2 codec. The manifest is
  `LGPL-2.1-or-later AND MIT`; the codec files carry the LGPL notice. It is not a permissive
  drop-in for the `bzip2` crate, despite the "replace the dependency" pitch.
- `oxiarc-bzip2` says in `src/encode.rs:128` that it is "a faithful port of libbz2's
  `sendMTFValues`" and copies the 512-entry `BZ2_rNums` table, but the crate is Apache-2.0 only
  and ships no bzip2/Julian Seward notice.
- `parallel_bzip2_decoder` declares MIT and lists `LICENSE` in `include`, but no license file is
  in the package. `decode-bzip2` declares Unlicense with no license text in the package.
- `lbzip2` declares `MIT OR Apache-2.0 AND bzip2-1.0.6`, which parses as
  `MIT OR (Apache-2.0 AND bzip2-1.0.6)`, so a consumer may read it as MIT-only.

### 7.7 `rsomics-igzip`: frozen ISA-L wrapper and boundary observation

- `src/lib.rs:285-290`: when a member ends with `ISAL_BLOCK_FINISH` and `avail_in == 0`, the
  code returns `Ok(false)` without checking whether the file has more bytes. `read` then sets
  `done = true`. A concatenated gzip/BGZF member boundary exactly at the 4 MiB input buffer
  boundary silently drops all following members. BGZF blocks are <= 64 KiB and often aligned,
  so this is realistic.
- `ISAL_END_INPUT` from `isal_read_gzip_header` is accepted as success in `new` and in the
  member reinit, although it means the header was not fully available.
- Only release 0.1.0 (2026-05-16); the crate path was removed from the repo HEAD and docs call
  it a historical namespace exception. Treat as frozen.
- The `isal-sys` build script was not audited locally; it compiles bundled ISA-L C/asm.

### 7.8 `fbz` and `deacon`: integration notes

- `fbz` has reproducible provenance: VCS SHA equals `v0.1.15`, `src/` equals HEAD, no
  crate-local C, no network/process code. Caveats: CI publishes with `cargo publish --no-verify`;
  ZIP extraction preserves setuid/setgid/sticky bits (`mode & 0o7777`); standalone output
  inherits input permissions. Running it as root on hostile archives is not advisable.
- `deacon` is mature (20 months, 8 contributors, 128 stars) but performs an HTTPS index fetch
  with no checksum/signature, then bincode-decodes the result. A malicious mirror selected via
  `DEACON_REPOSITORY_URL` is equally unverified. Its local Unix-socket server has no
  authentication. It is a downstream workload, not a codec harness.
- `rdstream` is clean on the classic supply-chain checks and is a real consumer of both
  `rapidgzip-core` and `rusty-rapidgzip`, but it is one release and six commits in 40 minutes.

### 7.9 No known advisories

OSV returned zero advisories for every queried crate name on 2026-11-15. That only means no
public advisory matched; it does not cover the static integrity findings above, dependency
build scripts, or future releases.

## 8. Benchmark recommendations

### 8.1 Priority for the benchmark matrix

| Priority | Crate / binary | Why | Caveat |
|---|---|---|---|
| P0 | `rapidgzip-core` + `rapidgzip-rust-cli` | Clean pure-Rust peer of the marker/window algorithm; gzip/zlib/raw/BGZF; index/seek; 10 reverse deps; source matches tag; strongest gzip ratarmount candidate | Last publish 2026-08-10; MSRV 1.88; needs a PyO3 wrapper for ratarmount |
| P0 | `gzippy` | Real parallel single-member and multi-member decode; drop-in CLI; large test suite | Dirty package, hook-writing build.rs, missing CRC on parallel BGZF/multi-member; **not a ratarmount backend** (multi-member index rejected, whole-buffer seek); run isolated |
| P0 | `pbz2` 0.2.0 (candidate) | bzip2 CLI with `-n`, `-z/-d/-t/-c/-k`, levels; parallel | Not audited in this pass; verify source before adding |
| P1 | `crabz2` 0.7.0 | Permissive pure-Rust bzip2 encoder+decoder; `forbid(unsafe_code)` by default | No installed CLI; whole-buffer parallel decode; dormant 3 months |
| P1 | `lbzip2` 0.5.13 | Parallel bzip2 library + `lbunzip2` CLI; output caps | Provenance broken (0.5.13 not tagged), license expression, no CI in mirror |
| P1 | `gzip-ng` 2.1.0 (candidate) | Parallel gzip replacement built on zlib-ng by a zlib-ng maintainer | Very new and low adoption; verify |
| P0 (bzip2 seek) | `fbz` 0.1.15 | Multi-format CLI; good provenance; parallel gzip/bzip2/LZ4/ZIP; only bzip2 crate with `IndexedReader` + Python `open_indexed` (`Read+Seek`) | Custom `.fbz2i` index; no `tell_compressed`/`set_block_offsets` yet; `--no-verify` publish; privilege-bit issue |
| P2 | `lgz` 0.3.17 | Parallel gzip decoder, `lgz` CLI | Cannot tie published source to a public commit; verify on the new forge first |
| P2 | `rusty-rapidgzip` 0.2.2 | Streaming/speculative gzip; used by `rdstream` | Repo URL 404, no license text, BGZF/streaming memory issues; benchmark only from an isolated copy |
| P2 | `bzip2-turbo` 0.2.1 | Fast bzip2 encoder+decoder, parallel | LGPL codec; benchmark only, do not vendor |
| P2 | `mibz2` 0.3.1 | Block-parallel bzip2 with block-level random access | No `Read+Seek` or persistent index; needs whole compressed slice/mmap; research code, heavy unsafe, patent note |
| P2 | `indexed_deflate` 0.1.0, `mim-index` 0.1.1 (candidates) | Index/random-access comparisons | `indexed_deflate` is one release; `mim-index` is from the COMBINE-lab authors |
| Low priority | `parallel_bzip2_decoder` 0.2.1 | Parallel bzip2 | Discards worker errors, accepts truncation, and does not check the stream CRC |
| Low priority | `rsomics-igzip` 0.1.0 | ISA-L wrapper | No longer maintained at HEAD; 4 MiB boundary truncation; not parallel |
| Low priority | `oxiarc-bzip2` 0.4.2 | Pure-Rust bzip2 | License/provenance gap; no CI |
| P2 (gzip seek bridge) | `rapidgzip`/`rapidgzip-sys` as "upstream rapidgzip" | FFI wrapper over librapidarchive 0.16.0; `Read+Seek` and indexed_gzip GZIDX export | Name collision; heavy CMake build; not the current C++ project; no `SeekFrom::End`; use only as a C++-via-Rust bridge |
| Not a harness | `deacon`, `rdstream` | Downstream workloads | Not codec A/B drivers |

### 8.2 Integration notes

- Build the Rust crates in isolated worktrees. `gzippy`'s `build.rs` can modify an enclosing
  git checkout; `rapidgzip-sys` can use `prebuilt/` if present; `rusty-rapidgzip`'s ISA-L
  feature embeds a `/nix/store` rpath.
- For library-only crates, write a small driver that uses the same input file, output sink, and
  thread count as the CLI baselines. A library's in-memory decode should not be compared to a CLI
  that writes to disk without accounting for I/O.
- Record the exact crate version, license, and binary identity. `gzippy` already has a
  `BUILD_FLAVOR` string; use it to exclude instrumented builds.
- For bzip2, separate block-scan throughput from full decode and from inverse-BWT/SIMD kernels.
  `mibz2` and `lbzip2` expose different stages; `bzip2-turbo` and `crabz2` are whole-codec
  libraries.
- For index/seek comparisons, `rapidgzip-core` is the strongest Rust side: it imports/exports
  RGZIDX01, GZIDX, htslib `.gzi`, and gztool indexes. `mim-index` and `indexed_deflate` are
  additional candidates, not drop-in replacements.
- Full-stream throughput does not predict random-access latency. For ratarmount, benchmark
  `Read + Seek` at representative offsets and read sizes separately; `rapidgzip-core` and
  `fbz` are the two crates worth measuring first. See Section 4.
- Keep the existing C/C++ baselines (gzip, pigz, bgzip, igzip, rapidgzip, indexed_bzip2,
  lbzip2, pbzip2) in the same session; do not append new-tool timings to a JSON measured in a
  different session.

## 9. Additional crates

These came from crates.io searches and were not fully audited. Treat the metadata as a lead,
not a verdict.

| Crate (version) | Role | Why it matters | Watch out |
|---|---|---|---|
| `gzip-ng` 2.1.0 | Parallel gzip CLI/library on zlib-ng | Credible zlib-ng maintainer; direct pigz-style comparator | Very new, 57 downloads, low adoption |
| `crabz` 0.10.1 | Parallel gzip/bzip2 compressor CLI | 50k downloads, established; compression-side baseline | Encoder, not decoder |
| `gzp` 2.0.4 | Parallel gzip compression library | 2.8M downloads; used by `deacon` | Compression only |
| `indexed_deflate` 0.1.0 | Gzip/zlib/DEFLATE random access | Direct index/seek comparison | One release, 2025 |
| `mim-index` 0.1.1 | FASTQ gzip index | From COMBINE-lab authors; real adoption | Niche; index format only |
| `sirocco` 0.3.1 | Pure-Rust DEFLATE/zlib/gzip | Strong scalar/sequential baseline; used by `deacon` | Not parallel |
| `linflate` 0.1.14 / `ldeflate` 0.1.3 | Pure-Rust DEFLATE decoder/encoder | Scalar/SIMD baselines from the znippy family | Same provenance caveats as `lgz`/`lbzip2` |
| `rebgzf` 0.2.0 | gzip-to-BGZF transcoder | Niche BGZF workflow | Not a general decoder |
| `pbz2` 0.2.0 | Parallel bzip2 CLI/library | Strong bzip2 benchmark candidate | Not audited here |
| `parallel-bz2-redux` 1.1.1 | Parallel bzip2 | Possible comparator | Four versions in two days; verify |
| `lzip-parallel` 0.2.12 / `ljar` 0.2.11 | Parallel ZIP/JAR DEFLATE | ZIP payload comparisons | Same znippy family; binary `lzip` collision |
| `ripzip` 0.1.0 | Multi-threaded ZIP CLI/library | Velopack project | One release, no update since 2026-02-27 |
| `zefiro` 1.0.0 | Pure-Rust zstd decoder, multithreaded | zstd comparison | One release; low adoption |
| `lz4r` 1.10.0 | LZ4 CLI port | LZ4 comparison | GPL-2.0-only; requires care when vendoring |
| `oxiarc-lz4` 0.4.2 | Pure-Rust LZ4 | LZ4 library | Not parallel |

Filtered out or low priority: `piz` (stale 2022), `medusa-zip` (stale 2023), `bgzip` (library,
last updated 2023), `sevenfold` (repository unreachable), `flate-zoo`
(GPL/LGPL-inclusive), `omnizip-libdeflate` (228 releases in seven weeks), `zenflate`, `noflate`,
`zflate`, `zstd-pure-rs`, `zstd-complete`, `structured-zstd` (not parallel decoders or not
benchmark CLIs in this sense), `zstd-framed` (2024), `geezipx` (company-owned; very low
adoption), `xcompress` (archiver, not codec), `kira-fastq` (application-level BGZF).

## 10. Limitations

- Static audit only. No crate was built or run. Findings such as the `rsomics-igzip` 4 MiB
  truncation and `rusty-rapidgzip` BGZF reservation are code-level findings, not reproduced
  exploits.
- The seekability section is an API-level audit. No ratarmount integration or random-access
  latency measurement was performed. `Read + Seek`, index, and block-cache claims come from
  the published APIs and source, not from running the crates.
- Dependency build scripts were not audited. `isal-sys` (used by `rsomics-igzip`) was not
  available locally; `libdeflate-sys`, `bzip2-sys`, `blake3`, `zstd-sys`, `liblzma-sys`, and
  rustls/aws-lc native builds are outside this pass.
- Repository comparisons are against local clones. `lgz`/`lbzip2` cannot be verified because
  the current forge is SSH-only; `rusty-rapidgzip` content was not diffed because only the
  repository-existence API was queried.
- Download counts and reverse dependencies are crates.io values on 2026-11-15 and can include
  CI, mirrors, and repeated installs. They are adoption signals, not user counts.
- OSV returned zero advisories, but absence of a public advisory is not proof of safety.
