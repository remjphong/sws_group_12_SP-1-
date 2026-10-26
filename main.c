
#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_DOCROOT "docroot"

static void print_usage(const char *program)
{
    printf(
        "Usage: %s [options]\n"
        "  -a ADDRESS  IPv4 bind address (default 127.0.0.1)\n"
        "  -p PORT     TCP port (default 8080)\n"
        "  -r DIR      Document root (default docroot)\n"
        "  -d          Enable debug logging\n"
        "  -D          Run as daemon\n"
        "  -h          Show this help\n",
        program
    );
}

static int parse_port(const char *value, int *port)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' ||
        parsed < 1 || parsed > 65535) {
        return -1;
    }

    *port = (int)parsed;
    return 0;
}

static int start_daemon(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork daemon");
        return -1;
    }

    if (pid > 0) {
        /* Parent thoat; daemon tiep tuc o child. */
        _exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        return -1;
    }

    pid = fork();

    if (pid < 0) {
        perror("fork daemon second");
        return -1;
    }

    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    umask(027);
    (void)chdir("/");

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        perror("open /dev/null");
        return -1;
    }

    if (dup2(fd, STDIN_FILENO) < 0 ||
        dup2(fd, STDOUT_FILENO) < 0 ||
        dup2(fd, STDERR_FILENO) < 0) {
        perror("dup2");
        close(fd);
        return -1;
    }

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return 0;
}

int main(int argc, char **argv)
{
    ServerConfig config = {
        .host = DEFAULT_HOST,
        .port = DEFAULT_PORT,
        .backlog = 128,
        .docroot = DEFAULT_DOCROOT,
        .debug = 0
    };

    int daemon_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "a:p:r:dDh")) != -1) {
        switch (opt) {
        case 'a':
            config.host = optarg;
            break;

        case 'p':
            if (parse_port(optarg, &config.port) < 0) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'r':
            config.docroot = optarg;
            break;

        case 'd':
            config.debug = 1;
            break;

        case 'D':
            daemon_mode = 1;
            break;

        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;

        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind != argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    struct stat st;

    if (stat(config.docroot, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Document root is not a directory: %s\n",
                config.docroot);
        return EXIT_FAILURE;
    }

    /*
     * Debug log ra stderr; daemon redirect stderr sang /dev/null.
     * Nen dung -d khi chay foreground de quan sat log.
     */
    if (daemon_mode && start_daemon() < 0) {
        return EXIT_FAILURE;
    }

    return server_run(&config, http_handle_client) == 0
           ? EXIT_SUCCESS
           : EXIT_FAILURE;
}

