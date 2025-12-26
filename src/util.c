#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

void die(const char* msg) {
    fprintf(stderr, "FATAL: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(1);
}