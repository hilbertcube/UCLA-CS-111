# Lecture 13 — File System Layers, Robustness, and Reliable Writes

---

## Overview

This lecture covers two major topics:

1. **The layered architecture of a file system** — how the OS abstracts from physical disk sectors up to the familiar `/home/alice/notes.txt` interface
2. **File system robustness** — what it means for a file system to survive crashes, and the engineering techniques used to make writes safe

---

## Part 1: The File System Layered Architecture

Modern file systems are not monolithic — they are built as a **stack of layers**, each providing a higher-level abstraction than the one below. Understanding the layers helps you reason about where a given feature lives and why.

### The Full Stack (Top to Bottom)

```
Application (user space)
    │  open("/home/alice/notes.txt", O_RDWR)
    ▼
─────────────────────────────────────────────
LAYER 6: Symbolic Links
  - Expand symlinks during path resolution (namei)
  - Transparent to the application
─────────────────────────────────────────────
LAYER 5: File Names / Path Resolution (namei)
  - Convert "/home/alice/notes.txt" → inode number
  - Component-by-component lookup through directory entries
─────────────────────────────────────────────
LAYER 4: File Name Components / Directories
  - Map name strings to inode numbers
  - Entries: (name, inode#) pairs stored in directory files
─────────────────────────────────────────────
LAYER 3: Inodes (Files + Metadata)
  - Data structure with size, uid, gid, permissions, timestamps,
    block pointers, link count
  - Lives partly on disk (stable), partly in memory (inode cache)
─────────────────────────────────────────────
LAYER 2: File Systems + Mount Table
  - Multiple file systems glued together into one namespace
  - Mount table: maps mount points to file system instances
─────────────────────────────────────────────
LAYER 1: Block Layer
  - Fixed-size blocks (typically 8 KiB in Linux ext)
  - Hides geometry differences between disk types
─────────────────────────────────────────────
LAYER 0: Physical Block Device
  - HDD: 512-byte sectors + rotational geometry
  - SSD/NVMe: 4 KiB pages + 8 MiB erase blocks (flash cells)
─────────────────────────────────────────────
```

**Why layers?** Each layer can be swapped independently. Linux's VFS (Virtual File System) layer lets different file system types (ext4, btrfs, NTFS, NFS) plug in at Layer 3/4, all sharing the same Layer 5/6 path resolution code.

---

## Part 2: Partitions and the Mount Table

### Partitions

A **partition** is a contiguous region of a physical disk, treated as an independent logical block device. A single physical disk is typically divided into multiple partitions:

```
Physical disk:
┌──────────┬──────────┬──────────┬──────────┬──────────┐
│   boot   │   /usr   │  swap    │  /home   │   /tmp   │
│ (50 MiB) │ (20 GiB) │  (8 GiB) │ (100 GiB)│ (10 GiB) │
└──────────┴──────────┴──────────┴──────────┴──────────┘
  sda1         sda2      sda3       sda4        sda5
```

Each partition gets its own file system, its own inode table, its own block allocation. Inode numbers are **local to a partition** — inode #5 in `/usr` is different from inode #5 in `/home`.

**Common partitions:**
- **boot** — contains the kernel and bootloader
- **/usr** — system binaries, libraries
- **swap** — kernel uses this as backing store for virtual memory page eviction
- **/home** — user home directories

### The Mount Table

The **mount table** is a kernel data structure that maps **mount points** (directories in the unified namespace) to **file system instances** (partitions or network shares).

```bash
$ mount        # print current mount table
/dev/sda2 on / type ext4 (rw,relatime)
/dev/sda4 on /home type ext4 (rw,relatime)
/dev/sdb1 on /mnt/usb type vfat (rw,nosuid)
tmpfs on /tmp type tmpfs (rw,nosuid,nodev)
```

Reading this: `/dev/sda2` is a block device (a partition). It is mounted **at** `/usr/share` — meaning when you access `/usr/share/anything`, the kernel redirects the lookup to the file system on `/dev/sda2`.

**How cross-filesystem path resolution works:**

```
namei("/home/alice/notes.txt"):
  1. Start at root filesystem (/)
  2. Look up "home" in root → it's a mount point!
  3. Switch to the filesystem mounted at /home
  4. Look up "alice" in that filesystem's root
  5. Look up "notes.txt" in alice's directory
  6. Return the inode from the /home filesystem
```

The mount table is what stitches multiple filesystems into one seamless tree. The process is completely transparent to applications.

**`tmpfs`** — a filesystem stored entirely in RAM (backed by swap if needed). Extremely fast. Used for `/tmp` on many systems. Contents are lost on reboot. Inode is "partly in data storage and partly in memory" — for tmpfs, the "data storage" part is RAM/swap.

---

## Part 3: Robustness Terminology

Before discussing robustness techniques, establish precise definitions — these terms are commonly confused on exams:

| Term | Definition | Example |
|------|-----------|---------|
| **Error** | A **developer mistake** — a bug in code or design | Programmer forgets to check return value of `write()` |
| **Fault** | A **defect** in the system that *may* cause a failure (latent problem) | A cosmic ray flips a bit in RAM; the flipped value hasn't been used yet |
| **Failure** | The system **behaves wrongly** in an observable, externally-visible way | The file system returns corrupt data; the system crashes |

**The relationship:** An error introduces a fault. A fault may remain dormant. Eventually, a fault causes a failure. Robustness engineering tries to prevent faults from causing failures.

> **Exam trap:** "Error" in this context means a human mistake, not an OS `errno` value. The terms are carefully distinct.

---

## Part 4: File System Robustness Goals

A robust file system must survive the real-world failure modes of storage hardware:

### The Three Goals

| Goal | Meaning |
|------|---------|
| **Durability** | Data that has been successfully written survives hardware failures — power outages, crashes, bad blocks |
| **Atomicity** | A multi-step change either completes fully or not at all — no partial writes visible after recovery |
| **Performance** | Robustness mechanisms must not make the file system unacceptably slow |

These goals are in tension. The most durable approach (write three copies, sync after every write) is also the slowest. Real systems make explicit tradeoffs (journaling, write-ahead logs, copy-on-write) to balance all three.

### Real Failure Modes

The file system must handle:
- **Power failure mid-write** — a block being written may contain arbitrary data (partial write, half-written sector)
- **Spontaneous data decay** — bit rot: magnetic domains flip on HDDs, charge leaks on SSDs
- **Corrupted neighboring blocks** — a write to one block can corrupt an adjacent block (write amplification, bad firmware)

---

## Part 5: The Reliable Write Problem — A Motivating Example

### Setup: A Simple Text Editor

- 80 KiB of text lives in memory (one buffer)
- We want to **save** it to disk
- **Problem:** if power fails mid-write, the on-disk file may be garbage

**Assumption (Lampson-Sturgis, see Part 7):** During a write, the target block may contain **arbitrary data** — not the old value, not the new value, just garbage. We cannot assume a partial write is safe.

### Attempt 1: Single Copy (No Redundancy)

```
Save:  write 80 KiB to disk
```

**Problem:** If power fails during the write, the file on disk is corrupted. We've **destroyed the only copy**.

### Attempt 2: Commit Record

**Idea:** Keep two copies of the data. Write the new version to a second location, then write a **commit record byte** to atomically signal "use the new version."

```
Location 0:  [old data A]
Location 1:  [new data B]  ← write B here first
Commit byte: [0 = use A, 1 = use B]
```

**Protocol:**
1. Write new data B to Location 1
2. Set commit byte = 1 (atomically signals "B is ready")
3. On recovery: check commit byte → if 1, use Location 1; if 0, use Location 0

**Why this still fails:**
- The "commit byte" itself is a write to disk — what if power fails between writing B and writing the commit byte?
- More insidiously: the commit byte might be in a disk write buffer. Even after the OS thinks it wrote the byte, the hardware might not have flushed it. After reboot, the byte might be 0 again.
- Commit record wastes space: you need an extra ~8 KiB block just to store one meaningful byte.

The core problem: **there is no such thing as an atomic single-byte disk write** unless the hardware explicitly guarantees it.

---

## Part 6: Triple Redundancy — A Robust Solution

### Core Idea

Store **three copies** of the data (blocks 0, 1, 2). Always write them in order: 0 first, then 1, then 2. On recovery, use **majority voting** among the three copies.

**Write protocol:**

```
To update from value A → value B:
  1. Write B to block 0
  2. Write B to block 1
  3. Write B to block 2
```

**What happens if power fails?**

| State at crash | Block 0 | Block 1 | Block 2 | Recovery |
|----------------|---------|---------|---------|----------|
| Before write starts | A | A | A | A (unanimous) |
| During write to block 0 | **?** | A | A | A wins (2 votes vs. 1 garbage) |
| After write to block 0 | B | A | A | A wins (2 votes vs. 1 new) |
| During write to block 1 | B | **?** | A | B wins (B written first, A not yet overwritten — tie broken by recency) |
| After write to block 1 | B | B | A | B wins (2 votes) |
| During write to block 2 | B | B | **?** | B wins (2 votes) |
| After all writes | B | B | B | B (unanimous) |

**Why this works:** At any moment, only **one** write is in progress. That block may be corrupted (**?**). The other two blocks are always valid and consistent (they agree). Two consistent blocks always form a majority of 2-out-of-3, and they agree on the winner.

**Detecting garbage (?): checksums.** Each block stores a checksum of its data. On read, the OS verifies the checksum. If it doesn't match, the block is marked as **?** (garbage) and excluded from the vote.

### Transition at Time Step 3 (the Tricky Case)

At the point where block 0 = B, block 1 = ?, block 2 = A:
- Block 1 is detected as garbage (checksum fails) → excluded
- We have B (block 0) vs. A (block 2) — a tie among valid blocks
- **Tiebreaker rule:** Since we write sequentially (0 before 1 before 2), block 0 having B means at least one write completed to B. Block 0 is the "most recently completed write." Use B.

> This requires knowing the write ordering, which is encoded in the algorithm itself (always write 0, 1, 2 in that sequence).

---

## Part 7: Hardware Atomicity Guarantees — NVMe

The triple-redundancy scheme relies on the hardware's ability to atomically write a single block. Modern NVMe SSDs provide explicit guarantees:

**NVMe Atomic Write Specifications:**

| Acronym | Full Name | Meaning |
|---------|-----------|---------|
| **AWUN** | Atomic Write Unit Normal | Maximum size guaranteed to be atomic under normal operation. Typically 4 KiB up to 256 GiB (device-dependent). A write of this size or smaller either fully succeeds or fails — no partial writes in normal operation. |
| **AWUPF** | Atomic Write Unit Power Fail | Maximum size guaranteed to be atomic **even if power fails during the write**. Typically 4 KiB up to 512 KiB. This is the critical guarantee for file system robustness. |

**What these mean in practice:**

- If your block size ≤ AWUPF (typically 4 KiB), a single block write is **power-fail atomic** — you'll always get either the old data or the new data, never garbage.
- This is why many file systems use 4 KiB blocks as the minimum (matching AWUPF).
- AWUN > AWUPF: the device can do larger atomic writes during normal operation, but power-fail atomicity only extends to AWUPF.

> **Implication for file systems:** If AWUPF ≥ block size, you don't need triple redundancy for individual block writes — the hardware handles it. But multi-block atomic operations (e.g., updating an inode + a data block + a directory entry together) still require higher-level mechanisms like journaling.

---

## Part 8: Lampson-Sturgis Assumptions

The **Lampson-Sturgis assumptions** are the formal model of what we can and cannot rely on from storage hardware. They define the "worst case" that a robust file system must handle:

| Type | Assumption | Implication |
|------|-----------|-------------|
| **Bad** | Storage writes may **fail** and/or **corrupt neighboring blocks** | Don't assume a write to block N is clean; check it; use checksums |
| **Good** | A later **read can detect** a bad (corrupted) block | Checksums work; the hardware (or FS layer) can tell you a block is bad |
| **Bad** | Storage can **spontaneously decay** | Bit rot is real; data must be periodically scrubbed and verified |
| **Good** | Errors are **rare** | We design for rare failures; we don't need to replicate everything 100× |
| **Good** | We have **time to recover** | After a crash, the system can spend time (minutes if needed) on recovery |

**These assumptions justify the design decisions:**

1. Because errors are **rare**, storing 3 copies (instead of 100) is sufficient — the probability of all 3 failing simultaneously is negligible.
2. Because a read **can detect** corruption, we can use checksums to identify the garbage block in triple redundancy.
3. Because we have **time to recover**, journaling (Lecture 14) can do expensive work at startup.
4. Because writes may **corrupt neighbors**, checksums must cover entire blocks, not just the written area.

> **Exam trap:** These are *assumptions* — they describe what the designers believed storage hardware would do. If a real system violates these (e.g., a bug makes errors non-detectable), the robustness mechanism fails. These assumptions are not guarantees — they're the basis on which guarantees are built.

---

## Part 9: Connection to Journaling (Preview)

The triple-redundancy approach works for a single block, but real file system operations involve **multiple blocks atomically** (e.g., updating a file requires writing a data block + the inode + the directory entry — three separate writes that must all succeed or all fail).

Triple redundancy doesn't help here — you can't vote across three semantically different blocks. This motivates **journaling** (Lecture 14):

- Write a log of intended changes first (the journal)
- Then apply the changes
- On crash recovery: either complete the logged changes (redo) or undo them (undo log)

This provides multi-block atomicity on top of the per-block atomicity hardware gives you.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| File system layers | Physical device → block layer → file systems/mount table → inodes → directories → file names → symlinks |
| Mount table | Maps mount points (directories) to file system instances (partitions); makes multiple FSes look like one tree |
| `mount` command | Prints current mount table; shows device → path mappings |
| Error vs. fault vs. failure | Error = developer mistake; fault = latent defect; failure = observable wrong behavior |
| Three FS robustness goals | Durability (survive hardware failure), Atomicity (all-or-nothing), Performance |
| Commit record | Write new data + a commit bit to signal "done"; fails because commit bit write itself isn't atomic |
| Triple redundancy | 3 copies, written sequentially (0→1→2); majority vote recovers correct value |
| Why triple redundancy works | Only 1 block written at a time → at most 1 garbage block → other 2 always agree |
| Checksum role | Detects the garbage (?) block; required for majority vote to work |
| AWUN | NVMe: largest atomic write under normal operation (can be very large) |
| AWUPF | NVVM: largest atomic write guaranteed even on power failure (typically ≤ 512 KiB) — the critical FS guarantee |
| Lampson-Sturgis (bad) | Writes can fail or corrupt neighbors; storage can spontaneously decay |
| Lampson-Sturgis (good) | Bad blocks detectable (checksums work); errors are rare; recovery time is available |
| Sparse files | Holes in files: inode pointers = 0 for missing blocks; reads return zeros; no disk space used |
| Inode: partly on disk/memory | Disk = persistent (block pointers, permissions); memory = inode cache for hot files |
| Journaling preview | Multi-block atomicity beyond what hardware gives; write intent log first, apply second |
