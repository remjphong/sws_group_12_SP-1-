#include "server.h"
#include "files.h"
#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static void handle_client(int fd, const char *root, unsigned short port)
{
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    Request r;
    if (http_read_request(fd, &r)) {
        http_error(fd, 400, "Malformed HTTP request", 1, NULL);
        return;
    }
    if (strcmp(r.method, "GET") && strcmp(r.method, "HEAD")) {
        http_error(fd, 405, "Only GET and HEAD are supported", 1,
                   "Allow: GET, HEAD\r\n");
        return;
    }
    files_handle(fd, root, &r, port);
}

int server_run(unsigned short port, const char *root)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) { perror("socket"); return -1; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 64) < 0) {
        perror("listen"); close(fd); return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    printf("SWS listening at http://127.0.0.1:%u/ (root=%s)\n",
           (unsigned)port, root);
    fflush(stdout);

    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(fd);
            handle_client(client, root, port);
            close(client);
            _exit(0);
        }
        close(client);
    }
}
