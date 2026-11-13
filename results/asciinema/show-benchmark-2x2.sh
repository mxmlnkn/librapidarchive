#!/usr/bin/env bash
# Record the 2x2 rapidgzip / ibzip2 / gzip / bzip2 comparison demo.
#
# The script creates a tmux session with a minimal htop CPU panel on top and
# four decoder panes below. It types one command per pane, waits for all
# decoders to finish, verifies the decoded sizes, and removes the session
# again. Run it under asciinema to capture the result. See README.md in this
# directory.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z $REPO_ROOT ]]; then
    REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
fi

DATA_DIR=${DEMO_DATA_DIR:-$REPO_ROOT/tmp/asciinema-demo}
PROFILE=full
FAST=0
NO_ATTACH=0
PRINT_LAYOUT=0
NO_PV_DISCARD=0
SESSION=rapidgzip-demo-$$
DEMO_COLUMNS=94
ROWS=
HTOP_ROWS=
TYPE_DELAY=
COMMAND_PAUSE=
DRAMATIC_PAUSE=
FINAL_WAIT=
ENTER_DELAY=
TIMEOUT_SECONDS=
THREADS=

usage() {
    cat <<'EOF'
Usage: show-benchmark-2x2.sh [OPTION]...

Build a 2x2 tmux demo with rapidgzip (top left), gzip (top right),
ibzip2 (bottom left), and bzip2 (bottom right) below a minimal htop
CPU panel. Every decoder is piped through pv so the pane shows the
decompression bandwidth.

The commands are typed as if by a human. The next command starts only after
the previous decoder has finished, so the benchmarks never overlap. After the
last decoder finishes, the result stays on screen before the session closes.
Run it under asciinema to record the demo.

Options:
  --fast              Short delays and the silesia-demo-small input from
                      prepare-demo.sh --fast. Useful for a quick visual test.
  --dry-run           Alias for --fast --no-attach.
  --no-attach         Run the tmux session detached and print a verification
                      summary. Useful in a non-interactive terminal.
  --print-layout      Print the resolved \"COLUMNS ROWS\" and exit. Useful for
                      passing the same size to asciinema rec.
  --no-pv-discard     Do not use the pv -X discard path even when pv is new
                      enough. Useful for comparing pv -X with pv | wc -c.
  --data-dir DIR      Directory with the prepared inputs (default:
                      <repo>/tmp/asciinema-demo).
  --session NAME      tmux session name (default: rapidgzip-demo-<pid>).
  --columns N         Terminal columns used for the tmux window (default: 94).
  --rows N            Terminal rows used for the tmux window (default:
                      measured htop rows plus 12).
  --htop-rows N       Total htop pane height including its border
                      (default: measured from the running htop layout).
  --threads N         Pass -P N to rapidgzip and ibzip2. By default both use
                      their automatic parallelism.
  --type-delay SEC    Base delay between typed characters with jitter
                      (default: 0.11, fast: 0.002).
  --command-pause SEC Pause after a decoder finishes before the next command
                      (default: 0.8, fast: 0.15).
  --dramatic-pause SEC Pause after a decoder finishes before a GNU tool
                      (default: 1.6, fast: 0.4).
  --final-wait SEC    Keep the final results on screen (default: 10, fast: 2).
  --timeout SEC       Give up waiting for a decoder after SEC seconds.
  -h, --help          Show this help.

Binary selection:
  rapidgzip and ibzip2 are taken from $RAPIDGZIP_BIN and $IBZIP2_BIN when set,
  otherwise from <repo>/build/src/tools/ and finally from PATH. gzip, bzip2,
  htop, and pv are taken from PATH.

Input data:
  Run results/asciinema/prepare-demo.sh first (add --fast for the small input).
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

while (( $# > 0 )); do
    case $1 in
        --fast)
            FAST=1
            ;;
        --dry-run)
            FAST=1
            NO_ATTACH=1
            ;;
        --no-attach)
            NO_ATTACH=1
            ;;
        --print-layout)
            PRINT_LAYOUT=1
            ;;
        --no-pv-discard)
            NO_PV_DISCARD=1
            ;;
        --data-dir)
            [[ $# -ge 2 ]] || die "--data-dir needs a path"
            DATA_DIR=$2
            shift
            ;;
        --session)
            [[ $# -ge 2 ]] || die "--session needs a name"
            SESSION=$2
            shift
            ;;
        --columns)
            [[ $# -ge 2 ]] || die "--columns needs a value"
            DEMO_COLUMNS=$2
            shift
            ;;
        --rows)
            [[ $# -ge 2 ]] || die "--rows needs a value"
            ROWS=$2
            shift
            ;;
        --htop-rows)
            [[ $# -ge 2 ]] || die "--htop-rows needs a value"
            HTOP_ROWS=$2
            shift
            ;;
        --threads)
            [[ $# -ge 2 ]] || die "--threads needs a value"
            THREADS=$2
            shift
            ;;
        --type-delay)
            [[ $# -ge 2 ]] || die "--type-delay needs a value"
            TYPE_DELAY=$2
            shift
            ;;
        --command-pause)
            [[ $# -ge 2 ]] || die "--command-pause needs a value"
            COMMAND_PAUSE=$2
            shift
            ;;
        --dramatic-pause)
            [[ $# -ge 2 ]] || die "--dramatic-pause needs a value"
            DRAMATIC_PAUSE=$2
            shift
            ;;
        --final-wait)
            [[ $# -ge 2 ]] || die "--final-wait needs a value"
            FINAL_WAIT=$2
            shift
            ;;
        --timeout)
            [[ $# -ge 2 ]] || die "--timeout needs a value"
            TIMEOUT_SECONDS=$2
            shift
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

DATA_DIR=$(realpath -m -- "$DATA_DIR")

if (( FAST )); then
    PROFILE=fast
    : "${TYPE_DELAY:=0.002}"
    : "${COMMAND_PAUSE:=0.15}"
    : "${DRAMATIC_PAUSE:=0.4}"
    : "${FINAL_WAIT:=2}"
    : "${ENTER_DELAY:=0.02}"
    : "${TIMEOUT_SECONDS:=120}"
else
    : "${TYPE_DELAY:=0.11}"
    : "${COMMAND_PAUSE:=0.8}"
    : "${DRAMATIC_PAUSE:=1.6}"
    : "${FINAL_WAIT:=10}"
    : "${ENTER_DELAY:=0.12}"
    : "${TIMEOUT_SECONDS:=3600}"
fi
TYPE_DELAY_MS=$(awk -v delay="$TYPE_DELAY" 'BEGIN { printf "%d", delay * 1000 }')

if [[ -n $THREADS && ! $THREADS =~ ^[1-9][0-9]*$ ]]; then
    die "--threads must be a positive integer"
fi
if [[ ! $DEMO_COLUMNS =~ ^[1-9][0-9]*$ ]]; then
    die "--columns must be a positive integer"
fi

TMUX_BIN=$(command -v tmux) || die "tmux is required (apt-get install tmux)"
HTOP_BIN=$(command -v htop) || die "htop is required (apt-get install htop)"
PV_BIN=$(command -v pv) || die "pv is required (apt-get install pv)"
# pv 1.12.7 added a splice-to-/dev/null path for -X. Older versions make -X imply -C and
# therefore read the stream into user space, so only use -X for a recent enough pv.
PV_VERSION=$("$PV_BIN" --version 2>/dev/null | sed -nE 's/^pv ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -1)
PV_DISCARD_SPLICE=0
if [[ -n $PV_VERSION ]] && printf '%s\n%s\n' '1.12.7' "$PV_VERSION" | sort -C -V; then
    PV_DISCARD_SPLICE=1
fi
if (( NO_PV_DISCARD )); then
    PV_DISCARD_SPLICE=0
fi
BASH_BIN=${BASH:-}
if [[ -z $BASH_BIN ]]; then
    BASH_BIN=$(command -v bash) || die "bash is required"
fi
if (( ! NO_ATTACH && ! PRINT_LAYOUT )) && [[ ! -t 1 ]]; then
    die "not a terminal; use --dry-run/--no-attach or run the script under asciinema"
fi

MANIFEST=$DATA_DIR/manifest-$PROFILE.env
if [[ ! -f $MANIFEST ]]; then
    prepare_command="$SCRIPT_DIR/prepare-demo.sh --data-dir $(printf '%q' "$DATA_DIR")"
    [[ $PROFILE == fast ]] && prepare_command+=" --fast"
    die "missing $MANIFEST. The prepared inputs are not committed; run '$prepare_command' on this machine first."
fi
# shellcheck source=/dev/null
source "$MANIFEST"

GZ_SIZE=${DEMO_GZ_SIZE:-${DEMO_SIZE:-}}
BZ2_SIZE=${DEMO_BZ2_SIZE:-${DEMO_SIZE:-}}
[[ -f $DEMO_GZ ]] || die "missing compressed input: $DEMO_GZ"
[[ -f $DEMO_BZ2 ]] || die "missing compressed input: $DEMO_BZ2"
[[ -n $GZ_SIZE && $GZ_SIZE =~ ^[0-9]+$ ]] || die "invalid DEMO_GZ_SIZE in $MANIFEST"
[[ -n $BZ2_SIZE && $BZ2_SIZE =~ ^[0-9]+$ ]] || die "invalid DEMO_BZ2_SIZE in $MANIFEST"

resolveBinary() {
    local variable=$1 explicit=$2 default=$3
    if [[ -n $explicit ]]; then
        [[ -x $explicit ]] || die "$variable is not executable: $explicit"
        realpath -- "$explicit"
        return
    fi
    if [[ -x $default ]]; then
        realpath -- "$default"
        return
    fi
    if command -v "$(basename -- "$default")" >/dev/null 2>&1; then
        command -v "$(basename -- "$default")"
        return
    fi
    die "cannot find $(basename -- "$default"); build it or set $variable"
}

RAPIDGZIP_BIN=$(resolveBinary RAPIDGZIP_BIN "${RAPIDGZIP_BIN:-}" "$REPO_ROOT/build/src/tools/rapidgzip")
IBZIP2_BIN=$(resolveBinary IBZIP2_BIN "${IBZIP2_BIN:-}" "$REPO_ROOT/build/src/tools/ibzip2")
GZIP_BIN=$(command -v gzip) || die "gzip is required"
BZIP2_BIN=$(command -v bzip2) || die "bzip2 is required"

VERIFY_DIR=$(mktemp -d "$DATA_DIR/.verify.XXXXXX")
CHOREO_PID=

cleanup() {
    local status=$?
    trap - EXIT INT TERM HUP
    if [[ -n $CHOREO_PID ]]; then
        kill "$CHOREO_PID" 2>/dev/null || true
    fi
    tmux kill-session -t "$SESSION-measure" 2>/dev/null || true
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    if [[ -z ${DEMO_KEEP_VERIFY:-} ]]; then
        rm -rf -- "$VERIFY_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

mkdir -p "$VERIFY_DIR/bin"
ln -s -- "$RAPIDGZIP_BIN" "$VERIFY_DIR/bin/rapidgzip"
ln -s -- "$IBZIP2_BIN" "$VERIFY_DIR/bin/ibzip2"
ln -s -- "$GZIP_BIN" "$VERIFY_DIR/bin/gzip"
ln -s -- "$BZIP2_BIN" "$VERIFY_DIR/bin/bzip2"
# Keep the typed command short and display SI units (GB/s) when pv supports them.
# pv 1.8.5, for example, predates --si and must be used without it.
if "$PV_BIN" --help 2>&1 | tr -d '[:space:]' | grep -q -- '--si'; then
    printf '#!/bin/sh\nexec %q --si "$@"\n' "$PV_BIN" > "$VERIFY_DIR/bin/pv"
    chmod +x "$VERIFY_DIR/bin/pv"
else
    ln -s -- "$PV_BIN" "$VERIFY_DIR/bin/pv"
fi

# htop rewrites its configuration file when it exits, so keep the committed
# copy pristine and let htop own a temporary copy.
HTOP_CONFIG=$VERIFY_DIR/htoprc
cp -- "$SCRIPT_DIR/htoprc" "$HTOP_CONFIG"
chmod u+w -- "$HTOP_CONFIG"

HTOP_ARGS=(-d 5)
if "$HTOP_BIN" --help 2>&1 | grep -q -- '--no-function-bar'; then
    HTOP_ARGS+=(--no-function-bar)
fi

# The number of CPU bars per row depends on the htop version, the terminal
# width, and the CPU count, so measure the actual meter height instead of
# guessing it. A too-tall pane exposes htop's process list below the bars.
measureHtopRows() {
    local session=$1 command output line headerRows=0 argument cpuCount
    command=$(printf 'HTOPRC=%q LC_ALL=C %q' "$HTOP_CONFIG" "$HTOP_BIN")
    for argument in "${HTOP_ARGS[@]}"; do
        command+=" $(printf '%q' "$argument")"
    done
    command+=' -n 1; sleep 5'
    if "$TMUX_BIN" new-session -d -s "$session" -x "$DEMO_COLUMNS" -y 64 -c "$REPO_ROOT" "$command"; then
        sleep 0.8
        output=$("$TMUX_BIN" capture-pane -p -t "$session" 2>/dev/null || true)
        "$TMUX_BIN" kill-session -t "$session" 2>/dev/null || true
    else
        output=
    fi
    while IFS= read -r line; do
        if [[ $line =~ ^[[:space:]]*[0-9]+\[[^]]*\] ]]; then
            headerRows=$(( headerRows + 1 ))
        else
            break
        fi
    done <<< "$output"
    if (( headerRows < 1 )); then
        cpuCount=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || printf '4')
        [[ $cpuCount =~ ^[1-9][0-9]*$ ]] || cpuCount=4
        headerRows=$(( (cpuCount + 3) / 4 ))
    fi
    printf '%d\n' $(( headerRows + 1 ))
}

if [[ -z $HTOP_ROWS ]]; then
    HTOP_ROWS=$(measureHtopRows "$SESSION-measure")
fi
if [[ -z $ROWS ]]; then
    ROWS=$(( HTOP_ROWS + 12 ))
fi
if [[ ! $ROWS =~ ^[1-9][0-9]*$ ]]; then
    die "--rows must be a positive integer"
fi
if [[ ! $HTOP_ROWS =~ ^[1-9][0-9]*$ ]] || (( HTOP_ROWS < 2 )); then
    die "--htop-rows must be at least 2"
fi
if (( ROWS <= HTOP_ROWS )); then
    die "--rows must be larger than the $HTOP_ROWS-row htop panel"
fi
if (( PRINT_LAYOUT )); then
    printf '%s %s\n' "$DEMO_COLUMNS" "$ROWS"
    exit 0
fi

HTOP_CMD=$(printf 'HTOPRC=%q exec %q' "$HTOP_CONFIG" "$HTOP_BIN")
for argument in "${HTOP_ARGS[@]}"; do
    HTOP_CMD+=" $(printf '%q' "$argument")"
done
PANE_SHELL=$(printf '%q --norc --noprofile' "$BASH_BIN")
BOTTOM=$(tmux new-session -d -P -F '#{pane_id}' -s "$SESSION" -x "$DEMO_COLUMNS" -y "$ROWS" \
    -c "$REPO_ROOT" "$PANE_SHELL")
TOP=$(tmux split-window -v -b -P -F '#{pane_id}' -t "$BOTTOM" -l "$(( HTOP_ROWS - 1 ))" \
    -c "$REPO_ROOT" "$HTOP_CMD")
GZIP_PANE=$(tmux split-window -h -P -F '#{pane_id}' -t "$BOTTOM" \
    -c "$REPO_ROOT" "$PANE_SHELL")
IBZIP2_PANE=$(tmux split-window -v -P -F '#{pane_id}' -t "$BOTTOM" \
    -c "$REPO_ROOT" "$PANE_SHELL")
BZIP2_PANE=$(tmux split-window -v -P -F '#{pane_id}' -t "$GZIP_PANE" \
    -c "$REPO_ROOT" "$PANE_SHELL")
RAPIDGZIP_PANE=$BOTTOM
sleep 0.3
PANE_LIST=$("$TMUX_BIN" list-panes -a -F '#{pane_id}')
if ! grep -Fxq -- "$TOP" <<< "$PANE_LIST"; then
    die "the htop pane exited immediately; check '$HTOP_BIN --version' and HTOPRC=$HTOP_CONFIG"
fi
tmux resize-pane -t "$TOP" -y "$(( HTOP_ROWS - 1 ))"
# Keep the tmux window at the resolved size even when the script runs inside a larger tmux
# client or the user's tmux configuration uses window-size largest/latest.
if "$TMUX_BIN" show-options -w -t "$SESSION:0" window-size >/dev/null 2>&1; then
    "$TMUX_BIN" set-option -w -t "$SESSION:0" window-size manual 2>/dev/null || true
fi
"$TMUX_BIN" resize-window -t "$SESSION:0" -x "$DEMO_COLUMNS" -y "$ROWS" 2>/dev/null || true

tmux set-option -t "$SESSION" status off
tmux set-option -t "$SESSION" pane-border-status off
tmux set-option -t "$SESSION" pane-border-style 'fg=colour7'
tmux set-option -t "$SESSION" pane-active-border-style 'fg=colour2'

STATUS_RAPIDGZIP=$VERIFY_DIR/pane-rapidgzip.status
STATUS_GZIP=$VERIFY_DIR/pane-gzip.status
STATUS_IBZIP2=$VERIFY_DIR/pane-ibzip2.status
STATUS_BZIP2=$VERIFY_DIR/pane-bzip2.status

setupPane() {
    local target=$1 status_file=$2 label=$3
    local prompt_command setup
    prompt_command=$(printf 'printf %%s "$?" > %q' "$status_file")
    setup=$(printf 'cd %q; clear; printf "\\033[38;5;7m%%s\\033[0m\\n" %q; export PS1="\\$ "; set -o pipefail; export PATH=%q:%q; PROMPT_COMMAND=%q' \
        "$INPUT_DIR" "$label" "$VERIFY_DIR/bin" "$PATH" "$prompt_command")
    tmux send-keys -t "$target" -l "$setup"
    tmux send-keys -t "$target" Enter
}

INPUT_DIR=$(dirname -- "$DEMO_GZ")
[[ $(dirname -- "$DEMO_BZ2") == "$INPUT_DIR" ]] \
    || die "the gzip and bzip2 inputs must be in the same directory"

setupPane "$RAPIDGZIP_PANE" "$STATUS_RAPIDGZIP" rapidgzip
setupPane "$GZIP_PANE" "$STATUS_GZIP" gzip
setupPane "$IBZIP2_PANE" "$STATUS_IBZIP2" ibzip2
setupPane "$BZIP2_PANE" "$STATUS_BZIP2" bzip2
sleep 0.3

GZ_Q=$(printf '%q' "$(basename -- "$DEMO_GZ")")
BZ2_Q=$(printf '%q' "$(basename -- "$DEMO_BZ2")")

CMD_RAPIDGZIP="rapidgzip"
CMD_IBZIP2="ibzip2"
if [[ -n $THREADS ]]; then
    CMD_RAPIDGZIP+=" -P $THREADS"
    CMD_IBZIP2+=" -P $THREADS"
fi
PV_PIPE=" | pv"
if (( PV_DISCARD_SPLICE )); then
    PV_PIPE+=" -X"
else
    PV_PIPE+=" | wc -c"
fi
CMD_RAPIDGZIP+=" -d -c $GZ_Q$PV_PIPE"
CMD_IBZIP2+=" -d -c $BZ2_Q$PV_PIPE"
CMD_GZIP="gzip -d -c $GZ_Q$PV_PIPE"
CMD_BZIP2="bzip2 -d -c $BZ2_Q$PV_PIPE"

typeText() {
    local target=$1 text=$2 index delay_ms delay
    for (( index = 0; index < ${#text}; index++ )); do
        tmux send-keys -t "$target" -l "${text:index:1}"
        delay_ms=$TYPE_DELAY_MS
        if (( delay_ms > 20 )); then
            delay_ms=$(( delay_ms + RANDOM % 80 - 30 ))
            if (( index > 0 && RANDOM % 12 == 0 )); then
                delay_ms=$(( delay_ms + 120 ))
            fi
        fi
        (( delay_ms < 5 )) && delay_ms=5
        printf -v delay '%d.%03d' $(( delay_ms / 1000 )) $(( delay_ms % 1000 ))
        sleep "$delay"
    done
}

startCommand() {
    local target=$1 command=$2 status_file=$3
    tmux select-pane -t "$target"
    rm -f -- "$status_file"
    typeText "$target" "$command"
    sleep "$ENTER_DELAY"
    tmux send-keys -t "$target" Enter
}

waitForMarker() {
    local status_file=$1 label=$2
    local deadline=$(( $(date +%s) + TIMEOUT_SECONDS ))
    while [[ ! -f $status_file ]]; do
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            printf 'tmux session disappeared while waiting for %s\n' "$label" >&2
            return 1
        fi
        if (( $(date +%s) >= deadline )); then
            printf 'timed out after %ss waiting for %s\n' "$TIMEOUT_SECONDS" "$label" >&2
            return 1
        fi
        sleep 0.1
    done
}

capturePane() {
    local target=$1 output=$2
    tmux capture-pane -p -J -S -200 -t "$target" > "$output"
}

runChoreography() {
    sleep 0.4
    startCommand "$RAPIDGZIP_PANE" "$CMD_RAPIDGZIP" "$STATUS_RAPIDGZIP"
    waitForMarker "$STATUS_RAPIDGZIP" rapidgzip
    sleep "$COMMAND_PAUSE"

    startCommand "$IBZIP2_PANE" "$CMD_IBZIP2" "$STATUS_IBZIP2"
    waitForMarker "$STATUS_IBZIP2" ibzip2
    sleep "$DRAMATIC_PAUSE"

    startCommand "$GZIP_PANE" "$CMD_GZIP" "$STATUS_GZIP"
    waitForMarker "$STATUS_GZIP" gzip
    sleep "$DRAMATIC_PAUSE"

    startCommand "$BZIP2_PANE" "$CMD_BZIP2" "$STATUS_BZIP2"
    waitForMarker "$STATUS_BZIP2" bzip2

    sleep "$FINAL_WAIT"
    capturePane "$RAPIDGZIP_PANE" "$VERIFY_DIR/pane-rapidgzip.out"
    capturePane "$GZIP_PANE" "$VERIFY_DIR/pane-gzip.out"
    capturePane "$IBZIP2_PANE" "$VERIFY_DIR/pane-ibzip2.out"
    capturePane "$BZIP2_PANE" "$VERIFY_DIR/pane-bzip2.out"
    # Detach the client first so tmux can restore the terminal state cleanly.
    "$TMUX_BIN" detach-client -s "$SESSION" 2>/dev/null || true
    sleep 0.2
    tmux kill-session -t "$SESSION" 2>/dev/null || true
}

choreographyExit() {
    local status=$?
    if (( status != 0 )); then
        printf 'demo choreography failed with status %d\n' "$status" > "$VERIFY_DIR/error"
        tmux kill-session -t "$SESSION" 2>/dev/null || true
    fi
}

(
    set -e
    trap choreographyExit EXIT
    runChoreography
) &
CHOREO_PID=$!

if (( NO_ATTACH )); then
    wait "$CHOREO_PID" || true
else
    tmux attach -t "$SESSION" || true
    wait "$CHOREO_PID" || true
fi
CHOREO_PID=

FAILURES=0
LOG_FILE=$DATA_DIR/last-demo-$PROFILE.log
{
    printf 'demo profile: %s\n' "$PROFILE"
    printf 'layout: %sx%s (htop %s rows)\n' "$DEMO_COLUMNS" "$ROWS" "$HTOP_ROWS"
    printf 'pv: %s (discard splice: %s)\n' "${PV_VERSION:-unknown}" "$PV_DISCARD_SPLICE"
    printf 'gzip input: %s (%s bytes)\n' "$DEMO_GZ" "$GZ_SIZE"
    printf 'bzip2 input: %s (%s bytes)\n' "$DEMO_BZ2" "$BZ2_SIZE"
    printf 'date: %s\n' "$(date --iso-8601=seconds)"
} > "$LOG_FILE"

if [[ -f $VERIFY_DIR/error ]]; then
    printf 'error: %s\n' "$(cat -- "$VERIFY_DIR/error")" >&2
    printf 'error: %s\n' "$(cat -- "$VERIFY_DIR/error")" >> "$LOG_FILE"
    FAILURES=$(( FAILURES + 1 ))
fi

verifyPane() {
    local name=$1 status_file=$2 output=$3 expected_size=${4:-}
    local status size_ok=skipped rate_ok=skipped
    status=$(cat -- "$status_file" 2>/dev/null || printf 'missing')
    if [[ -n $expected_size ]]; then
        size_ok=no
        if [[ -f $output ]]; then
            grep -Eq "(^|[^0-9])${expected_size}([^0-9]|$)" "$output" && size_ok=yes
        fi
    else
        rate_ok=no
        if [[ -f $output ]] && grep -Eq '([kMGTP]i?)?B/s' "$output"; then
            rate_ok=yes
        fi
    fi
    printf '%-10s status=%-7s size=%s rate=%s\n' "$name" "$status" "$size_ok" "$rate_ok" >> "$LOG_FILE"
    if [[ $status != 0 ]]; then
        printf 'error: %s exited with status %s\n' "$name" "$status" >&2
        FAILURES=$(( FAILURES + 1 ))
    fi
    if [[ -n $expected_size && $size_ok != yes ]]; then
        printf 'error: %s did not print the expected decoded size %s\n' "$name" "$expected_size" >&2
        FAILURES=$(( FAILURES + 1 ))
    fi
    if [[ -z $expected_size && $rate_ok != yes ]]; then
        printf 'error: %s did not print a pv bandwidth\n' "$name" >&2
        FAILURES=$(( FAILURES + 1 ))
    fi
}

if (( PV_DISCARD_SPLICE )); then
    verifyPane rapidgzip "$STATUS_RAPIDGZIP" "$VERIFY_DIR/pane-rapidgzip.out"
    verifyPane gzip "$STATUS_GZIP" "$VERIFY_DIR/pane-gzip.out"
    verifyPane ibzip2 "$STATUS_IBZIP2" "$VERIFY_DIR/pane-ibzip2.out"
    verifyPane bzip2 "$STATUS_BZIP2" "$VERIFY_DIR/pane-bzip2.out"
else
    verifyPane rapidgzip "$STATUS_RAPIDGZIP" "$VERIFY_DIR/pane-rapidgzip.out" "$GZ_SIZE"
    verifyPane gzip "$STATUS_GZIP" "$VERIFY_DIR/pane-gzip.out" "$GZ_SIZE"
    verifyPane ibzip2 "$STATUS_IBZIP2" "$VERIFY_DIR/pane-ibzip2.out" "$BZ2_SIZE"
    verifyPane bzip2 "$STATUS_BZIP2" "$VERIFY_DIR/pane-bzip2.out" "$BZ2_SIZE"
fi

printf 'verification log: %s\n' "$LOG_FILE" >> "$LOG_FILE"

if (( FAILURES > 0 )); then
    cat -- "$LOG_FILE"
    exit 1
fi
if (( NO_ATTACH )); then
    cat -- "$LOG_FILE"
fi
