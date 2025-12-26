#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>

#define BLOCK_SIZE 1024u
#define MAX_INODES 224u
#define MAX_FD     32u
#define MAX_PATH   256u

void die(const char* msg);

static inline uint32_t u32_min(uint32_t a, uint32_t b) { return (a < b) ? a : b; }
static inline uint32_t u32_max(uint32_t a, uint32_t b) { return (a > b) ? a : b; }

#endif
