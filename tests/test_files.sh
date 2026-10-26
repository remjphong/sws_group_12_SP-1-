#!/bin/sh
set -eu
BASE_URL=${1:-http://127.0.0.1:8080}

command -v curl >/dev/null 2>&1 || { echo "ERROR: cần cài curl" >&2; exit 1; }

body=$(curl -fsS "$BASE_URL/")
printf '%s' "$body" | grep -q 'SWS static-file test' || {
    echo 'FAIL: GET / không trả về index.html' >&2; exit 1;
}

txt=$(curl -fsS "$BASE_URL/about.txt")
printf '%s' "$txt" | grep -q 'static text file' || {
    echo 'FAIL: GET /about.txt không đúng' >&2; exit 1;
}

headers=$(curl -sSI "$BASE_URL/about.txt")
printf '%s' "$headers" | grep -qi '^HTTP/1\.[01] 200' || {
    echo 'FAIL: HEAD /about.txt không trả 200' >&2; exit 1;
}

missing_status=$(curl -sS -o /dev/null -w '%{http_code}' "$BASE_URL/not-found-file.txt")
[ "$missing_status" = '404' ] || {
    echo "FAIL: file không tồn tại phải trả 404 (đã nhận $missing_status)" >&2; exit 1;
}

head_body=$(curl -sS -I "$BASE_URL/about.txt" | wc -c | tr -d ' ')
[ "$head_body" -gt 0 ] || { echo 'FAIL: HEAD không trả headers' >&2; exit 1; }

echo 'PASS: basic static-file tests'
