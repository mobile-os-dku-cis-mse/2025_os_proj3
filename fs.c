#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fs.h"

#define DISK_IMAGE "./disk.img"

static struct partition part;
static unsigned char inode_map[224];
static unsigned char block_map[4088];
static int root_inode = -1;

#define MAX_OPEN_FILES 16

struct file {
    int used;
    int inum;
    int offset;
};

static struct file file_table[MAX_OPEN_FILES];

//Bitmap helpers
static void set_bit(unsigned char *bm, int n) {
    bm[n / 8] |= (1 << (n % 8));
}

static int test_bit(unsigned char *bm, int n) {
    return (bm[n / 8] & (1 << (n % 8))) != 0;
}
// Find the first free bit in a bitmap 
static int find_free(unsigned char *bm, int max) {
    for (int i = 1; i < max; i++)
        if (!test_bit(bm, i))
            return i;
    return -1;
}

// mount 
/*
 * Loads the disk image into memory, validates the filesystem,
 * rebuilds inode and block bitmaps, and initializes runtime structures.
 */
static int fs_mount(void) {
    FILE *f = fopen(DISK_IMAGE, "rb");
    if (!f) {
        perror("open disk.img");
        return -1;
    }
    fread(&part, 1, sizeof(part), f);
    fclose(f);

    if (part.s.partition_type != SIMPLE_PARTITION) {
        printf("Invalid partition type\n");
        return -1;
    }

    memset(inode_map, 0, sizeof(inode_map));
    memset(block_map, 0, sizeof(block_map));
    set_bit(inode_map, 0);
    set_bit(block_map, 0);
    
    // Rebuild allocation maps by scanning all inodes 
    for (int i = 1; i < 224; i++) {
        struct inode *n = &part.inode_table[i];
        if (n->mode == 0) continue;

        set_bit(inode_map, i);

        int blocks = (n->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if ((n->mode & INODE_MODE_DIR_FILE) && n->size == 0)
            blocks = 1;

        for (int k = 0; k < blocks; k++) {
            int b = -1;
            if (k < 6)
                b = n->blocks[k];
            else if (n->indirect_block >= 0) {
                unsigned short *ind =
                    (unsigned short *)part.data_blocks[n->indirect_block].d;
                b = ind[k - 6];
                set_bit(block_map, n->indirect_block);
            }
            if (b > 0 && b < 4088)
                set_bit(block_map, b);
        }
    }

    memset(file_table, 0, sizeof(file_table));
    return 0;
}

// Write in-memory filesystem back to disk 
static void fs_sync(void) {
    FILE *f = fopen(DISK_IMAGE, "r+b");
    if (!f) return;
    fwrite(&part, 1, sizeof(part), f);
    fclose(f);
}

// root 
/*
 * Locate the root directory by finding a directory
 * whose ".." entry points to itself.
 */
static int find_root(void) {
    for (int i = 1; i < 224; i++) {
        struct inode *n = &part.inode_table[i];
        if (!(n->mode & INODE_MODE_DIR_FILE)) continue;

        struct dentry d;
        int off = 0;
        while (off < n->size) {
            memcpy(&d,
                   part.data_blocks[n->blocks[0]].d + off,
                   sizeof(d));
            if (strcmp((char *)d.name, "..") == 0 && d.inode == i)
                return i;
            off += d.dir_length;
        }
    }
    return -1;
}

// blocks 
/*
 * Translate a logical block number to a physical block number.
 * Allocates blocks if requested and necessary.
 */

static int get_block(struct inode *n, int logical, int alloc) {
    if (logical < 6) {
        if (n->blocks[logical] == 0 && alloc) {
            int b = find_free(block_map, 4088);
            if (b < 0) return -1;
            set_bit(block_map, b);
            n->blocks[logical] = b;
            memset(part.data_blocks[b].d, 0, BLOCK_SIZE);
        }
        return n->blocks[logical];
    }

    if (n->indirect_block < 0) {
        if (!alloc) return -1;
        int b = find_free(block_map, 4088);
        if (b < 0) return -1;
        set_bit(block_map, b);
        n->indirect_block = b;
        memset(part.data_blocks[b].d, 0, BLOCK_SIZE);
    }

    unsigned short *ind =
        (unsigned short *)part.data_blocks[n->indirect_block].d;

    int idx = logical - 6;
    if (ind[idx] == 0 && alloc) {
        int b = find_free(block_map, 4088);
        if (b < 0) return -1;
        set_bit(block_map, b);
        ind[idx] = b;
        memset(part.data_blocks[b].d, 0, BLOCK_SIZE);
    }
    return ind[idx];
}

// inode rw 
/*
 * Unified read/write function for inodes.
 * Handles block translation, offsets, and size updates.
 */
static int inode_rw(int inum, void *buf, int size, int off, int write) {
    struct inode *n = &part.inode_table[inum];
    unsigned char *p = buf;
    int done = 0;

    while (done < size) {
        int blk = get_block(n, (off + done) / BLOCK_SIZE, write);
        if (blk < 0) break;

        int o = (off + done) % BLOCK_SIZE;
        int c = BLOCK_SIZE - o;
        if (c > size - done) c = size - done;

        if (write)
            memcpy(part.data_blocks[blk].d + o, p + done, c);
        else
            memcpy(p + done, part.data_blocks[blk].d + o, c);

        done += c;
    }

    if (write && off + done > n->size)
        n->size = off + done;

    return done;
}

//directory
// Lookup a filename inside a directory inode 

static int dir_lookup(int dir, const char *name) {
    struct inode *d = &part.inode_table[dir];
    struct dentry e;
    int off = 0;

    while (off < d->size) {
        inode_rw(dir, &e, sizeof(e), off, 0);
        e.name[e.name_len] = 0;
        if (strcmp((char *)e.name, name) == 0)
            return e.inode;
        off += e.dir_length;
    }
    return -1;
}

// Resolve a full path to an inode number 
static int resolve(const char *path) {
    char tmp[256];
    strcpy(tmp, path);

    int cur = root_inode;
    char *tok = strtok(tmp, "/");

    while (tok) {
        cur = dir_lookup(cur, tok);
        if (cur < 0) return -1;
        tok = strtok(NULL, "/");
    }
    return cur;
}

//open/read 
//Open a file and create an entry in the open file table
int fs_open(const char *path) {
    int inum = resolve(path);
    if (inum < 0) return -1;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].used) {
            file_table[i].used = 1;
            file_table[i].inum = inum;
            file_table[i].offset = 0;
            return i;
        }
    }
    return -1;
}
//Read from an open file descriptor
int fs_read(int fd, void *buf, int size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used)
        return -1;

    struct file *f = &file_table[fd];
    struct inode *n = &part.inode_table[f->inum];

    if (f->offset >= n->size)
        return 0;

    int remaining = n->size - f->offset;
    if (size > remaining)
        size = remaining;

    int r = inode_rw(f->inum, buf, size, f->offset, 0);
    f->offset += r;
    return r;
}
//Close file
void fs_close(int fd) {
    if (fd >= 0 && fd < MAX_OPEN_FILES)
        file_table[fd].used = 0;
}

// user commands

void fs_ls(const char *path) {
    int inum = resolve(path);
    if (inum < 0) return;

    struct inode *d = &part.inode_table[inum];
    struct dentry e;
    int off = 0;

    while (off < d->size) {
        inode_rw(inum, &e, sizeof(e), off, 0);
        e.name[e.name_len] = 0;
        printf("%s\n", e.name);
        off += e.dir_length;
    }
}

void fs_cat(const char *path) {
    int inum = resolve(path);
    if (inum < 0) return;

    char buf[256];
    int off = 0;

    while (off < part.inode_table[inum].size) {
        int r = inode_rw(inum, buf, sizeof(buf), off, 0);
        fwrite(buf, 1, r, stdout);
        off += r;
    }
}

//Check whether an inode represents a regular file
static int is_regular_file(int inum) {
    return !(part.inode_table[inum].mode & INODE_MODE_DIR_FILE);
}
//Uppercase text conversion is user is wrong
static void modify_text(char *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z')
            buf[i] -= 32;
}
//Randomly select files and display modified content
static void show_random_files(void) {
    struct inode *root = &part.inode_table[root_inode];
    struct dentry e;
    int off = 0;
    int files[64];
    int count = 0;

    while (off < root->size && count < 64) {
        inode_rw(root_inode, &e, sizeof(e), off, 0);
        off += e.dir_length;

        if (e.inode > 0 && is_regular_file(e.inode))
            files[count++] = e.inode;
    }

    srand(time(NULL));

    char buf[256];
    int shown = count < 10 ? count : 10;

    for (int i = 0; i < shown; i++) {
        int inum = files[rand() % count];
        int pos = 0;

        printf("\n--- inode %d  ---\n", inum);

        while (pos < part.inode_table[inum].size) {
            int r = inode_rw(inum, buf, sizeof(buf), pos, 0);
            if (r <= 0) break;

            modify_text(buf, r);
            fwrite(buf, 1, r, stdout);
            pos += r;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <file_path>\n", argv[0]);
        return 1;
    }

    if (fs_mount() < 0) {
        printf("Failed to mount filesystem\n");
        return 1;
    }

    root_inode = find_root();
    if (root_inode < 0) {
        printf("Root directory not found\n");
        return 1;
    }

    printf("=== Root directory ===\n");
    fs_ls("/");

    printf("\n=== cat %s (inode-based) ===\n", argv[1]);
    fs_cat(argv[1]);

    printf("\n=== cat %s (open/read) ===\n", argv[1]);
    int fd = fs_open(argv[1]);
    if (fd >= 0) {
        char buf[256];
        int r;
        while ((r = fs_read(fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, r, stdout);
        fs_close(fd);
    }

    printf("\n=== 10 random files ===\n");
    show_random_files();

    fs_sync();
    return 0;
}
