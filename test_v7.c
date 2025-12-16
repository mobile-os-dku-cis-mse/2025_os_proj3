#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>
#include "fs.h"




struct partition *part = NULL;

unsigned char inode_mapping[224];
unsigned char block_mapping[4088];

int root_inode; // index of root inode

#define O_RD 0 // read-only flag
#define MAX_OPEN_FILES 16 // max open files per process
#define MAX_DIR_ENTRIES 1024 // max entries in root dir
#define READ_CHUNK 1024 // read chunk size for fs_read

// dircache constants 
#define MAX_INODES_CACHE 224
#define DCACHE_BUCKETS 64

// file (open-file) structure kept in PCB for the single user 
struct file_desc {
    int fd;
    int inum;
    struct inode *inode;
    unsigned int offset;
    int flags;
};

static struct file_desc *fd_table[MAX_OPEN_FILES] = {0}; // open file descriptors table

/* === 1. Bitwise helpers === */

// set bit in bitmap
static void set_bit(unsigned char *bitmap, int pos) {
    bitmap[pos/8] |= (1 << (pos%8));
}
//check bit value in bitmap
static int test_bit(unsigned char *bitmap, int pos) {
    return (bitmap[pos/8] & (1 << (pos%8))) != 0;
}
// find first free bit in bitmap
static int find_free_bit(unsigned char *bitmap, int max) {
    for (int i = 1; i < max; i++) {
        if (!test_bit(bitmap, i)) return i;
    }
    return -1;
}

/* === 2. Disk and inode helpers === */

// load inode by index from partition
static struct inode* load_inode(struct partition *p, int inode_idx) {
    if (!p || inode_idx < 0 || inode_idx >= (int)p->s.num_inodes) return NULL;
    return &p->inode_table[inode_idx];
}
// allocate a free block and mark it in block bitmap
static int alloc_block(struct partition *p, unsigned char *block_bitmap) {
    int bnum = find_free_bit(block_bitmap, p->s.num_blocks);
    if (bnum < 0) return -1;
    set_bit(block_bitmap, bnum);
    p->s.num_free_blocks--;
    memset(p->data_blocks[bnum].d, 0, BLOCK_SIZE);
    return bnum;
}
// allocate a free inode and mark it in inode bitmap
static int alloc_inode(struct partition *p, unsigned char *inode_bitmap) {
    int inum = find_free_bit(inode_bitmap, p->s.num_inodes);
    if (inum < 0) return -1;
    set_bit(inode_bitmap, inum);
    p->s.num_free_inodes--;
    memset(&p->inode_table[inum], 0, sizeof(struct inode));
    p->inode_table[inum].indirect_block = -1;
    return inum;
}

/* === 3. Bitmaps === */

// rebuild inode and block bitmaps from partition data 
void rebuild_bitmaps(struct partition *p, unsigned char *inode_map, unsigned char *block_map) {
    memset(inode_map, 0, 224/8 + 1);
    memset(block_map, 0, 4088/8 + 1);
    set_bit(inode_map, 0); // Reserved 0 as invalid inode
    set_bit(block_map, 0); // Reserved

    for (unsigned int i = 1; i < p->s.num_inodes; i++) {
        struct inode *node = &p->inode_table[i];
        if (node->mode == 0) continue; // Free inode

        set_bit(inode_map, i);

        int blocks_needed = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (node->size == 0 && (node->mode & INODE_MODE_DIR_FILE)) {
            blocks_needed = 1;
        }

        for (int k = 0; k < blocks_needed; k++) {
            int blk = -1;
            if (k < 6) {
                blk = node->blocks[k];
            } else if (node->indirect_block >= 0) {
                if (k == 6) {
                    set_bit(block_map, node->indirect_block);
                }
                unsigned short *ind = (unsigned short*)p->data_blocks[node->indirect_block].d;
                blk = ind[k - 6];
            }

            if (blk > 0 && blk < (int)p->s.num_blocks) {
                set_bit(block_map, blk);
            }
        }
    }
}

/* === 4. Directory cache (dircache) implementation === */

struct dir_cache_entry {
    char *name;
    int inum;
    struct dir_cache_entry *next;
};

struct dir_cache {
    struct dir_cache_entry *buckets[DCACHE_BUCKETS];
    int built;
};

static struct dir_cache *dir_caches[MAX_INODES_CACHE] = {0};

// simple djb2 hash function for strings
static unsigned long djb2_hash(const unsigned char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}
// get or create dir cache for a directory inode
static struct dir_cache *dircache_get_or_create(int dir_inum) {
    if (dir_inum < 0 || dir_inum >= MAX_INODES_CACHE) return NULL;
    if (!dir_caches[dir_inum]) {
        dir_caches[dir_inum] = calloc(1, sizeof(struct dir_cache));
    }
    return dir_caches[dir_inum];
}
// insert an entry into dir cache
static int dircache_insert(int dir_inum, const char *name, int inum) {
    if (!name || dir_inum < 0 || dir_inum >= MAX_INODES_CACHE) return -1;
    struct dir_cache *dc = dircache_get_or_create(dir_inum);
    if (!dc) return -1;

    unsigned long h = djb2_hash((const unsigned char*)name);
    unsigned idx = (unsigned)(h % DCACHE_BUCKETS);

    struct dir_cache_entry *e = dc->buckets[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) {
            e->inum = inum;
            return 0;
        }
        e = e->next;
    }

    e = malloc(sizeof(*e));
    if (!e) return -1;
    e->name = strdup(name);
    if (!e->name) { free(e); return -1; }
    e->inum = inum;
    e->next = dc->buckets[idx];
    dc->buckets[idx] = e;
    return 0;
}

/* returns >0 inode, 0 not found, -1 error or not built */
static int dircache_lookup(int dir_inum, const char *name) {
    if (!name || dir_inum < 0 || dir_inum >= MAX_INODES_CACHE) return -1;
    struct dir_cache *dc = dir_caches[dir_inum];
    if (!dc || !dc->built) return 0; /* treat not built as miss (0) */

    unsigned long h = djb2_hash((const unsigned char*)name);
    unsigned idx = (unsigned)(h % DCACHE_BUCKETS);

    struct dir_cache_entry *e = dc->buckets[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) {
            return e->inum;
        }
        e = e->next;
    }
    return 0;
}
// free cache for a single directory
static void dircache_free_dir(int dir_inum) {
    if (dir_inum < 0 || dir_inum >= MAX_INODES_CACHE) return;
    struct dir_cache *dc = dir_caches[dir_inum];
    if (!dc) return;
    for (int i = 0; i < DCACHE_BUCKETS; ++i) {
        struct dir_cache_entry *e = dc->buckets[i];
        while (e) {
            struct dir_cache_entry *nx = e->next;
            free(e->name);
            free(e);
            e = nx;
        }
        dc->buckets[i] = NULL;
    }
    free(dc);
    dir_caches[dir_inum] = NULL;
}
// free all caches (when exiting)
static void dircache_free_all(void) {
    for (int i = 0; i < MAX_INODES_CACHE; ++i) dircache_free_dir(i);
}

// forward declare inode_data used in cache build 
static int inode_data(struct partition *p, int inum, void *buf, unsigned int size, unsigned int offset, int write);

static int dircache_build_for_dir(struct partition *p, int dir_inum) {
    if (!p) return -1;
    if (dir_inum < 0 || dir_inum >= (int)p->s.num_inodes) return -1;

    struct inode *dir_inode = &p->inode_table[dir_inum];
    if (!(dir_inode->mode & INODE_MODE_DIR_FILE)) return -1;

    struct dir_cache *dc = dircache_get_or_create(dir_inum);
    if (!dc) return -1;
    if (dc->built) return 0;

    unsigned int offset = 0;
    struct dentry d;
    int inserted = 0;

    while (offset < dir_inode->size) {
        int r = inode_data(p, dir_inum, &d, sizeof(struct dentry), offset, 0);
        if (r < (int)sizeof(struct dentry)) break;
        if (d.dir_length == 0) break;
        if (d.inode != 0 && d.name && d.name[0] != '\0') {
            if (dircache_insert(dir_inum, (char*)d.name, d.inode) == 0) inserted++;
        }
        offset += d.dir_length;
    }

    dc->built = 1;
    return inserted;
}
// build caches for all directory inodes in partition (can be done at mount)
static void dircache_build_all_dirs(struct partition *p) {
    if (!p) return;
    int n = (int)p->s.num_inodes;
    for (int i = 1; i < n && i < MAX_INODES_CACHE; ++i) {
        struct inode *node = &p->inode_table[i];
        if (node->mode & INODE_MODE_DIR_FILE) {
            dircache_build_for_dir(p, i);
        }
    }
}
// invalidate cache for a directory
static void dircache_invalidate_dir(int dir_inum) {
    dircache_free_dir(dir_inum);
}

/* === 5. Block index mapping === */

// get physical block index for a logical block in an inode, allocate if needed
static int get_block_index(struct partition *p, unsigned char *block_mapping, struct inode *node, int logical_blk, int alloc) {
    if (!node) return -1;

    if ((unsigned int)(logical_blk * BLOCK_SIZE) < node->size) {
        if (logical_blk < 6) return node->blocks[logical_blk];
        if (node->indirect_block >= 0) {
            unsigned short *ind = (unsigned short*)p->data_blocks[node->indirect_block].d;
            return ind[logical_blk - 6];
        }
    }

    if (logical_blk < 6) {
        if (node->blocks[logical_blk] == 0 && !alloc) return -1;
        if (alloc && ((unsigned int)(logical_blk * BLOCK_SIZE) >= node->size)) {
             int b = alloc_block(p, block_mapping);
             if (b < 0) return -1;
             node->blocks[logical_blk] = b;
        }
        return node->blocks[logical_blk];
    }

    if (node->indirect_block < 0) {
        if (!alloc) return -1;
        int b = alloc_block(p, block_mapping);
        if (b < 0) return -1;
        node->indirect_block = b;
    }
    unsigned short *ind = (unsigned short*)p->data_blocks[node->indirect_block].d;
    int ind_idx = logical_blk - 6;
    if (ind_idx < 0 || ind_idx >= BLOCK_SIZE / (int)sizeof(unsigned short)) return -1;

    if (alloc && ind[ind_idx] == 0) {
        int b = alloc_block(p, block_mapping);
        if (b < 0) return -1;
        ind[ind_idx] = b;
    }
    return ind[ind_idx];
}

/* === 6. inode data read/write === */

// read or write data from an inode (write not implemented yet)
static int inode_data(struct partition *p, int inum, void *buf, unsigned int size, unsigned int offset, int write) {
    struct inode *node = load_inode(p, inum);
    if(!node) return -1;

    if(!write && offset >= node->size) return 0;
    if(!write && offset + size > node->size) size = node->size - offset;

    unsigned int bytes_processed = 0;
    unsigned char *ptr = (unsigned char*)buf;

    while(bytes_processed < size) {
        unsigned int current_offset = (offset + bytes_processed);
        int block = get_block_index(p, block_mapping, node, current_offset / BLOCK_SIZE, write);
        if (block < 0) break;

        if (block >= (int)p->s.num_blocks) break;

        unsigned int offset_in_block = current_offset % BLOCK_SIZE;
        unsigned int bytes_to_copy = BLOCK_SIZE - offset_in_block;
        if(bytes_to_copy > size - bytes_processed) bytes_to_copy = size - bytes_processed;

        if(write) {
            memcpy(p->data_blocks[block].d + offset_in_block, ptr + bytes_processed, bytes_to_copy);
        } else {
            memcpy(ptr + bytes_processed, p->data_blocks[block].d + offset_in_block, bytes_to_copy);
        }

        bytes_processed += bytes_to_copy;

        if(write && (offset + bytes_processed) > node->size) {
            node->size = offset + bytes_processed;
        }
    }

    return bytes_processed;
}

/* === 7. Directory lookup with cache === */

// returns >0 inode number, 0 not found, -1 error 
int find_entry_in_dir(struct partition *p, int dir_inum, const char *name) {
    if (!p || !name) return -1;
    if (dir_inum < 0 || dir_inum >= (int)p->s.num_inodes) return -1;

    struct inode *dir_inode = &p->inode_table[dir_inum];
    if (!(dir_inode->mode & INODE_MODE_DIR_FILE)) return -1;

    /* try cache first */
    int cached = dircache_lookup(dir_inum, name);
    if (cached > 0) {
        if (cached < (int)p->s.num_inodes && p->inode_table[cached].mode != 0) {
            return cached;
        }
        /* cached entry invalid -> invalidate cache and fall back */
        dircache_invalidate_dir(dir_inum);
    } else if (cached < 0) {
        /* error from cache - fall back to disk scan */
    }

    unsigned int offset = 0;
    struct dentry d;
    while (offset < dir_inode->size) {
        int r = inode_data(p, dir_inum, &d, sizeof(struct dentry), offset, 0);
        if (r < (int)sizeof(struct dentry)) break;
        if (d.dir_length == 0) break;

        if (d.inode != 0) {
            if (strcmp((char*)d.name, name) == 0) {
                /* insert into cache for future */
                dircache_insert(dir_inum, (char*)d.name, d.inode);
                return d.inode;
            }
        }
        offset += d.dir_length;
    }

    return 0;
}

/* === 8. find root inode === */
// find root inode by looking for directory whose ".." points to itself
static int find_root_inode(struct partition *p) {
    for(int i = 1; i < (int)p->s.num_inodes; i++) {
        struct inode *node = load_inode(p,i);
        if(node && (node->mode & INODE_MODE_DIR_FILE)) {
            int parent = find_entry_in_dir(p, i, "..");
            if(parent == i) {
                printf("Kernel: Found Root Inode at index %d (matches '..')\n", i);
                return i;
            }
        }
    }
    return -1;
}

/* === 9. mount === */
// mount partition from disk image, rebuild bitmaps, find root inode
int mount_root(struct partition *part, unsigned char inode_mapping[], unsigned char block_mapping[], int *root_idx) {
    FILE *f = fopen("../disk.img", "rb");
    if (!f) {
        perror("Start Error: Cannot open disk.img");
        return -1;
    }

    if (!part) {
        fclose(f);
        fprintf(stderr, "mount_root: partition pointer is NULL\n");
        return -1;
    }

    size_t nr = fread(part,1,sizeof(struct partition),f);
    fclose(f);
    if (nr != sizeof(struct partition)) {
        fprintf(stderr, "Warning: fread read %zu bytes (expected %zu)\n", nr, sizeof(struct partition));
    }

    if (part->s.partition_type != SIMPLE_PARTITION) {
        printf("Mount Error: Invalid partition type 0x%x\n", part->s.partition_type);
        return -1;
    }
    printf("Kernel: Mount Successful. Volume: %s\n", part->s.volume_name);

    rebuild_bitmaps(part, inode_mapping, block_mapping);

    /* build directory caches for faster lookups */
    dircache_build_all_dirs(part);

    if (root_idx) {
        *root_idx = find_root_inode(part);
        if (*root_idx == -1) {
            printf("Root inode not found! defaulting to 1\n");
            *root_idx = 1;
            return -1;
        }
    }
    return 0;
}

/* === 10. ls_root (debug printing) === */
// help function for ls_root to format mode string
void format_mode(unsigned int mode, char *mode_str) {
    strcpy(mode_str, "----------");
    if (mode & INODE_MODE_DIR_FILE) mode_str[0] = 'd';
    else if (mode & INODE_MODE_REG_FILE) mode_str[0] = '-';

    if (mode & INODE_MODE_AC_USER_R) mode_str[1] = 'r';
    if (mode & INODE_MODE_AC_USER_W) mode_str[2] = 'w';
    if (mode & INODE_MODE_AC_USER_X) mode_str[3] = 'x';

    if (mode & INODE_MODE_AC_GRP_R) mode_str[4] = 'r';
    if (mode & INODE_MODE_AC_GRP_W) mode_str[5] = 'w';
    if (mode & INODE_MODE_AC_GRP_X) mode_str[6] = 'x';

    if (mode & INODE_MODE_AC_OTHER_R) mode_str[7] = 'r';
    if (mode & INODE_MODE_AC_OTHER_W) mode_str[8] = 'w';
    if (mode & INODE_MODE_AC_OTHER_X) mode_str[9] = 'x';
}
 // print directory entries in root directory
void ls_root(struct partition *p) {
    int root_idx = find_root_inode(p);
    if (root_idx < 0) {
        printf("ls_root: root inode not found\n");
        return;
    }
    struct inode *node = &p->inode_table[root_idx];

    int blocks_needed = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (node->size == 0) blocks_needed = 1;

    printf("total %d\n", blocks_needed);
    char mode_str[11];
    char date_str[20];

    for (int k = 0; k < blocks_needed; k++) {
        int phys_block = -1;
        if (k < 6) phys_block = node->blocks[k];
        else if (node->indirect_block >= 0) {
             unsigned short *ind = (unsigned short*)p->data_blocks[node->indirect_block].d;
             phys_block = ind[k-6];
        }

        if (phys_block <= 0 || phys_block >= (int)p->s.num_blocks) continue;

        char *block_ptr = p->data_blocks[phys_block].d;
        int offset = 0;

        while (offset < BLOCK_SIZE) {
            struct dentry *d = (struct dentry *)(block_ptr + offset);
            if (d->dir_length == 0) break;

            if (d->inode == 0) {
                offset += d->dir_length;
                continue;
            }

            struct inode *f = &p->inode_table[d->inode];
            format_mode(f->mode, mode_str);

            time_t t = (time_t)f->date;
            struct tm *tm_info = localtime(&t);
            if (tm_info) strftime(date_str, sizeof(date_str), "%b %d %H:%M", tm_info);
            else strcpy(date_str, "Unknown");

            printf("%s %2d root root %6d %s %s\n",
                   mode_str,
                   (f->mode & INODE_MODE_DIR_FILE) ? 2 : 1,
                   f->size, date_str, (char*)d->name);

            offset += d->dir_length;
        }
    }
}

/* === 11. Path resolution (resolve_path) === */
// resolve absolute path to inode number, return parent and leaf inode numbers
int resolve_path(struct partition *part, const char *path, int *parent_out, int *leaf_out) {
    if (!part || !path || path[0] == '\0') return -1;
    char tmp[strlen(path) + 1];
    strcpy(tmp, path);

    int cur = root_inode;
    char *saveptr = NULL;
    char *tok = NULL;

    char *p = tmp;
    if (p[0] == '/') {
        while (*p == '/') p++;
    }

    tok = strtok_r(p, "/", &saveptr);
    int prev = -1;
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            /* nothing */
        } else if (strcmp(tok, "..") == 0) {
            int parent = find_entry_in_dir(part, cur, "..");
            if (parent <= 0) cur = root_inode;
            else cur = parent;
        } else {
            int child = find_entry_in_dir(part, cur, tok);
            if (child <= 0) {
                /* not found */
                if (strtok_r(NULL, "/", &saveptr) != NULL) {
                    /* missing interior dir */
                    return -1;
                } else {
                    if (parent_out) *parent_out = cur;
                    if (leaf_out) *leaf_out = 0;
                    return 0;
                }
            }
            prev = cur;
            cur = child;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }

    if (parent_out) *parent_out = prev >= 0 ? prev : root_inode;
    if (leaf_out) *leaf_out = cur;
    return 1;
}

/* === 12. File operations: fs_open/fs_read/fs_close === */
// open file by pathname, return fd
int fs_open(const char *pathname, int flags) {
    if (!pathname || !part) return -1;

    int parent = 0, leaf = 0;
    int res = resolve_path(part, pathname, &parent, &leaf);
    if (res < 0) return -1;
    if (res == 0) { /* not found */
        return -1;
    }
    int inum = leaf;

    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (fd_table[i] == NULL) { fd = i; break; }
    }
    if (fd < 0) return -1;

    struct file_desc *f = malloc(sizeof(struct file_desc));
    if (!f) return -1;
    f->fd = fd;
    f->inum = inum;
    f->inode = &part->inode_table[inum];
    f->offset = 0;
    f->flags = flags;
    fd_table[fd] = f;
    return fd;
}
// read from open file descriptor
ssize_t fs_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -1;
    struct file_desc *f = fd_table[fd];
    if (!f) return -1;
    if ((f->flags & O_RD) != O_RD && f->flags != O_RD) return -1;
    if (!buf || count == 0) return 0;
    int ret = inode_data(part, f->inum, buf, (unsigned int)count, f->offset, 0);
    if (ret < 0) return -1;
    f->offset += (unsigned int)ret;
    return ret;
}
// close open file descriptor
int fs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -1;
    struct file_desc *f = fd_table[fd];
    if (!f) return -1;
    free(f);
    fd_table[fd] = NULL;
    return 0;
}

/* === 13. Helpers used by the child (collect filenames) === */
static int collect_root_filenames(char ***out_names) {
    *out_names = NULL;
    if (!part) return 0;
    int root_idx = find_root_inode(part);
    if (root_idx < 0) return 0;
    struct inode *dnode = &part->inode_table[root_idx];

    char **names = malloc(sizeof(char*) * MAX_DIR_ENTRIES);
    if (!names) return 0;
    int count = 0;

    int blocks_needed = (dnode->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (dnode->size == 0) blocks_needed = 1;

    for (int k = 0; k < blocks_needed; k++) {
        int phys_block = -1;
        if (k < 6) phys_block = dnode->blocks[k];
        else if (dnode->indirect_block >= 0) {
             unsigned short *ind = (unsigned short*)part->data_blocks[dnode->indirect_block].d;
             phys_block = ind[k-6];
        }
        if (phys_block <= 0 || phys_block >= (int)part->s.num_blocks) continue;
        char *block_ptr = part->data_blocks[phys_block].d;
        int offset = 0;
        while (offset < BLOCK_SIZE) {
            struct dentry *d = (struct dentry *)(block_ptr + offset);
            if (d->dir_length == 0) break;
            if (d->inode != 0) {
                struct inode *fi = &part->inode_table[d->inode];
                if (fi->mode & INODE_MODE_REG_FILE) {
                    names[count] = strdup((char*)d->name);
                    if (!names[count]) {
                        for (int j = 0; j < count; ++j) free(names[j]);
                        free(names);
                        return 0;
                    }
                    count++;
                    if (count >= MAX_DIR_ENTRIES) break;
                }
            }
            offset += d->dir_length;
        }
        if (count >= MAX_DIR_ENTRIES) break;
    }

    *out_names = names;
    return count;
}

/* === 14. child_work: open/read/close random up to 10 files === */
static void child_work(void) {
    char **names = NULL;
    int n = collect_root_filenames(&names);
    if (n <= 0) {
        printf("[child] No regular files found in root directory.\n");
        return;
    }

    srand((unsigned int)time(NULL) ^ getpid());
    int to_read = n < 10 ? n : 10;

    int *idx = malloc(sizeof(int) * n);
    for (int i = 0; i < n; ++i) idx[i] = i;
    for (int i = n-1; i > 0; --i) {
        int j = rand() % (i+1);
        int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }

    for (int t = 0; t < to_read; ++t) {
        int sel = idx[t];
        const char *name = names[sel];
        printf("[child] Opening file: %s\n", name);
        int fd = fs_open(name, O_RD);
        if (fd < 0) {
            printf("[child] fs_open failed for %s\n", name);
            continue;
        }
        struct inode *in = &part->inode_table[ fd_table[fd]->inum ];
        unsigned int remaining = in->size;
        unsigned int total_read = 0;
        char *buf = malloc(READ_CHUNK);
        if (!buf) { fs_close(fd); continue; }

        while (remaining > 0) {
            size_t toread = remaining > READ_CHUNK ? READ_CHUNK : remaining;
            ssize_t r = fs_read(fd, buf, toread);
            if (r < 0) {
                printf("[child] read error on %s\n", name);
                break;
            }
            if (r == 0) break;
            size_t show = (total_read == 0) ? (r < 256 ? (size_t)r : 256) : 0;
            if (show > 0) {
                printf("----- start of %s (first %zu bytes) -----\n", name, show);
                fwrite(buf, 1, show, stdout);
                printf("\n----- end fragment -----\n");
            }
            total_read += (unsigned int)r;
            remaining -= (unsigned int)r;
        }
        free(buf);
        fs_close(fd);
        printf("[child] Finished reading %s, total bytes read: %u\n\n", name, total_read);
    }

    for (int i = 0; i < n; ++i) free(names[i]);
    free(names);
    free(idx);
}

/* === MAIN === */
int main() {
    part = malloc(sizeof(struct partition));
    if (!part) {
        fprintf(stderr, "Failed to allocate partition structure\n");
        return 1;
    }
    memset(part, 0, sizeof(struct partition));

    if (mount_root(part, inode_mapping, block_mapping, &root_inode) != 0) {
        fprintf(stderr, "mount_root failed (see messages)\n");
    }

    printf("\n--- Listing Root Directory ---\n");
    ls_root(part);

    printf("\n--- Child Process Work: Open/Read Files ---\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        dircache_free_all();
        free(part);
        return 1;
    } else if (pid == 0) {
        child_work();
        _exit(0);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        printf("[parent] child finished with status %d\n", status);
    }

    /* cleanup caches and partition */
    dircache_free_all();
    free(part);
    return 0;
}