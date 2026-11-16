#!/usr/bin/env bash

set -euo pipefail

cd -- "$(dirname "${BASH_SOURCE[0]}")"

export MPLBACKEND=Agg

data=data-2026-11-16

# HPC only has CTU datasets benchmarked because Silesia-20x is too small to scale. Would require weak-scaling.
python3 plotBenchmarkIndexedBzip2.py --log-log --no-show --ignore-host "$data/Xeon-Platinum-8470-CTU-13.zip"
python3 plotBenchmarkIndexedBzip2.py --log-log --no-show --ignore-host "$data/EPYC-7702-CTU-13.zip"

for zip in "$data"/Ryzen-*.zip; do
    python3 plotBenchmarkIndexedBzip2.py --log-log --no-show "$zip"
done
