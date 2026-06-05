# Lecture 6 — File Descriptors, Pipes, Race Conditions, and Signals

---

## Overview

Four connected topics that all deal with things going wrong in a running system:

1. **File descriptor hazards** — what can go wrong when using open file handles
2. **Pipe failure modes** — what each error means and how to handle it correctly
3. **Race conditions in file creation** — the TOCTOU problem and how `O_EXCL` solves it
4. **Signals** — the OS mechanism for delivering asynchronous events to processes

---

## Part 1: File Descriptor Hazards

### How File Descriptors Work (Background)

A **file descriptor (fd)** is a small non-negative integer (0, 1, 2, 3, ...) that a process uses as a handle to refer to an open file, pipe, socket, or device. The kernel maintains a **file descriptor table** per process — each slot maps an fd number to a kernel `struct file` object.

```
Process fd table:
  fd 0 → struct file (stdin)
  fd 1 → struct file (stdout)
  fd 2 → struct file (stderr)
  fd 3 → struct file (open file "foo.txt")
  ...
```

The `struct file` contains: a reference count, a read/write offset, flags, and a pointer to the underlying file/inode.

> **The fd number itself is just a table index.** It has no meaning outside the process — fd 3 in process A and fd 3 in process B refer to completely different files.

---

### Hazard 1: Using a Closed File Descriptor

```c
int fd = open("foo.txt", O_RDONLY);
// ... some code ...
close(fd);             // fd is now invalid
// ... later ...
read(fd, buf, 100);    // BUG: fd is closed
// returns -1, errno = EBADF
```

**`EBADF`** — "bad file descriptor." The fd number you passed doesn't refer to an open file.

**The subtle danger — fd reuse:** After `close(fd)`, the kernel may reassign that same fd number to the next `open()` call. If another part of the code opens a file between the `close` and the stale `read`, the stale `read` silently operates on the *wrong file* without returning an error.

```c
close(fd);                     // fd = 3, now free
fd2 = open("secret.txt", ...); // fd2 = 3 (reused!)
read(fd, buf, 100);            // accidentally reads secret.txt
```

This is a real security vulnerability class. **Always set fds to -1 after closing them** to catch these bugs:

```c
close(fd);
fd = -1;   // subsequent use will give EBADF immediately
```

**`fd` leaks:** If you forget to `close()` an fd, the slot stays occupied. The kernel imposes a **per-process limit** on open file descriptors (default ~1024 on Linux, commonly reported as 1024 but technically 1024 is the soft limit). Leaking fds eventually causes `open()` to fail with `EMFILE` ("too many open files").

> **Exam note:** The exact Linux limit your professor cited is ~1024 (soft limit). Check `ulimit -n`. The system-wide hard limit is `/proc/sys/fs/file-max`.

---

### Hazard 2: Device Disappears

If a file descriptor is open to a hardware device (a USB drive, a serial port, a network interface) and that device is physically removed or disconnected:

- The next `read()` or `write()` will return -1 with an appropriate `errno` (often `EIO` — I/O error, or `ENXIO` — no such device)
- The specific `errno` depends on the device driver; read the man page for the syscall

**Correct pattern:** Always check return values from I/O syscalls. The `read()` man page documents all possible error conditions.

---

### Hazard 3: I/O Error

**`EIO`** — a physical I/O error (disk read failure, bad sector, hardware malfunction). Distinct from a logical error (wrong offset, wrong permissions). The data you tried to read may be permanently unrecoverable.

---

### Hazard 4: EOF (End of File)

`read()` returns **0** (not -1) when you reach the end of a file. This is a normal condition, not an error:

```c
ssize_t n = read(fd, buf, 100);
if (n < 0)  // error: check errno
if (n == 0) // EOF: no more data
if (n > 0)  // success: n bytes read into buf
```

> **Exam trap:** `read()` returning 0 means EOF — the file/pipe has no more data. It does **not** mean the read was unsuccessful.

---

### Hazard 5: No Data Yet (Blocking vs. Non-blocking)

When reading from a **network socket** or other slow/streaming source, data might not be available yet (the server is still computing):

- **Blocking mode (default):** `read()` blocks the calling thread until data arrives. The thread is suspended; other threads can run. When data arrives, the kernel wakes up the thread and `read()` returns.
- **Non-blocking mode (`O_NONBLOCK`):** `read()` returns immediately with -1 and `errno = EAGAIN` (or `EWOULDBLOCK`) if no data is available. The application must decide what to do (retry, do other work, etc.).

The choice between blocking and non-blocking I/O is a fundamental design decision in systems programming.

---

## Part 2: Pipe Failure Modes

A **pipe** has two ends: a write end and a read end. The behavior when one end is missing or not being used is carefully specified:

| Scenario | What happens |
|----------|-------------|
| **Read from pipe, no writer processes exist** | `read()` returns 0 (treated as EOF) |
| **Read from pipe, writer exists but isn't writing** | `read()` **blocks** — thread waits until data arrives |
| **Write to pipe, reader exists but isn't reading** | `write()` **blocks** — pipe buffer is full; thread waits for reader to consume |
| **Write to pipe, no reader processes exist** | `write()` returns -1 and `errno = EPIPE` OR the process receives **SIGPIPE** |

### The SIGPIPE / EPIPE Choice

When you write to a pipe with no readers, there are two outcomes:

1. **EPIPE (errno):** `write()` returns -1, sets `errno = EPIPE`. The application must check this and handle it. *Every process that writes to a pipe must check for this — when the pipe is gone, the writing is pointless.*
2. **SIGPIPE (default):** By default, the OS **kills the writing process** with `SIGPIPE`. This is the easier behavior for simple programs — they die automatically rather than looping pointlessly writing into nowhere.

> **Why SIGPIPE is the default:** In Unix pipelines like `cat bigfile | head -10`, `head` exits after reading 10 lines, closing its read end of the pipe. `cat` would otherwise keep running forever writing data nobody is reading. SIGPIPE kills `cat` automatically. This is the intended behavior.

To handle SIGPIPE gracefully, install a signal handler or use `signal(SIGPIPE, SIG_IGN)` to ignore it and instead handle the EPIPE errno.

---

## Part 3: Race Conditions in File Creation

### The Problem — Temporary Files

Consider a program (like `sort`) that needs to create temporary files. Naively:

```c
open("/tmp/sort_a", O_RDWR | O_CREAT, 0666);
open("/tmp/sort_b", O_RDWR | O_CREAT, 0666);
```

**Problem:** If two instances of `sort` run simultaneously, they both create `/tmp/sort_a` and clobber each other's data.

### Attempt 1 — Use PID in Filename

```c
pid_t p = getpid();
char buf[1000];
sprintf(buf, "/tmp/sort_%d", p);
open(buf, O_RDWR | O_CREAT, 0666);
```

**Why this works:** Every process has a unique PID. Two simultaneous `sort` processes get different PIDs, so they create different filenames. **This is the standard approach for temp files.**

### Attempt 2 — Random Name (Unnecessary Complexity)

```c
int i;
getrandom(&i, sizeof i);
sprintf(buf, "/tmp/sort_%d", i);
```

Using a random number instead of PID. Technically works but introduces a collision probability (however tiny) and adds complexity for no real benefit over using PID.

### The TOCTOU Race Condition (The Real Problem to Understand)

**TOCTOU = Time Of Check To Time Of Use.** This is a fundamental race condition pattern in file systems.

The broken code tries to:
1. **Check** if a filename is available (`access(buf, F_OK)`)
2. **Use** it (open with `O_CREAT`)

```c
// BROKEN — classic TOCTOU race:
while (access(buf, F_OK) == 0) {  // check: does it exist?
    generate_new_name(buf);
}
open(buf, O_RDWR | O_CREAT, 0666); // use: create it
// RACE WINDOW: another process can create buf between check and open!
```

Between the `access()` check and the `open()`, another process can create the same file. The check and the use are not atomic.

### The Correct Fix — `O_EXCL`

The `O_EXCL` flag makes creation **atomic**:

```c
int fd = open(buf, O_RDWR | O_CREAT | O_EXCL, 0666);
if (fd < 0 && errno == EEXIST) {
    // file already exists — try a different name
}
```

**`O_CREAT | O_EXCL`:** Create the file **only if it does not exist**. The check-and-create is a single atomic kernel operation — no race window. If the file already exists, `open()` fails with `errno = EEXIST`.

> **Exam trap:** `O_CREAT` alone does not fail if the file exists — it just opens the existing file. Only `O_CREAT | O_EXCL` together give you atomic exclusive creation.

**Standard library function:** `mkstemp(template)` uses this pattern internally — it generates a unique temp filename and opens it with `O_CREAT | O_EXCL` atomically. Prefer `mkstemp()` over rolling your own temp file logic.

---

## Part 4: File Locking with `fcntl`

### Why File Locks?

Sometimes multiple processes need to coordinate access to the same file (e.g., a shared log file, a database). File locks allow this coordination.

### `fcntl` Lock API

```c
#include <fcntl.h>

struct flock {
    short l_type;    // F_RDLCK, F_WRLCK, F_UNLCK
    short l_whence;  // SEEK_SET, SEEK_CUR, SEEK_END
    off_t l_start;   // offset from l_whence
    off_t l_len;     // length (0 = to end of file)
    pid_t l_pid;     // set by F_GETLK
};

int fcntl(int fd, int cmd, struct flock *lock);
// cmd:
//   F_SETLK  — set lock; fails immediately if lock can't be acquired
//   F_SETLKW — set lock; WAITS (blocks) until lock is available (W = wait)
//   F_GETLK  — query who holds the lock
```

**Lock types:**

| Type | Name | Behavior |
|------|------|---------|
| `F_RDLCK` | Read lock (shared) | Multiple readers allowed; blocks writers |
| `F_WRLCK` | Write lock (exclusive) | Only one writer; blocks all others |
| `F_UNLCK` | Unlock | Release the lock |

**Lock region:** You can lock a **byte range** within a file, not just the whole file. This enables fine-grained concurrency (e.g., two processes updating different records in the same file simultaneously).

### Advisory vs. Mandatory Locks

**`fcntl` locks are advisory** — the kernel does not enforce them. A process that doesn't use `fcntl` can happily read or write the file without checking whether it's locked.

> **Why not mandatory?** Mandatory locking creates too many complications. It can deadlock the kernel if a process opens a file for reading and the file is write-locked. It also breaks many Unix utilities that open files in unexpected ways (backup tools, log rotators, etc.). The Unix philosophy is "cooperating processes use the same locking protocol" — the kernel enforces the protocol only among participants.

### fcntl Lock Failure Modes

| Scenario | Behavior |
|----------|---------|
| Process `P` locks file, then **exits** | Lock is **automatically released** (associated with the process's fd table, which is freed on exit) |
| Process `P` locks file, then **forks** | Child has **no locks** — fork does not inherit locks |
| Two processes each wait for the other's lock | **DEADLOCK** — both block forever on `F_SETLKW` |
| Process `P` locks file, never unlocks | **Starvation** — all other waiters (`F_SETLKW`) block forever until P exits |

> **Exam trap:** Lock inheritance through `fork()` is a common trick question. Unlike file descriptors (which *are* inherited through fork), POSIX advisory locks are **not** inherited. The child starts with no locks, even if the parent held them.

---

## Part 5: Power Failure and the Signal System

### Power Failure Notification

When power fails:
1. The UPS or power supply detects the power loss
2. It signals the computer via the power bus ("I'm about to die")
3. The hardware generates an **interrupt** → kernel's power outage interrupt handler runs
4. The kernel sends **`SIGPWR`** to processes that have registered to receive it

**How does user-space know remaining power?** Two options:
- **`/dev/power`** (polling): Applications periodically read this file to check how many seconds remain. Simple but wastes CPU — you're constantly checking even when power is fine.
- **Signals (event-driven)**: `SIGPWR` is delivered when power fails. The application only runs code when it needs to. This is the preferred approach.

---

## Part 6: Signals

### What Is a Signal?

A **signal** is an asynchronous notification delivered to a process by the OS (or by another process). It's the kernel's mechanism for saying "something happened that you need to react to."

Key property: **a signal can be delivered between any two machine instructions.** The process is running normally, then suddenly the OS interrupts it and runs the signal handler, then resumes where it left off. The process has no say in *when* a signal arrives.

### Important Signals (Must Memorize)

| Signal | Number | Default action | Cause |
|--------|--------|---------------|-------|
| `SIGINT` | 2 | Terminate | User pressed Ctrl+C |
| `SIGQUIT` | 3 | Core dump | User pressed Ctrl+\ |
| `SIGILL` | 4 | Core dump | Illegal (invalid) machine instruction |
| `SIGFPE` | 8 | Core dump | Floating point exception (divide by zero) |
| `SIGKILL` | 9 | Terminate | Force kill — **cannot be caught or ignored** |
| `SIGSEGV` | 11 | Core dump | Segmentation violation (invalid memory access) |
| `SIGPIPE` | 13 | Terminate | Write to pipe with no readers |
| `SIGALRM` | 14 | Terminate | Timer expired (set by `alarm()`) |
| `SIGTERM` | 15 | Terminate | Polite kill request (can be caught) |
| `SIGCHLD` | 17 | Ignore | Child process stopped or terminated |
| `SIGCONT` | 18 | Continue | Resume a stopped process |
| `SIGSTOP` | 19 | Stop | Pause process — **cannot be caught or ignored** |
| `SIGTSTP` | 20 | Stop | User pressed Ctrl+Z (soft stop, can be caught) |
| `SIGHUP` | 1 | Terminate | Terminal closed / user logged out ("hangup") |
| `SIGBUS` | 7 | Core dump | Bus error (misaligned memory access) |
| `SIGPWR` | 30 | Terminate | Power failure notification |
| `SIGIO` | 29 | Terminate | Asynchronous I/O event |

> **Exam trap — SIGKILL vs SIGTERM:** `SIGTERM` (15) is a polite request — the process can catch it, clean up, and exit gracefully. `SIGKILL` (9) is unconditional — the kernel terminates the process with no opportunity to run any code. You cannot install a handler for SIGKILL or SIGSTOP.

> **Exam trap — SIGSTOP vs SIGTSTP:** Both pause a process. SIGTSTP (Ctrl+Z) can be caught (e.g., to save state before pausing). SIGSTOP cannot be caught — it always pauses the process immediately.

### Three Things a Signal Can Do (Upon Delivery)

When a signal is delivered to a process, the process does one of:

1. **Call a signal handler function** — run a user-defined function, then resume normal execution
2. **Exit immediately** — terminate the process (optionally with a core dump)
3. **Ignore it** — do nothing; the signal is discarded

The choice is controlled by the signal disposition, set with `signal()` or `sigaction()`.

### Signal Disposition API

```c
#include <signal.h>

typedef void (*sighandler_t)(int);

// Install a handler:
sighandler_t signal(int signum, sighandler_t handler);
// handler can be:
//   SIG_DFL  — restore default behavior
//   SIG_IGN  — ignore the signal
//   your_function  — call this function when signal arrives
```

**Example:**
```c
void handle_sigint(int sig) {
    printf("Caught SIGINT (Ctrl+C)\n");
    // clean up, then exit or re-raise
}

int main() {
    signal(SIGINT, handle_sigint);  // install handler at startup
    // ... rest of program ...
}
```

**`sigaction()` is the preferred interface** (more portable, safer):
```c
struct sigaction sa = {
    .sa_handler = handle_sigint,
    .sa_flags = SA_RESTART,  // restart interrupted syscalls
};
sigemptyset(&sa.sa_mask);
sigaction(SIGINT, &sa, NULL);
```

### Signals Change the Abstract Machine

This is a fundamental point: **signals introduce asynchronous concurrency even in single-threaded programs.**

A signal handler can run between *any two machine instructions*. If your main code and your signal handler both access the same global variable, you have a race condition — even with one thread.

```c
int count = 0;

void handler(int sig) {
    count++;  // increments count asynchronously
}

// Main code:
count++;  // RACE: handler might fire between loading and storing count
```

**Implication:** Signal handlers must only call **async-signal-safe functions** — functions that are reentrant and safe to call from a signal handler context. Many common functions (`malloc`, `printf`, `exit`) are NOT async-signal-safe. `write()` is safe; `printf()` is not (it uses an internal lock that may already be held by the interrupted code).

---

## Summary: Key Exam Points

| Concept | What to Know |
|---------|-------------|
| EBADF | Bad file descriptor — using a closed or invalid fd |
| fd reuse | After `close(fd)`, the number can be reassigned; stale use silently hits wrong file |
| fd limit | Per-process limit (~1024 soft); leak causes EMFILE |
| `read()` return values | >0 = bytes read; 0 = EOF; -1 = error (check errno) |
| SIGPIPE | Default: kills writer when pipe has no readers; or returns EPIPE if caught/ignored |
| Pipe read with no writers | `read()` returns 0 (EOF) |
| Pipe read with idle writer | `read()` blocks |
| TOCTOU | Check-then-act race: check if name available, then create — race window between |
| `O_CREAT \| O_EXCL` | Atomic exclusive creation; returns EEXIST if file exists — correct fix for TOCTOU |
| `fcntl` lock types | F_RDLCK (shared), F_WRLCK (exclusive), F_UNLCK; byte-range locking |
| F_SETLKW | Blocks (waits) until lock available; F_SETLK fails immediately |
| Advisory locks | Not enforced by kernel for non-participants; cooperative protocol only |
| Fork and locks | Child does NOT inherit parent's advisory locks |
| Exit and locks | Locks automatically released when process exits |
| SIGKILL / SIGSTOP | Cannot be caught or ignored — only two signals with this property |
| SIGTERM | Polite kill — can be caught and handled |
| Signal delivery timing | Can arrive between any two machine instructions — introduces async concurrency |
| Async-signal-safe | Only these functions are safe to call from signal handlers; `write()` yes, `printf()` no |
| `signal(sig, SIG_IGN)` | Ignore a signal; `SIG_DFL` restores default behavior |
