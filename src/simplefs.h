
#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SIMPLEFS_BLOCK_SIZE 1024u
#define SIMPLEFS_NUM_BLOCKS 4096u
#define SIMPLEFS_NUM_INODES 224u
#define SIMPLEFS_NUM_INODE_BLOCKS 7u
#define SIMPLEFS_FIRST_DATA_BLOCK 8u
#define SIMPLEFS_VOLUME_NAME_LEN 24u

// --- On-disk structures (match the assignment spec) ---
#pragma pack(push, 1)

struct super_block {
    uint32_t partition_type;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t first_inode;
    uint32_t num_inodes;
    uint32_t num_inode_blocks;
    uint32_t num_free_inodes;
    uint32_t num_blocks;
    uint32_t num_free_blocks;
    uint32_t first_data_block;
    char volume_name[SIMPLEFS_VOLUME_NAME_LEN];
    uint8_t padding[960]; // 1024 - 64
};
_Static_assert(sizeof(struct super_block) == SIMPLEFS_BLOCK_SIZE, "super_block must be 1024 bytes");

struct inode {
    uint32_t mode;         // file type + permissions (simple use in this project)
    uint32_t locked;       // opened for write (not used here)
    uint32_t date;         // unix time
    uint32_t size;         // file size in bytes
    int32_t  indirect_block;// block number or -1
    uint16_t blocks[0x6];  // 6 direct blocks
};
_Static_assert(sizeof(struct inode) == 32, "inode must be 32 bytes");

struct blocks {
    uint8_t d[SIMPLEFS_BLOCK_SIZE];
};
_Static_assert(sizeof(struct blocks) == SIMPLEFS_BLOCK_SIZE, "blocks must be 1024 bytes");

// Directory entry structure (aligned with the spec; we store fixed-size entries for simplicity)
struct dentry {
    uint32_t inode;
    uint32_t dir_length;
    uint32_t name_len;
    uint32_t file_type; // 1=reg, 2=dir (for this project)
    union {
        uint8_t name[255];
        uint8_t n_pad[16][16]; // 256 bytes
    };
};
_Static_assert(sizeof(struct dentry) == 272, "dentry must be 272 bytes");

#pragma pack(pop)

// --- Constants for this project ---
enum {
    SIMPLEFS_FT_REG = 1,
    SIMPLEFS_FT_DIR = 2
};

enum {
    SIMPLEFS_MODE_REG = 0x8000, // arbitrary; used only for printing
    SIMPLEFS_MODE_DIR = 0x4000
};

// --- I/O helpers ---
static inline void die(const char *msg) {
    fprintf(stderr, "ERROR: %s (%s)\n", msg, strerror(errno));
    exit(1);
}

static inline void read_block(FILE *fp, uint32_t block_no, void *out_buf) {
    if (block_no >= SIMPLEFS_NUM_BLOCKS) {
        fprintf(stderr, "ERROR: invalid block %u\n", block_no);
        exit(1);
    }
    if (fseek(fp, (long)block_no * SIMPLEFS_BLOCK_SIZE, SEEK_SET) != 0) die("fseek(read_block)");
    size_t n = fread(out_buf, 1, SIMPLEFS_BLOCK_SIZE, fp);
    if (n != SIMPLEFS_BLOCK_SIZE) die("fread(read_block)");
}

static inline void write_block(FILE *fp, uint32_t block_no, const void *buf) {
    if (block_no >= SIMPLEFS_NUM_BLOCKS) {
        fprintf(stderr, "ERROR: invalid block %u\n", block_no);
        exit(1);
    }
    if (fseek(fp, (long)block_no * SIMPLEFS_BLOCK_SIZE, SEEK_SET) != 0) die("fseek(write_block)");
    size_t n = fwrite(buf, 1, SIMPLEFS_BLOCK_SIZE, fp);
    if (n != SIMPLEFS_BLOCK_SIZE) die("fwrite(write_block)");
}

static inline void zero_block(uint8_t *buf) { memset(buf, 0, SIMPLEFS_BLOCK_SIZE); }

#endif
