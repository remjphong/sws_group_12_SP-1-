
#ifndef SWS_HTTP_H
#define SWS_HTTP_H

void http_handle_client(int client_fd,
                        const char *docroot,
                        int debug);

#endif

