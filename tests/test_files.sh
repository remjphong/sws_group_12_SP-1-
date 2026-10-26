#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
PORT=18082
TMP="$(mktemp -d)"
PID=""
cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p "$TMP/root"
printf 'static-test-content\n' > "$TMP/root/test.txt"
./sws "$PORT" "$TMP/root" >"$TMP/server.log" 2>&1 &
PID=$!
for i in $(seq 1 30); do
  if curl -sS "http://127.0.0.1:$PORT/test.txt" >/dev/null 2>&1; then break; fi
  sleep 0.2
done
code=$(curl -sS -o "$TMP/body" -w '%{http_code}' "http://127.0.0.1:$PORT/test.txt")
[ "$code" = 200 ]
grep -q 'static-test-content' "$TMP/body"
echo "PASS static file 200"
curl -sSI "http://127.0.0.1:$PORT/test.txt" > "$TMP/headers"
grep -qi '^Content-Type: text/plain' "$TMP/headers"
echo "PASS MIME type"
curl -sSI "http://127.0.0.1:$PORT/test.txt" > "$TMP/headers"
modified=$(sed -n 's/^[Ll]ast-[Mm]odified: //p' "$TMP/headers" | tr -d '\r')
code=$(curl -sS -H "If-Modified-Since: $modified" -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/test.txt")
[ "$code" = 304 ]
echo "PASS If-Modified-Since 304"
code=$(curl --path-as-is -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/../../etc/passwd")
[ "$code" = 403 ] || [ "$code" = 404 ]
echo "PASS traversal blocked"
