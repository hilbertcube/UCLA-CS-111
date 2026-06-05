# Lecture 12 — File System Implementation: FAT, Inodes, Directories, and Links

---

## Overview

This lecture covers how files are actually stored on disk. We move through three generations of design:

1. **Contiguous allocation** — simplest, most limited
2. **FAT (File Allocation Table)** — linked-list approach; flexible but slow for random access
3. **Inode-based file systems** — the design used by Linux (ext2/3/4) and most Unix systems; fast random access, rich metadata

Then we cover how directories, hard links, and symbolic links are built on top of inodes.

---

## Part 1: File System Design Goals

Before choosing a storage layout, define what you need:

| Goal | Meaning |
|------|---------|
| **Sequential access speed** | Reading a file from start to finish should be fast |
| **Random access speed** | Jumping to byte N (`lseek`) should be fast |
| **Space efficiency** | Minimize wasted space (fragmentation) |
| **Metadata** | Store ownership, permissions, timestamps per file |
| **Flexibility** | Files can grow, shrink, be created and deleted dynamically |

No single design perfectly optimizes all of these. Each approach trades one off against another.

---

## Part 2: Contiguous Allocation

**Idea:** Each file occupies a contiguous run of disk blocks, like an array.

```
Disk blocks:  [ file A: 0-4 ][ file B: 5-9 ][ file C: 10-14 ][ free ]
```

**Advantages:**
- Sequential reads are extremely fast (no seeking, just read forward)
- Random access is O(1): to read byte N, compute `block = base + N/block_size`
- Simple directory entry: just store (filename, start block, length)

**Disadvantages:**
- **External fragmentation:** As files are created and deleted, free space becomes scattered into small, non-contiguous gaps. A 100-block file might not fit even if 100+ blocks are free (just not contiguous).
- **Pre-allocation required:** You must know the file size at creation time, or reserve extra space (wasting it).
- Growing a file is painful — you may need to move the entire file.
- **This is essentially the RT-II approach** — used in early systems and some real-time/embedded file systems where files are fixed size and performance is paramount.

---

## Part 3: FAT — File Allocation Table

### Core Idea

Instead of storing files contiguously, allow files to be stored in **any available blocks** — even scattered across the disk. A global table (the FAT) tracks which block comes next for each file.

**Structure:**
- The disk is divided into fixed-size **clusters** (a cluster = one or more consecutive blocks)
- The FAT is an array with one entry per cluster
- Each FAT entry contains either:
  - The **next cluster number** in this file (if more data follows)
  - **EOF** (End of File — this is the last cluster)
  - **FREE** (this cluster is available)
  - **BAD** (this cluster is defective, do not use)

```
FAT array:
 Index:  0    1     2    3    4    5    6    7    8
 Value: [RSV][RSV][EOF][ 4 ][EOF][ 6 ][EOF][FREE][ 2 ]

Directory entry for "hello.txt":
  name = "hello.txt"
  start cluster = 8

Following the chain: 8 → 2 → EOF
File occupies clusters: 8, 2 (in that order)
```

**"Files are like linked lists"** — this is the essential mental model. Each cluster is a node; the FAT entry is the "next" pointer. The directory entry is the list head.

### `lseek` Performance on FAT

To implement `lseek(fd, offset, SEEK_SET)` — jump to an arbitrary byte position — you must:

1. Start at the first cluster (from the directory entry)
2. Walk the FAT chain one entry at a time
3. Stop when you've counted enough clusters to reach `offset`

This is **O(N)** in the number of clusters in the file. For a large file, seeking to the end requires reading through the entire FAT chain — very slow.

> **Exam trap:** FAT doesn't need to read the data blocks to seek — it only reads FAT entries. But FAT entries are in a global table that may span many disk sectors, and the chain walk is still linear.

### FAT Variants

| Version | Bits per entry | Max partition size | Max file size |
|---------|---------------|-------------------|---------------|
| FAT12 | 12 bits | ~32 MB | ~32 MB |
| FAT16 | 16 bits | ~2 GB | ~2 GB |
| FAT32 | 28 bits (!) | ~8 TB | 4 GB |
| exFAT | 32 bits | ~128 PB | ~128 PB |

> **Fact:** FAT32 uses 32-bit entries but only 28 bits are used for the cluster number; the top 4 bits are reserved. This limits FAT32 files to 4 GB maximum, which is why FAT32 can't handle large video files.

### FAT Limitations

- No per-file **metadata** (no owner, permissions, timestamps) in the traditional FAT design — this was added later as extensions
- No **hard links** (another filename that points to the same inode (same underlying file data) )
- Poor random access (O(N) seek)
- No **sparse files** (holes must be stored as zeros)
- Fragmentation (wasted or awkwardly arranged disk space that hurts storage efficiency or performance) is common as files grow and shrink

FAT persists today on USB drives, SD cards, and embedded systems because of its simplicity and cross-platform compatibility.

---

## Part 4: Inode-Based File Systems

### What Is an Inode?

An **inode** (index node) is a **fixed-size** data structure that describes one file. Every file on the system has exactly one inode. The inode stores all metadata and the locations of the file's data blocks.

Common sizes of an inode:

- ext2/ext3: usually 128 bytes
- ext4: usually 256 bytes (can be configured larger)

**Inode contents:**

| Field | Size | Description |
|-------|------|-------------|
| `size` | 8 bytes | File size in bytes |
| `uid` | 4 bytes | User ID of owner |
| `gid` | 4 bytes | Group ID |
| `mode` | 2 bytes | File type + permission bits (rwxrwxrwx) |
| `timestamps` | 12+ bytes | `atime` (last access), `mtime` (last modification), `ctime` (last inode change) |
| `link_count` | 2 bytes | Number of directory entries pointing to this inode (hard links) |
| `block pointers` | variable | Pointers to data blocks (see below) |

**Crucially, the inode does NOT store the filename.** Filenames live in directory entries. This separation is what enables hard links.

### Block Pointers: Direct and Indirect

Inodes use a **multi-level pointer scheme** to efficiently support both small and large files:

```
Inode block pointers:
  [0]  → data block 0        (direct)
  [1]  → data block 1        (direct)
  ...
  [9]  → data block 9        (direct)   ← 10 direct pointers
  [10] → indirect block      ← points to a block containing more block pointers
         └── [0] → data block 10
         └── [1] → data block 11
         └── ...
         └── [2047] → data block 2057
```

With a block size of **8 KiB** and **4-byte pointers**:
- Each indirect block holds `8192 / 4 = 2048` pointers
- 10 direct blocks = 10 × 8 KiB = **80 KiB** without indirection
- 1 indirect block = 2048 × 8 KiB = **16 MiB** additional reach
- For even larger files: double-indirect (pointer to block of indirect blocks) and triple-indirect

**Why inodes make `lseek` fast:**

To read byte at offset N:
1. Compute which block number (B = N / 8192) and offset within block
2. If B < 10: read `inode.ptr[B]` → done (1 disk read for the data)
3. If B ≥ 10: read the indirect block → read the data block (2 disk reads)

This is **O(1) for most files** (or O(2) for large files needing indirection). Compare to FAT's O(N).

---

## Part 5: Fragmentation Analysis

Two types of fragmentation matter for file systems:

| Type | Definition | Affected by |
|------|-----------|-------------|
| **Internal fragmentation** | Space wasted *inside* an allocated block | Block size; a 1-byte file still uses a full block |
| **External fragmentation** | Free space scattered in unusable small gaps | Contiguous allocation; FAT/inode avoid this |

**FAT and inode-based systems eliminate external fragmentation** (any free block can be used for any file). But **internal fragmentation** remains:

| File size | Wasted in last block | Indirect block overhead | Notes |
|-----------|---------------------|------------------------|-------|
| 1 byte | 8,191 bytes | 0 | Entire block allocated, 1 byte used |
| (10 × 8192) + 1 bytes = 81,921 bytes | 8,191 bytes | 8,192 bytes for indirect block | Needs 11th block → triggers indirect |

**Files can have holes (sparse files):**

A file can `lseek` past its current end and `write` there, leaving a gap. The kernel does **not** allocate blocks for the gap — the corresponding inode pointers are set to 0 (meaning "read as zeros"). This is a **sparse file**: a 1 TB file can consume nearly 0 disk space if it has only a few written bytes.

> **Exam trap:** If you `cp` a sparse file naively, the copy may be much larger (all the zeros are written explicitly). Tools like `rsync --sparse` and `cp --sparse` are aware of this.

---

## Part 6: Directories in Inode-Based Systems

### What Is a Directory?

A **directory is just a file** with a special type (`d`). Its data blocks contain **directory entries** — a list of (name, inode number) pairs. The OS interprets these entries to implement path lookup.

### Directory Entry Format (Linux ext3 v2)

Each entry in a directory block has this layout:

```
┌─────────────┬───────────┬─────────────┬────────────┬─────────────┐
│  inode #    │  rec_len  │  name_len   │  file_type │  name[]     │
│  (32 bits)  │  (16 bits)│  (8 bits)   │  (8 bits)  │  (variable) │
└─────────────┴───────────┴─────────────┴────────────┴─────────────┘
```

| Field | Purpose |
|-------|---------|
| `inode #` | Which inode this entry refers to |
| `rec_len` | Length of **this directory entry record** (to find the next entry) |
| `name_len` | Length of the filename string |
| `file_type` | Type of the file (regular, directory, symlink, etc.) |
| `name[]` | The filename, **not** null-terminated in the record |

**`file_type` is redundant** — the inode already stores the file type. It's cached here to avoid a second disk read just to find out whether a name refers to a file or directory (e.g., for `ls -l`). This is a **performance optimization** — caching an immutable part of the inode in the directory entry.

### How Deletion Works (Why "Ghosts" Appear)

Deleting a file from a directory does **not** compact the directory block. Instead:

1. Find the entry to delete
2. Find the **previous** entry
3. Add the deleted entry's length to the previous entry's `rec_len`

```
Before delete "foo":
  [entry: "bar", rec_len=16] [entry: "foo", rec_len=20] [entry: "baz", rec_len=16]

After delete "foo":
  [entry: "bar", rec_len=36]                             [entry: "baz", rec_len=16]
  (bar's rec_len now skips over the space where "foo" was)
```

The "ghost" of "foo" is still physically in the disk block — just unreachable by traversal. This means:

- **Deletion is O(1)** — no data movement needed
- Old deleted entries remain on disk (security implication — forensics can recover them)
- A directory block can contain "the ghost of 400 directories that are not there anymore"

---

## Part 7: Path Resolution — `namei`

**`namei`** is the kernel function that resolves a pathname string to an inode number. It works component-by-component:

```
Pathname: "/home/alice/notes.txt"

namei steps:
  1. Start at root inode (inode #2, always)
  2. Look up "home" in root directory → get inode #A
  3. Look up "alice" in directory A → get inode #B
  4. Look up "notes.txt" in directory B → get inode #C
  5. Return inode #C
```

**Special directory entries:**
- **`.`** — entry for "this directory" (always present; inode = this directory's inode)
- **`..`** — entry for "parent directory" (always present; root's `..` points to itself)

These are real directory entries — they take up space in the directory block like any other entry.

**Relative vs. absolute paths:**
- **Absolute** (starts with `/`): `namei` starts from the root inode
- **Relative** (no leading `/`): `namei` starts from the process's **current working directory** (stored in the process's file descriptor table)

---

## Part 8: Hard Links

A **hard link** is simply an additional directory entry pointing to an **existing inode**. Both entries point to the same inode, the same data — they are equal peers.

```
Directory /home/alice:
  "notes.txt" → inode #C

Directory /home/bob:
  "alicenotes.txt" → inode #C   ← same inode!
```

The inode has a **link count** field tracking how many directory entries point to it.

```bash
mv d/f e/g       # creates a directory entry "e/g" pointing to f's inode
                 # then removes the directory entry "d/f"
                 # link count stays the same (add 1, subtract 1)

rm d/f           # removes directory entry "d/f"
                 # decrements link count
                 # if link count reaches 0 → free the inode and data blocks
```

**Link count = 0 behavior:**

When `link_count` reaches 0, the kernel knows no directory points to this file anymore — **but it doesn't free it immediately if any process has the file open.** The file survives until both conditions are met:
- `link_count == 0` (no directory entries)
- No open file descriptors referring to it

This explains a useful Unix trick: create a file, open it, then `rm` it. The file disappears from the directory but the data remains accessible through the open file descriptor until you close it.

> **Exam trap:** `rm` does not mean "delete data." It means "remove a directory entry" (unlink). Data is freed only when link count drops to 0 AND no open FDs remain.

### Hard Links Cannot Point to Directories

Allowing hard links to directories would create cycles in the directory tree, which would break recursive operations (`find`, `du`, `rm -r`) — they'd loop forever.

```
PROHIBITED: hard link to a directory
  mkdir foo
  ln foo bar    ← FAILS: "hard link to directory not allowed"
```

The kernel enforces this restriction. (The only exceptions are `.` and `..`, which are managed exclusively by the kernel.)

---

## Part 9: Symbolic Links

A **symbolic link** (symlink) is a file of type `symlink` whose data block contains a **pathname string**. It is a reference by name, not by inode number.

```
Inode #D: type=symlink, data="/home/alice/notes.txt"

ls -la:
  mylink -> /home/alice/notes.txt
```

**How the kernel handles symlinks:**

When `namei` encounters a directory entry that points to a symlink inode, it:
1. Reads the pathname stored in the symlink's data block
2. Starts `namei` again from scratch with the new pathname
3. (If the new path is relative, it's resolved relative to the symlink's directory)

**Key properties of symlinks:**

| Property | Hard Link | Symbolic Link |
|----------|-----------|---------------|
| Points to | Inode (number) | Pathname (string) |
| Can cross filesystems? | No — inode numbers are per-filesystem | **Yes** |
| Can link to directories? | No | **Yes** |
| Survives target deletion? | Yes (same inode) | No — becomes **dangling** |
| Stores extra disk space? | No (just a dir entry) | Yes (small data block) |
| Immutable? | N/A | **Yes** — cannot change the target path |

**Dangling symlinks:** A symlink whose target path doesn't exist (the target was renamed or deleted). Accessing a dangling symlink gives `ENOENT` (no such file or directory).

```bash
ln -s /tmp/ghost mylink    # create symlink to nonexistent path
ls mylink                  # shows the link
cat mylink                 # ENOENT: /tmp/ghost does not exist
```

**Symlinks are immutable** — you cannot change where a symlink points without deleting it and recreating it. This is by design: the pathname string in the data block is written once at creation.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| Contiguous allocation | O(1) access, but external fragmentation and can't grow files |
| FAT structure | Global array; entry = next cluster or EOF; files = linked lists |
| FAT `lseek` | O(N) — must walk entire chain to reach arbitrary byte offset |
| Inode | Fixed-size struct; stores metadata + block pointers; no filename |
| Direct vs. indirect blocks | First 10 direct; then single-indirect (2048 more blocks); O(1) random access |
| Block size | 8 KiB → indirect block holds 2048 pointers |
| Sparse files | Inode pointers set to 0 for holes; holes read as zeros; no disk blocks allocated |
| Internal fragmentation | Last block always fully allocated; 1-byte file wastes 8,191 bytes |
| Directory = special file | Data blocks contain (inode#, rec_len, name_len, type, name) entries |
| `rec_len` trick | Deletion extends previous entry's rec_len; O(1) delete; leaves ghost entries |
| `file_type` in dir entry | Redundant with inode; cached for performance; immutable inode field |
| `namei` | Walks path component by component; `.` = self, `..` = parent |
| Hard link | New directory entry → same inode; increments link_count |
| `rm` / `unlink` | Removes directory entry, decrements link_count; frees data only when count=0 AND no open FDs |
| Hard link restriction | No hard links to directories (would create cycles) |
| Symbolic link | File of type symlink; data block = target pathname string |
| Symlink vs hard link | Symlink: by name, cross-FS, can link dirs, can dangle; Hard link: by inode, same FS only, no dirs |
| Dangling symlink | Target path doesn't exist; access gives ENOENT |
