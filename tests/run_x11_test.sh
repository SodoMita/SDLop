#!/bin/sh
# SDLop test harness: run the X11 integration tests under a real X server.
#
# Prefers Xvfb when available; otherwise falls back to the pure-python
# mini_xserver.py in this directory (useful in sandboxes without Xvfb).
#
# Environment:
#   TEST_X11_BIN       path to the test_x11 binary (default: ./test_x11)
#   DISPLAY            if already set to a live server, it is used as-is
#
# Exits with the test's exit code.
#
# This software is provided 'as-is', without any express or implied
# warranty (zlib license, see LICENSE).

set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEST_BIN=${TEST_X11_BIN:-"$HERE/test_x11"}
DISP_NUM=${X11_TEST_DISPLAY:-98}
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

server_ready() {
    # $1 = ready file to wait for
    i=0
    while [ "$i" -lt 100 ]; do
        if [ -s "$1" ]; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.05 2>/dev/null || sleep 1
    done
    return 1
}

if [ -n "${DISPLAY:-}" ]; then
    echo "run_x11_test: using existing DISPLAY=$DISPLAY"
    exec "$TEST_BIN"
fi

export DISPLAY=":$DISP_NUM"
SCRIPTED_INPUT=0

if command -v Xvfb >/dev/null 2>&1 && [ "${X11_TEST_FORCE_MINI:-0}" != 1 ]; then
    echo "run_x11_test: starting Xvfb on $DISPLAY"
    READY=$(mktemp)
    rm -f "$READY"
    # Xvfb -displayfd would be nicer but readiness via socket poll is portable
    Xvfb "$DISPLAY" -screen 0 1280x720x24 -nolisten tcp 2>/dev/null &
    SERVER_PID=$!
    i=0
    while [ "$i" -lt 100 ]; do
        if [ -S "/tmp/.X11-unix/X$DISP_NUM" ]; then
            break
        fi
        i=$((i + 1))
        sleep 0.05 2>/dev/null || sleep 1
    done
elif command -v python3 >/dev/null 2>&1; then
    echo "run_x11_test: Xvfb not found; using mini_xserver.py on $DISPLAY"
    SCRIPTED_INPUT=1
    READY=$(mktemp)
    rm -f "$READY"
    python3 "$HERE/mini_xserver.py" --display "$DISP_NUM" \
        --script basic_input >"$READY" 2>"$READY.err" &
    SERVER_PID=$!
    if ! server_ready "$READY"; then
        echo "run_x11_test: mini_xserver failed to start:" >&2
        cat "$READY.err" >&2
        exit 2
    fi
else
    echo "run_x11_test: no Xvfb and no python3; cannot run X11 tests" >&2
    exit 77  # automake-style "skip"
fi

# scripted input assertions only when the mini server drives the input
if [ "$SCRIPTED_INPUT" = 1 ]; then
    export SDLOP_TEST_EXPECT_INPUT=1
fi
"$TEST_BIN"
RC=$?

if [ -n "$SERVER_PID" ] && [ -n "${READY:-}" ] && [ -f "${READY:-/dev/null}" ]; then
    grep -h "MINI-X11-SERVER-STATS" "$READY" 2>/dev/null || true
fi
exit $RC
