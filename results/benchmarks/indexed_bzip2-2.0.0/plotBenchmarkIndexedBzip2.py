#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Plot JSON files written by benchmarkIndexedBzip2.py.

Examples:
    python3 results/benchmarks/indexed_bzip2-2.0.0/plotBenchmarkIndexedBzip2.py run.json
    python3 results/benchmarks/indexed_bzip2-2.0.0/plotBenchmarkIndexedBzip2.py run-1.7.0.json run-2.0.0.json \
        --output-prefix indexed_bzip2-1.7.0-vs-2.0.0
    python3 results/benchmarks/indexed_bzip2-2.0.0/plotBenchmarkIndexedBzip2.py \
        --log-log --speedup-baseline ibzip2 run.json
    python3 results/benchmarks/indexed_bzip2-2.0.0/plotBenchmarkIndexedBzip2.py \
        --directory results/benchmarks/indexed_bzip2-2.0.0
    python3 results/benchmarks/indexed_bzip2-2.0.0/plotBenchmarkIndexedBzip2.py runs.zip

A .zip input is expanded and all JSON members are plotted together. For a single input the plot is
written next to it with the same basename and .png/.pdf extensions. Multiple inputs are merged and use the
common prefix and suffix of their basenames; the differing middle, usually the timestamps, is dropped.
--output-prefix overrides the output path.
--directory plots every benchmark JSON below a directory and groups repeated runs of the same input
and machine automatically. The suptitle is built from the JSON metadata (dataset, CPU, date, size, and
compression ratio) and only falls back to the filename when the metadata is missing. Multiple JSON
files are merged by tool name and version. Results with the same tool, version, and thread count have
their raw iteration values combined. All inputs must describe the same compressed file and machine;
plot different datasets or CPUs separately.
The left panel compares single-core bandwidth with zero-based bars and violin overlays; higher is better.
The right panel shows the thread scaling with violins, median lines, and a speedup axis relative to GNU
bzip2 by default. Violins omit runs beyond ten median absolute deviations from the median, which are
usually process-launch stalls on small inputs; the JSON keeps every runtime.
"""

import argparse
import contextlib
import json
import os
import re
import shutil
import statistics
import tempfile
import zipfile
from pathlib import Path

import matplotlib
import matplotlib.patheffects as path_effects
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter, LogLocator, NullFormatter

# Avoid Type 3 fonts as they are not supported by some publishers.
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42

# igzip decodes the gzip data far faster than the bzip2 tools and compresses the shared bandwidth axis.
# The gzip variant of rapidgzip is excluded until the gzip and bzip2 comparisons are plotted separately.
#PLOT_EXCLUDED_TOOLS = {"igzip", "rapidgzip-gzip"}
PLOT_EXCLUDED_TOOLS = {""}

FIGURE_WIDTH = 18.0
FIGURE_HEIGHT = 6.4
AXES_RECT_WIDTH = 0.98

# Saturated tab10 colors, matching the palette used by the ratarmount bandwidth plots. Red is the slow
# baseline to improve upon, green is the ibzip2 command line tool, and violet is the Python library.
COLORS = {
    "CPython.bz2": "#1f77b4",   # tab10 blue
    "GNU bzip2": "#d62728",     # tab10 red, slowest reference
    "GNU gzip": "#ff9896",      # tab10 light red, gzip reference on a separate file
    "igzip": "#ffbb78",         # tab10 light orange, ISA-L gzip decoder
    "libarchive": "#bcbd22",    # tab10 olive, libarchive bsdcat
    "bzip2-rs-cli": "#17becf",  # tab10 cyan, pure-Rust libbzip2-rs CLI
    "seek-bzip2": "#c7c7c7",    # tab20 light gray, seek-bzip2/micro-bunzip single-core reference
    "pbzip2": "#7f7f7f",        # tab10 gray, single- and two-thread reference
    "pbz2": "#aec7e8",          # tab20 light blue, parallel Rust implementation
    "lbzip2-rs": "#98df8a",     # tab20 light green, pure-Rust lbzip2
    "fbz": "#f7b6d2",           # tab20 light pink, multi-format Rust CLI
    "mibz2": "#c49c94",         # tab20 light brown, Rust streaming decoder
    "crabz2": "#dbdb8d",        # tab20 light olive, Rust whole-buffer decoder
    "7z": "#e377c2",            # tab10 pink, single-core reference
    "lbzip2": "#ffbb78",        # tab20 light orange, competing tool (dotted)
    "ibzip2": "#2ca02c",        # tab10 green, our command line tool
    "rapidgzip": "#ff7f0e",     # tab10 orange, bzip2 chunk decoder (solid)
    "rapidgzip-gzip": "#9edae5", # light cyan, gzip variant (excluded by default)
    "ibzip2-1.7.0": "#c5b0d5",  # older version, lighter violet
    "indexed_bzip2": "#9467bd", # tab10 violet, our Python library
}
LANGUAGES = {
    "CPython.bz2": "C",
    "GNU bzip2": "C",
    "GNU gzip": "C",
    "igzip": "C",
    "libarchive": "C",
    "bzip2-rs-cli": "Rust",
    "seek-bzip2": "C",
    "pbzip2": "C++",
    "pbz2": "Rust",
    "lbzip2-rs": "Rust",
    "fbz": "Rust",
    "mibz2": "Rust",
    "crabz2": "Rust",
    "7z": "C++",
    "lbzip2": "C",
    "ibzip2": "C++",
    "rapidgzip": "C++, bzip2",
    "rapidgzip-gzip": "C++, gzip",
    "ibzip2-1.7.0": "C++",
    "indexed_bzip2": "C++ via Python 3",
}
# Older JSONs were written before the build script recorded a source revision next to the
# executable, so lbzip2 only reports "devel" there. New runs read lbzip2.version and store the
# revision in the JSON. Keep this mapping only for those legacy files.
LEGACY_SOURCE_REVISIONS = {"lbzip2": "724352c"}
TEXT_OUTLINE = [path_effects.withStroke(linewidth=3.0, foreground="white")]
SCALING_TOOLS = ["mibz2", "lbzip2-rs", "fbz", "pbz2", "crabz2", "lbzip2", "ibzip2-1.7.0",
                 "ibzip2", "rapidgzip", "rapidgzip-gzip", "indexed_bzip2"]
SCALING_ORDER = ["indexed_bzip2", "ibzip2", "rapidgzip", "rapidgzip-gzip", "mibz2", "lbzip2-rs",
                 "fbz", "pbz2", "lbzip2", "crabz2", "ibzip2-1.7.0"]
REFERENCE_STYLES = {
    "GNU bzip2": {"color": COLORS["GNU bzip2"], "linestyle": ":", "alpha": 0.9},
    "GNU gzip": {"color": COLORS["GNU gzip"], "linestyle": ":", "alpha": 0.9},
    "igzip": {"color": COLORS["igzip"], "linestyle": ":", "alpha": 0.9},
    "lbzip2": {"color": COLORS["lbzip2"], "linestyle": ":", "alpha": 0.9},
    "7z": {"color": COLORS["7z"], "linestyle": ":", "alpha": 0.9},
}


def shortVersion(version):
    if not version:
        return ""
    match = re.search(r"\d+(?:\.\d+)+", version)
    if match:
        return match.group(0)
    # Development builds can report only a name, for example "lbzip2 version devel".
    match = re.search(r"\bversion\b\s*(.+)$", version, flags=re.IGNORECASE)
    if match:
        return match.group(1).strip()
    return version.strip()


def versionKey(version):
    return tuple(int(number) for number in re.findall(r"\d+", version or ""))


def baseTool(tool):
    return tool[:-len("-native")] if tool.endswith("-native") else tool


def resolvedVersion(tool, version):
    base = baseTool(tool)
    if version == "devel":
        return LEGACY_SOURCE_REVISIONS.get(base, version)
    if version and version.lower().startswith(base.lower()):
        # Some tools report "<tool> <revision>" and the plot already prefixes the tool name.
        version = version[len(base):].strip(" :-")
    return version


def displayName(tool):
    base = baseTool(tool)
    if base == "ibzip2-1.7.0":
        return "ibzip2"
    if base == "GNU gzip":
        return "GNU gzip*"
    return base


def toolDetails(tool):
    base = baseTool(tool)
    details = []
    language = LANGUAGES.get(base)
    if language:
        details.append(language)
    if tool.endswith("-native"):
        details.append("native")
    return ", ".join(details)


def seriesLabel(key):
    tool, version = key
    version = resolvedVersion(tool, version)
    label = f"{displayName(tool)} {version}".strip()
    details = toolDetails(tool)
    if not details:
        return label
    detailLabel = f"({details})"
    # Keep the legend compact by breaking before long language and compression combinations.
    return f"{label} {detailLabel}" if len(detailLabel) <= 6 else f"{label}\n{detailLabel}"


def axisLabel(key):
    tool, version = key
    version = resolvedVersion(tool, version)
    label = f"{displayName(tool)} {version}".strip()
    details = toolDetails(tool)
    if not details:
        return label
    detailLabel = f"({details})"
    # Short tags such as (C), (C++), and (Rust) stay on the same line. Long combinations such as
    # (C++, bzip2) or (C++ via Python 3) go to a second line so that the labels stay readable.
    return f"{label} {detailLabel}" if len(detailLabel) <= 6 else f"{label}\n{detailLabel}"


def speedupLabel(value, _):
    if value <= 0:
        return ""
    if value < 1:
        reciprocal = 1.0 / value
        if abs(reciprocal - round(reciprocal)) < 1e-6:
            return f"1/{round(reciprocal)}"
        return f"{value:.2g}x"
    return f"{value:g}x"


def loadSeries(paths, excludedTools=None):
    """Merge all input JSONs by (tool, short version, thread count)."""
    if excludedTools is None:
        excludedTools = PLOT_EXCLUDED_TOOLS
    merged = {}
    for path in paths:
        with open(path) as file:
            data = json.load(file)
        size = data["uncompressed_size"]
        versions = data.get("versions", {})
        for result in data["results"]:
            if "tool" not in result:
                continue
            tool = result["tool"]
            if baseTool(tool) in excludedTools:
                continue
            version = shortVersion(versions.get(tool, ""))
            if not version and tool.startswith("ibzip2-"):
                version = shortVersion(tool[len("ibzip2-"):])
            key = (tool, version)
            threads = result["threads"]
            # The old 1.7.0 JSON has only one summary runtime; new JSONs also have raw iterations.
            times = result.get("runtimes") or [result["runtime"]]
            bandwidths = [size / runtime / 1e6 for runtime in times]
            merged.setdefault((key, threads), []).extend(bandwidths)

    series = {}
    for (key, threads), values in merged.items():
        series.setdefault(key, {})[threads] = {
            "values": values,
            "plot_values": plotValues(values),
            "median": statistics.median(values),
        }
    return series


def plotValues(values):
    """Drop process-launch stalls before drawing a distribution.

    The JSON keeps every runtime. A single multi-second stall while launching a 0.1 s subprocess
    would otherwise stretch a violin over two orders of magnitude and hide the real distribution.
    """
    if len(values) < 3:
        return values
    median = statistics.median(values)
    deviation = statistics.median(abs(value - median) for value in values)
    if deviation <= 0:
        return values
    limit = median + 10 * deviation
    filtered = [value for value in values if value <= limit]
    return filtered if filtered else values


def chooseBaseline(series, choice):
    singleCore = {
        key: threadData[1]
        for key, threadData in series.items()
        if 1 in threadData
    }
    if choice == "gnu-bzip2":
        for key in sorted(singleCore, key=lambda key: (baseTool(key[0]) != "GNU bzip2", key)):
            if baseTool(key[0]) == "GNU bzip2":
                return singleCore[key]["median"], seriesLabel(key)
    if choice == "cpython-bz2":
        for key in sorted(singleCore, key=lambda key: (baseTool(key[0]) != "CPython.bz2", key)):
            if baseTool(key[0]) == "CPython.bz2":
                return singleCore[key]["median"], seriesLabel(key)
    if choice == "ibzip2":
        keys = sorted((key for key in singleCore if baseTool(key[0]) in ("ibzip2", "ibzip2-1.7.0")),
                      key=lambda key: (baseTool(key[0]) != "ibzip2", key[1]))
        if keys:
            return singleCore[keys[0]]["median"], seriesLabel(keys[0])
    if singleCore:
        best = max(singleCore, key=lambda key: singleCore[key]["median"])
        return singleCore[best]["median"], f"best ({seriesLabel(best)})"

    # Partial runs may not have reached thread 1 yet. Use the lowest available thread count so
    # that the scaling panel remains useful instead of failing on an empty baseline.
    lowest = {}
    for key, threadData in series.items():
        if threadData:
            thread = min(threadData)
            lowest[key] = threadData[thread]
    if lowest:
        best = max(lowest, key=lambda key: lowest[key]["median"])
        return lowest[best]["median"], f"lowest available ({seriesLabel(best)})"
    return None, "unavailable"


DATASET_LABELS = {
    "silesia": "Silesia",
    "silesia-20x": "Silesia 20x",
    "CTU-13-Dataset": "CTU-13",
    "CTU-13-dataset": "CTU-13",
}


def datasetName(metadata, jsonPath, override):
    if override:
        return override
    source = metadata.get("file")
    if source:
        name = Path(str(source)).name
        for suffix in (".tar.bz2", ".tbz2", ".bz2", ".tar"):
            if name.lower().endswith(suffix):
                name = name[:-len(suffix)]
                break
        if name:
            return DATASET_LABELS.get(name, name)
    key = Path(jsonPath).stem.replace("indexed_bzip2-benchmark-", "")
    for token in ("silesia-20x", "silesia", "CTU-13-Dataset", "CTU-13-dataset"):
        if token in key:
            key = token
            break
    return DATASET_LABELS.get(key, key)


def metadataDate(metadata, jsonPath):
    for field in ("date", "timestamp", "datetime"):
        value = metadata.get(field)
        if value:
            match = re.search(r"\d{4}-\d{2}-\d{2}", str(value))
            if match:
                return match.group(0)
    match = re.search(r"(\d{4}-\d{2}-\d{2})", Path(jsonPath).name)
    return match.group(1) if match else None


def metadataCpu(metadata, override):
    if override:
        return override
    hardware = metadata.get("hardware") or {}
    cpu = hardware.get("cpu") or {}
    model = cpu.get("model") or metadata.get("cpu_model") or metadata.get("machine")
    return re.sub(r"\s+", " ", str(model)).strip() if model else None


def formatSize(size):
    if not size:
        return None
    return f"{size / 1e9:.1f} GB" if size >= 1e9 else f"{size / 1e6:.1f} MB"


def addSpeedupAxis(axis, baseline, axisLabelText, logarithmic, dataMinimum, dataMaximum,
                   includeFractions=True):
    secondary = axis.secondary_yaxis(
        "right",
        functions=(lambda value: value / baseline,
                   lambda value: value * baseline))
    secondary.set_ylabel(axisLabelText)
    if logarithmic:
        secondary.set_yscale("log", base=2)

    tickMinimum = dataMinimum / baseline / 2
    if not logarithmic and not includeFractions:
        tickMinimum = max(tickMinimum, 1.0)

    if logarithmic:
        tickMaximum = dataMaximum / baseline * 2
        ticks = [2.0 ** exponent for exponent in range(-8, 9)
                 if tickMinimum <= 2.0 ** exponent <= tickMaximum]
    else:
        # Powers of two are too sparse above 8x. Add the integer speedups used in the reports and
        # extend the axis to the largest tick within 15% of the data so the labels are not clipped.
        candidates = sorted({2.0 ** exponent for exponent in range(-8, 9)}
                            | {3.0, 4.0, 6.0, 8.0, 12.0, 20.0, 24.0, 28.0})
        tickMaximum = dataMaximum / baseline * 1.15
        ticks = [tick for tick in candidates if tickMinimum <= tick <= tickMaximum]
    if ticks:
        secondary.yaxis.set_major_locator(FixedLocator(ticks))
    secondary.yaxis.set_major_formatter(FuncFormatter(speedupLabel))


def defaultOutputPrefix(paths):
    """Return the full input path without .json, or the common prefix and suffix for multiple runs."""
    stems = [str(Path(path).with_suffix("")) for path in paths]
    if len(stems) == 1:
        return stems[0]

    directory = Path(stems[0]).parent
    names = [Path(stem).name for stem in stems]

    # Reference/project splits and repeated runs differ in the tool-group suffix and in the timestamp.
    # Reduce both to the stable part of the name so that the merged plot keeps a stable path.
    timestampPattern = re.compile(
        r"^(?P<prefix>.*?)-(?P<timestamp>\d{4}-\d{2}-\d{2}T\d{2}-\d{2})-(?P<rest>.*)$")
    normalized = []
    months = []
    for name in names:
        for partSuffix in ("-reference", "-project", ".reference", ".project"):
            if name.endswith(partSuffix):
                name = name[:-len(partSuffix)]
                break
        match = timestampPattern.match(name)
        if match:
            normalized.append((match.group("prefix"), match.group("rest")))
            months.append(match.group("timestamp")[:7])
        else:
            normalized.append((None, name))
            months.append(None)

    if normalized and all(prefix is not None for prefix, _ in normalized):
        if len({(prefix, rest) for prefix, rest in normalized}) == 1:
            prefix, rest = normalized[0]
            commonMonths = sorted(set(months))
            month = commonMonths[0] if len(commonMonths) == 1 else ""
            merged = f"{prefix}-{month}-{rest}" if month else f"{prefix}-{rest}"
            return str(directory / merged)

    prefix = os.path.commonprefix(names)
    suffix = os.path.commonprefix([name[::-1] for name in names])[::-1]

    # Drop the differing middle, usually a timestamp, at delimiter boundaries so that the merged
    # basename does not end with half a date component.
    prefix = prefix[:prefix.rfind("-")] if "-" in prefix else ""
    suffix = suffix[suffix.find("-"):] if "-" in suffix else ""
    if suffix and not suffix.startswith("-"):
        suffix = "-" + suffix
    merged = prefix + suffix
    if not merged or merged.startswith("-"):
        merged = names[0] + "-merged"
    return str(directory / merged)


def parseCpuList(value):
    cpus = set()
    for part in re.split(r"[,\s]+", str(value or "")):
        if not part:
            continue
        if "-" in part:
            start, end = part.split("-", 1)
            if start.isdigit() and end.isdigit():
                cpus.update(range(int(start), int(end) + 1))
        elif part.isdigit():
            cpus.add(int(part))
    return cpus


def normalizeCpu(cpu):
    """Ignore the integrated GPU suffix that newer benchmark runs add to the CPU model."""
    return re.sub(r"\s+w/.*$", "", cpu, flags=re.IGNORECASE).strip()


def discoverJsonGroups(directory):
    """Group benchmark JSONs that describe the same input on the same machine."""
    groups = {}
    for path in sorted(Path(directory).rglob("*.json")):
        try:
            with open(path) as file:
                data = json.load(file)
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(data, dict) or "uncompressed_size" not in data:
            continue
        results = data.get("results")
        if not isinstance(results, list) or not results:
            continue
        if not all(isinstance(result, dict) and "tool" in result for result in results):
            continue
        hardware = data.get("hardware") or {}
        cpu = (hardware.get("cpu") or {}).get("model") or data.get("cpu_model") or data.get("machine") or ""
        # New runs include the integrated GPU in the CPU model while older runs do not.
        cpu = normalizeCpu(cpu)
        host = hardware.get("hostname") or data.get("hostname") or ""
        # Staged runs use a per-job temporary path. Group by the original source path instead,
        # and keep different nodes separate even when they have the same CPU model.
        source = data.get("source_file") or data.get("file") or ""
        key = (str(source), data.get("uncompressed_size"),
               data.get("compressed_size"), cpu, str(host))
        groups.setdefault(key, []).append(str(path))
    return [groups[key] for key in
            sorted(groups, key=lambda key: (str(key[0]), str(key[3]), str(key[4])))]


def isZipPath(path):
    return Path(path).suffix.lower() == ".zip"


def expandJsonInputs(paths, exitStack):
    """Return JSON paths, extracting the JSON members of zip archives into temporary directories."""
    expanded = []
    for rawPath in paths:
        path = Path(rawPath)
        if not isZipPath(path):
            expanded.append(str(path))
            continue

        temporaryDirectory = exitStack.enter_context(
            tempfile.TemporaryDirectory(prefix=f"{path.stem}-"))
        try:
            with zipfile.ZipFile(path) as archive:
                members = sorted(
                    (member for member in archive.infolist()
                     if not member.is_dir()
                     and not member.filename.startswith("__MACOSX/")
                     and member.filename.lower().endswith(".json")),
                    key=lambda member: member.filename)
                if not members:
                    raise SystemExit(f"error: no JSON files found in {path}")
                for member in members:
                    relativePath = Path(member.filename.replace("\\", "/"))
                    if relativePath.is_absolute() or ".." in relativePath.parts:
                        relativePath = Path(relativePath.name)
                    target = Path(temporaryDirectory) / relativePath
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(member) as source, open(target, "wb") as destination:
                        shutil.copyfileobj(source, destination)
                    expanded.append(str(target))
        except zipfile.BadZipFile as error:
            raise SystemExit(f"error: {path} is not a valid zip archive: {error}") from error
    return expanded


def plotFiles( args, jsons, show=True ):
    outputPrefix = args.output_prefix or defaultOutputPrefix(jsons)

    series = loadSeries(jsons, PLOT_EXCLUDED_TOOLS | set(getattr(args, "exclude_tools", [])))
    if not series:
        raise SystemExit("error: no benchmark results found")

    metadataFiles = []
    for path in jsons:
        with open(path) as file:
            metadataFiles.append((path, json.load(file)))
    metadata = metadataFiles[0][1]
    hostname = (metadata.get("hardware") or {}).get("hostname") or metadata.get("hostname") or ""
    referenceCpu = normalizeCpu(metadataCpu(metadata, ""))
    for path, current in metadataFiles[1:]:
        currentHostname = (current.get("hardware") or {}).get("hostname") or current.get("hostname") or ""
        currentCpu = normalizeCpu(metadataCpu(current, ""))
        mismatches = []
        if current.get("uncompressed_size") != metadata.get("uncompressed_size"):
            mismatches.append(
                f"uncompressed_size {current.get('uncompressed_size')!r} "
                f"!= {metadata.get('uncompressed_size')!r}")
        if current.get("compressed_size") != metadata.get("compressed_size"):
            mismatches.append(
                f"compressed_size {current.get('compressed_size')!r} "
                f"!= {metadata.get('compressed_size')!r}")
        if currentCpu != referenceCpu:
            mismatches.append(f"CPU {currentCpu!r} != {referenceCpu!r}")
        if not args.ignore_host and currentHostname != hostname:
            mismatches.append(f"hostname {currentHostname!r} != {hostname!r}")
        if mismatches:
            raise SystemExit(
                f"error: {path} differs from {metadataFiles[0][0]}:\n  "
                + "\n  ".join(mismatches)
                + "\nPlot it separately; use --ignore-host only for a node difference."
            )
    dataset = datasetName(metadata, jsons[0], args.dataset)
    cpu = metadataCpu(metadata, args.machine)
    if args.date:
        date = args.date
    else:
        dates = sorted({value for path, data in metadataFiles
                        if (value := metadataDate(data, path))})
        date = dates[-1] if dates else None
    size = metadata.get("uncompressed_size")
    ratio = metadata.get("compression_ratio")
    if not ratio and size and metadata.get("compressed_size"):
        ratio = size / metadata["compressed_size"]
    sizeLabel = formatSize(size)
    suptitle = dataset or "bzip2 decoding benchmark"
    if cpu:
        suptitle += f" on {cpu}"
    if date:
        suptitle += f" at {date}"
    if sizeLabel:
        suptitle += f" with {sizeLabel} uncompressed"
    if ratio:
        suptitle += f", compression ratio {ratio:.2f}"
    gzipRatio = next((data.get("gzip_compression_ratio") for _, data in metadataFiles
                      if data.get("gzip_compression_ratio")), None)
    if gzipRatio:
        suptitle += f", *gzip ratio: {gzipRatio:.2f}"

    def color(key):
        return COLORS.get(baseTool(key[0]), "#777777")

    myTools = {"indexed_bzip2", "ibzip2", "ibzip2-1.7.0", "rapidgzip", "rapidgzip-gzip"}
    singleCoreKeys = sorted(
        (key for key, threadData in series.items() if 1 in threadData),
        key=lambda key: (baseTool(key[0]) in myTools, series[key][1]["median"]),
        reverse=True)
    myToolCount = sum(1 for key in singleCoreKeys if baseTool(key[0]) in myTools)
    scalingCandidates = [
        key for key, threadData in series.items()
        if baseTool(key[0]) in SCALING_TOOLS and any(thread > 1 for thread in threadData)
    ]
    # Prefer the native lbzip2 build when both the 2.5 and the development build have scaling data.
    if any(key[0] == "lbzip2-native" for key in scalingCandidates):
        scalingCandidates = [key for key in scalingCandidates if key[0] != "lbzip2"]
    scalingKeys = sorted(
        scalingCandidates,
        key=lambda key: (SCALING_ORDER.index(baseTool(key[0]))
                         if baseTool(key[0]) in SCALING_ORDER else 99, key[1]))
    scalingBases = {baseTool(key[0]) for key in scalingKeys}

    figure, (ax1, ax2) = plt.subplots(1, 2, figsize=(FIGURE_WIDTH, FIGURE_HEIGHT), layout="constrained")
    # Leave only a small right margin; the outside legend fits into it.
    figure.get_layout_engine().set(rect=(0.01, 0.01, AXES_RECT_WIDTH, 0.98))
    title = figure.suptitle(suptitle, fontsize=11)

    # Left panel: zero-based horizontal bars with violin overlays. The bar shows the magnitude
    # from zero; the violin shows the distribution of the repeated measurements.
    for position, key in enumerate(singleCoreKeys):
        threadData = series[key][1]
        values = threadData["plot_values"]
        median = threadData["median"]
        ax1.barh(position, median, height=0.62, color=color(key), alpha=0.6,
                 edgecolor="none", zorder=1)
        ax1.set_xscale("log", base=10)
        if len(values) > 1:
            result = ax1.violinplot(values, positions=[position], vert=False, widths=0.62,
                                    showextrema=False, showmedians=False)
            for body in result["bodies"]:
                body.set_facecolor(color(key))
                body.set_edgecolor("none")
                body.set_alpha(1.0)
                body.set_zorder(2)
        labelX = min(values) - max(2.0, median * 0.02)
        ax1.text(labelX, position, f"{median:.1f}", va="center", ha="right",
                 fontsize=7, path_effects=TEXT_OUTLINE)

    if 0 < myToolCount < len(singleCoreKeys):
        ax1.axhline(myToolCount - 0.5, color="0.5", linestyle="--", linewidth=0.9, zorder=0)

    # Single-core speedup arrows. Both are drawn on the row of the slower version.
    speedupColor = COLORS.get("ibzip2", "#d62728")
    oldKey = next((key for key in singleCoreKeys if baseTool(key[0]) == "ibzip2-1.7.0"), None)
    newKey = max((key for key in singleCoreKeys if baseTool(key[0]) == "ibzip2"),
                 key=lambda key: versionKey(key[1]), default=None)

    def drawSpeedupArrow( fromKey, fromPosition, toKey ):
        fromMedian = series[fromKey][1]["median"]
        toMedian = series[toKey][1]["median"]
        if fromMedian >= toMedian:
            return
        annotation = ax1.annotate(
            "", xy=(toMedian, fromPosition), xytext=(fromMedian, fromPosition),
            arrowprops={"arrowstyle": "->", "color": speedupColor, "lw": 1.6},
            zorder=7, annotation_clip=False)
        if annotation.arrow_patch is not None:
            annotation.arrow_patch.set_path_effects(TEXT_OUTLINE)
        ax1.text((fromMedian + toMedian) / 2, fromPosition - 0.04,
                 f"{series[toKey][1]['median'] / series[fromKey][1]['median']:.1f}x",
                 color=speedupColor, fontsize=8, ha="center", va="bottom", zorder=8,
                 path_effects=TEXT_OUTLINE)

    if oldKey and newKey and oldKey != newKey:
        drawSpeedupArrow(oldKey, singleCoreKeys.index(oldKey), newKey)

    gnuKey = next((key for key in singleCoreKeys if baseTool(key[0]) == "GNU bzip2"), None)
    if gnuKey and newKey and gnuKey != newKey:
        drawSpeedupArrow(gnuKey, singleCoreKeys.index(gnuKey), newKey)

    ax1.set_yticks(range(len(singleCoreKeys)))
    ax1.set_yticklabels([axisLabel(key) for key in singleCoreKeys])
    if ax1.get_xscale() == "linear":
        ax1.set_xlim(left=0)
    ax1.set_ylim([-0.5, len(singleCoreKeys) - 0.5])
    ax1.set_xlabel("Uncompressed Bandwidth / (MB/s)  (higher is better)")
    ax1.set_title("Single Core")
    ax1.invert_yaxis()
    if not singleCoreKeys:
        ax1.set_axis_off()
        ax1.text(0.5, 0.5, "No single-core results\nin this partial run",
                 ha="center", va="center", transform=ax1.transAxes, fontsize=10, color="0.4")
    ax1.grid(axis="x", alpha=0.3)

    # Right panel: violins at each thread count, with the median line connecting the measurements.
    # With more than 24 threads the interesting part is the parallel region, so use a log axis
    # instead of squeezing the high thread counts together.
    threadCount = max(thread for threadData in series.values() for thread in threadData)
    logThreads = args.log_threads or threadCount > 24
    for key in scalingKeys:
        threadData = series[key]
        threads = sorted(thread for thread in threadData)
        if len(threads) < 2:
            continue

        base = baseTool(key[0])
        isCurrent = base in ("ibzip2", "indexed_bzip2")
        isOurs = base in ("ibzip2", "indexed_bzip2", "ibzip2-1.7.0", "rapidgzip", "rapidgzip-gzip")
        lineStyle = "-" if isOurs else "--"
        lineAlpha = 1.0
        violinAlpha = 0.7
        # Keep the legend (zorder 5) above the curves and the current versions above the references.
        lineZorder = 4 if isCurrent else 3.5 if isOurs else 3

        medians = []
        for thread in threads:
            width = thread * 0.12 if logThreads else 0.8
            values = threadData[thread]["plot_values"]
            median = threadData[thread]["median"]
            medians.append(median)
            if len(values) > 1:
                result = ax2.violinplot(values, positions=[thread], widths=width,
                                        showextrema=False, showmedians=False)
                for body in result["bodies"]:
                    body.set_facecolor(color(key))
                    body.set_edgecolor("none")
                    body.set_alpha(violinAlpha)
                    body.set_zorder(2)
        ax2.plot(threads, medians, color=color(key), linestyle=lineStyle, alpha=lineAlpha,
                 linewidth=1.3, label=seriesLabel(key), zorder=lineZorder)

    referenceByTool = {}
    for key in singleCoreKeys:
        base = baseTool(key[0])
        if base in REFERENCE_STYLES:
            if (base not in referenceByTool
                    or series[key][1]["median"] > series[referenceByTool[base]][1]["median"]):
                referenceByTool[base] = key
    referenceKeys = sorted((key for key in referenceByTool.values()
                            if baseTool(key[0]) not in scalingBases),
                           key=lambda key: series[key][1]["median"], reverse=True)
    for key in referenceKeys:
        ax2.axhline(series[key][1]["median"], label=seriesLabel(key), zorder=1,
                    **REFERENCE_STYLES[baseTool(key[0])])

    physicalCores = args.physical_cores
    if physicalCores <= 0:
        physicalCores = metadata.get("physical_cores") or 0
    if physicalCores <= 0:
        physicalCores = metadata.get("cpu_count", 0) // 2
    if physicalCores <= 0:
        physicalCores = threadCount // 2
    physicalCores = int(physicalCores or 0)

    if 0 < physicalCores < threadCount:
        ax2.axvline(physicalCores, color="0.5", linestyle=":", linewidth=0.9, zorder=0)
        ax2.text(physicalCores, 0.98, f"{physicalCores} physical cores",
                 transform=ax2.get_xaxis_transform(), ha="center", va="top",
                 color="0.4", fontsize=8, zorder=6, path_effects=TEXT_OUTLINE)

    numa = (metadata.get("hardware") or {}).get("numa") or {}
    numaThreadCounts = [len(parseCpuList(node.get("cpus")))
                        for node in (numa.get("nodes") or [])]
    numaThreadCounts = [count for count in numaThreadCounts if count > 0]
    largestNumaThreads = max(numaThreadCounts) if numaThreadCounts else None
    # The physical-core line already marks the same position when the largest NUMA domain is the
    # physical core count. The rightmost border is not a useful boundary either.
    if (largestNumaThreads and 0 < largestNumaThreads < threadCount
            and largestNumaThreads != physicalCores):
        ax2.axvline(largestNumaThreads, color="0.5", linestyle=":", linewidth=0.9, zorder=0)
        ax2.text(largestNumaThreads, 0.03,
                 f"largest NUMA domain ({largestNumaThreads} threads)",
                 transform=ax2.get_xaxis_transform(), ha="center", va="bottom",
                 color="0.4", fontsize=8, zorder=6, path_effects=TEXT_OUTLINE)

    oldKey = next((key for key in series if baseTool(key[0]) == "ibzip2-1.7.0"), None)
    newKeys = [key for key in series if baseTool(key[0]) in ("ibzip2", "indexed_bzip2")]
    hasIbzip2 = any(baseTool(key[0]) == "ibzip2" for key in newKeys)

    def speedupTargetKey(thread):
        candidates = [key for key in newKeys if thread in series[key]]
        ibzip2Keys = [key for key in candidates if baseTool(key[0]) == "ibzip2"]
        if ibzip2Keys:
            return max(ibzip2Keys, key=lambda key: versionKey(key[1]))
        if hasIbzip2:
            return None
        return min(candidates, key=lambda key: series[key][thread]["median"], default=None)

    gnuBaseline = None
    for key, threadData in series.items():
        if baseTool(key[0]) == "GNU bzip2" and 1 in threadData:
            gnuBaseline = max(gnuBaseline or 0, threadData[1]["median"])

    def drawVersionArrow(targetThread, startOffset):
        if not oldKey or targetThread <= 1 or targetThread not in series[oldKey]:
            return
        newKey = speedupTargetKey(targetThread)
        if newKey is None:
            return
        oldMedian = series[oldKey][targetThread]["median"]
        newMedian = series[newKey][targetThread]["median"]
        if not (oldMedian > 0 and newMedian > oldMedian):
            return
        color = speedupColor
        # A linear offset is invisible on a logarithmic axis, so use a multiplicative offset there.
        startX = targetThread * 0.94 if logThreads else targetThread + startOffset
        annotation = ax2.annotate(
            "", xy=(startX, newMedian), xytext=(startX, oldMedian),
            arrowprops={"arrowstyle": "->", "color": color, "lw": 1.6}, zorder=6)
        if annotation.arrow_patch is not None:
            annotation.arrow_patch.set_path_effects(TEXT_OUTLINE)
        textX = startX * 0.97 if logThreads else startX - 0.05
        ax2.text(textX, (oldMedian + newMedian) / 2,
                 f"{newMedian / oldMedian:.1f}x", color=color,
                 fontsize=8, ha="right", va="center", zorder=7,
                 path_effects=TEXT_OUTLINE)

    def drawBaselineArrow(targetThread):
        if not (newKeys and gnuBaseline and gnuBaseline > 0 and targetThread > 1):
            return
        newKey = speedupTargetKey(targetThread)
        if newKey is None:
            return
        newMedian = series[newKey][targetThread]["median"]
        if newMedian <= gnuBaseline:
            return
        color = speedupColor
        startX = targetThread
        annotation = ax2.annotate(
            "", xy=(startX, newMedian), xytext=(startX, gnuBaseline),
            arrowprops={"arrowstyle": "->", "color": color, "lw": 1.6}, zorder=6)
        if annotation.arrow_patch is not None:
            annotation.arrow_patch.set_path_effects(TEXT_OUTLINE)
        textX = startX * 1.03 if logThreads else startX - 0.07
        ax2.text(textX, (gnuBaseline + newMedian) / 2,
                 f"{newMedian / gnuBaseline:.1f}x", color=color,
                 fontsize=8, ha="right" if not logThreads else "left", va="center", zorder=7,
                 path_effects=TEXT_OUTLINE)

    drawVersionArrow(threadCount, -0.45)
    drawBaselineArrow(threadCount)
    if 0 < physicalCores < threadCount:
        drawVersionArrow(physicalCores, -0.45)
        drawBaselineArrow(physicalCores)

    ax2.set_xlabel("Threads")
    ax2.set_ylabel("Uncompressed Bandwidth / (MB/s)")
    threadCounts = sorted({thread for threadData in series.values() for thread in threadData})
    if logThreads:
        ax2.set_xscale("log", base=2)
        ax2.xaxis.set_major_locator(FixedLocator(threadCounts))
        threadIndex = {value: index for index, value in enumerate(threadCounts)}

        def threadTickLabel(value, _):
            # Stagger adjacent labels such as 104 and 128, which are nearly on top of each other
            # on a logarithmic axis, instead of dropping one of them.
            return f"\n{value:g}" if threadIndex.get(value, 0) % 2 and 104 in threadCounts else f"{value:g}"

        ax2.xaxis.set_major_formatter(FuncFormatter(threadTickLabel))
        ax2.xaxis.set_minor_formatter(NullFormatter())
    else:
        ax2.xaxis.set_major_locator(FixedLocator(threadCounts))
        ax2.xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
        ax2.set_xlim(0.5, max(threadCounts) + 0.5)
    if args.log_bandwidth:
        ax2.set_yscale("log")
        # Label a sparse 1-2-3-5 sequence instead of only the powers of ten, which would leave
        # the bandwidth axis with just 10^3 and 10^4. Label every minor tick would collide.
        def bandwidthTickLabel(value, _):
            return f"{value:.0f}"

        ax2.yaxis.set_major_locator(LogLocator(base=10, numticks=20))
        ax2.yaxis.set_major_formatter(FuncFormatter(bandwidthTickLabel))
        ax2.yaxis.set_minor_locator(LogLocator(base=10, subs=(2, 3, 5), numticks=100))
        ax2.yaxis.set_minor_formatter(FuncFormatter(bandwidthTickLabel))
    else:
        ax2.set_ylim(bottom=0)
    ax2.set_title("Parallel Scaling")
    ax2.grid(True, alpha=0.3)
    legend = ax2.legend(loc="center left", bbox_to_anchor=(1.11, 0.5), borderaxespad=0,
                        framealpha=0.9)
    legend.set_zorder(8)

    medians = [threadData["median"] for keyData in series.values() for threadData in keyData.values()]
    baseline, baselineLabel = chooseBaseline(series, args.speedup_baseline)
    if baseline is None:
        baseline = min(medians) if medians else 1.0
        baselineLabel = "minimum"
    speedupAxisLabel = f"Speedup over {baselineLabel}"
    addSpeedupAxis(ax2, baseline, speedupAxisLabel, args.log_speedup or args.log_bandwidth,
                   min(medians), max(medians),
                   includeFractions=args.log_speedup or args.log_bandwidth)

    # Center the title over the actual axes instead of the full figure. Otherwise a long title
    # extends past the legend and bbox_inches="tight" keeps a large empty area to its right.
    figure.canvas.draw()
    axesLeft = min(axis.get_position().x0 for axis in (ax1, ax2))
    axesRight = max(axis.get_position().x1 for axis in (ax1, ax2))
    title.set_x((axesLeft + axesRight) / 2)

    outputPath = Path(outputPrefix)
    outputPath.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(f"{outputPrefix}.png", dpi=200, bbox_inches="tight")
    figure.savefig(f"{outputPrefix}.pdf", bbox_inches="tight",
                   metadata={"CreationDate": None, "ModDate": None})
    print(f"Wrote {outputPrefix}.png and {outputPrefix}.pdf")
    if show:
        plt.show()
    else:
        plt.close(figure)
    return figure


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("jsons", nargs="*",
                        help="JSON files or .zip archives containing JSON files written by "
                             "benchmarkIndexedBzip2.py")
    parser.add_argument("--directory", default=None,
                        help="plot every benchmark JSON below this directory, grouping repeated runs "
                             "of the same input on the same machine")
    parser.add_argument("--output-prefix", default=None,
                        help="output file prefix (default: next to the input JSON with the common prefix "
                             "and suffix of all inputs; one input uses its full path without .json)")
    parser.add_argument("--speedup-baseline",
                        choices=("gnu-bzip2", "cpython-bz2", "ibzip2", "best"),
                        default="gnu-bzip2",
                        help="reference for the right speedup axis")
    parser.add_argument("--log-speedup", action="store_true",
                        help="use a base-2 logarithmic speedup axis")
    parser.add_argument("--log-threads", action="store_true",
                        help="use a base-2 logarithmic thread axis")
    parser.add_argument("--log-bandwidth", action="store_true",
                        help="use a logarithmic bandwidth axis for the scaling panel")
    parser.add_argument("--log-log", action="store_true",
                        help="shortcut for logarithmic thread, bandwidth, and speedup axes")
    parser.add_argument("--machine", default="",
                        help="override the CPU name recorded in the JSON metadata")
    parser.add_argument("--ignore-host", action="store_true",
                        help="merge JSONs recorded on different nodes, for example old reference runs")
    parser.add_argument("--dataset", default="",
                        help="override the dataset name derived from the JSON metadata")
    parser.add_argument("--date", default="",
                        help="override the measurement date recorded in the JSON metadata")
    parser.add_argument("--physical-cores", type=int, default=0,
                        help="draw the physical-core boundary and speedup arrows at this thread count "
                             "(default: physical_cores recorded by the benchmark, otherwise max threads / 2)")
    parser.add_argument("--exclude-tools", nargs="+", metavar="TOOL", default=[],
                        help="additional tools to omit from both panels, on top of the built-in exclusions "
                             + ", ".join(sorted(PLOT_EXCLUDED_TOOLS)) + ".")
    parser.add_argument("--no-show", action="store_true",
                        help="do not open an interactive window; only write PNG and PDF files")
    args = parser.parse_args()
    if args.log_log:
        args.log_threads = True
        args.log_bandwidth = True
        args.log_speedup = True

    if args.directory:
        if args.jsons:
            parser.error("pass either JSON files or --directory, not both")
        if args.output_prefix:
            parser.error("--output-prefix cannot be used with --directory")
        groups = discoverJsonGroups(args.directory)
        if not groups:
            raise SystemExit(f"error: no benchmark JSON files found in {args.directory}")
        for group in groups:
            plotFiles(args, group, show=False)
    else:
        if not args.jsons:
            parser.error("at least one JSON file or --directory is required")
        if not args.output_prefix and any(isZipPath(path) for path in args.jsons):
            args.output_prefix = defaultOutputPrefix(args.jsons)
        with contextlib.ExitStack() as exitStack:
            jsons = expandJsonInputs(args.jsons, exitStack)
            plotFiles(args, jsons, show=not args.no_show)


if __name__ == "__main__":
    main()
