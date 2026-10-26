#ifndef SWS_FILES_H
#define SWS_FILES_H

#include "common.h"

void files_handle(int fd, const char *root, const Request *r,
                  unsigned short port);

#endif
