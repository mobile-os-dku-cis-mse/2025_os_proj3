#include <stdint.h> // For uint32_t
#include "../fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define DISK_IMAGE "../disk.img"
#define MAX_OPEN_FILES 10

// Global variables for file system state
static struct super_block sb;
static struct inode *inode_table;
static int disk_fd;

struct file_descriptor_entry {
    uint32_t inode_num;
    uint32_t offset; // current read/write offset
    int in_use;
    // Potentially other flags like mode (read/write)
};

static struct file_descriptor_entry open_files[MAX_OPEN_FILES];
static int next_fd = 0; // Simple counter for file descriptors

// Function prototypes
static uint32_t find_inode_by_name(const char *name, uint32_t parent_inode_num);
int fs_open(const char *pathname);
int fs_mount();
void fs_ls();

// ... fs_mount implementation starts here ...
int fs_mount() {
    disk_fd = open(DISK_IMAGE, O_RDONLY);
    if (disk_fd < 0) {
        perror("Failed to open disk image");
        return -1;
    }

    // Read superblock
    if (pread(disk_fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        perror("Failed to read superblock");
        close(disk_fd);
        return -1;
    }

    printf("Superblock loaded.\n");
    printf("Volume name: %s\n", sb.volume_name);
    printf("Number of inodes: %u\n", sb.num_inodes);
    printf("Number of blocks: %u\n", sb.num_blocks);
    printf("DEBUG: sb.first_inode=%u, sb.block_size=%u, inode_table_offset=%u\n",
           sb.first_inode, sb.block_size, sb.first_inode * sb.block_size);


    // Allocate memory for inode table
    inode_table = malloc(sb.num_inodes * sizeof(struct inode));
    if (!inode_table) {
        perror("Failed to allocate memory for inode table");
        close(disk_fd);
        return -1;
    }

    // Read inode table
    if (pread(disk_fd, inode_table, sb.num_inodes * sizeof(struct inode), sb.first_inode * sb.block_size) != sb.num_inodes * sizeof(struct inode)) {
        perror("Failed to read inode table");
        free(inode_table);
        close(disk_fd);
        return -1;
    }
    printf("Inode table loaded.\n");
    printf("Size of super_block: %lu\n", sizeof(struct super_block));
    printf("Size of inode: %lu\n", sizeof(struct inode));


    return 0;
}

void fs_ls() {
    printf("--- Root Directory Listing ---\n");
    struct inode root_inode = inode_table[0]; // Root inode is the first one
    printf("DEBUG: root_inode.mode=%u, root_inode.size=%u, root_inode.blocks[0]=%u\n",
           root_inode.mode, root_inode.size, root_inode.blocks[0]);

    for (int i = 0; i < 6; ++i) {
        if (root_inode.blocks[i] == 0) continue;

        char block_buffer[sb.block_size + 1]; // +1 for null terminator
        if (pread(disk_fd, block_buffer, sb.block_size, root_inode.blocks[i] * (uint64_t)sb.block_size) != sb.block_size) {
            perror("Failed to read directory block");
            return;
        }
        block_buffer[sb.block_size] = '\0'; // Null-terminate the buffer

        char *line = strtok(block_buffer, "\n");
        while (line != NULL) {
            char file_type_str[256];
            uint32_t inode_num;

            // Assuming format is "file <inode_number> <...>" or "<filename> <inode_number>".
            // The original instruction implies parsing filename and inode number.
            // Let's try to parse for a filename string and an inode number.
            // The instruction also mentioned "file <inode_number> <...>" as a possible format.
            // Given the goal is to extract filename and inode, we'll use a more generic sscanf.
            // If the format is "filename inode_num", then sscanf("%s %u", filename, &inode_num) would work.
            // If the format is "file <inode_number> <filename>", then sscanf("file %u %s", &inode_num, filename) would work.
            // The provided replace string uses "%s %u", implying "filename inode_num".
            // Sticking to the provided replace string's parsing logic.
            if (sscanf(line, "%s %u", file_type_str, &inode_num) == 2) {
                printf("Filename: %s, Inode: %u\n", file_type_str, inode_num);
            } else {
                printf("Could not parse directory entry line: %s\n", line);
            }

            line = strtok(NULL, "\n");
        }
    }
     printf("-----------------------------\n\n");
}

// Helper function to find inode by name within a directory
static uint32_t find_inode_by_name(const char *name, uint32_t parent_inode_num) {
    if (parent_inode_num >= sb.num_inodes) {
        fprintf(stderr, "Error: Invalid parent inode number %u\n", parent_inode_num);
        return 0; // 0 for invalid inode
    }

    struct inode parent_inode = inode_table[parent_inode_num];

    for (int i = 0; i < 6; ++i) { // Iterate through direct blocks of the parent inode
        if (parent_inode.blocks[i] == 0) continue;

        char block_buffer[sb.block_size + 1]; // +1 for null terminator
        if (pread(disk_fd, block_buffer, sb.block_size, parent_inode.blocks[i] * (uint64_t)sb.block_size) != sb.block_size) {
            perror("Failed to read directory block in find_inode_by_name");
            return 0;
        }
        block_buffer[sb.block_size] = '\0'; // Null-terminate the buffer

        char *line = strtok(block_buffer, "\n");
        while (line != NULL) {
            char file_type_str[256]; // Assuming "file" as the type string
            uint32_t inode_num;
            char extracted_name[256]; // To store the actual filename if present

            // Current parsing assumes "file <inode_num> <...>"
            // This needs to be adapted once a proper filename is available.
            // For now, let's assume the 'name' parameter is literally "file" and we search for inode "22".
            // This is a temporary hack until the disk.img format for names is clarified.

            // The format from raw dump was "file 22 122001270 776586478 425696772"
            // Let's assume the name is the first token, and inode is the second
            if (sscanf(line, "%s %u", extracted_name, &inode_num) == 2) {
                if (strcmp(extracted_name, name) == 0) {
                    return inode_num;
                }
            }
            line = strtok(NULL, "\n");
        }
    }
    return 0; // Not found
}


// Open file function
int fs_open(const char *pathname) {
    // For simplicity, assume pathnames are always in the root directory for now.
    // So, parent_inode_num will be 0 (root inode).
    uint32_t target_inode_num = find_inode_by_name(pathname, 0); // 0 is root inode

    if (target_inode_num == 0) {
        fprintf(stderr, "Error: File '%s' not found.\n", pathname);
        return -1;
    }

    // Find a free file descriptor entry
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!open_files[i].in_use) {
            open_files[i].inode_num = target_inode_num;
            open_files[i].offset = 0; // Start at beginning of file
            open_files[i].in_use = 1;
            return i; // Return file descriptor (index)
        }
    }

    fprintf(stderr, "Error: No free file descriptors.\n");
    return -1; // No free file descriptors
}


// Read file function
int fs_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].in_use) {
        fprintf(stderr, "Error: Invalid file descriptor %d.\n", fd);
        return -1;
    }

    uint32_t inode_num = open_files[fd].inode_num;
    struct inode current_inode = inode_table[inode_num];
    uint32_t current_offset = open_files[fd].offset;
    uint32_t bytes_read_total = 0;

    // Check if we are trying to read beyond the file size
    if (current_offset >= current_inode.size) {
        return 0; // Already at end of file
    }

    // Determine how much we can actually read
    uint32_t bytes_to_read = count;
    if (current_offset + bytes_to_read > current_inode.size) {
        bytes_to_read = current_inode.size - current_offset;
    }

    // Iterate through blocks to read data
    // Assuming block_size is 1024
    uint32_t block_size = sb.block_size;

    while (bytes_read_total < bytes_to_read) {
        uint32_t current_block_idx = current_offset / block_size; // Logical block index
        uint32_t offset_in_block = current_offset % block_size;   // Offset within the current block

        if (current_block_idx >= 6) { // Only direct blocks for now
            // Handle indirect blocks later if needed for larger files
            fprintf(stderr, "Error: File too large or indirect blocks not implemented.\n");
            break;
        }

        uint32_t physical_block_num = current_inode.blocks[current_block_idx];
        if (physical_block_num == 0) { // Should not happen if file size is correct
            fprintf(stderr, "Error: Encountered null data block in file.\n");
            break;
        }

        char temp_block_buffer[block_size];
        uint32_t read_from_block = block_size - offset_in_block; // Bytes remaining in this block
        if (read_from_block > (bytes_to_read - bytes_read_total)) {
            read_from_block = (bytes_to_read - bytes_read_total);
        }

        // Read the physical block from disk
        if (pread(disk_fd, temp_block_buffer, block_size, physical_block_num * (uint64_t)block_size) != block_size) {
            perror("Failed to read data block in fs_read");
            break;
        }

        // Copy data to user buffer
        memcpy((char *)buf + bytes_read_total, temp_block_buffer + offset_in_block, read_from_block);
        bytes_read_total += read_from_block;
        current_offset += read_from_block;
    }

    open_files[fd].offset = current_offset; // Update file offset
    return bytes_read_total;
}

// Close file function
int fs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].in_use) {
        fprintf(stderr, "Error: Invalid file descriptor %d for closing.\n", fd);
        return -1;
    }

    open_files[fd].in_use = 0;
    open_files[fd].offset = 0; // Reset offset for next open
    return 0;
}

int main() {
    if (fs_mount() != 0) {
        fprintf(stderr, "Failed to mount file system.\n");
        return 1;
    }

    fs_ls(); // Keep ls for initial verification

    printf("\n--- Simulating user process: Reading 'file' 10 times ---\n");
    for (int i = 0; i < 10; ++i) {
        int fd = fs_open("file"); // Try to open "file"
        if (fd != -1) {
            printf("Attempt %d: Successfully opened 'file' with fd: %d\n", i + 1, fd);

            // Read content
            char read_buffer[1024]; // Max buffer size for a small file
            int bytes_read = fs_read(fd, read_buffer, sizeof(read_buffer) - 1); // Leave space for null terminator
            if (bytes_read > 0) {
                read_buffer[bytes_read] = '\0'; // Null-terminate the read content
                printf("Attempt %d: Content of 'file':\n%s\n", i + 1, read_buffer);
            } else if (bytes_read == 0) {
                printf("Attempt %d: End of file or empty file.\n", i + 1);
            } else {
                printf("Attempt %d: Error reading file.\n", i + 1);
            }

            fs_close(fd); // Close the file
            printf("Attempt %d: File with fd %d closed.\n", i + 1, fd);
        } else {
            printf("Attempt %d: Failed to open 'file'.\n", i + 1);
            // If it fails to open once, it will likely fail repeatedly, so break.
            break;
        }
        printf("\n");
    }
    printf("--- Simulation complete ---\n");

    return 0;
}
