
#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_BACKLOG 128

static volatile sig_atomic_t stop_requested = 0;
static int debug_enabled = 0;

static void handle_stop_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void reap_children(int signo)
{
    int saved_errno = errno;
    (void)signo;

    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* Thu hoi tat ca tien trinh con da ket thuc. */
    }

    errno = saved_errno;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_stop_signal;
    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction stop");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = reap_children;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction SIGCHLD");
        return -1;
    }

    /* Tranh server bi ket thuc khi client dong ket noi som. */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;

    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction SIGPIPE");
        return -1;
    }

    return 0;
}

static int create_listen_socket(const ServerConfig *config)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)config->port);

    if (config->host == NULL ||
        strcmp(config->host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, config->host,
                         &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n",
                config->host);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    int backlog = config->backlog > 0
                    ? config->backlog
                    : DEFAULT_BACKLOG;

    if (listen(fd, backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int server_run(const ServerConfig *config, ClientHandler handler)
{
    int listen_fd;
    struct sockaddr_in peer;
    socklen_t peer_len;

    if (config == NULL || handler == NULL ||
        config->port < 1 || config->port > 65535 ||
        config->docroot == NULL) {
        fprintf(stderr, "Invalid server configuration\n");
        return -1;
    }

    debug_enabled = config->debug;

    if (install_signal_handlers() < 0) {
        return -1;
    }

    listen_fd = create_listen_socket(config);
    if (listen_fd < 0) {
        return -1;
    }

    if (debug_enabled) {
        fprintf(stderr, "SWS listening on %s:%d, docroot=%s\n",
                config->host, config->port, config->docroot);
    }

    while (!stop_requested) {
        int client_fd;
        pid_t pid;

        peer_len = sizeof(peer);
        client_fd = accept(listen_fd,
                           (struct sockaddr *)&peer,
                           &peer_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (stop_requested) {
                break;
            }

            perror("accept");
            continue;
        }

        if (debug_enabled) {
            char ip[INET_ADDRSTRLEN] = "unknown";

            (void)inet_ntop(AF_INET, &peer.sin_addr,
                            ip, sizeof(ip));

            fprintf(stderr, "Client connected: %s:%u\n",
                    ip, (unsigned)ntohs(peer.sin_port));
        }

        pid = fork();

        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* Child: chi xu ly ket noi cua minh. */
            close(listen_fd);

            handler(client_fd, config->docroot,
                    config->debug);

            close(client_fd);
            _exit(EXIT_SUCCESS);
        }

        /* Parent: tiep tuc accept client tiep theo. */
        close(client_fd);
    }

    close(listen_fd);

    if (debug_enabled) {
        fprintf(stderr, "SWS server stopped\n");
    }

    return 0;
}

