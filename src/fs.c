
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/fs.h"

// --- Disk image and superblock cache ---
static FILE *disk = NULL; // File pointer for disk image
static struct super_block sb_cache; // Cached superblock
static int sb_loaded = 0; // Flag: is superblock loaded?

/**
 * @brief Open the disk image for reading and writing.
 * @param path Path to the disk image file.
 * @return 0 on success, -1 on failure.
 */
int disk_open(const char *path) {
    disk = fopen(path, "rb+");
    return disk ? 0 : -1;
}

/**
 * @brief Close the disk image and reset internal state.
 */
void disk_close(void) {
    if (disk) {
        fclose(disk);
        disk = NULL;
        sb_loaded = 0;
    }
}

/**
 * @brief Read the superblock from disk into cache and optionally output.
 * @param out Optional pointer to store the superblock.
 * @return 0 on success, -1 on failure.
 */
int read_superblock(struct super_block *out) {
    if (!disk) return -1;
    fseek(disk, 0, SEEK_SET);
    if (fread(&sb_cache, sizeof(sb_cache), 1, disk) != 1) return -1;
    sb_loaded = 1;
    if (out) memcpy(out, &sb_cache, sizeof(sb_cache));
    return 0;
}

/**
 * @brief Read an inode by its 1-based index (1 = first inode).
 * @param inode_index Index of the inode (1-based).
 * @param out Pointer to store the inode data.
 * @return 0 on success, -1 on failure.
 */
int read_inode(unsigned int inode_index, struct inode *out) {
    if (!disk || !sb_loaded || inode_index == 0) return -1;
    unsigned int inode_size = sb_cache.inode_size ? sb_cache.inode_size : sizeof(struct inode);
    long offset = sizeof(struct super_block) + (inode_index - 1) * inode_size;
    fseek(disk, offset, SEEK_SET);
    void *buf = malloc(inode_size);
    if (!buf) return -1;
    if (fread(buf, inode_size, 1, disk) != 1) { free(buf); return -1; }
    memcpy(out, buf, sizeof(struct inode) < inode_size ? sizeof(struct inode) : inode_size);
    free(buf);
    return 0;
}

/**
 * @brief Read a data block by block number (handles relative/absolute addressing).
 * @param block_rel Relative or absolute block number.
 * @param buf Buffer to store the block data.
 * @return 0 on success, -1 on failure.
 */
int read_data_block(unsigned int block_rel, void *buf) {
    if (!disk || !sb_loaded) return -1;
    unsigned int abs_block = (block_rel >= sb_cache.first_data_block)
        ? block_rel : (sb_cache.first_data_block + block_rel);
    long offset = abs_block * sb_cache.block_size;
    fseek(disk, offset, SEEK_SET);
    if (fread(buf, sb_cache.block_size, 1, disk) != 1) return -1;
    return 0;
}

/**
 * @brief Get pointer to the cached superblock.
 * @return Pointer to the cached superblock, or NULL if not loaded.
 */
const struct super_block* get_superblock(void) {
    return sb_loaded ? &sb_cache : NULL;
}

/* ====== Added FS API: mount / open / read / close (small single-process file table) ====== */


// --- File table and directory cache ---
#define MAX_OPEN_FILES 16 // Max open files
struct open_file {
    int used;                // Is this slot used?
    unsigned int inode_index;// Inode number
    struct inode ino;        // Inode data
    unsigned int offset;     // Current file offset
    int flags;               // Open flags (unused except for O_RDONLY)
    unsigned int *indirect_entries; // Indirect block entries (if any)
    unsigned int indirect_count;    // Count of indirect entries
};
static struct open_file oftab[MAX_OPEN_FILES]; // File descriptor table

#define MAX_ROOT_CACHE_ENTRIES 4096
struct dir_cache_entry {
    char name[256];
    unsigned int inode;
    unsigned int file_type;
};
static struct dir_cache_entry root_cache[MAX_ROOT_CACHE_ENTRIES];
static unsigned int root_cache_count = 0;

// Clear the root directory cache
/**
 * @brief Clear the root directory cache.
 */
static void clear_root_cache(void) {
    for (unsigned int i = 0; i < root_cache_count; ++i) {
        root_cache[i].name[0] = '\0';
        root_cache[i].inode = 0;
        root_cache[i].file_type = 0;
    }
    root_cache_count = 0;
}


/**
 * @brief Mount the file system: open disk, load superblock, cache root directory, print files.
 * @param disk_path Path to the disk image file.
 * @return 0 on success, -1 on failure.
 */
int fs_mount(const char *disk_path) {
    if (disk_open(disk_path) != 0) { return -1; }
    if (read_superblock(NULL) != 0) { disk_close(); return -1; }
    memset(oftab, 0, sizeof(oftab)); // Clear open file table
    clear_root_cache();

    // Load root inode and cache directory entries
    const struct super_block *sb = get_superblock();
    if (!sb) return 0;
    unsigned int root_idx = sb->first_inode;
    struct inode root_ino;
    if (read_inode(root_idx, &root_ino) != 0) return 0;
    unsigned int bsize = sb->block_size;
    void *buf = malloc(bsize);
    if (!buf) return 0;
    unsigned int remaining = root_ino.size;
    for (unsigned bi = 0; bi < sb->num_blocks && remaining > 0; ++bi) {
        unsigned short block_no = root_ino.blocks[bi];
        if (block_no == 0) continue;
        if (read_data_block(block_no, buf) != 0) break;
        unsigned int off = 0;
        while (off + sizeof(struct dentry) <= bsize && remaining > 0) {
            struct dentry *de = (struct dentry *)((char*)buf + off);
            if (de->dir_length == 0) break;
            unsigned int namelen = de->name_len > 255 ? 255 : de->name_len;
            if (root_cache_count < MAX_ROOT_CACHE_ENTRIES) {
                memset(root_cache[root_cache_count].name, 0, sizeof(root_cache[root_cache_count].name));
                memcpy(root_cache[root_cache_count].name, de->name, namelen);
                root_cache[root_cache_count].inode = de->inode;
                root_cache[root_cache_count].file_type = de->file_type;
                root_cache_count++;
            }
            off += de->dir_length;
            remaining = (remaining > de->dir_length) ? (remaining - de->dir_length) : 0;
        }
    }
    free(buf);

    // Print root directory listing
    printf("Mounted volume: %.24s | root entries: %u\n", sb->volume_name, root_cache_count);
    for (unsigned int i = 0; i < root_cache_count; ++i) {
        printf("%3u: inode=%u type=%u name=%s\n", i, root_cache[i].inode, root_cache[i].file_type, root_cache[i].name);
    }
    return 0;
}


/**
 * @brief Unmount the file system: free resources and close disk.
 * @return 0 on success.
 */
int fs_unmount(void) {
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (oftab[i].used && oftab[i].indirect_entries) {
            free(oftab[i].indirect_entries);
            oftab[i].indirect_entries = NULL;
            oftab[i].indirect_count = 0;
        }
        oftab[i].used = 0;
    }
    clear_root_cache();
    disk_close();
    return 0;
}


/**
 * @brief Open a file by name (in root directory).
 * @param path File name to open.
 * @param flags Open flags (unused except for O_RDONLY).
 * @return File descriptor (>=0) on success, -1 on error.
 */
int fs_open(const char *path, int flags) {
    if (!path || !sb_loaded) return -1;
    // Remove leading '/' if present
    const char *name = (path[0] == '/') ? path + 1 : path;

    // Find a free file descriptor slot
    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!oftab[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;

    // Try to find the file in the root directory cache
    unsigned int found_inode = 0;
    for (unsigned int i = 0; i < root_cache_count; ++i) {
        if (strcmp(root_cache[i].name, name) == 0) {
            found_inode = root_cache[i].inode;
            break;
        }
    }

    // If not found in cache, scan root directory on disk
    if (found_inode == 0) {
        unsigned int root_idx = sb_cache.first_inode;
        struct inode root_ino;
        if (read_inode(root_idx, &root_ino) != 0) return -1;
        unsigned int bsize = sb_cache.block_size;
        void *buf = malloc(bsize);
        if (!buf) return -1;
        unsigned int dir_remaining = root_ino.size;
        for (unsigned bi = 0; bi < sb_cache.num_blocks && dir_remaining > 0 && !found_inode; ++bi) {
            unsigned short block_no = root_ino.blocks[bi];
            if (block_no == 0) continue;
            if (read_data_block(block_no, buf) != 0) break;
            unsigned int off = 0;
            while (off + sizeof(struct dentry) <= bsize && dir_remaining > 0) {
                struct dentry *de = (struct dentry *)((char*)buf + off);
                if (de->dir_length == 0) { off = bsize; break; }
                unsigned int namelen = de->name_len > 255 ? 255 : de->name_len;
                char nm[256] = {0};
                memcpy(nm, de->name, namelen);
                if (strcmp(nm, name) == 0) {
                    found_inode = de->inode;
                    break;
                }
                off += de->dir_length;
                dir_remaining = (dir_remaining > de->dir_length) ? (dir_remaining - de->dir_length) : 0;
            }
        }
        free(buf);
        if (!found_inode) return -1;
    }

    // Load inode into open file slot
    if (read_inode(found_inode, &oftab[fd].ino) != 0) return -1;
    oftab[fd].inode_index = found_inode;
    oftab[fd].offset = 0;
    oftab[fd].flags = flags;
    oftab[fd].used = 1;
    oftab[fd].indirect_entries = NULL;
    oftab[fd].indirect_count = 0;
    return fd;
}


// Helper: load indirect block entries for a file if needed
/**
 * @brief Load indirect block entries for a file if needed.
 * @param of Pointer to open_file structure.
 * @return 0 on success, -1 on failure.
 */
static int load_indirect_if_needed(struct open_file *of) {
    if (!of->ino.indirect_block || (int)of->ino.indirect_block == -1) return -1;
    if (of->indirect_entries) return 0; // Already loaded
    unsigned int bsize = sb_cache.block_size;
    void *buf = malloc(bsize);
    if (!buf) return -1;
    if (read_data_block((unsigned int)of->ino.indirect_block, buf) != 0) { free(buf); return -1; }
    unsigned int max_entries = bsize / sizeof(unsigned int);
    unsigned int *arr = malloc(max_entries * sizeof(unsigned int));
    if (!arr) { free(buf); return -1; }
    memcpy(arr, buf, max_entries * sizeof(unsigned int));
    of->indirect_entries = arr;
    of->indirect_count = max_entries;
    free(buf);
    return 0;
}


/**
 * @brief Read from an open file descriptor.
 * @param fd File descriptor.
 * @param outbuf Buffer to store read data.
 * @param count Number of bytes to read.
 * @return Number of bytes read, 0 on EOF, -1 on error.
 */
int fs_read(int fd, void *outbuf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -1;
    struct open_file *of = &oftab[fd];
    if (!of->used) return -1;
    if (!outbuf || count == 0) return 0;

    unsigned int file_size = of->ino.size;
    if (of->offset >= file_size) return 0; // EOF

    unsigned int bsize = sb_cache.block_size;
    unsigned int toread = (unsigned int)count;
    unsigned int canread = (of->offset + toread <= file_size) ? toread : (file_size - of->offset);
    unsigned int total_read = 0;
    void *blockbuf = malloc(bsize);
    if (!blockbuf) return -1;

    while (canread > 0) {
        unsigned int lb = of->offset / bsize; // Logical block index
        unsigned int off_in_block = of->offset % bsize;
        unsigned int phys_block = 0;
        if (lb < of->ino.size / bsize + ((of->ino.size % bsize) ? 1 : 0)) {
            phys_block = of->ino.blocks[lb]; // Direct block
        } else {
            // Indirect block
            if (load_indirect_if_needed(of) != 0) break;
            unsigned int idx = lb - 6;
            if (idx >= of->indirect_count) break;
            phys_block = of->indirect_entries[idx];
        }
        if (phys_block == 0) {
            // Sparse/empty block: fill with zeros
            unsigned int chunk = bsize - off_in_block;
            if (chunk > canread) chunk = canread;
            memset((char*)outbuf + total_read, 0, chunk);
            of->offset += chunk;
            total_read += chunk;
            canread -= chunk;
            continue;
        }
        if (read_data_block(phys_block, blockbuf) != 0) break;
        unsigned int avail = bsize - off_in_block;
        unsigned int chunk = (avail < canread) ? avail : canread;
        memcpy((char*)outbuf + total_read, (char*)blockbuf + off_in_block, chunk);
        of->offset += chunk;
        total_read += chunk;
        canread -= chunk;
    }

    free(blockbuf);
    return (int)total_read;
}


/**
 * @brief Close an open file descriptor.
 * @param fd File descriptor to close.
 * @return 0 on success, -1 on error.
 */
int fs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -1;
    if (!oftab[fd].used) return -1;
    if (oftab[fd].indirect_entries) free(oftab[fd].indirect_entries);
    oftab[fd].indirect_entries = NULL;
    oftab[fd].indirect_count = 0;
    oftab[fd].used = 0;
    return 0;
}


// === BEGIN: Minimal file/directory creation, writing, and deletion support ===

// Helper: find a free inode (returns 1-based index or 0 if none)
/**
 * @brief Find a free inode (returns 1-based index or 0 if none).
 * @return 1-based index of free inode, or 0 if none found.
 */
static unsigned int find_free_inode(void) {
    struct inode ino;
    for (unsigned int i = 2; i <= sb_cache.num_inodes; ++i) { // skip root inode
        if (read_inode(i, &ino) == 0 && ino.mode == 0)
            return i;
    }
    return 0;
}

// Helper: find a free data block (returns absolute block number or 0 if none)
/**
 * @brief Find a free data block (returns absolute block number or 0 if none).
 * @return Absolute block number of free block, or 0 if none found.
 */
static unsigned int find_free_block(void) {
    unsigned char *block = malloc(sb_cache.block_size);
    if (!block) return 0;
    for (unsigned int b = sb_cache.first_data_block + 1; b < sb_cache.first_data_block + sb_cache.num_blocks; ++b) {
        fseek(disk, b * sb_cache.block_size, SEEK_SET);
        if (fread(block, sb_cache.block_size, 1, disk) != 1) continue;
        int empty = 1;
        for (unsigned int i = 0; i < sb_cache.block_size; ++i) {
            if (block[i] != 0) { empty = 0; break; }
        }
        if (empty) { free(block); return b; }
    }
    free(block);
    return 0;
}

// Helper: write inode to disk (1-based index)
/**
 * @brief Write an inode to disk (1-based index).
 * @param idx 1-based index of the inode.
 * @param ino Pointer to inode data to write.
 * @return 0 on success, -1 on failure.
 */
static int write_inode(unsigned int idx, const struct inode *ino) {
    unsigned int inode_size = sb_cache.inode_size ? sb_cache.inode_size : sizeof(struct inode);
    long offset = sizeof(struct super_block) + (idx - 1) * inode_size;
    fseek(disk, offset, SEEK_SET);
    return fwrite(ino, inode_size, 1, disk) == 1 ? 0 : -1;
}

// Helper: write data block to disk (absolute block number)
/**
 * @brief Write a data block to disk (absolute block number).
 * @param block Absolute block number.
 * @param buf Buffer containing data to write.
 * @return 0 on success, -1 on failure.
 */
static int write_data_block(unsigned int block, const void *buf) {
    fseek(disk, block * sb_cache.block_size, SEEK_SET);
    return fwrite(buf, sb_cache.block_size, 1, disk) == 1 ? 0 : -1;
}

// Helper: write root directory block (block 0 in root inode)
/**
 * @brief Write the root directory block (block 0 in root inode).
 * @param buf Buffer containing directory data.
 * @return 0 on success, -1 on failure.
 */

/**
 * @brief Create a new file in the root directory.
 * @param path Name of the file to create.
 * @param flags Creation flags (unused).
 * @return 0 on success, -1 on failure.
 */
int fs_createfiel(const char *path, int flags) {
    (void)flags; // Mark unused
    if (!disk) {  return -1; }
    if (!sb_loaded) { return -1; }
    if (!path) { return -1; }

    // Parse path: look for last '/'
    const char *slash = strrchr(path, '/');
    const char *fname = path;
    char dirname[256] = {0};
    unsigned int dir_inode_idx = sb_cache.first_inode; // default: root
    if (slash) {
        size_t dlen = slash - path;
        if (dlen > 255) dlen = 255;
        memcpy(dirname, path, dlen);
        fname = slash + 1;
        // Find directory inode in root
        struct inode root_ino;
        if (read_inode(sb_cache.first_inode, &root_ino) != 0) return -1;
        unsigned char *dirbuf = malloc(sb_cache.block_size);
        if (!dirbuf) return -1;
        if (read_data_block(root_ino.blocks[0], dirbuf) != 0) { free(dirbuf); return -1; }
        unsigned int off = 0;
        struct dentry *de = NULL;
        for (; off + sizeof(struct dentry) <= sb_cache.block_size; off += sizeof(struct dentry)) {
            de = (struct dentry *)(dirbuf + off);
            if (de->dir_length == 0) break;
            if (de->file_type == DENTRY_TYPE_DIR_FILE && strncmp((char*)de->name, dirname, de->name_len) == 0) {
                dir_inode_idx = de->inode;
                break;
            }
        }
        free(dirbuf);
        if (dir_inode_idx == sb_cache.first_inode) return -1; // dir not found
    }

    // 1. Find free inode and data block
    unsigned int free_ino = find_free_inode();
    unsigned int free_blk = find_free_block();
    if (!free_ino || !free_blk) {return -1; }

    // 2. Read target dir inode
    struct inode dir_ino;
    if (read_inode(dir_inode_idx, &dir_ino) != 0) { return -1; }
    int found_slot = 0;
    int block_idx = -1;
    unsigned int slot_off = 0;
    struct dentry *de = NULL;
    unsigned char *dirbuf = malloc(sb_cache.block_size);
    if (!dirbuf) { return -1; }
    if (dir_inode_idx == sb_cache.first_inode) {
        // Scan all root dir blocks
        for (int bi = 0; bi < 6; ++bi) {
            if (dir_ino.blocks[bi] == 0) continue;
            if (read_data_block(dir_ino.blocks[bi], dirbuf) != 0) continue;
            unsigned int off = 0;
            while (off + sizeof(struct dentry) <= sb_cache.block_size) {
                de = (struct dentry *)(dirbuf + off);
                if (de->dir_length == 0) {
                    found_slot = 1;
                    block_idx = bi;
                    slot_off = off;
                    break;
                }
                off += de->dir_length;
            }
            if (found_slot) break;
        }
        // If no slot found, try to allocate a new block
        if (!found_slot) {
            for (unsigned bi = 0; bi < sb_cache.num_blocks; ++bi) {
                if (dir_ino.blocks[bi] == 0) {
                    unsigned int new_blk = find_free_block();
                    if (!new_blk) { free(dirbuf); return -1; }
                    dir_ino.blocks[bi] = new_blk;
                    if (write_inode(dir_inode_idx, &dir_ino) != 0) { free(dirbuf); return -1; }
                    memset(dirbuf, 0, sb_cache.block_size);
                    found_slot = 1;
                    block_idx = bi;
                    slot_off = 0;
                    break;
                }
            }
        }
        if (!found_slot) { free(dirbuf); return -1; }
        de = (struct dentry *)(dirbuf + slot_off);
        memset(de, 0, sizeof(struct dentry));
        de->inode = free_ino;
        de->dir_length = sizeof(struct dentry);
        de->name_len = strlen(fname) > 255 ? 255 : strlen(fname);
        de->file_type = DENTRY_TYPE_REG_FILE;
        memcpy(de->name, fname, de->name_len);
        if (write_data_block(dir_ino.blocks[block_idx], dirbuf) != 0) { free(dirbuf); return -1; }
    } else {
        // Subdirectory: only one block supported
        if (read_data_block(dir_ino.blocks[0], dirbuf) != 0) { free(dirbuf); return -1; }
        unsigned int off = 0;
        struct dentry *de = NULL;
        int found_slot = 0;
        while (off + sizeof(struct dentry) <= sb_cache.block_size) {
            de = (struct dentry *)(dirbuf + off);
            if (de->dir_length == 0) { found_slot = 1; break; }
            off += de->dir_length;
        }
        if (!found_slot && off + sizeof(struct dentry) <= sb_cache.block_size) {
            found_slot = 1;
        }
        if (!found_slot || off + sizeof(struct dentry) > sb_cache.block_size) {
            free(dirbuf); return -1;
        }
        memset(de, 0, sizeof(struct dentry));
        de->inode = free_ino;
        de->dir_length = sizeof(struct dentry);
        de->name_len = strlen(fname) > 255 ? 255 : strlen(fname);
        de->file_type = DENTRY_TYPE_REG_FILE;
        memcpy(de->name, fname, de->name_len);
        if (write_data_block(dir_ino.blocks[0], dirbuf) != 0) { free(dirbuf); return -1; }
    }
    free(dirbuf);

    // 6. Fill and write inode
    struct inode newino = {0};
    newino.mode = INODE_MODE_REG_FILE | INODE_MODE_AC_ALL;
    newino.locked = 0;
    newino.date = (unsigned)time(NULL);
    newino.size = 0;
    newino.indirect_block = -1;
    newino.blocks[0] = free_blk;
    if (write_inode(free_ino, &newino) != 0) { return -1; }
    return 0;
}

/**
 * @brief Write to an open file (single block, overwrite only).
 * @param fd File descriptor.
 * @param buf Buffer containing data to write.
 * @param count Number of bytes to write.
 * @return Number of bytes written, or -1 on error.
 */
int fs_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -1;
    struct open_file *of = &oftab[fd];
    if (!of->used) return -1;
    if (!buf || count == 0) return 0;
    unsigned int bsize = sb_cache.block_size;
    if (count > bsize) count = bsize;
    // Write to disk
    if (write_data_block(of->ino.blocks[0], buf) != 0) return -1;
    of->ino.size = count;
    // Update inode on disk
    if (write_inode(of->inode_index, &of->ino) != 0) return -1;
    return (int)count;
}

/**
 * @brief Remove a file from the root directory.
 * @param path Name of the file to delete.
 * @return 0 on success, -1 on failure.
 */
int fs_delete(const char *path) {
    if (!disk || !sb_loaded || !path) return -1;
    // Parse path: look for last '/'
    const char *slash = strrchr(path, '/');
    const char *fname = path;
    char dirname[256] = {0};
    unsigned int dir_inode_idx = sb_cache.first_inode; // default: root
    if (slash) {
        size_t dlen = slash - path;
        if (dlen > 255) dlen = 255;
        memcpy(dirname, path, dlen);
        fname = slash + 1;
        // Find directory inode in root
        struct inode root_ino;
        if (read_inode(sb_cache.first_inode, &root_ino) != 0) return -1;
        unsigned char *dirbuf = malloc(sb_cache.block_size);
        if (!dirbuf) return -1;
        if (read_data_block(root_ino.blocks[0], dirbuf) != 0) { free(dirbuf); return -1; }
        unsigned int off = 0;
        struct dentry *de = NULL;
        for (; off + sizeof(struct dentry) <= sb_cache.block_size; off += sizeof(struct dentry)) {
            de = (struct dentry *)(dirbuf + off);
            if (de->dir_length == 0) break;
            if (de->file_type == DENTRY_TYPE_DIR_FILE && strncmp((char*)de->name, dirname, de->name_len) == 0) {
                dir_inode_idx = de->inode;
                break;
            }
        }
        free(dirbuf);
        if (dir_inode_idx == sb_cache.first_inode) return -1; // dir not found
    }
    // 1. Read target dir inode
    struct inode dir_ino;
    if (read_inode(dir_inode_idx, &dir_ino) != 0) return -1;
    unsigned char *dirbuf = malloc(sb_cache.block_size);
    if (!dirbuf) return -1;
    if (dir_inode_idx == sb_cache.first_inode) {
        // Scan all root dir blocks
        for (unsigned bi = 0; bi < sb_cache.num_blocks; ++bi) {
            if (dir_ino.blocks[bi] == 0) continue;
            if (read_data_block(dir_ino.blocks[bi], dirbuf) != 0) continue;
            unsigned int off = 0;
            struct dentry *de = NULL;
            for (; off + sizeof(struct dentry) <= sb_cache.block_size; off += sizeof(struct dentry)) {
                de = (struct dentry *)(dirbuf + off);
                if (de->dir_length == 0) break;
                if (strncmp((char*)de->name, fname, de->name_len) == 0) {
                    memset(de, 0, sizeof(struct dentry));
                    int res = write_data_block(dir_ino.blocks[bi], dirbuf);
                    free(dirbuf);
                    return res;
                }
            }
        }
        free(dirbuf);
        return -1;
    } else {
        // Subdirectory: only one block supported
        if (read_data_block(dir_ino.blocks[0], dirbuf) != 0) { free(dirbuf); return -1; }
        unsigned int off = 0;
        struct dentry *de = NULL;
        for (; off + sizeof(struct dentry) <= sb_cache.block_size; off += sizeof(struct dentry)) {
            de = (struct dentry *)(dirbuf + off);
            if (de->dir_length == 0) break;
            if (strncmp((char*)de->name, fname, de->name_len) == 0) {
                memset(de, 0, sizeof(struct dentry));
                int res = write_data_block(dir_ino.blocks[0], dirbuf);
                free(dirbuf);
                return res;
            }
        }
        free(dirbuf);
        return -1;
    }
}

/**
 * @brief Create a new directory in the root (not recursive).
 * @param path Name of the directory to create.
 * @return 0 on success, -1 on failure.
 */
int fs_makedir(const char *path) {
    if (!disk) { return -1; }
    if (!sb_loaded) { return -1; }
    if (!path) { return -1; }
    // 1. Find free inode and data block
    unsigned int free_ino = find_free_inode();
    unsigned int free_blk = find_free_block();
    if (!free_ino || !free_blk) { return -1; }
    // 2. Read root inode
    struct inode root_ino;
    if (read_inode(sb_cache.first_inode, &root_ino) != 0) { return -1; }
    int found_slot = 0;
    int block_idx = -1;
    unsigned int slot_off = 0;
    struct dentry *de = NULL;
    unsigned char *dirbuf = malloc(sb_cache.block_size);
    if (!dirbuf) { return -1; }
    // Scan all root dir blocks for free slot
    for (unsigned bi = 0; bi < sb_cache.num_blocks; ++bi) {
        if (root_ino.blocks[bi] == 0) continue;
        if (read_data_block(root_ino.blocks[bi], dirbuf) != 0) continue;
        unsigned int off = 0;
        while (off + sizeof(struct dentry) <= sb_cache.block_size) {
            de = (struct dentry *)(dirbuf + off);
            if (de->dir_length == 0) {
                found_slot = 1;
                block_idx = bi;
                slot_off = off;
                break;
            }
            off += de->dir_length;
        }
        if (found_slot) break;
    }
    // If no slot found, try to allocate a new block
    if (!found_slot) {
        for (unsigned bi = 0; bi < sb_cache.num_blocks; ++bi) {
            if (root_ino.blocks[bi] == 0) {
                unsigned int new_blk = find_free_block();
                if (!new_blk) { free(dirbuf); return -1; }
                root_ino.blocks[bi] = new_blk;
                if (write_inode(sb_cache.first_inode, &root_ino) != 0) { free(dirbuf); return -1; }
                memset(dirbuf, 0, sb_cache.block_size);
                found_slot = 1;
                block_idx = bi;
                slot_off = 0;
                break;
            }
        }
    }
    if (!found_slot) { free(dirbuf); return -1; }
    // Fill dentry
    de = (struct dentry *)(dirbuf + slot_off);
    memset(de, 0, sizeof(struct dentry));
    de->inode = free_ino;
    de->dir_length = sizeof(struct dentry);
    de->name_len = strlen(path) > 255 ? 255 : strlen(path);
    de->file_type = DENTRY_TYPE_DIR_FILE;
    memcpy(de->name, path, de->name_len);
    // Write updated dir block
    if (write_data_block(root_ino.blocks[block_idx], dirbuf) != 0) { free(dirbuf); return -1; }
    free(dirbuf);
    // 6. Fill and write inode
    struct inode newino = {0};
    newino.mode = INODE_MODE_DIR_FILE | INODE_MODE_AC_ALL;
    newino.locked = 0;
    newino.date = (unsigned)time(NULL);
    newino.size = 0;
    newino.indirect_block = -1;
    newino.blocks[0] = free_blk;
    if (write_inode(free_ino, &newino) != 0) { return -1; }
    // Zero the new directory's data block
    unsigned char zerobuf[1024] = {0};
    if (write_data_block(free_blk, zerobuf) != 0) { return -1; }
    return 0;
}

/**
 * @brief Remove a directory from the root (not recursive).
 * @param path Name of the directory to remove.
 * @return 0 on success, -1 on failure.
 */
int fs_removedir(const char *path) {
    if (!disk || !sb_loaded || !path) {
        return -1;
    }
    struct inode root_ino;
    if (read_inode(sb_cache.first_inode, &root_ino) != 0) {
        return -1;
    }
    unsigned char *dirbuf = malloc(sb_cache.block_size);
    if (!dirbuf) {
        return -1;
    }
    int found = 0;
    int block_idx = -1;
    unsigned int slot_off = 0;
    struct dentry *de = NULL;
    unsigned int dir_inode_idx = 0;
    // Scan all root dir blocks for dentry
    for (unsigned bi = 0; bi < sb_cache.num_blocks; ++bi) {
        if (root_ino.blocks[bi] == 0) continue;
        if (read_data_block(root_ino.blocks[bi], dirbuf) != 0) continue;
        unsigned int off = 0;
        while (off + sizeof(struct dentry) <= sb_cache.block_size) {
            de = (struct dentry *)(dirbuf + off);
            if (de->dir_length == 0) break;
            if (strncmp((char*)de->name, path, de->name_len) == 0 && de->file_type == DENTRY_TYPE_DIR_FILE) {
                dir_inode_idx = de->inode;
                found = 1;
                block_idx = bi;
                slot_off = off;
                break;
            }
            off += de->dir_length;
        }
        if (found) break;
    }
    if (!dir_inode_idx) {
        free(dirbuf);
        return -1;
    }
    struct inode dir_ino;
    if (read_inode(dir_inode_idx, &dir_ino) != 0) {
        free(dirbuf);
        return -1;
    }
    unsigned char *subdirbuf = malloc(sb_cache.block_size);
    if (!subdirbuf) {
        free(dirbuf);
        return -1;
    }
    if (read_data_block(dir_ino.blocks[0], subdirbuf) != 0) {
        free(dirbuf);
        free(subdirbuf);
        return -1;
    }
    // 4. Check if directory is empty (all entries dir_length==0)
    int empty = 1;
    for (unsigned int suboff = 0; suboff + sizeof(struct dentry) <= sb_cache.block_size; suboff += sizeof(struct dentry)) {
        struct dentry *subde = (struct dentry *)(subdirbuf + suboff);
        if (subde->dir_length != 0) {
            empty = 0;
            break;
        }
    }
    free(subdirbuf);
    if (!empty) {
        free(dirbuf);
        return -1;
    }
    // 5. Directory is empty, clear dentry in root dir
    de = (struct dentry *)(dirbuf + slot_off);
    memset(de, 0, sizeof(struct dentry));
    int res = write_data_block(root_ino.blocks[block_idx], dirbuf);
    free(dirbuf);
    // 6. Optionally, clear the inode (not strictly required for test)
    struct inode zero_ino = {0};
    int inode_res = write_inode(dir_inode_idx, &zero_ino);
    (void)inode_res;
    return res;
}
// === END: Minimal file/directory creation, writing, and deletion support ===