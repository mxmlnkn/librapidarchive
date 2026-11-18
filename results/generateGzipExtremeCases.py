#!/usr/bin/env python3
"""Generate a deterministic corpus of gzip extreme cases.

The corpus targets the stages of the custom inflate used by ``rapidgzip``:
block-type handling (fixed, dynamic, and stored), literal decoding, the
length/distance pair and backreference copy paths (short, long, overlapping),
window handling, and per-block, per-member, and per-stream overhead.

Every case is written as a ``.gz`` file.  Compression uses only the Python
standard library ``zlib`` module, so no external compressor version influences
the result.  All randomness comes from ``random.Random(seed)`` and
``Random.randbytes`` so that a given seed produces byte-identical inputs on
any platform with the same Python major version.

Examples
--------
    python3 results/generateGzipExtremeCases.py --output-dir tmp/gzip-extremes/corpus
    python3 results/generateGzipExtremeCases.py --output-dir /data/corpus \
        --size-mib 32 --seed 1234 --cases zeros,base64,fixed-huffman

The output directory receives ``<case>.gz`` files plus ``manifest.json``.
"""

from __future__ import annotations

import argparse
import base64
import json
import random
import zlib
from dataclasses import dataclass
from pathlib import Path


MIB = 1 << 20
GZIP_WBITS = 31


# A small, fixed vocabulary so that the text cases are self-contained and
# deterministic.  It is only used when no reference text corpus is available.
FALLBACK_WORDS = """
the quick brown fox jumps over lazy dog and then runs into a quiet forest
where many small birds sing while the sun rises above green hills and slow
rivers carry cold water through old stone bridges into distant valleys
system data stream block transform table buffer symbol decoder reader
compress archive index parallel thread worker memory cache latency branch
""".split()


@dataclass
class Case:
    name: str
    description: str
    stresses: list[str]
    level: int = 6
    strategy: int = zlib.Z_DEFAULT_STRATEGY
    kind: str = "single"


CASES: list[Case] = [
    Case(
        "fixed-huffman",
        "text-like data compressed with the fixed Huffman block type",
        ["fixed-huffman-block", "literal-lut"],
        strategy=zlib.Z_FIXED,
    ),
    Case(
        "dynamic-huffman",
        "strongly skewed 256-symbol alphabet, i.e. a dynamic tree with many code lengths",
        ["dynamic-huffman-block", "precode", "literal-lut"],
    ),
    Case(
        "stored-blocks",
        "incompressible random bytes at level 0, i.e. stored blocks",
        ["stored-block-copy", "crc"],
        level=0,
    ),
    Case(
        "short-matches",
        "short dictionary copies of 4-32 bytes mixed with literals",
        ["resolveBackreference-short", "length-distance-decode"],
    ),
    Case(
        "long-matches",
        "repeated 4 KiB blocks, i.e. long non-overlapping matches",
        ["resolveBackreference-memcpy", "length-distance-decode"],
        level=9,
    ),
    Case(
        "overlapping-backrefs",
        "periods of 2-31 bytes repeated, i.e. length > distance",
        ["resolveBackreference-overlap", "length-distance-decode"],
        level=9,
    ),
    Case(
        "base64",
        "deterministic base64 of random bytes, a literal-dominated mixed tree",
        ["literal-simd", "mixed-code-lengths", "crc"],
    ),
    Case(
        "english",
        "English-like text with a realistic literal/match mix",
        ["literal-lut", "resolveBackreference-short", "crc"],
    ),
    Case(
        "zeros",
        "constant 0x00, i.e. long distance-1 runs",
        ["resolveBackreference-fill", "crc"],
        level=9,
    ),
    Case(
        "random-256",
        "uniform random over all 256 byte values at level 0",
        ["stored-block-copy", "crc"],
        level=0,
    ),
    Case(
        "mixed-window",
        "random bytes with deterministic zero intervals",
        ["resolveBackreference-fill", "literal-lut", "window"],
    ),
    Case(
        "small-blocks",
        "dynamic Huffman blocks flushed every 32 KiB",
        ["per-block-header", "precode", "literal-table-construction"],
        kind="small-blocks",
    ),
    Case(
        "concatenated",
        "sixteen independently compressed gzip members",
        ["member-header", "stream-plumbing"],
        kind="multi-stream",
    ),
    Case(
        "high-ratio",
        "repeated structured text, i.e. many long matches at a high ratio",
        ["resolveBackreference-memcpy", "length-distance-decode"],
        level=9,
        kind="highratio",
    ),
]


def _fallback_text(rng: random.Random, size: int) -> bytes:
    """Build a deterministic English-like text by repeating a seeded template."""
    if size <= 0:
        return b""

    target = min(size, MIB)
    chunks: list[str] = []
    total = 0
    while total < target:
        word_count = rng.randint(8, 24)
        line = " ".join(rng.choice(FALLBACK_WORDS) for _ in range(word_count))
        line = line.capitalize() + ".\n"
        chunks.append(line)
        total += len(line)
    template = "".join(chunks).encode("ascii")
    return (template * (size // len(template) + 1))[:size]


def _read_text_source(path: Path | None, size: int, rng: random.Random) -> tuple[bytes, str]:
    if path is not None and path.is_file():
        with path.open("rb") as file:
            data = file.read(size)
        if len(data) < size:
            data += _fallback_text(rng, size - len(data))
        return data, str(path)
    return _fallback_text(rng, size), "deterministic-fallback"


def _mixed_zero_intervals(rng: random.Random, size: int) -> bytes:
    data = bytearray(rng.randbytes(size))
    position = 0
    spike_lengths = [64, 256, 1024, 4096, 16384, 65536]
    while position < size:
        position += rng.randrange(4096, 262144)
        length = rng.choice(spike_lengths)
        end = min(size, position + length)
        if end > position:
            data[position:end] = bytes(end - position)
        position = end
    return bytes(data)


def _short_match_data(rng: random.Random, size: int) -> bytes:
    """Mix literals with 4-32 byte copies from the previous 4 KiB."""
    data = bytearray(rng.randbytes(4096))
    while len(data) < size:
        if rng.random() < 0.5:
            data.extend(rng.randbytes(rng.randint(4, 16)))
        else:
            distance = rng.randint(32, min(4096, len(data)))
            length = rng.randint(4, 32)
            begin = len(data) - distance
            for index in range(length):
                data.append(data[begin + index])
    return bytes(data[:size])


def _long_match_data(rng: random.Random, size: int) -> bytes:
    """Repeat 4 KiB blocks so that matches reach the 258-byte length limit."""
    data = bytearray()
    while len(data) < size:
        block = rng.randbytes(4096)
        data.extend(block)
        for _ in range(rng.randint(2, 8)):
            data.extend(block)
            if len(data) >= size:
                break
    return bytes(data[:size])


def _overlapping_data(rng: random.Random, size: int) -> bytes:
    """Repeat short periods with runs far longer than the period."""
    data = bytearray()
    while len(data) < size:
        period = rng.randbytes(rng.randint(2, 31))
        repeats = rng.randint(16, 512)
        data.extend(period * repeats)
    return bytes(data[:size])


def _skewed_symbols(rng: random.Random, size: int) -> bytes:
    """Map a cubic distribution onto all 256 byte values to force a wide tree."""
    return bytes(int(256 * rng.random() ** 3) for _ in range(size))


def _build_single(case: Case, size: int, rng: random.Random,
                  text_path: Path | None) -> tuple[bytes, str]:
    name = case.name
    if name == "dynamic-huffman":
        return _skewed_symbols(rng, size), "skewed 256-symbol alphabet"
    if name == "english":
        return _read_text_source(text_path, size, rng)
    if name == "fixed-huffman":
        text, source = _read_text_source(text_path, size, rng)
        return text, source
    if name in {"stored-blocks", "random-256"}:
        return rng.randbytes(size), "uniform 256 symbols"
    if name == "zeros":
        return bytes(size), "constant zero byte"
    if name == "base64":
        raw_size = ((size + 3) // 4) * 3
        return base64.b64encode(rng.randbytes(raw_size))[:size], "base64 of random bytes"
    if name == "short-matches":
        return _short_match_data(rng, size), "literals plus 4-32 byte dictionary copies"
    if name == "long-matches":
        return _long_match_data(rng, size), "repeated 4 KiB blocks"
    if name == "overlapping-backrefs":
        return _overlapping_data(rng, size), "periods of 2-31 bytes"
    if name == "mixed-window":
        return _mixed_zero_intervals(rng, size), "random bytes with zero intervals"
    raise ValueError(f"unknown single-file case: {name}")


def _compress(raw: bytes, destination: Path, level: int, strategy: int,
              flush_interval: int | None = None) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    compressor = zlib.compressobj(level, zlib.DEFLATED, GZIP_WBITS, 8, strategy)
    parts: list[bytes] = []
    if flush_interval is None:
        parts.append(compressor.compress(raw))
    else:
        for begin in range(0, len(raw), flush_interval):
            parts.append(compressor.compress(raw[begin:begin + flush_interval]))
            parts.append(compressor.flush(zlib.Z_FULL_FLUSH))
    parts.append(compressor.flush(zlib.Z_FINISH))
    destination.write_bytes(b"".join(parts))


def _generate_multi_stream(size: int, rng: random.Random, text_path: Path | None,
                           destination: Path, stream_count: int) -> tuple[int, str]:
    raw, source = _read_text_source(text_path, size, rng)
    chunk_size = max(1, size // stream_count)
    members: list[bytes] = []
    for index in range(stream_count):
        begin = index * chunk_size
        end = size if index == stream_count - 1 else min(size, begin + chunk_size)
        if begin >= end:
            break
        compressor = zlib.compressobj(9, zlib.DEFLATED, GZIP_WBITS)
        members.append(compressor.compress(raw[begin:end]))
        members.append(compressor.flush(zlib.Z_FINISH))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(b"".join(members))
    return size, f"{len(members) // 2} concatenated -9 members from {source}"


def _generate_highratio(size: int, rng: random.Random, text_path: Path | None,
                        destination: Path) -> tuple[int, str]:
    text, source = _read_text_source(text_path, 8 * 1024, rng)
    repeated = (text * (size // max(1, len(text)) + 1))[:size]
    _compress(repeated, destination, 9, zlib.Z_DEFAULT_STRATEGY)
    return len(repeated), f"repeated text fallback from {source}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="directory for .gz files and manifest.json")
    parser.add_argument("--size-mib", type=int, default=64,
                        help="uncompressed size per case in MiB (default: 64)")
    parser.add_argument("--seed", type=int, default=0x6717,
                        help="deterministic random seed (default: 0x6717)")
    parser.add_argument("--cases", default="all",
                        help="comma-separated case names, or 'all'")
    parser.add_argument("--stream-count", type=int, default=16,
                        help="number of concatenated members for concatenated")
    parser.add_argument("--flush-interval-kib", type=int, default=32,
                        help="flush interval for small-blocks in KiB (default: 32)")
    parser.add_argument("--text-source", type=Path, default=None,
                        help="English text source; falls back to the built-in word text")
    parser.add_argument("--skip-existing", action="store_true",
                        help="keep existing .gz files instead of regenerating them")
    args = parser.parse_args()

    if args.text_source is not None and not args.text_source.is_file():
        parser.error(f"text source does not exist: {args.text_source}")

    selected = CASES if args.cases == "all" else [case for case in CASES if case.name in args.cases.split(",")]
    unknown = set(args.cases.split(",")) - {case.name for case in CASES} if args.cases != "all" else set()
    if unknown:
        parser.error(f"unknown cases: {', '.join(sorted(unknown))}")

    size = args.size_mib * MIB
    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output_dir / "manifest.json"
    manifest = {
        "seed": args.seed,
        "size_bytes": size,
        "python": ".".join(str(part) for part in __import__("sys").version_info[:3]),
        "zlib": zlib.ZLIB_RUNTIME_VERSION,
        "cases": [],
    }

    # Seed each case independently so that generating a subset produces the same
    # bytes as generating the full corpus.
    for case in selected:
        rng = random.Random(args.seed ^ zlib.crc32(case.name.encode()))
        destination = args.output_dir / f"{case.name}.gz"
        if args.skip_existing and destination.is_file():
            print(f"[skip] {case.name}: {destination} exists")
            manifest["cases"].append({
                "name": case.name,
                "description": case.description,
                "stresses": case.stresses,
                "level": case.level,
                "strategy": case.strategy,
                "compressed_size": destination.stat().st_size,
                "raw_size": None,
                "note": "skipped existing file",
            })
            continue

        print(f"[gen] {case.name}: {case.description}")
        if case.kind == "single":
            raw, detail = _build_single(case, size, rng, args.text_source)
            _compress(raw, destination, case.level, case.strategy)
            raw_size: int | None = len(raw)
            del raw
        elif case.kind == "small-blocks":
            raw, detail = _read_text_source(args.text_source, size, rng)
            _compress(raw, destination, case.level, case.strategy,
                      flush_interval=args.flush_interval_kib * 1024)
            raw_size = len(raw)
            del raw
        elif case.kind == "multi-stream":
            raw_size, detail = _generate_multi_stream(size, rng, args.text_source, destination,
                                                      args.stream_count)
        elif case.kind == "highratio":
            raw_size, detail = _generate_highratio(size, rng, args.text_source, destination)
        else:
            raise ValueError(f"unknown case kind: {case.kind}")

        manifest["cases"].append({
            "name": case.name,
            "description": case.description,
            "stresses": case.stresses,
            "level": case.level,
            "strategy": case.strategy,
            "detail": detail,
            "raw_size": raw_size,
            "compressed_size": destination.stat().st_size,
            "file": destination.name,
        })
        ratio = (raw_size / destination.stat().st_size) if raw_size else 0.0
        print(f"[ ok] {case.name}: {raw_size} B -> {destination.stat().st_size} B (ratio {ratio:.2f})")

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"[done] manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
