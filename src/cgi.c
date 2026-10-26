#include "cgi.h"
#include "http.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int cgi_handle(int fd, const char *script, const Request *r,
               const char *query)
{
    int pipes[2];
    if (pipe(pipes) < 0) {
        http_error(fd, 500, "Cannot create CGI pipe", 1, NULL);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]); close(pipes[1]);
        http_error(fd, 500, "Cannot start CGI", 1, NULL);
        return -1;
    }

    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[1]);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.0", 1);
        setenv("REQUEST_METHOD", r->method, 1);
        setenv("QUERY_STRING", query ? query : "", 1);
        setenv("SCRIPT_NAME", r->target, 1);
        execl(script, script, (char *)NULL);
        _exit(127);
    }

    close(pipes[1]);
    char output[1024 * 1024];
    size_t used = 0;
    while (used < sizeof(output)) {
        ssize_t n = read(pipes[0], output + used, sizeof(output) - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    close(pipes[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    char *body = NULL;
    size_t body_len = 0;
    char type[256] = "text/plain; charset=utf-8";
    char *sep = NULL;
    for (size_t i = 0; i + 3 < used; ++i) {
        if (!memcmp(output + i, "\r\n\r\n", 4)) {
            sep = output + i; body = sep + 4; body_len = used - i - 4; break;
        }
    }
    if (!sep) {
        for (size_t i = 0; i + 1 < used; ++i) {
            if (!memcmp(output + i, "\n\n", 2)) {
                sep = output + i; body = sep + 2; body_len = used - i - 2; break;
            }
        }
    }

    if (sep) {
        *sep = '\0';
        char *ct = strstr(output, "Content-Type:");
        if (ct) {
            ct += strlen("Content-Type:");
            while (*ct == ' ' || *ct == '\t') ++ct;
            char *end = strpbrk(ct, "\r\n");
            if (end) *end = '\0';
            snprintf(type, sizeof(type), "%s", ct);
        }
    } else {
        body = output;
        body_len = used;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        http_error(fd, 500, "CGI program failed", 1, NULL);
        return -1;
    }

    http_response(fd, 200, type, body, body_len,
                  strcmp(r->method, "HEAD") != 0, NULL);
    return 0;
}
