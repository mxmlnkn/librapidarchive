#!/usr/bin/env bash

# Public headers must compile as the first include of a translation unit. This catches missing includes and
# collisions with platform macros, for example BYTE_SIZE, which the macOS SDK defines in <mach/vm_param.h>
# and pulls in through <malloc/malloc.h>.


set -euo pipefail

repoRoot=$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )
compiler=${CXX:-c++}
tempDir=$( mktemp -d )
trap 'rm -rf "$tempDir"' EXIT

cd "$repoRoot"

status=0
for path in src/rapidgzip/*.hpp src/indexed_bzip2/*.hpp; do
    header=${path#src/}
    # Bzip2SymbolKernel.hpp is a fragment that Bzip2SymbolDecoder.hpp includes once per kernel variant.
    if [ "$header" = "indexed_bzip2/Bzip2SymbolKernel.hpp" ]; then
        continue
    fi
    source="$tempDir/${header//\//_}.cpp"
    printf '#include <%s>\nint main() { return 0; }\n' "$header" > "$source"
    if ! "$compiler" -std=c++17 -fsyntax-only -I"$repoRoot/src" "$source"; then
        echo "Header is not self-contained: $header" >&2
        status=1
    fi
done

exit "$status"
