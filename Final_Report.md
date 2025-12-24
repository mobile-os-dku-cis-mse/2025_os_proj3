# Final Project Report: Simple File System Implementation

## 1. Project Purpose

The goal of this project was to implement a simple, read-only file system based on a provided disk image (`disk.img`). The implementation was required to support core file system operations: mounting the file system, listing the files in the root directory, and opening, reading, and closing a file.

## 2. Implementation Details

The entire implementation is consolidated within the `cmd/project_fs.c` file. The file system interacts with the `disk.img` file, which contains the superblock, inode table, and data blocks.

### File System Mounting (`fs_mount`)

- The `fs_mount` function is the first operation performed. It opens `disk.img` in read-only mode.
- It reads the **superblock** from the beginning of the disk image to understand the file system's metadata (block size, number of inodes, etc.).
- It then calculates the offset to the **inode table** using information from the superblock (`sb.first_inode * sb.block_size`).
- Finally, it reads the entire inode table into memory for quick access.

### Directory Listing (`fs_ls`)

- A significant challenge was understanding the format of directory entries. While the project documentation provided a `struct dentry`, analysis of the `disk.img` revealed that the directory data blocks contained plain **ASCII text**, not binary structures.
- The `fs_ls` function reads the data block corresponding to the root directory (inode 0).
- It then parses this block as a text file, splitting it by newline characters to get individual directory entry lines.
- Each line is further parsed to extract a "filename" and its corresponding inode number, which are then printed to the console.

### File Operations (`fs_open`, `fs_read`, `fs_close`)

- **`fs_open(pathname)`**: This function takes a filename, searches for it within the root directory (by parsing the directory's text-based data block), and, if found, returns a file descriptor. The file descriptor is an index into a global `open_files` array, which stores the state of all currently open files (like the inode number and current read offset).
- **`fs_read(fd, buf, count)`**: This function reads `count` bytes from the file specified by the file descriptor `fd` into the buffer `buf`. It uses the inode information to locate the physical data blocks on the disk and reads the content sequentially, updating the file's offset after each read.
- **`fs_close(fd)`**: This function marks the file descriptor as no longer in use, making it available for future `fs_open` calls.

### User Simulation (`main`)

- The `main` function orchestrates the simulation.
- It first calls `fs_mount` to initialize the file system.
- It then calls `fs_ls` to display the root directory's contents.
- To fulfill the requirement of reading from ten files, and given the provided `disk.img` only contains one file named "file", the simulation proceeds to open, read, and close this single file ten times in a loop.

## 3. Challenges and Solutions

The primary challenge was the **discrepancy between the binary `struct dentry` provided in the documentation and the actual text-based content of the directory blocks in `disk.img`**. Initial attempts to `memcpy` the binary struct resulted in garbled data and segmentation faults.

The solution was a systematic debugging process:
1.  **Packing Structs:** Ensured all file system structures were packed using `__attribute__((__packed__))` to eliminate padding issues.
2.  **Verifying Sizes:** Printed the `sizeof` each struct to confirm they matched the documentation.
3.  **Raw Byte Analysis:** After previous steps failed, I printed the raw hexadecimal content of the directory data block. This revealed the ASCII text "file 22 ...", which was the breakthrough.
4.  **Adapting the Logic:** Based on this discovery, I abandoned the binary struct approach for directory parsing and re-implemented it to parse the directory block as a simple text file. This resolved all data corruption issues.

## 4. Final Code Structure

The project is structured with a single C source file, `cmd/project_fs.c`, which contains all the logic. The data structures are defined in `fs.h`. The project is compiled using CMake, as specified in `CMakeLists.txt`.
