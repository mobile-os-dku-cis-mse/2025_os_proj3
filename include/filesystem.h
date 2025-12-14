#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stdio.h>

/* --- Definitions --- */
#define SIMPLE_PARTITION      0x1111
#define INVALID_INODE         0
#define BLOCK_SIZE            1024

// Permissions & Modes
#define INODE_MODE_AC_ALL     0x777
#define INODE_MODE_REG_FILE   0x10000
#define INODE_MODE_DIR_FILE   0x20000

// Directory Entry Types
#define DENTRY_TYPE_REG_FILE  0x1
#define DENTRY_TYPE_DIR_FILE  0x2

// Error Codes
#define FS_SUCCESS     0
#define FS_ERROR      -1
#define FS_ENOENT     -2
#define FS_EEXIST     -3
#define FS_ENOSPC     -4
#define FS_EINVAL     -5
#define FS_EISDIR     -6
#define FS_ENOTDIR    -7

/* --- Structures --- */

struct super_block {
    unsigned int partition_type;
    unsigned int block_size;
    unsigned int inode_size;
    unsigned int first_inode;
    unsigned int num_inodes;
    unsigned int num_inode_blocks;
    unsigned int num_free_inodes;
    unsigned int num_blocks;
    unsigned int num_free_blocks;
    unsigned int first_data_block;
    char volume_name[24];
    unsigned char padding[960];
};

struct inode {
    unsigned int mode;
    unsigned int locked;
    unsigned int date;
    unsigned int size;
    int indirect_block;
    unsigned short blocks[0x6];
};

struct blocks {
    unsigned char d[BLOCK_SIZE];
};

struct partition {
    struct super_block s;
    struct inode inode_table[224];
    struct blocks data_blocks[4088];
};

struct dentry {
    unsigned int inode;
    unsigned int dir_length;
    unsigned int name_len;
    unsigned int file_type;
    union {
        unsigned char name[256];
        unsigned char n_pad[16][16];
    };
};

/* Context structure */
typedef struct {
    struct partition *part;
    unsigned char *inode_bitmap;
    unsigned char *block_bitmap;
    int root_inode;
} fs_context;

/* --- Function Prototypes --- */

// Lifecycle
fs_context* fs_init(void);
void fs_destroy(fs_context *ctx);

// Disk Operations
int fs_format(fs_context *ctx, const char *vol_name);
int fs_mount(fs_context *ctx, const char *path);
int fs_save(fs_context *ctx, const char *path);

// File Operations
int fs_create(fs_context *ctx, const char *path, int type);
int fs_write(fs_context *ctx, const char *path, const void *buf, unsigned int size, unsigned int offset);
int fs_read(fs_context *ctx, const char *path, void *buf, unsigned int size, unsigned int offset);
int fs_list(fs_context *ctx, const char *path, void (*cb)(const char*, int, unsigned int));

#endif // FILESYSTEM_H