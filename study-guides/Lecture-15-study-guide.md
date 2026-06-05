# Lecture 15 — Virtual Memory: Page Fault Mechanism, Replacement Policy, mmap, and Distributed FS Intro

---

## Overview

This lecture has two major topics:

1. **Page fault mechanism** — how the kernel handles page faults step by step, including swap management, the removal policy hook, and implementation pitfalls
2. **Page replacement policy** — how to choose which physical page to evict; FIFO, Bélády's anomaly, OPT, LRU, and practical approximations

Then a brief introduction to **distributed file systems** — the problems they must solve.

---

## Part 1: The Page Fault Mechanism

### Background — Why Page Faults Happen

A **page fault** is a hardware trap that fires when a running process accesses a virtual address that has no valid physical mapping in the page table. The MMU checks the PTE (Page Table Entry) on every memory access; if the Present/Valid bit is 0, it traps into the kernel.

**Three reasons a page fault can occur:**

| Reason | What it means | Kernel response |
|--------|--------------|----------------|
| **Truly invalid address** | The virtual address is not part of any mapping the process owns (e.g., NULL dereference, wild pointer) | Kill the process (SIGSEGV) |
| **Present but protected** | The page exists but the access violates permissions (e.g., write to a read-only page) | Kill or handle (used for Copy-on-Write) |
| **Valid but not in RAM** | The page is legitimately part of the process's address space but is currently stored in swap space | Bring the page in from swap, update PTE, retry |

The third case is **demand paging** — the focus of this lecture.

---

### The Kernel's Page Fault Handler

When a hardware page fault fires:

1. **Hardware saves minimal state:**
   - **Instruction pointer (PC)** — where execution was when the fault occurred (so we can restart)
   - **Status/flags register** — processor mode, condition bits
   - **Faulting virtual address** — the address that caused the fault (placed in a special register, e.g., `CR2` on x86)

2. **Kernel inspects the faulting address:**
   - If the address is 0 (or near 0, in the unmapped "null guard" region): it's almost certainly a null pointer dereference → kill the process
   - The kernel looks up the address in the process's memory map to determine whether it's a valid virtual address that just happens not to be in RAM

3. **Hardware "extra bits" in PTEs:** Modern hardware leaves several bits in PTE entries unused (x86 has bits 9–11 and bits 52–62 available for software use). The OS uses these to tag pages with state information (e.g., "this page is valid but currently in swap").

---

### `swapmap` — Finding a Page in Swap

The kernel must know where each swapped-out page lives on secondary storage. A conceptual function:

```c
off_t swapmap(uintptr_t va, struct task *proc);
// Given a virtual address and the process that owns it,
// return the byte offset in swap space where that page is stored.
// Returns < 0 if the address is invalid (not mapped anywhere).
```

**Swap space** is a reserved region of a disk or SSD (or a swap file) formatted as a flat array of page-sized slots:

```
Swap space:
| slot 0 | slot 1 | slot 2 | ... | slot N-1 |
```

Each slot holds one 4 KiB (or 8 KiB) page. The `swapmap` function maps `(virtual_address, process)` → slot index.

---

### Full Page Fault Algorithm

```c
/*
 * Pseudocode — not compilable C. OS-level operations (write_to_swap,
 * read_from_swap, pte_set, tlb_flush) stand in for actual kernel calls.
 * removal_policy() returns its results via output pointer parameters.
 *
 * Bug in the professor's original: the final pte_set used victim_va
 * instead of va. Fixed here: we must update the mapping for the
 * faulting address va, not the victim's address victim_va.
 */
void pfault(uintptr_t va, struct task *proc)
{
    // 
    off_t o = swapmap(va, proc);

    if (o < 0) {
        /* va is not mapped anywhere — truly invalid access */
        kill_process(proc, SIGSEGV);
        return;
    }

    /* va is valid but its page is in swap — bring it in */

    /* Step 1: choose a victim physical frame to evict */
    uintptr_t    phys_page;   /* which physical frame to reuse        */
    uintptr_t    victim_va;   /* virtual address currently in that frame */
    struct task *victim_proc; /* process that owns victim_va           */
    removal_policy(&phys_page, &victim_va, &victim_proc);

    /* Step 2: evict the victim — write its contents to swap */
    off_t victim_swap_off = swapmap(victim_va, victim_proc);
    write_to_swap(phys_page, victim_swap_off); /* flush victim frame → swap  */
    pte_set(victim_proc, victim_va, PTE_SWAPPED); /* mark victim not-present */
    tlb_flush(victim_proc, victim_va);         /* invalidate stale TLB entry */

    /* Step 3: load the requested page into the freed physical frame */
    read_from_swap(o, phys_page);  /* load swap offset o → phys_page  */

    /* Step 4: update the FAULTING process's PTE for address va (not victim_va) */
    pte_set(proc, va, phys_page | PTE_PRESENT | PTE_RW);

    /* RTI: return-from-interrupt; hardware re-executes the faulting instruction */
}
```

**The critical finale:** The kernel returns from the interrupt and the **faulting instruction is re-executed from scratch**. The hardware retries it — this time the PTE is valid, so the access succeeds. The process never knows anything happened.

---

### Performance Concern: Overhead

Each page fault involves:
- Trapping into the kernel
- Calling `removal_policy()` (which may scan many pages)
- Potentially writing the evicted page to swap (a full disk/SSD write)
- Reading the requested page from swap (a full disk/SSD read)
- Updating PTEs and flushing TLB entries
- Returning from the interrupt

**This is orders of magnitude slower than a regular memory access.** A cache hit is ~1 ns; a page fault requiring a disk read is ~5–15 ms for HDD, ~0.1 ms for SSD. That's 5,000–15,000,000× slower.

> **Key rule:** You *can* run a 100 GB application on an 8 GB machine using swap, but you should never actually do this for performance-sensitive workloads. If you're constantly paging (called **thrashing**), the system spends more time handling page faults than doing real work.

---

### The Double Page Fault Problem

**Can the fault handler itself page fault?** Yes — and this is a real problem.

Consider an instruction that touches two memory locations:

```asm
movsq   ; reads from [rsi], writes to [rdi] — touches two virtual addresses
```

This touches two addresses. If `[rsi]` is swapped out:
1. Page fault on `[rsi]`
2. Kernel loads `[rsi]`'s page, restarts instruction
3. Now `[rdi]` is swapped out (or was evicted as the victim to make room for `[rsi]`)
4. Page fault again on `[rdi]`
5. Kernel loads `[rdi]`'s page, restarts instruction
6. *Now both are in RAM* — instruction succeeds

This "restart → fault again" chain terminates **as long as we make forward progress** on each restart. The kernel must ensure the pages it just loaded stay in RAM long enough for the restarted instruction to finish. Most kernels **pin** recently loaded pages (mark them un-evictable) for a brief period.

---

### Kernel Memory and the Bootstrap Problem

**The chicken-and-egg problem:** The kernel's own page fault handler, the swap code, and the page tables themselves live in virtual memory. What happens if the kernel's memory gets paged out?

The kernel must **never** let its own critical memory become a victim. The solution: mark all kernel pages as **pinned** (non-evictable, always present). The kernel only swaps out **user-space pages**. Kernel memory is locked in physical RAM at all times.

---

## Part 2: Page Replacement Policy

### Locality of Reference

Why does any replacement policy work at all? Because real programs exhibit **locality**:

| Type | Definition | Example |
|------|-----------|---------|
| **Temporal locality** | A page accessed recently will likely be accessed again soon | Loop variables, frequently called functions |
| **Spatial locality** | Pages near a recently accessed page will likely be accessed soon | Sequential array reads, code execution |

Because of locality, the "working set" of pages a program actively uses at any moment is much smaller than its total virtual address space. Good replacement policies exploit locality — they try to keep the working set in RAM.

---

### Evaluating Policies: Reference Strings

To compare replacement algorithms, we use a **reference string** — a sequence of virtual page numbers (VPNs) representing a program's page access pattern.

**Example reference string from lecture:**
```
0 1 2 3 0 1 4 0 1 2 3 4
```
(12 accesses; each number is a VPN)

We measure the number of **page faults** each algorithm incurs on this string.

---

### Algorithm 1: FIFO (First-In, First-Out)

**Rule:** Evict the page that has been in RAM the longest (oldest resident).

**Simulation with 3 physical frames:**

| Access | 0 | 1 | 2 | 3 | 0 | 1 | 4 | 0 | 1 | 2 | 3 | 4 |
|--------|---|---|---|---|---|---|---|---|---|---|---|---|
| **Frame A** | **0**F | 0 | 0 | **3**F | 3 | 3 | **4**F | 4 | 4 | 4 | 4 | 4 |
| **Frame B** | – | **1**F | 1 | 1 | **0**F | 0 | 0 | 0 | 0 | **2**F | 2 | 2 |
| **Frame C** | – | – | **2**F | 2 | 2 | **1**F | 1 | 1 | 1 | 1 | **3**F | 3 |
| **Fault?** | F | F | F | F | F | F | F | – | – | F | F | – |

**9 faults** out of 12 accesses. (**F** = fault, **–** = hit)

**Eviction order:** the FIFO pointer cycles A→B→C→A. Each fault evicts from whichever slot the pointer currently indicates, then the pointer advances.

---

### Bélády's Anomaly — More RAM, More Faults (with FIFO)

**Intuition says:** more physical frames → fewer page faults.

This is true for most algorithms. But **FIFO violates it** for certain reference strings. Adding a frame can actually *increase* the number of page faults. This is called **Bélády's anomaly**.

**Simulation with 4 physical frames (same reference string):**

| Access | 0 | 1 | 2 | 3 | 0 | 1 | 4 | 0 | 1 | 2 | 3 | 4 |
|--------|---|---|---|---|---|---|---|---|---|---|---|---|
| **Frame A** | **0**F | 0 | 0 | 0 | 0 | 0 | **4**F | 4 | 4 | 4 | **3**F | 3 |
| **Frame B** | – | **1**F | 1 | 1 | 1 | 1 | 1 | **0**F | 0 | 0 | 0 | **4**F |
| **Frame C** | – | – | **2**F | 2 | 2 | 2 | 2 | 2 | **1**F | 1 | 1 | 1 |
| **Frame D** | – | – | – | **3**F | 3 | 3 | 3 | 3 | 3 | **2**F | 2 | 2 |
| **Fault?** | F | F | F | F | – | – | F | F | F | F | F | F |

**10 faults** — *worse* than 3 frames (9 faults)!

> **Why this happens:** With 4 frames, pages 0, 1, 2, 3 all fit initially. But after evicting 0 to load 4, the ordering of residents is wrong for the next accesses. The extra frame changed which pages were "oldest" and caused cascading evictions.

**Bélády's anomaly applies only to FIFO** (and a few other algorithms). LRU and OPT do **not** suffer from it — more frames always means fewer or equal faults. This is called the **stack property** (these algorithms' working sets are nested as frame count increases).

---

### Algorithm 2: OPT — Optimal Page Replacement

**Rule:** Evict the page that will not be used for the longest time in the future.

**This is provably optimal** — it minimizes page faults for any reference string and any frame count. **It cannot be implemented** in practice because it requires knowing the future. It serves as a theoretical benchmark — the best any real algorithm can possibly do.

**Simulation with 3 frames:**

| Access | 0 | 1 | 2 | 3 | 0 | 1 | 4 | 0 | 1 | 2 | 3 | 4 |
|--------|---|---|---|---|---|---|---|---|---|---|---|---|
| **Frame A** | **0**F | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **2**F | **3**F | 3 |
| **Frame B** | – | **1**F | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| **Frame C** | – | – | **2**F | **3**F | 3 | 3 | **4**F | 4 | 4 | 4 | 4 | 4 |
| **Fault?** | F | F | F | F | – | – | F | – | – | F | F | – |

**7 faults** — fewer than FIFO's 9. OPT evictions explained:

- **t=3:** frames hold {0,1,2}; next uses: 0→t=4, 1→t=5, 2→t=9. Evict **2** (used farthest).
- **t=6:** frames hold {0,1,3}; next uses: 0→t=7, 1→t=8, 3→t=10. Evict **3** (used farthest).
- **t=9:** frames hold {0,1,4}; next uses: 0→never again, 1→never again, 4→t=11. Evict **0** (never used again).
- **t=10:** frames hold {2,1,4}; next uses: 2→never, 1→never, 4→t=11. Evict **2** (never used again).

> **Exam trap:** You may be asked to trace OPT on a reference string. The procedure: at each fault, look ahead in the reference string for each currently-resident page, and evict the one that appears farthest in the future (or never appears again).

---

### Algorithm 3: LRU — Least Recently Used

**Rule:** Evict the page that was used least recently in the *past*.

**Why this approximates OPT:** By temporal locality, a page not used recently is less likely to be used soon. LRU uses the past to predict the future.

LRU is an **optimal stack algorithm** — it has the stack property (more frames → always ≤ faults), so it never suffers Bélády's anomaly.

**LRU is difficult to implement exactly:**

A true LRU implementation needs to timestamp every memory access and evict the page with the oldest timestamp. But memory accesses happen billions of times per second — maintaining exact timestamps would require hardware support that is prohibitively expensive.

---

### Practical LRU: Clock-Interrupt Approximation

A practical approximation of LRU on x86-64:

**Idea:** Use hardware page fault machinery to approximate access tracking.

**Algorithm:**
1. **On clock interrupt** (every ~10 ms): Mark ALL PTEs as "invalid/not-present" in software (without actually evicting anything — just clear the present bit).
2. **When a process accesses a page whose PTE was just cleared:** A page fault fires. The kernel marks the page as "accessed at time T" (current time), restores the PTE to present, and returns.
3. **When choosing a victim:** Evict the page with the oldest "last accessed" timestamp.

**Why this approximates LRU:** Pages accessed within the last clock interval get refreshed. Pages not accessed in many intervals have old timestamps → good eviction candidates.

**This is approximate, not exact.** Two pages accessed within the same clock interval have the same "time" — we can't distinguish which was used more recently. But it's a good practical approximation.

**The Clock Algorithm (Second-Chance)** — a simpler variant widely used in practice:

```
Physical frames arranged in a circular list (the "clock"):
  [page A] → [page B] → [page C] → [page D] → [page A] → ...
       ^
    clock hand
```

Each frame has an **accessed bit** (set by hardware when the page is read or written).

**On page fault:**
1. Look at the page the clock hand points to
2. If accessed bit = 0 → evict this page (it hasn't been used since we last cleared its bit)
3. If accessed bit = 1 → **give it a second chance**: clear the bit, advance the hand, go to step 1
4. On eviction: load the new page here, set its accessed bit = 1, advance the hand

**Intuition:** Pages that are actively used get their accessed bit set quickly (before the hand sweeps past them). Cold pages accumulate bit=0 and get evicted.

The Clock algorithm is **O(1) per eviction** (in the common case) and requires only 1 bit per frame. It's the algorithm used by most real OS kernels (Linux calls it "active/inactive" lists which is a two-hand clock variant).

---

### Summary: Replacement Algorithms Compared

| Algorithm | Rule | Optimal? | Bélády's anomaly? | Implementable? | Cost |
|-----------|------|---------|-------------------|---------------|------|
| **FIFO** | Evict oldest resident | No | **Yes** | Yes | O(1) |
| **OPT** | Evict page used farthest in future | **Yes** | No (stack property) | No (requires future knowledge) | — |
| **LRU** | Evict least recently used | Near-optimal | No (stack property) | Approximately | O(N) exact; O(1) approx |
| **Clock** | Second-chance approximation of LRU | Near-optimal | No | **Yes** | O(1) amortized |

---

## Part 3: Optimizations

### Optimization 1: Dirty Bit

**Problem:** Not every evicted page needs to be written to swap. If a page has not been modified since it was loaded, the swap copy is still up-to-date — we can just discard the physical frame.

**Dirty bit (in the PTE):** The hardware automatically sets this bit on any **store** instruction that modifies the page. Software clears it when the page is brought in from swap.

- **Dirty bit = 0 (clean):** Page matches its swap copy → discard and reuse the frame; no I/O needed
- **Dirty bit = 1 (dirty):** Page has been modified → must write to swap before reusing the frame

**Impact:** Eliminates up to 50% of swap writes in typical workloads (many pages are read-only — code pages, mmap'd files opened O_RDONLY).

**Software dirty bit tracking (for hardware that doesn't support it natively):**

1. When loading a page, mark it **read-only** in the PTE (even if it should be writable)
2. When a process writes to it, a page fault fires (write-protect fault)
3. In the fault handler: mark the PTE as writable **and** set a software dirty bit
4. Use the software dirty bit in eviction decisions

---

### Optimization 2: Demand Paging

**Idea:** Don't load a program's pages eagerly (at `exec` time). Instead, start the process with an empty page table and load each page only when it's first accessed (on the resulting page fault).

**Upside:**
- **Faster startup:** The process can start executing after loading just one page (the entry point). No need to load 50 MB of code that won't be touched for the first 10 seconds.
- **Works for enormous address spaces:** A process can "own" a 100 GB virtual address space without allocating a single physical page for regions it never touches.

**Downside:**
- **More page faults during startup:** The initial run has many cold faults. This can make the first few seconds of execution slower.
- **Less batching:** Loading many pages at once is more efficient than handling each fault individually (each fault involves a trap, kernel execution, I/O scheduling, etc.).

**In practice:** Demand paging is the universal default. The startup overhead is amortized quickly, and the memory savings are enormous.

---

## Part 4: `mmap` — The Universal Memory-Mapping Interface

`mmap` is the system call that connects the virtual address space to backing storage. It is the **foundation of all memory management** in modern Unix — `malloc`, `new`, loading executables, shared libraries, and file I/O all flow through it.

```c
void *mmap(
    void   *addr,    // Hint: where in virtual address space (or NULL to let OS choose)
    size_t  len,     // How many bytes to map
    int     prot,    // Protection: PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE
    int     flags,   // Behavior flags (see below)
    int     fd,      // File descriptor: where pages come from when loaded
    off_t   off      // Offset into the file (must be page-aligned)
);
// Returns: the virtual address of the mapping (or MAP_FAILED on error)
```

### `prot` — Protection Flags

| Flag | Meaning |
|------|---------|
| `PROT_READ` | Pages can be read |
| `PROT_WRITE` | Pages can be written |
| `PROT_EXEC` | Pages can be executed (code) |
| `PROT_NONE` | No access allowed (used for guard pages) |

### `flags` — Behavior Flags

| Flag | Meaning |
|------|---------|
| `MAP_SHARED` | Changes to the mapping are visible to other processes that map the same file; writes go back to the file |
| `MAP_PRIVATE` | Copy-on-write: changes are private to this process; the file is not modified |
| `MAP_ANONYMOUS` | Not backed by a file; pages are zero-initialized (backed by swap if evicted). Used for heap allocation |
| `MAP_FIXED` | Place the mapping at exactly `addr`; **forcibly unmaps** any existing mapping at that range (use `MAP_FIXED_NOREPLACE` to fail instead of replacing) |

### `fd` — The Backing File

When pages are evicted from RAM, they need somewhere to go. For `mmap`:
- **File-backed mapping** (`fd` = real file): pages are paged out back to the file on disk. The file is the "swap" for these pages.
- **Anonymous mapping** (`MAP_ANONYMOUS`, `fd = -1`): pages are backed by the swap partition/file.
- **Special device `/dev/zero`** (`fd = open("/dev/zero", ...)`): reads always return zeros; used to get zero-initialized anonymous memory (equivalent to `MAP_ANONYMOUS`).

### How `malloc` Uses `mmap`

```c
// Internally, malloc/new often do:
void *p = mmap(NULL, size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0);
```

This gives `malloc` a fresh chunk of private, zero-initialized virtual memory. It's demand-paged — no physical RAM is consumed until the program actually writes to it.

### Loading Executables

When you run a program, the kernel uses `mmap` to map the executable file into virtual memory:

```
Code segment:  mmap(text_addr, text_size, PROT_READ|PROT_EXEC, MAP_PRIVATE, exe_fd, text_offset)
Data segment:  mmap(data_addr, data_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, exe_fd, data_offset)
```

The program doesn't actually get loaded into RAM — the PTEs are set up but marked "not present." As execution proceeds, page faults bring in only the pages actually touched. This is why large programs start quickly.

### Shared Memory via `MAP_SHARED`

```c
// Process A and Process B both call:
int fd = open("shared_data.bin", O_RDWR);
void *shared = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
```

Both processes now see the same physical pages. Writes by A are immediately visible to B (and vice versa). This is the fastest possible IPC — no kernel copying, no system calls for data transfer.

---

## Part 5: Distributed File Systems — Introduction

### What Is a Distributed File System?

A **distributed file system (DFS)** is a file system where the actual storage is on a different machine (or multiple machines), accessed over a network.

Examples: NFS (Network File System), AFS (Andrew File System), Google File System, HDFS (Hadoop).

**Basic topology:**
```
Client machine                        Server machine (or cluster)
┌────────────────┐    network         ┌─────────────────────┐
│  application   │                    │  file server        │
│  /home/alice/  │ ←── requests ───→  │  /exports/home/     │
│  notes.txt     │                    │  (actual disk here) │
└────────────────┘                    └─────────────────────┘
```

To the application, it looks like a normal file. Underneath, every `open()`, `read()`, `write()` generates a network request.

---

### Problems a Distributed File System Must Solve

| Problem | Description |
|---------|-------------|
| **Performance** | Network latency (LAN: ~0.1 ms; cloud: ~50 ms) vs. local disk (~0.1 ms). Caching and prefetching are essential. Throughput is limited by network bandwidth. |
| **Scalability** | A single server can become a bottleneck. Must scale to millions of clients and petabytes of data. |
| **Flexibility** | Clients have different OSes, hardware, and access patterns. The DFS must serve all of them uniformly. |
| **Robustness** | Server crashes, network partitions, and disk failures must not cause data loss or permanent unavailability. Replication and failover are required. |
| **Security** | Data travels over the network — must be encrypted in transit. Authentication must verify that clients are who they claim to be (Kerberos, TLS). |
| **Transparency** | The application should not need to know it's talking to a remote file system. The DFS namespace should look identical to a local file system. |
| **Consistency** | If two clients write to the same file simultaneously, what do they see? This is the hardest problem. |

---

### The Consistency / Serializability Problem

**Consistency** means: if multiple clients perform operations concurrently, all clients observe a coherent, agreed-upon view of the file system.

**Serializability** — the strongest consistency model: there exists some total sequential ordering of all operations from all clients such that:
1. The ordering is consistent with the real-time order of non-overlapping operations
2. Each operation produces the result it would have produced if executed in isolation at its position in the sequence

In other words: even though operations happen concurrently in reality, the result must be *as if* they happened one at a time in some order.

```
Client 1: write(file, "hello")  ──────────────────────────────→ time
Client 2: write(file, "world")      ──────────────────────────→ time
Client 3: read(file) → ?                 ─────────────────────→ time

Serializable outcomes: "hello" or "world" (one must complete before the other)
Non-serializable: "heworldllo" (interleaved bytes) ← NOT allowed
```

**Why this is hard:**
- Network latency means clients can't know each other's state in real time
- Caching (for performance) means a client may see stale data
- Server crashes mean some operations may be partially applied

**The CAP theorem** (background context): A distributed system can provide at most two of: **Consistency**, **Availability**, **Partition tolerance**. Network partitions are inevitable in practice, so real systems choose between strong consistency (CP systems: ZooKeeper, HBase) or high availability (AP systems: Cassandra, DynamoDB).

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| Three causes of page faults | Invalid address (kill), permission violation (kill/COW), valid but swapped out (bring in and retry) |
| Kernel state saved on fault | Instruction pointer, status bits, faulting virtual address (in CR2 on x86) |
| `swapmap(va, proc)` | Returns offset of virtual page in swap space; negative = invalid address |
| Page fault algorithm steps | swapmap → removal_policy → evict victim to swap → mark victim PTE invalid → load requested page → update PTE → RTI → retry instruction |
| Double page fault | Instruction touching two addresses may fault twice; kernel must pin recently loaded pages |
| Kernel bootstrap problem | Kernel memory must be pinned (never evictable) to prevent faulting during the fault handler |
| Thrashing | When a system spends more time handling page faults than doing real work — caused by running too much workload for available RAM |
| FIFO replacement | Evict oldest-resident page; simple; Bélády's anomaly possible |
| Bélády's anomaly | With FIFO, adding more physical frames can *increase* page faults; demonstration: 3 frames = 9 faults, 4 frames = 10 faults on reference string 0 1 2 3 0 1 4 0 1 2 3 4 |
| OPT algorithm | Evict page needed farthest in future; provably optimal; unimplementable (needs future); benchmark only |
| LRU algorithm | Evict least recently used; approximates OPT; stack property (no Bélády's); exact implementation expensive |
| Clock (second-chance) | Circular list of frames; accessed bit; second chance before eviction; O(1) practical LRU approximation |
| Dirty bit | Hardware bit set on any write; clean pages discarded without I/O on eviction; dirty pages must be written to swap first |
| Demand paging | Pages loaded only on first access (page fault); faster startup; more faults during warmup |
| `mmap` | Maps file or anonymous memory into virtual address space; foundation of `malloc`, `exec`, shared memory |
| `MAP_PRIVATE` | CoW: changes private to this process; file not modified |
| `MAP_SHARED` | Changes visible to all processes mapping same file; fastest IPC mechanism |
| `MAP_ANONYMOUS` | Not file-backed; zero-initialized; backed by swap when evicted |
| DFS key challenges | Performance (latency), scalability, robustness (replication), security, transparency, consistency |
| Serializability | Strongest consistency: all concurrent ops appear to execute in some sequential order; result same as sequential execution |
