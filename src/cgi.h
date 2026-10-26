#ifndef SWS_CGI_H
#define SWS_CGI_H

#include "common.h"

/*
 * Chạy CGI cho URL nằm dưới config->cgi_directory.
 * Quy ước cgi_directory là URI prefix (ví dụ "/cgi-bin") và URI đó phải
 * ánh xạ tới thư mục tương ứng bên trong document_root.
 * Chỉ GET/HEAD có hỗ trợ trong bản này; request body/POST cần mở rộng parser.
 * Trả 0 khi đã xử lý request, -1 khi lỗi I/O.
 */
int cgi_handle_request(int client_fd,
                       const struct http_request *request,
                       const struct server_config *config);

/* Dùng trong http.c để quyết định chuyển request cho CGI hay files.c. */
int cgi_is_request(const struct http_request *request,
                   const struct server_config *config);

#endif
