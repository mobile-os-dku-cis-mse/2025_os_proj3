#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "filesystem.h"

#define MAX_CMD_LEN 1024
#define MAX_ARG_LEN 256

// Colors for the shell
#define COL_RESET "\033[0m"
#define COL_PROMPT "\033[1;32m" // Green
#define COL_ERROR "\033[1;31m"  // Red
#define COL_INFO "\033[1;34m"   // Blue

// Global state
static fs_context *ctx = NULL;
static char current_disk[256] = {0};
static int is_mounted = 0;

void print_help() {
    printf("\n--- Available Commands ---\n");
    printf("  mount <file>        : Load a disk image (e.g., mount disk.img)\n");
    printf("  format <file>       : Create and format a new disk\n");
    printf("  save                : Save changes to the current disk\n");
    printf("  ls <path>           : List directory contents\n");
    printf("  cat <path>          : Read file content\n");
    printf("  mkdir <path>        : Create a directory\n");
    printf("  touch <path>        : Create an empty file\n");
    printf("  write <path> <text> : Write text to a file\n");
    printf("  info                : Show disk information\n");
    printf("  help                : Show this help\n");
    printf("  exit                : Quit the shell\n");
    printf("--------------------------\n");
}

void print_ls_callback(const char *name, int type, unsigned int size) {
    int is_dir = 0;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        is_dir = 1;
    else if (type == DENTRY_TYPE_DIR_FILE)
        is_dir = 1;

    printf("  %-15s %s (%u octets)\n", name, is_dir ? "[DIR]" : "[FILE]", size);
}

int main() {
    char input[MAX_CMD_LEN];
    char *cmd, *arg1, *arg2;

    printf("=== SimpleFS Shell ===\n");
    printf("Type 'help' for commands.\n");

    ctx = fs_init();
    if (!ctx) {
        printf(COL_ERROR "Fatal: Memory allocation failed.\n" COL_RESET);
        return 1;
    }

    while (1) {
        // Display Prompt
        if (is_mounted) {
            printf(COL_PROMPT "FS[%s]> " COL_RESET, current_disk);
        } else {
            printf(COL_PROMPT "FS[No Disk]> " COL_RESET);
        }

        // Get Input
        if (!fgets(input, MAX_CMD_LEN, stdin)) break;
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;

        // Skip empty lines
        if (strlen(input) == 0) continue;

        // Parse command
        cmd = strtok(input, " ");
        arg1 = strtok(NULL, " ");
        arg2 = strtok(NULL, ""); // Get the rest of the line

        // --- COMMANDS ---

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        }

        else if (strcmp(cmd, "help") == 0) {
            print_help();
        }

        else if (strcmp(cmd, "format") == 0) {
            if (!arg1) {
                printf(COL_ERROR "Usage: format <filename>\n" COL_RESET);
                continue;
            }
            if (fs_format(ctx, "NewVolume") == FS_SUCCESS) {
                if (fs_save(ctx, arg1) == FS_SUCCESS) {
                    printf(COL_INFO "Disk formatted and created: %s\n" COL_RESET, arg1);
                    strncpy(current_disk, arg1, 255);
                    is_mounted = 1;
                    // Remount to be sure pointers are fresh
                    fs_mount(ctx, current_disk); 
                } else {
                    printf(COL_ERROR "Error saving disk to file.\n" COL_RESET);
                }
            } else {
                printf(COL_ERROR "Error formatting memory.\n" COL_RESET);
            }
        }

        else if (strcmp(cmd, "mount") == 0) {
            if (!arg1) {
                printf(COL_ERROR "Usage: mount <filename>\n" COL_RESET);
                continue;
            }
            if (fs_mount(ctx, arg1) == FS_SUCCESS) {
                strncpy(current_disk, arg1, 255);
                is_mounted = 1;
                printf(COL_INFO "Mounted %s successfully.\n" COL_RESET, arg1);
            } else {
                printf(COL_ERROR "Failed to mount %s. (Does file exist?)\n" COL_RESET, arg1);
            }
        }

        // --- Commands requiring a mounted disk ---

        else if (strcmp(cmd, "info") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            printf("Volume: %s\n", ctx->part->s.volume_name);
            printf("Root Inode: %d\n", ctx->root_inode);
            printf("Free Inodes: %d\n", ctx->part->s.num_free_inodes);
            printf("Free Blocks: %d\n", ctx->part->s.num_free_blocks);
        }

        else if (strcmp(cmd, "ls") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            const char *path = arg1 ? arg1 : "/";
            printf("Directory listing of %s:\n", path);
            int res = fs_list(ctx, path, print_ls_callback);
            if (res != FS_SUCCESS) printf(COL_ERROR "Error: %d (Path not found?)\n" COL_RESET, res);
        }

        else if (strcmp(cmd, "mkdir") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            if (!arg1) { printf(COL_ERROR "Usage: mkdir <path>\n" COL_RESET); continue; }
            
            int res = fs_create(ctx, arg1, INODE_MODE_DIR_FILE);
            if (res == FS_SUCCESS) printf("Directory created.\n");
            else printf(COL_ERROR "Error: %d\n" COL_RESET, res);
        }

        else if (strcmp(cmd, "touch") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            if (!arg1) { printf(COL_ERROR "Usage: touch <path>\n" COL_RESET); continue; }

            int res = fs_create(ctx, arg1, INODE_MODE_REG_FILE);
            if (res == FS_SUCCESS) printf("File created.\n");
            else printf(COL_ERROR "Error: %d\n" COL_RESET, res);
        }

        else if (strcmp(cmd, "write") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            if (!arg1 || !arg2) { printf(COL_ERROR "Usage: write <path> <text content>\n" COL_RESET); continue; }

            int len = strlen(arg2);
            int res = fs_write(ctx, arg1, arg2, len, 0);
            if (res >= 0) printf("Written %d bytes to %s.\n", res, arg1);
            else printf(COL_ERROR "Error writing file: %d\n" COL_RESET, res);
        }

        else if (strcmp(cmd, "cat") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            if (!arg1) { printf(COL_ERROR "Usage: cat <path>\n" COL_RESET); continue; }

            char buf[2048] = {0};
            int res = fs_read(ctx, arg1, buf, 2047, 0);
            if (res >= 0) {
                printf("--- Start of file ---\n");
                printf("%s\n", buf);
                printf("--- End of file ---\n");
            } else {
                printf(COL_ERROR "Error reading file: %d\n" COL_RESET, res);
            }
        }

        else if (strcmp(cmd, "save") == 0) {
            if (!is_mounted) { printf(COL_ERROR "No disk mounted.\n" COL_RESET); continue; }
            if (fs_save(ctx, current_disk) == FS_SUCCESS) {
                printf(COL_INFO "Disk saved to %s\n" COL_RESET, current_disk);
            } else {
                printf(COL_ERROR "Error saving disk.\n" COL_RESET);
            }
        }

        else {
            printf(COL_ERROR "Unknown command: %s\n" COL_RESET, cmd);
        }
    }

    fs_destroy(ctx);
    printf("Bye :)\n");
    return 0;
}
