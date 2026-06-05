# Lecture 14 — File System Robustness: Journaling & Transactions

---

## Overview

Two main topics:

1. **Commit record** — how we mark a transaction as "done" atomically
2. **Journaling** — how we log changes before/after so we can recover after failure

---

## The Core Idea

Supposed we want to make a change to the file system that maybe involve changing the file (e.g., updating the data block, the inode, and the directory entry — three separate disk writes).

**We put in our journal the plan that we plan to do** before doing it. This way, if power fails mid-operation, we can consult the journal on recovery and finish (or undo) the operation cleanly.

> **Key rule: Recovery must be idempotent.**
>
> An idempotent operation produces the same result whether applied once or many times. Recovery may replay operations (e.g., after crashing mid-recovery), so every recovery step must be safe to repeat.

**Cell storage** in this model might be volatile (does not survive reboot) — data in write buffers or caches may be lost on power loss, which is why ordering constraints matter.

---

## Logging Protocol

Two strategies, differing in *what* they log and *when*:

---

### Write-Ahead Log (WAL / Redo Log)

Write your *intentions* (new values) to the journal **before** applying them.

```
1. BEGIN
2. Write plans (new values) to journal
3. COMMIT   ← point of no return; this single write is atomic
4. Copy data from journal to actual cells (apply)
5. DONE
```

**Recovery:** Scan journal *forward*. For each **committed** transaction → **redo** it. For each **non-committed** transaction → **ignore** it (discard).

---

### Write-Behind Log (Undo Log)

Write the *old values* (what you're about to destroy) to the journal **before** overwriting them.

```
1. BEGIN
2. Copy old values to journal   ← save what we're about to overwrite
3. Install all new values (note: typo in original — "cinstall" → "install")
4. COMMIT
5. DONE
```

**Recovery from failure** in a write-behind log:

- **Scan backwards through the journal**
- For each **non-committed** transaction:
  - Copy old data back to cells (undo)
- For each **committed** transaction → leave alone (already correct)

> Scanning **backwards** is important because later log entries may depend on earlier ones. Undoing in reverse order ensures consistency.

---

## Patterns for Commits

```
BEGIN
  │
  ▼
Pre-commit phase
  │   ← We can be doing quite a bit of stuff here, but none of that
  │     actually counts yet (not durable, not visible to others by default)
  │   ← Reads/writes go to the journal; not yet applied to stable cells
  ▼
COMMIT  (or ABORT)
  │   ← We're happy with this transaction; we know that even if we
  │     lose power after this point, the transaction survives
  │   ← Sometimes, during pre-commit, if we don't want to commit
  │     for some reason, we can ABORT instead — cleanly undone
  ▼
Post-commit phase
  │   ← Still may have some work to do that is necessary for efficiency
  │     (e.g., flushing caches, propagating changes). Crash here is OK
  │     because we can redo from the committed journal entry.
  ▼
END
```

**Abort:** At any point before COMMIT, we can decide to abort. The system restores the previous state (using the journal) as if the transaction never happened.

---

## Complication: Visibility During Pre-Commit

Another complication that can make things more interesting:

Once we have a complicated enough transaction (like **move a directory and its entire content from one directory to another**) — if our transaction is complicated but we still want it to be atomic for efficiency reasons, should we let the user see what's in the file?

The question: **access the file system during the pre-commit phase** — let other actors look into an uncommitted transaction?

- There can be an advantage (better performance, no waiting), **but whenever we let someone do that, there's a catch: the other actors must know when an abort occurs.**
- The rule is: **if the original transaction aborts, the pre-commit of the other actor must abort as well** (both transactions fail) — this is called a **cascading abort**.

### Option B: Compensating Action

Instead of cascading aborts, we can design a **compensating action** — an operation that semantically reverses the effect of what was done, so we don't need to abort the other transaction.

- Example: if transaction T1 moved a file (uncommitted) and T2 already read from its new location, the compensating action puts the file back — T2's result is still valid.
- More complex to implement, but avoids the domino effect of cascading aborts.

> **Database isolation levels** formalize this:
>
> - **READ UNCOMMITTED** — can see other transactions' uncommitted data (cascading aborts possible)
> - **READ COMMITTED** — only see committed data (no cascading aborts; the standard safe default)

---

## IO Scheduling: Interaction Between IO Scheduling + Robustness

Suppose we want to write in this order (journal commit record first, then data):

```
Disk layout:
|       | 1 (commit record) |     | 3 (data) |     | 2 (data) |         |
           ^-- MUST hit disk before 2 and 3
```

**The problem:** The underlying hardware might try to do the commit record **first** (or in any order it wants) when it does IO scheduling for performance (e.g., elevator algorithm reorders writes to minimize seek time). If the commit record lands before the journal entries, and we crash, we think the transaction committed but the journal is incomplete.

**Naive solution:** Don't do IO scheduling at all — but it's gonna kill performance.

**Better approach:** When we communicate (when we write a block), we don't simply hand over (data + location). We also need to **tell the underlying system about the ordering constraint** — e.g., "location Y must be written *after* location X." This lets the underlying system still do optimizations, but we can now tell it when *not* to do the optimization.

> This sort of thing is done routinely by a **flash driver (SSD)**. SSDs do internal wear-leveling (moving data to different physical cells for longevity) but maintain ordering guarantees in firmware.
>
> In Linux, this is enforced via:
>
> - `fsync()` / `fdatasync()` — flush pending writes for a file to stable storage
> - **Write barriers** — a special IO command that prevents the scheduler from reordering writes across the barrier point

---

## Secure File Deletion: `shred` vs `rm`

There's a command on Linux called `shred`:

```bash
shred mysecret.txt   # securely erase
```

### Difference Between `shred` and `rm`

- **`rm` (remove)** is a fast, standard command that **deletes file references** (removes the directory entry, marks blocks as free), leaving the actual data on disk untouched and recoverable until those blocks are overwritten.
- **`shred`** securely deletes files by **overwriting them multiple times** with random bit patterns to make data recovery nearly impossible.
  - Default: 3 passes of random data, then removes the file
  - `rm` is best for daily use; `shred` is best for sensitive data on HDDs

**What `shred` does:** Overwrite the data with a random bit pattern **3 times**, then remove the file.

---

### Why `shred` Doesn't Work With Journaling File Systems

However, the idea of `shred` is **not gonna work with the file system that we talk about today** — because of journaling, the **old data still sits in the journal**. Even if shred overwrites the file's data blocks, the journal (in a separate area of disk) may contain the original data as an undo-log entry. `shred` doesn't know about or touch the journal.

**Other reasons shred fails on modern hardware:**

| Scenario | Why shred fails |
| -------- | --------------- |
| Journaling file systems | Old data survives in the journal |
| SSDs / Flash | Wear leveling may keep old physical copies at different cells |
| RAID arrays | Parity drives may retain copies |
| Network/cloud storage | Remote replicas exist |
| Filesystem snapshots | Previous snapshots preserve old data |

> **Bottom line: Don't bother with `shred` anymore.**

---

### So What Do We Do?

- **If it's on SEASNET:** You're done — the admins handle it; don't worry about it.
- **If it's on your own machine and you want to keep things really secret:**
  - **Destroy the physical device**, or **demagnetize it** (degaussing) if it's a hard drive — but note degaussing only works on magnetic HDDs, not SSDs.
  - **Better (more practical) approach — Encryption:**
    - Encrypt the data, and the **key is kept somewhere on the drive**, accessible only by a passphrase.
    - To "destroy" the data: simply **forget (or delete) the passphrase/key**.
    - Without the key, the ciphertext on disk is computationally useless — even with physical access to the drive.
    - This is called **cryptographic erasure** (or *crypto-shredding*) — instant, provably secure, no physical destruction needed.
    - Tools: **LUKS** (Linux), **BitLocker** (Windows), **FileVault** (macOS)

---

## Not Yet Covered (Coming Soon)

- **Failures due to drives going bad** — There's a technique called **RAID** to deal with bad drives (discussed later):
  - **RAID 0 (Striping):** Performance, no redundancy
  - **RAID 1 (Mirroring):** Full copy on two drives; survives one failure
  - **RAID 5 (Striping + Parity):** Distributed parity; survives one drive failure
  - **RAID 6:** Two parity blocks; survives two simultaneous failures

- **Network file systems** — Google Drive or NFS (networked): having things remotely can introduce some complexity (network partitions, consistency across replicas, partial failures)

- **Multi-partition file systems** — A logical file system spanning multiple physical partitions or devices (e.g., LVM, ZFS); adds complexity to journaling and atomic commits

---

## Summary: Key Exam Points

| Concept | What to Know |
| ------- | ------------ |
| Journaling purpose | Make multi-step FS changes atomic; survive crashes |
| Write-ahead log | Write new values to journal first; recover by **redoing** committed txns |
| Write-behind log | Write old values to journal first; recover by **undoing** non-committed txns (scan **backwards**) |
| Idempotency | Recovery operations must be safely repeatable |
| Cell storage volatility | Write buffers may not survive reboot; ordering matters |
| Commit record | Single atomic write; the point of no return |
| Pre-commit visibility | Letting others read uncommitted data → cascading aborts on rollback |
| Cascading abort | If T1 aborts, every txn that read T1's uncommitted data must also abort |
| Compensating action | Alternative to cascading abort; semantically reverse the side effect |
| IO ordering problem | IO scheduler can reorder writes, breaking journal safety |
| Solution | Expose ordering constraints to IO subsystem; write barriers, fsync |
| `shred` limitations | Fails on journaling FSes (old data in journal), SSDs (wear leveling), RAID, snapshots |
| Secure deletion | Use full-disk encryption from the start; crypto-shredding = delete the key |


---

# Unreliable Software & Memory Protection

If we have unreliable (or malicious) software, what can go wrong?

- **Bad IO:** It can try to access a device in a bad way — e.g., go to the disk drive and ask it to delete everything. We already covered this: **privilege separation** (kernel mode vs. user mode) prevents it. Instructions like `outb` / `inb` (raw IO port access) are **privileged** — only the kernel can execute them. User-space code that tries gets a trap.

- **But privilege alone isn't enough** — even with ordinary (unprivileged) instructions that everybody can use, applications can still crash the kernel. The culprit: **load / store / mov instructions**. Without memory isolation, a user program can do `mov [kernel_address], 0` and corrupt kernel data structures directly.
  - The question then becomes: what about a **bad kernel**? If kernel code itself is buggy, it can corrupt itself — which is a harder problem (microkernels, formal verification, etc.)

- **Solution: virtual memory** — a standard technique that isolates each process's view of memory from every other process and from the kernel.

- **Still not enough — infinite loops:** Even with IO protection and memory isolation, a bad thread can spin in an infinite (or near-infinite) loop and starve every other process. The fix: **clock interrupts**. The hardware timer fires periodically, preempting the spinning thread and handing control back to the kernel scheduler, which can decide what to do (deschedule it, kill it, etc.).

---

## Virtual Memory — Three Ideas

### Idea 1: Base + Bound (Base + Limit)

Each process gets two hardware registers: a **base** address and a **bound** (limit).

```
Physical memory:
[ OS ][ Process A region ][ Process B region ][ ... ]
       ^base_A  bound_A^
```

On every memory access the hardware checks:
- If `address < base` or `address >= base + bound` → **trap** (segfault)
- Otherwise, translate: `physical = virtual + base`

**Advantages:** Simple; cheap to implement.

**Disadvantages:**

- No sharing between processes (each gets one contiguous block)
- Hard to grow the region (must move the whole thing or leave padding)
- No fine-grained permissions (no read-only pages, etc.)

---

### Idea 2: Segmented Memory

Contains 2 parts: a **segment number** (16 bits) and an **offset** (48 bits), together forming a 64-bit virtual address.

```
|   16 bits   |            48 bits             |
  segment #              offset
```

Each segment number indexes into a **segment table** (held by the OS), which stores the base, limit, and **permissions (read / write / execute)** for that segment. The hardware checks permissions on every access.

> Segmentation is supported by Intel and AMD — x86 segment registers (CS, DS, SS, ES, FS, GS) are 16-bit selectors into the Global Descriptor Table (GDT).

**What's wrong with this approach?**

- Suppose you use segment #27 and allocate it 1 MB. Later you realize you need 2 MB — but that space might already be in use by another segment. **Growing a segment is painful** — you may need to move it entirely, and there may be no room (external fragmentation).
- Segments vary in size → **external fragmentation** builds up in physical memory over time.
- This is why modern OSes moved away from segmentation toward paging.

---

### Idea 3: Paging (Virtual Memory)

- Each process has a **linear address space** (as if it owns all of memory).
- Memory is divided into fixed-size chunks called **pages**; each page is individually allocated and mapped.
- Hardware maintains a **page table**: a data structure that maps virtual page numbers → physical frame numbers.
- We can think of the page table as being indexed by the virtual page number.

```
Virtual address:  [ virtual page number | offset ]
                         ↓ (page table lookup)
Physical address: [ physical frame number | offset ]
```

**For x86-64:** page size = **4 KiB = 2^12 bytes** → offset field is **12 bits**

- Each page table entry (PTE) contains the physical frame address, plus bits for: present/valid, dirty, accessed, read/write, user/supervisor, etc.
- **Problem:** We may have a huge virtual address space, but real physical memory might not be there for all of it. When a process accesses a virtual page with no backing physical frame → **page fault**.
- Also: a single flat page table for a 48-bit address space would need 2^36 entries — way too large. So a flat page table won't work.

---

### How It Actually Works: Multi-Level Page Table

The idea: use a **tree of page tables** so that only the portions of the address space actually in use need table entries.

For **x86-64 with 5-level paging** (Linux on modern CPUs):

```
|  unused |  PGD  |  P4D  |  PUD  |  PMD  |  PTE  |  offset  |
 63      56      47      38      29      20      11           0
```

> **Fact correction:** The level labeled `PWD` in the original notes is actually **PMD** (Page Middle Directory). The `?` above is **20**. The hierarchy is:
> `PGD → P4D → PUD → PMD → PTE`

Each level is a 9-bit index (2^9 = 512 entries per table), and the offset is 12 bits. Total: 9×5 + 12 = 57 bits of addressable virtual space.

| Level | Full Name | Bits |
| ----- | --------- | ---- |
| PGD | Page Global Directory | 56–48 |
| P4D | Page 4th-level Directory | 47–39 |
| PUD | Page Upper Directory | 38–30 |
| PMD | Page Middle Directory | 29–21 |
| PTE | Page Table Entry | 20–12 |
| offset | byte within page | 11–0 |

> **TLB (Translation Lookaside Buffer):** Walking 5 levels of page table on every memory access would be catastrophically slow (5 extra memory reads per access). The hardware caches recent virtual→physical translations in a small, fast hardware structure called the **TLB**. On a TLB hit, translation is done in hardware with ~zero overhead. On a TLB miss, the hardware **page table walker** traverses the tree and fills the TLB. On a context switch, the TLB must be flushed (or tagged with ASIDs — Address Space IDs — to avoid flushing).

---

### Tagged Pointers

In a 64-bit address space, x86-64 only uses 57 bits for addresses (5-level paging). The remaining upper bits are normally sign-extended and must be canonical — but this wastes useful bits.

**Tagged pointers** repurpose those unused high bits to store metadata (e.g., memory tags for safety, garbage collector bits, type information).

- **LAM — Linear Address Masking (Intel):** masks off the high bits before using the address for translation, so software can use them freely.
- **UAI — Upper Address Ignore (AMD):** AMD's equivalent feature — the CPU ignores the upper non-canonical bits during address translation.

> **Fact correction:** The original notes said "UAM (AMD)" but the correct AMD term is **UAI (Upper Address Ignore)**, not UAM.

---

### Huge Pages

**Another complication:** For large computations (big arrays, scientific workloads, databases), standard 4 KiB pages create a lot of overhead — many TLB entries needed, many page faults, large page tables.

#### Intel's solution: huge pages

| Size | Bits | When used |
| ---- | ---- | --------- |
| 2 MiB (2^21) | skip PMD level | common for large allocations |
| 1 GiB (2^30) | skip PUD level | databases, HPC workloads |

With a 2 MiB page, the offset field is 21 bits (12 + 9), skipping the PTE level. One TLB entry covers 512× more memory. Linux supports **Transparent Huge Pages (THP)** which automatically promotes aligned 4 KiB pages to huge pages without application changes.

---

## Page Faulting

A **page fault** occurs when the virtual address maps to a non-existent page or an inaccessible page (e.g., wrong permissions).

**The kernel can respond in several ways:**

- **Kill (terminate) the process** — the access was illegal with no recovery possible.

- **Send `SIGSEGV` to the process** — lets the process handle it (e.g., a custom signal handler).
  > **Fact correction:** The original notes said "SIGSEV" — the correct signal name is **`SIGSEGV`** (Segmentation Violation).

- **Fix the PTE and restart the faulted instruction** — this is how **demand paging** works. Suppose we have more virtual memory than physical memory: we can set up page table entries for a large virtual space, but mark most PTEs as "not present." When the process accesses one of those pages, the page fault fires, the kernel loads the page from swap (or disk/file), updates the PTE to point to the new physical frame, and **restarts the faulting instruction** as if nothing happened. The process never knows.

---

## Physical Memory, Virtual Memory, and Swap

```
Virtual memory (per-process, large):        Physical memory (shared, limited):
┌─────────────────────┐                      ┌──────────────────┐
│  code               │◄──── PTE (present) ─►│  frame for code  │
│  heap               │◄──── PTE (present) ─►│  frame for heap  │
│  stack              │◄──── PTE (present) ─►│  frame for stack │
│  mmap region        │ ──── PTE (absent) ──► (not in RAM)      │
└─────────────────────┘                      └──────────────────┘
                                                      │
                                            PTE absent → page fault
                                                      │
                                                      ▼
                                            ┌──────────────────┐
                                            │   Swap space     │
                                            │ (disk / SSD)     │
                                            └──────────────────┘
```

**Swap space:** A reserved region on secondary storage (HDD, SSD, or a swap file) where the OS stores pages evicted from physical memory.

> **Fact correction:** The original notes said "special spot on flash drive" — more precisely, swap is on any secondary storage (typically a dedicated **swap partition** or **swap file** on disk or SSD). It's not necessarily a flash drive specifically.

When physical memory fills up: the OS picks a **victim page**, copies it to swap, frees the physical frame, and maps it to the new page. Later, if the evicted page is accessed again, another page fault fires and it's brought back in.

---

## To Make This Work: Two Required Things

### 1. Policy for Page Replacement

When a page fault occurs and physical memory is full — which page do we evict?

| Algorithm | Strategy | Notes |
| --------- | --------- | ----- |
| **Optimal (OPT)** | Evict the page used farthest in the future | Best possible; not implementable (requires future knowledge). Used as benchmark. |
| **FIFO** | Evict the oldest page (first in) | Simple, but poor — suffers Bélády's anomaly (more frames → more faults). |
| **LRU** | Evict the least recently used page | Good approximation of optimal. Expensive to implement exactly. |
| **Clock (Second-Chance)** | Circular scan; evict pages whose "accessed" bit is clear | Cheap approximation of LRU. Standard in practice. |
| **Working Set** | Keep the pages referenced in a recent time window | Good for multiprogramming; more complex. |

### 2. Mechanism for Page Replacement

How does page replacement actually work?

```
Page fault occurs
      │
      ▼
1. Pick a victim frame (using policy above)
      │
      ▼
2. Is the victim page dirty (modified)?
   Yes → write it to swap space first (flush to disk)
   No  → can discard immediately
      │
      ▼
3. Update victim's PTE: mark as "not present," record swap location
      │
      ▼
4. Flush TLB entry for the victim (or broadcast TLB shootdown on multicore)
      │
      ▼
5. Load the requested page from swap (or file/disk) into the now-free frame
      │
      ▼
6. Update the faulting PTE: point to new frame, mark "present"
      │
      ▼
7. Restart the faulted instruction — process continues unaware
```

> **Copy-on-Write (COW):** After `fork()`, parent and child share physical pages (marked read-only in both PTEs). On the first write by either process, a page fault fires, the kernel copies the page, and each gets its own writable copy. This makes `fork()` very cheap when followed by `exec()`.

---

## Summary: Key Exam Points (Part 2)

| Concept | What to Know |
| ------- | ------------ |
| Why privilege isn't enough | load/store can corrupt kernel memory without virtual memory isolation |
| Clock interrupts | prevent infinite loops from starving the system; give kernel control back |
| Base + bound | simple; one contiguous region per process; no sharing or fine permissions |
| Segmentation | segment # + offset; per-segment permissions; fails due to external fragmentation and hard-to-grow segments |
| Paging | fixed-size pages; no external fragmentation; enables demand paging and swapping |
| x86-64 page size | 4 KiB (2^12), 12-bit offset |
| Multi-level page table | PGD → P4D → PUD → **PMD** → PTE (not PWD); 9 bits each; 5-level paging covers 57-bit VA space |
| TLB | caches VA→PA translations; flushed on context switch; critical for performance |
| Huge pages | 2 MiB or 1 GiB; skip PTE/PMD levels; fewer TLB entries for large allocations |
| Tagged pointers | LAM (Intel) / UAI (AMD — not UAM) — use unused high VA bits for metadata |
| Page fault responses | terminate, SIGSEGV, or fix PTE + restart instruction (demand paging) |
| Swap | secondary storage area for evicted pages; not specifically "flash drive" |
| Page replacement policy | OPT (best, unimplementable), FIFO (bad), LRU (good, expensive), Clock (practical LRU approx) |
| Page replacement mechanism | pick victim → flush if dirty → update PTEs → TLB shootdown → load new page → restart instruction |
