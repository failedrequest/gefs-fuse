# GEFS FUSE Port for FreeBSD

This repository contains the FreeBSD FUSE port of **GEFS** (**Good Enough File System**), originally designed and implemented by Ori Bernstein for Plan 9 / 9front (`git://shithub.us/ori/gefs`).

GEFS is a crash-safe, copy-on-write snapshotting filesystem based on $B^\epsilon$-trees (write-optimized B-trees with deferred mutation buffers) and multi-arena block storage with MetroHash64 data integrity checksums.

---

## Features

- **Full Read/Write FUSE Driver (`gefs-fuse`)**:
  - Namespace & Directory: `getattr`, `readdir`, `mkdir`, `rmdir`, `unlink`, `rename`, `symlink`, `readlink`
  - File I/O: `open`, `create`, `read`, `write`, `truncate`, `fsync`
  - Metadata: `chmod`, `chown`, `utimens`, `statfs`
  - Snapshot selection: Mount default `main` snapshot or any snapshot label via `-s <snapshot>`.
- **On-Disk Format Compatibility**:
  - 16 KB fixed block size (`1 << 14`).
  - MetroHash64 block integrity checksums.
  - Multi-arena layout with allocation/free logging.
  - $B^\epsilon$-tree snapshot catalog and individual snapshot trees.
- **Userland Tooling**:
  - `mkfs.gefs`: Formats raw disk devices or image files.
  - `fsck.gefs`: Offline consistency checker that validates superblocks, arena logs, snapshot catalogs, and recursive $B^\epsilon$-tree node ordering.
  - `gefs-reader`: Diagnostic CLI to inspect superblocks, snapshot trees, directory hierarchies, and file payloads.
  - `gefs-test`: Automated unit & integration test runner.
  - `scripts/run_stress_test.sh`: End-to-end stress test script running `fsx` and `fio` benchmarks and generating test reports.

---

## Prerequisites

On FreeBSD, install `fusefs-libs3` and ensure the `fusefs` kernel module is loaded:

```sh
# Load the FreeBSD FUSE kernel module
sudo kldload fusefs

# To make fusefs persistent across reboots, add to /etc/rc.conf:
# sysrc kld_list+="fusefs"

# Install FUSE 3 libraries and (optional) fio benchmark tool
sudo pkg install fusefs-libs3 fio
```

---

## Building

To build all binaries:

```sh
make clean
make all
```

This compiles:
- `gefs-fuse` — FUSE 3 filesystem daemon
- `mkfs.gefs` — Filesystem formatter
- `fsck.gefs` — Consistency checker
- `gefs-reader` — Diagnostic inspection utility
- `gefs-test` — Internal integration test runner

---

## Quick Start & Usage

### 1. Create and Format a GEFS Image

```sh
# Create a sparse disk image file (minimum 128MB, e.g., 512MB)
truncate -s 512M /var/tmp/gefs.img

# Format with mkfs.gefs (creates main, adm, and empty snapshots)
./mkfs.gefs /var/tmp/gefs.img $USER
```

### 2. Verify Image Consistency

```sh
./fsck.gefs /var/tmp/gefs.img
```

### 3. Inspect Filesystem & Snapshots

```sh
# Inspect root directory of the active snapshot ('main')
./gefs-reader /var/tmp/gefs.img /

# Inspect admin snapshot (/adm/users)
./gefs-reader /var/tmp/gefs.img -s adm /users
```

### 4. Mount via FUSE

```sh
# Create mountpoint
mkdir -p /tmp/gefs-mnt

# Mount in foreground (-f) with debug/info output
./gefs-fuse /var/tmp/gefs.img /tmp/gefs-mnt -f

# (Alternatively, run in background)
# ./gefs-fuse /var/tmp/gefs.img /tmp/gefs-mnt
```

### 5. Unmount

```sh
sudo umount /tmp/gefs-mnt
```

---

## Testing

### Automated Test Suite

Run the built-in test suite to verify formatting, file I/O, directory traversal, symlinks, rename, truncation, unlinking, snapshots, and consistency checking:

```sh
make test
# Or directly:
./gefs-test
```

### Automated Stress Testing (`fsx` & `fio`)

The included script `scripts/run_stress_test.sh` automates the entire lifecycle: compiles the code, creates a test image, mounts it via FUSE, runs `fsx` random operations and `fio` benchmarks, unmounts, runs `fsck.gefs`, and generates `GEFS_STRESS_REPORT.txt`:

```sh
# Syntax: sudo ./scripts/run_stress_test.sh [fsx_operations] [image_size_mb]
sudo ./scripts/run_stress_test.sh 5000 512

# Or via make target:
sudo make stress
```

---

## Source Structure

- `src/gefs_storage.h` — Core on-disk structures, B-tree types, and storage engine APIs.
- `src/gefs_storage.c` — MetroHash64, serialization, LRU cache, $B^\epsilon$-tree engine, block allocator, mutations, and file/dir operations.
- `src/gefs_fuse.c` — High-level FUSE 3 filesystem operations driver.
- `src/gefs_ream.c` — `mkfs.gefs` implementation.
- `src/gefs_check.c` — `fsck.gefs` consistency checker.
- `src/gefs_reader.c` — `gefs-reader` diagnostic inspection tool.
- `src/gefs_test.c` — Automated integration test suite.
- `scripts/run_stress_test.sh` — Automated stress testing and benchmark reporting script.
