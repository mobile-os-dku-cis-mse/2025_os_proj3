
#include "simplefs.h"
#include <time.h>

static void write_inode_table(FILE *fp, const struct inode *inodes) {
    // inode table starts at block 1 and spans SIMPLEFS_NUM_INODE_BLOCKS blocks
    const uint8_t *raw = (const uint8_t*)inodes;
    size_t total = SIMPLEFS_NUM_INODES * sizeof(struct inode); // 7168 bytes
    size_t off = 0;
    for (uint32_t b = 0; b < SIMPLEFS_NUM_INODE_BLOCKS; b++) {
        uint8_t blk[SIMPLEFS_BLOCK_SIZE];
        zero_block(blk);
        size_t chunk = SIMPLEFS_BLOCK_SIZE;
        if (off + chunk > total) chunk = total - off;
        memcpy(blk, raw + off, chunk);
        write_block(fp, 1 + b, blk);
        off += chunk;
    }
}

static void write_bytes_to_blocks(FILE *fp, uint32_t start_block, const uint8_t *data, size_t len) {
    size_t off = 0;
    uint32_t blkno = start_block;
    while (off < len) {
        uint8_t blk[SIMPLEFS_BLOCK_SIZE];
        zero_block(blk);
        size_t chunk = SIMPLEFS_BLOCK_SIZE;
        if (off + chunk > len) chunk = len - off;
        memcpy(blk, data + off, chunk);
        write_block(fp, blkno, blk);
        blkno++;
        off += chunk;
    }
}

static uint32_t blocks_needed(size_t bytes) {
    return (uint32_t)((bytes + SIMPLEFS_BLOCK_SIZE - 1) / SIMPLEFS_BLOCK_SIZE);
}

static void fill_dentry(struct dentry *de, uint32_t inode_no, const char *name, uint32_t ftype) {
    memset(de, 0, sizeof(*de));
    de->inode = inode_no;
    de->dir_length = (uint32_t)sizeof(struct dentry);
    de->name_len = (uint32_t)strlen(name);
    de->file_type = ftype;
    if (de->name_len > 255) de->name_len = 255;
    memcpy(de->name, name, de->name_len);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <disk.img> [num_files]\n", argv[0]);
        return 1;
    }
    const char *img_path = argv[1];
    int num_files = 10;
    if (argc >= 3) {
        num_files = atoi(argv[2]);
        if (num_files < 1) num_files = 1;
        if (num_files > 200) num_files = 200; // keep it safe within inode count
    }

    FILE *fp = fopen(img_path, "wb+");
    if (!fp) die("fopen(disk image)");

    // Pre-size disk image to 4MB (4096 blocks * 1024)
    if (fseek(fp, (long)SIMPLEFS_NUM_BLOCKS * SIMPLEFS_BLOCK_SIZE - 1, SEEK_SET) != 0) die("fseek(size)");
    if (fputc(0, fp) == EOF) die("fputc(size)");
    fflush(fp);

    // --- Build superblock ---
    struct super_block sb;
    memset(&sb, 0, sizeof(sb));
    sb.partition_type = 0x1234ABCD;
    sb.block_size = SIMPLEFS_BLOCK_SIZE;
    sb.inode_size = sizeof(struct inode);
    sb.first_inode = 0;
    sb.num_inodes = SIMPLEFS_NUM_INODES;
    sb.num_inode_blocks = SIMPLEFS_NUM_INODE_BLOCKS;
    sb.num_blocks = SIMPLEFS_NUM_BLOCKS;
    sb.first_data_block = SIMPLEFS_FIRST_DATA_BLOCK;
    snprintf(sb.volume_name, sizeof(sb.volume_name), "SIMPLEFS_VOL");

    // --- inode table ---
    struct inode inodes[SIMPLEFS_NUM_INODES];
    memset(inodes, 0, sizeof(inodes));
    for (uint32_t i = 0; i < SIMPLEFS_NUM_INODES; i++) {
        inodes[i].indirect_block = -1;
        for (int k = 0; k < 6; k++) inodes[i].blocks[k] = 0;
    }

    // simple free block allocator (sequential)
    uint32_t next_free_block = SIMPLEFS_FIRST_DATA_BLOCK;

    // --- Root directory (inode 0) ---
    // Build directory entries: file_1..file_N
    size_t dir_bytes = (size_t)num_files * sizeof(struct dentry);
    uint32_t dir_blocks = blocks_needed(dir_bytes);
    if (dir_blocks > 6) {
        fprintf(stderr, "Too many files for 6 direct blocks in this simple builder. Reduce num_files.\n");
        return 1;
    }

    struct dentry *entries = (struct dentry*)calloc((size_t)num_files, sizeof(struct dentry));
    if (!entries) die("calloc(entries)");

    // Create file inodes (start at inode 1)
    srand((unsigned)time(NULL));
    for (int i = 0; i < num_files; i++) {
        uint32_t ino = (uint32_t)(1 + i);
        char fname[32];
        snprintf(fname, sizeof(fname), "file_%d", i + 1);

        // Fill directory entry
        fill_dentry(&entries[i], ino, fname, SIMPLEFS_FT_REG);

        // Create file content (text)
        char content[2048];
        int lines = 3 + (rand() % 6);
        int pos = 0;
        pos += snprintf(content + pos, sizeof(content) - (size_t)pos,
                        "=== %s ===\n", fname);
        for (int l = 0; l < lines && pos < (int)sizeof(content) - 64; l++) {
            pos += snprintf(content + pos, sizeof(content) - (size_t)pos,
                            "Line %d: simplefs sample text %u\n", l + 1, (unsigned)(rand() % 100000));
        }
        pos += snprintf(content + pos, sizeof(content) - (size_t)pos,
                        "EOF\n");

        size_t fsize = (size_t)pos;
        uint32_t fblocks = blocks_needed(fsize);
        if (fblocks > 6) fblocks = 6; // cap for this basic builder

        // allocate blocks
        uint32_t first = next_free_block;
        for (uint32_t b = 0; b < fblocks; b++) {
            inodes[ino].blocks[b] = (uint16_t)(next_free_block++);
        }

        // write file data
        write_bytes_to_blocks(fp, first, (const uint8_t*)content, fsize);

        // fill inode
        inodes[ino].mode = SIMPLEFS_MODE_REG;
        inodes[ino].locked = 0;
        inodes[ino].date = (uint32_t)time(NULL);
        inodes[ino].size = (uint32_t)fsize;
    }

    // root inode
    inodes[0].mode = SIMPLEFS_MODE_DIR;
    inodes[0].locked = 0;
    inodes[0].date = (uint32_t)time(NULL);
    inodes[0].size = (uint32_t)dir_bytes;

    uint32_t dir_first_block = next_free_block;
    for (uint32_t b = 0; b < dir_blocks; b++) {
        inodes[0].blocks[b] = (uint16_t)(next_free_block++);
    }
    write_bytes_to_blocks(fp, dir_first_block, (const uint8_t*)entries, dir_bytes);

    // counts
    sb.num_free_inodes = SIMPLEFS_NUM_INODES - (uint32_t)(1 + num_files);
    sb.num_free_blocks = SIMPLEFS_NUM_BLOCKS - next_free_block;

    // --- Write superblock + inode table ---
    write_block(fp, 0, &sb);
    write_inode_table(fp, inodes);

    fflush(fp);
    fclose(fp);
    free(entries);

    // Print summary to stdout (meets "mk_simplefs prints")
    printf("Created %s\n", img_path);
    printf("Volume: %s\n", sb.volume_name);
    printf("Block size: %u\n", sb.block_size);
    printf("First data block: %u\n", sb.first_data_block);
    printf("Inodes used: %u / %u\n", (uint32_t)(1 + num_files), sb.num_inodes);
    printf("Blocks used: %u / %u\n", next_free_block, sb.num_blocks);
    printf("Root directory contains %d files.\n", num_files);
    return 0;
}
