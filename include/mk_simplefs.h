#ifndef MK_SIMPLEFS_H
#define MK_SIMPLEFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/*
 * mk_simplefs is a disk image generator for the SimpleFS project.
 *
 * Contract:
 *  - Uses the SAME on-disk structs as the runtime filesystem (simplefs.h).
 *  - Produces disk.img with layout:
 *      [superblock (1 block)] [inode table blocks] [data blocks]
 *
 * Error model:
 *  - return 0 on success
 *  - return negative errno-style codes on failure (e.g., -EINVAL, -EIO)
 */

#include "simplefs.h"   // shares struct super_block, struct inode, struct dentry

/* ---------- Generator options ---------- */

typedef struct {
    const char* out_path;          // e.g., "disk.img"
    const char* volume_name;       // <= 24 bytes, will be truncated/padded
    uint32_t    block_size;        // usually 1024 (must match runtime)
    uint32_t    num_inodes;        // usually 224
    uint32_t    num_blocks;        // total blocks (e.g., 4096)
    uint32_t    num_inode_blocks;  // derived from num_inodes * inode_size / block_size
    uint32_t    first_inode;       // usually 0
    uint32_t    first_data_block;  // usually 1 + num_inode_blocks

    /* file population */
    uint32_t    file_count;        // number of regular files to create in root
    uint32_t    min_file_size;     // bytes
    uint32_t    max_file_size;     // bytes

    /* determinism */
    uint32_t    seed;              // RNG seed (0 => auto time-based)
    int         deterministic;     // if 1, use seed exactly (reproducible images)

    /* output verbosity */
    int         verbose;           // print super/inodes/files summary
} mkfs_opts_t;

/* ---------- Public API ---------- */

/* Create a new disk image based on opts. */
int mk_simplefs_create(const mkfs_opts_t* opts);

/* Print a human-readable summary of an existing disk image (for grading/debug). */
int mk_simplefs_dump(const char* img_path);

/* ---------- Helpers (optional but useful for tests) ---------- */

/* Compute how many inode blocks are required for given params. */
uint32_t mkfs_calc_inode_blocks(uint32_t num_inodes, uint32_t inode_size, uint32_t block_size);

/* Compute first_data_block given inode blocks. */
static inline uint32_t mkfs_calc_first_data_block(uint32_t num_inode_blocks) {
    return 1u + num_inode_blocks; // 1 superblock block + inode blocks
}

#ifdef __cplusplus
}
#endif

#endif // MK_SIMPLEFS_H
