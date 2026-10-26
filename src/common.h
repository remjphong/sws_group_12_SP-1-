#ifndef SWS_COMMON_H
#define SWS_COMMON_H

#define REQUEST_MAX 16384
#define HEADER_MAX 64

typedef struct {
    char name[256];
    char value[4096];
} Header;

typedef struct {
    char method[16];
    char target[4096];
    char version[16];
    Header headers[HEADER_MAX];
    int header_count;
} Request;

#endif
