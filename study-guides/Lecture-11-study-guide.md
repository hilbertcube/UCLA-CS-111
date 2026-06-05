# Lecture 11 — I/O Performance: Disk Scheduling, Flash Storage, and File System Intro

---

## Overview

Three connected topics:

1. **Disk (HDD) I/O scheduling** — how the OS orders reads/writes to a spinning disk to maximize throughput while maintaining fairness
2. **Flash (SSD/NVMe) storage** — the fundamentally different physics of flash, why it complicates the OS, and modern solutions (ZNS, FDP)
3. **File system design intro** — the FAT on-disk layout as a concrete starting point for file system implementation

---

## Part 1: Disk (HDD) I/O Scheduling

### Background: How a Hard Disk Drive Works

Understanding scheduling requires knowing why seeks are expensive.

An HDD has:
- **Platters** — spinning magnetic disks (typically 5400–7200 RPM)
- **Read/write heads** — one per platter surface, on a movable arm
- **Tracks** — concentric rings on each platter surface
- **Sectors** — fixed-size (512 bytes) arc segments of a track
- **Cylinders** — the set of all tracks at the same arm position across all platters

**Accessing a block involves three delays:**

| Component | Description | Typical time |
|-----------|-------------|-------------|
| **Seek time** | Moving the arm to the correct track | 5–15 ms |
| **Rotational latency** | Waiting for the target sector to rotate under the head | 0–8 ms (avg ~4 ms at 7200 RPM) |
| **Transfer time** | Reading/writing the data once head is in position | < 1 ms for a sector |

**Seek time dominates.** Minimizing seek distance is the primary goal of disk scheduling. This is why HDD scheduling is a fundamentally different problem than CPU scheduling.

> **Key mental model:** Think of the HDD as a number line from sector 0 to sector N-1. The read/write head is currently at position `h`. To service a request at position `i`, the cost is `|h - i|`. Scheduling is about choosing the order of requests to minimize total arm movement.

---

### Algorithm 1: FCFS (First Come, First Served)

Process requests in arrival order.

**Analysis:**

- If requests are uniformly random across sectors 0 to N-1:
  - Expected seek distance per request = **N/3**
  - This is because E[|h - i|] for two uniform random variables over [0, N-1] is approximately N/3

- Example: Sectors 0–999 (N=1000). If head is at 500 and next request is random, average seek = ~333.

**Verdict:** Simple, fair, starvation-free. But ignores the physical reality of disk positioning — adjacent requests may be skipped over repeatedly, causing maximum seek distances.

---

### Algorithm 2: SSTF — Shortest Seek Time First

Always service the request **closest** to the current head position.

```
Head at 50. Pending: [14, 37, 65, 98, 122, 184]
SSTF order: 65, 37, 14, 98, 122, 184   ← always pick nearest
```

**Advantages:**
- Much better throughput than FCFS — minimizes total arm movement

**Disadvantages — Starvation:**
- Requests near the center of the disk starve out requests at the edges
- If a constant stream of requests arrives near the current head position, outer-track requests may never be served
- This is the disk equivalent of SJF's long-job starvation

**Verdict:** SSTF > FCFS in throughput. Neither is actually used in practice.

---

### Algorithm 3: Elevator Algorithm (SCAN)

**Hybrid approach:** SSTF-like efficiency with a starvation prevention guarantee.

**Rules:**
- Head moves in **one direction** (say, increasing sector numbers), servicing all requests along the way
- When it reaches the **end of the disk** (or the last pending request in this direction), it **reverses direction**
- Services requests on the way back

```
Head starts at 50, moving in + direction. Pending: [14, 37, 65, 98, 122, 184]

→ Phase: service 65, 98, 122, 184 (all requests in + direction)
← Phase: service 37, 14            (all requests in - direction)
```

**Why the elevator analogy:** Exactly like a building elevator — it goes up, picks up/drops off people along the way, reaches the top floor, then goes down. People on the bottom floor eventually get served.

**Advantages:**
- No starvation — every request will be served within at most 2 full passes across the disk
- Better throughput than FCFS

**Disadvantages:**
- Not as optimal as SSTF (you may pass near a request but not serve it immediately because you're committed to the current direction)
- Requests near the **center** of the disk get better average service than requests at the **edges** — center requests are hit on every pass, edge requests only on every other pass

---

### Algorithm 4: Circular Elevator (C-SCAN)

**Modification for fairness:** Only service requests in **one direction** (say, 0 → N-1). When the head reaches the end, **jump instantly back to sector 0** without servicing on the return trip.

```
Head at 50, moving →. Pending: [14, 65, 98, 122, 184]

→ Pass: service 65, 98, 122, 184
↩ Jump back to 0 instantly (no servicing on return)
→ Pass: service 14, (next arrivals) ...
```

**Advantages:**
- More **uniform wait times** — every sector waits at most one full sweep
- Fairer for edge sectors (they're no longer second-class citizens)

**Disadvantages:**
- **Lower throughput** — the return trip is "wasted" (no I/Os served while jumping back)
- The jump-back costs seek time with no work done

**Comparison summary:**

| Algorithm | Throughput | Fairness | Starvation | Real-world use |
|-----------|-----------|---------|-----------|---------------|
| FCFS | Poor (N/3 average seek) | Perfect | No | No |
| SSTF | Best | Poor | Yes (edge starvation) | No |
| Elevator (SCAN) | Good | Good | No | Basis of real algorithms |
| C-SCAN | Moderate | Best | No | Preferred in practice |

---

### Optimization: Anticipatory Scheduling (Dallying)

**The problem with reactive scheduling:** After finishing a request, the scheduler immediately picks the next request. If two applications are both doing I/O to the same region of the disk, they may alternate in a way that causes the head to thrash back and forth.

**Dallying:** After completing an I/O, the scheduler **intentionally waits a brief time** (a few milliseconds) before moving to the next request, hoping a nearby request arrives from the same process or disk region.

```
Process A makes requests to sectors ~100, ~110, ~120 (sequential read)
Process B makes requests to sectors ~500, ~510, ~520

Without dallying: 100, 500, 110, 510, 120, 520  ← 4 long seeks
With dallying:    100, 110, 120, 500, 510, 520   ← 2 long seeks + short seeks
```

**"Dallying" = keeping a resource idle when it could be doing other work, in anticipation of a better request arriving.**

**Trade-off:** Dallying adds latency for the waiting request. If no nearby request arrives in the wait window, throughput decreases for nothing. Works best when there are multiple concurrent applications doing sequential I/O to different regions.

> **Exam trap:** "Dallying" is a general concept — keeping a resource deliberately idle to improve future decisions. It appears in I/O scheduling but the concept generalizes (e.g., CPU scheduler dallying before a thread migration, network scheduler batching packets).

---

## Part 2: Flash Storage (SSDs and NVMe)

### Why Flash Is Different

HDDs store data magnetically. Flash stores data by **trapping electrons in floating-gate transistors**. This fundamentally different physics creates fundamentally different performance characteristics and failure modes.

**The OS started on disk but evolved to accommodate flash.** Many OS assumptions (uniform access time, arbitrary overwrite, no wear) break for flash.

---

### Flash Physical Structure

```
┌─────────────────────────────────────────────────┐
│  Flash chip                                     │
│  ┌──────────────┐ ┌──────────────┐              │
│  │ Block (~MiB) │ │ Block (~MiB) │   ...        │
│  │ ┌──────────┐ │ │              │              │
│  │ │ Page(KiB)│ │ │              │              │
│  │ ├──────────┤ │ │              │              │
│  │ │ Page(KiB)│ │ │              │              │
│  │ └──────────┘ │ │              │              │
│  └──────────────┘ └──────────────┘              │
└─────────────────────────────────────────────────┘
```

| Unit | Size | I/O operation |
|------|------|---------------|
| **Page** | 8–32 KiB | Read and **write** unit |
| **Block** | 1–128 MiB | **Erase** unit |

**Critical constraint: erase before write.**

Flash cells can only be written once without erasing first. To overwrite a page:
1. You cannot write directly — you must erase the entire **block** containing that page
2. Erase the block (all pages in the block → all 1s)
3. Write the desired pages

This means **a single 8 KiB overwrite might require erasing a 128 MiB block** — an enormous mismatch.

---

### Flash Latency

| Operation | Latency | Notes |
|-----------|---------|-------|
| **Read** | 10–100 µs | Fastest operation; electrons are simply measured |
| **Write (program)** | 100–200 µs | Electrons must be injected into floating gate |
| **Erase** | 1,500–10,000 µs | Must discharge entire block; most expensive |
| **HDD read/write** | 5–15 ms | For comparison |

**Flash is 50–1000× faster than HDD** for reads and writes. But **erase is 10–100× slower than write** — a critical asymmetry within flash itself.

---

### Flash Cell Types

Flash cells can store different numbers of bits depending on how many distinct charge levels the cell can hold:

| Type | Bits/cell | Speed | Endurance (P/E cycles) | Cost/bit | Use case |
|------|-----------|-------|----------------------|---------|---------|
| **SLC** | 1 | Fastest | ~100,000 | Highest | Enterprise, caches |
| **MLC** | 2 | Fast | ~10,000 | High | Consumer SSDs (older) |
| **TLC** | 3 | Moderate | ~3,000 | Low | Consumer NVMe (most common today) |
| **QLC** | 4 | Slowest | ~1,000–3,000 | Lowest | High-capacity SSDs |

> **P/E cycle = Program/Erase cycle.** Each time a block is erased and rewritten, it degrades slightly. After enough P/E cycles, the cell can no longer reliably hold a charge → the block is "worn out" and must be retired.

**Most consumer NVMe drives today are TLC/QLC with ~3,000 P/E cycles.** This sounds low, but with wear leveling (below), a 1 TB drive with 3,000 P/E cycles can handle 3,000 × 1 TB = 3 PB of writes before wearing out — the drive is rated in **TBW (Total Bytes Written)**, e.g., a 1 TB drive might be rated 600 TBW.

**Why QLC has more bit flips:**

More bits per cell means smaller voltage differences between levels. Adjacent cells' electrons can interfere with each other (**read disturb**, **program disturb**). Flash must include heavy **Error Correction Codes (ECC)** circuitry. ECC adds latency and silicon overhead but is essential for reliability.

---

### Flash Solution: The Flash Translation Layer (FTL)

Because flash cannot be overwritten directly, the **FTL (Flash Translation Layer)** sits between the OS and the raw flash chips. It is firmware running on the SSD controller.

#### 1. Logical-to-Physical (L2P) Mapping

The host OS thinks it's writing to **logical block addresses (LBAs)** — the same sequential block numbers it would use on an HDD. The FTL transparently remaps these to **physical flash page addresses**.

```
Host writes to LBA 1000:
  FTL: "Physical page 4250 is free; write to 4250"
  L2P table: LBA 1000 → physical page 4250

Host later writes to LBA 1000 again (overwrite):
  FTL: "Physical page 6130 is free; write to 6130"
  L2P table: LBA 1000 → physical page 6130  (updated)
  Old page 4250 is now STALE (marked for garbage collection)
```

**Why this is necessary:** You can't overwrite a flash page directly. Instead, write the new version to a fresh page and update the mapping. The old page is stale until the block it lives in gets garbage collected.

#### 2. Wear Leveling

**Problem:** If the FTL always uses the same physical blocks for frequently-updated data (like a heavily-written log file), those blocks will wear out while other blocks remain pristine.

**Solution — Wear leveling:**
- The FTL tracks the **erase count** of every block
- When choosing where to write new data, it **prefers blocks with lower erase counts**
- Even data that rarely changes may be **relocated** to a worn block so the corresponding fresh block can absorb more writes

This distributes wear evenly across all physical blocks, maximizing the drive's lifespan.

#### 3. Garbage Collection

Over time, blocks accumulate stale pages (old versions of overwritten data). A block cannot be reused until it is erased. The FTL must periodically:
1. Find blocks containing mostly stale pages
2. Copy the still-valid pages to fresh locations
3. Erase the old block (making it available again)

This garbage collection runs **in the background** but consumes flash bandwidth → contributes to write amplification.

#### 4. DRAM Buffers

SSDs include significant DRAM:
- **Read buffer:** Cache recently-read pages (avoids re-reading from flash)
- **Write buffer:** Batch incoming writes → write them to flash in large sequential chunks (more efficient than many small writes)
- **SLC cache:** High-capacity QLC drives sometimes use a small portion of flash in SLC mode as a fast write cache; data is then migrated to QLC in the background

---

### Write Amplification (WA)

**Write amplification** = the ratio of **physical bytes written to flash** divided by **logical bytes written by the host**.

```
WA = physical writes (bytes) / logical writes (bytes)

Example: Host writes 10 pages. FTL writes 50 pages (10 new + 40 for GC relocation).
WA = 50 / 10 = 5
```

**What causes write amplification:**
1. **Garbage collection:** To free a block, valid pages must be relocated (copied to new blocks). Each copied page counts as a physical write.
2. **Over-provisioning:** SSDs reserve extra capacity (~7–28%) to keep garbage collection efficient. This space is invisible to the host.
3. **Fragmentation:** Blocks with a mix of valid and stale pages are less efficiently collected than blocks that are entirely stale.

**WA range:** 2–20, depending on:
- Access pattern (sequential writes → WA near 1; random small writes → WA can be very high)
- Over-provisioning amount
- Drive fill level (a nearly full drive has less room to maneuver → higher WA)

**Endurance implication:** A 1 TB drive rated for 600 TBW assumes ~WA of ~1. With WA=5, your effective TBW drops to 120 TBW.

> **Exam trap:** We care about physical **erasures** more than physical writes, because erasures are what wear out the device. But WA is typically measured in writes because erasures happen at block granularity (hard to attribute to individual requests).

---

### ZNS — Zoned Namespaces

**Motivation:** Traditional NVMe hides the erase-before-write constraint behind the FTL. The FTL is complex, causes unpredictable write amplification, requires a lot of DRAM, and adds latency. **ZNS exposes the zoned structure directly to the host OS**, letting the OS participate in placement decisions.

**Core idea:** Partition the drive into **zones** (each zone = one large erase block or a set of erase blocks).

**Zone properties:**
- Each zone is **written only sequentially** (append-only)
- Each zone is **erased atomically** (the entire zone at once)
- The host controls when to open, write, finish, and reset zones

**Zone states:**

| State | Description |
|-------|-------------|
| **Empty** | Freshly erased; ready to receive writes |
| **Open** | Currently being written to; write pointer advances with each write |
| **Closed** | Write pointer stopped (but zone not full); can be re-opened |
| **Full** | Zone is completely written; read-only until explicitly reset |
| **Read Only** | Permanently read-only (e.g., after an error) |
| **Offline** | Unusable (hardware failure in this zone) |

**ZNS write operations:**

| Operation | Description | Latency |
|-----------|-------------|---------|
| **Write** | Host specifies exact offset within zone; must be sequential | Normal write latency |
| **Append** | Host says "append to this zone"; device reports where it wrote | ~23% lower latency than write |
| **Finish** | Explicitly close a zone (open → full); expensive | ~0.1 seconds |

**ZNS advantages:**
- **WA ≈ 1 + δ** (small δ): When the OS writes sequentially to zones, there's almost no FTL-induced write amplification — data is written exactly where the OS wants it.
- **Simpler FTL**: Less DRAM needed (no huge L2P table), less firmware complexity.
- **Predictable latency**: No hidden GC pauses.

**ZNS disadvantages:**
- **3× lower random read throughput** vs plain NVMe under concurrent I/O — zone metadata management adds overhead.
- The **OS/file system must be zone-aware** (e.g., F2FS, RocksDB with ZNS support).
- The host must manage zone resets and garbage collection (it's now your problem, not the FTL's).

---

### FDP — Flexible Device Placement

**Motivation:** ZNS is powerful but requires the application to fully manage sequential zone placement. Many existing applications can't do this.

**FDP is a middle ground:**
- Host uses **logical block numbers** (like traditional NVMe) — no need to manage zones manually
- Host provides **hints** about data placement (e.g., "these writes will be updated frequently together")
- The FTL uses these hints to **build its own internal zones** — it groups hinted data onto the same erase blocks
- Result: related data shares erase blocks → when GC runs, blocks are either fully stale (easy) or fully valid (don't erase) → lower WA

**FDP = traditional NVMe API + placement hints → lower write amplification without changing the application's addressing model.**

---

### NVMe Device Drivers: PIO vs. DMA

How does the CPU actually communicate with the NVMe controller?

**PIO — Programmed I/O:**
- CPU directly reads/writes device registers to transfer data
- Every byte of data transfer goes through the CPU
- Simple to implement, but CPU is **fully occupied** during the transfer — it can't do anything else
- **Trap-intensive:** Every byte (or word) access to a device register is a privileged operation

**DMA — Direct Memory Access:**
- CPU tells the device: "data is at RAM address X, length Y — go fetch/store it yourself"
- The DMA controller moves data between RAM and device **without CPU involvement**
- CPU is free to do other work during the transfer
- One trap to initiate the transfer, one interrupt when done → much fewer traps than PIO

**NVMe queues (the modern approach):**

NVMe uses DMA with a **submission queue / completion queue** model in RAM:

```
For each CPU core:
  ┌──────────────────────────────────┐
  │  Submission Queue (in RAM)       │  ← CPU writes I/O commands here
  │  [cmd1][cmd2][cmd3][ ... ]       │
  └──────────────────────────────────┘
                  │ DMA
                  ▼
          NVMe Controller
                  │ DMA
                  ▼
  ┌──────────────────────────────────┐
  │  Completion Queue (in RAM)       │  ← Controller writes results here
  │  [done1][done2][ ... ]           │
  └──────────────────────────────────┘
```

**Per-CPU-core queues:** Each CPU core has its own submission and completion queues. This eliminates lock contention — different cores can submit I/O simultaneously without coordinating. This is critical for NVMe performance, which can saturate multiple CPU cores.

---

## Part 3: File System Design Intro — FAT On-Disk Layout

### File System as a Hybrid Data Structure

A file system is a **hybrid data structure**:

- **Persistent (secondary storage):** The bulk of the data — file contents, directory structures, metadata. Survives power loss.
- **Volatile (DRAM):** Caches of frequently-accessed metadata (inode cache, directory entry cache, buffer cache). Lost on power failure, but can be reconstructed from the on-disk state.

The OS constantly moves data between these tiers to balance performance (DRAM is fast) and durability (disk is persistent).

### FAT-16 On-Disk Layout

The disk is divided into fixed-size **sectors** (512 bytes each, for FAT-16). The disk layout is:

```
Sector 0:  Boot Sector
Sector 1:  Superblock (File System Metadata)
Sector 2+: FAT (File Allocation Table)
  ...
Data Blocks: File and directory contents
```

| Region | Contents |
|--------|---------|
| **Boot Sector** | Bootloader code; executes on startup; loads the OS kernel |
| **Superblock** | File system metadata: version, total size, cluster size, FAT location, root directory location. The OS reads this first to understand the layout. |
| **FAT (File Allocation Table)** | Array of entries, one per data block. Entry = next block in file chain, or EOF, or FREE. |
| **Data Blocks** | Actual file and directory contents. |

**Superblock fields (key ones):**
- Version — which FAT variant (FAT-12, FAT-16, FAT-32)
- Bytes per sector, sectors per cluster
- Number of FAT copies (FAT stores two copies for redundancy)
- Total sector count
- Root directory location (for FAT-16: fixed; for FAT-32: in a cluster)

**FAT array structure:**
- One entry per **cluster** (cluster = one or more consecutive sectors)
- Each entry is 16 bits (FAT-16) → can address 2^16 = 65,536 clusters
- At 32 KiB clusters: max 2 GB partition size (FAT-16 limit)

**Reading a file in FAT-16:**
1. Look up file in directory → get starting cluster number
2. Read cluster from data area
3. Look up cluster number in FAT → get next cluster number
4. Repeat until FAT entry = EOF (`0xFFFF` for FAT-16)

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| HDD seek cost | Three components: seek + rotational latency + transfer; seek dominates |
| FCFS disk | Process in arrival order; average seek = N/3; simple but inefficient |
| SSTF | Always pick nearest pending request; best throughput; edge starvation |
| Elevator (SCAN) | One direction at a time; reverse at end; no starvation; center bias |
| C-SCAN | One direction only; jump back to 0; uniform wait times; lower throughput |
| Dallying | Wait briefly after I/O completion hoping for nearby request; improves locality |
| Flash: page vs. block | Page (8–32 KiB) = read/write unit; Block (1–128 MiB) = erase unit |
| Erase before write | Cannot overwrite a flash page; must erase entire block first |
| Flash latency order | Read (10–100 µs) < Write (100–200 µs) < Erase (1.5–10 ms) |
| SLC vs QLC | SLC: 1 bit/cell, fast, durable (~100K P/E); QLC: 4 bits/cell, cheap, fragile (~1K–3K P/E) |
| Flash wears out | P/E cycles degrade cells; TLC/QLC ~3,000 cycles; tracked in TBW (e.g., 600 TBW) |
| L2P mapping | FTL maps logical block addresses → physical page addresses; enables overwrite by redirect |
| Wear leveling | Track erase counts per block; route writes to less-worn blocks; extends drive life |
| Write amplification | Physical writes / logical writes; range 2–20; caused by garbage collection |
| GC and stale pages | Overwritten pages become stale; block must be erased to free them; GC copies valid pages first |
| ZNS | Expose zones to host; sequential-write-only zones; erase atomically; WA ≈ 1; host manages placement |
| ZNS: append op | Append to zone, device reports where; 23% lower latency than write; tradeoff is lower random read throughput |
| FDP | Logical block numbers + hints; FTL builds zones internally; lower WA without zone-aware apps |
| PIO vs DMA | PIO: CPU does transfer (slow, CPU busy); DMA: device does transfer, CPU free |
| NVMe queues | Submission/completion queues in RAM; per-CPU-core queues eliminate lock contention |
| FAT on-disk layout | Boot sector → superblock → FAT array → data blocks |
| Superblock | Stores FS metadata: version, size, cluster size, FAT location |
| FAT-16 limits | 16-bit entries → 65,536 clusters; at 32 KiB/cluster → 2 GB max |
| File system as hybrid | Persistent on disk (durable) + volatile DRAM cache (fast but lost on crash) |
