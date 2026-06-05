# Lecture 10 — Deadlock: Causes, Detection, Prevention, and Livelock

---

## Overview

This lecture bridges synchronization (Lecture 9) and file systems. The motivating context is real kernel code for pipe I/O, which naturally requires locks — and those locks can deadlock. We cover:

1. **Deadlock in practice** — how it arises in file system / pipe code
2. **The four Coffman conditions** — what must be true for deadlock to occur
3. **Strategies to deal with deadlock** — detection, prevention, avoidance
4. **Livelock** — a related problem that's even harder to debug

---

## Part 1: How Deadlock Arises in File System / Pipe Code

The kernel's implementation of `read()` and `write()` on a pipe must hold a lock to prevent races on the shared buffer:

```
Kernel: read(pipe fd)
  1. lock(pipe)
  2. copy data from kernel pipe buffer → user memory
  3. increment pipe's read pointer (r)
  4. unlock(pipe)

Kernel: write(pipe fd)
  1. lock(pipe)
  2. copy data from user memory → kernel pipe buffer
  3. increment pipe's write pointer (w)
  4. unlock(pipe)
```

This single-lock design is safe against races. But the moment the system becomes more complex — multiple pipes, multiple threads, more than one lock acquired at a time — deadlock becomes possible.

**Minimal deadlock scenario (two threads, two locks):**

```
Thread 1:  lock(pipe_A) ... lock(pipe_B) ...   ← blocks on pipe_B
Thread 2:  lock(pipe_B) ... lock(pipe_A) ...   ← blocks on pipe_A
```

Thread 1 holds pipe_A and waits for pipe_B. Thread 2 holds pipe_B and waits for pipe_A. Neither can proceed — **deadlock**.

> **Key insight:** Deadlock is not a bug in *using* the lock — both threads are following the lock/unlock protocol correctly. The bug is in the *order* locks are acquired across threads.

---

## Part 2: The Four Coffman Conditions

Deadlock can **only** occur when **all four** of the following conditions hold simultaneously. Eliminating any one of them prevents deadlock.

| # | Condition | Meaning |
|---|-----------|---------|
| 1 | **Mutual Exclusion** | At least one resource is held in a non-sharable mode (only one thread can hold the lock at a time) |
| 2 | **Hold and Wait** | A thread holds at least one resource while waiting to acquire additional resources |
| 3 | **No Preemption** | Resources (locks) cannot be forcibly taken away from a thread; they must be released voluntarily |
| 4 | **Circular Wait** | There exists a cycle in the "waits-for" graph: T1 waits for T2, T2 waits for T3, ..., Tn waits for T1 |

**Deadlock = all four conditions simultaneously.** If any one is broken, deadlock cannot occur.

> **Exam trap:** Starvation and deadlock are different. Starvation means a thread waits forever but *could* theoretically run (it's just never scheduled). Deadlock means threads are *logically* blocked — no sequence of events can break the cycle without external intervention.

---

## Part 3: Strategies to Deal With Deadlock

There are four broad approaches: **ignore it**, **detect and recover**, **prevent**, or **avoid**.

---

### Strategy 1: Ignore It (Ostrich Algorithm)

Used more often than you'd think. If deadlocks are rare and the cost of prevention/detection is high, simply restart the system when a deadlock occurs. This is acceptable for single-user workstations but not for servers.

---

### Strategy 2: Detection and Recovery

**Let deadlocks occur, then find and break them.**

The OS maintains a **resource allocation graph** (or **waits-for graph**):
- Nodes = threads and resources
- Edge T → R = "thread T is waiting for resource R"
- Edge R → T = "resource R is held by thread T"
- **A cycle in this graph = deadlock**

**Cycle detection** runs in **O(N)** for a simple implementation (DFS/BFS on the graph, where N = number of threads + resources).

```
Thread 1 holds: [Lock A]        waits for: [Lock B]
Thread 2 holds: [Lock B]        waits for: [Lock A]

Waits-for graph:
  T1 → T2 (T1 waits for B held by T2)
  T2 → T1 (T2 waits for A held by T1)
  CYCLE FOUND → deadlock
```

**Recovery options once detected:**
- **Kill one thread** — terminate a deadlocked thread; the other can proceed. Simplest, but can leave data in inconsistent state.
- **Rollback** — roll the killed thread back to a checkpoint before it acquired the first lock. Requires checkpointing infrastructure.
- **Preempt a resource** — forcibly take a lock from one thread and give it to another. Dangerous unless the application is designed for it.

**Downsides of detection:**
- The detection cycle must run periodically → overhead
- Deadlocks only detected after they occur → threads may have been stuck for a while
- Breaking deadlock may require killing/restarting work

---

### Strategy 3: Prevention

**Eliminate one of the four Coffman conditions structurally.**

| Condition to break | Technique | Tradeoff |
|-------------------|-----------|----------|
| **Mutual Exclusion** | Use lock-free data structures (CAS-based) | Very hard to implement correctly |
| **Hold and Wait** | Acquire all needed locks at once, atomically | Must know all locks in advance; reduces parallelism |
| **No Preemption** | Allow OS to forcibly revoke locks | Requires rollback ability; complex |
| **Circular Wait** | **Global lock ordering** | Most practical; easy to enforce |

**Global lock ordering** (most practical and commonly used):

Assign a total order to all locks in the system (e.g., by memory address, by assigned ID number). All threads must acquire locks in **increasing order** only.

```c
// BAD — can deadlock depending on call order:
void transfer(account *src, account *dst) {
    lock(src);          // Thread 1 does transfer(A, B)
    lock(dst);          // Thread 2 does transfer(B, A) → deadlock
    ...
}

// GOOD — always lock lower-address account first:
void transfer(account *src, account *dst) {
    account *first  = (src < dst) ? src : dst;
    account *second = (src < dst) ? dst : src;
    lock(first);
    lock(second);
    do_transfer(src, dst);
    unlock(second);
    unlock(first);
}
```

This works because if every thread acquires locks in the same global order, there can be no cycle in the waits-for graph. Thread 1 might hold A and wait for B; Thread 2 might be waiting for A, but it cannot hold B (B > A, and you can only hold B if you already hold everything below B).

**"Release all locks and start over" (backoff):**

An alternative approach that the notes mention: if a thread cannot acquire all the locks it needs, it **releases everything it currently holds** and retries from scratch.

```
Thread 1:
  lock(A)
  try_lock(B) → FAILS (held by Thread 2)
  unlock(A)      ← release everything
  // wait a bit, retry from start

Thread 2:
  lock(B)
  try_lock(A) → FAILS (held by Thread 1 ... or now free)
  unlock(B)
  // wait a bit, retry
```

**Downside:** If both threads always retry at the same time, they can repeatedly collide → **livelock** (see Part 4). A random backoff delay breaks this.

**The burden falls on the application.** The OS doesn't enforce lock ordering — it's up to the programmer to consistently follow the ordering convention.

---

### Strategy 4: Avoidance (Banker's Algorithm)

The OS dynamically decides at runtime whether granting a resource request could lead to an unsafe state. The **Banker's Algorithm** is the classic example:

- Each thread declares its maximum resource needs upfront
- The OS only grants a request if the resulting state is "safe" (there exists some order in which all threads can finish)
- If the state would be unsafe → block the requesting thread until it's safe

**Not used in practice** for general-purpose locking because:
- Threads rarely know their maximum needs in advance
- The algorithm is O(n²·m) per request (n threads, m resource types) — too expensive
- It's a theoretical benchmark, not a practical tool

---

## Part 4: Livelock

**Livelock** is related to deadlock but different: threads are **not blocked** — they are actively running — but they are making **no useful progress** because they keep responding to each other in a loop.

**Classic example — packet processing:**

```
Pseudocode for a network receive loop:
  get_packet()
  process_packet()      ← processing the packet generates ANOTHER packet
  → get_packet()
  → process_packet()    ← which generates ANOTHER packet
  → ...
```

The system is alive (the CPU is busy), no thread is blocked, but no real work gets done. This is the network equivalent of two people in a hallway who keep stepping to the same side to let each other pass.

**Livelock with lock backoff (degenerate case):**

```
Thread 1:  lock(A), try_lock(B) fails, unlock(A), retry...
Thread 2:  lock(B), try_lock(A) fails, unlock(B), retry...
            → both retry at exactly the same time, forever
```

**Fix:** Introduce **randomized exponential backoff** — each thread waits a random amount of time before retrying. This breaks the synchronization between retries and allows one thread to proceed.

**Livelock vs. Deadlock:**

| Property | Deadlock | Livelock |
|----------|----------|----------|
| Threads blocked? | Yes — cannot run | No — actively running |
| Progress made? | No | No |
| CPU usage | 0% (threads sleep) | 100% (threads spin uselessly) |
| Detection | Cycle in waits-for graph | Hard — looks like normal activity |
| Fix | Kill/rollback, lock ordering | Random backoff, exponential retreat |

> **Exam trap:** Livelock is harder to detect than deadlock because the system appears busy. A deadlock shows up immediately (threads stop responding); livelock can look like a slow system or high CPU load with no output.

---

## Part 5: Other Synchronization Hazards

### Priority Inversion

A high-priority thread waits for a low-priority thread to release a lock. If a medium-priority thread preempts the low-priority thread (because the medium thread has higher priority than the low thread), the low thread never runs, never releases the lock, and the high-priority thread is stuck indefinitely — **despite having the highest priority.**

**Real-world example:** The Mars Pathfinder rover (1997) suffered priority inversion between a low-priority meteorological task and a high-priority bus management task, causing system resets.

**Fix — Priority Inheritance:** When a low-priority thread holds a lock that a high-priority thread is waiting for, temporarily elevate the lock-holder's priority to match the waiter. This lets it finish quickly and release the lock.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| Deadlock definition | Set of threads each waiting for a resource held by another in the set; no external progress possible |
| Four Coffman conditions | Mutual exclusion, hold-and-wait, no preemption, circular wait — ALL four required |
| Break any one condition | Eliminates deadlock; most practical is breaking circular wait via lock ordering |
| Detection: waits-for graph | Build a "T waits for T'" graph; cycle = deadlock; O(N) detection |
| Lock ordering | Assign total order to locks; all threads acquire in increasing order; no cycles possible |
| "Release all and retry" | Alternative to ordering; requires random backoff to avoid livelock |
| Banker's algorithm | Theoretical avoidance algorithm; not practical (requires upfront knowledge, too expensive) |
| Livelock vs. deadlock | Deadlock: threads blocked, 0% CPU; Livelock: threads running, 100% CPU, no progress |
| Livelock fix | Randomized exponential backoff breaks the retry synchronization |
| Priority inversion | High-priority thread stuck behind low-priority lock holder; fix with priority inheritance |
| Kernel pipe locking | read/write each lock(pipe) → copy → unlock; multi-pipe operations can deadlock on lock ordering |
