# Lecture 8 — Scheduling, Critical Sections, and Synchronization

---

## Overview

Three connected topics:

1. **Scheduling** — how the OS decides which thread/process runs next (and for how long)
2. **Critical Sections** — how we identify and protect code that must not run concurrently
3. **Synchronization primitives** — the tools (`lock`, `unlock`, signals) used to enforce those protections

---

## Part 1: Scheduling

### What Is Scheduling?

When multiple threads or processes are runnable at the same time, the OS must decide who runs on which CPU core. Scheduling consists of two separable concerns:

- **Dispatch (mechanism):** *How* the OS actually switches from one thread to another — saving registers, switching the stack pointer, loading the new thread's state. This is a pure mechanism: it does exactly what it's told, no decisions.
- **Policy:** *Which* thread to run next, and for how long. This is where the interesting tradeoffs live.

> **Exam trap:** Dispatch and policy are deliberately separated. A good OS design lets you swap out the policy without changing the mechanism.

---

### Scheduling Metrics

Before you can judge whether a scheduling algorithm is good, you need to define what "good" means. The main metrics:

| Metric | Definition | Who cares |
|--------|-----------|-----------|
| **CPU Utilization** | Fraction of time the CPU is doing useful work (not idle) | OS / system admin |
| **Throughput** | Number of jobs completed per unit time | System admin |
| **Wait Time** | Time from job *arrival* to job *start* | User |
| **Turnaround Time** | Time from job *arrival* to job *completion* (= wait + runtime) | User |
| **Response Time** | Time from job arrival to *first response* (key for interactive tasks) | User |
| **Fairness** | Each job gets a "fair" share of CPU | User, auditors |

**Utilization and fairness are competing goals.** Maximum utilization might mean always running the job that uses CPU most efficiently — which could starve low-priority or long jobs. True fairness might reduce throughput. Every scheduler is a tradeoff.

**Variance matters too.** Low variance = predictability. A scheduler with average wait time 5ms but sometimes 0ms and sometimes 100ms may be worse in practice than one with a consistent 5ms, because unpredictability makes applications hard to design.

**Summary of what we care about:**
- Average case (overall performance)
- Worst case (guarantees, SLAs)
- Variance (predictability)
- **Fairness** (no job waits forever)

---

### Algorithm 1: FIFO / First Come First Served (FCFS)

**Rules:**
- Jobs run in arrival order
- **No preemption** — a job runs until it finishes
- **No yielding** — the CPU is not voluntarily given up

**Example:**

| Job | Arrival | Runtime | Wait Time | Turnaround |
|-----|---------|---------|-----------|------------|
| A   | 0       | 5       | 0         | 5          |
| B   | 1       | 2       | 4 + k     | 6 + k      |
| C   | 2       | 9       | 5 + 2k    | 14 + 2k    |
| D   | 3       | 4       | 13 + 3k   | 17 + 3k    |
| **Avg** | | 5 | 5.5 + 1.5k | 10.5 + 1.5k |

```
Timeline:  AAAAA BB CCCCCCCCC DDDD
              ^B arrives ^C arrives ^D arrives
```

> `k` is the **context switch overhead** — the brief dead time between stopping one job and starting the next.

**The Convoy Effect:** This is FCFS's key pathology. If a long job (like C with runtime 9) arrives first, all shorter jobs queue up behind it — exactly like getting stuck behind a semi-truck on a single-lane road. Short jobs that could complete quickly end up waiting a disproportionately long time.

**Verdict:** Simple, predictable, starvation-free (every job eventually runs). But terrible average wait time due to the convoy effect.

---

### Algorithm 2: Shortest Job First (SJF)

**Rules:**
- When the CPU becomes free, pick the **shortest** (lowest runtime) waiting job
- **No preemption** (non-preemptive SJF)

> **Fact correction:** The original notes say "favor long jobs" — that's backwards. SJF favors **short** jobs. The reason for mentioning trucks is the convoy effect in FIFO, which SJF is designed to fix.

**Example** (same jobs as FIFO):

When A finishes at time 5, all of B(2), C(9), D(4) have arrived. SJF picks the shortest: B(2), then D(4), then C(9).

| Job | Arrival | Runtime | Wait Time | Turnaround |
|-----|---------|---------|-----------|------------|
| A   | 0       | 5       | 0         | 5          |
| B   | 1       | 2       | 4 + k     | 6 + k      |
| D   | 3       | 4       | 4 + 2k    | 8 + 2k     |
| C   | 2       | 9       | 9 + 3k    | 18 + 3k    |
| **Avg** | | 5 | ~4.25 + 1.5k | ~9.25 + 1.5k |

SJF is **provably optimal** for minimizing average wait time (for a fixed set of non-preemptive jobs).

**Critical Weakness — Starvation:**

> If short jobs keep arriving continuously, a long job may **never** get to run. This is called **starvation** — the long job waits indefinitely. This is a critical fairness failure.

**Practical problem:** SJF requires knowing each job's runtime in advance. In practice, runtimes are unknown or unpredictable. Approximations (e.g., exponential averaging of past runtimes) are used.

---

### Algorithm 3: Round Robin (RR) — FIFO With Preemption

**Rules:**
- Jobs are queued in arrival order (like FIFO)
- Each job gets a fixed **time quantum** (time slice) on the CPU
- After the quantum expires, the running job is **preempted** and moved to the back of the queue
- The next job in line runs

**Example timeline (quantum = 1 time unit):**

```
A B A C D A B C D A C D A C D C C C C C
```

(A, B, C, D cycle through one unit each as they arrive, until each completes)

**Key insight:**
- **If you care about wait time → use Round Robin.** RR distributes CPU time evenly, so no job waits too long.
- **If you care about turnaround time → use something else.** RR slices each job into tiny pieces, so jobs take longer to finish than they would under SJF or even FIFO.

**Starvation in RR:** Round Robin avoids starvation **only if new arrivals go to the end of the queue.** If newly arriving jobs jump ahead in the queue, starvation can still occur.

**Quantum size tradeoff:**
- Too small → context switch overhead dominates (k becomes significant fraction of runtime)
- Too large → degenerates toward FCFS behavior
- In practice, Linux uses quanta of ~4ms–100ms depending on priority.

---

### Algorithm 4: Priority Scheduling

**Rules:**
- Each job has a **priority value**
- The highest-priority runnable job always runs next

**Linux niceness:**
- Priority numbers in Linux range from **-19 (highest priority)** to **+19 (lowest priority)**
- The term is "niceness" — a process with high niceness is "nice" to other processes (yields CPU)
- A process with low niceness (or negative niceness) is "mean" — it wants the CPU badly
- **Lower number = higher priority** (counterintuitive, but standard)

```bash
nice make foo          # run with niceness +10 (lower priority, "nicer")
nice -n -19 make foo   # run with niceness -19 (highest priority, requires root)
```

**Static vs. Dynamic Priorities:**
- **Static:** priority assigned at creation, never changes. Simple, but can starve low-priority jobs.
- **Dynamic:** priority changes over time. Common approach: **aging** — the longer a job waits, the higher its priority becomes. This prevents starvation.

  > Mnemonic: "Jobs get meaner as they get older." The more they've been waiting, the more aggressively they compete for CPU.

**The general framework:** Many algorithms are special cases of priority scheduling:
- FCFS: priority = arrival time (earlier arrival = higher priority)
- SJF: priority = runtime (shorter job = higher priority)
- Round Robin: equal priority for everyone + preemption

---

### Algorithm 5: Real-Time Schedulers

**Use case:** Systems where timing guarantees are *part of the correctness specification* — missing a deadline is a bug.

**Two types:**

**Hard Real-Time:**
- **Cannot miss any deadline.** Missing a deadline is as bad as a crash.
- Examples: anti-lock brakes, airplane control surfaces, nuclear reactor control systems
- If you cannot guarantee a deadline will be met, the system should refuse to schedule that task (or abort)
- **Implementation choices for predictability:**
  - **Disable caches** — caches make performance variable (sometimes fast, sometimes slow). Hard real-time systems always go to RAM directly so every access takes the same time.
  - **Use polling instead of interrupts** — interrupts introduce unpredictable latency. Instead, the CPU periodically checks ("polls") devices to see if they need service. Slower, but fully predictable.
- Predictability is more important than average performance. All you care about is **worst-case execution time (WCET)**.

**Soft Real-Time:**
- Deadlines should be met most of the time, but occasional misses are acceptable.
- Example: video streaming (one dropped frame is okay), audio playback
- Common algorithm: **Earliest Deadline First (EDF)** — always run the task whose deadline is soonest
  - If you can finish by the deadline, do it
  - If you cannot, drop the task

| Property | Hard Real-Time | Soft Real-Time |
|----------|---------------|----------------|
| Miss deadline? | Never (system failure) | Occasionally OK |
| Caches | Disabled | Enabled |
| I/O model | Polling | Interrupts |
| Example | Car brakes, ATC | Video playback |
| Algorithm | Rate Monotonic, EDF | EDF, weighted RR |

---

## Part 2: Critical Sections

### What Is a Critical Section?

A **critical section** is a block of code that:
1. Accesses **shared state** (memory, files, hardware registers)
2. Must not be executed by more than one thread **at the same time**

If two threads execute the same critical section concurrently, you get a **race condition** — the outcome depends on exact interleaving, which is non-deterministic and usually wrong.

### The Three Properties of Correct Critical Sections

Every correct critical section implementation must satisfy all three:

1. **Mutual Exclusion** — At most one thread can be inside the critical section at any moment. This is the core safety property.

2. **Progress** — If no thread is currently in the critical section, and some threads are waiting to enter, at least one of them must be allowed to proceed (the system doesn't deadlock outside the section).

3. **Bounded Waiting (No Starvation)** — Every thread that tries to enter the critical section must eventually succeed. No thread waits forever.

> **Exam trap:** All three properties are required. A solution with mutual exclusion but no bounded waiting has a starvation bug. A solution with bounded waiting but no mutual exclusion is just broken.

### Why Critical Sections Are Needed: The Two Sources of Danger

Even single-threaded code can have critical section issues. There are two sources of unexpected concurrency:

1. **Signals / Interrupts / Traps** — A signal handler can interrupt your thread at any instruction and execute code that accesses the same data. `pthread_sigmask(SIG_BLOCK, ...)` prevents this by blocking signals during the critical section.

2. **Multiple Threads** — Two threads running simultaneously on different CPU cores can interleave their instructions arbitrarily. Signal masking does nothing here — you need locks.

---

### Example 1: Bank Account (Single Lock)

```c
long long int balance;

bool deposit(long long int amt) {
    block();   // block signals (or acquire lock)
    bool ok = (0 <= amt && amt <= LLONG_MAX - balance);
    if (ok)
        balance += amt;
    unblock();  // unblock signals (or release lock)
    return ok;
}

bool withdraw(long long int amt) {
    block();
    bool ok = (balance >= amt && amt >= 0);
    if (ok)
        balance -= amt;
    unblock();
    return ok;
}
```

**Why the overflow check matters:** `balance += amt` can overflow if not checked. The condition `amt <= LLONG_MAX - balance` ensures the sum fits in a `long long`. This is a real correctness bug many students miss.

**Why the entire check-and-modify must be atomic:** The pattern `if (balance >= amt) balance -= amt` has a TOCTOU (Time Of Check To Time Of Use) bug: between checking `balance >= amt` and doing `balance -= amt`, another thread can withdraw money, making the balance go negative.

---

### Example 2: Circular Buffer / Pipe (More Complex)

```c
#define N 8192

struct pipe {
    char buf[N];
    size_t r;   // read index
    size_t w;   // write index
};
```

The pipe is full when `w - r == N`. The pipe is empty when `r == w`. Indices wrap with `% N`.

**Writer (correct version):**
```c
void writec(struct pipe *p, char c) {
    for (;;) {
        lock(&p->l);
        if (p->w - p->r != N)   // space available
            break;
        unlock(&p->l);           // release while waiting (don't hold lock while spinning!)
    }
    p->buf[p->w++ % N] = c;
    unlock(&p->l);
}
```

**Reader (correct version):**
```c
char readc(struct pipe *p) {
    for (;;) {
        lock(&p->l);
        if (p->w != p->r)        // data available
            break;
        unlock(&p->l);            // release while waiting
    }
    char ch = p->buf[p->r++ % N];
    unlock(&p->l);
    return ch;
}
```

**Critical insight:** The lock must be released while spinning (waiting for space/data), then re-acquired before breaking out of the loop. Holding the lock while spinning is a bug — the writer can never make progress because the reader can never run to advance `r`.

**Lock API contract:**

```c
void lock(lock_t *);
// PRE:  this thread does NOT already own the lock
// POST: this thread owns the lock; no other thread owns it

void unlock(lock_t *);
// PRE:  this thread owns the lock
// POST: this thread no longer owns the lock
```

Violating the preconditions (double-locking, unlocking without owning) is undefined behavior.

---

### The Goldilocks Principle of Critical Sections

Critical sections must be sized **just right**:

- **Too small** → races (you didn't protect everything that needed protecting)
- **Too large** → bottlenecks (threads spend all their time waiting to enter; no parallelism)

**Procedure for finding the right critical section:**

1. **Find shared state** — what data is accessed by multiple threads?
2. **Find writes to it** — reads of truly read-only data are safe without locks
3. **Find dependent reads** — reads that must be consistent with a prior read or write (TOCTOU patterns)

Protect the minimal set of operations that must be atomic together.

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| Dispatch vs. Policy | Dispatch = mechanism (how to switch); Policy = decision (who runs next) |
| Key scheduling metrics | Utilization, throughput, wait time, turnaround time, fairness, variance |
| FCFS | In-order, no preemption; convoy effect; no starvation |
| SJF | Shortest job first; optimal average wait time; starvation of long jobs |
| Round Robin | FIFO + preemption; minimizes wait time; hurts turnaround time |
| Priority scheduling | Lower number = higher Linux priority; aging prevents starvation |
| Realtime: hard | No missed deadlines; disable caches; use polling; predictability over performance |
| Realtime: soft | Occasional misses ok; use EDF; interrupts ok |
| Critical section 3 properties | Mutual exclusion, progress, bounded waiting (all required) |
| Two sources of races | Signals/interrupts AND multiple threads (need both defenses) |
| TOCTOU | Time Of Check To Time Of Use — check and use must be atomic together |
| Lock contract | `lock()`: PRE=don't own it, POST=own it; `unlock()`: PRE=own it, POST=released |
| Pipe pattern | Release lock while spinning; reacquire before break; never hold lock while waiting on a condition you can't satisfy |
| Goldilocks | Not too small (races), not too large (bottlenecks); protect exactly what needs protecting |
