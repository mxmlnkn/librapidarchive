#!/usr/bin/env bash
# Regression test for rapidgzip#39: a delayed pipe reader must not observe pages that the writer has already
# freed. This only exercises the vmsplice path when built with LIBRAPIDARCHIVE_WITH_VMSPLICE=ON.
set -euo pipefail

RAPIDGZIP=${1:?usage: testVmspliceDelayedReader.sh RAPIDGZIP}
workDirectory=$(mktemp -d)
trap 'rm -rf -- "$workDirectory"' EXIT

# Keep the file small enough that the writer exits while the whole output is still in the pipe.
head -c 32768 /dev/urandom > "$workDirectory/input"
gzip -c "$workDirectory/input" > "$workDirectory/input.gz"

expectedHash=$(sha256sum "$workDirectory/input" | cut -d' ' -f1)
actualHash=$("$RAPIDGZIP" -d -c "$workDirectory/input.gz" | (sleep 2; cat) | sha256sum | cut -d' ' -f1)

if [[ "$actualHash" != "$expectedHash" ]]; then
    echo "vmsplice delayed-reader mismatch: expected $expectedHash, got $actualHash" >&2
    exit 1
fi
