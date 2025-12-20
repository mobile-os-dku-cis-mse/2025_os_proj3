
# Simple File System Project (os_proj3)

## Overview
This project is a fully functional educational file system written in C, simulating the core mechanisms of a real operating system's file system. It supports persistent storage, file and directory management, and exposes a C API and interactive shell for user operations. The codebase is modular, well-documented, and demonstrates key OS and filesystem concepts such as superblocks, inodes, directory entries, and block management.

## Project Structure

- **include/fs.h**: Public API, core data structures (superblock, inode, dentry), and constants.
- **src/fs.c**: Main file system implementation, including mounting, file/directory operations, and all internal logic.
- **src/mainloop.c**: Interactive shell for user commands (ls, open, read, write, create, delete, mkdir, rmdir, etc.).
- **src/demo.c**: Example program for mounting, listing, and reading files.
- **tools/mk_simplefs.c**: Utility to create a new file system disk image from host files.
- **tests/fs_all_tests.c**: Automated test suite for the file system API.
- **Makefile**: Build instructions for all binaries and test images.
- **bin/**: Compiled binaries (mainloop, kernel_sim, mk_simplefs, fs_all_tests, etc.).
- **tests/**: Test files and generated disk images.

## Core File System Design

### Layout
- **Superblock**: Contains metadata about the file system (block size, inode info, volume name, etc.).
- **Inode Table**: Array of inodes, each representing a file or directory (mode, size, block pointers, etc.).
- **Data Blocks**: Store file and directory contents.
- **Directory Entries (dentry)**: Map file/directory names to inodes and types.

### Key Structures (see `include/fs.h`)
- `struct super_block`: File system metadata (partition type, block size, inode info, etc.).
- `struct inode`: Describes a file or directory (mode, permissions, size, block pointers).
- `struct dentry`: Directory entry (name, inode, type, length).

### Features
- **Mount/Unmount**: Loads the superblock, caches root directory, and manages open file table.
- **File Operations**: Open, read, write, and close files using inodes and data blocks.
- **Directory Management**: Create, delete, and list directories and files. Supports both root and subdirectory operations.
- **Disk Image**: All data is stored in a single disk image file, simulating a real file system partition.
- **Debugging**: Extensive debug output for all major operations, especially for directory removal and error cases.
- **Testing**: Automated tests for all major API functions.

## API Functions (see `include/fs.h`)

- `int fs_mount(const char *disk_path);` — Mount a disk image.
- `int fs_unmount(void);` — Unmount the file system.
- `int fs_open(const char *path, int flags);` — Open a file (supports both root and subdirectory files).
- `int fs_close(int fd);` — Close a file descriptor.
- `int fs_read(int fd, void *outbuf, size_t count);` — Read from a file.
- `int fs_createfiel(const char *path, int flags);` — Create a new file (supports subdirectory paths).
- `int fs_write(int fd, const void *buf, size_t count);` — Write to a file.
- `int fs_delete(const char *path);` — Delete a file (supports subdirectory paths).
- `int fs_makedir(const char *path);` — Create a directory (root only).
- `int fs_removedir(const char *path);` — Remove a directory (root only, with detailed debug output).

## Implementation Details

### Mounting and Unmounting
- Loads the superblock and caches the root directory entries for fast lookup.
- Initializes the open file table and directory cache.

### File Operations
- **Open**: Finds the file in the root or subdirectory, loads its inode, and allocates a file descriptor.
- **Read/Write**: Supports reading and writing to files using direct and indirect block pointers. Handles file offsets and block boundaries.
- **Close**: Releases resources associated with the file descriptor.

### Directory Operations
- **Create Directory**: Adds a new directory entry in the root, allocates an inode and data block, and initializes the directory.
- **Remove Directory**: Checks if the directory is empty, clears its entry and inode, and provides detailed debug output for every step.
- **Create/Delete File**: Supports both root and subdirectory files. Handles directory entry management, inode allocation, and block management.

### Disk Image
- All file system data is stored in a single disk image file, which can be created using the `mk_simplefs` tool.
- The image contains the superblock, inode table, and all data blocks.

### Debugging
- The codebase includes extensive debug output, especially in directory removal and error handling, to aid in development and testing.

## Building the Project

Run `make` to build all binaries. Main targets:
- `bin/mainloop`: Interactive shell for the file system.
- `bin/kernel_sim`: Kernel simulation.
- `bin/mk_simplefs`: Disk image creation tool.
- `bin/fs_all_tests`: Automated test runner.

## Creating a File System Image

Use the `mk_simplefs` tool to create a new disk image:

```sh
bin/mk_simplefs tests/test.img file1.txt file2.txt ...
```
This creates a 4MB disk image with the specified files in the root directory.

## Running the File System

### Interactive Shell
Run the mainloop shell:

```sh
bin/mainloop tests/test.img
```
Available commands:
- `ls` — List files in the current directory
- `open <filename>` — Open a file
- `read` — Read from open file
- `write <text>` — Write to open file
- `close` — Close open file
- `create <filename>` — Create a new file
- `delete <filename>` — Delete a file
- `mkdir <dirname>` — Create a directory (root only)
- `rmdir <dirname>` — Remove a directory (root only)
- `exit` — Exit shell
- `help` — Show commands

### Demo Program
Run the demo to mount, list, and read files:

```sh
bin/demo tests/test.img
```

### Automated Tests
Run all tests:

```sh
make test
```
Or manually:
```sh
bin/fs_all_tests
```

## Example Workflow

1. Create a disk image:
	```sh
	bin/mk_simplefs tests/test.img tests/dummy.txt
	```
2. Run the interactive shell:
	```sh
	bin/mainloop tests/test.img
	```
3. Use commands like `ls`, `mkdir mydir`, `create mydir/foo`, `open mydir/foo`, `write Hello`, `read`, `close`, `delete mydir/foo`, `rmdir mydir`, etc.

## Notes
- The file system is for educational/demo purposes and is not intended for production use.
- The implementation demonstrates core OS and file system concepts: superblock, inodes, directory entries, block management, and file operations.
- Directory support is limited to root and single-level subdirectories for files.
- Debug output is extensive for learning and troubleshooting.

---
For more details, see the source code and comments in each file, and the project description in `Pojekt3TExt.txt`.

