#ifndef SWS_CGI_H
#define SWS_CGI_H

#include "common.h"

int cgi_handle(int fd, const char *script, const Request *r,
               const char *query);

#endif
