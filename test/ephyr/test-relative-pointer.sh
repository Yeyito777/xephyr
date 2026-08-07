#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
XEPHYR_BIN="${XEPHYR_BIN:-$ROOT_DIR/build/hw/kdrive/ephyr/Xephyr}"
HOST_DISPLAY="${XEPHYR_TEST_HOST_DISPLAY:-}"
TEST_BUILD_DIR="$ROOT_DIR/build/test/ephyr-relative-pointer"
TITLE="xephyr-relative-pointer-test-$$"
EVENTS="$TEST_BUILD_DIR/events-$$.log"
XEPHYR_LOG="$TEST_BUILD_DIR/xephyr-$$.log"
xephyr_pid=""
recorder_pid=""

fail() {
    echo "relative-pointer test: $*" >&2
    if [[ -f "$EVENTS" ]]; then
        echo "--- raw events ---" >&2
        cat "$EVENTS" >&2
    fi
    if [[ -f "$XEPHYR_LOG" ]]; then
        echo "--- Xephyr log ---" >&2
        cat "$XEPHYR_LOG" >&2
    fi
    exit 1
}

cleanup() {
    [[ -z "$recorder_pid" ]] || kill "$recorder_pid" 2>/dev/null || true
    [[ -z "$xephyr_pid" ]] || kill "$xephyr_pid" 2>/dev/null || true
    [[ -z "$recorder_pid" ]] || wait "$recorder_pid" 2>/dev/null || true
    [[ -z "$xephyr_pid" ]] || wait "$xephyr_pid" 2>/dev/null || true
    rm -f "$EVENTS" "$XEPHYR_LOG"
}
trap cleanup EXIT

[[ "${XEPHYR_TEST_NESTED:-}" == "1" ]] ||
    fail "refusing to inject input without XEPHYR_TEST_NESTED=1"
[[ -n "$HOST_DISPLAY" ]] || fail "XEPHYR_TEST_HOST_DISPLAY is required"
[[ -x "$XEPHYR_BIN" ]] || fail "Xephyr binary is not executable: $XEPHYR_BIN"

mkdir -p "$TEST_BUILD_DIR"
cc -O2 -Wall -Wextra -o "$TEST_BUILD_DIR/xi2-raw-recorder" \
    "$ROOT_DIR/test/ephyr/xi2-raw-recorder.c" \
    $(pkg-config --cflags --libs xi x11)
cc -O2 -Wall -Wextra -o "$TEST_BUILD_DIR/pointer-control" \
    "$ROOT_DIR/test/ephyr/pointer-control.c" \
    $(pkg-config --cflags --libs x11)

display=""
for number in $(seq 90 199); do
    if [[ ! -S "/tmp/.X11-unix/X$number" ]]; then
        display=":$number"
        break
    fi
done
[[ -n "$display" ]] || fail "could not find a free nested display"

DISPLAY="$HOST_DISPLAY" "$XEPHYR_BIN" \
    -br -ac -noreset -screen 800x600 -resizeable \
    -name "$TITLE" -title "$TITLE" "$display" \
    >"$XEPHYR_LOG" 2>&1 &
xephyr_pid=$!

for _ in $(seq 1 100); do
    [[ -S "/tmp/.X11-unix/X${display#:}" ]] && break
    kill -0 "$xephyr_pid" 2>/dev/null || fail "Xephyr exited during startup"
    sleep 0.05
done
[[ -S "/tmp/.X11-unix/X${display#:}" ]] || fail "Xephyr display did not appear"

"$TEST_BUILD_DIR/xi2-raw-recorder" "$display" >"$EVENTS" 2>&1 &
recorder_pid=$!
for _ in $(seq 1 100); do
    grep -q '^READY ' "$EVENTS" 2>/dev/null && break
    kill -0 "$recorder_pid" 2>/dev/null || fail "raw recorder exited during startup"
    sleep 0.02
done
grep -q '^READY ' "$EVENTS" || fail "raw recorder did not become ready"

for _ in $(seq 1 100); do
    if "$TEST_BUILD_DIR/pointer-control" "$HOST_DISPLAY" \
        window-query "$TITLE" >/dev/null 2>&1; then
        break
    fi
    sleep 0.02
done
"$TEST_BUILD_DIR/pointer-control" "$HOST_DISPLAY" \
    window-query "$TITLE" >/dev/null 2>&1 || fail "could not find Xephyr host window"

# Establish the host-position baseline, then move by exactly +10,-5.
"$TEST_BUILD_DIR/pointer-control" "$HOST_DISPLAY" \
    window-move "$TITLE" 200 200
sleep 0.08
baseline_lines=$(wc -l < "$EVENTS")
"$TEST_BUILD_DIR/pointer-control" "$HOST_DISPLAY" \
    window-move "$TITLE" 210 195

matched=0
for _ in $(seq 1 100); do
    if tail -n "+$((baseline_lines + 1))" "$EVENTS" |
        grep -q '^RAW dx=10\.000 dy=-5\.000 '; then
        matched=1
        break
    fi
    sleep 0.02
done
[[ "$matched" == "1" ]] || fail "host +10,-5 motion did not produce raw +10,-5"

guest_position=$("$TEST_BUILD_DIR/pointer-control" "$display" root-query)
[[ "$guest_position" == "210 195" ]] ||
    fail "normal nested pointer position was not retained (got $guest_position)"

# A nested WarpPointer should be mirrored to the host but must not return as
# another XI_RawMotion event in the nested server.
sleep 0.04
before_warp=$(grep -c '^RAW ' "$EVENTS" || true)
"$TEST_BUILD_DIR/pointer-control" "$display" root-warp 400 300
sleep 0.15
after_warp=$(grep -c '^RAW ' "$EVENTS" || true)
[[ "$after_warp" == "$before_warp" ]] ||
    fail "guest WarpPointer leaked back as nested raw motion"

host_position=$("$TEST_BUILD_DIR/pointer-control" "$HOST_DISPLAY" \
    window-query "$TITLE")
[[ "$host_position" == "400 300" ]] ||
    fail "guest warp was not mirrored to host (got $host_position)"

echo "relative-pointer test: PASS (+10,-5 raw delta; mirrored warp suppressed)"
