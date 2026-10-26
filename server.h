
#ifndef SWS_SERVER_H
#define SWS_SERVER_H

typedef struct {
    const char *host;
    int port;
    int backlog;
    const char *docroot;
    int debug;
} ServerConfig;

/* Ham xu ly mot ket noi HTTP, duoc cung cap boi HTTP module. */
typedef void (*ClientHandler)(int client_fd,
                              const char *docroot,
                              int debug);

/* Khoi dong server; ham chi ket thuc khi server dung hoac gap loi. */
int server_run(const ServerConfig *config, ClientHandler handler);

#endif

