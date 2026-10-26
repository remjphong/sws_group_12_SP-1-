#define _GNU_SOURCE
#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FILE_CHUNK_SIZE 16384
#define LISTING_MAX_SIZE (4U * 1024U * 1024U)

struct strbuf {
    char *data;
    size_t len;
    size_t cap;
};

static int send_all(int fd, const void *data, size_t length)
{
    const char *p = (const char *)data;
    size_t sent = 0;

    while (sent < length) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + sent, length - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p + sent, length - sent, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int sb_reserve(struct strbuf *b, size_t extra)
{
    size_t needed;
    size_t new_cap;
    char *p;

    if (extra > SIZE_MAX - b->len - 1) return -1;
    needed = b->len + extra + 1;
    if (needed > LISTING_MAX_SIZE) return -1;
    if (needed <= b->cap) return 0;

    new_cap = (b->cap == 0) ? 1024 : b->cap;
    while (new_cap < needed) {
        if (new_cap > LISTING_MAX_SIZE / 2) {
            new_cap = LISTING_MAX_SIZE;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < needed) return -1;
    p = realloc(b->data, new_cap);
    if (p == NULL) return -1;
    b->data = p;
    b->cap = new_cap;
    if (b->len == 0) b->data[0] = '\0';
    return 0;
}

static int sb_append_n(struct strbuf *b, const char *s, size_t n)
{
    if (sb_reserve(b, n) < 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int sb_append(struct strbuf *b, const char *s)
{
    return sb_append_n(b, s, strlen(s));
}


static int html_escape(struct strbuf *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        const char *replacement = NULL;
        switch (*p) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }
        if (replacement != NULL) {
            if (sb_append(b, replacement) < 0) return -1;
        } else {
            char c = (char)*p;
            if (sb_append_n(b, &c, 1) < 0) return -1;
        }
    }
    return 0;
}

static bool url_char_is_safe(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

static int url_encode_path(struct strbuf *b, const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        if (url_char_is_safe(*p) || *p == '/') {
            char c = (char)*p;
            if (sb_append_n(b, &c, 1) < 0) return -1;
        } else {
            char encoded[3] = {'%', hex[*p >> 4], hex[*p & 0x0f]};
            if (sb_append_n(b, encoded, sizeof(encoded)) < 0) return -1;
        }
    }
    return 0;
}

static const char *reason_phrase(int status)
{
    switch (status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 414: return "URI Too Long";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

static void format_http_date(time_t value, char out[64])
{
    struct tm tm_value;
    if (gmtime_r(&value, &tm_value) == NULL ||
        strftime(out, 64, "%a, %d %b %Y %H:%M:%S GMT", &tm_value) == 0) {
        (void)snprintf(out, 64, "Thu, 01 Jan 1970 00:00:00 GMT");
    }
}

static int send_error_response_ex(int fd, int status, const char *extra_header,
                                  bool suppress_body)
{
    char body[512];
    char headers[1024];
    char date[64];
    time_t now = time(NULL);
    int body_len;
    int headers_len;

    format_http_date(now, date);
    body_len = snprintf(body, sizeof(body), "%d %s\n", status, reason_phrase(status));
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) return -1;
    headers_len = snprintf(headers, sizeof(headers),
        "HTTP/1.0 %d %s\r\n"
        "Date: %s\r\n"
        "Server: sws/1.0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status, reason_phrase(status), date, body_len,
        extra_header != NULL ? extra_header : "");
    if (headers_len < 0 || (size_t)headers_len >= sizeof(headers)) return -1;
    if (send_all(fd, headers, (size_t)headers_len) < 0) return -1;
    if (!suppress_body && send_all(fd, body, (size_t)body_len) < 0) return -1;
    return 0;
}

static int send_error_response(int fd, int status, const char *extra_header)
{
    return send_error_response_ex(fd, status, extra_header, false);
}

static const char *mime_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".log") == 0)
        return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".xml") == 0) return "application/xml";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(dot, ".mp4") == 0) return "video/mp4";
    return "application/octet-stream";
}

/* Decode URL path and reject malformed escapes, control bytes and '..' segments. */
static int decode_uri_path(const char *uri, char out[SWS_URI_MAX])
{
    size_t out_len = 0;
    const char *end;

    if (uri == NULL || uri[0] != '/') return -1;
    end = strchr(uri, '?');
    if (end == NULL) end = uri + strlen(uri);

    for (const char *p = uri; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') {
            int hi, lo;
            if (p + 2 >= end) return -1;
            hi = isdigit((unsigned char)p[1]) ? p[1] - '0' :
                 (tolower((unsigned char)p[1]) >= 'a' && tolower((unsigned char)p[1]) <= 'f' ?
                  tolower((unsigned char)p[1]) - 'a' + 10 : -1);
            lo = isdigit((unsigned char)p[2]) ? p[2] - '0' :
                 (tolower((unsigned char)p[2]) >= 'a' && tolower((unsigned char)p[2]) <= 'f' ?
                  tolower((unsigned char)p[2]) - 'a' + 10 : -1);
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            p += 2;
        }
        if (c == '\0' || c == '\\' || c < 0x20 || c == 0x7f) return -1;
        if (out_len + 1 >= SWS_URI_MAX) return -1;
        out[out_len++] = (char)c;
    }
    out[out_len] = '\0';

    /* Reject parent-directory traversal after percent decoding. */
    const char *segment = out;
    for (const char *p = out; ; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - segment);
            if (n == 2 && segment[0] == '.' && segment[1] == '.') return -2;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 0;
}

static bool path_is_within_root(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (strncmp(root, path, n) != 0) return false;
    if (n == 1 && root[0] == '/') return true;
    return path[n] == '\0' || path[n] == '/';
}

static int parse_http_date(const char *text, time_t *result)
{
    struct tm tm_value;
    char *end;
    if (text == NULL || *text == '\0') return -1;
    memset(&tm_value, 0, sizeof(tm_value));
    end = strptime(text, "%a, %d %b %Y %H:%M:%S GMT", &tm_value);
    if (end == NULL) return -1;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return -1;
    *result = timegm(&tm_value);
    return 0;
}

static int send_not_modified(int fd, time_t mtime)
{
    char date[64], last_modified[64], headers[1024];
    int n;
    format_http_date(time(NULL), date);
    format_http_date(mtime, last_modified);
    n = snprintf(headers, sizeof(headers),
        "HTTP/1.0 304 Not Modified\r\n"
        "Date: %s\r\n"
        "Server: sws/1.0\r\n"
        "Last-Modified: %s\r\n"
        "Connection: close\r\n\r\n", date, last_modified);
    if (n < 0 || (size_t)n >= sizeof(headers)) return -1;
    return send_all(fd, headers, (size_t)n);
}

static int serve_regular_file(int fd, const char *path, const char *request_method,
                              const char *if_modified_since)
{
    int file_fd;
    struct stat st;
    char date[64], last_modified[64], headers[2048];
    int n;
    bool is_head = strcmp(request_method, "HEAD") == 0;

    file_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) {
        if (errno == EACCES) return send_error_response_ex(fd, 403, NULL, is_head);
        if (errno == ENOENT || errno == ENOTDIR) return send_error_response_ex(fd, 404, NULL, is_head);
        return send_error_response_ex(fd, 500, NULL, is_head);
    }
    if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        return send_error_response_ex(fd, 403, NULL, is_head);
    }

    if (if_modified_since != NULL && *if_modified_since != '\0') {
        time_t requested_time;
        if (parse_http_date(if_modified_since, &requested_time) == 0 &&
            st.st_mtime <= requested_time) {
            close(file_fd);
            return send_not_modified(fd, st.st_mtime);
        }
    }

    format_http_date(time(NULL), date);
    format_http_date(st.st_mtime, last_modified);
    n = snprintf(headers, sizeof(headers),
        "HTTP/1.0 200 OK\r\n"
        "Date: %s\r\n"
        "Server: sws/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Last-Modified: %s\r\n"
        "Connection: close\r\n\r\n",
        date, mime_type_for_path(path), (long long)st.st_size, last_modified);
    if (n < 0 || (size_t)n >= sizeof(headers) || send_all(fd, headers, (size_t)n) < 0) {
        close(file_fd);
        return -1;
    }

    if (!is_head) {
        char buffer[FILE_CHUNK_SIZE];
        for (;;) {
            ssize_t got = read(file_fd, buffer, sizeof(buffer));
            if (got == 0) break;
            if (got < 0) {
                if (errno == EINTR) continue;
                close(file_fd);
                return -1;
            }
            if (send_all(fd, buffer, (size_t)got) < 0) {
                close(file_fd);
                return -1;
            }
        }
    }
    close(file_fd);
    return 0;
}

static int serve_directory_listing(int fd, const char *filesystem_path,
                                   const char *uri_path, bool is_head)
{
    DIR *directory = opendir(filesystem_path);
    struct dirent *entry;
    struct strbuf body = {0};
    struct strbuf encoded_base = {0};
    char base_uri[SWS_URI_MAX + 2];
    char date[64], headers[2048];
    int result = -1;
    int n;

    if (directory == NULL) {
        return send_error_response_ex(fd, errno == EACCES ? 403 : 500, NULL, is_head);
    }

    (void)snprintf(base_uri, sizeof(base_uri), "%s", uri_path);
    size_t base_len = strlen(base_uri);
    if (base_len == 0 || base_uri[base_len - 1] != '/') {
        if (base_len + 1 >= sizeof(base_uri)) {
            closedir(directory);
            return send_error_response_ex(fd, 414, NULL, is_head);
        }
        base_uri[base_len++] = '/';
        base_uri[base_len] = '\0';
    }
    if (url_encode_path(&encoded_base, base_uri) < 0 ||
        sb_append(&body, "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of ") < 0 ||
        html_escape(&body, uri_path) < 0 ||
        sb_append(&body, "</title></head><body><h1>Index of ") < 0 ||
        html_escape(&body, uri_path) < 0 || sb_append(&body, "</h1><ul>\n") < 0) {
        goto done;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct stat st;
        char child_path[PATH_MAX];
        bool is_dir = false;
        struct strbuf encoded_name = {0};

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        int child_len = snprintf(child_path, sizeof(child_path), "%s/%s", filesystem_path,
                                 entry->d_name);
        if (child_len < 0 || (size_t)child_len >= sizeof(child_path)) {
            free(encoded_name.data);
            goto done;
        }
        if (stat(child_path, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;

        for (const unsigned char *p = (const unsigned char *)entry->d_name; *p; ++p) {
            if (url_char_is_safe(*p)) {
                char c = (char)*p;
                if (sb_append_n(&encoded_name, &c, 1) < 0) break;
            } else {
                static const char hex[] = "0123456789ABCDEF";
                char triplet[3] = {'%', hex[*p >> 4], hex[*p & 15]};
                if (sb_append_n(&encoded_name, triplet, sizeof(triplet)) < 0) break;
            }
        }
        if (sb_append(&body, "<li><a href=\"") < 0 ||
            sb_append_n(&body, encoded_base.data, encoded_base.len) < 0 ||
            sb_append_n(&body, encoded_name.data != NULL ? encoded_name.data : "",
                        encoded_name.len) < 0 ||
            (is_dir && sb_append(&body, "/") < 0) ||
            sb_append(&body, "\">") < 0 || html_escape(&body, entry->d_name) < 0 ||
            (is_dir && sb_append(&body, "/") < 0) ||
            sb_append(&body, "</a></li>\n") < 0) {
            free(encoded_name.data);
            goto done;
        }
        free(encoded_name.data);
        errno = 0;
    }
    if (errno != 0) goto done;
    if (sb_append(&body, "</ul></body></html>\n") < 0) goto done;

    format_http_date(time(NULL), date);
    n = snprintf(headers, sizeof(headers),
        "HTTP/1.0 200 OK\r\n"
        "Date: %s\r\n"
        "Server: sws/1.0\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        date, body.len);
    if (n < 0 || (size_t)n >= sizeof(headers) || send_all(fd, headers, (size_t)n) < 0)
        goto done;
    if (!is_head && send_all(fd, body.data, body.len) < 0) goto done;
    result = 0;

done:
    closedir(directory);
    free(encoded_base.data);
    free(body.data);
    if (result < 0) {
        /* Nếu lỗi tạo listing do kích thước/ bộ nhớ, thử trả lỗi 500. */
        return send_error_response_ex(fd, 500, NULL, is_head);
    }
    return 0;
}

int files_handle_request(int client_fd, const struct http_request *request,
                         const struct server_config *config)
{
    char root[PATH_MAX];
    char decoded_path[SWS_URI_MAX];
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    struct stat st;
    int decode_result;
    bool is_head;

    if (request == NULL || config == NULL || config->document_root == NULL) {
        bool suppress_body = request != NULL && strcmp(request->method, "HEAD") == 0;
        return send_error_response_ex(client_fd, 500, NULL, suppress_body);
    }

    is_head = strcmp(request->method, "HEAD") == 0;
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) {
        return send_error_response(client_fd, 405, "Allow: GET, HEAD\r\n");
    }

    if (realpath(config->document_root, root) == NULL || stat(root, &st) < 0 ||
        !S_ISDIR(st.st_mode)) {
        return send_error_response_ex(client_fd, 500, NULL, is_head);
    }

    decode_result = decode_uri_path(request->uri, decoded_path);
    if (decode_result == -1) return send_error_response_ex(client_fd, 400, NULL, is_head);
    if (decode_result == -2) return send_error_response_ex(client_fd, 403, NULL, is_head);

    int candidate_len = snprintf(candidate, sizeof(candidate), "%s%s", root, decoded_path);
    if (candidate_len < 0 || (size_t)candidate_len >= sizeof(candidate)) {
        return send_error_response_ex(client_fd, 414, NULL, is_head);
    }
    if (realpath(candidate, resolved) == NULL) {
        if (errno == EACCES) return send_error_response_ex(client_fd, 403, NULL, is_head);
        if (errno == ENOENT || errno == ENOTDIR) return send_error_response_ex(client_fd, 404, NULL, is_head);
        return send_error_response_ex(client_fd, 500, NULL, is_head);
    }
    if (!path_is_within_root(root, resolved)) return send_error_response_ex(client_fd, 403, NULL, is_head);
    if (stat(resolved, &st) < 0) return send_error_response_ex(client_fd, 404, NULL, is_head);

    if (S_ISDIR(st.st_mode)) {
        char index_candidate[PATH_MAX];
        char index_resolved[PATH_MAX];
        struct stat index_stat;
        int index_n = snprintf(index_candidate, sizeof(index_candidate), "%s/index.html", resolved);
        if (index_n >= 0 && (size_t)index_n < sizeof(index_candidate) &&
            realpath(index_candidate, index_resolved) != NULL &&
            path_is_within_root(root, index_resolved) &&
            stat(index_resolved, &index_stat) == 0 && S_ISREG(index_stat.st_mode)) {
            return serve_regular_file(client_fd, index_resolved, request->method,
                                      request->if_modified_since);
        }
        return serve_directory_listing(client_fd, resolved, decoded_path, is_head);
    }
    if (!S_ISREG(st.st_mode)) return send_error_response_ex(client_fd, 403, NULL, is_head);

    return serve_regular_file(client_fd, resolved, request->method,
                              request->if_modified_since);
}
