#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* --- Definitions & Structures --- */
#define SIMPLE_PARTITION      0x1111
#define INVALID_INODE         0
#define BLOCK_SIZE            1024
#define INODE_MODE_REG_FILE   0x10000
#define INODE_MODE_DIR_FILE   0x20000
#define DENTRY_TYPE_REG_FILE  0x1
#define DENTRY_TYPE_DIR_FILE  0x2

#define FS_SUCCESS     0
#define FS_ERROR      -1
#define FS_ENOENT     -2
#define FS_EEXIST     -3
#define FS_ENOSPC     -4
#define FS_EINVAL     -5
#define FS_EISDIR     -6
#define FS_ENOTDIR    -7
#define FS_ENOTEMPTY  -8

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

typedef struct {
    struct partition *part;
    unsigned char *inode_bitmap;
    unsigned char *block_bitmap;
    int root_inode;
} fs_context;

/* --- Bitmaps & Allocation --- */

static void set_bit(unsigned char *bitmap, int pos) { bitmap[pos/8] |= (1 << (pos%8)); }
static void clear_bit(unsigned char *bitmap, int pos) { bitmap[pos/8] &= ~(1 << (pos%8)); }
static int test_bit(unsigned char *bitmap, int pos) { return (bitmap[pos/8] & (1 << (pos%8))) != 0; }

static int find_free_bit(unsigned char *bitmap, int max) {
    for (int i = 0; i < max; i++) if (!test_bit(bitmap, i)) return i;
    return -1;
}

static struct inode* get_inode(struct partition *part, int inum) {
    if (!part || inum < 0 || inum >= 224) return NULL;
    return &part->inode_table[inum];
}

static void rebuild_bitmaps(fs_context *ctx) {
    memset(ctx->inode_bitmap, 0, 224/8 + 1);
    memset(ctx->block_bitmap, 0, 4088/8 + 1);

    for (unsigned int i = 0; i < ctx->part->s.num_inodes; i++) {
        struct inode *node = &ctx->part->inode_table[i];
        if (node->mode == 0) continue;
        
        set_bit(ctx->inode_bitmap, i);
        
        // Mark blocks based on file size to support Block 0 usage
        int blocks_needed = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        for (int k = 0; k < blocks_needed; k++) {
            int blk_idx = -1;
            if (k < 6) {
                blk_idx = node->blocks[k];
            } else if (node->indirect_block >= 0) {
                set_bit(ctx->block_bitmap, node->indirect_block);
                unsigned short *ind = (unsigned short*)ctx->part->data_blocks[node->indirect_block].d;
                blk_idx = ind[k - 6];
            }
            if (blk_idx >= 0 && blk_idx < (int)ctx->part->s.num_blocks) {
                set_bit(ctx->block_bitmap, blk_idx);
            }
        }
    }
}

static int alloc_block(fs_context *ctx) {
    int bnum = find_free_bit(ctx->block_bitmap, ctx->part->s.num_blocks);
    if (bnum < 0) return -1;
    set_bit(ctx->block_bitmap, bnum);
    ctx->part->s.num_free_blocks--;
    memset(ctx->part->data_blocks[bnum].d, 0, BLOCK_SIZE);
    return bnum;
}

static int alloc_inode(fs_context *ctx) {
    int inum = find_free_bit(ctx->inode_bitmap, ctx->part->s.num_inodes);
    if (inum < 0) return -1;
    set_bit(ctx->inode_bitmap, inum);
    ctx->part->s.num_free_inodes--;
    memset(&ctx->part->inode_table[inum], 0, sizeof(struct inode));
    ctx->part->inode_table[inum].indirect_block = -1;
    return inum;
}

/* --- I/O Operations --- */

static int get_block_index(fs_context *ctx, struct inode *node, int logical_blk, int alloc) {
    if (logical_blk < 6) {
        if (node->blocks[logical_blk] == 0 && !alloc && node->size == 0) return -1;
        // Check if block is allocated by checking bitmap or if we are allocating
        if (alloc && (node->size <= (unsigned int)(logical_blk * BLOCK_SIZE))) {
             int b = alloc_block(ctx);
             if (b < 0) return -1;
             node->blocks[logical_blk] = b;
        }
        return node->blocks[logical_blk];
    }
    
    // Indirect handling
    if (node->indirect_block < 0) {
        if (!alloc) return -1;
        int b = alloc_block(ctx);
        if (b < 0) return -1;
        node->indirect_block = b;
    }
    
    unsigned short *ind = (unsigned short*)ctx->part->data_blocks[node->indirect_block].d;
    int ind_idx = logical_blk - 6;
    
    if (ind[ind_idx] == 0 && !alloc && node->size <= (unsigned int)(logical_blk * BLOCK_SIZE)) return -1;
    if (alloc && (node->size <= (unsigned int)(logical_blk * BLOCK_SIZE))) {
        int b = alloc_block(ctx);
        if (b < 0) return -1;
        ind[ind_idx] = b;
    }
    return ind[ind_idx];
}

static int rw_inode_data(fs_context *ctx, int inum, void *buf, unsigned int size, unsigned int offset, int write) {
    struct inode *node = get_inode(ctx->part, inum);
    if (!node) return FS_EINVAL;
    
    if (!write && offset >= node->size) return 0;
    if (!write && offset + size > node->size) size = node->size - offset;

    unsigned int processed = 0;
    unsigned char *ptr = (unsigned char*)buf;

    while (processed < size) {
        unsigned int curr_off = offset + processed;
        int blk = get_block_index(ctx, node, curr_off / BLOCK_SIZE, write);
        
        if (blk < 0) break; // Error or EOF
        
        unsigned int blk_off = curr_off % BLOCK_SIZE;
        unsigned int chunk = BLOCK_SIZE - blk_off;
        if (chunk > size - processed) chunk = size - processed;

        if (write) {
            memcpy(ctx->part->data_blocks[blk].d + blk_off, ptr + processed, chunk);
        } else {
            memcpy(ptr + processed, ctx->part->data_blocks[blk].d + blk_off, chunk);
        }
        processed += chunk;
    }

    if (write && (offset + processed > node->size)) node->size = offset + processed;
    return processed;
}

/* --- Directory Ops --- */

static int lookup_in_dir(fs_context *ctx, int dir_inum, const char *name) {
    struct inode *dir = get_inode(ctx->part, dir_inum);
    if (!dir || !(dir->mode & INODE_MODE_DIR_FILE)) return FS_ENOTDIR;
    
    unsigned int offset = 0;
    struct dentry entry;
    
    while (offset < dir->size) {
        if (rw_inode_data(ctx, dir_inum, &entry, sizeof(struct dentry), offset, 0) < 12) break;
        
        if (entry.inode != INVALID_INODE && strcmp((char*)entry.name, name) == 0) {
            return entry.inode;
        }
        if (entry.dir_length == 0) break;
        offset += entry.dir_length;
    }
    return FS_ENOENT;
}

static int add_dentry(fs_context *ctx, int dir_inum, const char *name, int inum, int type) {
    struct inode *dir = get_inode(ctx->part, dir_inum);
    struct dentry entry;
    memset(&entry, 0, sizeof(entry));
    
    entry.inode = inum;
    entry.file_type = type;
    entry.name_len = strlen(name);
    strncpy((char*)entry.name, name, 255);
    entry.dir_length = sizeof(struct dentry); // Simplified appending
    
    // Append to end
    return rw_inode_data(ctx, dir_inum, &entry, sizeof(struct dentry), dir->size, 1);
}

/* --- Dynamic Root Detection --- */

static int find_root_inode(fs_context *ctx) {
    // 1. Try common locations first
    int candidates[] = {0, 2, 1}; 
    
    for (int k = 0; k < 3; k++) {
        int i = candidates[k];
        struct inode *node = get_inode(ctx->part, i);
        if (node && (node->mode & INODE_MODE_DIR_FILE)) {
             int parent = lookup_in_dir(ctx, i, "..");
             if (parent == i) return i; // Root points to itself
        }
    }

    // 2. Full scan if heuristic failed
    for (int i = 0; i < (int)ctx->part->s.num_inodes; i++) {
        struct inode *node = get_inode(ctx->part, i);
        if (node && (node->mode & INODE_MODE_DIR_FILE)) {
            if (lookup_in_dir(ctx, i, "..") == i) return i;
        }
    }
    return -1;
}

/* --- Path Resolution --- */

static int resolve_path(fs_context *ctx, const char *path) {
    if (strcmp(path, "/") == 0) return ctx->root_inode;
    
    int curr = ctx->root_inode;
    char *dup = strdup(path);
    char *tok = strtok(dup, "/");
    
    while (tok) {
        int next = lookup_in_dir(ctx, curr, tok);
        if (next < 0) { free(dup); return next; }
        curr = next;
        tok = strtok(NULL, "/");
    }
    free(dup);
    return curr;
}

/* --- Public API --- */

fs_context* fs_init(void) {
    fs_context *ctx = calloc(1, sizeof(fs_context));
    if (ctx) ctx->part = malloc(sizeof(struct partition));
    if (ctx && ctx->part) {
        ctx->inode_bitmap = calloc(224/8 + 1, 1);
        ctx->block_bitmap = calloc(4088/8 + 1, 1);
    }
    return ctx;
}

void fs_destroy(fs_context *ctx) {
    if (ctx) {
        free(ctx->inode_bitmap); free(ctx->block_bitmap);
        free(ctx->part); free(ctx);
    }
}

int fs_mount(fs_context *ctx, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return FS_ERROR;
    if (fread(ctx->part, 1, sizeof(struct partition), f) != sizeof(struct partition)) {
        // Warning: File size might differ, but proceed if header valid
    }
    fclose(f);

    if (ctx->part->s.partition_type != SIMPLE_PARTITION) return FS_ERROR;
    
    rebuild_bitmaps(ctx);
    ctx->root_inode = find_root_inode(ctx);
    
    return (ctx->root_inode >= 0) ? FS_SUCCESS : FS_ERROR;
}

int fs_read(fs_context *ctx, const char *path, void *buf, unsigned int size, unsigned int offset) {
    int inum = resolve_path(ctx, path);
    if (inum < 0) return inum;
    struct inode *node = get_inode(ctx->part, inum);
    if (node->mode & INODE_MODE_DIR_FILE) return FS_EISDIR;
    return rw_inode_data(ctx, inum, buf, size, offset, 0);
}

int fs_list(fs_context *ctx, const char *path, void (*cb)(const char*, int, unsigned int)) {
    int inum = resolve_path(ctx, path);
    if (inum < 0) return inum;
    struct inode *dir = get_inode(ctx->part, inum);
    if (!(dir->mode & INODE_MODE_DIR_FILE)) return FS_ENOTDIR;

    unsigned int offset = 0;
    struct dentry entry;
    while (offset < dir->size) {
        if (rw_inode_data(ctx, inum, &entry, sizeof(entry), offset, 0) < 12) break;
        if (entry.inode != INVALID_INODE) {
            struct inode *t = get_inode(ctx->part, entry.inode);
            if (t) cb((char*)entry.name, entry.file_type, t->size);
        }
        if (entry.dir_length == 0) break;
        offset += entry.dir_length;
    }
    return FS_SUCCESS;
}

/* --- Main Test --- */

static void print_entry(const char *name, int type, unsigned int size) {
    printf("  %-20s %s (%u bytes)\n", name, (type == 2) ? "[DIR]" : "[FILE]", size);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== Filesystem Manager ===\n");

    fs_context *ctx = fs_init();
    if (fs_mount(ctx, "disk.img") != FS_SUCCESS) {
        printf("Error: Mount failed (check file existence/format)\n");
        return 1;
    }

    printf("Mounted. Root found at inode %d.\n", ctx->root_inode);
    printf("Root contents:\n");
    fs_list(ctx, "/", print_entry);

    // Read example (file_1)
    char buf[100];
    int bytes = fs_read(ctx, "/file_1", buf, sizeof(buf)-1, 0);
    if (bytes > 0) {
        buf[bytes] = 0;
        printf("\nContent of /file_1: %s\n", buf);
    }

    fs_destroy(ctx);
    return 0;
}