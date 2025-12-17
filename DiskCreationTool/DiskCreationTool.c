/*
Usage

gcc -o DiskCreationTool DiskCreationTool.c
./DiskCreationTool

Description: This tool creates a simple disk image file with a fixed structure.
It initializes a partition with a superblock, root directory, and several files
with random content. The disk image is saved as "disk.img".
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../fs.h"
#define DISK_FILE "disk.img"
#define NUM_RANDOM_FILES 10

const char *LOREM_IPSUM = 
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor "
    "incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud "
    "exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.";

struct partition *part;
unsigned char inode_bitmap[224];
unsigned char block_bitmap[4088];

//bitmaps helpers
void set_bit(unsigned char *bmap, int index) {
    bmap[index / 8] |= (1 << (index % 8));
}

int is_set(unsigned char *bmap, int index) {
    return (bmap[index / 8] & (1 << (index % 8)));
}

// allocate inode and mark in bitmap
int alloc_inode() {
    for (int i = 1; i < (int)part->s.num_inodes; i++) {
        if (!is_set(inode_bitmap, i)) {
            set_bit(inode_bitmap, i);
            part->s.num_free_inodes--;
            memset(&part->inode_table[i], 0, sizeof(struct inode));
            part->inode_table[i].indirect_block = -1;
            return i;
        }
    }
    return -1;
}

int alloc_block() {
    for (int i = 0; i < (int)part->s.num_blocks; i++) {
        if (!is_set(block_bitmap, i)) {
            set_bit(block_bitmap, i);
            part->s.num_free_blocks--;
            memset(part->data_blocks[i].d, 0, BLOCK_SIZE);
            return i;
        }
    }
    return -1;
}

//file write helper
int add_block_to_inode(struct inode *node) {
    int b = alloc_block();
    if (b < 0) return -1;

    //direct blocks
    for (int i = 0; i < 6; i++) {
        if (node->blocks[i] == 0) {
            if (b == 0) b = alloc_block();
            node->blocks[i] = b;
            return b;
        }
    }

    //indirect blocks
    if (node->indirect_block == -1) {
        int ind = alloc_block();
        if (ind < 0) return -1;
        if (ind == 0) ind = alloc_block();
        node->indirect_block = ind;
    }

    unsigned short *indices = (unsigned short *)part->data_blocks[node->indirect_block].d;
    for (int i = 0; i < 512; i++) {
        if (indices[i] == 0) {
            if (b == 0) b = alloc_block();
            indices[i] = (unsigned short)b;
            return b;
        }
    }
    return -1;
}

void write_to_file(int inode_idx, const char *data, int len) {
    struct inode *node = &part->inode_table[inode_idx];
    int written = 0;

    while (written < len) {
        int current_pos = node->size + written;
        int log_blk_idx = current_pos / BLOCK_SIZE;
        int offset_in_blk = current_pos % BLOCK_SIZE;
        int phys_blk = -1;

        if (log_blk_idx < 6) {
            phys_blk = node->blocks[log_blk_idx];
        } else if (node->indirect_block != -1) {
            unsigned short *ind = (unsigned short*)part->data_blocks[node->indirect_block].d;
            phys_blk = ind[log_blk_idx - 6];
        }

        if (phys_blk <= 0) {
            phys_blk = add_block_to_inode(node);
            if (phys_blk < 0) {
                printf("Error: No space left to write file\n");
                return;
            }
        }

        int to_write = BLOCK_SIZE - offset_in_blk;
        if (to_write > (len - written)) to_write = (len - written);

        memcpy(part->data_blocks[phys_blk].d + offset_in_blk, data + written, to_write);
        written += to_write;
    }
    //update inode metadata
    node->size += written; 
    node->date = (unsigned int)time(NULL);
}

void add_entry_to_dir(int parent_inode_idx, int child_inode_idx, const char *name) {
    struct inode *parent = &part->inode_table[parent_inode_idx];
    
    //current offset in parent directory
    int offset_in_block = parent->size % BLOCK_SIZE;
    int space_left = BLOCK_SIZE - offset_in_block;
    
    //if not enough space, pad the block
    if (space_left < sizeof(struct dentry) && space_left > 0 && offset_in_block != 0) {
    
        char pad_buffer[1024]; 
        memset(pad_buffer, 0, space_left);
        
        struct dentry *pad_de = (struct dentry*)pad_buffer;
        
        pad_de->inode = 0;           
        pad_de->dir_length = space_left; 
    
        write_to_file(parent_inode_idx, pad_buffer, space_left);
        
        printf("DEBUG: Added padding of %d bytes to inode %d\n", space_left, parent_inode_idx);
    }

    //create directory entry
    struct dentry de;
    memset(&de, 0, sizeof(de));
    de.inode = child_inode_idx;
    de.dir_length = sizeof(struct dentry);
    
    strncpy((char*)de.name, name, 255); 

    if (part->inode_table[child_inode_idx].mode & INODE_MODE_DIR_FILE)
        de.file_type = DENTRY_TYPE_DIR_FILE;
    else
        de.file_type = DENTRY_TYPE_REG_FILE;

    write_to_file(parent_inode_idx, (char*)&de, sizeof(struct dentry));
}

////////////////////// Main Function //////////////////////
int main() {
    printf("--- Generating Disk Image (Fixed) ---\n");
    
    part = malloc(sizeof(struct partition));
    if (!part) return 1;
    memset(part, 0, sizeof(struct partition));
    memset(inode_bitmap, 0, sizeof(inode_bitmap));
    memset(block_bitmap, 0, sizeof(block_bitmap));

    struct super_block *sb = &part->s;
    sb->partition_type = SIMPLE_PARTITION;
    sb->block_size = BLOCK_SIZE;
    sb->inode_size = sizeof(struct inode);
    sb->num_inodes = 224;
    sb->num_blocks = 4088;
    sb->num_free_inodes = 224;
    sb->num_free_blocks = 4088;
    strcpy(sb->volume_name, "FIXED_DISK");

    // Reserve block 0 and inode 0
    set_bit(inode_bitmap, 0);
    set_bit(block_bitmap, 0);
    sb->num_free_inodes--;
    sb->num_free_blocks--;

    // Root Inode
    int root_idx = alloc_inode();
    struct inode *root = &part->inode_table[root_idx];
    root->mode = INODE_MODE_DIR_FILE | INODE_MODE_AC_ALL;
    root->date = (unsigned int)time(NULL);
    
    add_entry_to_dir(root_idx, root_idx, ".");
    add_entry_to_dir(root_idx, root_idx, "..");

    srand(time(NULL));
    char filename[32];
    int lorem_len = strlen(LOREM_IPSUM);

    // Create random files in root directory
    for (int i = 0; i < NUM_RANDOM_FILES; i++) {
        int f_idx = alloc_inode();
        if (f_idx < 0) break;

        struct inode *f = &part->inode_table[f_idx];
        f->mode = INODE_MODE_REG_FILE | INODE_MODE_AC_ALL; // rwxrwxrwx
        
        sprintf(filename, "file_%d", i);

        //random content
        int content_size = (rand() % 2000) + 100;
        char *buffer = malloc(content_size + 1);
        for (int k = 0; k < content_size; k++) buffer[k] = LOREM_IPSUM[k % lorem_len];
        
        write_to_file(f_idx, buffer, content_size);
        free(buffer);
        
        add_entry_to_dir(root_idx, f_idx, filename);
        
        printf("Created %s (inode %d, size %d)\n", filename, f_idx, f->size);
    }

    FILE *fp = fopen(DISK_FILE, "wb");
    if (fp) {
        fwrite(part, 1, sizeof(struct partition), fp);
        fclose(fp);
        printf("Disk image '%s' created successfully.\n", DISK_FILE);
    } else {
        perror("Error writing file");
    }

    free(part);
    return 0;
}
