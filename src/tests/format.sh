#!/usr/bin/env bash
# Format C/C++ sources with uncrustify.
#
# Usage:
#   src/tests/format.sh [changed|all]
#
# "changed" formats all C/C++ files that differ from HEAD, or, if the working
# tree is clean, the files touched by the current commit. "all" formats all
# tracked C/C++ sources.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
git_cmd=${GIT:-git}
repo_root=$("$git_cmd" -C "$script_dir" rev-parse --show-toplevel)
config="${repo_root}/src/uncrustify.cfg"
uncrustify=${UNCRUSTIFY:-uncrustify}
mode=${1:-changed}

if ! command -v "$uncrustify" >/dev/null 2>&1; then
    echo "error: uncrustify not found; install version 0.78.1 or newer" >&2
    exit 1
fi

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

# Refuse to run with an uncrustify that silently ignores options. Older
# versions only warn about unknown options and otherwise format with a
# different style.
: > "${tmp_dir}/empty.cpp"
if ! config_messages=$("$uncrustify" -c "$config" --no-backup -f "${tmp_dir}/empty.cpp" 2>&1 >/dev/null); then
    echo "error: uncrustify could not parse ${config}" >&2
    echo "$config_messages" >&2
    exit 1
fi
if grep -q 'unknown option' <<< "$config_messages"; then
    echo "error: the installed uncrustify does not support all options in ${config}" >&2
    echo "       install version 0.78.1 or newer" >&2
    grep 'unknown option' <<< "$config_messages" >&2
    exit 1
fi

candidates=()
if [ "$mode" = all ]; then
    while IFS= read -r -d '' file; do
        candidates+=("$file")
    done < <("$git_cmd" -C "$repo_root" ls-files -z)
else
    while IFS= read -r -d '' file; do
        candidates+=("$file")
    done < <("$git_cmd" -C "$repo_root" diff --name-only --diff-filter=ACMR -z HEAD)

    if [ ${#candidates[@]} -eq 0 ]; then
        while IFS= read -r -d '' file; do
            candidates+=("$file")
        done < <("$git_cmd" -C "$repo_root" show --pretty= --name-only --diff-filter=ACMR -z HEAD)
    fi
fi

files=()
for file in "${candidates[@]}"; do
    case "$file" in
        src/external/*) continue ;;
    esac
    case "$file" in
        *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx) ;;
        *) continue ;;
    esac
    [ -f "${repo_root}/${file}" ] || continue
    files+=("$file")
done

if [ ${#files[@]} -eq 0 ]; then
    echo "No C/C++ files to format."
    exit 0
fi

echo "Formatting ${#files[@]} file(s) with uncrustify:"
printf '  %s\n' "${files[@]}"
"$uncrustify" -c "$config" --no-backup -q "${files[@]/#/${repo_root}/}"
