
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../include/fs.h"

// Forward declarations for low-level helpers
int read_superblock(struct super_block *out);
int read_inode(unsigned int inode_index, struct inode *out);
int read_data_block(unsigned int block_rel, void *buf);
const struct super_block* get_superblock(void);

// Collect all regular file names in the root directory
char **collect_root_files(unsigned int *out_count) {
    *out_count = 0;
    const struct super_block *sb = get_superblock();
    if (!sb) return NULL;
    unsigned int root_idx = sb->first_inode;
    struct inode root_ino;
    if (read_inode(root_idx, &root_ino) != 0) return NULL;
    unsigned int bsize = sb->block_size;
    void *buf = malloc(bsize);
    if (!buf) return NULL;

    // Dynamic array for file names
    size_t cap = 128;
    char **names = calloc(cap, sizeof(char*));
    if (!names) { free(buf); return NULL; }

    unsigned int remaining = root_ino.size;
    for (int bi = 0; bi < 6 && remaining > 0; ++bi) {
        unsigned short block_no = root_ino.blocks[bi];
        if (block_no == 0) continue;
        if (read_data_block(block_no, buf) != 0) break;
        unsigned int off = 0;
        while (off + sizeof(struct dentry) <= bsize && remaining > 0) {
            struct dentry *de = (struct dentry *)((char*)buf + off);
            if (de->dir_length == 0) break;
            if (de->file_type == DENTRY_TYPE_REG_FILE) {
                unsigned int namelen = de->name_len > 255 ? 255 : de->name_len;
                char *nm = malloc(namelen + 1);
                if (!nm) break;
                memcpy(nm, de->name, namelen);
                nm[namelen] = '\0';
                if (*out_count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(names, cap * sizeof(char*));
                    if (!tmp) { free(nm); break; }
                    names = tmp;
                }
                names[*out_count] = nm;
                (*out_count)++;
            }
            off += de->dir_length;
            remaining = (remaining > de->dir_length) ? (remaining - de->dir_length) : 0;
        }
    }
    free(buf);
    return names;
}


int main(int argc, char **argv) {
    // Mount the file system (use disk.img by default)
    const char *disk = (argc > 1) ? argv[1] : "../disk.img";
    if (fs_mount(disk) != 0) {
        perror("fs_mount");
        return 1;
    }

    // Get superblock and print volume name
    const struct super_block *sb = get_superblock();
    if (!sb) {
        fprintf(stderr, "No superblock found!\n");
        fs_unmount();
        return 1;
    }
    printf("Mounted volume: %.24s\n", sb->volume_name);

    // Collect all regular files in root
    unsigned int count = 0;
    char **files = collect_root_files(&count);
    if (!files || count == 0) {
        fprintf(stderr, "No regular files found in root (count=%u)\n", count);
        fs_unmount();
        return 1;
    }
    printf("Found %u regular files in root\n", count);

    // Pick up to 10 unique random files
    unsigned int pick = (count < 10) ? count : 10;
    int *indices = calloc(count, sizeof(int));
    for (unsigned int i = 0; i < count; ++i) indices[i] = i;
    srand((unsigned)time(NULL));
    // Shuffle indices (Fisher-Yates)
    for (unsigned int i = 0; i < pick; ++i) {
        unsigned int j = i + rand() % (count - i);
        int tmp = indices[i]; indices[i] = indices[j]; indices[j] = tmp;
    }

    // Fork a child process to do file operations
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
    } else if (pid == 0) {
        // Child: open, read, and close each selected file
        printf("[child %d] Starting file operations for %u files\n", getpid(), pick);
        for (unsigned int k = 0; k < pick; ++k) {
            char *fname = files[indices[k]];
            printf("[child %d] OPEN %s\n", getpid(), fname);
            int fd = fs_open(fname, 0);
            if (fd < 0) {
                fprintf(stderr, "[child %d] fs_open failed for %s\n", getpid(), fname);
                continue;
            }
            const size_t CHUNK = 4096;
            void *buf = malloc(CHUNK);
            if (!buf) { fs_close(fd); continue; }
            printf("===== %s =====\n", fname);
            while (1) {
                int r = fs_read(fd, buf, CHUNK);
                if (r < 0) {
                    fprintf(stderr, "[child %d] fs_read error on %s\n", getpid(), fname);
                    break;
                }
                if (r == 0) break; // EOF
                fwrite(buf, 1, r, stdout);
            }
            printf("\n===== EOF %s =====\n\n", fname);
            free(buf);
            fs_close(fd);
            printf("[child %d] CLOSED %s\n", getpid(), fname);
        }
        // Free resources and exit
        for (unsigned int i = 0; i < count; ++i) free(files[i]);
        free(files);
        free(indices);
        fs_unmount();
        _exit(0);
    } else {
        // Parent: wait for child
        int status = 0;
        waitpid(pid, &status, 0);
        printf("[parent] Child finished with status %d\n", WEXITSTATUS(status));
    }

    // Cleanup
    for (unsigned int i = 0; i < count; ++i) free(files[i]);
    free(files);
    free(indices);
    fs_unmount();
    return 0;
}