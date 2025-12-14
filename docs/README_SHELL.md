# SimpleFS Shell - User Guide

## Introduction
The **SimpleFS Shell** is a command-line interface (CLI) that allows you to interact with SimpleFS disk images. It functions similarly to a standard UNIX terminal, allowing you to create files, directories, and navigate the custom file system structure.

## Compilation
To build the shell, you must compile the shell source code alongside the filesystem engine.

**Prerequisites:** GCC Compiler.

Run the following command in your terminal:
```bash
gcc shell.c filesystem.c -o shell
```

## Getting Started

### 1. Launching the Shell
You can launch the shell with or without a target disk image.

**Option A: Load an existing disk**
```bash
./shell my_disk.img
```

**Option B: Start empty (and format later)**
```bash
./shell
```

### 2. The Command Prompt
* `FS[No Disk]>`: No disk is currently mounted. You must use `mount` or `format`.
* `FS[disk.img]>`: The disk `disk.img` is mounted and ready for operations.

---

## Command Reference

### Disk Management

| Command | Usage | Description |
| :--- | :--- | :--- |
| **format** | `format <filename>` | Creates a NEW valid filesystem structure and mounts it. **Warning:** Overwrites existing files with the same name. |
| **mount** | `mount <filename>` | Loads an existing disk image into memory. |
| **save** | `save` | **Crucial:** Persists all changes made in memory to the physical disk file. If you exit without saving, changes are lost. |
| **info** | `info` | Displays volume name, block usage, and inode usage stats. |
| **exit** | `exit` | Closes the shell and frees memory. |

### File & Directory Operations

| Command | Usage | Description |
| :--- | :--- | :--- |
| **ls** | `ls <path>` | Lists the contents of a directory. <br>Ex: `ls /` or `ls /documents` |
| **mkdir** | `mkdir <path>` | Creates a new directory. <br>Ex: `mkdir /photos` |
| **touch** | `touch <path>` | Creates a new empty file. <br>Ex: `touch /notes.txt` |
| **write** | `write <path> <text>` | Overwrites a file with the provided text string. <br>Ex: `write /notes.txt Hello_World` |
| **cat** | `cat <path>` | Reads and displays the content of a file. |

---

## Example Workflow

Here is a typical session to create a disk and add files:

```text
$ ./shell
FS[No Disk]> format data.img
Disk formatted and created: data.img

FS[data.img]> mkdir /users
Directory created.

FS[data.img]> mkdir /users/john
Directory created.

FS[data.img]> touch /users/john/profile.txt
File created.

FS[data.img]> write /users/john/profile.txt This_is_my_profile_data
Written 23 bytes to /users/john/profile.txt.

FS[data.img]> ls /users/john
Directory listing of /users/john:
  .                    <DIR> (816 bytes)
  ..                   <DIR> (816 bytes)
  profile.txt          <FILE> (23 bytes)

FS[data.img]> save
Disk saved to data.img

FS[data.img]> exit
```

## Common Error Codes
If a command fails, the shell might return an error code. Here is what they mean:

* **-1 (FS_ERROR):** Generic error (IO or Memory).
* **-2 (FS_ENOENT):** File or Directory not found.
* **-3 (FS_EEXIST):** File already exists (cannot create duplicate).
* **-4 (FS_ENOSPC):** Disk is full (No free inodes or blocks).
* **-5 (FS_EINVAL):** Invalid path or argument.
* **-7 (FS_ENOTDIR):** You tried to use `ls` or `cd` on a regular file.