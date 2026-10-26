#!/bin/sh
set -eu
BASE_URL=${1:-http://127.0.0.1:8080}

command -v curl >/dev/null 2>&1 || { echo "ERROR: cần cài curl" >&2; exit 1; }

response=$(curl -fsS "$BASE_URL/cgi-bin/hello.sh?name=sws")
printf '%s' "$response" | grep -q 'Hello from CGI!' || {
    echo 'FAIL: CGI không trả nội dung mong đợi' >&2; exit 1;
}
printf '%s' "$response" | grep -q 'QUERY_STRING=name=sws' || {
    echo 'FAIL: CGI không nhận QUERY_STRING' >&2; exit 1;
}
printf '%s' "$response" | grep -q 'REQUEST_METHOD=GET' || {
    echo 'FAIL: CGI không nhận REQUEST_METHOD' >&2; exit 1;
}

echo 'PASS: basic CGI tests'
