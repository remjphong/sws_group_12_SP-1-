#ifndef SWS_HTTP_H
#define SWS_HTTP_H

#include "common.h"
#include <stddef.h>
#include <time.h>

int http_read_request(int fd, Request *r);
const char *http_header(const Request *r, const char *name);
int http_send_all(int fd, const void *buf, size_t len);
void http_response(int fd, int status, const char *type,
                   const void *body, size_t len, int send_body,
                   const char *extra);
void http_error(int fd, int status, const char *message, int send_body,
                const char *extra);
const char *http_reason(int status);
time_t http_parse_date(const char *s);
void http_date(time_t t, char *out, size_t n);

#endif
