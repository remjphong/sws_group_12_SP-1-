#ifndef SWS_FILES_H
#define SWS_FILES_H

#include "common.h"

/*
 * Phục vụ GET/HEAD cho file tĩnh hoặc directory listing.
 * Trả 0 khi đã xử lý request (kể cả khi gửi HTTP 4xx/5xx), -1 khi lỗi I/O.
 */
int files_handle_request(int client_fd,
                         const struct http_request *request,
                         const struct server_config *config);

#endif
