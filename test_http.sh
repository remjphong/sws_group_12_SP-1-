
#!/usr/bin/env bash

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="$ROOT/sws"
DOCROOT="$ROOT/docroot"
PORT="${TEST_PORT:-18080}"

TMPDIR_TEST="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi

    rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT INT TERM

if [[ ! -x "$SERVER" ]]; then
    echo "ERROR: Server binary not found. Run make first."
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl is required."
    exit 1
fi

mkdir -p "$DOCROOT"

if [[ ! -f "$DOCROOT/index.html" ]]; then
    echo "ERROR: $DOCROOT/index.html is missing."
    echo "Create an index.html before running this test."
    exit 1
fi

"$SERVER" -a 127.0.0.1 -p "$PORT" -r "$DOCROOT" \
    >"$TMPDIR_TEST/server.log" 2>&1 &
SERVER_PID=$!

ready=0

for _ in $(seq 1 50); do
    if curl --silent --show-error \
        --max-time 1 \
        "http://127.0.0.1:$PORT/" \
        -o "$TMPDIR_TEST/body" \
        -w '%{http_code}' \
        >"$TMPDIR_TEST/status" 2>/dev/null; then
        ready=1
        break
    fi

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "ERROR: Server exited unexpectedly."
        cat "$TMPDIR_TEST/server.log"
        exit 1
    fi

    sleep 0.1
done

if [[ "$ready" -ne 1 ]]; then
    echo "ERROR: Server did not become ready."
    cat "$TMPDIR_TEST/server.log"
    exit 1
fi

status="$(cat "$TMPDIR_TEST/status")"

if [[ "$status" != "200" ]]; then
    echo "FAIL: GET / returned HTTP $status (expected 200)."
    cat "$TMPDIR_TEST/body"
    exit 1
fi

echo "PASS: GET / returned HTTP 200."

# Kiem tra HEAD khong tra ve response body.
head_status="$(
    curl --silent --show-error \
        --head --max-time 3 \
        -o "$TMPDIR_TEST/head-body" \
        -w '%{http_code}' \
        "http://127.0.0.1:$PORT/"
)"

if [[ "$head_status" != "200" ]]; then
    echo "FAIL: HEAD / returned HTTP $head_status."
    exit 1
fi

if [[ -s "$TMPDIR_TEST/head-body" ]]; then
    echo "FAIL: HEAD response unexpectedly contains a body."
    exit 1
fi

echo "PASS: HEAD / returned HTTP 200 with no body."
echo "All basic HTTP integration tests passed."

