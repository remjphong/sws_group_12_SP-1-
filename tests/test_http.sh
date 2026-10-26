#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
PORT=18081
TMP="$(mktemp -d)"
PID=""
cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT
./sws "$PORT" docroot >"$TMP/server.log" 2>&1 &
PID=$!
for i in $(seq 1 30); do
  if curl -sS "http://127.0.0.1:$PORT/" >/dev/null 2>&1; then break; fi
  sleep 0.2
done
code=$(curl -sS -o "$TMP/get" -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$code" = 200 ]
grep -qi '<html\|<!doctype' "$TMP/get"
echo "PASS HTTP GET 200"
code=$(curl -sS -I -o "$TMP/head" -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$code" = 200 ]
! grep -qi '<html\|<!doctype' "$TMP/head"
echo "PASS HTTP HEAD 200 without body"
code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/missing-file-xyz")
[ "$code" = 404 ]
echo "PASS HTTP 404"
code=$(curl -sS -X POST -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$code" = 405 ]
echo "PASS HTTP 405"
code=$(curl --http1.0 -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$code" = 200 ]
echo "PASS HTTP/1.0"
