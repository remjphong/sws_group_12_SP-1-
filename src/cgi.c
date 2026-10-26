#define _GNU_SOURCE
#include "cgi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CGI_MAX_OUTPUT (8U * 1024U * 1024U)
#define CGI_IDLE_TIMEOUT_MS 10000

struct bytebuf {
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

static const char *reason_phrase(int status)
{
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "CGI Response";
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

static int send_error_response(int fd, int status, const char *extra_header)
{
    char body[512], date[64], headers[1024];
    int body_len, headers_len;
    format_http_date(time(NULL), date);
    body_len = snprintf(body, sizeof(body), "%d %s\n", status, reason_phrase(status));
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) return -1;
    headers_len = snprintf(headers, sizeof(headers),
        "HTTP/1.0 %d %s\r\nDate: %s\r\nServer: sws/1.0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\n"
        "Connection: close\r\n%s\r\n",
        status, reason_phrase(status), date, body_len,
        extra_header != NULL ? extra_header : "");
    if (headers_len < 0 || (size_t)headers_len >= sizeof(headers)) return -1;
    if (send_all(fd, headers, (size_t)headers_len) < 0) return -1;
    return send_all(fd, body, (size_t)body_len);
}

static int buffer_append(struct bytebuf *b, const char *data, size_t length)
{
    size_t needed;
    size_t new_cap;
    char *next;
    if (length > CGI_MAX_OUTPUT || b->len > CGI_MAX_OUTPUT - length) return -1;
    needed = b->len + length;
    if (needed <= b->cap) {
        memcpy(b->data + b->len, data, length);
        b->len += length;
        return 0;
    }
    new_cap = b->cap == 0 ? 4096 : b->cap;
    while (new_cap < needed) {
        if (new_cap > CGI_MAX_OUTPUT / 2) {
            new_cap = CGI_MAX_OUTPUT;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < needed) return -1;
    next = realloc(b->data, new_cap);
    if (next == NULL) return -1;
    b->data = next;
    b->cap = new_cap;
    memcpy(b->data + b->len, data, length);
    b->len += length;
    return 0;
}

static bool path_is_within_root(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (strncmp(root, path, n) != 0) return false;
    if (n == 1 && root[0] == '/') return true;
    return path[n] == '\0' || path[n] == '/';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int decode_path(const char *input, char output[SWS_URI_MAX])
{
    const char *end = strchr(input, '?');
    size_t out_len = 0;
    if (end == NULL) end = input + strlen(input);
    if (input[0] != '/') return -1;
    for (const char *p = input; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') {
            int hi, lo;
            if (p + 2 >= end) return -1;
            hi = hex_value(p[1]);
            lo = hex_value(p[2]);
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            p += 2;
        }
        if (c == '\0' || c == '\\' || c < 0x20 || c == 0x7f) return -1;
        if (out_len + 1 >= SWS_URI_MAX) return -1;
        output[out_len++] = (char)c;
    }
    output[out_len] = '\0';

    const char *segment = output;
    for (const char *p = output; ; ++p) {
        if (*p == '/' || *p == '\0') {
            if ((size_t)(p - segment) == 2 && segment[0] == '.' && segment[1] == '.')
                return -2;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 0;
}

static int get_cgi_prefix(const struct server_config *config,
                          char out[SWS_URI_MAX], size_t *out_len)
{
    size_t n;
    if (config->cgi_directory == NULL || config->cgi_directory[0] != '/') return -1;
    n = strlen(config->cgi_directory);
    if (n == 0 || n >= SWS_URI_MAX) return -1;
    while (n > 1 && config->cgi_directory[n - 1] == '/') --n;
    memcpy(out, config->cgi_directory, n);
    out[n] = '\0';
    *out_len = n;
    return 0;
}

static int extract_path_query(const char *uri, char path[SWS_URI_MAX],
                              char query[SWS_URI_MAX])
{
    const char *qmark = strchr(uri, '?');
    size_t path_len = qmark == NULL ? strlen(uri) : (size_t)(qmark - uri);
    size_t query_len = qmark == NULL ? 0 : strlen(qmark + 1);
    if (path_len == 0 || path_len >= SWS_URI_MAX || query_len >= SWS_URI_MAX)
        return -1;
    memcpy(path, uri, path_len);
    path[path_len] = '\0';
    if (qmark != NULL) memcpy(query, qmark + 1, query_len);
    query[query_len] = '\0';
    return 0;
}

int cgi_is_request(const struct http_request *request,
                   const struct server_config *config)
{
    char prefix[SWS_URI_MAX];
    char raw_path[SWS_URI_MAX];
    char decoded[SWS_URI_MAX];
    size_t prefix_len;
    int result;

    if (request == NULL || config == NULL || request->uri[0] != '/') return 0;
    if (get_cgi_prefix(config, prefix, &prefix_len) < 0) return 0;
    if (extract_path_query(request->uri, raw_path, (char[SWS_URI_MAX]){0}) < 0)
        return 0;
    result = decode_path(raw_path, decoded);
    if (result != 0) return 0;
    if (strncmp(decoded, prefix, prefix_len) != 0) return 0;
    return decoded[prefix_len] == '\0' || decoded[prefix_len] == '/';
}

/* Find executable CGI script and split PATH_INFO after the first executable path. */
static int locate_script(const struct http_request *request,
                         const struct server_config *config,
                         char root[PATH_MAX], char script_path[PATH_MAX],
                         char script_name[SWS_URI_MAX], char path_info[SWS_URI_MAX],
                         char query[SWS_URI_MAX])
{
    char raw_path[SWS_URI_MAX], decoded[SWS_URI_MAX], prefix[SWS_URI_MAX];
    char candidate[PATH_MAX], resolved[PATH_MAX];
    size_t prefix_len;
    int decode_result;
    struct stat st;

    if (config->document_root == NULL || realpath(config->document_root, root) == NULL)
        return -3;
    if (extract_path_query(request->uri, raw_path, query) < 0) return -1;
    decode_result = decode_path(raw_path, decoded);
    if (decode_result != 0) return decode_result;
    if (get_cgi_prefix(config, prefix, &prefix_len) < 0) return -1;
    if (strncmp(decoded, prefix, prefix_len) != 0 ||
        !(decoded[prefix_len] == '\0' || decoded[prefix_len] == '/')) return -4;

    /* Must name a script below /cgi-bin, not the directory itself. */
    if (decoded[prefix_len] == '\0') return -2;

    for (char *slash = decoded + prefix_len + 1; ; ++slash) {
        if (*slash != '/' && *slash != '\0') continue;
        size_t script_len = (size_t)(slash - decoded);
        if (script_len == 0 || script_len >= SWS_URI_MAX) return -1;
        int candidate_len = snprintf(candidate, sizeof(candidate), "%s%.*s", root,
                                     (int)script_len, decoded);
        if (candidate_len < 0 || (size_t)candidate_len >= sizeof(candidate)) return -1;

        if (realpath(candidate, resolved) != NULL && path_is_within_root(root, resolved) &&
            stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
            if (access(resolved, X_OK) != 0) return -5;
            (void)snprintf(script_path, PATH_MAX, "%s", resolved);
            (void)snprintf(script_name, SWS_URI_MAX, "%.*s", (int)script_len, decoded);
            (void)snprintf(path_info, SWS_URI_MAX, "%s", *slash == '/' ? slash : "");
            return 0;
        }
        if (*slash == '\0') break;
    }
    return -2;
}

static int set_cgi_environment(const char *root, const char *script_path,
                               const char *script_name, const char *path_info,
                               const char *query, const struct http_request *request)
{
    char path_translated[PATH_MAX];
    if (clearenv() != 0) return -1;
    if (setenv("PATH", "/usr/bin:/bin", 1) != 0 ||
        setenv("LANG", "C", 1) != 0 ||
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1) != 0 ||
        setenv("SERVER_PROTOCOL", "HTTP/1.0", 1) != 0 ||
        setenv("SERVER_SOFTWARE", "sws/1.0", 1) != 0 ||
        setenv("SERVER_NAME", "localhost", 1) != 0 ||
        setenv("SERVER_PORT", "80", 1) != 0 ||
        setenv("REQUEST_METHOD", request->method, 1) != 0 ||
        setenv("QUERY_STRING", query, 1) != 0 ||
        setenv("SCRIPT_NAME", script_name, 1) != 0 ||
        setenv("SCRIPT_FILENAME", script_path, 1) != 0 ||
        setenv("PATH_INFO", path_info, 1) != 0 ||
        setenv("DOCUMENT_ROOT", root, 1) != 0) return -1;

    int translated_len = snprintf(path_translated, sizeof(path_translated), "%s%s", root, path_info);
    if (translated_len < 0 || (size_t)translated_len >= sizeof(path_translated)) return -1;
    if (setenv("PATH_TRANSLATED", path_translated, 1) != 0) return -1;
    if (request->if_modified_since[0] != '\0' &&
        setenv("HTTP_IF_MODIFIED_SINCE", request->if_modified_since, 1) != 0) return -1;
    return 0;
}

static int run_cgi(const char *root, const char *script_path, const char *script_name,
                   const char *path_info, const char *query,
                   const struct http_request *request, struct bytebuf *output)
{
    int pipe_fds[2];
    pid_t pid;
    int child_status = 0;
    bool timed_out = false;
    bool too_large = false;

    if (pipe(pipe_fds) < 0) return -1;
    (void)fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]); close(pipe_fds[1]);
        return -1;
    }
    if (pid == 0) {
        char *const argv[] = {(char *)script_path, NULL};
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipe_fds[1]);
        if (set_cgi_environment(root, script_path, script_name, path_info, query, request) < 0)
            _exit(126);
        execv(script_path, argv);
        _exit(127);
    }

    close(pipe_fds[1]);
    for (;;) {
        struct pollfd pfd;
        char chunk[8192];
        int poll_result;
        pfd.fd = pipe_fds[0];
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;
        do {
            poll_result = poll(&pfd, 1, CGI_IDLE_TIMEOUT_MS);
        } while (poll_result < 0 && errno == EINTR);

        if (poll_result == 0) {
            timed_out = true;
            break;
        }
        if (poll_result < 0) break;

        ssize_t n = read(pipe_fds[0], chunk, sizeof(chunk));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (buffer_append(output, chunk, (size_t)n) < 0) {
            too_large = true;
            break;
        }
    }
    close(pipe_fds[0]);

    if (timed_out || too_large) (void)kill(pid, SIGKILL);
    while (waitpid(pid, &child_status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }
    if (timed_out) return -2;
    if (too_large) return -3;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) return -1;
    return 0;
}

static char *find_header_separator(char *data, size_t length, size_t *sep_len)
{
    for (size_t i = 0; i + 3 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            *sep_len = 4;
            return data + i;
        }
    }
    for (size_t i = 0; i + 1 < length; ++i) {
        if (data[i] == '\n' && data[i + 1] == '\n') {
            *sep_len = 2;
            return data + i;
        }
    }
    return NULL;
}

static bool valid_header_name(const char *name, size_t length)
{
    if (length == 0) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL)) return false;
    }
    return true;
}

static bool valid_header_value(const char *value, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

static int send_cgi_response(int fd, struct bytebuf *output, bool is_head)
{
    size_t separator_len = 0;
    char *separator = find_header_separator(output->data, output->len, &separator_len);
    char *headers_end;
    char *body;
    size_t header_length, body_length;
    int status = 200;
    char status_reason[128] = "OK";
    bool has_content_type = false;
    bool has_location = false;
    struct bytebuf filtered = {0};
    char date[64], response_headers[16384];
    int response_headers_len;

    if (separator == NULL) return send_error_response(fd, 502, NULL);
    header_length = (size_t)(separator - output->data);
    body = separator + separator_len;
    body_length = output->len - (size_t)(body - output->data);
    headers_end = output->data + header_length;

    char *line = output->data;
    while (line < headers_end) {
        char *line_end = memchr(line, '\n', (size_t)(headers_end - line));
        size_t line_length = line_end == NULL ? (size_t)(headers_end - line) : (size_t)(line_end - line);
        if (line_length > 0 && line[line_length - 1] == '\r') --line_length;
        if (line_length == 0) break;

        char *colon = memchr(line, ':', line_length);
        if (colon == NULL || !valid_header_name(line, (size_t)(colon - line))) {
            free(filtered.data);
            return send_error_response(fd, 502, NULL);
        }
        size_t name_len = (size_t)(colon - line);
        char *value = colon + 1;
        size_t value_len = line_length - name_len - 1;
        while (value_len > 0 && (*value == ' ' || *value == '\t')) {
            ++value; --value_len;
        }
        while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
            --value_len;
        if (!valid_header_value(value, value_len)) {
            free(filtered.data);
            return send_error_response(fd, 502, NULL);
        }

        if (name_len == 6 && strncasecmp(line, "Status", 6) == 0) {
            char tmp[160];
            size_t copy_len = value_len < sizeof(tmp) - 1 ? value_len : sizeof(tmp) - 1;
            memcpy(tmp, value, copy_len); tmp[copy_len] = '\0';
            char *endptr;
            long code = strtol(tmp, &endptr, 10);
            if (endptr == tmp || code < 100 || code > 599) {
                free(filtered.data);
                return send_error_response(fd, 502, NULL);
            }
            status = (int)code;
            while (*endptr == ' ') ++endptr;
            if (*endptr != '\0') (void)snprintf(status_reason, sizeof(status_reason), "%s", endptr);
            else (void)snprintf(status_reason, sizeof(status_reason), "%s", reason_phrase(status));
        } else if (name_len == 12 && strncasecmp(line, "Content-Type", 12) == 0) {
            has_content_type = true;
            if (filtered.len + line_length + 2 >= 15000 ||
                buffer_append(&filtered, line, line_length) < 0 ||
                buffer_append(&filtered, "\r\n", 2) < 0) {
                free(filtered.data);
                return send_error_response(fd, 502, NULL);
            }
        } else if (name_len == 8 && strncasecmp(line, "Location", 8) == 0) {
            has_location = true;
            if (filtered.len + line_length + 2 >= 15000 ||
                buffer_append(&filtered, line, line_length) < 0 ||
                buffer_append(&filtered, "\r\n", 2) < 0) {
                free(filtered.data);
                return send_error_response(fd, 502, NULL);
            }
        } else {
            bool skip = (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0) ||
                        (name_len == 10 && strncasecmp(line, "Connection", 10) == 0) ||
                        (name_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) ||
                        (name_len == 4 && strncasecmp(line, "Date", 4) == 0) ||
                        (name_len == 6 && strncasecmp(line, "Server", 6) == 0);
            if (!skip) {
                if (filtered.len + line_length + 2 >= 15000 ||
                    buffer_append(&filtered, line, line_length) < 0 ||
                    buffer_append(&filtered, "\r\n", 2) < 0) {
                    free(filtered.data);
                    return send_error_response(fd, 502, NULL);
                }
            }
        }
        line = line_end == NULL ? headers_end : line_end + 1;
    }

    if (has_location && status == 200) {
        status = 302;
        (void)snprintf(status_reason, sizeof(status_reason), "Found");
    }
    if (status == 204 || status == 304) body_length = 0;
    format_http_date(time(NULL), date);
    response_headers_len = snprintf(response_headers, sizeof(response_headers),
        "HTTP/1.0 %d %s\r\nDate: %s\r\nServer: sws/1.0\r\n"
        "%s%s"
        "%s"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        status, status_reason, date,
        has_content_type ? "" : "Content-Type: text/plain; charset=utf-8\r\n",
        filtered.data != NULL ? filtered.data : "",
        "", body_length);
    free(filtered.data);
    if (response_headers_len < 0 || (size_t)response_headers_len >= sizeof(response_headers))
        return send_error_response(fd, 502, NULL);
    if (send_all(fd, response_headers, (size_t)response_headers_len) < 0) return -1;
    if (!is_head && body_length > 0 && send_all(fd, body, body_length) < 0) return -1;
    return 0;
}

int cgi_handle_request(int client_fd, const struct http_request *request,
                       const struct server_config *config)
{
    char root[PATH_MAX];
    char script_path[PATH_MAX];
    char script_name[SWS_URI_MAX];
    char path_info[SWS_URI_MAX];
    char query[SWS_URI_MAX];
    struct bytebuf output = {0};
    int locate_result;
    int run_result;

    if (request == NULL || config == NULL) return send_error_response(client_fd, 500, NULL);
    if (!cgi_is_request(request, config)) return send_error_response(client_fd, 404, NULL);
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) {
        return send_error_response(client_fd, 501, NULL);
    }

    locate_result = locate_script(request, config, root, script_path, script_name,
                                  path_info, query);
    if (locate_result == -3) return send_error_response(client_fd, 500, NULL);
    if (locate_result == -1) return send_error_response(client_fd, 400, NULL);
    if (locate_result == -4 || locate_result == -2) return send_error_response(client_fd, 404, NULL);
    if (locate_result == -5) return send_error_response(client_fd, 403, NULL);
    if (locate_result != 0) return send_error_response(client_fd, 500, NULL);

    run_result = run_cgi(root, script_path, script_name, path_info, query, request, &output);
    if (run_result == -2) {
        free(output.data);
        return send_error_response(client_fd, 504, NULL);
    }
    if (run_result == -3) {
        free(output.data);
        return send_error_response(client_fd, 502, NULL);
    }
    if (run_result != 0) {
        free(output.data);
        return send_error_response(client_fd, 502, NULL);
    }

    int result = send_cgi_response(client_fd, &output, strcmp(request->method, "HEAD") == 0);
    free(output.data);
    return result;
}
