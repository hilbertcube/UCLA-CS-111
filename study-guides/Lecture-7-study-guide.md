# Lecture 7 — Signals (Continued), Threads, and Scheduling Intro

---

## Overview

This lecture finishes signals and transitions into the core scheduling topics:

1. **Signals in practice** — blocking signals to protect critical sections (the `gzip` example)
2. **Signal tradeoffs** — why signals exist and what makes them hard to use
3. **Processes vs. threads** — what each owns; why threads exist
4. **POSIX thread API** — `pthread_create`, `pthread_join`, and friends
5. **Scheduling mechanisms** — cooperative vs. preemptive; how the OS reclaims the CPU
6. **I/O with threads** — blocking, polling, busy-waiting
7. **Scheduling policy and metrics** — utilization, the rwlock vs. spinlock example, scheduling scale

---

## Part 1: Signals in Practice — The `gzip` Example

### The Problem

```bash
$ gzip foo
# User presses Ctrl+C in the middle
```

`gzip` reads `foo`, compresses it, writes `foo.gz`, then deletes `foo`. If SIGINT arrives partway through:
- `foo` might still exist (uncompressed, not yet deleted)
- `foo.gz` might exist but be **incomplete** (a partial, corrupt compressed file)
- The user is now left with a corrupt output file and possibly a lost original

**Goal:** If interrupted, clean up the partial output file (`foo.gz`) before exiting.

### The Solution — Signal Blocking

```c
char *fname = "foo";
char *outname = "foo.gz";
int ifd, ofd;

void handle(int sig) {
    if (outname)
        unlink(outname);          // delete the partial output file
    signal(SIGINT, SIG_DFL);      // restore default SIGINT behavior
    kill(getpid(), sig);          // re-raise the signal to terminate properly
}

int main() {
    ifd = open(fname, O_RDONLY);
    signal(SIGINT, handle);       // install cleanup handler

    // CRITICAL SECTION: open + create output file must be atomic w.r.t. SIGINT
    // If SIGINT fires after open() but before we set outname, handle() won't unlink it
    block_sigint();               // block SIGINT during this window
    ofd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    unblock_sigint();             // safe to deliver SIGINT again

    compress(ifd, ofd);           // compress; handler can fire here
    close(ofd);

    signal(SIGINT, SIG_DFL);      // restore default behavior
    unlink(fname);                // delete original
    exit(0);
}
```

**Why block SIGINT around the `open()`?**

Consider: SIGINT fires *after* `open(outname)` creates the file but *before* `outname` is set. The handler checks `if (outname)` — but `outname` isn't set yet — so it doesn't clean up. The partial file is left behind.

Blocking SIGINT during that window ensures the signal is **deferred**, not lost — it will be delivered as soon as we unblock. The critical section is tiny (just one `open()` call), so the block time is negligible.

> **Key principle:** Block signals during critical sections where partial state would be dangerous. Keep the block window as short as possible.

### `pthread_sigmask` — Signal Mask API

```c
#include <signal.h>

int pthread_sigmask(int how, const sigset_t *newmask, sigset_t *oldmask);
// how:
//   SIG_BLOCK   — add newmask signals to the blocked set
//   SIG_UNBLOCK — remove newmask signals from the blocked set
//   SIG_SETMASK — replace the blocked set with newmask entirely

// Build signal sets:
sigemptyset(&mask);        // empty set
sigaddset(&mask, SIGINT);  // add SIGINT to set
sigfillset(&mask);         // all signals
```

**Signal mask vs. signal handler:** The mask controls which signals are *delivered* right now. A blocked signal is **pending** — it's queued and will be delivered when unblocked. It is not lost. Installing a handler (via `signal()`) controls what *happens* when a signal is delivered.

**`oldmask` parameter:** On return, `oldmask` is filled with the previous mask. This lets you save and restore the mask:

```c
sigset_t old;
pthread_sigmask(SIG_BLOCK, &mask, &old);   // block, save old
// ... critical section ...
pthread_sigmask(SIG_SETMASK, &old, NULL);  // restore exactly the old mask
```

> **Exam trap:** Blocking a signal does not discard it — it defers it. If SIGINT is blocked and the user presses Ctrl+C, the signal is queued. When you unblock SIGINT, it is delivered immediately. This is different from ignoring (`SIG_IGN`), which discards the signal entirely.

### Why `kill(getpid(), sig)` in the Handler?

The handler restores `SIG_DFL` then re-raises the signal with `kill(getpid(), sig)`. Why?

- If the handler just called `exit(1)`, the process exits with status 1, but the **shell would not know it was killed by SIGINT** — it would think the process exited normally.
- By restoring `SIG_DFL` and re-raising, the signal kills the process the normal way. The shell sees "killed by SIGINT" and can react appropriately (e.g., stopping a pipeline).

This is the correct, idiomatic pattern for "do cleanup then die the way the signal intended."

---

## Part 2: Signals — Pros and Cons

### Why Signals Exist (Pros)

| Reason | Explanation |
|--------|-------------|
| **Asynchronous events are real** | Power failure, Ctrl+C, timer expiry, child death — these happen at unpredictable moments. The system needs a way to notify processes. |
| **Polling is painful** | Without signals, you'd have to constantly check `/dev/power`, periodically call `waitpid(WNOHANG)`, etc. Every function in your program needs to be modified to do these checks. |
| **Pipeline integration** | SIGPIPE lets pipelines terminate cleanly without every program manually checking for dead readers. |

### Why Signals Are Hard (Cons)

| Problem | Explanation |
|---------|-------------|
| **Finding critical sections is non-obvious** | You must identify every window where a signal could corrupt state. This is a form of concurrency analysis that's easy to get wrong. |
| **Async-signal-safety restrictions** | Signal handlers can only call a small subset of library functions. Most of the C standard library (`malloc`, `printf`, `fopen`) is not safe. This makes handlers hard to write. |
| **Nested signal handlers** | A signal can arrive while a signal handler is running. If SIGINT arrives while the SIGINT handler is running, behavior is undefined unless you block signals in the handler. |
| **Race conditions with shared state** | Any global variable accessed by both the main code and a handler creates a race. The variable must be declared `volatile sig_atomic_t` to be safe. |

---

## Part 3: Processes vs. Threads

### Why Threads?

The CPU needs to do multiple things "at once":
- While waiting on I/O (disk read, network), run other work
- Handle multiple users or requests simultaneously
- Use multiple CPU cores in parallel

**Processes** can do this, but they're heavyweight:
- `fork()` copies the entire address space (expensive, even with CoW)
- Processes communicate only through IPC (pipes, shared memory, sockets) — explicit and slow
- Context switching between processes requires a full address space switch (TLB flush, etc.)

**Threads** are lightweight: they share the same address space and file descriptor table but have their own registers, stack, and some thread-local state.

> **"We get performance but lose isolation."** Threads share memory — a bug in one thread can corrupt another thread's data. With processes, address space isolation prevents this.

### What a Process Owns

| Resource | Description |
|----------|-------------|
| **Address space** | Virtual memory: heap, code, data, memory-mapped files |
| **File descriptor table** | All open fds (inherited by child threads and `fork()`ed children) |
| **Signal handler table** | Handlers installed with `signal()`/`sigaction()` — shared by all threads |
| **Working directory** | Current directory for relative path resolution |
| **Root directory** | Used by `chroot` jails |
| **umask** | Default permission mask for new files |
| **UID / GID** | User and group credentials |
| **PID** | Process identifier |

### What a Thread Owns (Per-Thread State)

| Resource | Description |
|----------|-------------|
| **Registers** | PC (program counter), stack pointer, general-purpose registers — entirely per-thread |
| **Stack** | Each thread has its own call stack, allocated in the process's virtual address space |
| **Signal mask** | Which signals are currently blocked — per-thread (set via `pthread_sigmask`) |
| **`errno`** | The error number from the last failed syscall — per-thread (via Thread-Local Storage) |
| **TID** | Thread identifier (within the process) |

**Why is `errno` per-thread?**

`errno` is declared as `extern int errno` in classic C. But if `errno` were a single global, two threads calling `read()` simultaneously could overwrite each other's `errno` before the caller checks it. The compiler and runtime implement `errno` as a **thread-local variable** (TLS — Thread-Local Storage): each thread has its own independent copy. Syntactically it looks like a global, but reads/writes go to the current thread's copy.

> **Exam trap:** `errno` looks like a global `int` but is actually thread-local. Never share `errno` between threads.

---

## Part 4: POSIX Thread API

```c
#include <pthread.h>
// Link with: gcc -lpthread
```

### Creating and Joining Threads

```c
// Create a thread:
int pthread_create(
    pthread_t *tid,                 // output: thread ID
    const pthread_attr_t *attr,     // NULL for defaults
    void *(*thread_fn)(void *),     // function to run
    void *arg                       // argument to thread_fn
);

// Wait for a thread to finish:
int pthread_join(
    pthread_t tid,     // thread to wait for
    void **status      // output: value passed to pthread_exit()
);
// Analogous to waitpid() for child processes
```

**Example:**

```c
void *worker(void *arg) {
    int id = *(int *)arg;
    printf("Thread %d running\n", id);
    return NULL;  // equivalent to pthread_exit(NULL)
}

int main() {
    pthread_t t1, t2;
    int id1 = 1, id2 = 2;
    pthread_create(&t1, NULL, worker, &id1);
    pthread_create(&t2, NULL, worker, &id2);
    pthread_join(t1, NULL);  // wait for t1
    pthread_join(t2, NULL);  // wait for t2
    return 0;
}
```

### Exiting and Cancelling

```c
_Noreturn void pthread_exit(void *status);
// Terminate the calling thread; passes status to pthread_join()
// Does NOT terminate the process (unlike exit())

int pthread_cancel(pthread_t tid);
// Request cancellation of thread tid
// Thread is cancelled at the next "cancellation point" (blocking syscall, etc.)
// Does not immediately kill the thread

int pthread_kill(pthread_t tid, int sig);
// Send signal sig to thread tid within the same process
// Analogous to kill(pid, sig) for processes
```

> **Exam trap:** If **any** thread calls `exit()` or crashes (unhandled signal, unhandled SIGSEGV), the **entire process** terminates — all other threads die too. `pthread_exit()` terminates only the calling thread.

### Thread Relationship to Processes

| Operation | Processes | Threads |
|-----------|-----------|---------|
| Create | `fork()` | `pthread_create()` |
| Wait for completion | `waitpid()` | `pthread_join()` |
| Kill | `kill(pid, SIGKILL)` | `pthread_cancel()` or `pthread_kill()` |
| Terminate self | `exit()` | `pthread_exit()` |
| ID | `getpid()` → PID | `pthread_self()` → TID |

---

## Part 5: Scheduling Mechanisms

### The Problem

With `n` CPU cores and more than `n` runnable threads, the OS must decide:
- Which threads get to run? (policy)
- How does the OS actually switch between them? (mechanism)

### Mechanism 1: Cooperative Scheduling

**Idea:** Threads voluntarily give up the CPU when they're willing to let others run.

```c
sched_yield();   // "I'm done with my time slice; let someone else run"
```

**Properties:**
- Simple: no hardware support needed, no preemption logic
- Requires threads to cooperate — a greedy or buggy thread never yields → other threads starve
- Works well in controlled environments (coroutines, some embedded systems)
- **Does not work for general-purpose OS** — you can't trust application code to yield

**I/O with cooperative threading:**

When a thread needs to do I/O (which blocks), it has options:

```c
// Option 1: Blocking I/O
read(...);   // thread blocks until data available
             // should yield the CPU while waiting — but doesn't automatically

// Option 2: Polling with yield
while (device_busy())
    sched_yield();   // check periodically, yield between checks
// Not busy-waiting (yields CPU), but still adds latency

// Option 3: Busy-waiting
while (device_busy())
    continue;        // spin — wastes CPU, but lowest latency
```

### Mechanism 2: Preemptive Scheduling

**Idea:** The OS forcibly interrupts threads — it doesn't ask for the CPU back, it takes it.

**How?** The hardware timer fires a periodic interrupt (typically every 1–10 ms). This interrupt transfers control to the kernel regardless of what user code is running. The kernel's interrupt handler then decides which thread to run next.

```
Thread A running:
  instruction 1
  instruction 2
  instruction 3         ← TIMER INTERRUPT fires here
  [kernel: save A's registers, pick thread B, restore B's registers, return]
  // Thread B is now running where it left off
```

This is why preemptive scheduling has **critical section issues** — a thread can be interrupted at any instruction, including in the middle of a multi-step operation. This is exactly why we need mutexes and locks.

**Preemptive scheduling is what general-purpose OSes (Linux, Windows, macOS) use.**

| Property | Cooperative | Preemptive |
|----------|-------------|-----------|
| CPU reclaim | Thread must yield | Timer interrupt forces it |
| Greedy threads | Can starve others | Cannot — get preempted |
| Critical section issues | None (yield is explicit) | Yes — must protect with locks |
| Implementation cost | Low | Higher (interrupt handling) |
| Used in | Coroutines, embedded | Linux, Windows, macOS |

---

## Part 6: Scheduling Policy and Metrics

### Scheduling Scale

Scheduling decisions happen at different timescales:

| Timescale | Name | Question |
|-----------|------|---------|
| **Long-term** | Admission scheduler | Which new threads/processes are allowed into the system at all? (e.g., reject new jobs when load is too high) |
| **Medium-term** | Memory scheduler | Which processes reside in RAM vs. swapped to disk? (virtual memory management intersects here) |
| **Short-term** | CPU scheduler | Which runnable thread gets the CPU right now? (runs every few milliseconds) |

The "short-term scheduler" is what we normally mean by "the scheduler."

### Utilization — A Key Metric

**CPU utilization** = fraction of time the CPU is doing useful work (running application code), as opposed to being idle or running the scheduler itself.

**Utilization example from lecture:**

Setup:
- 4 threads, each accessing one shared variable
- Each thread: reads 90% of the time, writes 10% of the time

**With a single spinlock:**
```
lock(); read_or_write(); unlock();
```
Only **1 thread** can hold the lock at a time → only 1 thread does work at a time → **25% utilization** (1/4 of threads are doing useful work; the other 3 are spinning).

**With a read-write lock (`rwlock`):**
- 90% of the time, all threads want to read → multiple readers can hold the lock simultaneously
- Probability all 4 threads want to read: 0.9⁴ ≈ 65.6% → when this happens, all 4 threads work → 100% effective utilization in this scenario
- 10% of the time, someone wants to write → serialize like before

**Calculation:**
- P(all 4 reading) = 0.9⁴ ≈ 0.656 → 100% of 4 threads working = 4 units of work
- P(at least one writer) ≈ 0.344 → only 1 thread works = 1 unit of work
- Expected work per time unit: 0.656 × 4 + 0.344 × 1 ≈ 2.97 threads doing useful work
- Utilization ≈ 2.97 / 4 ≈ **~74%** (the lecture approximates this as ~80%)

**The lesson:** Simply changing the locking algorithm — with zero changes to application logic — dramatically improves utilization. The right synchronization primitive is not just about correctness; it's about performance.

> **General principle:** Prefer read-write locks when reads vastly outnumber writes. The extra complexity is worth it for high-read workloads.

### Other Scheduling Metrics

| Metric | Definition | Who it matters to |
|--------|-----------|-------------------|
| **Utilization** | % of time CPU does useful work | System owner / datacenter |
| **Throughput** | Jobs completed per second | System owner |
| **Wait time** | Time from arrival to first run | End user |
| **Turnaround time** | Time from arrival to completion | End user |
| **Response time** | Time from request to first response | Interactive user |
| **Fairness** | Equal share of CPU for equal-priority threads | All users |

> **Lecture 8 goes deeper on these metrics with specific algorithms (FIFO, SJF, Round Robin).** This lecture establishes the vocabulary.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| Signal blocking purpose | Prevents signal delivery during critical sections where partial state is dangerous |
| Pending vs. ignored | Blocked signal: deferred, delivered when unblocked. Ignored (SIG_IGN): discarded forever |
| `pthread_sigmask` how flags | SIG_BLOCK adds; SIG_UNBLOCK removes; SIG_SETMASK replaces |
| Re-raise in handler | `signal(sig, SIG_DFL); kill(getpid(), sig);` — dies with correct signal status for shell |
| Signal cons | Non-obvious critical sections; async-signal-safety restrictions; nested handler issues |
| Process vs. thread ownership | Process: address space, fd table, signal handlers, uid, pid; Thread: registers, stack, signal mask, errno, tid |
| `errno` is thread-local | Each thread has its own `errno` via TLS — not a true global despite appearance |
| `pthread_exit` vs `exit` | `pthread_exit`: terminate calling thread only; `exit`: terminate entire process |
| Any thread crash | Kills the whole process (unhandled signal in any thread = process dies) |
| `pthread_create` ~ `fork` | `pthread_join` ~ `waitpid`; `pthread_kill` ~ `kill`; `pthread_exit` ~ `_exit` |
| Cooperative scheduling | Thread yields voluntarily; simple but greedy thread starves others |
| Preemptive scheduling | Timer interrupt forces CPU switch; requires locks for critical sections |
| `sched_yield()` | Voluntary yield in cooperative or hybrid scheduling |
| Busy-waiting vs. polling+yield | Busy-wait: lowest latency, wastes CPU; polling+yield: some latency, but gives up CPU |
| Scheduling scale | Long-term: admission; Medium-term: RAM residency; Short-term: which thread runs now |
| Utilization: spinlock vs rwlock | Spinlock: 25% (1/4 threads work); rwlock: ~74–80% (readers run in parallel) |
| Why rwlock wins | Multiple concurrent readers; only writers serialize — huge win when reads dominate |
