#ifndef FS_H
#define FS_H

#include <stdint.h> // For uint32_t and uint16_t

/**
 * Superblock structure
 */
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
    char volume_name[24];
    unsigned char padding[960]; // 1024-64
} __attribute__((__packed__));

/**
 * 32-byte I-node structure
 */
struct inode {
    uint32_t mode;           // reg. file, directory, dev., permissions
    uint32_t locked;         // opened for write
    uint32_t date;
    uint32_t size;
    int32_t indirect_block; // N.B. -1 for NULL, using int32_t for consistency
    uint16_t blocks[0x6];
} __attribute__((__packed__));

struct blocks {
    unsigned char d[1024];
} __attribute__((__packed__));

/* physical partition structure */
struct partition {
    struct super_block s;
    struct inode inode_table[224];
    struct blocks data_blocks[4088]; // 4096-8
} __attribute__((__packed__));

#endif // FS_H