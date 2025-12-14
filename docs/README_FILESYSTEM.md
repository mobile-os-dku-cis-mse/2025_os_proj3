# OS_PROJ_3 - Filesystem Implementation Details

## Overview
This **project** is a custom, lightweight, userspace file system implementation designed to simulate low-level disk management. It operates on a single binary file (disk image) which acts as the physical storage device.

This document details the internal architecture, data structures, and memory management strategies used in `filesystem.c` and `filesystem.h`.

## Technical Specifications

| Parameter | Value | Description |
| :--- | :--- | :--- |
| **Magic Number** | `0x1111` | Identifier used to validate the partition. |
| **Block Size** | 1024 Bytes | Size of a single data block. |
| **Max Inodes** | 224 | Maximum number of files/directories allowed. |
| **Max Blocks** | 4088 | Total storage capacity (~4MB). |
| **Addressing** | 32-bit | Uses `unsigned int` for block/inode referencing. |

---

## 1. Disk Layout
The disk image is a binary blob structured strictly as follows:

```text
+---------------------+ Offset 0x0000
|     Super Block     | (Metadata, Magic Num, Volume Name)
+---------------------+ Offset 0x0400 (1KB)
|     Inode Table     | (Array of 224 Inode structures)
+---------------------+ Offset 0x2000 (8KB)
|                     |
|     Data Blocks     | (Array of 4088 Data Blocks)
|                     |
+---------------------+ End of File
```

## 2. Core Data Structures

### The Super Block
Located at the beginning of the partition, it holds global metadata.
* **Partition Type:** Must match `0x1111`.
* **Bitmaps:** Note that bitmaps (free/used maps) are **not** stored on the disk. They are rebuilt in RAM dynamically upon mounting (`fs_mount`) by scanning the Inode Table.

### The Inode (`struct inode`)
Represents a file or a directory.
* **Size:** Actual size of the file in bytes.
* **Blocks [0-5]:** Direct pointers to data blocks.
* **Indirect Block:** A pointer to a block containing a list of additional block indices (allows files larger than 6KB).
* **Mode:** Defines if it is a File (`0x10000`) or Directory (`0x20000`).

### The Directory Entry (`struct dentry`)
Directories are special files containing a sequence of these structures.
* **Inode:** The index of the file in the Inode Table.
* **Name:** Filename (max 256 chars).
* **Dir Length:** The offset to the next entry (used for variable-length records, though currently fixed to structure size).

---

## 3. Key Algorithms

### 3.1. Bitmap Reconstruction & Safety
To ensure data consistency without storing bitmaps on disk:
1.  On `fs_mount`, the system scans all 224 Inodes.
2.  If an Inode is used, it marks the corresponding bit in the `inode_bitmap`.
3.  It calculates how many blocks that file uses and marks them in the `block_bitmap`.
4.  **Critical Safety:** Inode **0** and Block **0** are permanently marked as "Reserved/Used" in the bitmaps. This prevents the allocator from returning `0`, which is used internally as a `NULL` or `INVALID` flag.

### 3.2. Path Resolution
The system does not store full paths. To resolve `/documents/secret.txt`:
1.  Start at **Root Inode** (stored in `fs_context`).
2.  Read the directory content of Root.
3.  Look for "documents". Get its Inode.
4.  Read the directory content of "documents".
5.  Look for "secret.txt". Get its Inode.

### 3.3. Allocation Strategy
* **First-Fit:** The system searches linearly for the first bit set to `0` in the bitmaps (starting from index 1).
* **Auto-Expansion:** When writing to a file, if the offset exceeds current capacity, new blocks are allocated and linked automatically (including the indirect block if necessary).

---

## 4. API Reference
The filesystem exposes the following operations via `filesystem.h`:

* **Lifecycle:** `fs_init`, `fs_destroy`.
* **Disk Ops:** `fs_format` (creates new layout), `fs_mount` (loads existing), `fs_save` (persists to disk).
* **File Ops:** `fs_create`, `fs_read`, `fs_write`, `fs_list`.

## 5. Limitations
* **No Defragmentation:** Deleted blocks are freed, but fragmentation is not actively managed.
* **Single Indirect:** Max file size is limited to approx 512KB (Direct 6 + Indirect 1024/2 blocks).
* **Synchronous:** Operations block until memory copy is complete.