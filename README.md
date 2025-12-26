# SimpleFS

A minimal, disk-image–based file system implementation designed to study **filesystem structure, state transitions, and I/O behavior** at the code level.

This project prioritizes **structural clarity, observability, and verifiable behavior** over feature completeness.

---

## Overview

**SimpleFS** is an educational file system that implements the core components of a traditional UNIX-like filesystem:

- Superblock
- Inode table
- Directory entries
- Data block mapping
- Buffer cache and directory hash cache

The primary goal is to understand how file system operations (`mount`, `open`, `read`, `write`, `close`) interact with metadata and storage, and how these interactions form a coherent state transition model.

The implementation intentionally limits scope (e.g., no journaling, limited write support) to keep the internal logic explicit and analyzable.

---

## Repository Structure

```
.
├── Dockerfile          # Multi-stage build / debug / release pipeline
├── Makefile            # Release, debug, and sanitizer builds
├── include/            # Public headers
├── src/
│   ├── main.c          # Program entry point
│   ├── simplefs.c      # Core filesystem logic
│   ├── mk_simplefs.c   # Disk image generator
│   └── util.c          # Shared utilities
└── README.md
```

---

## Build System

### Makefile Targets

| Target       | Description                                       |
| ------------ | ------------------------------------------------- |
| `make`       | Optimized release build (`-O3`, LTO enabled)      |
| `make debug` | Debug build (`-O0`, `-g3`, `FS_DEBUG`)            |
| `make asan`  | AddressSanitizer + UBSan build (development only) |
| `make clean` | Remove build artifacts                            |

The release build prioritizes predictable execution and performance, while debug and sanitizer builds are intended for internal verification and analysis.

---

## Docker-Based Workflow

The project uses a **multi-stage Docker build** to clearly separate concerns.

### 1. Build Stage

- Base image: `gcc:13-bookworm`
- Performs compilation only
- Optimized Docker layer caching by copying files in order of increasing volatility
- Produces `simplefs` and `mk_simplefs` binaries

### 2. Debug / Analysis Stage

- Base image: `debian:bookworm-slim`
- Includes:

  - `strace`
  - `gdb`
  - `procps`

- Runs as **root** to allow `SYS_PTRACE`

This stage exists to **observe filesystem behavior at the syscall and kernel boundary**.

Example:

```bash
strace -e read,write,lseek ./simplefs disk.img
```

### 3. Release Stage

- Base image: `debian:bookworm-slim`
- Runs as a non-root user
- Deploys only the compiled binary
- Uses a dedicated volume (`/data`) to avoid OverlayFS copy-on-write overhead
- Read-only executable permissions

This stage represents a **minimal, hardened runtime environment**.

---

## Usage

### 1. Create a Disk Image

```bash
./mk_simplefs disk.img
```

This generates:

- Superblock
- Inode table
- Root directory
- Files with deterministic random content

### 2. Run SimpleFS

```bash
./simplefs disk.img
```

The program will:

- Mount the filesystem
- Parse and list the root directory
- Perform random file `open` and `read` operations
- Print buffer cache statistics

---

## Design Focus

- Explicit metadata handling (no hidden global state)
- Clear separation between:

  - Name resolution
  - Inode lookup
  - Block mapping
  - Data access

- Cache behavior is observable and measurable
- State transitions are analyzable step by step

This is not a black-box filesystem; it is designed to be **inspected, traced, and reasoned about**.

---

## Limitations (Intentional)

- No journaling or write-ahead logging
- Limited write semantics
- No directory mutation (`rename`, `unlink`)
- Single root filesystem

These constraints are intentional to keep the system small enough that **every state transition is understandable**.

---

## Summary

SimpleFS is a compact filesystem implementation built to answer one question:

> _How do filesystem structures, metadata, and I/O paths interact in practice?_

Rather than maximizing features, this project emphasizes:

- correctness,
- transparency,
- and explainability.

It is intended as a foundation for deeper exploration into filesystem reliability, journaling, and storage-specific designs.
