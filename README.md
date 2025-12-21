# SimpleFS - Operating Systems Project #3 (Simple File System)

This project provides a **fully working** reference implementation for the assignment:
- mount root filesystem (read superblock + inode table)
- print root directory listing (similar to `ls -al`)
- open / read / close regular files
- optional in-memory directory cache (hash table)
- disk-image creation tool: `mk_simplefs` (so the project runs even without a prebuilt `disk.img`)

## Folder structure
- `src/mk_simplefs.c` : creates `disk.img` with files `file_1`..`file_N`
- `src/simplefs_demo.c`: mounts `disk.img`, lists `/`, then opens/reads/closes 10 files
- `src/simplefs.h`      : on-disk structures + shared helpers
- `Makefile`            : build both tools

## Build (Linux / WSL / MSYS2 UCRT64)
```bash
make
```

## Create a disk image
Creates `disk.img` with 10 files by default:
```bash
./mk_simplefs disk.img 10
```

## Run the demo
```bash
./simplefs_demo disk.img
```

## Using your own disk.img
If you already have a `disk.img` that matches the assignment structure, you can run:
```bash
./simplefs_demo disk.img
```

## Notes
- Block size: 1024 bytes
- Total blocks: 4096 (4MB image)
- Inodes: 224 (32-byte on-disk inode)
- Root directory inode number: 0
- Directory entries are stored sequentially in the root directory file.

