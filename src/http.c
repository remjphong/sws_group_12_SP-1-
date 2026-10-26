#define _POSIX_C_SOURCE 200809L

#include "http.h"
#include "files.h"
#include "cgi.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>

#define HTTP_HEADER_BUFFER 8192

/* --------------------------------------------------
 * Gửi dữ liệu đầy đủ qua socket.
 * send() có thể chỉ gửi được một phần dữ liệu.
 * -------------------------------------------------- */
static int send_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, p + sent, length - sent, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

/* --------------------------------------------------
 * Lấy lý do tương ứng với HTTP status code.
 * -------------------------------------------------- */
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
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown Status";
    }
}

static void make_http_date(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm utc;

    if (gmtime_r(&now, &utc) == NULL ||
        strftime(buffer, size,
                 "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) {
        snprintf(buffer, size, "Thu, 01 Jan 1970 00:00:00 GMT");
    }
}

static int has_header_end(const char *buffer)
{
    return strstr(buffer, "\r\n\r\n") != NULL ||
           strstr(buffer, "\n\n") != NULL;
}

static int has_control_chars(const char *s)
{
    for (; *s != '\0'; ++s) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20 || c == 0x7f)
            return 1;
    }

    return 0;
}

static char *trim(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s))
        ++s;

    size_t len = strlen(s);

    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }

    return s;
}

/* --------------------------------------------------
 * Đọc request header đến CRLF CRLF hoặc LF LF.
 * GET/HEAD không cần đọc request body.
 * -------------------------------------------------- */
static int read_headers(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    if (capacity < 2)
        return -1;

    buffer[0] = '\0';

    while (used < capacity - 1) {
        ssize_t n = recv(fd, buffer + used,
                         capacity - 1 - used, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        used += (size_t)n;
        buffer[used] = '\0';

        if (has_header_end(buffer))
            return 0;
    }

    return -1;
}

/* --------------------------------------------------
 * Phân tích request line và các header cần dùng.
 * Trả về 0 nếu hợp lệ, -1 nếu không hợp lệ.
 * -------------------------------------------------- */
static int parse_request(int fd, HttpRequest *request)
{
    char buffer[HTTP_HEADER_BUFFER];
    char *saveptr = NULL;
    char *line;
    char extra;

    memset(request, 0, sizeof(*request));

    if (read_headers(fd, buffer, sizeof(buffer)) < 0)
        return -1;

    char *end = strstr(buffer, "\r\n\r\n");

    if (end == NULL)
        end = strstr(buffer, "\n\n");

    if (end == NULL)
        return -1;

    *end = '\0';

    line = strtok_r(buffer, "\n", &saveptr);

    if (line == NULL)
        return -1;

    size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\r')
        line[len - 1] = '\0';

    int fields = sscanf(line, "%15s %2047s %15s %c",
                        request->method,
                        request->uri,
                        request->version,
                        &extra);

    if (fields != 3)
        return -1;

    if (request->uri[0] != '/' ||
        has_control_chars(request->uri)) {
        return -1;
    }

    if (strcmp(request->version, "HTTP/1.0") != 0 &&
        strcmp(request->version, "HTTP/1.1") != 0) {
        return -1;
    }

    int has_host = 0;

    while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL) {
        len = strlen(line);

        if (len > 0 && line[len - 1] == '\r')
            line[len - 1] = '\0';

        if (*line == '\0')
            break;

        char *colon = strchr(line, ':');

        if (colon == NULL || colon == line)
            return -1;

        *colon = '\0';

        char *name = trim(line);
        char *value = trim(colon + 1);

        if (has_control_chars(name) ||
            has_control_chars(value)) {
            return -1;
        }

        if (strcasecmp(name, "Host") == 0 && *value != '\0')
            has_host = 1;

        if (strcasecmp(name, "If-Modified-Since") == 0) {
            snprintf(request->if_modified_since,
                     sizeof(request->if_modified_since),
                     "%s", value);
        }
    }

    /* HTTP/1.1 yêu cầu header Host. */
    if (strcmp(request->version, "HTTP/1.1") == 0 &&
        !has_host) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------
 * Gửi response HTTP/1.0.
 * HEAD chỉ gửi header, Content-Length vẫn là độ dài
 * body tương ứng với response GET.
 * -------------------------------------------------- */
int http_send_response(int client_fd,
                       int status,
                       const char *content_type,
                       const void *body,
                       size_t body_len,
                       int head_only)
{
    char date[64];
    char headers[1024];

    if (body_len > 0 && body == NULL)
        return -1;

    if (content_type == NULL)
        content_type = "text/plain; charset=utf-8";

    make_http_date(date, sizeof(date));

    int n = snprintf(
        headers, sizeof(headers),
        "HTTP/1.0 %d %s\r\n"
        "Date: %s\r\n"
        "Server: sws\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason_phrase(status), date,
        content_type, body_len
    );

    if (n < 0 || (size_t)n >= sizeof(headers))
        return -1;

    if (send_all(client_fd, headers, (size_t)n) < 0)
        return -1;

    if (!head_only && body_len > 0) {
        if (send_all(client_fd, body, body_len) < 0)
            return -1;
    }

    return 0;
}

/* --------------------------------------------------
 * Gửi trang lỗi HTML đơn giản.
 * -------------------------------------------------- */
int http_send_error(int client_fd,
                    int status,
                    const char *message,
                    int head_only)
{
    char body[1024];

    if (message == NULL)
        message = reason_phrase(status);

    int n = snprintf(
        body, sizeof(body),
        "<!doctype html>\n"
        "<html><head><title>%d %s</title></head>\n"
        "<body><h1>%d %s</h1><p>%s</p></body></html>\n",
        status, reason_phrase(status),
        status, reason_phrase(status), message
    );

    if (n < 0)
        return -1;

    size_t body_len = (size_t)n;

    if (body_len >= sizeof(body))
        body_len = sizeof(body) - 1;

    return http_send_response(
        client_fd,
        status,
        "text/html; charset=utf-8",
        body,
        body_len,
        head_only
    );
}

/* --------------------------------------------------
 * Entry point mà server của A gọi.
 * -------------------------------------------------- */
void http_handle_client(int client_fd,
                        const char *docroot,
                        int debug)
{
    HttpRequest request;

    if (docroot == NULL || *docroot == '\0') {
        (void)http_send_error(
            client_fd, 500, "Invalid document root", 0
        );
        return;
    }

    if (parse_request(client_fd, &request) < 0) {
        if (debug)
            fprintf(stderr, "[HTTP] Invalid or incomplete request\n");

        (void)http_send_error(
            client_fd, 400, "Malformed HTTP request", 0
        );
        return;
    }

    int head_only = strcmp(request.method, "HEAD") == 0;
    int is_get = strcmp(request.method, "GET") == 0;

    if (!is_get && !head_only) {
        static const char body[] =
            "<html><body><h1>405 Method Not Allowed</h1></body></html>\n";

        char date[64];
        char headers[512];

        make_http_date(date, sizeof(date));

        int n = snprintf(
            headers, sizeof(headers),
            "HTTP/1.0 405 Method Not Allowed\r\n"
            "Date: %s\r\n"
            "Server: sws\r\n"
            "Allow: GET, HEAD\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            date, sizeof(body) - 1
        );

        if (n > 0 && (size_t)n < sizeof(headers)) {
            if (send_all(client_fd, headers, (size_t)n) == 0 &&
                !head_only) {
                (void)send_all(client_fd, body, sizeof(body) - 1);
            }
        }

        return;
    }

    if (debug) {
        fprintf(stderr, "[HTTP] %s %s %s\n",
                request.method, request.uri, request.version);
    }

    /*
     * URI /cgi-bin/... được chuyển cho module CGI.
     * Module CGI phải thực hiện kiểm tra đường dẫn và quyền chạy.
     */
    if (strncmp(request.uri, "/cgi-bin/", 9) == 0) {
        (void)cgi_handle_request(
            client_fd, &request, docroot, debug, head_only
        );
        return;
    }

    (void)files_handle_request(
        client_fd, &request, docroot, debug, head_only
    );
}