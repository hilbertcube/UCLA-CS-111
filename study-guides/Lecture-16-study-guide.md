# Lecture 16 — Distributed File Systems, RPC, and NFS

---

## Overview

This lecture builds directly on the distributed FS introduction from Lecture 15. It covers:

1. **DFS architecture** — components, design patterns, and why "ext4 on wheels" fails
2. **Fallacies of distributed computing** — the assumptions engineers wrongly make
3. **Remote Procedure Calls (RPC)** — how distributed calls differ from local calls; marshalling
4. **RPC failure modes** — at-least-once, at-most-once, exactly-once semantics
5. **NFS** — a real, widely-deployed DFS; its protocol, file handles, and consistency model

---

## Part 1: Components of a Distributed File System

A DFS is not a monolithic piece of software. It is a collection of cooperating components spread across machines:

| Component | Role |
|-----------|------|
| **Name nodes** | Store file system metadata — directory trees, file names, inode-equivalent structures. The "brain" of the file system. |
| **Data nodes** | Store the actual file content (data blocks). Can be moved, replicated, and deleted. |
| **Metadata nodes** | Sometimes separate from name nodes — handle attributes, access control lists, timestamps. |
| **Clients** | Application machines that issue `open`/`read`/`write` calls. |
| **Network / Routers** | The physical infrastructure connecting all components. Every DFS call crosses this boundary. |

### Common DFS Architectures

| Architecture | Description | Trade-offs |
|-------------|-------------|-----------|
| **Centralized namenode** | One machine manages all metadata (e.g., HDFS) | Simple, consistent; single point of failure and bottleneck |
| **Distributed namenodes** | Metadata partitioned across many machines | Scalable; more complex consistency |
| **Hierarchical** | Tree of metadata servers; global root delegates to regional servers | Scales to internet-size namespaces |
| **Peer-to-peer (P2P)** | No dedicated servers; every node acts as both client and server | Very resilient; hard to provide strong consistency guarantees |

---

## Part 2: Why "ext4 on Wheels" Doesn't Work

A tempting first idea: take a local file system (like ext4) and just add network calls wherever it does disk I/O. This is called **"ext4 on wheels"** — a local FS dragged onto the network.

**It fails because it violates fundamental assumptions.** These violations are codified as **Peter Deutsch's 8 Fallacies of Distributed Computing** — the eight things engineers incorrectly assume are true when designing distributed systems.

### The 8 Fallacies

| # | Fallacy | Reality |
|---|---------|---------|
| 1 | **The network is reliable** | Packets are dropped, routers fail, cables are cut. Every network call can fail silently. |
| 2 | **Latency is zero** | Even a fast LAN has ~0.1 ms latency. Cloud can be 50+ ms. This is 100,000× worse than a local memory access. |
| 3 | **Bandwidth is infinite** | You will hit bandwidth limits, especially for large file transfers or many concurrent clients. |
| 4 | **The network is secure** | Packets traverse untrusted hardware; eavesdropping, injection, and replay attacks are real. |
| 5 | **There is one administrator** | Large distributed systems span organizations, jurisdictions, and ownership boundaries. No single person controls everything. |
| 6 | **The topology won't change** | Machines are added, removed, and fail. IP addresses change. Routes change. The system must handle this dynamically. |
| 7 | **Transport cost is zero** | Bandwidth, cloud egress fees, and serialization CPU time all cost money and time. |
| 8 | **The network is homogeneous** | Clients may run different OSes, CPU architectures, endianness, and pointer sizes. You cannot assume uniform hardware. |

> **Why this matters for exams:** Each fallacy motivates a specific DFS design decision. When asked "why does DFS X have feature Y?", trace it back to which fallacy Y addresses.

---

## Part 3: Remote Procedure Calls (RPC)

### What Is RPC?

**RPC (Remote Procedure Call)** is the abstraction that lets a client call a function as if it were local, while the actual execution happens on a remote server.

**Goal:** Keep the client code looking like this:
```c
read(fd, buf, sizeof buf);   // caller wants this to look local
```
And have the RPC framework handle all the networking transparently.

---

### How RPC Differs from Local Calls

| Property | Local call | RPC |
|----------|-----------|-----|
| **Address space** | Shared — caller and callee share pointers | **Separate** — a pointer from the client is meaningless on the server |
| **Call by reference** | Works — pass a pointer, callee reads/writes it | **Broken** — must copy data over the network, not pass pointers |
| **Failure modes** | Crashes are local, obvious | Call can fail invisibly (network drop, server crash) |
| **Latency** | Nanoseconds | Milliseconds (network round-trip) |
| **Data representation** | Same CPU architecture | May differ — endianness, pointer width, struct padding |

**Hard modularity:** Because the caller and callee do not share address space, a bug in the callee cannot corrupt the caller's memory — unlike a shared-library call. This is a meaningful isolation benefit.

**Endianness problem:** x86/x86-64 is **little-endian** (least significant byte first). SPARC-64 and most network protocols use **big-endian** (most significant byte first, also called "network byte order"). A 32-bit integer `0x01020304` is stored as bytes `01 02 03 04` on a big-endian machine but `04 03 02 01` on a little-endian machine. If you send raw bytes without conversion, the other side misinterprets the value.

---

### Marshalling and Unmarshalling

**Marshalling** is the process of converting in-memory data structures into a flat byte stream suitable for network transmission. **Unmarshalling** (or deserialization) is the reverse.

```
Caller machine                     Callee machine
┌────────────────────┐             ┌────────────────────┐
│  tree / struct     │             │  reconstructed     │
│  in memory         │             │  tree / struct     │
│        ↓           │             │        ↑           │
│   marshalling      │──[bytes]──→│   unmarshalling    │
│  (serialize)       │  network    │  (deserialize)     │
└────────────────────┘             └────────────────────┘
```

Marshalling must handle:
- **Endianness conversion** (e.g., `htonl()` / `ntohl()` for network byte order)
- **Struct padding/alignment differences** between architectures
- **Pointer replacement** — replace pointers with the actual data they point to (deep copy)
- **Complex data structures** — trees, linked lists must be linearized (e.g., DFS traversal to a flat array)

Common marshalling formats: **XDR** (used by NFS), **Protocol Buffers**, **JSON**, **MessagePack**, **Cap'n Proto**.

---

### RPC Performance: The Sequential Round-Trip Problem

Each RPC requires a full network round-trip:

```
Client                      Server
  |                            |
  |──[read request]──────────→|   ← latency = call setup + marshalling +
  |←─[read response]──────────|              speed-of-light delay + unmarshal
  |                            |
  |──[read request]──────────→|
  |←─[read response]──────────|
  |                            |
  |──[read request]──────────→|
  |←─[read response]──────────|
```

If each round-trip takes 1 ms and you make 1,000 sequential reads, that's **1 full second of stalled waiting** — catastrophic for an application that locally would take microseconds.

**Three techniques to fix this:**

| Technique | How | Benefit | Catch |
|-----------|-----|---------|-------|
| **Async calls** | Tag requests with IDs; send many without waiting for responses; match responses to requests as they arrive | Hides latency — can have many in-flight simultaneously | Requests must be independent (can't use result of request N as input to N+1) |
| **Client-side caching** | Cache file data and metadata locally; serve reads from cache without contacting server | Eliminates network RTT for repeated reads | Requires cache coherence — cache may become stale if another client writes |
| **Prefetching** | If the client is reading sequentially through a file, pre-fetch the next block(s) before they are requested | Eliminates stall time — data arrives before needed | Wastes bandwidth if prediction is wrong; more important than local prefetch because network latency is so high |

---

## Part 4: RPC Failure Modes and Semantics

### The Core Problem: Ambiguous Failure

When an RPC gets no response, there are multiple possible causes that look identical from the client's perspective:

- Network dropped the request (server never received it)
- Server received the request but the response was dropped
- Server is running slowly (not crashed, just busy)
- Server has crashed permanently

The client **cannot tell the difference** without additional mechanisms. This is the fundamental challenge of distributed system reliability.

### The Three RPC Delivery Semantics

| Semantic | Behavior | When to use |
|----------|---------|------------|
| **At-least-once** | If no response received, **keep retrying** until one arrives. The operation may execute multiple times on the server. | Only safe for **idempotent** operations — operations where executing twice gives the same result as executing once (e.g., read, overwrite with same data, stat). |
| **At-most-once** | If no response, **return an error to the caller**. The application handles the ambiguity. The operation executes 0 or 1 times. | When the caller can handle errors and decide what to do. Safe for non-idempotent operations. |
| **Exactly-once** | Guarantee the operation executes exactly once, no matter what. | Theoretically possible but requires expensive multi-phase commit handshaking. Rarely used in file systems due to cost. |

> **Idempotency** — critical concept: an operation is idempotent if `f(f(x)) = f(x)`. For RPC:
> - `READ(offset, length)` → idempotent ✅ (reading the same bytes twice gives the same result)
> - `WRITE(offset, data)` → idempotent ✅ (writing the same data to the same place twice is harmless)
> - `APPEND(data)` → **not** idempotent ❌ (appending twice doubles the data)
> - `INCREMENT(counter)` → **not** idempotent ❌ (incrementing twice gives wrong result)

**Why at-least-once + idempotent operations = NFS's design choice:** NFS makes its operations idempotent wherever possible, so the client can safely retry failed calls without worrying about double-execution.

**How to detect corruption:** Use a strong **checksum** (CRC32, SHA) on the message. If the received checksum doesn't match the recomputed one, the message is corrupt → retransmit. (Not the same as encryption — checksums detect accidental corruption; encryption prevents intentional tampering.)

---

## Part 5: NFS — Network File System

### Overview

**NFS (Network File System)** was designed by Sun Microsystems (1984) originally for LANs, later extended for WANs and the internet. It is still the dominant network file system in Unix/Linux environments.

**Architecture:**
```
Client application
      ↓  (syscall: open, read, write)
  VFS (Virtual File System layer in kernel)
      ↓  (dispatch to appropriate FS)
  NFS client module (in kernel)
      ↓  (network)
  NFS server (on remote machine)
      ↓
  Local file system (ext4, etc.) on server's disk
```

The **VFS layer** is what makes NFS transparent to applications — `read()` looks the same whether the file is local or remote.

### NFS Protocol Operations

NFS exposes operations that deliberately mirror Linux system calls, but with important differences (stateless server, file handles instead of fds):

| Operation | Description |
|-----------|-------------|
| `LOOKUP(dir_fh, name)` → `fh + attrs` | Resolve a name in a directory to a file handle + attributes |
| `CREATE(dir_fh, name, attrs)` | Create a new file |
| `MKDIR(dir_fh, name, attrs)` | Create a new directory |
| `REMOVE(dir_fh, name)` | Delete a file |
| `RMDIR(dir_fh, name)` | Delete a directory |
| `READ(fh, offset, count)` → `data` | Read bytes from a file |
| `WRITE(fh, offset, data)` → `count` | Write bytes to a file |

---

### NFS File Handles

An **NFS file handle (fh)** uniquely identifies a file on a specific server. It is the network analogue of an inode number — but more robust.

A file handle contains three fields:

```
┌────────────┬───────────┬──────────────┐
│  device #  │  inode #  │  serial #    │
└────────────┴───────────┴──────────────┘
```

| Field | Purpose |
|-------|---------|
| **Device #** | Index into the server's mount table — identifies which mounted file system the file lives on |
| **Inode #** | The inode number within that file system — uniquely identifies the file |
| **Serial #** | A generation counter that detects inode number reuse |

**Why the serial number?** Consider:
1. Process A opens file `foo.txt` → gets file handle with inode #42
2. `foo.txt` is deleted → inode #42 is freed by the OS
3. Process B creates a new file `bar.txt` → OS reuses inode #42
4. Process A's old file handle (inode #42) now points to `bar.txt` — **wrong file!**

The serial number solves this: every time an inode number is freed and reused, the serial number is incremented. Process A's handle has serial N; the new file has serial N+1. The server detects the mismatch and returns `ESTALE`.

**`ESTALE` error:** "Stale file handle" — the file this handle referred to no longer exists (or a different file now has the same inode number). The application must re-open the file by name to get a fresh handle.

---

### The Stateless Server Design

**NFS servers are designed to be stateless** — the server does not store any per-client session state in RAM. All state needed to serve a request is contained in the request itself (the file handle, offset, data).

**Why stateless?**
- If the server crashes and restarts, clients can simply retry their requests — no session state was lost because none was stored
- Clients can switch to a different server replica seamlessly
- The server doesn't need to track which clients have what files open

**Contrast with stateful servers:** A stateful server might track "client X has file Y open for writing at offset Z." If the server crashes, this information is lost and clients are stuck.

**Implication:** NFS uses file handles (not file descriptors) because file descriptors are stateful (they track the current read/write offset in the kernel). Every NFS READ and WRITE call includes the explicit byte offset.

---

### NFS Consistency Model: Close-to-Open

NFS's consistency model is **weaker than POSIX**:

**POSIX would require:** If client A writes to a file, client B reading the same file immediately after should see A's write.

**NFS guarantees only close-to-open consistency:**
- When you `close()` a file, NFS flushes all cached writes to the server. After close returns, the data is on the server.
- When another client `open()`s the file, it gets the current server version.
- **Between open and close:** NFS may cache reads and writes locally. Other clients may not see your writes yet. You may not see others' writes.

```
Client 1: open("data.txt") ... write(...) ... [still open — data may be in cache]
Client 2: read("data.txt") → may get stale data from server ← NFS allows this!
Client 1: close("data.txt") → data now flushed to server ← guaranteed after this
Client 2: open("data.txt") ... read(...) → now sees client 1's data ← guaranteed
```

**Why this exists:** Flushing every write immediately to the server would be prohibitively slow (every `write()` syscall would become an RPC). Caching writes and flushing at `close()` gives much better performance at the cost of weaker mid-session consistency.

**The delayed failure problem:** `close()` returns -1 on I/O error. This means you may write data successfully but only discover the write failed when you close the file. Many applications forget to check the return value of `close()`.

```c
// Common bug — checking write but not close:
write(fd, buf, size);   // returns size bytes written (from cache, "success")
close(fd);              // THIS may return -1 if the network write actually failed
                        // most code ignores close's return value — silent data loss!
```

> **Exam trap:** Always check the return value of `close()` for files written over NFS. The error from the network write may not surface until `close()`.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| DFS components | Name nodes (metadata), data nodes (content), clients, network |
| DFS architectures | Centralized, distributed, hierarchical, P2P — tradeoffs between simplicity and scalability |
| Fallacy #1 | Network is reliable → design for packet loss |
| Fallacy #2 | Latency is zero → batch operations, cache, prefetch |
| Fallacy #4 | Network is secure → encrypt, authenticate |
| Fallacy #8 | Network homogeneous → handle endianness and arch differences |
| RPC vs local call | No shared address space; no call-by-reference; endianness issues; failure is ambiguous |
| Marshalling | Serialize data structures to bytes (handle endianness, pointers, layout) |
| At-least-once | Retry until success; safe only for idempotent operations |
| At-most-once | Return error on no response; app handles ambiguity; safe for non-idempotent ops |
| Idempotent | f(f(x)) = f(x); READ and overwrite-WRITE are idempotent; APPEND and INCREMENT are not |
| Exactly-once | Correct but expensive; requires multi-phase commit; rarely used in FS |
| NFS LOOKUP | Returns file handle + attributes; the core path-resolution RPC |
| NFS file handle | device# + inode# + serial# — serial# detects inode reuse |
| ESTALE | Stale file handle — inode was deleted and serial# mismatches; must re-open by name |
| Stateless NFS server | No per-client RAM state; server can crash and restart transparently; clients retry |
| Close-to-open consistency | Writes guaranteed to reach server after `close()`; reads between open/close may be stale |
| Check `close()` return value | NFS write errors may only surface at `close()` time — silent data loss if not checked |
| Async RPC | Tag requests with IDs; send many in-flight; collect responses as they arrive |
| Client-side cache | Eliminates RTT for repeated reads; must handle staleness (another client wrote) |
| Prefetching | More important in DFS than local FS due to high network latency |
