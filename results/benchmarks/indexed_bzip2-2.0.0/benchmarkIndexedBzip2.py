#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Benchmark the bzip2 decoders that are commonly compared with indexed_bzip2.

The script measures CPython's bz2 module, GNU bzip2, GNU gzip, ISA-L igzip, libarchive's bsdcat, pbzip2, 7-Zip, lbzip2,
seek-bzip2's micro-bunzip, the ibzip2 CLI, and the indexed_bzip2 Python module. It also measures the
parallel Rust decoders pbz2, lbzip2-rs (the `lbunzip2` binary), fbz, and mibz2 through the streaming
driver in `helpers/mibz2-bench`, plus crabz2 through the whole-buffer driver in
`helpers/crabz2-bench`. If an `ibzip2-1.7.0` executable is in PATH, it is measured as an
additional CLI baseline. pbzip2 is measured
at 1 and 2 threads to show that it does not scale on files compressed with GNU bzip2; its parallel
decompression only applies to files that pbzip2 itself compressed in parallel. 7-Zip is measured
single-core because its bzip2 decoder is not parallelized for this format. The other tools are measured
with 1..N threads to show their scaling.

Example:
    python3 results/benchmarks/indexed_bzip2-2.0.0/benchmarkIndexedBzip2.py archive.tar.bz2 --iterations 3
    python3 results/benchmarks/indexed_bzip2-2.0.0/benchmarkIndexedBzip2.py --create archive.tar.bz2 --size-mib 128

Overnight rerun on the Ryzen 7 Pro 8840U, from the repository's build/ directory:
    ../results/benchmarks/indexed_bzip2-2.0.0/build-other-tools.sh
    export PATH="$PWD/../build-other-tools/bin:$PATH"
    for spec in silesia:7 silesia-20x:3 CTU-13-dataset:3; do
        python3 ../results/benchmarks/indexed_bzip2-2.0.0/benchmarkIndexedBzip2.py \
            "../test-files/${spec%:*}.tar.bz2" --iterations "${spec#*:}"
    done

The JSON files are written to the current directory (build/ here). Move the three JSON files into
results/benchmarks/indexed_bzip2-2.0.0/Ryzen 7 Pro 8840U/ after the run and plot each one with
plotBenchmarkIndexedBzip2.py.

The command-line tools write the decoded stream to /dev/null, which is their natural output sink.
The Python readers consume each chunk and discard it in memory because their public API is read-only.
No tool accumulates the decoded output, except crabz2, whose only parallel API returns the whole
plaintext; its driver discards the result after timing. The comparison therefore measures
decompression rather than disk I/O.

The Markdown table is printed to stdout and the raw results are written as JSON. Each result keeps
all iteration runtimes so that the plot can show error bars or individual measurements. The summary
runtime is the median of those iterations. The faster indexed_bzip2, ibzip2 2.0.0, and rapidgzip
measurements use --fast-iterations so their noisier results get more samples without dominating the
total runtime; the slower ibzip2 1.7.0, pbzip2, and reference tools keep --iterations.

The JSON also records detailed memory counters, NUMA topology, the hwloc XML topology, load and PSI
state before and after the run, and optional STREAM bandwidth. DIMM type and speed usually require
root, so --hardware-json can merge manually collected dmidecode data into the metadata.
"""

import argparse
import getpass
import base64
import bz2
import glob
from datetime import datetime
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree

CHUNK_SIZE = 4 * 1024 * 1024

# Keep this in sync with the wants() checks below. rapidgzip-gzip is derived from rapidgzip when a
# matching .gz input exists and is therefore not a separate --tools value.
ALLOWED_TOOLS = [
    "indexed_bzip2",
    "ibzip2",
    "ibzip2-1.7.0",
    "rapidgzip",
    "pbzip2",
    "pbz2",
    "lbzip2",
    "lbzip2-rs",
    "fbz",
    "mibz2",
    "crabz2",
    "CPython.bz2",
    "GNU bzip2",
    "GNU gzip",
    "igzip",
    "libarchive",
    "bzip2-rs-cli",
    "seek-bzip2",
    "7z",
]
# crabz2's only parallel entry point returns the complete plaintext. Skip it on inputs whose
# output would dominate memory; the streaming tools remain comparable on those datasets.
CRABZ2_MAX_OUTPUT_BYTES = 8 * 1024 ** 3


def createMixedFile(path, sizeMib):
    """Create a deterministic mix of English text and base64 data and compress it with bzip2."""
    text = (
        b"The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.\n"
        b"All human beings are born free and equal in dignity and rights.\n"
    )
    randomBlock = base64.b64encode(os.urandom(768 * 1024)) + b"\n"
    target = sizeMib * 1024 * 1024

    with tempfile.NamedTemporaryFile(dir=os.path.dirname(os.path.abspath(path)), delete=False) as raw:
        rawName = raw.name
        written = 0
        while written < target:
            for block in (text * 1024, randomBlock):
                raw.write(block)
                written += len(block)
                if written >= target:
                    break

    try:
        bzip2 = shutil.which("bzip2")
        if not bzip2:
            raise SystemExit("error: bzip2 is required to create the benchmark file")
        subprocess.run([bzip2, "-9", "-k", "-f", rawName], check=True)
        os.replace(rawName + ".bz2", path)
    finally:
        if os.path.exists(rawName):
            os.unlink(rawName)


def sanitizePath(path, home=None, user=None):
    """Replace the user's home and user name in path components for committed result JSONs."""
    if not path:
        return path
    if home is None:
        home = os.path.expanduser("~")
    if home and home != os.sep and (path == home or path.startswith(home + os.sep)):
        path = "/home/user" + path[len(home):]
    if user is None:
        user = getpass.getuser()
    if user:
        def sanitizeComponent(component):
            if (component == user or component.startswith(user + "-")
                    or component.startswith(user + "_") or component.startswith(user + ".")):
                return "user" + component[len(user):]
            return component
        path = os.sep.join(sanitizeComponent(component) for component in path.split(os.sep))
    if path.startswith(("/media/", "/data/", "/mnt/", "/Users/")):
        return os.path.basename(path)
    return path


def readInChunks(file):
    # readinto reuses one buffer. file.read would allocate and free a new bytes object for every
    # 4 MiB chunk, which adds about 15 s on the 80 GB CTU-13 output and is not decode time.
    buffer = bytearray(CHUNK_SIZE)
    while file.readinto(buffer):
        pass


def getUncompressedSize(path):
    try:
        import indexed_bzip2
    except ImportError:
        indexed_bzip2 = None

    if indexed_bzip2 is not None:
        # Build the index with all cores; the size query is not part of the timed benchmark.
        file = indexed_bzip2.IndexedBzip2File(path, parallelization=0)
    else:
        file = bz2.open(path, "rb")

    with file:
        size = file.size() if indexed_bzip2 is not None else 0
        if size:
            return size

        # Older versions return 0 until the reader reached EOF. Counting the decoded bytes also
        # keeps this script usable with them.
        total = 0
        while True:
            chunk = file.read(CHUNK_SIZE)
            if not chunk:
                break
            total += len(chunk)
        return total


def getVersion(command):
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, errors="replace", check=False, timeout=10)
        # bzip2 1.0.8 prints its version and then keeps reading stdin to compress it. Closing stdin and
        # ignoring the exit status keeps the version probe from blocking or treating it as an error.
        output = "\n".join(part for part in (result.stderr, result.stdout) if part).strip()
        lines = [line for line in output.splitlines() if line.strip()]
        return lines[0] if lines else None
    except Exception:
        return None


def getToolVersions(ibzip2, ibzip2_1_7_0, rapidgzip, lbzip2, bzip2, gzip, igzip, pbzip2, bzip2RsCli,
                    pbz2, sevenZip, libarchive, seekBzip2, haveIndexedBzip2, fbz, lbzip2Rs,
                    mibz2, crabz2):
    versions = {"CPython.bz2": f"CPython {platform.python_version()} (bz2 module)"}
    if haveIndexedBzip2:
        import indexed_bzip2
        versions["indexed_bzip2"] = getattr(indexed_bzip2, "__version__", None)
        # The version string cannot distinguish a stale local extension from a current one. Record
        # the loaded file so that a rebuild after a kernel change is visible in the JSON.
        modulePath = getattr(indexed_bzip2, "__file__", None)
        if modulePath:
            moduleStat = os.stat(modulePath)
            versions["indexed_bzip2.module"] = (
                f"{os.path.basename(modulePath)} ({moduleStat.st_size} B, mtime {moduleStat.st_mtime:.0f})")
    if bzip2:
        versions["GNU bzip2"] = getVersion([bzip2, "--version"])
    if gzip:
        versions["GNU gzip"] = getVersion([gzip, "--version"])
    if igzip:
        versions["igzip"] = getVersion([igzip, "--version"]) or getVersion([igzip, "-V"]) or ""
    if libarchive:
        versions["libarchive"] = getVersion([libarchive, "--version"])
    if pbzip2:
        versions["pbzip2"] = getVersion([pbzip2, "--version"])
    if bzip2RsCli:
        versions["bzip2-rs-cli"] = getVersion([bzip2RsCli, "--version"])
    if seekBzip2:
        versionPath = os.path.join(os.path.dirname(seekBzip2), "micro-bunzip.version")
        try:
            with open(versionPath, "r", encoding="utf-8") as versionFile:
                versions["seek-bzip2"] = versionFile.read().strip()
        except OSError:
            versions["seek-bzip2"] = "micro-bunzip"
    if pbz2:
        versions["pbz2"] = getVersion([pbz2, "--version"])
    if fbz:
        versions["fbz"] = getVersion([fbz, "--version"])
    if lbzip2Rs:
        versionPath = os.path.join(os.path.dirname(lbzip2Rs), "lbzip2-rs.version")
        try:
            with open(versionPath, "r", encoding="utf-8") as versionFile:
                versions["lbzip2-rs"] = versionFile.read().strip()
        except OSError:
            versions["lbzip2-rs"] = "lbzip2-rs"
    if mibz2:
        versionPath = os.path.join(os.path.dirname(mibz2), "mibz2.version")
        try:
            with open(versionPath, "r", encoding="utf-8") as versionFile:
                versions["mibz2"] = versionFile.read().strip()
        except OSError:
            versions["mibz2"] = "mibz2"
    if crabz2:
        versionPath = os.path.join(os.path.dirname(crabz2), "crabz2.version")
        try:
            with open(versionPath, "r", encoding="utf-8") as versionFile:
                versions["crabz2"] = versionFile.read().strip()
        except OSError:
            versions["crabz2"] = "crabz2"
    if sevenZip:
        versions["7z"] = getVersion([sevenZip, "i"])
    if lbzip2:
        version = getVersion([lbzip2, "--version"])
        revisionPath = os.path.join(os.path.dirname(lbzip2), "lbzip2.version")
        try:
            with open(revisionPath, "r", encoding="utf-8") as revisionFile:
                revision = revisionFile.read().strip()
        except OSError:
            revision = None
        # Older lbzip2 CMake builds hardcode "devel"; the build script writes the real revision
        # next to the executable. Prefer that over the useless version string.
        if revision and version and "devel" in version:
            version = revision
        versions["lbzip2"] = version
    if ibzip2:
        versions["ibzip2"] = getVersion([ibzip2, "--version"])
        commitPath = os.path.join(os.path.dirname(ibzip2), "ibzip2.commit")
        try:
            with open(commitPath, "r", encoding="utf-8") as commitFile:
                versions["ibzip2.commit"] = commitFile.read().strip()
        except OSError:
            pass
    if ibzip2_1_7_0:
        versions["ibzip2-1.7.0"] = getVersion([ibzip2_1_7_0, "--version"])
    if rapidgzip:
        versions["rapidgzip"] = getVersion([rapidgzip, "--version"])
        versions["rapidgzip-gzip"] = versions["rapidgzip"]
    return {name: version for name, version in versions.items() if version}


def measure(function, iterations, progress=None):
    times = []
    try:
        for _ in range(iterations):
            start = time.perf_counter()
            function()
            times.append(time.perf_counter() - start)
            if progress is not None:
                progress(times)
    except Exception as error:
        print(f"Benchmark failed and will be skipped: {type(error).__name__}: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors="replace").strip(), file=sys.stderr)
        return None
    return statistics.median(times), times


def benchmarkPythonBz2(path, iterations, progress=None):
    return measure(lambda: benchmarkPythonBz2Once(path), iterations, progress)


def benchmarkPythonBz2Once(path):
    with bz2.open(path, "rb") as file:
        readInChunks(file)


def benchmarkIndexedBzip2(path, parallelization, iterations, progress=None):
    def run():
        import indexed_bzip2
        with indexed_bzip2.IndexedBzip2File(path, parallelization=parallelization) as file:
            readInChunks(file)
    return measure(run, iterations, progress)


def benchmarkCommand(command, iterations, stdinPath=None, progress=None, env=None):
    def run():
        runEnv = None
        if env is not None:
            runEnv = os.environ.copy()
            runEnv.update(env)
        if stdinPath is None:
            subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, check=True, env=runEnv)
        else:
            with open(stdinPath, "rb") as stdin:
                subprocess.run(command, stdin=stdin, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, check=True, env=runEnv)
    return measure(run, iterations, progress)


def defaultThreads(cpuCount):
    # This is the set of thread counts used by the committed 1.7.0 reference plot after merging the
    # partial runs. Keep it stable so that new releases can be plotted against that reference.
    threads = {1, 2, 4, 8, 12, 16, 20, 24}
    threads.add(cpuCount)
    return sorted(thread for thread in threads if 1 <= thread <= cpuCount)




def shortProcessorName(model):
    """Return a short, filename-safe processor name."""
    if not model:
        return platform.machine() or "unknown-cpu"
    name = re.sub(r"\((?:R|TM|C)\)", "", str(model), flags=re.I)
    name = re.sub(r"@.*$", "", name)
    name = re.sub(r"\b(?:w/|with)\s*.*$", "", name, flags=re.I)
    name = re.sub(r"\b\d+-Core\b", "", name, flags=re.I)
    name = re.sub(r"\b(AMD|Intel|Apple|ARM|Processor|CPU|Core|with|Radeon|Graphics)\b", " ", name, flags=re.I)
    name = re.sub(r"[^A-Za-z0-9.+_-]+", "-", name.strip())
    name = re.sub(r"-+", "-", name).strip("-")
    return name[:40] or (platform.machine() or "unknown-cpu")


def _formatBytes(value):
    if value is None:
        return "unknown"
    return f"{value / 1e9:.1f} GB"


def benchmarkOutputName(inputFile, hardware):
    """Build a machine- and input-specific default result name."""
    timestamp = datetime.now().strftime("%Y-%m-%dT%H-%M")
    processor = shortProcessorName(hardware.get("cpu", {}).get("model"))
    name = os.path.basename(inputFile)
    for suffix in (".bz2", ".tar", ".gz", ".xz", ".zip", ".7z"):
        if name.lower().endswith(suffix):
            name = name[:-len(suffix)]
    name = re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip("-") or "input"
    jobId = os.environ.get("SLURM_JOB_ID")
    jobSuffix = f"-job{jobId}" if jobId else ""
    return f"indexed_bzip2-{timestamp}-{processor}-{name}{jobSuffix}.json"


def _runQuietly(command, timeout=10):
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    return completed.stdout if completed.returncode == 0 else None


def _readTextQuietly(path):
    try:
        with open(path, "r", errors="replace") as file:
            return file.read()
    except OSError:
        return ""


def _parseColonFields(text):
    fields = {}
    for line in text.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            fields[key.strip()] = value.strip()
    return fields


def _toNumber(value):
    if value is None:
        return None
    match = re.search(r"[-+]?\d+(?:\.\d+)?", str(value))
    return float(match.group()) if match else None


def _toInt(value):
    number = _toNumber(value)
    return int(number) if number is not None else None


def _toString(value):
    return str(value).strip() if value is not None else None


def _parseMeminfo(text):
    """Return /proc/meminfo-style fields, with kB values converted to bytes."""
    fields = {}
    for line in text.splitlines():
        key, separator, rest = line.partition(":")
        if not separator:
            continue
        key = re.sub(r"^Node\s+\d+\s+", "", key.strip())
        match = re.match(r"\s*(\d+)(?:\s*(kB))?\s*$", rest)
        if not match:
            continue
        value = int(match.group(1))
        fields[key] = value * 1024 if match.group(2) else value
    return fields


def _readMeminfo(path="/proc/meminfo"):
    return _parseMeminfo(_readTextQuietly(path))


def _parseSize(value):
    if value is None:
        return None
    match = re.search(r"(\d+(?:\.\d+)?)\s*([KMGTP]?)i?B\b", str(value), re.IGNORECASE)
    if not match:
        return None
    factors = {"": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4, "P": 1024 ** 5}
    return int(float(match.group(1)) * factors[match.group(2).upper()])


def _parseByteCount(value):
    """Parse hwloc XML sizes, which may be bare byte counts or have an IEC suffix."""
    text = str(value).strip() if value is not None else ""
    return int(text) if re.fullmatch(r"\d+", text) else _parseSize(text)


def _buddyInfo():
    zones = []
    for line in _readTextQuietly("/proc/buddyinfo").splitlines():
        match = re.match(r"Node\s+(\d+),\s+zone\s+(\S+)\s+(.*)$", line)
        if not match:
            continue
        try:
            counts = [int(value) for value in match.group(3).split()]
        except ValueError:
            continue
        zones.append({"node": int(match.group(1)), "zone": match.group(2), "free_blocks_by_order": counts})
    return zones or None


def _vmstat():
    fields = {}
    for line in _readTextQuietly("/proc/vmstat").splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip().isdigit():
            fields[parts[0]] = int(parts[1])
    return fields or None


def _cgroupMemoryLimit():
    for path in ("/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"):
        value = _readTextQuietly(path).strip()
        if not value or value == "max":
            continue
        try:
            return int(value)
        except ValueError:
            pass
    return None


def _dmiMemoryModules():
    """Parse dmidecode output when the job happens to run with sufficient privileges."""
    text = _runQuietly(["dmidecode", "-t", "memory"], timeout=20)
    if not text:
        return None
    modules = []
    fields = {}
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Memory Device"):
            if fields:
                modules.append(fields)
            fields = {}
        elif ":" in stripped:
            key, value = stripped.split(":", 1)
            fields[key.strip()] = value.strip()
    if fields:
        modules.append(fields)

    result = []
    for fields in modules:
        if fields.get("Size", "").lower() in ("", "no module installed", "0 mb"):
            continue
        result.append({
            "size": fields.get("Size"),
            "size_bytes": _parseSize(fields.get("Size")),
            "type": fields.get("Type"),
            "speed_mts": _toNumber(fields.get("Speed")),
            "configured_speed_mts": _toNumber(fields.get("Configured Memory Speed")),
            "manufacturer": fields.get("Manufacturer"),
            "part_number": fields.get("Part Number"),
            "rank": fields.get("Rank"),
            "form_factor": fields.get("Form Factor"),
            "locator": fields.get("Locator"),
            "bank_locator": fields.get("Bank Locator"),
            "data_width_bits": _toInt(fields.get("Data Width")),
            "total_width_bits": _toInt(fields.get("Total Width")),
            "technology": fields.get("Technology"),
            "configured_voltage": fields.get("Configured Voltage"),
        })
    return result or None


def _edacMemoryInfo():
    modules = []
    for path in sorted(glob.glob("/sys/devices/system/edac/mc/mc*/dimm*")):
        module = {}
        for field, key in (("dimm_mem_type", "type"), ("size", "size"),
                           ("dimm_label", "label"), ("dimm_location", "location")):
            value = _readTextQuietly(os.path.join(path, field)).strip()
            if value:
                module[key] = value
        if module:
            module["size_bytes"] = _parseSize(module.get("size"))
            modules.append(module)
    return modules or None


def _linuxMemoryInfo():
    meminfo = _readMeminfo()
    memory = {
        "total_bytes": meminfo.get("MemTotal"),
        "free_bytes": meminfo.get("MemFree"),
        "available_bytes": meminfo.get("MemAvailable"),
        "used_bytes": None,
        "buffers_bytes": meminfo.get("Buffers"),
        "cached_bytes": meminfo.get("Cached"),
        "swap_cached_bytes": meminfo.get("SwapCached"),
        "active_bytes": meminfo.get("Active"),
        "inactive_bytes": meminfo.get("Inactive"),
        "anon_pages_bytes": meminfo.get("AnonPages"),
        "mapped_bytes": meminfo.get("Mapped"),
        "shmem_bytes": meminfo.get("Shmem"),
        "unevictable_bytes": meminfo.get("Unevictable"),
        "mlocked_bytes": meminfo.get("Mlocked"),
        "slab_bytes": meminfo.get("Slab"),
        "slab_reclaimable_bytes": meminfo.get("SReclaimable"),
        "slab_unreclaimable_bytes": meminfo.get("SUnreclaim"),
        "page_tables_bytes": meminfo.get("PageTables"),
        "dirty_bytes": meminfo.get("Dirty"),
        "writeback_bytes": meminfo.get("Writeback"),
        "swap_total_bytes": meminfo.get("SwapTotal"),
        "swap_free_bytes": meminfo.get("SwapFree"),
        "commit_limit_bytes": meminfo.get("CommitLimit"),
        "committed_as_bytes": meminfo.get("Committed_AS"),
        "hugepages_total": meminfo.get("HugePages_Total"),
        "hugepages_free": meminfo.get("HugePages_Free"),
        "hugepage_size_bytes": meminfo.get("Hugepagesize"),
        "cgroup_limit_bytes": _cgroupMemoryLimit(),
        "page_size_bytes": None,
        "buddyinfo": _buddyInfo(),
        "meminfo": meminfo,
        "vmstat": _vmstat(),
    }
    try:
        memory["page_size_bytes"] = os.sysconf("SC_PAGE_SIZE")
    except (AttributeError, ValueError):
        pass
    if memory["total_bytes"] is not None and memory["available_bytes"] is not None:
        memory["used_bytes"] = memory["total_bytes"] - memory["available_bytes"]

    modules = _dmiMemoryModules()
    if modules:
        memory["hardware_source"] = "dmidecode"
        memory["modules"] = modules
        first = modules[0]
        memory.update({
            "type": first.get("type"),
            "speed_mts": first.get("speed_mts"),
            "configured_speed_mts": first.get("configured_speed_mts"),
            "manufacturer": first.get("manufacturer"),
            "part_number": first.get("part_number"),
            "module_size": first.get("size"),
        })
    else:
        edac = _edacMemoryInfo()
        if edac:
            memory["hardware_source"] = "edac"
            memory["edac"] = edac
            memory["type"] = edac[0].get("type")
        else:
            lshw = _runQuietly(["lshw", "-short", "-C", "memory"], timeout=20)
            if lshw:
                type_match = re.search(r"(DDR\d|LPDDR\d)", lshw)
                if type_match:
                    memory["type"] = type_match.group(1)
                    memory["hardware_source"] = "lshw"
                speed_match = re.search(r"(\d+)\s*MHz", lshw)
                if speed_match:
                    memory["speed_mts"] = int(speed_match.group(1))
    return memory


def _parseNumactlHardware(text):
    nodes = []
    distances = {}
    current = None
    inDistances = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("node distances:"):
            inDistances = True
            continue
        if inDistances:
            match = re.match(r"(\d+):\s+(.+)$", stripped)
            if match:
                distances[match.group(1)] = [int(value) for value in match.group(2).split()]
            continue
        match = re.match(r"node (\d+) (cpus|size|free):\s*(.*)$", stripped)
        if not match:
            continue
        nodeId = int(match.group(1))
        if match.group(2) == "cpus":
            current = {"id": nodeId, "cpus": match.group(3).strip()}
            nodes.append(current)
        elif current is not None and current["id"] == nodeId:
            scale = 1024 * 1024
            if match.group(2) == "size":
                current["size_bytes"] = _toInt(match.group(3)) * scale
            else:
                current["free_bytes"] = _toInt(match.group(3)) * scale
    if not nodes:
        return None
    for node in nodes:
        if str(node["id"]) in distances:
            node["distance"] = distances[str(node["id"])]
    return {"node_count": len(nodes), "nodes": nodes, "distances": distances, "source": "numactl"}


def _sysfsNumaInfo():
    base = "/sys/devices/system/node"
    if not os.path.isdir(base):
        return None
    nodeIds = sorted(int(entry[4:]) for entry in os.listdir(base) if re.fullmatch(r"node\d+", entry))
    if not nodeIds:
        return None
    nodes = []
    for nodeId in nodeIds:
        path = os.path.join(base, f"node{nodeId}")
        meminfo = _readMeminfo(os.path.join(path, "meminfo"))
        distance = _readTextQuietly(os.path.join(path, "distance")).split()
        nodes.append({
            "id": nodeId,
            "cpus": _readTextQuietly(os.path.join(path, "cpulist")).strip() or None,
            "size_bytes": meminfo.get("MemTotal"),
            "free_bytes": meminfo.get("MemFree"),
            "used_bytes": meminfo.get("MemUsed"),
            "distance": [int(value) for value in distance if value.isdigit()] or None,
        })
    return {"node_count": len(nodes), "nodes": nodes, "source": "sysfs"}


def _numaInfo():
    text = _runQuietly(["numactl", "--hardware"], timeout=10)
    if text:
        info = _parseNumactlHardware(text)
        if info:
            return info
    return _sysfsNumaInfo()


def _parseCpuList(value):
    cpus = set()
    for part in re.split(r"[,\s]+", value or ""):
        if not part:
            continue
        if "-" in part:
            start, end = part.split("-", 1)
            if start.isdigit() and end.isdigit():
                cpus.update(range(int(start), int(end) + 1))
        elif part.isdigit():
            cpus.add(int(part))
    return cpus


def _affinityNumaNodes(affinity, numa):
    if affinity is None or not numa or not numa.get("nodes"):
        return None
    affinitySet = set(affinity)
    result = []
    for node in numa["nodes"]:
        intersection = affinitySet & _parseCpuList(node.get("cpus"))
        if intersection:
            result.append({"id": node["id"], "cpus": sorted(intersection)})
    return result or None


def _linuxVulnerabilities():
    base = "/sys/devices/system/cpu/vulnerabilities"
    if not os.path.isdir(base):
        return None
    info = {}
    for name in sorted(os.listdir(base)):
        value = _readTextQuietly(os.path.join(base, name)).strip()
        if value:
            info[name] = value
    return info or None


def _kernelInfo():
    # The kernel command line can contain root=UUID=... and other machine-local identifiers.
    return {
        "release": platform.release(),
        "version": platform.version(),
    }


def _rawToolOutputs():
    """Keep the raw output of read-only probes so fields can be extracted in post.

    Raw dmidecode output is deliberately not stored because it contains serial numbers.
    """
    outputs = {
        "lscpu": _runQuietly(["lscpu"], timeout=10),
        "numactl_hardware": _runQuietly(["numactl", "--hardware"], timeout=10),
        "free": _runQuietly(["free", "-b"], timeout=10),
    }
    return {key: value.strip() for key, value in outputs.items() if value and value.strip()} or None


def _linuxSystemDmi():
    fields = {
        "system_vendor": "sys_vendor",
        "product_name": "product_name",
        "product_version": "product_version",
        "board_vendor": "board_vendor",
        "board_name": "board_name",
        "bios_vendor": "bios_vendor",
        "bios_version": "bios_version",
        "bios_date": "bios_date",
    }
    info = {}
    for key, name in fields.items():
        value = _readTextQuietly(f"/sys/class/dmi/id/{name}").strip()
        if value:
            info[key] = value
    return info or None


def _hwlocTopology():
    for executable in ("lstopo-no-graphics", "lstopo"):
        path = shutil.which(executable)
        if not path:
            continue
        text = _runQuietly([path, "--of", "xml"], timeout=30)
        if text and "<topology" in text:
            return text
    return None


def _summarizeHwloc(xmlText):
    try:
        root = ElementTree.fromstring(xmlText)
    except ElementTree.ParseError:
        return {}
    counts = {}
    cacheBytes = {}
    for entry in root.iter("object"):
        typeName = entry.get("type", "")
        counts[typeName] = counts.get(typeName, 0) + 1
        if typeName.endswith("Cache"):
            for info in entry.findall("info"):
                if info.get("name") == "size":
                    cacheBytes[typeName] = cacheBytes.get(typeName, 0) + (_parseByteCount(info.get("value")) or 0)
    summary = {
        "packages": counts.get("Package"),
        "numa_nodes": counts.get("NUMANode"),
        "cores": counts.get("Core"),
        "processing_units": counts.get("PU"),
        "object_counts": counts,
    }
    if cacheBytes:
        summary["cache_total_bytes"] = cacheBytes
    return summary


def _schedulerInfo():
    variables = {
        "job_id": "SLURM_JOB_ID",
        "job_name": "SLURM_JOB_NAME",
        "partition": "SLURM_PARTITION",
        "cpus_per_task": "SLURM_CPUS_PER_TASK",
        "cpus_on_node": "SLURM_CPUS_ON_NODE",
        "nodes": "SLURM_NNODES",
        "node_list": "SLURM_NODELIST",
        "memory_per_node_mib": "SLURM_MEM_PER_NODE",
        "memory_per_cpu_mib": "SLURM_MEM_PER_CPU",
        "constraint": "SLURM_CONSTRAINT",
    }
    info = {key: os.environ[name] for key, name in variables.items() if os.environ.get(name)}
    if not info:
        return None
    for key in ("cpus_per_task", "cpus_on_node", "nodes", "memory_per_node_mib", "memory_per_cpu_mib"):
        if key in info:
            number = _toInt(info[key])
            if number is not None:
                info[key] = number
    info["type"] = "slurm"
    return info


def _systemState():
    state = {}
    loadAverage = _readTextQuietly("/proc/loadavg").split()
    if len(loadAverage) >= 4:
        try:
            state["load_average"] = [float(value) for value in loadAverage[:3]]
            running, total = loadAverage[3].split("/", 1)
            state["running_tasks"] = int(running)
            state["total_tasks"] = int(total)
        except ValueError:
            pass
    pressure = {}
    for resource in ("cpu", "memory", "io"):
        for line in _readTextQuietly(f"/proc/pressure/{resource}").splitlines():
            fields = {}
            for item in line.split()[1:]:
                key, separator, value = item.partition("=")
                if separator:
                    try:
                        fields[key] = float(value)
                    except ValueError:
                        pass
            if line.split() and fields:
                pressure.setdefault(resource, {})[line.split()[0]] = fields
    if pressure:
        state["pressure"] = pressure
    meminfo = _readMeminfo()
    state["memory"] = {
        "free_bytes": meminfo.get("MemFree"),
        "available_bytes": meminfo.get("MemAvailable"),
        "swap_free_bytes": meminfo.get("SwapFree"),
        "dirty_bytes": meminfo.get("Dirty"),
        "writeback_bytes": meminfo.get("Writeback"),
    }
    for path in ("/sys/fs/cgroup/memory.current", "/sys/fs/cgroup/memory/memory.usage_in_bytes"):
        value = _readTextQuietly(path).strip()
        if value.isdigit():
            state["cgroup_usage_bytes"] = int(value)
            break
    try:
        state["process_count"] = sum(1 for entry in os.listdir("/proc") if entry.isdigit())
    except OSError:
        state["process_count"] = None
    return state


def _mergeDictionary(target, override):
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(target.get(key), dict):
            _mergeDictionary(target[key], value)
        else:
            target[key] = value


def _streamBandwidth(executable, threads):
    if not executable or not os.access(executable, os.X_OK):
        return None
    environment = os.environ.copy()
    if threads:
        environment["OMP_NUM_THREADS"] = str(threads)
    try:
        completed = subprocess.run([executable], capture_output=True, text=True, errors="replace",
                                   timeout=300, env=environment, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    result = {"tool": "STREAM", "threads": threads}
    for line in completed.stdout.splitlines():
        match = re.match(r"\s*(Copy|Scale|Add|Triad):\s+([\d.]+)", line)
        if match:
            result[match.group(1).lower() + "_mb_s"] = float(match.group(2))
    if not any(key.endswith("_mb_s") for key in result):
        return None
    result["raw"] = completed.stdout.strip()
    return result


def _linuxHardwareInfo():
    cpu = {}
    cpuinfo = _readTextQuietly("/proc/cpuinfo")
    flags = []
    physicalIds = set()
    for block in cpuinfo.split("\n\n"):
        fields = _parseColonFields(block)
        if not fields:
            continue
        for field, key in (("model name", "model"), ("vendor_id", "vendor"), ("cpu family", "cpu_family"),
                           ("model", "model_id"), ("stepping", "stepping"), ("microcode", "microcode"),
                           ("cache size", "cache_size"), ("cpu MHz", "mhz")):
            if key not in cpu and field in fields:
                cpu[key] = fields[field]
        if not flags and "flags" in fields:
            flags = fields["flags"].split()
        if "physical id" in fields and "core id" in fields:
            physicalIds.add((fields["physical id"], fields["core id"]))

    cpu["model"] = _toString(cpu.get("model")) or platform.processor() or None
    cpu["vendor"] = _toString(cpu.get("vendor"))
    cpu["mhz"] = _toNumber(cpu.get("mhz"))
    cpu["flags"] = flags or None
    cpu["physical_cores"] = len(physicalIds) or None
    cpu["logical_cores"] = os.cpu_count()

    lscpu = _parseColonFields(_runQuietly(["lscpu"]) or "")
    if lscpu:
        cpu["model"] = cpu["model"] or _toString(lscpu.get("Model name"))
        cpu["vendor"] = cpu["vendor"] or _toString(lscpu.get("Vendor ID"))
        if not cpu["physical_cores"]:
            try:
                cpu["physical_cores"] = (
                    int(lscpu.get("Socket(s)", "0")) * int(lscpu.get("Core(s) per socket", "0")) or None
                )
            except ValueError:
                pass
        cpu["flags"] = cpu["flags"] or (lscpu.get("Flags", "").split() or None)
        cpu["max_mhz"] = _toNumber(lscpu.get("CPU max MHz"))
        cpu["min_mhz"] = _toNumber(lscpu.get("CPU min MHz"))
        cpu["sockets"] = _toInt(lscpu.get("Socket(s)"))
        cpu["threads_per_core"] = _toInt(lscpu.get("Thread(s) per core"))
        cpu["numa_nodes"] = _toInt(lscpu.get("NUMA node(s)"))
        cpu["lscpu"] = lscpu

    return {"cpu": cpu, "memory": _linuxMemoryInfo()}


def _sysctl(name):
    value = _runQuietly(["sysctl", "-n", name])
    return value.strip() if value else None


def _macHardwareInfo():
    cpu = {
        "model": _sysctl("machdep.cpu.brand_string") or platform.processor() or None,
        "vendor": _sysctl("machdep.cpu.vendor"),
        "physical_cores": _toInt(_sysctl("hw.physicalcpu")),
        "logical_cores": _toInt(_sysctl("hw.logicalcpu")) or os.cpu_count(),
    }
    frequencies = [_toNumber(_sysctl(name)) for name in ("hw.cpufrequency_max", "hw.cpufrequency")]
    cpu["max_mhz"] = next((frequency / 1e6 for frequency in frequencies if frequency), None)
    flags = []
    for name in ("machdep.cpu.features", "machdep.cpu.leaf7_features"):
        value = _sysctl(name)
        if value:
            flags.extend(value.split())
    cpu["flags"] = flags or None

    memory = {"total_bytes": _toInt(_sysctl("hw.memsize"))}
    text = _runQuietly(["system_profiler", "SPMemoryDataType", "-json"], timeout=20)
    if text:
        try:
            for entry in json.loads(text).get("SPMemoryDataType", []):
                for item in entry.get("_items", []):
                    if item.get("dimm_type") or item.get("dimm_speed"):
                        memory.update({
                            "type": item.get("dimm_type"),
                            "speed_mts": _toNumber(item.get("dimm_speed")),
                            "manufacturer": item.get("dimm_manufacturer"),
                            "part_number": item.get("dimm_part_number"),
                            "module_size": item.get("dimm_size"),
                        })
                        break
        except (ValueError, KeyError, TypeError):
            pass

    return {"cpu": cpu, "memory": memory}


def _smbiosMemoryType(code):
    return {
        20: "DDR", 21: "DDR2", 22: "DDR2 FB-DIMM", 24: "DDR3", 26: "DDR4",
        27: "LPDDR", 28: "LPDDR2", 29: "LPDDR3", 30: "LPDDR4", 34: "DDR5",
    }.get(code)


def _windowsHardwareInfo():
    cpu = {"physical_cores": None, "logical_cores": os.cpu_count(), "flags": None}
    text = _runQuietly([
        "powershell", "-NoProfile", "-Command",
        "Get-CimInstance Win32_Processor | Select-Object Name,Vendor,NumberOfCores,"
        "NumberOfLogicalProcessors,MaxClockSpeed | ConvertTo-Json -Compress",
    ], timeout=20)
    if text:
        try:
            data = json.loads(text)
            if isinstance(data, list):
                data = data[0] if data else {}
            cpu.update({
                "model": data.get("Name"),
                "vendor": data.get("Vendor"),
                "physical_cores": data.get("NumberOfCores"),
                "logical_cores": data.get("NumberOfLogicalProcessors"),
                "max_mhz": data.get("MaxClockSpeed"),
            })
        except (ValueError, KeyError, TypeError):
            pass

    memory = {"total_bytes": None}
    text = _runQuietly([
        "powershell", "-NoProfile", "-Command",
        "Get-CimInstance Win32_PhysicalMemory | Select-Object Capacity,Speed,ConfiguredClockSpeed,"
        "SMBIOSMemoryType,Manufacturer,PartNumber | ConvertTo-Json -Compress",
    ], timeout=20)
    if text:
        try:
            data = json.loads(text)
            if isinstance(data, dict):
                data = [data]
            if data:
                first = data[0]
                memory.update({
                    "type": _smbiosMemoryType(first.get("SMBIOSMemoryType")),
                    "speed_mts": first.get("Speed"),
                    "configured_speed_mts": first.get("ConfiguredClockSpeed"),
                    "manufacturer": first.get("Manufacturer"),
                    "part_number": first.get("PartNumber"),
                    "module_size": first.get("Capacity"),
                })
        except (ValueError, KeyError, TypeError):
            pass

    return {"cpu": cpu, "memory": memory}


def getHardwareInfo(collectTopology=True):
    """Return best-effort CPU, memory, and NUMA information without failing the benchmark."""
    system = platform.system()
    if system == "Linux":
        info = _linuxHardwareInfo()
    elif system == "Darwin":
        info = _macHardwareInfo()
    elif system == "Windows":
        info = _windowsHardwareInfo()
    else:
        info = {"cpu": {}, "memory": {}}

    if system == "Linux":
        info["numa"] = _numaInfo()
        info["dmi"] = _linuxSystemDmi()
        info["kernel"] = _kernelInfo()
        info["raw"] = _rawToolOutputs()
        if collectTopology and os.environ.get("BENCH_HWLOC", "1") != "0":
            xml = _hwlocTopology()
            if xml:
                # The raw hwloc XML contains memory serial numbers and NIC GUIDs. Only keep the
                # summary, which is what the plots and the benchmark report use.
                topology = {"source": "hwloc", "xml_bytes": len(xml.encode("utf-8"))}
                topology.update(_summarizeHwloc(xml))
                info["topology"] = topology

    try:
        affinity = sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        affinity = None
    info.setdefault("cpu", {}).setdefault("logical_cores", os.cpu_count())
    info["cpu"].setdefault("model", platform.processor() or None)
    info["cpu"].setdefault("physical_cores", None)
    info["cpu"]["affinity"] = affinity
    info["cpu"]["affinity_count"] = len(affinity) if affinity is not None else None
    info["cpu"]["affinity_numa_nodes"] = _affinityNumaNodes(affinity, info.get("numa"))
    if system == "Linux":
        info["cpu"]["vulnerabilities"] = _linuxVulnerabilities()
    info["memory"].setdefault("total_bytes", None)
    info["system"] = system
    info["machine"] = platform.machine()
    info["processor"] = platform.processor()
    info["hostname"] = platform.node()
    info["scheduler"] = _schedulerInfo()
    return info


def _usableCores(hardware):
    affinity = hardware["cpu"].get("affinity_count") or os.cpu_count() or 1
    hostLogical = hardware["cpu"].get("logical_cores") or os.cpu_count() or affinity
    hostPhysical = hardware["cpu"].get("physical_cores") or max(1, hostLogical // 2)
    smt = max(1, hostLogical // hostPhysical)
    slurmCpus = (_toInt(os.environ.get("SLURM_CPUS_PER_TASK")) or
                 _toInt(os.environ.get("SLURM_CPUS_ON_NODE")))
    if slurmCpus and slurmCpus > 0:
        if slurmCpus >= hostPhysical:
            cpuCount = min(affinity, hostLogical)
        else:
            cpuCount = min(affinity, slurmCpus * smt)
    else:
        cpuCount = affinity
    return max(1, cpuCount)


def formatTable(results, compressedSize, uncompressedSize, toolSuffix="", suffixTools=()):
    def baseTool(tool):
        if toolSuffix and tool.endswith(toolSuffix):
            base = tool[:-len(toolSuffix)]
            if base in suffixTools:
                return base
        return tool

    baseline = next((result["runtime"] for result in results
                     if baseTool(result["tool"]) == "GNU bzip2" and result["threads"] == 1), None)
    if baseline is None:
        baseline = next((result["runtime"] for result in results
                         if baseTool(result["tool"]) == "CPython.bz2" and result["threads"] == 1), None)

    def toolOrder(tool):
        tool = baseTool(tool)
        order = ("CPython.bz2", "GNU bzip2", "GNU gzip", "igzip", "libarchive", "bzip2-rs-cli",
                 "seek-bzip2", "pbzip2", "pbz2", "lbzip2-rs", "fbz", "crabz2", "mibz2", "7z",
                 "lbzip2", "ibzip2-1.7.0", "ibzip2", "rapidgzip", "rapidgzip-gzip", "indexed_bzip2")
        return order.index(tool) if tool in order else len(order)

    singleCore = sorted((result for result in results if result["threads"] == 1),
                        key=lambda result: toolOrder(result["tool"]))
    scaling = sorted((result for result in results if result["threads"] > 1),
                     key=lambda result: (toolOrder(result["tool"]), result["threads"]))

    def row(result):
        effectiveCompressedSize = result.get("compressed_size", compressedSize)
        compressed = effectiveCompressedSize / result["runtime"] / 1e6
        uncompressed = uncompressedSize / result["runtime"] / 1e6
        toolLabel = result["tool"] + (" *" if baseTool(result["tool"]) in
                                      ("GNU gzip", "igzip", "rapidgzip", "rapidgzip-gzip") else "")
        return (
            f'| {toolLabel} | {result["threads"]} | {result["runtime"]:.2f} '
            f'| {compressed:.1f} | {uncompressed:.1f} '
            f'| {("-" if baseline is None else f"{baseline / result["runtime"]:.2f}")} |'
        )

    lines = [
        "| Tool | Threads | Runtime / s | Compressed / (MB/s) | Uncompressed / (MB/s) | Speedup |",
        "|------|--------:|------------:|--------------------:|----------------------:|--------:|",
    ]
    lines.extend(row(result) for result in singleCore)
    if scaling:
        lines.append("| | | | | |")
        lines.extend(row(result) for result in scaling)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", nargs="?", help="bzip2 file to benchmark")
    parser.add_argument("--create", metavar="FILE", help="create a mixed text/base64 bzip2 file and exit")
    parser.add_argument("--size-mib", type=int, default=128, help="size of the file created by --create")
    parser.add_argument("--iterations", type=int, default=3,
                        help="timed iterations per configuration (default: 3). Roughly 7 iterations fit "
                             "an 8 hour overnight run on the full CTU archive; 10 need about 12 hours.")
    parser.add_argument("--fast-iterations", type=int, default=None,
                        help="timed iterations for indexed_bzip2, ibzip2 2.0.0, and rapidgzip "
                             "(default: 4 * --iterations). These tools are much faster than the rest, so "
                             "extra iterations reduce noise without dominating the total runtime. ibzip2 1.7.0, "
                             "pbzip2, and the reference tools keep --iterations.")
    parser.add_argument("--json-interval", type=float, default=30.0,
                        help="minimum seconds between partial JSON writes during a configuration. "
                             "Writing the full metadata after every 0.1 s iteration to a shared filesystem "
                             "can stall the next subprocess launch. 0 writes after every iteration.")
    parser.add_argument("--threads", type=int, nargs="+",
                        help="thread counts for the scaling benchmarks "
                             "(default: 1 2 4 8 12 16 20 24, filtered to the CPU count)")
    parser.add_argument("--tools", nargs="+", metavar="TOOL",
                        help="only benchmark these tools, even if others are found in PATH. "
                             "Allowed values: " + ", ".join(ALLOWED_TOOLS) + ". "
                             "rapidgzip also runs rapidgzip-gzip when a matching .gz input exists. "
                             "Quote names that contain spaces. (default: all detected tools)")
    parser.add_argument("--output", default=None,
                        help="JSON output path (default: indexed_bzip2-<YYYY-MM-DDTHH-MM>-<cpu>-<input>.json "
                             "in the current directory)")
    parser.add_argument("--ibzip2", help="path to the ibzip2 executable")
    parser.add_argument("--ibzip2-1.7.0", dest="ibzip2_1_7_0",
                        help="path to an ibzip2 1.7.0 executable (default: ibzip2-1.7.0 in PATH)")
    parser.add_argument("--rapidgzip", help="path to the rapidgzip executable (bzip2 chunk decoder)")
    parser.add_argument("--lbzip2", help="path to the lbzip2 executable")
    parser.add_argument("--bzip2", help="path to the GNU bzip2 executable")
    parser.add_argument("--gzip", help="path to the GNU gzip executable")
    parser.add_argument("--igzip", help="path to the ISA-L igzip executable")
    parser.add_argument("--gzip-file",
                        help="gzip-compressed equivalent of the bzip2 input (default: the input with .gz "
                             "instead of .bz2, if it exists)")
    parser.add_argument("--bsdcat", help="path to bsdcat from libarchive (single-core, raw decompression)")
    parser.add_argument("--pbzip2", help="path to the pbzip2 executable (measured at 1 and 2 threads)")
    parser.add_argument("--7z", dest="seven_zip", help="path to a 7-Zip executable (single-core)")
    parser.add_argument("--bzip2-rs-cli", dest="bzip2_rs_cli",
                        help="path to the bzip2-rs-cli (bzip2-cli/libbzip2-rs) executable (single-core)")
    parser.add_argument("--seek-bzip2", dest="seek_bzip2",
                        help="path to seek-bzip2's micro-bunzip full-stream decoder (single-core)")
    parser.add_argument("--pbz2", help="path to the Rust pbz2 executable (parallel)")
    parser.add_argument("--lbzip2-rs", dest="lbzip2_rs",
                        help="path to the Rust lbzip2 executable (lbunzip2, parallel)")
    parser.add_argument("--fbz", help="path to the fbz executable (parallel, multi-format)")
    parser.add_argument("--mibz2", help="path to the mibz2 benchmark driver (parallel streaming)")
    parser.add_argument("--crabz2", help="path to the crabz2 benchmark driver (parallel whole-buffer)")
    parser.add_argument("--tool-suffix", default="",
                        help="suffix for the tools named in --suffix-tools, e.g. -native")
    parser.add_argument("--suffix-tools", nargs="+", default=[],
                        help="tool names to suffix for a second, source-built comparison run")
    parser.add_argument("--source-file",
                        help="original dataset path when FILE is a staged copy in a faster filesystem")
    parser.add_argument("--hardware-json", default=None,
                        help="JSON file whose cpu, memory, numa, or topology fields override the "
                             "auto-detected hardware metadata (default: $BENCH_HARDWARE_JSON)")
    parser.add_argument("--no-topology", action="store_true",
                        help="do not collect the hwloc topology summary (also disabled by BENCH_HWLOC=0)")
    parser.add_argument("--stream", metavar="PATH", default=None,
                        help="STREAM memory bandwidth executable (default: stream in PATH)")
    parser.add_argument("--no-stream", action="store_true",
                        help="do not run the STREAM memory bandwidth benchmark")
    args = parser.parse_args()
    if args.fast_iterations is None:
        args.fast_iterations = 4 * args.iterations

    requestedTools = set(args.tools) if args.tools else None

    def baseToolName(tool):
        return tool[:-len("-native")] if tool.endswith("-native") else tool

    def wants(tool):
        return requestedTools is None or tool in requestedTools

    if args.create:
        createMixedFile(args.create, args.size_mib)
        print(f"Created {args.create} ({args.size_mib} MiB uncompressed)")
        return

    if not args.file:
        parser.error("a bzip2 file is required unless --create is given")

    path = os.path.abspath(args.file)
    if not os.path.isfile(path):
        raise SystemExit(f"error: file not found: {path}")

    ibzip2 = args.ibzip2 or shutil.which("ibzip2")
    ibzip2_1_7_0 = args.ibzip2_1_7_0 or shutil.which("ibzip2-1.7.0")
    rapidgzip = args.rapidgzip or shutil.which("rapidgzip")
    lbzip2 = args.lbzip2 or shutil.which("lbzip2")
    libarchive = args.bsdcat or shutil.which("bsdcat")
    bzip2 = args.bzip2 or shutil.which("bzip2")
    gzip = args.gzip or shutil.which("gzip")
    igzip = args.igzip or shutil.which("igzip")
    pbzip2 = args.pbzip2 or shutil.which("pbzip2")
    sevenZip = args.seven_zip or shutil.which("7z") or shutil.which("7zz") or shutil.which("7za")
    bzip2RsCli = args.bzip2_rs_cli or shutil.which("bzip2-rs-cli")
    seekBzip2 = args.seek_bzip2 or shutil.which("micro-bunzip")
    pbz2 = args.pbz2 or shutil.which("pbz2")
    lbzip2Rs = args.lbzip2_rs or shutil.which("lbzip2-rs")
    fbz = args.fbz or shutil.which("fbz")
    mibz2 = args.mibz2 or shutil.which("mibz2")
    crabz2 = args.crabz2 or shutil.which("crabz2")

    indexedBzip2ImportError = None
    try:
        import indexed_bzip2  # noqa: F401
        haveIndexedBzip2 = True
    except ImportError as error:
        indexedBzip2ImportError = error
        haveIndexedBzip2 = False

    gzipFile = None
    gzipCompressedSize = None
    if wants("GNU gzip") or wants("igzip") or wants("rapidgzip"):
        gzipFile = args.gzip_file
        if gzipFile is None and path.lower().endswith(".bz2"):
            candidate = path[:-4] + ".gz"
            if os.path.isfile(candidate):
                gzipFile = candidate
        if gzipFile is not None:
            gzipFile = os.path.abspath(gzipFile)
            if not os.path.isfile(gzipFile):
                raise SystemExit(f"error: gzip file not found: {gzipFile}")
        gzipCompressedSize = os.path.getsize(gzipFile) if gzipFile else None

    versions = getToolVersions(ibzip2, ibzip2_1_7_0, rapidgzip, lbzip2, bzip2, gzip, igzip, pbzip2,
                               bzip2RsCli, pbz2, sevenZip, libarchive, seekBzip2, haveIndexedBzip2,
                               fbz, lbzip2Rs, mibz2, crabz2)
    def versionBelongsToRequestedTool(tool):
        base = baseToolName(tool)
        if base in requestedTools:
            return True
        for suffix in (".module", ".commit"):
            if base.endswith(suffix) and base[:-len(suffix)] in requestedTools:
                return True
        return base == "rapidgzip-gzip" and "rapidgzip" in requestedTools

    if requestedTools is not None:
        versions = {tool: version for tool, version in versions.items()
                    if versionBelongsToRequestedTool(tool)}
    suffixTools = set(args.suffix_tools)

    def labelTool(tool):
        return tool + args.tool_suffix if args.tool_suffix and tool in suffixTools else tool

    versions = {labelTool(tool): version for tool, version in versions.items()}
    uncompressedSize = getUncompressedSize(path)
    compressedSize = os.path.getsize(path)
    hostCpuCount = os.cpu_count() or 1
    hardware = getHardwareInfo(collectTopology=not args.no_topology)
    hardwareJson = args.hardware_json or os.environ.get("BENCH_HARDWARE_JSON")
    if hardwareJson:
        try:
            with open(hardwareJson, "r", encoding="utf-8") as file:
                _mergeDictionary(hardware, json.load(file))
            hardware["override_file"] = sanitizePath(os.path.abspath(hardwareJson))
        except (OSError, ValueError) as error:
            print(f"warning: could not read hardware override {hardwareJson}: {error}", file=sys.stderr)

    usableCores = _usableCores(hardware)
    if not args.no_stream and not hardware.get("memory", {}).get("stream"):
        streamPath = args.stream or shutil.which("stream")
        if streamPath:
            bandwidth = _streamBandwidth(streamPath, usableCores)
            if bandwidth:
                hardware["memory"]["stream"] = bandwidth
                print(f"STREAM: copy {bandwidth.get('copy_mb_s')} MB/s, scale {bandwidth.get('scale_mb_s')} MB/s, "
                      f"add {bandwidth.get('add_mb_s')} MB/s, triad {bandwidth.get('triad_mb_s')} MB/s",
                      file=sys.stderr)
            else:
                print(f"warning: STREAM produced no parseable output: {streamPath}", file=sys.stderr)

    startState = _systemState()
    sourceFile = os.path.abspath(args.source_file) if args.source_file else path
    sourceGzipFile = gzipFile
    if gzipFile and args.source_file and sourceFile.lower().endswith(".bz2"):
        candidate = sourceFile[:-4] + ".gz"
        if os.path.isfile(candidate):
            sourceGzipFile = candidate
    if args.output is None:
        args.output = benchmarkOutputName(path, hardware)
    threads = sorted(set(args.threads or defaultThreads(usableCores)))

    # Bring the compressed file into the page cache once so that the first timed decoder is not penalized.
    with open(path, "rb") as file:
        file.read()

    results = []
    metadata = {
        # The dataset file names are enough to identify the input and its gzip counterpart in plots
        # and reports. Absolute paths would expose the directory layout and the user name.
        "file": os.path.basename(path),
        "source_file": os.path.basename(sourceFile),
        "gzip_file": os.path.basename(gzipFile) if gzipFile else None,
        "source_gzip_file": os.path.basename(sourceGzipFile) if sourceGzipFile else None,
        "gzip_compressed_size": gzipCompressedSize,
        "gzip_compression_ratio": (uncompressedSize / gzipCompressedSize) if gzipCompressedSize else None,
        "date": datetime.now().astimezone().isoformat(timespec="minutes"),
        "compressed_size": compressedSize,
        "uncompressed_size": uncompressedSize,
        "compression_ratio": uncompressedSize / compressedSize,
        "versions": versions,
        "cpu_count": hostCpuCount,
        "usable_cores": usableCores,
        "physical_cores": hardware["cpu"].get("physical_cores"),
        "logical_cores": hardware["cpu"].get("logical_cores"),
        "hardware": hardware,
        "system_state": {"start": startState},
        "platform": platform.platform(),
        "iterations": args.iterations,
        "fast_iterations": args.fast_iterations,
        "results": results,
        "complete": False,
    }
    outputDirectory = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(outputDirectory, exist_ok=True)

    def writeJson():
        """Write atomically so that an interrupted timing keeps the last complete result set."""
        temporaryPath = args.output + ".tmp"
        with open(temporaryPath, "w") as file:
            json.dump(metadata, file, indent=2)
        os.replace(temporaryPath, args.output)

    lastJsonWrite = time.monotonic()
    writeJson()

    def writeJsonThrottled(force=False):
        nonlocal lastJsonWrite
        if not force and args.json_interval > 0:
            now = time.monotonic()
            if now - lastJsonWrite < args.json_interval:
                return
        lastJsonWrite = time.monotonic()
        writeJson()

    def add(tool, threadCount, benchmarkFunction, resultCompressedSize=None):
        if tool == "rapidgzip-gzip" and resultCompressedSize is None:
            resultCompressedSize = gzipCompressedSize
        tool = labelTool(tool)
        entry = {
            "tool": tool,
            "threads": threadCount,
            "runtime": None,
            "runtimes": [],
            "compressed_size": (compressedSize if resultCompressedSize is None
                                else resultCompressedSize),
        }
        results.append(entry)

        def progress(runtimes):
            entry["runtimes"] = list(runtimes)
            entry["runtime"] = statistics.median(runtimes)
            writeJsonThrottled()

        measurement = benchmarkFunction(progress)
        if measurement is None:
            results.pop()
            writeJsonThrottled(force=True)
            print(f"  Skipped {tool} P{threadCount}: benchmark failed", file=sys.stderr)
            return
        runtime, runtimes = measurement
        entry["runtime"] = runtime
        entry["runtimes"] = runtimes
        writeJsonThrottled(force=True)
        fastest = min(runtimes)
        slowest = max(runtimes)
        # A single launch stall must not make the whole configuration look unstable. Compare the
        # median with the fastest run, which detects a sustained slowdown but ignores one outlier.
        spread = runtime / fastest if fastest > 0 else float("inf")
        print(f"  {tool} P{threadCount}: {runtime:.2f} s median of {len(runtimes)} "
              f"(min {fastest:.2f}, max {slowest:.2f}, median/min {spread:.2f}x)", file=sys.stderr)
        if spread > 1.15:
            print(f"    WARNING: the median is {spread:.2f}x the fastest run; the node was not quiet",
                  file=sys.stderr)

    print(f"File: {path}", file=sys.stderr)
    print(f"Compressed size: {compressedSize / 1e6:.1f} MB, uncompressed size: "
          f"{uncompressedSize / 1e6:.1f} MB, ratio: "
          f"{uncompressedSize / compressedSize:.1f}", file=sys.stderr)
    print(f"Iterations: {args.iterations}, fast iterations: {args.fast_iterations}, "
          f"CPU count: {usableCores} usable of {hostCpuCount} logical", file=sys.stderr)
    print(f"CPU: {hardware['cpu'].get('model')}, {hardware['cpu'].get('physical_cores')} physical / "
          f"{hardware['cpu'].get('logical_cores')} logical cores, "
          f"{hardware['cpu'].get('affinity_count')} in affinity mask", file=sys.stderr)
    memory = hardware["memory"]
    print(f"Memory: {_formatBytes(memory.get('total_bytes'))} total, "
          f"{_formatBytes(memory.get('free_bytes'))} free, "
          f"{_formatBytes(memory.get('available_bytes'))} available, "
          f"{_formatBytes(memory.get('swap_free_bytes'))}/"
          f"{_formatBytes(memory.get('swap_total_bytes'))} swap free/total, "
          f"{memory.get('type') or 'unknown type'}, {memory.get('speed_mts') or '?'} MT/s"
          f"{' (' + memory['hardware_source'] + ')' if memory.get('hardware_source') else ''}", file=sys.stderr)
    numa = hardware.get("numa") or {}
    if numa.get("nodes"):
        nodeSummary = ", ".join(
            f"node{node['id']}: {_formatBytes(node.get('size_bytes'))}, cpus {node.get('cpus')}"
            for node in numa["nodes"])
        print(f"NUMA: {numa.get('node_count')} node(s) from {numa.get('source')}: {nodeSummary}", file=sys.stderr)
    topology = hardware.get("topology") or {}
    if topology.get("packages"):
        print(f"Topology: {topology.get('packages')} package(s), {topology.get('numa_nodes')} NUMA node(s), "
              f"{topology.get('cores')} core(s), {topology.get('processing_units')} PU(s)", file=sys.stderr)
    for name, version in versions.items():
        print(f"{name}: {version}", file=sys.stderr)

    parallelTools = []
    if wants("indexed_bzip2"):
        if haveIndexedBzip2:
            parallelTools.append((
                "indexed_bzip2",
                lambda threadCount, progress: benchmarkIndexedBzip2(
                    path, threadCount, args.fast_iterations, progress)))
        else:
            print(f"Skipping indexed_bzip2: import failed: {indexedBzip2ImportError}", file=sys.stderr)

    if wants("ibzip2"):
        if ibzip2:
            # -d always decodes, also when stdout is /dev/null. Do not add -t: the 1.7.0 baseline's
            # -t runs an extra checkOffsets pass, which would make the two measurements incomparable.
            parallelTools.append((
                "ibzip2",
                lambda threadCount, progress: benchmarkCommand(
                    [ibzip2, "-d", "-c", "-f", "-q", "-P", str(threadCount), path], args.fast_iterations,
                    progress=progress)))
        else:
            print("Skipping ibzip2: executable not found", file=sys.stderr)

    if wants("rapidgzip"):
        if rapidgzip:
            parallelTools.append((
                "rapidgzip",
                lambda threadCount, progress: benchmarkCommand(
                    [rapidgzip, "-d", "-c", "-q", "-P", str(threadCount), path],
                    args.fast_iterations, progress=progress)))
        else:
            print("Skipping rapidgzip: executable not found", file=sys.stderr)

    if wants("rapidgzip") and gzipFile:
        parallelTools.append((
            "rapidgzip-gzip",
            lambda threadCount, progress: benchmarkCommand(
                [rapidgzip, "-d", "-c", "-q", "-P", str(threadCount), gzipFile],
                args.fast_iterations, progress=progress)))

    if wants("ibzip2-1.7.0"):
        if ibzip2_1_7_0:
            parallelTools.append((
                "ibzip2-1.7.0",
                lambda threadCount, progress: benchmarkCommand(
                    [ibzip2_1_7_0, "-d", "-c", "-f", "-q", "-P", str(threadCount), path], args.iterations,
                    progress=progress)))
        else:
            print("Skipping ibzip2-1.7.0: executable not found", file=sys.stderr)

    if wants("pbz2"):
        if pbz2:
            parallelTools.append((
                "pbz2",
                lambda threadCount, progress: benchmarkCommand(
                    [pbz2, "-d", "-c", "-q", "-n", str(threadCount), path], args.iterations,
                    progress=progress)))
        else:
            print("Skipping pbz2: executable not found", file=sys.stderr)

    if wants("lbzip2-rs"):
        if lbzip2Rs:
            parallelTools.append((
                "lbzip2-rs",
                lambda threadCount, progress: benchmarkCommand(
                    [lbzip2Rs, path, "/dev/null"], args.iterations, progress=progress,
                    env={"LBZIP2_THREADS": str(threadCount)})))
        else:
            print("Skipping lbzip2-rs: executable not found", file=sys.stderr)

    if wants("fbz"):
        if fbz:
            parallelTools.append((
                "fbz",
                lambda threadCount, progress: benchmarkCommand(
                    [fbz, "d", "-q", "-P", str(threadCount), "-o", "-", path], args.iterations,
                    progress=progress)))
        else:
            print("Skipping fbz: executable not found", file=sys.stderr)

    if wants("mibz2"):
        if mibz2:
            parallelTools.append((
                "mibz2",
                lambda threadCount, progress: benchmarkCommand(
                    [mibz2, str(threadCount), path], args.iterations, progress=progress)))
        else:
            print("Skipping mibz2: executable not found", file=sys.stderr)

    if wants("crabz2"):
        if crabz2:
            if uncompressedSize > CRABZ2_MAX_OUTPUT_BYTES:
                print(f"Skipping crabz2: its whole-buffer API needs {uncompressedSize / 1e9:.1f} GB "
                      f"of plaintext, above the {CRABZ2_MAX_OUTPUT_BYTES / 1e9:.0f} GB limit; "
                      "use a streaming tool on this input", file=sys.stderr)
            else:
                parallelTools.append((
                    "crabz2",
                    lambda threadCount, progress: benchmarkCommand(
                        [crabz2, str(threadCount), path], args.iterations, progress=progress)))
        else:
            print("Skipping crabz2: executable not found", file=sys.stderr)

    if wants("lbzip2"):
        if lbzip2:
            parallelTools.append((
                "lbzip2",
                lambda threadCount, progress: benchmarkCommand(
                    [lbzip2, "-dc", "-n", str(threadCount), path], args.iterations,
                    progress=progress)))
        else:
            print("Skipping lbzip2: executable not found", file=sys.stderr)

    # Run the most interesting parallel configurations first, interleaved by tool, so that partial
    # results are available early. The single-core-only tools are the slowest and come last.
    for threadCount in sorted((threadCount for threadCount in threads if threadCount > 1), reverse=True):
        for tool, benchmark in parallelTools:
            print(f"Benchmarking {tool} with {threadCount} threads ...", file=sys.stderr)
            add(tool, threadCount, lambda progress: benchmark(threadCount, progress))

    if 1 in threads:
        for tool, benchmark in parallelTools:
            print(f"Benchmarking {tool} with 1 thread ...", file=sys.stderr)
            add(tool, 1, lambda progress: benchmark(1, progress))

    if wants("CPython.bz2"):
        print("Benchmarking CPython.bz2 ...", file=sys.stderr)
        add("CPython.bz2", 1, lambda progress: benchmarkPythonBz2(path, args.iterations, progress))

    if wants("GNU bzip2"):
        if bzip2:
            print("Benchmarking GNU bzip2 ...", file=sys.stderr)
            add("GNU bzip2", 1, lambda progress: benchmarkCommand([bzip2, "-dc", path], args.iterations,
                                                                     progress=progress))
        else:
            print("Skipping GNU bzip2: executable not found", file=sys.stderr)

    if wants("GNU gzip"):
        if gzip and gzipFile:
            print("Benchmarking GNU gzip ...", file=sys.stderr)
            add("GNU gzip", 1, lambda progress: benchmarkCommand([gzip, "-dc", gzipFile], args.iterations,
                                                                     progress=progress), gzipCompressedSize)
        elif gzip:
            print("Skipping GNU gzip: no gzip file given or found next to the input", file=sys.stderr)
        else:
            print("Skipping GNU gzip: executable not found", file=sys.stderr)

    if wants("igzip"):
        if igzip and gzipFile:
            print("Benchmarking igzip ...", file=sys.stderr)
            add("igzip", 1, lambda progress: benchmarkCommand([igzip, "-d", "-c", gzipFile], args.iterations,
                                                                  progress=progress), gzipCompressedSize)
        elif igzip:
            print("Skipping igzip: no gzip file given or found next to the input", file=sys.stderr)
        else:
            print("Skipping igzip: executable not found", file=sys.stderr)

    if wants("libarchive"):
        if libarchive:
            print("Benchmarking libarchive (bsdcat) ...", file=sys.stderr)
            add("libarchive", 1, lambda progress: benchmarkCommand([libarchive, path], args.iterations,
                                                                       progress=progress))
        else:
            print("Skipping libarchive: bsdcat not found", file=sys.stderr)

    if wants("bzip2-rs-cli"):
        if bzip2RsCli:
            print("Benchmarking bzip2-rs-cli with 1 thread ...", file=sys.stderr)
            add("bzip2-rs-cli", 1, lambda progress: benchmarkCommand([bzip2RsCli, "-d", "-c", path], args.iterations,
                                                                             progress=progress))
        else:
            print("Skipping bzip2-rs-cli: executable not found", file=sys.stderr)

    if wants("seek-bzip2"):
        if seekBzip2:
            print("Benchmarking seek-bzip2 (micro-bunzip) with 1 thread ...", file=sys.stderr)
            add("seek-bzip2", 1, lambda progress: benchmarkCommand([seekBzip2], args.iterations, stdinPath=path,
                                                                          progress=progress))
        else:
            print("Skipping seek-bzip2: micro-bunzip not found", file=sys.stderr)

    if wants("pbzip2"):
        if pbzip2:
            print("Benchmarking pbzip2 with 1 thread ...", file=sys.stderr)
            add("pbzip2", 1, lambda progress: benchmarkCommand(
                [pbzip2, "-d", "-c", "-p1", path], args.iterations, progress=progress))
            if 2 in threads:
                print("Benchmarking pbzip2 with 2 threads ...", file=sys.stderr)
                add("pbzip2", 2, lambda progress: benchmarkCommand(
                    [pbzip2, "-d", "-c", "-p2", path], args.iterations, progress=progress))
        else:
            print("Skipping pbzip2: executable not found", file=sys.stderr)

    if wants("7z"):
        if sevenZip:
            print("Benchmarking 7-Zip with 1 thread ...", file=sys.stderr)
            add("7z", 1, lambda progress: benchmarkCommand([sevenZip, "e", "-so", "-bd", "-y", "-mmt=1", path],
                                                          args.iterations, progress=progress))
        else:
            print("Skipping 7-Zip: executable not found", file=sys.stderr)

    table = formatTable(results, compressedSize, uncompressedSize, args.tool_suffix, args.suffix_tools)
    print()
    print(table)
    if gzipFile and gzipCompressedSize:
        print(f"\n\\* GNU gzip, igzip, and rapidgzip-gzip decode `{os.path.basename(gzipFile)}` with a "
              f"compression ratio of {uncompressedSize / gzipCompressedSize:.1f} instead of the bzip2 ratio "
              f"{uncompressedSize / compressedSize:.1f}. Their compressed MB/s uses the gzip size, so compare "
              f"the uncompressed bandwidth.")
    if any(result["tool"] == "rapidgzip" for result in results):
        print("\n\\* rapidgzip uses its bzip2 chunk decoder; gzip is its primary format.")

    metadata["system_state"]["end"] = _systemState()
    metadata["complete"] = True
    writeJson()
    print(f"\nWrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
