#include "files.h"
#include "cgi.h"
#include "http.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm"))
        return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css")) return "text/css; charset=utf-8";
    if (!strcasecmp(ext, ".js")) return "application/javascript";
    if (!strcasecmp(ext, ".txt")) return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".json")) return "application/json";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

static int decode_path(const char *src, char *dst, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (!src[i+1] || !src[i+2]) return -1;
            char hex[3] = { src[i+1], src[i+2], 0 };
            char *end;
            long v = strtol(hex, &end, 16);
            if (*end) return -1;
            c = (unsigned char)v;
            i += 2;
        }
        if (c == 0 || c == '\\' || c < 32 || c == 127 || j + 1 >= cap)
            return -1;
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    return 0;
}

static int inside_root(const char *root, const char *path)
{
    size_t n = strlen(root);
    return strncmp(root, path, n) == 0 &&
           (path[n] == '\0' || path[n] == '/');
}

static void listing(int fd, const char *path, const char *uri, int send_body)
{
    DIR *d = opendir(path);
    if (!d) {
        http_error(fd, 403, "Directory cannot be read", send_body, NULL);
        return;
    }
    char body[65536];
    size_t used = (size_t)snprintf(body, sizeof(body),
        "<!doctype html><html><body><h1>Index of %s</h1><ul>", uri);
    struct dirent *e;
    while ((e = readdir(d)) != NULL && used < sizeof(body) - 256) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        used += (size_t)snprintf(body + used, sizeof(body) - used,
            "<li><a href=\"%s%s\">%s</a></li>",
            e->d_name, "", e->d_name);
    }
    closedir(d);
    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "</ul></body></html>");
    http_response(fd, 200, "text/html; charset=utf-8",
                  body, used, send_body, NULL);
}

void files_handle(int fd, const char *root, const Request *r,
                  unsigned short port)
{
    (void)port;
    int send_body = strcmp(r->method, "HEAD") != 0;
    char uri[4096], path[PATH_MAX], resolved[PATH_MAX], root_real[PATH_MAX];
    char *query;
    snprintf(uri, sizeof(uri), "%s", r->target);
    query = strchr(uri, '?');
    if (query) *query++ = '\0';

    if (decode_path(uri, uri, sizeof(uri)) ||
        uri[0] != '/' || strstr(uri, "/../") ||
        !strcmp(uri, "/..") || strstr(uri, "/~")) {
        http_error(fd, 403, "Forbidden path", send_body, NULL);
        return;
    }

    if (!realpath(root, root_real)) {
        http_error(fd, 500, "Invalid document root", send_body, NULL);
        return;
    }
    if (snprintf(path, sizeof(path), "%s%s", root_real, uri) >= (int)sizeof(path)) {
        http_error(fd, 400, "Path too long", send_body, NULL);
        return;
    }

    if (!realpath(path, resolved)) {
        http_error(fd, 404, "File not found", send_body, NULL);
        return;
    }
    if (!inside_root(root_real, resolved)) {
        http_error(fd, 403, "Forbidden path", send_body, NULL);
        return;
    }

    struct stat st;
    if (stat(resolved, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
        http_error(fd, 404, "File not found", send_body, NULL);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        size_t n = strlen(resolved);
        char index_path[PATH_MAX];
        if (snprintf(index_path, sizeof(index_path), "%s/index.html", resolved)
            < (int)sizeof(index_path) && stat(index_path, &st) == 0 &&
            S_ISREG(st.st_mode)) {
            snprintf(resolved, sizeof(resolved), "%s", index_path);
            if (stat(resolved, &st)) {
                http_error(fd, 404, "File not found", send_body, NULL);
                return;
            }
        } else {
            listing(fd, resolved, uri, send_body);
            return;
        }
        (void)n;
    }

    const char *ims = http_header(r, "If-Modified-Since");
    if (ims) {
        time_t t = http_parse_date(ims);
        if (t != (time_t)-1 && st.st_mtime <= t) {
            http_response(fd, 304, NULL, NULL, 0, 0, NULL);
            return;
        }
    }

    if (strstr(uri, "/cgi-bin/") == uri) {
        if (!(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            http_error(fd, 403, "CGI is not executable", send_body, NULL);
            return;
        }
        cgi_handle(fd, resolved, r, query);
        return;
    }

    if (st.st_size < 0 || st.st_size > 64 * 1024 * 1024) {
        http_error(fd, 413, "File too large", send_body, NULL);
        return;
    }
    FILE *f = fopen(resolved, "rb");
    if (!f) {
        http_error(fd, 403, "Cannot read file", send_body, NULL);
        return;
    }
    size_t len = (size_t)st.st_size;
    char *data = malloc(len ? len : 1);
    if (!data) {
        fclose(f);
        http_error(fd, 500, "Out of memory", send_body, NULL);
        return;
    }
    size_t got = fread(data, 1, len, f);
    fclose(f);
    if (got != len) {
        free(data);
        http_error(fd, 500, "Cannot read file", send_body, NULL);
        return;
    }
    char modified[64], extra[128];
    extern void http_date(time_t, char *, size_t);
    http_date(st.st_mtime, modified, sizeof(modified));
    snprintf(extra, sizeof(extra), "Last-Modified: %s\r\n", modified);
    http_response(fd, 200, mime(resolved), data, len, send_body, extra);
    free(data);
}
