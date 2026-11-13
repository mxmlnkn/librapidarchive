#!/usr/bin/env bash
# Create the .gz and .bz2 inputs used by show-benchmark-2x2.sh.
#
# The full profile uses silesia-20x.gz for the gzip tools and silesia-10x.bz2
# for the bzip2 tools. If those files are not available, it repeats a local
# silesia.tar to the same sizes and compresses the result. The fast profile
# truncates the corpus to a short input used for both formats.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z $REPO_ROOT ]]; then
    REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
fi

DATA_DIR=${DEMO_DATA_DIR:-$REPO_ROOT/tmp/asciinema-demo}
PROFILE=full
INPUT=
SIZE_MIB=
FORCE=0

usage() {
    cat <<'EOF'
Usage: prepare-demo.sh [OPTION]...

Create the gzip and bzip2 inputs for the 2x2 demo.

Options:
  --fast              Prepare a small input for a dry run (default 16 MiB).
  --size-mib N        Truncate the input to N MiB. Implies a generated raw file.
  --input FILE        Use FILE as the corpus. May be .tar, .tar.gz, or .tar.bz2.
  --data-dir DIR      Write generated files to DIR (default: <repo>/tmp/asciinema-demo).
  --force             Recreate everything even if the inputs are up to date.
  -h, --help          Show this help.

The full profile prefers silesia-20x.gz and silesia-10x.bz2 below test-files/
or tmp/. If they are missing, it repeats silesia.tar 20 times for the gzip
input and 10 times for the bzip2 input, then compresses those raw files. The
fast profile uses the same raw input for both formats so a dry run stays small.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

while (( $# > 0 )); do
    case $1 in
        --fast)
            PROFILE=fast
            ;;
        --size-mib)
            [[ $# -ge 2 ]] || die "--size-mib needs a value"
            SIZE_MIB=$2
            shift
            ;;
        --input)
            [[ $# -ge 2 ]] || die "--input needs a path"
            INPUT=$2
            shift
            ;;
        --data-dir)
            [[ $# -ge 2 ]] || die "--data-dir needs a path"
            DATA_DIR=$2
            shift
            ;;
        --force)
            FORCE=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1 (try --help)"
            ;;
    esac
    shift
done

if [[ -z $SIZE_MIB && $PROFILE == fast ]]; then
    SIZE_MIB=16
fi
DATA_DIR=$(realpath -m -- "$DATA_DIR")
if [[ -n $SIZE_MIB && ! $SIZE_MIB =~ ^[1-9][0-9]*$ ]]; then
    die "--size-mib must be a positive integer"
fi

command -v gzip >/dev/null || die "gzip is required"
command -v bzip2 >/dev/null || die "bzip2 is required"

if [[ $PROFILE == fast ]]; then
    GZ_NAME=silesia-demo-small.tar.gz
    BZ2_NAME=silesia-demo-small.tar.bz2
else
    GZ_NAME=silesia-20x.gz
    BZ2_NAME=silesia-10x.bz2
fi
GZ=$DATA_DIR/$GZ_NAME
BZ2=$DATA_DIR/$BZ2_NAME
MANIFEST=$DATA_DIR/manifest-$PROFILE.env

mkdir -p -- "$DATA_DIR"
TMP_DIR=$(mktemp -d "$DATA_DIR/.prepare.XXXXXX")
cleanup() {
    rm -rf -- "$TMP_DIR"
}
trap cleanup EXIT

findFirst() {
    local candidate
    for candidate in "$@"; do
        if [[ -f $candidate ]]; then
            realpath -- "$candidate"
            return 0
        fi
    done
    return 1
}

sourceType() {
    case $1 in
        *.tar) printf 'tar\n' ;;
        *.gz|*.tgz) printf 'gzip\n' ;;
        *.bz2|*.tbz2) printf 'bzip2\n' ;;
        *) die "unsupported input suffix: $1 (expected .tar, .gz, or .bz2)" ;;
    esac
}

makeRepeat() {
    local source=$1 count=$2 target=$3 index expected
    expected=$(( $(stat -c %s -- "$source") * count ))
    if (( ! FORCE )) && [[ -f $target && $(stat -c %s -- "$target") -eq $expected ]]; then
        return
    fi
    printf 'repeating %s %s times -> %s (%s bytes)\n' "$source" "$count" "$target" "$expected"
    : > "$target"
    for (( index = 0; index < count; index++ )); do
        cat -- "$source" >> "$target"
    done
}

compressTo() {
    local source=$1 output=$2 kind=$3 temporary
    if (( ! FORCE )) && [[ -f $output && $output -nt $source ]]; then
        return
    fi
    temporary=$TMP_DIR/$(basename -- "$output")
    printf 'compressing %s -> %s\n' "$source" "$output"
    case $kind in
        gzip) gzip -9 -n -c -- "$source" > "$temporary" ;;
        bzip2) bzip2 -9 -c -- "$source" > "$temporary" ;;
    esac
    mv -f -- "$temporary" "$output"
}

decompressedSize() {
    local source=$1 kind=$2 cache size
    cache=$DATA_DIR/.size-$(basename -- "$source")-$(stat -c %s -- "$source")-$(stat -c %Y -- "$source")
    if [[ -f $cache ]]; then
        cat -- "$cache"
        return
    fi
    case $kind in
        gzip) size=$(gzip -dc -- "$source" | wc -c) ;;
        bzip2) size=$(bzip2 -dc -- "$source" | wc -c) ;;
    esac
    printf '%s\n' "$size" > "$cache"
    printf '%s\n' "$size"
}

prepareSingleSource() {
    local source sourceKind targetBytes raw needsRaw=0
    if [[ -n $INPUT ]]; then
        [[ -f $INPUT ]] || die "input does not exist: $INPUT"
        source=$(realpath -- "$INPUT")
    else
        source=$(findFirst \
            "$REPO_ROOT/test-files/silesia.tar" \
            "$REPO_ROOT/test-files/silesia/silesia.tar" \
            "$REPO_ROOT/tmp/silesia/silesia.tar" \
            "$REPO_ROOT/test-files/silesia.tar.gz" \
            "$REPO_ROOT/tmp/silesia/silesia.tar.gz" \
            "$REPO_ROOT/test-files/silesia.tar.bz2" \
            "$REPO_ROOT/test-files/silesia/silesia.tar.bz2" \
            "$REPO_ROOT/tmp/silesia/silesia.tar.bz2") \
            || die "no silesia corpus found; pass --input or put test-files/silesia.tar in place"
    fi
    sourceKind=$(sourceType "$source")
    targetBytes=
    if [[ -n $SIZE_MIB ]]; then
        targetBytes=$(( SIZE_MIB * 1024 * 1024 ))
    fi

    if [[ $PROFILE == fast ]]; then
        raw=$DATA_DIR/silesia-demo-small.tar
    else
        raw=$DATA_DIR/silesia-demo.tar
    fi

    if [[ $sourceKind == tar && -z $targetBytes ]]; then
        # The corpus is already a raw file. Reuse it instead of copying it.
        raw=$source
    else
        if (( FORCE )); then
            needsRaw=1
        elif [[ ! -f $raw || $source -nt $raw ]]; then
            needsRaw=1
        elif [[ -n $targetBytes ]] && [[ $(stat -c %s -- "$raw") -ne $targetBytes ]]; then
            needsRaw=1
        fi
        if (( needsRaw )); then
            if [[ -n $targetBytes ]]; then
                case $sourceKind in
                    tar) head -c "$targetBytes" -- "$source" > "$TMP_DIR/raw" ;;
                    gzip)
                        set +o pipefail
                        gzip -dc -- "$source" | head -c "$targetBytes" > "$TMP_DIR/raw"
                        set -o pipefail
                        ;;
                    bzip2)
                        set +o pipefail
                        bzip2 -dc -- "$source" | head -c "$targetBytes" > "$TMP_DIR/raw"
                        set -o pipefail
                        ;;
                esac
            else
                case $sourceKind in
                    gzip) gzip -dc -- "$source" > "$TMP_DIR/raw" ;;
                    bzip2) bzip2 -dc -- "$source" > "$TMP_DIR/raw" ;;
                esac
            fi
            mv -f -- "$TMP_DIR/raw" "$raw"
        fi
    fi

    [[ -s $raw ]] || die "failed to create $raw"
    GZ_SIZE=$(stat -c %s -- "$raw")
    BZ2_SIZE=$GZ_SIZE
    GZ_SOURCE=$raw
    BZ2_SOURCE=$raw
    compressTo "$raw" "$GZ" gzip
    compressTo "$raw" "$BZ2" bzip2
}

prepareLargeSources() {
    local corpus gzipSource bzip2Source gzipRaw bzip2Raw
    gzipSource=$(findFirst \
        "$REPO_ROOT/test-files/silesia-20x.gz" \
        "$REPO_ROOT/test-files/silesia/silesia-20x.gz" \
        "$REPO_ROOT/tmp/silesia/silesia-20x.gz" \
        "$REPO_ROOT/test-files/silesia-20x.tar.gz" \
        "$REPO_ROOT/test-files/silesia/silesia-20x.tar.gz" \
        "$REPO_ROOT/tmp/silesia/silesia-20x.tar.gz") || gzipSource=
    bzip2Source=$(findFirst \
        "$REPO_ROOT/test-files/silesia-10x.bz2" \
        "$REPO_ROOT/test-files/silesia/silesia-10x.bz2" \
        "$REPO_ROOT/tmp/silesia/silesia-10x.bz2" \
        "$REPO_ROOT/test-files/silesia-10x.tar.bz2" \
        "$REPO_ROOT/test-files/silesia/silesia-10x.tar.bz2" \
        "$REPO_ROOT/tmp/silesia/silesia-10x.tar.bz2") || bzip2Source=

    if [[ -z $gzipSource || -z $bzip2Source ]]; then
        corpus=$(findFirst \
            "$REPO_ROOT/test-files/silesia.tar" \
            "$REPO_ROOT/test-files/silesia/silesia.tar" \
            "$REPO_ROOT/tmp/silesia/silesia.tar" \
            "$REPO_ROOT/test-files/silesia.tar.gz" \
            "$REPO_ROOT/tmp/silesia/silesia.tar.gz" \
            "$REPO_ROOT/test-files/silesia.tar.bz2" \
            "$REPO_ROOT/test-files/silesia/silesia.tar.bz2" \
            "$REPO_ROOT/tmp/silesia/silesia.tar.bz2") \
            || die "no silesia-20x.gz, silesia-10x.bz2, or silesia.tar found"
        case $(sourceType "$corpus") in
            gzip)
                gzip -dc -- "$corpus" > "$DATA_DIR/silesia.tar"
                corpus=$DATA_DIR/silesia.tar
                ;;
            bzip2)
                bzip2 -dc -- "$corpus" > "$DATA_DIR/silesia.tar"
                corpus=$DATA_DIR/silesia.tar
                ;;
            tar) ;;
        esac
    fi

    if [[ -n $gzipSource ]]; then
        case $(sourceType "$gzipSource") in
            gzip)
                GZ=$gzipSource
                GZ_SIZE=$(decompressedSize "$gzipSource" gzip)
                ;;
            tar)
                GZ=$DATA_DIR/$GZ_NAME
                GZ_SIZE=$(stat -c %s -- "$gzipSource")
                compressTo "$gzipSource" "$GZ" gzip
                ;;
        esac
        GZ_SOURCE=$gzipSource
    else
        gzipRaw=$DATA_DIR/${GZ_NAME%.gz}.tar
        makeRepeat "$corpus" 20 "$gzipRaw"
        GZ=$DATA_DIR/$GZ_NAME
        GZ_SIZE=$(stat -c %s -- "$gzipRaw")
        GZ_SOURCE=$gzipRaw
        compressTo "$gzipRaw" "$GZ" gzip
    fi

    if [[ -n $bzip2Source ]]; then
        case $(sourceType "$bzip2Source") in
            bzip2)
                BZ2=$bzip2Source
                BZ2_SIZE=$(decompressedSize "$bzip2Source" bzip2)
                ;;
            tar)
                BZ2=$DATA_DIR/$BZ2_NAME
                BZ2_SIZE=$(stat -c %s -- "$bzip2Source")
                compressTo "$bzip2Source" "$BZ2" bzip2
                ;;
        esac
        BZ2_SOURCE=$bzip2Source
    else
        bzip2Raw=$DATA_DIR/${BZ2_NAME%.bz2}.tar
        makeRepeat "$corpus" 10 "$bzip2Raw"
        BZ2=$DATA_DIR/$BZ2_NAME
        BZ2_SIZE=$(stat -c %s -- "$bzip2Raw")
        BZ2_SOURCE=$bzip2Raw
        compressTo "$bzip2Raw" "$BZ2" bzip2
    fi
}

if [[ $PROFILE == full && -z $INPUT && -z $SIZE_MIB ]]; then
    prepareLargeSources
else
    prepareSingleSource
fi

# Keep both inputs in the data directory so the demo can use their basenames
# from a single working directory even when the originals live elsewhere.
if [[ $(dirname -- "$GZ") != "$DATA_DIR" ]]; then
    ln -sfn -- "$GZ" "$DATA_DIR/$GZ_NAME"
    GZ=$DATA_DIR/$GZ_NAME
fi
if [[ $(dirname -- "$BZ2") != "$DATA_DIR" ]]; then
    ln -sfn -- "$BZ2" "$DATA_DIR/$BZ2_NAME"
    BZ2=$DATA_DIR/$BZ2_NAME
fi

for path in "$GZ" "$BZ2"; do
    [[ -f $path ]] || die "failed to create $path"
done
[[ $GZ_SIZE =~ ^[0-9]+$ ]] || die "could not determine the gzip input size"
[[ $BZ2_SIZE =~ ^[0-9]+$ ]] || die "could not determine the bzip2 input size"

{
    printf 'DEMO_PROFILE=%q\n' "$PROFILE"
    printf 'DEMO_GZ=%q\n' "$GZ"
    printf 'DEMO_BZ2=%q\n' "$BZ2"
    printf 'DEMO_GZ_SIZE=%q\n' "$GZ_SIZE"
    printf 'DEMO_BZ2_SIZE=%q\n' "$BZ2_SIZE"
    printf 'DEMO_GZ_SOURCE=%q\n' "$GZ_SOURCE"
    printf 'DEMO_BZ2_SOURCE=%q\n' "$BZ2_SOURCE"
    if [[ $GZ_SIZE == "$BZ2_SIZE" ]]; then
        printf 'DEMO_SIZE=%q\n' "$GZ_SIZE"
    fi
} > "$TMP_DIR/manifest.env"
mv -f -- "$TMP_DIR/manifest.env" "$MANIFEST"

printf 'prepared %s\n' "$MANIFEST"
printf '  gzip:   %s (%s bytes)\n' "$GZ" "$GZ_SIZE"
printf '  bzip2:  %s (%s bytes)\n' "$BZ2" "$BZ2_SIZE"
