# Lecture 9 — Synchronization: Implementing Locks, Blocking, and Deadlocks

---

## Overview

This lecture goes deeper on *how* synchronization primitives are actually built. The key progression:

1. **What do we need?** — Goals of synchronization
2. **Naive attempts** — why obvious software-only approaches fail
3. **Hardware atomics** — the actual solution (`xchg`, `CAS`)
4. **Lock granularity** — coarse-grained vs. fine-grained
5. **Spinning vs. blocking** — spinlocks vs. blocking mutexes
6. **Higher-level abstractions** — semaphores, condition variables
7. **Deadlock** — when everything goes wrong

---

## Part 1: Synchronization Goals

Multiple CPU cores all pull from a **shared address space** (RAM). They can read and write the same memory locations simultaneously. This creates two fundamental problems:

### Problem 1: Sequence Coordination

Some operations must happen in a specific order, even across different threads.

```
Thread 1:  A11 → A12 → A13
Thread 2:  A21 → A22 → A23 → A24
Thread 3:  A31 → A32 → A33
           ——————————————————→ time
```

**Sequence coordination** means enforcing "before" relationships (`<`) between operations in different threads. Example: "A11 must complete before A12 starts" — even across thread boundaries, we sometimes need these guarantees.

### Problem 2: Isolation

Operations from different threads must not interfere. If thread 1 is doing A12 and thread 2 is doing A22 simultaneously, they must not corrupt each other's data.

The solution is **atomicity** — certain sequences of operations must appear to execute as an indivisible "all or nothing" unit. No other thread can observe the intermediate states.

### Three Goals of Synchronization (Exam must-know)

| Goal | Meaning |
|------|---------|
| **Consistency** | Shared data is always in a valid state |
| **Parallelism** | Threads can make progress simultaneously where safe |
| **Clarity/Safety** | The synchronization logic is understandable and correct |

**Worked examples of the right tool for the job:**

| Operation | Tool | Why |
|-----------|------|-----|
| `deposit` / `withdraw` on one account | 1 lock per account | Only one shared variable |
| `transfer` between two accounts | 2 locks | Must protect both accounts atomically |
| Bank audit (read all accounts) | Stop-the-world lock | Must see a consistent snapshot; no operations can proceed |

---

## Part 2: Implementing Locks — The Hard Problem

### Naive Attempt: Software-Only "Lock"

```c
typedef long mutex_t;

void lock(mutex_t *m) {
    while (*m)       // wait while locked
        continue;
    *m = true;       // claim the lock
}

void unlock(mutex_t *m) {
    *m = false;
}

void usage()
{
    // Initialize (unlocked = 0/false)
    mutex_t my_lock = 0; // ( *my_lock is not null)

    // In your code:
    lock(&my_lock);
    // ... critical section ...
    unlock(&my_lock);
}
```

**This is completely broken.** Here's why:

Why This Implementation Is Broken
The lock/unlock cycle is not atomic. Two threads can both pass the while (*m) check before either sets *m = true, so both enter the critical section simultaneously — defeating the entire purpose.

```
Thread A: reads *m == 0  ✓ (exits loop)
Thread B: reads *m == 0  ✓ (exits loop)  ← race!
Thread A: sets *m = true
Thread B: sets *m = true
// Both threads now hold the "lock"
```

This is the classic **check-then-act** race condition.

---

### The Lock Object Size Problem

You might think the solution is to make the lock object larger or smaller. Both fail:

**Too big — multi-word lock object:**
```c
typedef char[10] mutex_t;
strcpy(m, "lock");   // "acquire"
```
`strcpy` copies one byte at a time. It is **not atomic**. Another thread could read or modify `m` halfway through the copy, leaving it in a corrupted intermediate state.

**Too small — single-bit lock:**
```c
typedef struct {
    unsigned locked : 1;
    // other stuff
} lock_t;
```
Reading and setting a single bit still requires a load-modify-store sequence. The hardware cannot read and write a single bit atomically — it must load the whole word, modify the bit, and store the word back. **That sequence is not atomic.**

> **Key insight:** The lock object must fit in exactly one machine word (e.g., 64-bit integer) that can be **loaded and stored atomically** by the hardware. But even that isn't enough — we need a way to *atomically test and modify* it in a single instruction.

---

### The Real Problem: Atomicity at the Hardware Level

Even with a single 64-bit word, the naive code:

```arm
# What the compiler generates:
L1:
    testb  (%rsi)       # test: is lock held?
    jne    L1           # if yes, loop
    movb   $1, (%rsi)   # set: claim the lock
```

The `testb` and `movb` are two separate instructions. **The hardware can interleave another thread's instructions between them.** We need an instruction that atomically tests AND sets in one uninterruptible operation.

**The answer: ask the hardware for help.**

---

## Part 3: Hardware Atomic Instructions

Modern CPUs provide special instructions that perform read-modify-write operations **atomically** — no other processor can observe or interrupt the operation between the read and the write.

### Solution 1: `xchg` (Exchange)

`xchg` atomically **swaps** the contents of a register with a memory location:

```c
// C-level pseudocode (actually implemented in assembly)
long xchg(long *p, long new) {
    long old = *p;
    *p = new;
    return old;
    // asm: xchgq %rdi, (%rsi)
}
```

The x86 `xchg` instruction is **implicitly locked** — it always has the `LOCK` prefix semantics. No explicit `LOCK` prefix needed.

> **Performance note:** `xchg` is roughly **10× slower** than a regular `mov` because it must coordinate with the cache coherence protocol across all CPU cores to ensure exclusive ownership of the cache line.

**Correct spinlock using `xchg`:**

```c
typedef long mutex_t;   // 0 = unlocked, 1 = locked

void lock(mutex_t *m) {
    while (xchg(m, 1))   // atomically set to 1, returns old value
        continue;         // if old value was 1, someone else had it → keep spinning
}

void unlock(mutex_t *m) {
    *m = 0;   // plain store is fine for unlock (only one thread can be here)
}
```

**Why this works:** `xchg(m, 1)` atomically sets `*m = 1` and returns the old value. If it returns 0, we just changed it from unlocked to locked — we won the race. If it returns 1, it was already locked — we didn't change anything meaningful, and we spin. The exchange itself is indivisible, so two threads cannot both see a 0.

---

### Solution 2: `CAS` — Compare-And-Swap

`CAS` is a more powerful primitive:

```c
bool cas(long *p, long old, long new) {
    // Atomically:
    // if (*p == old) { *p = new; return true; }
    // else           { return false; }
    asm("lock cmpxchgq ...");  // x86: LOCK CMPXCHG
}
```

CAS is the Swiss Army knife of lock-free programming. It lets you **atomically update a value only if it hasn't changed** since you last read it.

**Usage pattern — "apply a function atomically":**

```c
void apply(long (*f)(long), long *p) {
    long old, new;
    do {
        old = *p;
        new = f(old);         // compute desired new value
    } while (!cas(p, old, new));  // retry if *p changed under us
}
```

This is the foundation of **lock-free** data structures: read the current state, compute the new state, atomically install it if nothing changed. If something changed (another thread intervened), retry.

**`xchg` vs `CAS`:**

| Property | `xchg` | `CAS` |
|----------|--------|-------|
| Operation | Always writes | Writes only if value matches |
| Return | Old value | Success/failure |
| Use case | Simple spinlocks | Lock-free algorithms, complex state transitions |
| Composability | Limited | High |

---

## Part 4: Lock Granularity

### Coarse-Grained vs. Fine-Grained Locking

| Approach | Description | Advantage | Disadvantage |
|----------|-------------|-----------|--------------|
| **Coarse-grained** | One lock for a large data structure or module | Simple to reason about; hard to get wrong | Bottleneck: all threads serialize even for unrelated operations |
| **Fine-grained** | One lock per small, independent piece of state | High parallelism; threads rarely block each other | Complex; more lock ordering decisions; deadlock risk |

**Example — Pipe with its own lock:**

```c
struct pipe {
    char buf[N];
    size_t r, w;
    lock_t l;         // lock embedded in the struct → fine-grained
};
```

Each pipe has its own lock. Two threads operating on different pipes never block each other. This is fine-grained locking — the lock controls access to exactly the data it's embedded in.

**Rule of thumb:** Start with one coarse lock, measure, and only add fine-grained locks where contention is proven to be a bottleneck.

---

### Read-Write Locks

**Motivation:** Many operations only *read* shared data. Multiple concurrent readers are safe — they don't modify anything. Only a writer needs exclusive access.

A **read-write lock** (`rwlock`) enforces:
- **Multiple concurrent readers** (shared read access)
- **Exclusive single writer** (no readers or other writers during write)

```c
typedef struct {
    int readers;    // count of active readers
    lock_t l;       // protects the 'readers' count itself
} rwlock_t;

void rlock(rwlock_t *l) {        // acquire read lock
    lock(&l->l);
    l->readers++;
    unlock(&l->l);
    // Note: simplified — real rwlocks also block if a writer is waiting
}

void runlock(rwlock_t *l) {      // release read lock
    lock(&l->l);
    l->readers--;
    unlock(&l->l);
}

void wlock(rwlock_t *l) {        // acquire write lock
    for (;;) {
        lock(&l->l);
        bool no_readers = (l->readers == 0);
        unlock(&l->l);
        if (no_readers)
            return;
        // spin while readers exist
    }
}

void wunlock(rwlock_t *l) {      // release write lock
    unlock(&l->l);
}
```

> **Exam trap — Writer Starvation:** If readers continuously arrive, a waiting writer may never get access (readers always > 0). This is a starvation problem symmetric to the long-job starvation in SJF scheduling. Production rwlock implementations use a "writer preference" or "fair queuing" policy to prevent this.

---

## Part 5: Spinning vs. Blocking

### The Spinlock Problem

All the lock implementations above are **spinlocks** — a thread that can't acquire the lock burns CPU cycles in a tight loop:

```c
void lock(mutex_t *m) {
    while (xchg(m, 1))
        continue;   // ← burning CPU doing nothing useful
}
```

**When is spinning appropriate?**
- The lock is expected to be held for a very short time (microseconds)
- The system has spare CPU capacity
- Context switching is expensive relative to wait time

**When is spinning a problem?**
- The lock holder might run for milliseconds → the spinner wastes a whole CPU
- On a uniprocessor, spinning is especially bad: the lock holder can't run to release the lock if the spinner is consuming the only CPU

---

### Blocking Mutex (`bmutex`)

A **blocking mutex** solves the spinning problem: if the lock isn't available, the thread **suspends itself** (removes itself from the run queue) rather than spinning. The CPU is freed for other work.

```c
typedef struct {
    lock_t l;                   // spinlock protecting this struct
    bool acquired;              // is the mutex held?
    thread_t *blocked_threads;  // linked list of waiting threads
} bmutex_t;

void acquire(bmutex_t *p) {
    for (;;) {
        lock(&p->l);
        if (!p->acquired)
            break;
        // Mutex is held — add ourselves to the wait list and sleep
        self->next = p->blocked_threads;
        p->blocked_threads = self;
        unlock(&p->l);
        schedule();    // yield the CPU; we won't run again until someone calls release()
    }
    p->acquired = true;
    unlock(&p->l);
}

void release(bmutex_t *p) {
    lock(&p->l);
    p->acquired = false;
    // Wake up a waiting thread (if any)
    if (p->blocked_threads) {
        thread_t *t = p->blocked_threads;
        p->blocked_threads = t->next;
        make_runnable(t);   // put it back on the run queue
    }
    unlock(&p->l);
}
```

**Key implementation detail:** `acquire` uses an inner **spinlock** (`p->l`) to protect the `bmutex_t` struct itself (the `acquired` flag and the wait list). The spinlock is held only briefly while checking/updating the struct — never across the `schedule()` call. This nests correctly.

**Why `schedule()` instead of spinning:**
- The thread is placed in the wait list, then yields the CPU
- The CPU runs other threads
- When `release()` is called, it picks a blocked thread and makes it runnable
- The woken thread re-enters `acquire()`, which will now find the mutex free

**Comparison:**

| Property | Spinlock | Blocking Mutex |
|----------|----------|----------------|
| Waiting behavior | Burns CPU in loop | Sleeps (not runnable) |
| Overhead when uncontended | Very low | Low (one atomic op) |
| Overhead when contended | Wastes CPU | Context switch overhead |
| Best for | Short critical sections, real-time | Long waits, general use |
| Kernel required? | No | Yes (need `schedule()`) |

---

### Semaphores

A **semaphore** is a generalization of a blocking mutex. Instead of a boolean `acquired`, it uses an **integer counter**.

- `sem_wait(s)`: if counter > 0, decrement and proceed. If counter == 0, block.
- `sem_post(s)`: increment counter; wake a blocked thread if any.

A **binary semaphore** (counter ∈ {0, 1}) behaves like a mutex.  
A **counting semaphore** (counter = N) allows up to N concurrent holders — useful for resource pools (e.g., "at most 5 connections at a time").

---

## Part 6: Condition Variables

### Motivation

The pipe example has a recurring pattern:

```c
// Writer: wait until there's space
while (p->w - p->r == N) {  // while full
    unlock(&p->l);
    lock(&p->l);             // spin-wait, checking repeatedly
}
```

This spin-wait is wasteful. The writer knows *exactly* what it's waiting for: "the pipe is not full." A **condition variable** provides a way to sleep until that condition becomes true.

### Condition Variable API

```c
void cond_wait(condvar_t *c, bmutex_t *b);
// PRE:  b is acquired (you must hold the lock before calling)
// Effect: atomically releases b AND blocks this thread
// On wake: reacquires b before returning

void cond_signal(condvar_t *c);
// Wakes up ONE thread waiting on c

void cond_broadcast(condvar_t *c);
// Wakes up ALL threads waiting on c
```

**The atomicity of `cond_wait` is critical:** releasing the lock and going to sleep must be atomic. Otherwise, between releasing the lock and going to sleep, another thread could call `cond_signal` — and the signal would be lost (sent to nobody), causing the waiter to sleep forever.

**Correct pipe using condition variables:**

```c
struct pipe {
    char buf[N];
    size_t r, w;
    bmutex_t m;
    condvar_t not_full;   // writers wait here
    condvar_t not_empty;  // readers wait here
};

void writec(struct pipe *p, char c) {
    acquire(&p->m);
    while (p->w - p->r == N)          // use while, not if (spurious wakeups)
        cond_wait(&p->not_full, &p->m);
    p->buf[p->w++ % N] = c;
    cond_signal(&p->not_empty);        // wake a blocked reader
    release(&p->m);
}

char readc(struct pipe *p) {
    acquire(&p->m);
    while (p->r == p->w)              // use while, not if
        cond_wait(&p->not_empty, &p->m);
    char ch = p->buf[p->r++ % N];
    cond_signal(&p->not_full);         // wake a blocked writer
    release(&p->m);
    return ch;
}
```

> **Exam trap — Always use `while`, never `if` with `cond_wait`:** When a thread is woken up, the condition might no longer be true (another thread may have consumed the resource first, or the wakeup could be spurious). Always re-check the condition in a loop.

---

## Part 7: Deadlock

### What Is Deadlock?

A **deadlock** occurs when a set of threads are each waiting for a resource held by another thread in the set — and none of them can ever make progress.

**Classic example:**

- Thread 1 holds Lock A, wants Lock B
- Thread 2 holds Lock B, wants Lock A
- Neither can proceed → **deadlock**

```
Thread 1:  lock(A); ... lock(B);   ← blocked waiting for B
Thread 2:  lock(B); ... lock(A);   ← blocked waiting for A
```

### The Four Conditions for Deadlock (Coffman Conditions)

Deadlock can only occur when **all four** of these hold simultaneously:

1. **Mutual Exclusion** — resources can't be shared (only one thread holds the lock at a time)
2. **Hold and Wait** — a thread holds one resource while waiting for another
3. **No Preemption** — locks can't be forcibly taken away (a thread must release them voluntarily)
4. **Circular Wait** — there's a cycle in the "waits-for" graph (T1 waits for T2, T2 waits for T1)

> **Exam trap:** Removing **any one** of the four Coffman conditions prevents deadlock. Real systems prevent deadlock by attacking #2 (acquire all locks at once) or #4 (enforce a global lock ordering).

### Deadlock in C: `pthread_mutex_t` Example

The following runnable C program reliably triggers a deadlock using POSIX mutexes:

```c
#include <stdio.h>
#include <pthread.h>

pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

/* Thread 1: acquires A, then tries to acquire B */
void *thread1(void *arg) {
    pthread_mutex_lock(&lock_a);
    printf("Thread 1: acquired A\n");

    /* Yield the CPU so Thread 2 can acquire B first,
       making the deadlock deterministic in this demo. */
    sched_yield();

    printf("Thread 1: waiting for B...\n");
    pthread_mutex_lock(&lock_b);   /* ← blocks forever if Thread 2 holds B */
    printf("Thread 1: acquired B\n");

    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

/* Thread 2: acquires B, then tries to acquire A */
void *thread2(void *arg) {
    pthread_mutex_lock(&lock_b);
    printf("Thread 2: acquired B\n");

    sched_yield();

    printf("Thread 2: waiting for A...\n");
    pthread_mutex_lock(&lock_a);   /* ← blocks forever if Thread 1 holds A */
    printf("Thread 2: acquired A\n");

    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread1, NULL);
    pthread_create(&t2, NULL, thread2, NULL);
    pthread_join(t1, NULL);   /* hangs — both threads blocked forever */
    pthread_join(t2, NULL);
    return 0;
}
```

**What happens at runtime:**

```
Thread 1: acquired A
Thread 2: acquired B
Thread 1: waiting for B...    ← blocked: Thread 2 holds B
Thread 2: waiting for A...    ← blocked: Thread 1 holds A
(program hangs forever)
```

**Waits-for graph:**

```
Thread 1 ──holds──▶ Lock A ◀──wants── Thread 2
Thread 1 ──wants──▶ Lock B ◀──holds── Thread 2
```

There is a cycle → all four Coffman conditions are satisfied → deadlock.

**Fix — enforce a global lock ordering (always acquire A before B):**

```c
/* Both threads now acquire in the same order: A → B */
void *thread1_fixed(void *arg) {
    pthread_mutex_lock(&lock_a);
    pthread_mutex_lock(&lock_b);
    /* critical section */
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

void *thread2_fixed(void *arg) {
    pthread_mutex_lock(&lock_a);   /* same order as thread1 */
    pthread_mutex_lock(&lock_b);
    /* critical section */
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}
```

Now the waits-for graph has no cycle — whichever thread wins `lock_a` first will also acquire `lock_b` and finish before the other thread can proceed. **Circular wait (#4) is eliminated.**

> **Compile & run:** `gcc -o deadlock deadlock.c -lpthread && ./deadlock`  
> The deadlocked version hangs indefinitely; kill it with `Ctrl-C`.

---

### Deadlock Prevention Strategies

| Strategy | How | Eliminates |
|----------|-----|-----------|
| **Lock ordering** | Always acquire multiple locks in the same global order | Circular wait (#4) |
| **Try-lock with backoff** | If you can't get all locks, release what you have and retry | Hold and wait (#2) |
| **Lock hierarchy** | Assign levels to locks; only acquire higher-level while holding lower-level | Circular wait (#4) |
| **Single global lock** | Only one lock in the system | Circular wait (#4), but kills parallelism |

**Lock ordering example:**

```c
// BAD — can deadlock:
void transfer(account *from, account *to) {
    lock(from);
    lock(to);     // Thread 1 does transfer(A→B); Thread 2 does transfer(B→A) → deadlock
    ...
}

// GOOD — always lock lower-address account first:
void transfer(account *from, account *to) {
    if (from < to) { lock(from); lock(to); }
    else           { lock(to);   lock(from); }
    ...
}
```

By always acquiring locks in the same order (by address, by ID, etc.), you guarantee there's no cycle in the waits-for graph.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| Synchronization goals | Consistency, parallelism, clarity — managing shared state correctly and efficiently |
| Sequence coordination | Enforcing ordering (`<`) across threads |
| Isolation / Atomicity | Operations appear indivisible; no partial states visible to others |
| Naive spinlock | Test-then-set is broken — two threads can both pass the test |
| Lock object size | Too big (multi-word, not atomic) or too small (single-bit, not atomic) both fail |
| `xchg` | Atomically swaps register with memory; implicitly locked; ~10× slower than mov |
| CAS | Compare-and-swap; atomically writes only if value matches; foundation of lock-free programming |
| Correct spinlock | `while (xchg(m, 1)) continue;` — returns old value; 0 means you won the lock |
| Coarse-grained lock | One lock for big structure; simple but creates bottleneck |
| Fine-grained lock | Lock per small unit of state; high parallelism; deadlock risk |
| Read-write lock | Multiple concurrent readers OR one exclusive writer; writer starvation risk |
| Spinlock vs. blocking mutex | Spinlock burns CPU; bmutex sleeps — use bmutex for long waits |
| Blocking mutex internals | Uses inner spinlock to protect wait list; calls `schedule()` to yield CPU |
| Semaphore | Generalization of mutex with an integer counter; counting semaphore for resource pools |
| Condition variable | Sleep until a specific condition is true; `cond_wait` atomically releases lock + blocks |
| `while` not `if` | Always re-check condition after `cond_wait` (spurious wakeups, lost races) |
| Deadlock | All four Coffman conditions must hold: mutual exclusion, hold-and-wait, no preemption, circular wait |
| Deadlock prevention | Break any one Coffman condition; most practical: global lock ordering (breaks circular wait) |
