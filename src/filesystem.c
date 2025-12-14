#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* --- Definitions & Structures --- */

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

/* --- Disk Structures --- */

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

/* --- Runtime Context --- */

typedef struct {
    struct partition *part;
    unsigned char *inode_bitmap;
    unsigned char *block_bitmap;
    int root_inode;
} fs_context;

/* --- Bitmap Helpers --- */

static void set_bit(unsigned char *bitmap, int pos) { bitmap[pos/8] |= (1 << (pos%8)); }
static void clear_bit(unsigned char *bitmap, int pos) { bitmap[pos/8] &= ~(1 << (pos%8)); }
static int test_bit(unsigned char *bitmap, int pos) { return (bitmap[pos/8] & (1 << (pos%8))) != 0; }

static int find_free_bit(unsigned char *bitmap, int max) {
    // Start at 1 to avoid index 0 (Reserved for INVALID)
    for (int i = 1; i < max; i++) {
        if (!test_bit(bitmap, i)) return i;
    }
    return -1;
}

static struct inode* get_inode(struct partition *part, int inum) {
    if (!part || inum < 0 || inum >= 224) return NULL;
    return &part->inode_table[inum];
}

/* --- Allocation & Management --- */

static void rebuild_bitmaps(fs_context *ctx) {
    memset(ctx->inode_bitmap, 0, 224/8 + 1);
    memset(ctx->block_bitmap, 0, 4088/8 + 1);
    
    // SAFETY: Mark index 0 as used so we never allocate it
    set_bit(ctx->inode_bitmap, 0);
    set_bit(ctx->block_bitmap, 0);

    for (unsigned int i = 1; i < ctx->part->s.num_inodes; i++) {
        struct inode *node = &ctx->part->inode_table[i];
        if (node->mode == 0) continue; // Free inode
        
        set_bit(ctx->inode_bitmap, i);
        
        // Calculate used blocks based on file size
        int blocks_needed = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (node->size == 0 && (node->mode & INODE_MODE_DIR_FILE)) blocks_needed = 1;

        for (int k = 0; k < blocks_needed; k++) {
            int blk = -1;
            if (k < 6) blk = node->blocks[k];
            else if (node->indirect_block >= 0) {
                // Mark indirect block itself
                set_bit(ctx->block_bitmap, node->indirect_block);
                unsigned short *ind = (unsigned short*)ctx->part->data_blocks[node->indirect_block].d;
                blk = ind[k - 6];
            }
            // Mark data block
            if (blk > 0 && blk < (int)ctx->part->s.num_blocks) {
                set_bit(ctx->block_bitmap, blk);
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
    // 1. Check if block exists (based on size)
    if ((unsigned int)(logical_blk * BLOCK_SIZE) < node->size) {
        if (logical_blk < 6) return node->blocks[logical_blk];
        if (node->indirect_block >= 0) {
            unsigned short *ind = (unsigned short*)ctx->part->data_blocks[node->indirect_block].d;
            return ind[logical_blk - 6];
        }
    }

    // 2. Direct Block Allocation
    if (logical_blk < 6) {
        if (node->blocks[logical_blk] == 0 && !alloc) return -1;
        if (alloc && ((unsigned int)(logical_blk * BLOCK_SIZE) >= node->size)) {
             int b = alloc_block(ctx);
             if (b < 0) return -1;
             node->blocks[logical_blk] = b;
        }
        return node->blocks[logical_blk];
    }
    
    // 3. Indirect Block Allocation
    if (node->indirect_block < 0) {
        if (!alloc) return -1;
        int b = alloc_block(ctx);
        if (b < 0) return -1;
        node->indirect_block = b;
    }
    unsigned short *ind = (unsigned short*)ctx->part->data_blocks[node->indirect_block].d;
    int ind_idx = logical_blk - 6;
    
    if (alloc && ind[ind_idx] == 0) {
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
        if (blk < 0) break; 
        
        unsigned int blk_off = curr_off % BLOCK_SIZE;
        unsigned int chunk = BLOCK_SIZE - blk_off;
        if (chunk > size - processed) chunk = size - processed;

        if (write) memcpy(ctx->part->data_blocks[blk].d + blk_off, ptr + processed, chunk);
        else memcpy(ptr + processed, ctx->part->data_blocks[blk].d + blk_off, chunk);
        processed += chunk;
    }
    if (write && (offset + processed > node->size)) node->size = offset + processed;
    return processed;
}

/* --- Directory Operations --- */

static int lookup_in_dir(fs_context *ctx, int dir_inum, const char *name) {
    struct inode *dir = get_inode(ctx->part, dir_inum);
    if (!dir || !(dir->mode & INODE_MODE_DIR_FILE)) return FS_ENOTDIR;
    
    unsigned int offset = 0;
    struct dentry entry;
    
    while (offset < dir->size) {
        if (rw_inode_data(ctx, dir_inum, &entry, sizeof(entry), offset, 0) < 12) break;
        
        if (entry.inode != INVALID_INODE) {
            if (strcmp((char*)entry.name, name) == 0) return entry.inode;
        }

        if (entry.dir_length == 0) break; // Avoid infinite loop
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
    entry.dir_length = sizeof(struct dentry); 
    
    // Append to end of directory file
    return rw_inode_data(ctx, dir_inum, &entry, sizeof(struct dentry), dir->size, 1);
}

/* --- Path Resolution & Root Finding --- */

static int find_root_inode(fs_context *ctx) {
    // Scan all inodes to find one where ".." points to itself
    for (int i = 1; i < (int)ctx->part->s.num_inodes; i++) {
        struct inode *node = get_inode(ctx->part, i);
        if (node && (node->mode & INODE_MODE_DIR_FILE)) {
            int parent = lookup_in_dir(ctx, i, "..");
            if (parent == i) return i;
        }
    }
    return -1;
}

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

static int split_path(const char *path, char **parent, char **name) {
    if (!path || !parent || !name || strcmp(path, "/") == 0) return -1;
    const char *slash = strrchr(path, '/');
    if (!slash) return -1;
    if (slash == path) *parent = strdup("/");
    else *parent = strndup(path, slash - path);
    *name = strdup(slash + 1);
    return 0;
}

/* --- Public API --- */

fs_context* fs_init(void) {
    fs_context *ctx = calloc(1, sizeof(fs_context));
    if (ctx) ctx->part = calloc(1, sizeof(struct partition));
    if (ctx && ctx->part) {
        ctx->inode_bitmap = calloc(224/8 + 1, 1);
        ctx->block_bitmap = calloc(4088/8 + 1, 1);
    }
    return ctx;
}

void fs_destroy(fs_context *ctx) {
    if (ctx) { 
        free(ctx->inode_bitmap); 
        free(ctx->block_bitmap); 
        free(ctx->part); 
        free(ctx); 
    }
}

int fs_format(fs_context *ctx, const char *vol_name) {
    if (!ctx || !ctx->part) return FS_ERROR;
    memset(ctx->part, 0, sizeof(struct partition));
    memset(ctx->inode_bitmap, 0, 224/8 + 1);
    memset(ctx->block_bitmap, 0, 4088/8 + 1);

    // Reserve 0 to avoid ambiguity
    set_bit(ctx->inode_bitmap, 0);
    set_bit(ctx->block_bitmap, 0);

    struct super_block *sb = &ctx->part->s;
    sb->partition_type = SIMPLE_PARTITION;
    sb->block_size = BLOCK_SIZE;
    sb->inode_size = sizeof(struct inode);
    sb->num_inodes = 224;
    sb->num_blocks = 4088;
    sb->num_free_inodes = 223; 
    sb->num_free_blocks = 4087; 
    strncpy(sb->volume_name, vol_name, 23);

    // Create Root Directory
    int root = alloc_inode(ctx); 
    if (root < 0) return FS_ERROR;
    
    struct inode *node = get_inode(ctx->part, root);
    node->mode = INODE_MODE_DIR_FILE | INODE_MODE_AC_ALL;
    node->date = (unsigned int)time(NULL);
    node->indirect_block = -1;
    node->size = 0;
    
    // Allocate initial block
    int blk = alloc_block(ctx);
    node->blocks[0] = blk;
    
    // Init Root
    ctx->root_inode = root;
    add_dentry(ctx, root, ".", root, DENTRY_TYPE_DIR_FILE);
    add_dentry(ctx, root, "..", root, DENTRY_TYPE_DIR_FILE);
    
    return FS_SUCCESS;
}

int fs_mount(fs_context *ctx, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return FS_ERROR;
    
    fread(ctx->part, 1, sizeof(struct partition), f);
    fclose(f);
    
    if (ctx->part->s.partition_type != SIMPLE_PARTITION) {
        printf("[ERROR] Bad Magic Number: 0x%X\n", ctx->part->s.partition_type);
        return FS_ERROR;
    }
    
    rebuild_bitmaps(ctx);
    ctx->root_inode = find_root_inode(ctx);
    
    if (ctx->root_inode < 0) {
        printf("[ERROR] Root inode not found.\n");
        return FS_ERROR;
    }
    return FS_SUCCESS;
}

int fs_create(fs_context *ctx, const char *path, int type) {
    char *parent_path, *filename;
    if (split_path(path, &parent_path, &filename) != 0) return FS_EINVAL;
    int p_inum = resolve_path(ctx, parent_path);
    free(parent_path);
    
    if (p_inum < 0) { free(filename); return FS_ENOENT; }
    if (lookup_in_dir(ctx, p_inum, filename) >= 0) { free(filename); return FS_EEXIST; }
    
    int new_inum = alloc_inode(ctx);
    if (new_inum < 0) { free(filename); return FS_ENOSPC; }
    
    struct inode *node = get_inode(ctx->part, new_inum);
    node->mode = type | INODE_MODE_AC_ALL;
    node->date = (unsigned int)time(NULL);
    node->indirect_block = -1;
    node->size = 0;
    
    if (type == INODE_MODE_DIR_FILE) {
        int blk = alloc_block(ctx);
        if (blk < 0) return FS_ENOSPC;
        node->blocks[0] = blk;
        
        struct dentry dot = {0}, dotdot = {0};
        
        dot.inode = new_inum; dot.file_type = DENTRY_TYPE_DIR_FILE;
        dot.name_len = 1; strcpy((char*)dot.name, "."); dot.dir_length = sizeof(struct dentry);
        
        dotdot.inode = p_inum; dotdot.file_type = DENTRY_TYPE_DIR_FILE;
        dotdot.name_len = 2; strcpy((char*)dotdot.name, ".."); dotdot.dir_length = sizeof(struct dentry);
        
        memcpy(ctx->part->data_blocks[blk].d, &dot, sizeof(dot));
        memcpy(ctx->part->data_blocks[blk].d + sizeof(dot), &dotdot, sizeof(dotdot));
        node->size = 2 * sizeof(struct dentry);
    }
    
    int res = add_dentry(ctx, p_inum, filename, new_inum, 
        (type == INODE_MODE_DIR_FILE) ? DENTRY_TYPE_DIR_FILE : DENTRY_TYPE_REG_FILE);
    free(filename);
    return res;
}

int fs_write(fs_context *ctx, const char *path, const void *buf, unsigned int size, unsigned int offset) {
    int inum = resolve_path(ctx, path);
    if (inum < 0) return inum;
    return rw_inode_data(ctx, inum, (void*)buf, size, offset, 1);
}

int fs_read(fs_context *ctx, const char *path, void *buf, unsigned int size, unsigned int offset) {
    int inum = resolve_path(ctx, path);
    if (inum < 0) return inum;
    return rw_inode_data(ctx, inum, buf, size, offset, 0);
}

int fs_save(fs_context *ctx, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return FS_ERROR;
    fwrite(ctx->part, 1, sizeof(struct partition), f);
    fclose(f);
    return FS_SUCCESS;
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

/* --- Main Application --- */

void print_entry(const char *name, int type, unsigned int size) {
    int is_dir = 0;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        is_dir = 1;
    else if (type == DENTRY_TYPE_DIR_FILE)
        is_dir = 1;

    printf("  %-15s %s (%u octets)\n", name, is_dir ? "[DIR]" : "[FILE]", size);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <disk_image>\n", argv[0]);
        return 1;
    }

    char *disk_name = argv[1];
    printf("=== Filesystem Tool ===\n");
    printf("Target Image: %s\n\n", disk_name);

    fs_context *ctx = fs_init();
    if (!ctx) {
        printf("Memory Error.\n");
        return 1;
    }

    // 1. Try to Mount existing disk
    if (fs_mount(ctx, disk_name) == FS_SUCCESS) {
        printf("[INFO] Mounted successfully.\n");
        printf("Volume: %s\n", ctx->part->s.volume_name);
        printf("Root Inode: %d\n\n", ctx->root_inode);
        
        printf("--- Content of / ---\n");
        fs_list(ctx, "/", print_entry);
        
        // Attempt to read a sample file if it exists
        char buf[64] = {0};
        if (fs_read(ctx, "/documents/secret.txt", buf, 63, 0) > 0) {
            printf("\n[READ TEST] Content of /documents/secret.txt:\n -> \"%s\"\n", buf);
        }

    } else {
        // 2. If mount fails, create new disk
        printf("[WARN] Could not mount %s (File missing or invalid).\n", disk_name);
        printf("[INFO] Creating and Formatting new disk...\n");

        if (fs_format(ctx, "MyNewDisk") != FS_SUCCESS) {
            printf("[ERROR] Failed to format.\n");
            fs_destroy(ctx);
            return 1;
        }

        printf("[INFO] Generating sample data...\n");
        fs_create(ctx, "/documents", INODE_MODE_DIR_FILE);
        fs_create(ctx, "/documents/secret.txt", INODE_MODE_REG_FILE);
        
        char *txt = "Hello World! This is a test file.";
        fs_write(ctx, "/documents/secret.txt", txt, strlen(txt), 0);
        
        fs_create(ctx, "/images", INODE_MODE_DIR_FILE);

        if (fs_save(ctx, disk_name) == FS_SUCCESS) {
            printf("[SUCCESS] New disk saved to %s.\n", disk_name);
            printf("Run the command again to read the disk.\n");
        } else {
            printf("[ERROR] Failed to save disk file.\n");
        }
    }

    fs_destroy(ctx);
    printf("\n=== Done ===\n");
    return 0;
}