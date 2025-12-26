#include "simplefs.h"
#include "util.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static void bm_zero(uint8_t* bm, size_t bytes) { memset(bm, 0, bytes); }

static void bm_set(uint8_t* bm, uint32_t idx) { bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u)); }
static int  bm_test(const uint8_t* bm, uint32_t idx) { return (bm[idx >> 3] >> (idx & 7u)) & 1u; }

static uint32_t alloc_free_bit(uint8_t* bm, uint32_t nbits) {
    for (uint32_t i = 0; i < nbits; i++) if (!bm_test(bm, i)) { bm_set(bm, i); return i; }
    return UINT32_MAX;
}

static uint32_t dentry_len_fixed(void) {
    return (uint32_t)sizeof(struct dentry); // fixed records (simple + scalable)
}

static void write_block(FILE* f, uint64_t off, const void* data) {
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) die("mk_simplefs seek");
    if (fwrite(data, 1, BLOCK_SIZE, f) != BLOCK_SIZE) die("mk_simplefs write");
}

static void write_inode_table(FILE* f, const struct inode* itab) {
    if (fseeko(f, (off_t)BLOCK_SIZE, SEEK_SET) != 0) die("mk_simplefs seek inode table");
    if (fwrite(itab, 1, MAX_INODES * sizeof(struct inode), f) != MAX_INODES * sizeof(struct inode))
        die("mk_simplefs write inode table");
}

static void fill_random_text(char* out, size_t n, uint32_t* state) {
    static const char alnum[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \n";
    for (size_t i = 0; i < n; i++) {
        *state = (*state * 1664525u) + 1013904223u;
        out[i] = alnum[(*state >> 24) % (sizeof(alnum) - 1)];
    }
}

int mk_simplefs_create(const char* out_img,
                       const char* volume_name,
                       uint32_t seed,
                       uint32_t n_random_files,
                       const char** host_files,
                       size_t host_files_n)
{
    FILE* f = fopen(out_img, "wb+");
    if (!f) return -1;

    // Create empty image: 4096 blocks
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < 4096u; i++) {
        if (fwrite(zero, 1, BLOCK_SIZE, f) != BLOCK_SIZE) return -1;
    }

    struct super_block sb;
    memset(&sb, 0, sizeof(sb));
    sb.partition_type = 0x12345678u;
    sb.block_size = BLOCK_SIZE;
    sb.inode_size = (uint32_t)sizeof(struct inode);
    sb.first_inode = 0;
    sb.num_inodes = MAX_INODES;
    sb.num_inode_blocks = 7;              // 7168 bytes
    sb.num_blocks = 4096;
    sb.first_data_block = 1 + sb.num_inode_blocks; // 8
    sb.num_free_inodes = MAX_INODES;
    sb.num_free_blocks = sb.num_blocks - sb.first_data_block;
    snprintf(sb.volume_name, sizeof(sb.volume_name), "%s", volume_name ? volume_name : "SIMPLEFS");

    // bitmaps in padding
    uint8_t* inode_bm = &sb.padding[SB_INODE_BM_OFF];
    uint8_t* block_bm = &sb.padding[SB_BLOCK_BM_OFF];
    bm_zero(inode_bm, SB_INODE_BM_LEN);
    bm_zero(block_bm, SB_BLOCK_BM_LEN);

    struct inode itab[MAX_INODES];
    memset(itab, 0, sizeof(itab));
    for (uint32_t i = 0; i < MAX_INODES; i++) itab[i].indirect_block = -1;

    // Allocate root inode = 0
    bm_set(inode_bm, 0);
    sb.num_free_inodes--;

    // Build file list: host files + random files
    uint32_t total_files = (uint32_t)host_files_n + n_random_files;
    if (total_files > (MAX_INODES - 1)) total_files = MAX_INODES - 1;

    // Root directory records: "." ".." + files
    // We'll store only file entries (skip dot for simplicity; still fine)
    uint32_t dir_entries = total_files;
    uint32_t dir_size = dir_entries * dentry_len_fixed();
    uint32_t dir_blocks = (dir_size + BLOCK_SIZE - 1u) / BLOCK_SIZE;

    // allocate data blocks for root dir
    for (uint32_t lbn = 0; lbn < dir_blocks && lbn < SIMPLEFS_DIRECT; lbn++) {
        uint32_t bi = alloc_free_bit(block_bm, sb.num_free_blocks + 0u);
        if (bi == UINT32_MAX) die("no blocks for root dir");
        itab[0].blocks[lbn] = (uint16_t)bi;
        sb.num_free_blocks--;
    }
    itab[0].size = dir_size;
    itab[0].mode = 0x4000u; // "dir" (course-defined; ok to be symbolic)

    // Helper to allocate blocks for file data and write content
    uint32_t next_ino = 1;
    uint32_t state = seed ? seed : (uint32_t)time(NULL);

    // Build root dir blocks in memory then flush
    uint8_t dirbuf[BLOCK_SIZE];
    uint32_t dir_off = 0;
    memset(dirbuf, 0, sizeof(dirbuf));

    for (uint32_t fi = 0; fi < total_files; fi++) {
        uint32_t ino = next_ino++;
        bm_set(inode_bm, ino);
        sb.num_free_inodes--;

        // name
        char name[64];
        if (fi < host_files_n) {
            // take basename-ish
            const char* p = host_files[fi];
            const char* slash = strrchr(p, '/');
            const char* base = slash ? slash + 1 : p;
            snprintf(name, sizeof(name), "h_%u_%s", fi, base);
        } else {
            snprintf(name, sizeof(name), "file_%u", fi);
        }
        uint32_t nlen = (uint32_t)strnlen(name, 255);

        // content
        uint8_t* content = NULL;
        uint32_t content_len = 0;

        if (fi < host_files_n) {
            FILE* hf = fopen(host_files[fi], "rb");
            if (hf) {
                fseeko(hf, 0, SEEK_END);
                long sz = ftell(hf);
                fseeko(hf, 0, SEEK_SET);
                if (sz < 0) sz = 0;
                content_len = (uint32_t)sz;
                content = (uint8_t*)malloc(content_len ? content_len : 1u);
                if (content_len && fread(content, 1, content_len, hf) != content_len) {
                    content_len = 0;
                }
                fclose(hf);
            }
        }
        if (!content) {
            // random
            content_len = 128u + (state % 1024u);
            content = (uint8_t*)malloc(content_len);
            fill_random_text((char*)content, content_len, &state);
        }

        // allocate file blocks (direct only for generator; runtime supports indirect anyway)
        uint32_t need_blocks = (content_len + BLOCK_SIZE - 1u) / BLOCK_SIZE;
        for (uint32_t lbn = 0; lbn < need_blocks && lbn < SIMPLEFS_DIRECT; lbn++) {
            uint32_t bi = alloc_free_bit(block_bm, sb.num_free_blocks + 0u);
            if (bi == UINT32_MAX) die("no blocks for file");
            itab[ino].blocks[lbn] = (uint16_t)bi;
            sb.num_free_blocks--;

            uint8_t blk[BLOCK_SIZE];
            memset(blk, 0, sizeof(blk));
            uint32_t chunk = BLOCK_SIZE;
            uint32_t off = lbn * BLOCK_SIZE;
            if (off + chunk > content_len) chunk = content_len - off;
            memcpy(blk, content + off, chunk);

            uint64_t doff = (uint64_t)sb.first_data_block * BLOCK_SIZE + (uint64_t)bi * BLOCK_SIZE;
            write_block(f, doff, blk);
        }

        itab[ino].size = content_len;
        itab[ino].mode = 0x8000u; // "regular file"
        free(content);

        // emit dentry into root dir stream (fixed record)
        struct dentry de;
        memset(&de, 0, sizeof(de));
        de.inode = ino;
        de.dir_length = dentry_len_fixed();
        de.name_len = nlen;
        de.file_type = 1; // "regular" for demo
        memcpy(de.name, name, nlen);

        if (dir_off + sizeof(de) > BLOCK_SIZE) {
            // flush to current dir block and advance
            uint32_t blk_idx = (dir_off / BLOCK_SIZE) - 1; (void)blk_idx;
        }
        // write record into dirbuf; flush when full
        uint32_t pos = dir_off % BLOCK_SIZE;
        if (pos + sizeof(de) > BLOCK_SIZE) {
            // flush current buffer
            uint32_t which = dir_off / BLOCK_SIZE;
            uint32_t bi = itab[0].blocks[which];
            uint64_t doff = (uint64_t)sb.first_data_block * BLOCK_SIZE + (uint64_t)bi * BLOCK_SIZE;
            write_block(f, doff, dirbuf);
            memset(dirbuf, 0, sizeof(dirbuf));
            dir_off = which * BLOCK_SIZE; // align
            pos = 0;
        }
        memcpy(dirbuf + pos, &de, sizeof(de));
        dir_off += sizeof(de);
    }

    // flush last dir buffer
    if (dir_blocks > 0) {
        uint32_t which = (dir_off == 0) ? 0 : ((dir_off - 1u) / BLOCK_SIZE);
        uint32_t bi = itab[0].blocks[which];
        uint64_t doff = (uint64_t)sb.first_data_block * BLOCK_SIZE + (uint64_t)bi * BLOCK_SIZE;
        write_block(f, doff, dirbuf);
    }

    // Write superblock + inode table
    if (fseeko(f, 0, SEEK_SET) != 0) die("mk_simplefs seek super");
    if (fwrite(&sb, 1, sizeof(sb), f) != sizeof(sb)) die("mk_simplefs write super");
    write_inode_table(f, itab);

    fclose(f);
    return 0;
}

/* CLI tool */
static void usage(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s disk.img --random N [--seed S] [--volume NAME] [host_files...]\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 2; }

    const char* out = argv[1];
    uint32_t nrand = 0;
    uint32_t seed = 0;
    const char* vol = "SIMPLEFS";

    int i = 2;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--random") == 0 && i + 1 < argc) {
            nrand = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            vol = argv[++i];
        } else {
            break;
        }
    }

    const char** host = (const char**)&argv[i];
    size_t host_n = (size_t)(argc - i);

    if (mk_simplefs_create(out, vol, seed, nrand, host, host_n) != 0) {
        die("mk_simplefs_create failed");
    }

    printf("Created %s (random_files=%u, host_files=%zu)\n", out, nrand, host_n);
    return 0;
}
