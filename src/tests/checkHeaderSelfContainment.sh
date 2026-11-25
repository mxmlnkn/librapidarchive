#!/usr/bin/env bash

# Public headers must compile as the first include of a translation unit. This catches missing includes and
# collisions with platform macros, for example BYTE_SIZE, which the macOS SDK defines in <mach/vm_param.h>
# and pulls in through <malloc/malloc.h>.


set -euo pipefail

repoRoot=$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )
compiler=${CXX:-c++}
tempDir=$( mktemp -d )
trap 'rm -rf "$tempDir"' EXIT

cd -- "$repoRoot"

jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

git ls-tree -r -z --name-only HEAD | 'grep' -z -E '[.]hpp$' |
    'grep' -zvE '(Bzip2SymbolKernel|ISAL|isal|Python|ScopedGIL|precode)[.]hpp' |
    xargs -0 -n 1 -P "$jobs" bash -c '
        repoRoot=$1
        tempDir=$2
        compiler=$3
        path=$4

        header=${path#src/}
        source="$tempDir/${header//\//_}.cpp"

        printf "#include <%s>\nint main() { return 0; }\n" "$header" > "$source"

        if ! "$compiler" -std=c++17 -fsyntax-only \
            -I"$repoRoot/src" \
            -I"$repoRoot/src/external/cxxopts/include" \
            -I"$repoRoot/src/external/libsais/include" \
            "$source"
        then
            echo "Header is not self-contained: $header" >&2
            exit 1
        fi
    ' _ "$repoRoot" "$tempDir" "$compiler"
