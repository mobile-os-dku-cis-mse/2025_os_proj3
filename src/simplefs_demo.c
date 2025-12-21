
#include "simplefs.h"
#include <time.h>

#define MAX_OPEN_FILES 32

struct file {
    int used;
    uint32_t inode_no;
    uint32_t offset;
    uint32_t mode;
};

struct dir_cache_entry {
    char name[256];
    uint32_t inode_no;
};

struct dir_cache {
    struct dir_cache_entry *items;
    size_t count;
};

static void read_inode_table(FILE *fp, struct inode *out_inodes) {
    uint8_t blk[SIMPLEFS_BLOCK_SIZE];
    uint8_t *raw = (uint8_t*)out_inodes;
    size_t total = SIMPLEFS_NUM_INODES * sizeof(struct inode);
    size_t off = 0;
    for (uint32_t b = 0; b < SIMPLEFS_NUM_INODE_BLOCKS; b++) {
        read_block(fp, 1 + b, blk);
        size_t chunk = SIMPLEFS_BLOCK_SIZE;
        if (off + chunk > total) chunk = total - off;
        memcpy(raw + off, blk, chunk);
        off += chunk;
    }
}

static int inode_is_dir(const struct inode *in) { return (in->mode == SIMPLEFS_MODE_DIR); }

static void print_root_listing(FILE *fp, const struct inode *inodes, struct dir_cache *cache) {
    const struct inode *root = &inodes[0];
    if (!inode_is_dir(root)) {
        fprintf(stderr, "ERROR: inode 0 is not a directory. (This project assumes root inode=0)\n");
        exit(1);
    }

    printf("\n=== Root Directory ===\n");
    printf("%-10s %-6s %-10s %s\n", "Inode", "Type", "Size", "Name");

    // Directory entries are stored as a byte stream in the directory file.
    // Do NOT assume entries align to 1024-byte blocks (they may straddle blocks).
    // Read the whole directory file into memory, then parse fixed-size dentries.
    uint32_t entry_count = root->size / (uint32_t)sizeof(struct dentry);
    cache->items = (struct dir_cache_entry*)calloc(entry_count, sizeof(struct dir_cache_entry));
    cache->count = 0;

    uint8_t *dirbuf = (uint8_t*)calloc(1, root->size);
    if (!dirbuf) die("calloc(dirbuf)");

    // Read root directory bytes across direct blocks
    uint32_t remaining = root->size;
    uint32_t out_off = 0;
    for (int i = 0; i < 6 && remaining > 0; i++) {
        uint32_t blkno = root->blocks[i];
        if (blkno == 0) break;
        uint8_t blk[SIMPLEFS_BLOCK_SIZE];
        read_block(fp, blkno, blk);

        uint32_t chunk = remaining;
        if (chunk > SIMPLEFS_BLOCK_SIZE) chunk = SIMPLEFS_BLOCK_SIZE;
        memcpy(dirbuf + out_off, blk, chunk);
        out_off += chunk;
        remaining -= chunk;
    }

    for (uint32_t e = 0; e < entry_count; e++) {
        struct dentry de;
        memcpy(&de, dirbuf + (size_t)e * sizeof(struct dentry), sizeof(de));
        if (de.inode == 0 || de.name_len == 0) continue;

        char name[256];
        uint32_t nl = de.name_len;
        if (nl > 255) nl = 255;
        memcpy(name, de.name, nl);
        name[nl] = '\0';

        const struct inode *in = &inodes[de.inode];
        const char *type = (de.file_type == SIMPLEFS_FT_DIR) ? "DIR" : "REG";
        printf("%-10u %-6s %-10u %s\n", de.inode, type, in->size, name);

        strncpy(cache->items[cache->count].name, name, sizeof(cache->items[cache->count].name) - 1);
        cache->items[cache->count].inode_no = de.inode;
        cache->count++;
    }

    free(dirbuf);
}

static int lookup_inode_by_name(const struct dir_cache *cache, const char *name, uint32_t *out_ino) {
    for (size_t i = 0; i < cache->count; i++) {
        if (strcmp(cache->items[i].name, name) == 0) {
            *out_ino = cache->items[i].inode_no;
            return 1;
        }
    }
    return 0;
}

static int sys_open(struct file *oft, const struct dir_cache *cache, const char *pathname, uint32_t mode) {
    uint32_t ino = 0;
    if (!lookup_inode_by_name(cache, pathname, &ino)) return -1;

    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!oft[fd].used) {
            oft[fd].used = 1;
            oft[fd].inode_no = ino;
            oft[fd].offset = 0;
            oft[fd].mode = mode;
            return fd;
        }
    }
    return -1;
}

static int sys_close(struct file *oft, int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !oft[fd].used) return -1;
    oft[fd].used = 0;
    oft[fd].inode_no = 0;
    oft[fd].offset = 0;
    oft[fd].mode = 0;
    return 0;
}

static int32_t map_logical_to_physical(const struct inode *in, uint32_t logical_block, FILE *fp) {
    if (logical_block < 6) {
        uint32_t phys = in->blocks[logical_block];
        return (phys == 0) ? -1 : (int32_t)phys;
    }
    // indirect not used by default disk builder; basic support included
    if (in->indirect_block < 0) return -1;
    uint8_t blk[SIMPLEFS_BLOCK_SIZE];
    read_block(fp, (uint32_t)in->indirect_block, blk);
    uint16_t *arr = (uint16_t*)blk;
    uint32_t idx = logical_block - 6;
    if (idx >= SIMPLEFS_BLOCK_SIZE / 2) return -1;
    uint16_t phys = arr[idx];
    return (phys == 0) ? -1 : (int32_t)phys;
}

static int32_t sys_read(FILE *fp, const struct inode *inodes, struct file *oft, int fd, uint8_t *buf, uint32_t nbytes) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !oft[fd].used) return -1;
    const struct inode *in = &inodes[oft[fd].inode_no];

    uint32_t offset = oft[fd].offset;
    if (offset >= in->size) return 0;

    uint32_t to_read = nbytes;
    if (offset + to_read > in->size) to_read = in->size - offset;

    uint32_t remaining = to_read;
    uint32_t buf_off = 0;

    while (remaining > 0) {
        uint32_t logical_block = offset / SIMPLEFS_BLOCK_SIZE;
        uint32_t in_block_off = offset % SIMPLEFS_BLOCK_SIZE;

        int32_t phys = map_logical_to_physical(in, logical_block, fp);
        if (phys < 0) break;

        uint8_t blk[SIMPLEFS_BLOCK_SIZE];
        read_block(fp, (uint32_t)phys, blk);

        uint32_t chunk = SIMPLEFS_BLOCK_SIZE - in_block_off;
        if (chunk > remaining) chunk = remaining;

        memcpy(buf + buf_off, blk + in_block_off, chunk);

        buf_off += chunk;
        offset += chunk;
        remaining -= chunk;
    }

    oft[fd].offset = offset;
    return (int32_t)buf_off;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <disk.img>\n", argv[0]);
        return 1;
    }
    const char *img_path = argv[1];

    FILE *fp = fopen(img_path, "rb");
    if (!fp) die("fopen(disk image)");

    // mount: read superblock + inode table
    struct super_block sb;
    read_block(fp, 0, &sb);
    if (sb.block_size != SIMPLEFS_BLOCK_SIZE || sb.inode_size != sizeof(struct inode)) {
        fprintf(stderr, "ERROR: disk image format mismatch (block_size=%u inode_size=%u)\n",
                sb.block_size, sb.inode_size);
        return 1;
    }

    printf("Mounted volume: %s\n", sb.volume_name);
    printf("First data block: %u\n", sb.first_data_block);

    struct inode inodes[SIMPLEFS_NUM_INODES];
    read_inode_table(fp, inodes);

    struct dir_cache cache = {0};
    print_root_listing(fp, inodes, &cache);

    // single user process: open/read/close for 10 files (or as many as exist)
    struct file oft[MAX_OPEN_FILES];
    memset(oft, 0, sizeof(oft));

    printf("\n=== User process: open/read/close ===\n");
    size_t n = cache.count;
    if (n > 10) n = 10;

    for (size_t i = 0; i < n; i++) {
        const char *fname = cache.items[i].name;
        int fd = sys_open(oft, &cache, fname, 0);
        if (fd < 0) {
            printf("[open] %s -> FAILED\n", fname);
            continue;
        }
        printf("[open] %s -> fd=%d\n", fname, fd);

        uint8_t buf[2048];
        int32_t r = sys_read(fp, inodes, oft, fd, buf, sizeof(buf)-1);
        if (r < 0) {
            printf("[read] fd=%d -> FAILED\n", fd);
            sys_close(oft, fd);
            continue;
        }
        buf[r] = '\0';
        printf("[read] fd=%d -> %d bytes\n", fd, (int)r);
        printf("----- file content start -----\n%s----- file content end -----\n", buf);

        if (sys_close(oft, fd) == 0) {
            printf("[close] fd=%d -> OK\n\n", fd);
        } else {
            printf("[close] fd=%d -> FAILED\n\n", fd);
        }
    }

    free(cache.items);
    fclose(fp);
    return 0;
}
