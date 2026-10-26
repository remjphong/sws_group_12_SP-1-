#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
PORT=18083
TMP="$(mktemp -d)"
PID=""
cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p "$TMP/root/cgi-bin"
cat > "$TMP/root/cgi-bin/hello.sh" <<'CGI'
#!/bin/sh
printf 'Content-Type: text/plain\r\n\r\n'
printf 'hello-from-cgi\n'
printf 'method=%s\n' "$REQUEST_METHOD"
printf 'query=%s\n' "$QUERY_STRING"
CGI
chmod +x "$TMP/root/cgi-bin/hello.sh"
./sws "$PORT" "$TMP/root" >"$TMP/server.log" 2>&1 &
PID=$!
for i in $(seq 1 30); do
  if curl -sS "http://127.0.0.1:$PORT/cgi-bin/hello.sh" >/dev/null 2>&1; then break; fi
  sleep 0.2
done
code=$(curl -sS -o "$TMP/body" -w '%{http_code}' "http://127.0.0.1:$PORT/cgi-bin/hello.sh?name=Phong")
[ "$code" = 200 ]
grep -q 'hello-from-cgi' "$TMP/body"
grep -q 'method=GET' "$TMP/body"
grep -q 'query=name=Phong' "$TMP/body"
echo "PASS CGI GET and environment"
