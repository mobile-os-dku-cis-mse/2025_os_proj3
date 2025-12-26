#include "simplefs.h"
#include "util.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

/* =========================
 * Bitmap helpers (fast, simple)
 * ========================= */
static inline int bm_test(const uint8_t* bm, uint32_t idx) {
    return (bm[idx >> 3] >> (idx & 7u)) & 1u;
}
static inline void bm_set(uint8_t* bm, uint32_t idx) {
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}
static inline void bm_clear(uint8_t* bm, uint32_t idx) {
    bm[idx >> 3] &= (uint8_t)~(1u << (idx & 7u));
}

/* sb.padding packing:
 * [0..31]   inode bitmap (224 bits => 28 bytes, but reserve 32 for alignment)
 * [32..543] block bitmap (4088 bits => 511 bytes, reserve 512)
 * remaining bytes unused for now
 */
#define SB_INODE_BM_OFF   0u
#define SB_INODE_BM_LEN   32u
#define SB_BLOCK_BM_OFF   32u
#define SB_BLOCK_BM_LEN   512u

static uint64_t disk_off_inode_table(void) {
    return (uint64_t)BLOCK_SIZE; // right after superblock
}
static uint64_t disk_off_data_block(const FS* fs, uint32_t data_bi) {
    const uint64_t bs = (fs->sb.block_size != 0u) ? fs->sb.block_size : BLOCK_SIZE;
    return (uint64_t)fs->sb.first_data_block * bs + (uint64_t)data_bi * bs;
}

static int disk_pread_block(FS* fs, uint32_t data_bi, void* out) {
    const uint64_t off = disk_off_data_block(fs, data_bi);
    if (fseeko(fs->disk, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(out, 1, BLOCK_SIZE, fs->disk) == BLOCK_SIZE ? 0 : -1;
}
static int disk_pwrite_block(FS* fs, uint32_t data_bi, const void* in) {
    const uint64_t off = disk_off_data_block(fs, data_bi);
    if (fseeko(fs->disk, (off_t)off, SEEK_SET) != 0) return -1;
    return fwrite(in, 1, BLOCK_SIZE, fs->disk) == BLOCK_SIZE ? 0 : -1;
}

static int disk_write_inode_table(FS* fs) {
    if (fseeko(fs->disk, (off_t)disk_off_inode_table(), SEEK_SET) != 0) return -1;
    return fwrite(fs->inode_table, 1, sizeof(fs->inode_table), fs->disk) == sizeof(fs->inode_table) ? 0 : -1;
}
static int disk_write_super(FS* fs) {
    if (fseeko(fs->disk, 0, SEEK_SET) != 0) return -1;
    return fwrite(&fs->sb, 1, sizeof(fs->sb), fs->disk) == sizeof(fs->sb) ? 0 : -1;
}

/* =========================
 * BufCache: hash (open addressing) + LRU + pin + dirty writeback
 * "free page frames" are fixed Buf objects.
 * ========================= */
static uint32_t hash_u32(uint32_t x) {
    // cheap mix (good enough for block indices)
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void bcache_lru_detach(BufCache* bc, Buf* b) {
    if (b->prev) b->prev->next = b->next;
    if (b->next) b->next->prev = b->prev;
    if (bc->lru_head == b) bc->lru_head = b->next;
    if (bc->lru_tail == b) bc->lru_tail = b->prev;
    b->prev = b->next = NULL;
}
static void bcache_lru_push_front(BufCache* bc, Buf* b) {
    b->prev = NULL;
    b->next = bc->lru_head;
    if (bc->lru_head) bc->lru_head->prev = b;
    bc->lru_head = b;
    if (!bc->lru_tail) bc->lru_tail = b;
}
static void bcache_lru_touch(BufCache* bc, Buf* b) {
    if (bc->lru_head == b) return;
    bcache_lru_detach(bc, b);
    bcache_lru_push_front(bc, b);
}

static int bcache_hash_init(BufCache* bc, size_t cap) {
    bc->h_cap = cap;
    bc->h_used = 0;
    bc->h_keys = (uint32_t*)malloc(sizeof(uint32_t) * cap);
    bc->h_vals = (Buf**)malloc(sizeof(Buf*) * cap);
    if (!bc->h_keys || !bc->h_vals) return -1;
    for (size_t i = 0; i < cap; i++) {
        bc->h_keys[i] = UINT32_MAX;
        bc->h_vals[i] = NULL;
    }
    return 0;
}

static Buf* bcache_hash_get(BufCache* bc, uint32_t key) {
    const size_t cap = bc->h_cap;
    size_t i = (size_t)hash_u32(key) & (cap - 1u);
    for (;;) {
        uint32_t k = bc->h_keys[i];
        if (k == UINT32_MAX) return NULL;
        if (k == key) return bc->h_vals[i];
        i = (i + 1u) & (cap - 1u);
    }
}

static int bcache_hash_put(BufCache* bc, uint32_t key, Buf* val) {
    const size_t cap = bc->h_cap;
    size_t i = (size_t)hash_u32(key) & (cap - 1u);
    for (;;) {
        if (bc->h_keys[i] == UINT32_MAX || bc->h_keys[i] == key) {
            if (bc->h_keys[i] == UINT32_MAX) bc->h_used++;
            bc->h_keys[i] = key;
            bc->h_vals[i] = val;
            return 0;
        }
        i = (i + 1u) & (cap - 1u);
    }
}

static void bcache_hash_del(BufCache* bc, uint32_t key) {
    // simple delete (tombstone-free) by rehashing cluster (cap is small; acceptable)
    // For scalability you'd implement tombstones; we keep it readable.
    size_t cap = bc->h_cap;
    size_t i = (size_t)hash_u32(key) & (cap - 1u);
    for (;;) {
        if (bc->h_keys[i] == UINT32_MAX) return;
        if (bc->h_keys[i] == key) break;
        i = (i + 1u) & (cap - 1u);
    }
    bc->h_keys[i] = UINT32_MAX;
    bc->h_vals[i] = NULL;

    // reinsert subsequent cluster
    for (size_t j = (i + 1u) & (cap - 1u); bc->h_keys[j] != UINT32_MAX; j = (j + 1u) & (cap - 1u)) {
        uint32_t k = bc->h_keys[j];
        Buf* v = bc->h_vals[j];
        bc->h_keys[j] = UINT32_MAX;
        bc->h_vals[j] = NULL;
        bcache_hash_put(bc, k, v);
    }
}

static int bcache_init(BufCache* bc, size_t nbuf) {
    memset(bc, 0, sizeof(*bc));
    bc->pool = (Buf*)calloc(nbuf, sizeof(Buf));
    if (!bc->pool) return -1;
    bc->nbuf = nbuf;

    // hash capacity: power of two >= 2*nbuf (to keep load factor low)
    size_t cap = 1;
    while (cap < (nbuf * 2u)) cap <<= 1u;
    if (bcache_hash_init(bc, cap) != 0) return -1;

    // All buffers start as invalid and on LRU list
    for (size_t i = 0; i < nbuf; i++) {
        bcache_lru_push_front(bc, &bc->pool[i]);
    }
    return 0;
}

static void bcache_free(BufCache* bc) {
    free(bc->h_keys);
    free(bc->h_vals);
    free(bc->pool);
    memset(bc, 0, sizeof(*bc));
}

static int bcache_flush_one(FS* fs, Buf* b) {
    if (b->valid && b->dirty) {
        if (disk_pwrite_block(fs, b->bi, b->data) != 0) return -1;
        b->dirty = 0;
        fs->bcache.writebacks++;
    }
    return 0;
}

static Buf* bcache_getblk(FS* fs, uint32_t bi) {
    BufCache* bc = &fs->bcache;

    Buf* hit = bcache_hash_get(bc, bi);
    if (hit) {
        bc->hits++;
        hit->pin++;
        bcache_lru_touch(bc, hit);
        return hit;
    }
    bc->misses++;

    // select eviction candidate from LRU tail that is unpinned
    Buf* v = bc->lru_tail;
    while (v && v->pin != 0) v = v->prev;
    if (!v) return NULL; // all pinned (shouldn't happen in this project)

    bc->evictions++;
    // remove old mapping if valid
    if (v->valid) {
        if (bcache_flush_one(fs, v) != 0) return NULL;
        bcache_hash_del(bc, v->bi);
    }

    // fill
    if (disk_pread_block(fs, bi, v->data) != 0) return NULL;
    v->bi = bi;
    v->valid = 1;
    v->dirty = 0;
    v->pin = 1;

    bcache_hash_put(bc, bi, v);
    bcache_lru_touch(bc, v);
    return v;
}

static void bcache_mark_dirty(Buf* b) { b->dirty = 1; }
static void bcache_brelse(BufCache* bc, Buf* b) { if (b && b->pin) b->pin--; (void)bc; }

static int bcache_sync_all(FS* fs) {
    BufCache* bc = &fs->bcache;
    for (size_t i = 0; i < bc->nbuf; i++) {
        if (bcache_flush_one(fs, &bc->pool[i]) != 0) return -1;
    }
    return 0;
}

/* =========================
 * DirHash (Extra #5)
 * ========================= */
static uint32_t fnv1a(const char* s, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}
static int dirhash_init(DirHash* h, size_t nb) {
    h->nbuckets = nb;
    h->buckets = (DirNode**)calloc(nb, sizeof(DirNode*));
    return h->buckets ? 0 : -1;
}
static void dirhash_free(DirHash* h) {
    for (size_t i = 0; i < h->nbuckets; i++) {
        DirNode* cur = h->buckets[i];
        while (cur) {
            DirNode* nx = cur->next;
            free(cur->name);
            free(cur);
            cur = nx;
        }
    }
    free(h->buckets);
    memset(h, 0, sizeof(*h));
}
static int dirhash_put(DirHash* h, const char* name, uint32_t len, uint32_t ino) {
    uint32_t hv = fnv1a(name, len);
    size_t b = (size_t)hv % h->nbuckets;

    DirNode* node = (DirNode*)malloc(sizeof(*node));
    if (!node) return -1;
    node->name = (char*)malloc((size_t)len + 1u);
    if (!node->name) { free(node); return -1; }
    memcpy(node->name, name, len);
    node->name[len] = '\0';
    node->name_len = len;
    node->ino = ino;
    node->next = h->buckets[b];
    h->buckets[b] = node;
    return 0;
}
static int dirhash_get(const DirHash* h, const char* name, uint32_t len, uint32_t* out_ino) {
    uint32_t hv = fnv1a(name, len);
    size_t b = (size_t)hv % h->nbuckets;
    for (DirNode* cur = h->buckets[b]; cur; cur = cur->next) {
        if (cur->name_len == len && memcmp(cur->name, name, len) == 0) {
            *out_ino = cur->ino;
            return 0;
        }
    }
    return -1;
}

/* =========================
 * Block allocator (Extra #6)
 * ========================= */
static int alloc_inode(FS* fs, uint32_t* out_ino) {
    for (uint32_t i = 0; i < fs->inode_count; i++) {
        if (!bm_test(fs->inode_bm, i)) {
            bm_set(fs->inode_bm, i);
            fs->sb.num_free_inodes--;
            *out_ino = i;
            return 0;
        }
    }
    return -1;
}

static int alloc_dblk(FS* fs, uint32_t* out_bi) {
    for (uint32_t bi = 0; bi < fs->data_blocks; bi++) {
        if (!bm_test(fs->block_bm, bi)) {
            bm_set(fs->block_bm, bi);
            fs->sb.num_free_blocks--;
            *out_bi = bi;
            return 0;
        }
    }
    return -1;
}

static void free_dblk(FS* fs, uint32_t bi) {
    if (bi >= fs->data_blocks) return;
    if (bm_test(fs->block_bm, bi)) {
        bm_clear(fs->block_bm, bi);
        fs->sb.num_free_blocks++;
    }
}

/* =========================
 * Logical->physical block mapping (direct + single indirect)
 * ========================= */
static int inode_get_phys(FS* fs, struct inode* ino, uint32_t lbn, uint32_t* out_phys) {
    if (lbn < SIMPLEFS_DIRECT) {
        uint16_t v = ino->blocks[lbn];
        if (v == 0 && lbn != 0) { /* could be valid block 0; project image uses 0.. */ }
        *out_phys = v;
        return 0;
    }
    // indirect
    if (ino->indirect_block < 0) return -1;
    uint32_t idx = lbn - SIMPLEFS_DIRECT;
    if (idx >= (uint32_t)SIMPLEFS_PTRS_PER_INDIRECT) return -1;

    Buf* b = bcache_getblk(fs, (uint32_t)ino->indirect_block);
    if (!b) return -1;
    uint16_t* ptrs = (uint16_t*)b->data;
    *out_phys = (uint32_t)ptrs[idx];
    bcache_brelse(&fs->bcache, b);
    return 0;
}

static int inode_set_phys(FS* fs, struct inode* ino, uint32_t lbn, uint32_t phys) {
    if (lbn < SIMPLEFS_DIRECT) {
        ino->blocks[lbn] = (uint16_t)phys;
        return 0;
    }
    uint32_t idx = lbn - SIMPLEFS_DIRECT;
    if (idx >= (uint32_t)SIMPLEFS_PTRS_PER_INDIRECT) return -1;

    if (ino->indirect_block < 0) {
        uint32_t ib;
        if (alloc_dblk(fs, &ib) != 0) return -1;
        ino->indirect_block = (int32_t)ib;

        // initialize indirect block with zeros
        Buf* nb = bcache_getblk(fs, ib);
        if (!nb) return -1;
        memset(nb->data, 0, BLOCK_SIZE);
        bcache_mark_dirty(nb);
        bcache_brelse(&fs->bcache, nb);
    }

    Buf* b = bcache_getblk(fs, (uint32_t)ino->indirect_block);
    if (!b) return -1;
    uint16_t* ptrs = (uint16_t*)b->data;
    ptrs[idx] = (uint16_t)phys;
    bcache_mark_dirty(b);
    bcache_brelse(&fs->bcache, b);
    return 0;
}


#define FAIL(msg) do { \
    fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
    return -1; \
} while (0)

#define SIMPLEFS_ROOT_INO(fs) ((fs)->sb.first_inode)
/* =========================
 * Directory parsing helpers
 * We store fixed-length dentry records for simplicity/scalability.
 * Each record is sizeof(struct dentry) (272 bytes).
 * ========================= */
static int parse_root_and_build_cache(FS* fs) {
    if (dirhash_init(&fs->dircache, 1024) != 0)
        FAIL("dirhash_init failed");

    struct inode* root = &fs->inode_table[SIMPLEFS_ROOT_INO(fs)];
    uint32_t remain = root->size;
    uint32_t off = 0;

    while (remain >= sizeof(struct dentry)) {
        uint32_t lbn = off / BLOCK_SIZE;
        uint32_t inb = off % BLOCK_SIZE;

        uint32_t phys = 0;
        if (inode_get_phys(fs, root, lbn, &phys) != 0)
            FAIL("inode_get_phys failed");

        Buf* b = bcache_getblk(fs, phys);
        if (!b)
            FAIL("bcache_getblk failed");

        struct dentry de;
        memcpy(&de, b->data + inb, sizeof(de));
        bcache_brelse(&fs->bcache, b);

        if (de.dir_length == 0)
            FAIL("dir_length == 0");
        if (de.name_len == 0)
            FAIL("name_len == 0");
        if (de.name_len > 255)
            FAIL("name_len > 255");

        // if (de.dir_length < sizeof(struct dentry))
        //     FAIL("dir_length < sizeof(struct dentry)");

        // skip "." and ".." if present
        if (!(de.name_len == 1 && de.name[0] == '.') &&
            !(de.name_len == 2 && de.name[0] == '.' && de.name[1] == '.')) {

            if (dirhash_put(&fs->dircache,
                            (const char*)de.name,
                            de.name_len,
                            de.inode) != 0)
                FAIL("dirhash_put failed");
        }

        off += de.dir_length;
        remain -= de.dir_length;
    }

    return 0;
}

#ifdef FS_DEBUG
#define DEBUG_LOG(fmt, ...) \
    fprintf(stderr, "[fs_mount] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...)
#endif
/* =========================
 * FS lifecycle
 * ========================= */
int fs_mount(FS* fs, const char* disk_img_path) {
    memset(fs, 0, sizeof(*fs));

    fs->disk = fopen(disk_img_path, "rb+");
    if (!fs->disk) return -1;

    if (fread(&fs->sb, 1, sizeof(fs->sb), fs->disk) != sizeof(fs->sb)) {
        errno = EIO;
        return -1;
    }

    // derive counts
    fs->inode_count = (fs->sb.num_inodes != 0u && fs->sb.num_inodes <= MAX_INODES) ? fs->sb.num_inodes : MAX_INODES;
    // total blocks includes super + inode blocks + data blocks
    // assignment struct suggests 4096 total and 4088 data blocks.
    fs->data_blocks = (fs->sb.num_blocks != 0u) ? (fs->sb.num_blocks - fs->sb.first_data_block) : 4088u;

    // bitmap views
    fs->inode_bm = &fs->sb.padding[SB_INODE_BM_OFF];
    fs->block_bm = &fs->sb.padding[SB_BLOCK_BM_OFF];

    // load inode table
    if (fseeko(fs->disk, (off_t)disk_off_inode_table(), SEEK_SET) != 0) return -1;
    if (fread(fs->inode_table, 1, sizeof(fs->inode_table), fs->disk) != sizeof(fs->inode_table)) {
        errno = EIO;
        return -1;
    }

    #ifdef FS_DEBUG
    DEBUG_LOG("=== FS parsed state ===");

    /* disk */
    DEBUG_LOG("disk=%p", (void *)fs->disk);

    /* superblock */
    DEBUG_LOG("super_block:");
    DEBUG_LOG("  partition_type=%u", fs->sb.partition_type);
    DEBUG_LOG("  block_size=%u", fs->sb.block_size);
    DEBUG_LOG("  inode_size=%u", fs->sb.inode_size);
    DEBUG_LOG("  first_inode=%u", fs->sb.first_inode);
    DEBUG_LOG("  num_inodes=%u", fs->sb.num_inodes);
    DEBUG_LOG("  num_inode_blocks=%u", fs->sb.num_inode_blocks);
    DEBUG_LOG("  num_free_inodes=%u", fs->sb.num_free_inodes);
    DEBUG_LOG("  num_blocks=%u", fs->sb.num_blocks);
    DEBUG_LOG("  num_free_blocks=%u", fs->sb.num_free_blocks);
    DEBUG_LOG("  first_data_block=%u", fs->sb.first_data_block);
    DEBUG_LOG("  volume_name=\"%.*s\"",
              (int)sizeof(fs->sb.volume_name),
              fs->sb.volume_name);

    /* derived values */
    DEBUG_LOG("derived:");
    DEBUG_LOG("  inode_count=%u", fs->inode_count);
    DEBUG_LOG("  data_blocks=%u", fs->data_blocks);

    /* bitmap views */
    DEBUG_LOG("bitmaps:");
    DEBUG_LOG("  inode_bm=%p (sb.padding + %u)",
              (void *)fs->inode_bm, SB_INODE_BM_OFF);
    DEBUG_LOG("  block_bm=%p (sb.padding + %u)",
              (void *)fs->block_bm, SB_BLOCK_BM_OFF);

    /* root inode */
    struct inode *root = &fs->inode_table[SIMPLEFS_ROOT_INO(fs)];
    DEBUG_LOG("root inode:");
    DEBUG_LOG("  ino=%u", SIMPLEFS_ROOT_INO(fs));
    DEBUG_LOG("  mode=0x%x", root->mode);
    DEBUG_LOG("  locked=%u", root->locked);
    DEBUG_LOG("  date=%u", root->date);
    DEBUG_LOG("  size=%u", root->size);
    DEBUG_LOG("  indirect_block=%d", root->indirect_block);
    DEBUG_LOG("  direct_blocks={%u,%u,%u,%u,%u,%u}",
              root->blocks[0], root->blocks[1], root->blocks[2],
              root->blocks[3], root->blocks[4], root->blocks[5]);

    DEBUG_LOG("=== end FS parsed state ===");
#endif

    // init caches
    if (bcache_init(&fs->bcache, 128) != 0) return -1; // 128 "frames" (tuneable)

    // build root directory hash cache
    if (parse_root_and_build_cache(fs) != 0) return -1;

    return 0;
}

int fs_sync(FS* fs) {
    if (bcache_sync_all(fs) != 0) return -1;
    if (disk_write_inode_table(fs) != 0) return -1;
    if (disk_write_super(fs) != 0) return -1;
    fflush(fs->disk);
    return 0;
}

int fs_umount(FS* fs) {
    if (!fs->disk) return 0;
    (void)fs_sync(fs);
    dirhash_free(&fs->dircache);
    bcache_free(&fs->bcache);
    fclose(fs->disk);
    memset(fs, 0, sizeof(*fs));
    return 0;
}

/* =========================
 * Printing (prof-friendly demo output)
 * ========================= */
void fs_print_super(const FS* fs) {
    printf("== Superblock ==\n");
    printf("volume_name      : %.24s\n", fs->sb.volume_name);
    printf("block_size       : %u\n", fs->sb.block_size);
    printf("inode_size       : %u\n", fs->sb.inode_size);
    printf("num_inodes       : %u\n", fs->sb.num_inodes);
    printf("num_blocks       : %u\n", fs->sb.num_blocks);
    printf("first_data_block : %u\n", fs->sb.first_data_block);
    printf("free_inodes      : %u\n", fs->sb.num_free_inodes);
    printf("free_blocks      : %u\n", fs->sb.num_free_blocks);
    printf("\n");
}

void fs_print_root_ls(FS* fs) {
    struct inode* root = &fs->inode_table[SIMPLEFS_ROOT_INO(fs)];
    printf("== Root directory listing (/) ==\n");
    printf("(inode=%u, size=%u bytes)\n", SIMPLEFS_ROOT_INO(fs), root->size);

    uint32_t remain = root->size;
    uint32_t off = 0;
    while (remain >= sizeof(struct dentry)) {
        uint32_t lbn = off / BLOCK_SIZE;
        uint32_t inb = off % BLOCK_SIZE;

        uint32_t phys = 0;
        if (inode_get_phys(fs, root, lbn, &phys) != 0) break;

        Buf* b = bcache_getblk(fs, phys);
        if (!b) break;

        struct dentry de;
        memcpy(&de, b->data + inb, sizeof(de));
        bcache_brelse(&fs->bcache, b);

        char name[256];
        uint32_t n = de.name_len < 255u ? de.name_len : 255u;
        memcpy(name, de.name, n);
        name[n] = '\0';

        printf("inode=%u  type=%u  name=%s\n", de.inode, de.file_type, name);

        off += de.dir_length;
        remain -= de.dir_length;
    }
    printf("\n");
}

/* =========================
 * Syscalls
 * ========================= */
int sys_open(PCB* pcb, FS* fs, const char* pathname, int flags) {
    if (!pathname || pathname[0] != '/' || pathname[1] == '\0') return -1;
    const char* name = pathname + 1;
    uint32_t len = (uint32_t)strnlen(name, 255);

    uint32_t ino = 0;
    if (dirhash_get(&fs->dircache, name, len, &ino) != 0) {
        // fallback: could scan directory again, but cache should cover root
        return -1;
    }
    if (ino >= MAX_INODES) return -1;

    for (int fd = 0; fd < (int)MAX_FD; fd++) {
        if (!pcb->fdtable[fd].used) {
            pcb->fdtable[fd].used = 1;
            pcb->fdtable[fd].inode_no = ino;
            pcb->fdtable[fd].offset = 0;
            pcb->fdtable[fd].flags = flags;
            return fd;
        }
    }
    return -1;
}

int sys_close(PCB* pcb, FS* fs, int fd) {
    (void)fs;
    if (fd < 0 || fd >= (int)MAX_FD) return -1;
    if (!pcb->fdtable[fd].used) return -1;
    memset(&pcb->fdtable[fd], 0, sizeof(pcb->fdtable[fd]));
    return 0;
}

int sys_read(PCB* pcb, FS* fs, int fd, void* user_buf, uint32_t req_size) {
    if (fd < 0 || fd >= (int)MAX_FD) return -1;
    if (!pcb->fdtable[fd].used || !user_buf) return -1;

    File* f = &pcb->fdtable[fd];
    struct inode* ino = &fs->inode_table[f->inode_no];

    if (f->offset >= ino->size) return 0;

    uint32_t can = ino->size - f->offset;
    uint32_t todo = (req_size < can) ? req_size : can;

    uint8_t* out = (uint8_t*)user_buf;
    uint32_t done = 0;

    while (done < todo) {
        uint32_t cur = f->offset + done;
        uint32_t lbn = cur / BLOCK_SIZE;
        uint32_t inb = cur % BLOCK_SIZE;

        uint32_t phys = 0;
        if (inode_get_phys(fs, ino, lbn, &phys) != 0) break;

        Buf* b = bcache_getblk(fs, phys);
        if (!b) return -1;

        uint32_t chunk = u32_min(BLOCK_SIZE - inb, todo - done);
        memcpy(out + done, b->data + inb, chunk);

        bcache_brelse(&fs->bcache, b);
        done += chunk;
    }

    f->offset += done;
    return (int)done;
}

/* write() implements overwrite-from-beginning semantics (assignment extra).
 * - allocates new blocks if required
 * - frees unused old blocks beyond new size
 * - supports single-indirect for scalability
 */
static int inode_ensure_blocks(FS* fs, struct inode* ino, uint32_t need_blocks) {
    // allocate required blocks and set pointers
    for (uint32_t lbn = 0; lbn < need_blocks; lbn++) {
        uint32_t phys = 0;
        if (inode_get_phys(fs, ino, lbn, &phys) == 0 && phys != 0) continue;

        uint32_t nb;
        if (alloc_dblk(fs, &nb) != 0) return -1;
        if (inode_set_phys(fs, ino, lbn, nb) != 0) return -1;
    }
    return 0;
}

static void inode_free_excess(FS* fs, struct inode* ino, uint32_t keep_blocks) {
    // free direct blocks beyond keep
    for (uint32_t lbn = keep_blocks; lbn < SIMPLEFS_DIRECT; lbn++) {
        if (ino->blocks[lbn] != 0) {
            free_dblk(fs, ino->blocks[lbn]);
            ino->blocks[lbn] = 0;
        }
    }

    // handle indirect: free entries beyond keep_blocks
    if (ino->indirect_block >= 0) {
        Buf* b = bcache_getblk(fs, (uint32_t)ino->indirect_block);
        if (!b) return;

        uint16_t* ptrs = (uint16_t*)b->data;

        uint32_t start = (keep_blocks > SIMPLEFS_DIRECT) ? (keep_blocks - SIMPLEFS_DIRECT) : 0;
        for (uint32_t i = start; i < (uint32_t)SIMPLEFS_PTRS_PER_INDIRECT; i++) {
            if (ptrs[i] != 0) {
                free_dblk(fs, ptrs[i]);
                ptrs[i] = 0;
                bcache_mark_dirty(b);
            }
        }
        bcache_brelse(&fs->bcache, b);

        // if keep_blocks <= direct and indirect is now empty, you could free the indirect block itself
        // we keep it for simplicity (still correct).
    }
}

int sys_write(PCB* pcb, FS* fs, int fd, const void* user_buf, uint32_t nbytes) {
    if (fd < 0 || fd >= (int)MAX_FD) return -1;
    if (!pcb->fdtable[fd].used || !user_buf) return -1;

    File* f = &pcb->fdtable[fd];
    if (!(f->flags & O_WR)) return -1;

    struct inode* ino = &fs->inode_table[f->inode_no];

    // overwrite from beginning (spec)
    f->offset = 0;

    uint32_t need_blocks = (nbytes + BLOCK_SIZE - 1u) / BLOCK_SIZE;

    // 1) ensure blocks exist (allocator)
    if (inode_ensure_blocks(fs, ino, need_blocks) != 0) return -1;

    // 2) write through buffer cache (dirty)
    const uint8_t* in = (const uint8_t*)user_buf;
    uint32_t done = 0;

    while (done < nbytes) {
        uint32_t lbn = done / BLOCK_SIZE;
        uint32_t inb = done % BLOCK_SIZE;

        uint32_t phys = 0;
        if (inode_get_phys(fs, ino, lbn, &phys) != 0) return -1;

        Buf* b = bcache_getblk(fs, phys);
        if (!b) return -1;

        uint32_t chunk = u32_min(BLOCK_SIZE - inb, nbytes - done);
        memcpy(b->data + inb, in + done, chunk);
        bcache_mark_dirty(b);
        bcache_brelse(&fs->bcache, b);

        done += chunk;
    }

    // 3) update size + free old excess blocks
    ino->size = nbytes;
    inode_free_excess(fs, ino, need_blocks);

    // persist eventually (close/fs_sync)
    return (int)nbytes;
}

/* Demo helper: collect names by iterating directory blocks (not by scanning dirhash)
 * so you can still pick random files without needing an iterator for the hash.
 */
char** collect_root_filenames(FS* fs, size_t* out_count) {
    struct inode* root = &fs->inode_table[SIMPLEFS_ROOT_INO(fs)];

    size_t cap = 64, n = 0;
    char** arr = (char**)calloc(cap, sizeof(char*));
    if (!arr) return NULL;

    uint32_t remain = root->size;
    uint32_t off = 0;

    while (remain >= sizeof(struct dentry)) {
        uint32_t lbn = off / BLOCK_SIZE;
        uint32_t inb = off % BLOCK_SIZE;

        uint32_t phys = 0;
        if (inode_get_phys(fs, root, lbn, &phys) != 0) break;

        Buf* b = bcache_getblk(fs, phys);
        if (!b) break;

        struct dentry de;
        memcpy(&de, b->data + inb, sizeof(de));
        bcache_brelse(&fs->bcache, b);

        if (de.name_len > 0 && de.name_len <= 255) {
            char* s = (char*)malloc((size_t)de.name_len + 1u);
            memcpy(s, de.name, de.name_len);
            s[de.name_len] = '\0';

            if (n == cap) {
                cap *= 2;
                char** tmp = (char**)realloc(arr, cap * sizeof(char*));
                if (!tmp) { free(s); break; }
                arr = tmp;
            }
            arr[n++] = s;
        }

        off += de.dir_length;
        remain -= de.dir_length;
    }

    *out_count = n;
    return arr;
}

void free_filenames(char** names, size_t n) {
    if (!names) return;
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}
