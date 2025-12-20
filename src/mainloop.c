
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
char **collect_root_elements(unsigned int *out_count, unsigned int type_filter) {
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
    for (unsigned bi = 0; bi < sb->num_blocks && remaining > 0; ++bi) {
        unsigned short block_no = root_ino.blocks[bi];
        if (block_no == 0) continue;
        if (read_data_block(block_no, buf) != 0) break;
        unsigned int off = 0;
        while (off + sizeof(struct dentry) <= bsize && remaining > 0) {
            struct dentry *de = (struct dentry *)((char*)buf + off);
            if (de->dir_length == 0) break;
            if (de->file_type == type_filter) {
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

    // Command loop
    char cmd[256];
    int open_fd = -1;
    while (1) {
        printf("\n> ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        // Remove trailing newline
        cmd[strcspn(cmd, "\n")] = 0;
        if (strcmp(cmd, "ls") == 0) {
            unsigned int count = 0;
            char **files = collect_root_elements(&count, DENTRY_TYPE_REG_FILE);
            if (!files || count == 0) {
                printf("No files found.\n");
            } else {
                printf("Files in root:\n");
                for (unsigned int i = 0; i < count; ++i) {
                    printf("  %s\n", files[i]);
                    free(files[i]);
                }
                free(files);
            }
            count = 0;
            files = collect_root_elements(&count, DENTRY_TYPE_DIR_FILE);
            if (!files || count == 0) {
                printf("No Direktorys found.\n");
            } else {
                printf("Dirs in root:\n");
                for (unsigned int i = 0; i < count; ++i) {
                    printf("  %s\n", files[i]);
                    free(files[i]);
                }
                free(files);
            }
        } else if (strncmp(cmd, "open ", 5) == 0) {
            if (open_fd != -1) {
                printf("A file is already open. Close it first.\n");
                continue;
            }
            const char *fname = cmd + 5;
            open_fd = fs_open(fname, 0);
            if (open_fd < 0) {
                printf("Failed to open file: %s\n", fname);
            } else {
                printf("Opened file: %s (fd=%d)\n", fname, open_fd);
            }
        } else if (strcmp(cmd, "read") == 0) {
            if (open_fd == -1) {
                printf("No file is open.\n");
                continue;
            }
            char buf[1025];
            int n = fs_read(open_fd, buf, 1024);
            if (n < 0) {
                printf("Read error.\n");
            } else if (n == 0) {
                printf("EOF\n");
            } else {
                buf[n] = 0;
                printf("%s\n", buf);
            }
        } else if (strcmp(cmd, "close") == 0) {
            if (open_fd == -1) {
                printf("No file is open.\n");
            } else {
                fs_close(open_fd);
                printf("Closed file (fd=%d)\n", open_fd);
                open_fd = -1;
            }
        } else if (strncmp(cmd, "create ", 7) == 0) {
            const char *fname = cmd + 7;
            if (fs_createfiel(fname, 0) == 0) {
                printf("Created file: %s\n", fname);
            } else {
                printf("Failed to create file: %s\n", fname);
            }
        } else if (strncmp(cmd, "write ", 6) == 0) {
            if (open_fd == -1) {
                printf("No file is open. Use 'open <filename>' first.\n");
            } else {
                const char *text = cmd + 6;
                int n = fs_write(open_fd, text, strlen(text));
                if (n < 0) {
                    printf("Write error.\n");
                } else {
                    printf("Wrote %d bytes.\n", n);
                }
            }
        } else if (strncmp(cmd, "delete ", 7) == 0) {
            const char *fname = cmd + 7;
            if (fs_delete(fname) == 0) {
                printf("Deleted file: %s\n", fname);
            } else {
                printf("Failed to delete file: %s\n", fname);
            }
        } else if (strncmp(cmd, "mkdir ", 6) == 0) {
            const char *dname = cmd + 6;
            if (fs_makedir(dname) == 0) {
                printf("Created directory: %s\n", dname);
            } else {
                printf("Failed to create directory: %s\n", dname);
            }
        } else if (strncmp(cmd, "rmdir ", 6) == 0) {
            const char *dname = cmd + 6;
            if (fs_removedir(dname) == 0) {
                printf("Removed directory: %s\n", dname);
            } else {
                printf("Failed to remove directory: %s\n", dname);
            }
        } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0) {
            printf("Commands:\n");
            printf("  ls\n");
            printf("  open <filename>\n");
            printf("  read\n");
            printf("  write <text>\n");
            printf("  close\n");
            printf("  create <filename>\n");
            printf("  delete <filename>\n");
            printf("  mkdir <dirname>\n");
            printf("  rmdir <dirname>\n");
            printf("  exit\n");
        } else {
            printf("Unknown command. Type 'help' for commands.\n");
        }
    }
    if (open_fd != -1) fs_close(open_fd);
    fs_unmount();
    printf("Bye!\n");
    return 0;
}