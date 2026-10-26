#include "http.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

int http_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int token_char(unsigned char c)
{
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

int http_read_request(int fd, Request *r)
{
    char buf[REQUEST_MAX + 1];
    size_t used = 0;
    memset(r, 0, sizeof(*r));

    while (used < REQUEST_MAX) {
        ssize_t n = recv(fd, buf + used, REQUEST_MAX - used, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (!strstr(buf, "\r\n\r\n")) return -1;

    char *line = buf;
    char *end = strstr(line, "\r\n");
    if (!end) return -1;
    *end = '\0';

    char *p = line;
    char *method = p;
    while (*p && *p != ' ') {
        if (!token_char((unsigned char)*p)) return -1;
        ++p;
    }
    if (!*p) return -1;
    *p++ = '\0';
    while (*p == ' ') ++p;
    char *target = p;
    while (*p && *p != ' ') ++p;
    if (!*p) return -1;
    *p++ = '\0';
    while (*p == ' ') ++p;
    char *version = p;
    if (!*method || !*target || !*version || strchr(version, ' ')) return -1;
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1"))
        return -1;
    if (strlen(method) >= sizeof(r->method) ||
        strlen(target) >= sizeof(r->target)) return -1;

    strcpy(r->method, method);
    strcpy(r->target, target);
    strcpy(r->version, version);

    line = end + 2;
    while (*line && strncmp(line, "\r\n", 2)) {
        end = strstr(line, "\r\n");
        if (!end || r->header_count >= HEADER_MAX) return -1;
        *end = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return -1;
        *colon = '\0';
        for (char *q = line; *q; ++q)
            if (!token_char((unsigned char)*q)) return -1;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = '\0';
        if (strlen(line) >= sizeof(r->headers[0].name) ||
            strlen(value) >= sizeof(r->headers[0].value)) return -1;
        strcpy(r->headers[r->header_count].name, line);
        strcpy(r->headers[r->header_count].value, value);
        ++r->header_count;
        line = end + 2;
    }
    return 0;
}

const char *http_header(const Request *r, const char *name)
{
    for (int i = 0; i < r->header_count; ++i)
        if (!strcasecmp(r->headers[i].name, name))
            return r->headers[i].value;
    return NULL;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Error";
    }
}

void http_date(time_t t, char *out, size_t n)
{
    struct tm tm;
    if (!gmtime_r(&t, &tm) ||
        !strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm))
        snprintf(out, n, "Thu, 01 Jan 1970 00:00:00 GMT");
}

time_t http_parse_date(const char *s)
{
    if (!s) return (time_t)-1;
    struct tm tm = {0};
    char *end = strptime(s, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!end || *end) return (time_t)-1;
    return timegm(&tm);
}

void http_response(int fd, int status, const char *type,
                   const void *body, size_t len, int send_body,
                   const char *extra)
{
    char date[64], header[8192];
    http_date(time(NULL), date, sizeof(date));

    int n;
    if (status == 304) {
        n = snprintf(header, sizeof(header),
            "HTTP/1.0 304 Not Modified\r\n"
            "Server: SWS/1.0\r\nDate: %s\r\nConnection: close\r\n%s\r\n",
            date, extra ? extra : "");
    } else {
        n = snprintf(header, sizeof(header),
            "HTTP/1.0 %d %s\r\n"
            "Server: SWS/1.0\r\nDate: %s\r\nConnection: close\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Content-Type: %s\r\nContent-Length: %zu\r\n%s\r\n",
            status, http_reason(status), date,
            type ? type : "text/plain; charset=utf-8",
            len, extra ? extra : "");
    }
    if (n < 0 || (size_t)n >= sizeof(header)) return;
    if (http_send_all(fd, header, (size_t)n)) return;
    if (status != 304 && send_body && body && len)
        (void)http_send_all(fd, body, len);
}

void http_error(int fd, int status, const char *message, int send_body,
                const char *extra)
{
    char body[1024];
    int n = snprintf(body, sizeof(body), "%d %s\n%s\n",
                     status, http_reason(status), message ? message : "");
    if (n < 0) return;
    if ((size_t)n >= sizeof(body)) n = (int)sizeof(body) - 1;
    http_response(fd, status, "text/plain; charset=utf-8",
                  body, (size_t)n, send_body, extra);
}
