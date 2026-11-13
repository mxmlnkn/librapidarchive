#!/usr/bin/env bash
# Build pv from source and install it below PREFIX.
set -euo pipefail

PV_VERSION=${PV_VERSION:-1.12.7}
PV_SHA256=${PV_SHA256:-}
if [[ -z $PV_SHA256 && $PV_VERSION == 1.12.7 ]]; then
    PV_SHA256=5326ed7ee95ef18039390c0b17891ac28985ac7c7a4235070fab767c1fe37bd8
fi
PREFIX=${PREFIX:-$HOME/.local}
BUILD_DIR=${BUILD_DIR:-$HOME/tmp/pv-$PV_VERSION}
JOBS=${JOBS:-8}
KEEP=0
CHECK=1
FORCE=0

usage() {
    cat <<'EOF'
Usage: install-pv.sh [OPTION]...

Download and build pv from ivarch.com, then install it below PREFIX.
The defaults install pv 1.12.7 into ~/.local, which is the version needed
for the zero-copy pv -X discard path.

Options:
  --prefix DIR       Installation prefix (default: ~/.local).
  --build-dir DIR    Source and build directory (default: ~/tmp/pv-VERSION).
  --jobs N           Parallel build jobs (default: 8).
  --no-check         Skip make check.
  --keep             Keep the downloaded source and build tree.
  --force            Rebuild even if PREFIX/bin/pv already has VERSION.
  -h, --help         Show this help.

Environment:
  PV_VERSION         Version to build (default: 1.12.7).
  PV_SHA256          Expected source SHA-256. Pinned for 1.12.7.
  PV_URL             Source URL override.
  PREFIX, BUILD_DIR, JOBS as above.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

while (( $# > 0 )); do
    case $1 in
        --prefix)
            [[ $# -ge 2 ]] || die "--prefix needs a path"
            PREFIX=$2
            shift
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir needs a path"
            BUILD_DIR=$2
            shift
            ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs needs a value"
            JOBS=$2
            shift
            ;;
        --no-check)
            CHECK=0
            ;;
        --keep)
            KEEP=1
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

[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"
PREFIX=$(realpath -m -- "$PREFIX")
BUILD_DIR=$(realpath -m -- "$BUILD_DIR")

if (( ! FORCE )) && [[ -x $PREFIX/bin/pv ]]; then
    installed=$("$PREFIX/bin/pv" --version 2>/dev/null | awk 'NR == 1 { print $2 }')
    if [[ $installed == "$PV_VERSION" ]]; then
        printf 'pv %s is already installed at %s\n' "$installed" "$PREFIX/bin/pv"
        exit 0
    fi
fi

for command in curl tar make sh awk sha256sum; do
    command -v "$command" >/dev/null || die "$command is required"
done
if ! command -v cc >/dev/null && ! command -v gcc >/dev/null; then
    die "a C compiler is required"
fi

mkdir -p -- "$PREFIX" "$BUILD_DIR"
SOURCE=$BUILD_DIR/pv-$PV_VERSION.tar.gz
URL=${PV_URL:-https://www.ivarch.com/s/pv-$PV_VERSION.tar.gz}
if [[ ! -f $SOURCE ]]; then
    printf 'Downloading %s\n' "$URL"
    curl -fL --retry 3 -o "$SOURCE" "$URL"
fi
if [[ -n $PV_SHA256 ]]; then
    printf '%s  %s\n' "$PV_SHA256" "$SOURCE" | sha256sum -c -
else
    printf 'warning: no SHA-256 pinned for pv %s\n' "$PV_VERSION" >&2
fi

rm -rf "$BUILD_DIR/pv-$PV_VERSION"
tar xzf "$SOURCE" -C "$BUILD_DIR"
cd "$BUILD_DIR/pv-$PV_VERSION"
sh ./configure --prefix="$PREFIX"
make -j"$JOBS"
if (( CHECK )); then
    make check
fi
make install

"$PREFIX/bin/pv" --version | head -2
if [[ ":$PATH:" != *":$PREFIX/bin:"* ]]; then
    printf 'Note: put %s before /usr/bin in PATH to use this pv.\n' "$PREFIX/bin"
fi

cd /
if (( ! KEEP )); then
    rm -rf "$BUILD_DIR"
fi
