#!/usr/bin/env bash
# Record the 2x2 demo, finalize the cast, render the GIF, and round the corners.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z $REPO_ROOT ]]; then
    REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
fi

FAST=0
PREPARE=0
ROUND=1
DATA_DIR=${DEMO_DATA_DIR:-$REPO_ROOT/tmp/asciinema-demo}
CAST_FILE=
GIF_FILE=
RADIUS=10
FONT_FAMILY='DejaVu Sans Mono'
FONT_SIZE=12
LINE_HEIGHT=1.25
LAST_FRAME_DURATION=3
IDLE_TIME_LIMIT=2
SHOW_ARGS=()

usage() {
    cat <<'EOF'
Usage: record-demo.sh [OPTION]... [-- SHOW-OPTION]...

Run the 2x2 demo under asciinema, finalize the cast, render the GIF, and round
its corners with transparency in one go. The inputs must already be prepared,
unless --prepare is passed.

Options:
  --fast                  Use the fast input and short timings.
  --prepare               Run prepare-demo.sh before recording. The full
                          profile can take several minutes and several GiB.
  --data-dir DIR          Prepared input directory for prepare and the demo.
  --cast FILE             Cast output (default: <data-dir>/rapidgzip-ibzip2-comparison[-fast].cast).
  --gif FILE              GIF output (default: results/asciinema/rapidgzip-ibzip2-comparison[-fast].gif).
  --radius PX             Rounded-corner radius in pixels (default: 10).
  --font-family NAME      agg font family (default: DejaVu Sans Mono).
  --font-size PX          agg font size (default: 12).
  --line-height FACTOR    agg line height (default: 1.25, which keeps the
                          pane border glyphs continuous).
  --last-frame-duration S agg last-frame duration (default: 3).
  --idle-time-limit S     asciinema idle time limit (default: 2).
  --no-round              Skip the rounded-corner post-processing.
  -h, --help              Show this help.

Options after -- are passed to show-benchmark-2x2.sh, for example:

  record-demo.sh --data-dir tmp/asciinema-demo -- --threads 8

The first run prints the resolved layout, then records with matching size
options. asciinema 2.x uses --cols/--rows; asciinema 3.x uses --window-size and
requests an asciicast-v2 file so agg can read it.

Set ASCIINEMA_BIN to use a specific asciinema binary.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

restoreTerminal() {
    tput rmcup 2>/dev/null || true
    tput cnorm 2>/dev/null || true
    stty sane 2>/dev/null || true
    printf '\033[0m'
}

while (( $# > 0 )); do
    case $1 in
        --fast)
            FAST=1
            ;;
        --prepare)
            PREPARE=1
            ;;
        --no-round)
            ROUND=0
            ;;
        --data-dir)
            [[ $# -ge 2 ]] || die "--data-dir needs a path"
            DATA_DIR=$2
            shift
            ;;
        --cast)
            [[ $# -ge 2 ]] || die "--cast needs a path"
            CAST_FILE=$2
            shift
            ;;
        --gif)
            [[ $# -ge 2 ]] || die "--gif needs a path"
            GIF_FILE=$2
            shift
            ;;
        --radius)
            [[ $# -ge 2 ]] || die "--radius needs a value"
            RADIUS=$2
            shift
            ;;
        --font-family)
            [[ $# -ge 2 ]] || die "--font-family needs a value"
            FONT_FAMILY=$2
            shift
            ;;
        --font-size)
            [[ $# -ge 2 ]] || die "--font-size needs a value"
            FONT_SIZE=$2
            shift
            ;;
        --line-height)
            [[ $# -ge 2 ]] || die "--line-height needs a value"
            LINE_HEIGHT=$2
            shift
            ;;
        --last-frame-duration)
            [[ $# -ge 2 ]] || die "--last-frame-duration needs a value"
            LAST_FRAME_DURATION=$2
            shift
            ;;
        --idle-time-limit)
            [[ $# -ge 2 ]] || die "--idle-time-limit needs a value"
            IDLE_TIME_LIMIT=$2
            shift
            ;;
        --)
            shift
            SHOW_ARGS+=("$@")
            break
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1 (try --help; use -- to forward options to show-benchmark-2x2.sh)"
            ;;
    esac
    shift
done

if [[ ! $FONT_SIZE =~ ^[1-9][0-9]*$ ]]; then
    die "--font-size must be a positive integer"
fi
if [[ ! $LINE_HEIGHT =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    die "--line-height must be a positive number"
fi

DATA_DIR=$(realpath -m -- "$DATA_DIR")
if [[ -z $CAST_FILE ]]; then
    CAST_FILE=$DATA_DIR/rapidgzip-ibzip2-comparison
    (( FAST )) && CAST_FILE+='-fast'
    CAST_FILE+='.cast'
else
    CAST_FILE=$(realpath -m -- "$CAST_FILE")
fi
if [[ -z $GIF_FILE ]]; then
    GIF_FILE=$SCRIPT_DIR/rapidgzip-ibzip2-comparison
    (( FAST )) && GIF_FILE+='-fast'
    GIF_FILE+='.gif'
else
    GIF_FILE=$(realpath -m -- "$GIF_FILE")
fi

ASCIINEMA_BIN=${ASCIINEMA_BIN:-$(command -v asciinema)}
[[ -n $ASCIINEMA_BIN && -x $ASCIINEMA_BIN ]] \
    || die "asciinema is required (apt-get install asciinema)"
AGG_BIN=$(command -v agg) || die "agg is required"
AGG_VERSION=$("$AGG_BIN" --version 2>/dev/null | awk 'NR == 1 { print $2 }')
if [[ -n $AGG_VERSION ]] && ! printf '%s\n%s\n' '1.9.0' "$AGG_VERSION" | sort -C -V; then
    printf 'warning: agg %s is old; 1.9.0 or newer is recommended for continuous pane borders\n' "$AGG_VERSION" >&2
fi
command -v python3 >/dev/null || die "python3 is required"
if (( ROUND )); then
    command -v gifsicle >/dev/null || die "gifsicle is required for rounding (apt-get install gifsicle)"
fi
[[ -x $SCRIPT_DIR/show-benchmark-2x2.sh ]] || die "missing $SCRIPT_DIR/show-benchmark-2x2.sh"
[[ -f $SCRIPT_DIR/finalize-cast.py ]] || die "missing $SCRIPT_DIR/finalize-cast.py"
if (( ROUND )); then
    [[ -f $SCRIPT_DIR/round-gif.py ]] || die "missing $SCRIPT_DIR/round-gif.py"
fi

data_args=()
[[ -n $DATA_DIR ]] && data_args+=(--data-dir "$DATA_DIR")
fast_args=()
(( FAST )) && fast_args+=(--fast)

if (( PREPARE )); then
    printf 'Preparing demo inputs...\n'
    "$SCRIPT_DIR/prepare-demo.sh" "${data_args[@]}" "${fast_args[@]}"
fi

show_command_parts=(
    "$SCRIPT_DIR/show-benchmark-2x2.sh"
    "${data_args[@]}"
    "${fast_args[@]}"
    "${SHOW_ARGS[@]}"
)
layout=$("${show_command_parts[@]}" --print-layout)
read -r DEMO_COLUMNS ROWS <<< "$layout"
if [[ ! $DEMO_COLUMNS =~ ^[1-9][0-9]*$ || ! $ROWS =~ ^[1-9][0-9]*$ ]]; then
    die "could not resolve the demo layout: $layout"
fi
printf 'Recording layout: %sx%s\n' "$DEMO_COLUMNS" "$ROWS"
[[ -t 1 ]] || die "not a terminal; asciinema needs a tty"

record_command=$(printf '%q' "$SCRIPT_DIR/show-benchmark-2x2.sh")
for argument in "${data_args[@]}" "${fast_args[@]}" "${SHOW_ARGS[@]}"; do
    record_command+=" $(printf '%q' "$argument")"
done

ASCIINEMA_HELP=$("$ASCIINEMA_BIN" rec --help 2>&1 || true)
asciinema_args=(rec --overwrite -i "$IDLE_TIME_LIMIT" -c "$record_command")
if grep -q -- '--window-size' <<< "$ASCIINEMA_HELP"; then
    # asciinema 3.x ignores --cols/--rows and uses the current terminal size.
    asciinema_args+=(--window-size "${DEMO_COLUMNS}x${ROWS}")
    if grep -q -- '--output-format' <<< "$ASCIINEMA_HELP"; then
        asciinema_args+=(--output-format asciicast-v2)
    fi
    if grep -q -- '--return' <<< "$ASCIINEMA_HELP"; then
        asciinema_args+=(--return)
    fi
else
    asciinema_args+=(--cols "$DEMO_COLUMNS" --rows "$ROWS")
fi
asciinema_args+=("$CAST_FILE")

mkdir -p -- "$(dirname -- "$CAST_FILE")" "$(dirname -- "$GIF_FILE")"
printf 'Recording cast: %s\n' "$CAST_FILE"
trap 'restoreTerminal' EXIT
TERM=xterm-256color "$ASCIINEMA_BIN" "${asciinema_args[@]}"
trap - EXIT
restoreTerminal

printf 'Finalizing cast: %s\n' "$CAST_FILE"
python3 "$SCRIPT_DIR/finalize-cast.py" "$CAST_FILE"

raw_gif=
cleanup() {
    if [[ -n $raw_gif ]]; then
        rm -f -- "$raw_gif"
    fi
}
trap cleanup EXIT

if (( ROUND )); then
    raw_gif=$(mktemp "${TMPDIR:-/tmp}/rapidgzip-demo-raw.XXXXXX.gif")
    "$AGG_BIN" --font-family "$FONT_FAMILY" --font-size "$FONT_SIZE" --line-height "$LINE_HEIGHT" \
        --last-frame-duration "$LAST_FRAME_DURATION" "$CAST_FILE" "$raw_gif"
    printf 'Rounding GIF: %s\n' "$GIF_FILE"
    python3 "$SCRIPT_DIR/round-gif.py" "$raw_gif" "$GIF_FILE" --radius "$RADIUS"
else
    "$AGG_BIN" --font-family "$FONT_FAMILY" --font-size "$FONT_SIZE" --line-height "$LINE_HEIGHT" \
        --last-frame-duration "$LAST_FRAME_DURATION" "$CAST_FILE" "$GIF_FILE"
fi

printf 'Wrote:\n'
ls -lh -- "$CAST_FILE" "$GIF_FILE"
