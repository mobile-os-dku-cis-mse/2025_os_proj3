#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "util.h"

/* ============================================================
 * On-disk layout (matches assignment)
 *  Block 0: superblock (1024 bytes)
 *  Blocks 1..7: inode table (224 * 32 = 7168 bytes = 7 blocks)
 *  Blocks 8..: data blocks (4088 blocks if total blocks=4096)
 * ============================================================ */

 /*
 * inode bitmap:
 *   - one bit per inode
 *   - MAX_INODES = 224 → 224 bits = 28 bytes
 */
#define SB_INODE_BM_OFF   0
#define SB_INODE_BM_LEN   ((MAX_INODES + 7) / 8)

/*
 * block bitmap:
 *   - one bit per data block
 *   - data blocks = 4088 → 4088 bits = 511 bytes
 */
#define SB_BLOCK_BM_OFF   (SB_INODE_BM_OFF + SB_INODE_BM_LEN)
#define SB_BLOCK_BM_LEN   ((4096 - 8 + 7) / 8)   /* 4088 blocks */

/* Sanity check: bitmaps must fit into superblock padding */
#if (SB_INODE_BM_OFF + SB_INODE_BM_LEN + SB_BLOCK_BM_LEN) > 960
#error "Superblock padding overflow: bitmap layout invalid"
#endif

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
    char     volume_name[24];
    uint8_t  padding[960];
};

struct inode {
    uint32_t mode;           // file type/perm (course-defined)
    uint32_t locked;         // opened for write (optional)
    uint32_t date;
    uint32_t size;           // bytes
    int32_t  indirect_block; // -1 if none; else data block index
    uint16_t blocks[6];      // 6 direct data block indices
};

struct dentry {
    uint32_t inode;
    uint32_t dir_length; // record length (we store fixed 272 for simplicity)
    uint32_t name_len;
    uint32_t file_type;
    union {
        uint8_t name[255];
        uint8_t n_pad[16][16];
    };
};
#pragma pack(pop)

/* ---------- SimpleFS limits ---------- */
enum {
    SIMPLEFS_ROOT_INO = 0,
    SIMPLEFS_DIRECT   = 6,
    SIMPLEFS_PTRS_PER_INDIRECT = (int)(BLOCK_SIZE / sizeof(uint16_t)), // 512
};

/* ---------- File descriptor flags ---------- */
#define O_RD  1
#define O_WR  2

typedef struct {
    int used;
    uint32_t inode_no;
    uint32_t offset;
    int flags;
} File;

typedef struct {
    File fdtable[MAX_FD];
    int pid;
} PCB;

/* ---------- Buffer cache ("free page frames") ---------- */
typedef struct Buf {
    uint32_t bi;         // data block index
    uint8_t  data[BLOCK_SIZE];
    uint8_t  valid;
    uint8_t  dirty;
    uint16_t pin;

    // intrusive LRU
    struct Buf* prev;
    struct Buf* next;
} Buf;

typedef struct {
    Buf*    pool;
    size_t  nbuf;

    // LRU list: head=MRU, tail=LRU
    Buf* lru_head;
    Buf* lru_tail;

    // hash table: bi -> Buf*
    uint32_t* h_keys;   // data block index, or UINT32_MAX for empty
    Buf**     h_vals;
    size_t    h_cap;
    size_t    h_used;

    // stats (for report/debug)
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
} BufCache;

/* ---------- Directory hash cache ---------- */
typedef struct DirNode {
    struct DirNode* next;
    uint32_t ino;
    uint32_t name_len;
    char*    name; // NUL-terminated copy
} DirNode;

typedef struct {
    DirNode** buckets;
    size_t nbuckets;
} DirHash;

typedef struct {
    FILE* disk;
    struct super_block sb;
    struct inode inode_table[MAX_INODES];

    // allocator bitmaps stored in sb.padding (loaded into memory as views)
    uint8_t* inode_bm;   // bit i => inode i in use
    uint8_t* block_bm;   // bit bi => data block bi in use
    uint32_t data_blocks; // number of data blocks (derived)
    uint32_t inode_count;

    // caches
    DirHash  dircache;
    BufCache bcache;
} FS;

/* ============ FS public API ============ */
int  fs_mount(FS* fs, const char* disk_img_path);
int  fs_umount(FS* fs);
int  fs_sync(FS* fs);                 // flush dirty buffers + inode table + sb

void fs_print_super(const FS* fs);
void fs_print_root_ls(FS* fs);        // uses caches (safe)

int  sys_open(PCB* pcb, FS* fs, const char* pathname, int flags);
int  sys_read(PCB* pcb, FS* fs, int fd, void* user_buf, uint32_t req_size);
int  sys_write(PCB* pcb, FS* fs, int fd, const void* user_buf, uint32_t nbytes);
int  sys_close(PCB* pcb, FS* fs, int fd);

/* helper for demo */
char** collect_root_filenames(FS* fs, size_t* out_count);
void   free_filenames(char** names, size_t n);

/* ============ mk_simplefs (Extra #8) ============ */
int mk_simplefs_create(const char* out_img,
                       const char* volume_name,
                       uint32_t seed,
                       uint32_t n_random_files,
                       const char** host_files,
                       size_t host_files_n);

#endif
