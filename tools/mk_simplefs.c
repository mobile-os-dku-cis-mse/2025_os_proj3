
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/fs.h"

// Helper: write zeros to a file
void write_zeros(FILE *f, size_t n) {
    char z[4096] = {0};
    while (n > 0) {
        size_t chunk = n > sizeof(z) ? sizeof(z) : n;
        fwrite(z, 1, chunk, f);
        n -= chunk;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s disk.img file1 [file2 ...]\n", argv[0]);
        return 1;
    }
    const char *imgname = argv[1];
    int nfiles = argc - 2;

    FILE *img = fopen(imgname, "wb");
    if (!img) { perror("fopen"); return 1; }

    // --- Superblock ---
    struct super_block sb = {0};
    sb.partition_type = SIMPLE_PARTITION;
    sb.block_size = BLOCK_SIZE;
    sb.inode_size = sizeof(struct inode);
    sb.first_inode = 1;
    sb.num_inodes = 224;
    sb.num_inode_blocks = 7;
    sb.num_free_inodes = 224 - (nfiles + 1); // root + files used
    sb.num_blocks = 4096 - 8; // 4096 blocks total, minus 8 for superblock+inodes
    sb.num_free_blocks = 4088 - (nfiles + 1); // crude: root dir + files
    sb.first_data_block = 8; // after superblock + inode table
    strncpy(sb.volume_name, "Simple_partition_volume", sizeof(sb.volume_name));

    // --- Inode table ---
    struct inode inodes[224] = {0};

    // Root directory inode (inode 1)
    inodes[0].mode = INODE_MODE_DIR_FILE | INODE_MODE_AC_ALL;
    inodes[0].locked = 0;
    inodes[0].date = (unsigned)time(NULL);
    // The root inode size should be a multiple of block size if the directory fits in one block
    inodes[0].size = nfiles * sizeof(struct dentry);
    if (inodes[0].size < BLOCK_SIZE) inodes[0].size = BLOCK_SIZE;
    inodes[0].indirect_block = -1;
    // Root directory will be at absolute block 8
    inodes[0].blocks[0] = 8;

    // File inodes (start at inode 2)
    for (int i = 0; i < nfiles; ++i) {
        inodes[i+1].mode = INODE_MODE_REG_FILE | INODE_MODE_AC_ALL;
        inodes[i+1].locked = 0;
        inodes[i+1].date = (unsigned)time(NULL);
        // We'll fill size and blocks below
        inodes[i+1].indirect_block = -1;
    }

    // --- Data blocks ---
    // Block 0: root directory entries
    struct dentry *dentries = calloc(nfiles, sizeof(struct dentry));
    // Data block layout:
    // block 8: root directory
    // block 9+: file data
    unsigned short root_dir_block = 8; // absolute block number for root dir
    unsigned short next_block = 9; // absolute block numbers for file data blocks

    char *filebuf = malloc(BLOCK_SIZE);
    for (int i = 0; i < nfiles; ++i) {
        const char *hostfile = argv[2 + i];
        // Use only the base name for the FS file name
        const char *slash = strrchr(hostfile, '/');
        const char *fsname = slash ? slash + 1 : hostfile;
        size_t namelen = strlen(fsname);
        if (namelen > 255) namelen = 255;

        // Fill directory entry
        dentries[i].inode = i + 2; // inode number (1-based, root is 1)
        dentries[i].dir_length = sizeof(struct dentry);
        dentries[i].name_len = namelen;
        dentries[i].file_type = DENTRY_TYPE_REG_FILE;
        memset(dentries[i].name, 0, 256);
        memcpy(dentries[i].name, fsname, namelen);

        // Fill inode and file data
        FILE *f = fopen(hostfile, "rb");
        size_t sz = 0;
        if (f) {
            sz = fread(filebuf, 1, BLOCK_SIZE, f);
            fclose(f);
        } else {
            snprintf(filebuf, BLOCK_SIZE, "This is file %s.\n", fsname);
            sz = strlen(filebuf);
        }
        inodes[i+1].mode = INODE_MODE_REG_FILE | INODE_MODE_AC_ALL;
        inodes[i+1].locked = 0;
        inodes[i+1].date = (unsigned)time(NULL);
        inodes[i+1].size = sz;
        inodes[i+1].indirect_block = -1;
        inodes[i+1].blocks[0] = next_block; // absolute block number
        ++next_block;
    }
    inodes[0].blocks[0] = root_dir_block; // root dir is at absolute block 8

    fwrite(&sb, sizeof(sb), 1, img);
    fwrite(inodes, sizeof(inodes), 1, img);

    // Pad to first data block (block 8)
    long cur = sizeof(sb) + sizeof(inodes);
    long want = sb.first_data_block * sb.block_size;
    if (cur < want) write_zeros(img, want - cur);

    // Write root directory block (block 8)
    char datablock[BLOCK_SIZE] = {0};
    memset(datablock, 0, BLOCK_SIZE);
    // Copy only the used dentries, leave the rest zeroed (dir_length==0)
    if (nfiles > 0) {
        memcpy(datablock, dentries, nfiles * sizeof(struct dentry));
    }
    fwrite(datablock, BLOCK_SIZE, 1, img);

    // Write file data blocks (block 9+)
    for (int i = 0; i < nfiles; ++i) {
        memset(datablock, 0, 1024);
        const char *fname = argv[2 + i];
        FILE *f = fopen(fname, "rb");
        if (f) {
            size_t dummy = fread(datablock, 1, 1024, f);
            (void)dummy;
            fclose(f);
        } else {
            snprintf(datablock, 1024, "This is file %s.\n", fname);
        }
        fwrite(datablock, 1024, 1, img);
    }

    // Pad the rest of the image to 4MB
    long written = sizeof(sb) + sizeof(inodes) + (1 + nfiles) * BLOCK_SIZE;
    long total = 4 * 1024 * 1024;
    if (written < total) write_zeros(img, total - written);

    fclose(img);
    free(dentries);
    free(filebuf);
    printf("Created disk image '%s' with %d files.\n", imgname, nfiles);
    return 0;
}