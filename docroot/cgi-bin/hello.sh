#!/bin/sh
printf 'Content-Type: text/plain\r\n\r\n'
printf 'Hello from CGI!\n'
printf 'Method: %s\n' "$REQUEST_METHOD"
printf 'Query: %s\n' "$QUERY_STRING"
