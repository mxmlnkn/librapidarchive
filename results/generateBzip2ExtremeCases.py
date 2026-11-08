#!/usr/bin/env python3
"""Generate a deterministic corpus of bzip2 extreme cases.

The corpus targets the individual stages of the bzip2 decoder used by
``indexed_bzip2``: the initial run-length encoder (RLE1), the Burrows-Wheeler
transform (BWT), move-to-front (MTF), the zero-run encoder (RLE2, i.e. RUNA and
RUNB), the Huffman decoder, and the per-block/stream plumbing.  Each case is
written as a plain ``.bin`` file, compressed with the system ``bzip2``, and the
uncompressed intermediate is removed afterwards.

Only the Python standard library is used.  Alphabet reduction is performed with
``bytes.translate``.  All randomness comes from ``random.Random(seed)`` so that
a given seed produces byte-identical inputs on any platform with the same
Python major version (``Random.randbytes`` is used).

Examples
--------
    python3 results/generateBzip2ExtremeCases.py --output-dir tmp/bzip2-extremes/corpus
    python3 results/generateBzip2ExtremeCases.py --output-dir /data/corpus \
        --size-mib 32 --seed 1234 --cases zeros,dna,random-256

The output directory receives ``<case>.bin.bz2`` files plus ``manifest.json``.
"""

from __future__ import annotations

import argparse
import base64
import json
import random
import shutil
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


MIB = 1 << 20


# A small, fixed vocabulary so that the text fallback is self-contained and
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
    level: int = 9
    kind: str = "single"


CASES: list[Case] = [
    Case(
        "zeros",
        "constant 0x00; longest possible RLE1 and RLE2 runs",
        ["rle1", "rle2", "decodeBlock-fill", "crc"],
    ),
    Case(
        "constant-a",
        "constant 0x41; single-symbol alphabet, non-zero byte value",
        ["rle1", "rle2", "mtf-index-0", "crc"],
    ),
    Case(
        "skewed-pair",
        "two-symbol random, approximately 98.8% A and 1.2% B; long zero runs in MTF space",
        ["rle1", "rle2", "runa-runb", "huffman-runs"],
    ),
    Case(
        "two-symbol",
        "two-symbol random, 50/50 A/B",
        ["bwt", "mtf-small-index", "rle2", "huffman"],
    ),
    Case(
        "dna",
        "uniform random DNA alphabet ACGT",
        ["bwt", "mtf-small-index", "rle2", "huffman"],
    ),
    Case(
        "sixteen-symbol",
        "uniform random over 16 byte values; MTF indices mostly below 16",
        ["mtf-sse2", "bwt", "huffman"],
    ),
    Case(
        "random-256",
        "uniform random over all 256 byte values; compressed ratio close to one",
        ["mtf-scalar-tail", "bwt-libsais", "huffman-lut", "decodeBlock-scan"],
    ),
    Case(
        "base64",
        "deterministic base64 of random bytes; 64-symbol alphabet",
        ["mtf-sse2", "huffman", "bwt-libsais"],
    ),
    Case(
        "english",
        "reference English text (or a documented deterministic fallback)",
        ["bwt-libsais", "mtf", "rle2", "decodeBlock-runs"],
    ),
    Case(
        "mixed-zero-spikes",
        "uniform random bytes with deterministic long zero intervals inserted",
        ["rle1", "decodeBlock-runs", "crc", "bwt-libsais"],
    ),
    Case(
        "rle1-boundaries",
        "alternating run lengths around 3/4/255/259/260/516/517",
        ["rle1-boundaries", "decodeBlock-run-count", "crc"],
    ),
    Case(
        "rle2-boundaries",
        "single-byte separators between runs whose lengths straddle powers of two",
        ["rle2-boundaries", "runa-runb", "rle1"],
    ),
    Case(
        "small-blocks",
        "English text compressed with bzip2 -1 (100 kB blocks)",
        ["per-block-overhead", "trees", "libsais-setup", "parallel-scheduling"],
        level=1,
    ),
    Case(
        "many-streams",
        "many independently compressed bzip2 streams concatenated into one file",
        ["stream-header", "eos", "block-finder", "stream-crc", "parallel-scheduling"],
        kind="multi-stream",
    ),
    Case(
        "highratio",
        "realistic high-ratio archive if available, else repeated-text fallback",
        ["rle1", "rle2", "decodeBlock-fill", "crc"],
        kind="highratio",
    ),
]


def _translation_table(alphabet: bytes) -> bytes:
    """Map every byte value onto an alphabet with the smallest possible bias."""
    if len(alphabet) == 1:
        return bytes([alphabet[0]]) * 256
    return bytes(alphabet[index % len(alphabet)] for index in range(256))


def _translated_random(rng: random.Random, size: int, alphabet: bytes) -> bytes:
    return rng.randbytes(size).translate(_translation_table(alphabet))


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


def _run_boundary_pattern(lengths: Iterable[int], separators: bool = False) -> bytes:
    pattern = bytearray()
    symbols = list(lengths)
    index = 0
    while len(pattern) < 65536:
        length = symbols[index % len(symbols)]
        byte = 0x41 + (index % 25)
        pattern.extend(bytes([byte]) * length)
        if separators:
            pattern.append(0x5A)
        index += 1
    return bytes(pattern)


def _mixed_zero_spikes(rng: random.Random, size: int) -> bytes:
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


def _build_single(case: Case, size: int, rng: random.Random, text_path: Path | None) -> tuple[bytes, str]:
    name = case.name
    if name in {"zeros", "constant-a"}:
        return bytes([0x00 if name == "zeros" else 0x41]) * size, "constant byte"
    if name == "skewed-pair":
        return _translated_random(rng, size, b"A" * 253 + b"B" * 3), "~98.8% A, ~1.2% B"
    if name == "two-symbol":
        return _translated_random(rng, size, b"AB"), "50/50 A/B"
    if name == "dna":
        return _translated_random(rng, size, b"ACGT"), "uniform ACGT"
    if name == "sixteen-symbol":
        return _translated_random(rng, size, b"0123456789ABCDEF"), "uniform 16 symbols"
    if name == "random-256":
        return rng.randbytes(size), "uniform 256 symbols"
    if name == "base64":
        raw_size = ((size + 3) // 4) * 3
        return base64.b64encode(rng.randbytes(raw_size))[:size], "base64 of random bytes"
    if name in {"english", "small-blocks"}:
        return _read_text_source(text_path, size, rng)
    if name == "mixed-zero-spikes":
        return _mixed_zero_spikes(rng, size), "random bytes with zero spikes"
    if name == "rle1-boundaries":
        lengths = [1, 2, 3, 4, 5, 254, 255, 256, 258, 259, 260, 261, 514, 515, 516, 517, 518, 519]
        pattern = _run_boundary_pattern(lengths)
        return (pattern * (size // len(pattern) + 1))[:size], "run lengths at RLE1 boundaries"
    if name == "rle2-boundaries":
        lengths = [1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 257, 258, 259,
                   511, 512, 513, 516, 517]
        pattern = _run_boundary_pattern(lengths, separators=True)
        return (pattern * (size // len(pattern) + 1))[:size], "zero runs at RLE2 boundaries"
    raise ValueError(f"unknown single-file case: {name}")


def _compress(raw: bytes, destination: Path, level: int, bzip2: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output:
        subprocess.run([bzip2, f"-{level}", "-c"], input=raw, stdout=output, check=True)


def _compress_file(raw_path: Path, destination: Path, level: int, bzip2: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as output, raw_path.open("rb") as source:
        subprocess.run([bzip2, f"-{level}", "-c"], stdin=source, stdout=output, check=True)


def _generate_many_streams(size: int, rng: random.Random, text_path: Path | None,
                           destination: Path, stream_count: int, bzip2: str) -> tuple[int, str]:
    raw, source = _read_text_source(text_path, size, rng)
    chunk_size = max(1, size // stream_count)
    with tempfile.TemporaryDirectory(prefix="bzip2-extremes-streams-") as temp_dir:
        temp_root = Path(temp_dir)
        parts: list[Path] = []
        for index in range(stream_count):
            begin = index * chunk_size
            end = size if index == stream_count - 1 else min(size, begin + chunk_size)
            if begin >= end:
                break
            chunk_path = temp_root / f"chunk-{index:04d}.bin"
            chunk_path.write_bytes(raw[begin:end])
            part_path = temp_root / f"chunk-{index:04d}.bin.bz2"
            _compress_file(chunk_path, part_path, 9, bzip2)
            parts.append(part_path)
        with destination.open("wb") as output:
            for part_path in parts:
                with part_path.open("rb") as part:
                    shutil.copyfileobj(part, output)
    return size, f"{len(parts)} concatenated -9 streams from {source}"


def _generate_highratio(args: argparse.Namespace, size: int, rng: random.Random,
                        text_path: Path | None, destination: Path) -> tuple[int | None, str]:
    if args.highratio_source is not None and args.highratio_source.is_file():
        shutil.copyfile(args.highratio_source, destination)
        return None, f"copied {args.highratio_source}"
    raw, source = _read_text_source(text_path, size, rng)
    with tempfile.TemporaryDirectory(prefix="bzip2-extremes-highratio-") as temp_dir:
        raw_path = Path(temp_dir) / "highratio.bin"
        # Repeating the text several times yields a plausible very high ratio while
        # keeping the intermediate small enough for a fallback.
        repeated = (raw * 8)[: max(size, 8 * MIB)]
        raw_path.write_bytes(repeated)
        _compress_file(raw_path, destination, 9, args.bzip2)
        return len(repeated), f"repeated text fallback from {source}"


def _remove_raw(raw_path: Path) -> None:
    try:
        raw_path.unlink()
    except FileNotFoundError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="directory for .bin.bz2 files and manifest.json")
    parser.add_argument("--size-mib", type=int, default=64,
                        help="uncompressed size per case in MiB (default: 64)")
    parser.add_argument("--seed", type=int, default=0xB22E47,
                        help="deterministic random seed (default: 0xB22E47)")
    parser.add_argument("--cases", default="all",
                        help="comma-separated case names, or 'all'")
    parser.add_argument("--stream-count", type=int, default=64,
                        help="number of concatenated streams for many-streams")
    parser.add_argument("--text-source", type=Path, default=None,
                        help="English text source; defaults to tmp/ref-corpus/text.bin when found")
    parser.add_argument("--highratio-source", type=Path, default=None,
                        help="existing high-ratio .bz2; defaults to tmp/highratio-subset.bin.bz2")
    parser.add_argument("--skip-existing", action="store_true",
                        help="keep existing .bin.bz2 files instead of regenerating them")
    parser.add_argument("--keep-raw", action="store_true",
                        help="keep the uncompressed .bin intermediates (debugging only)")
    parser.add_argument("--bzip2", default="bzip2", help="bzip2 executable (default: bzip2)")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    main_checkout = repo_root.parents[1] if len(repo_root.parents) >= 2 else repo_root

    if args.text_source is None:
        for candidate in (repo_root / "tmp/ref-corpus/text.bin",
                          main_checkout / "tmp/ref-corpus/text.bin"):
            if candidate.is_file():
                args.text_source = candidate
                break
    if args.highratio_source is None:
        for candidate in (repo_root / "tmp/highratio-subset.bin.bz2",
                          main_checkout / "tmp/highratio-subset.bin.bz2"):
            if candidate.is_file():
                args.highratio_source = candidate
                break

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
        "cases": [],
    }

    # Seed each case independently so that generating a subset produces the same
    # bytes as generating the full corpus.
    for case in selected:
        rng = random.Random(args.seed ^ zlib.crc32(case.name.encode()))
        destination = args.output_dir / f"{case.name}.bin.bz2"
        raw_path = args.output_dir / f"{case.name}.bin"
        if args.skip_existing and destination.is_file():
            print(f"[skip] {case.name}: {destination} exists", file=sys.stderr)
            manifest["cases"].append({
                "name": case.name,
                "description": case.description,
                "stresses": case.stresses,
                "level": case.level,
                "compressed_size": destination.stat().st_size,
                "raw_size": None,
                "note": "skipped existing file",
            })
            continue

        print(f"[gen] {case.name}: {case.description}", file=sys.stderr)
        if case.kind == "single":
            raw, detail = _build_single(case, size, rng, args.text_source)
            _compress(raw, destination, case.level, args.bzip2)
            raw_size = len(raw)
            if args.keep_raw:
                raw_path.write_bytes(raw)
            del raw
        elif case.kind == "multi-stream":
            raw_size, detail = _generate_many_streams(size, rng, args.text_source, destination,
                                                      args.stream_count, args.bzip2)
        elif case.kind == "highratio":
            raw_size, detail = _generate_highratio(args, size, rng, args.text_source, destination)
        else:
            raise ValueError(f"unknown case kind: {case.kind}")

        _remove_raw(raw_path)
        manifest["cases"].append({
            "name": case.name,
            "description": case.description,
            "stresses": case.stresses,
            "level": case.level,
            "detail": detail,
            "raw_size": raw_size,
            "compressed_size": destination.stat().st_size,
            "file": destination.name,
        })
        if raw_size is not None:
            ratio = raw_size / destination.stat().st_size
            print(f"[ ok] {case.name}: {raw_size} B -> {destination.stat().st_size} B (ratio {ratio:.2f})",
                  file=sys.stderr)
        else:
            print(f"[ ok] {case.name}: {destination.stat().st_size} B compressed (raw size pending verification)",
                  file=sys.stderr)

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"[done] manifest: {manifest_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
