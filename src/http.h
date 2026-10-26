#ifndef SWS_HTTP_H
#define SWS_HTTP_H

#include <stddef.h>

#define HTTP_METHOD_MAX  16
#define HTTP_URI_MAX     2048
#define HTTP_VERSION_MAX 16
#define HTTP_DATE_MAX    128

typedef struct {
    char method[HTTP_METHOD_MAX];
    char uri[HTTP_URI_MAX];
    char version[HTTP_VERSION_MAX];
    char if_modified_since[HTTP_DATE_MAX];
} HttpRequest;

/* A gọi hàm này sau khi accept() thành công. */
void http_handle_client(int client_fd,
                        const char *docroot,
                        int debug);

/* Các hàm hỗ trợ để module file và CGI tạo response. */
int http_send_response(int client_fd,
                       int status,
                       const char *content_type,
                       const void *body,
                       size_t body_len,
                       int head_only);

int http_send_error(int client_fd,
                    int status,
                    const char *message,
                    int head_only);

#endif