#include "server.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    char *end = NULL;
    long port = 8080;
    const char *root = "docroot";

    if (argc > 1) {
        errno = 0;
        port = strtol(argv[1], &end, 10);
        if (errno || !end || *end || port < 1 || port > 65535) {
            fprintf(stderr, "Usage: %s [port] [document-root]\n", argv[0]);
            return 2;
        }
    }
    if (argc > 2) root = argv[2];
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [port] [document-root]\n", argv[0]);
        return 2;
    }
    return server_run((unsigned short)port, root) == 0 ? 0 : 1;
}
