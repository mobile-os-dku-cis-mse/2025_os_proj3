# Disk Image Generation Report

## 1. Overview
The **DiskCreationTool** is a C utility designed to generate a raw disk image file (`disk.img`) formatted with a custom, lightweight file system. The tool initializes the partition structure in memory, populates the file system metadata (Superblock, Inodes), creates a root directory, and populates it with randomly generated files before flushing the entire structure to a binary file.

## 2. File System Architecture (`fs.h`)
The file system is defined by a fixed-size partition structure containing a Superblock, an Inode Table, and a Data Block region.

### 2.1 Disk Layout
The physical partition is represented by the `struct partition`, which imposes a strict layout on the disk image.

| Region | Size | Description |
| :--- | :--- | :--- |
| **Superblock** | 1024 Bytes | Contains global filesystem metadata (volume name, counts, sizes). |
| **Inode Table** | 7168 Bytes | A fixed array of **224** inodes (32 bytes each) ($32 \times 224$). |
| **Data Blocks** | ~4 MB | An array of **4088** blocks, each 1024 bytes in size. |

### 2.2 Key Specifications
* **Magic Number:** `0x1111` (Identifies the partition type).
* **Block Size:** $1024$ bytes (`0x400`).
* **Max Files (Inodes):** 224.
* **Max Storage Blocks:** 4088.

### 2.3 Data Structures

#### **Superblock** (`struct super_block`)
Occupies the first 1024 bytes of the partition. It tracks the state of the filesystem, including:
* `partition_type`: Magic number validation.
* `block_size` & `inode_size`: Geometry definitions.
* `num_free_inodes` & `num_free_blocks`: Usage tracking.
* `volume_name`: 24-byte volume label (set to "FIXED_DISK" in `DiskCreationTool.c`).

#### **Inode** (`struct inode`)
A 32-byte structure representing a file or directory.
* **Addressing Scheme:**
    * **Direct:** `blocks[6]` array storing the first 6 data block indices.
    * **Indirect:** `indirect_block` points to a data block containing up to 512 additional block indices (block size 1024 / 2 bytes per short).
* **Metadata:** Stores file mode (permissions/type), size, write-lock status, and creation date.

#### **Directory Entry** (`struct dentry`)
Directories are special files containing a list of `dentry` structures.
* **Size:** ~272 bytes (Fixed name buffer of 256 bytes + metadata).
* **Fields:** Inode number, directory record length, file type, and filename.

---

## 3. Implementation Logic (`DiskCreationTool.c`)

The tool operates in a single pass, constructing the filesystem in RAM (`malloc` of `struct partition`) and writing the result to disk.

### 3.1 Initialization
1.  **Memory Allocation:** Allocates the full partition structure.
2.  **Bitmap Setup:** Initializes in-memory bitmaps (`inode_bitmap`, `block_bitmap`) to track allocations.
3.  **Superblock Setup:** Sets the magic number, block counts, and volume name.
4.  **Reservation:** Reserves Block 0 and Inode 0 (marking them as used in bitmaps).

### 3.2 Core Algorithms

* **Allocation (`alloc_inode`, `alloc_block`)**:
    Performs a linear search on the respective bitmap to find the first free index, marks it as used, and updates the superblock counters.

* **File Writing (`write_to_file`)**:
    Writes data buffer to a specific inode. It handles the logic of switching between direct and indirect addressing:
    1.  Calculates the logical block index based on the write offset.
    2.  If the logical index $< 6$, it uses `node->blocks[]`.
    3.  If the logical index $\ge 6$, it allocates an indirect block (if null) and looks up/allocates the physical block within that indirect block.

* **Directory Management (`add_entry_to_dir`)**:
    Appends a `dentry` to a directory's data.
    * **Padding Logic:** Before writing a new entry, it checks if the remaining space in the current block is sufficient. If not ($< \text{sizeof(dentry)}$), it fills the remainder with padding to ensure directory entries do not cross block boundaries.

### 3.3 Main Execution Flow
1.  **Root Creation:** Allocates Inode 1 for the root directory and adds standard `.` and `..` entries.
2.  **Content Generation:** Loops 10 times to create random files:
    * Allocates a new inode.
    * Generates random text content using a "Lorem Ipsum" source.
    * Writes content to the file.
    * Updates the root directory with the new file's name (`file_0`, `file_1`, etc.).
3.  **Image Flush:** Opens `disk.img` and writes the entire `struct partition` memory block to the file.

---

## 4. Usage

To build and run the tool, use the GCC compiler:

```bash
# Compile the tool
gcc -o DiskCreationTool DiskCreationTool.c

# Run the tool to generate disk.img
./DiskCreationTool
