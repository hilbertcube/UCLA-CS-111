# Lab 3 — Hash Hash Hash: Concept Notes

## What the Lab Is About

You have a hash table implemented in C. It works fine with one thread, but when multiple threads call `add_entry` at the same time, you get **data races** — concurrent writes corrupt the linked lists inside the table. The lab has you fix this in two ways, trading off correctness-only vs. correctness-with-performance.

---

## The Data Structure

The hash table uses **separate chaining** to handle collisions:

```
entries[0] -> [key:value] -> [key:value] -> NULL
entries[1] -> NULL
entries[2] -> [key:value] -> NULL
...
entries[N] -> [key:value] -> NULL
```

- The table is a fixed-size array of **buckets** (`hash_table_entry`).
- Each bucket holds a **singly-linked list** (`SLIST`) of key-value pairs (`list_entry`).
- A key is hashed to an index: `index = bernstein_hash(key) % HASH_TABLE_CAPACITY`.

`add_entry` does three things, all of which are unsafe across threads:
1. Look up which bucket the key belongs to.
2. Walk the bucket's list to check if the key already exists.
3. If not found, allocate a new `list_entry` and insert it at the head of the list.

Steps 2 and 3 together are a **read-then-write** sequence. If two threads both see the key is absent and both try to insert, you get a duplicate entry or a corrupted list pointer.

---

## The Race Condition

Two threads running `add_entry` concurrently on the **same bucket**:

```
Thread A: get_list_entry() → key not found
Thread B: get_list_entry() → key not found   (A hasn't inserted yet)
Thread A: SLIST_INSERT_HEAD(...)              (inserts new node)
Thread B: SLIST_INSERT_HEAD(...)              (also inserts → duplicate / lost pointer)
```

The result is either a missing entry, a duplicate entry, or a completely broken linked list (causing a crash or infinite loop on the next traversal).

---

## V1 — One Global Mutex (Correctness Only)

**Idea:** Put a single `pthread_mutex_t` on the entire hash table. Every call to `add_entry` locks it at the start and unlocks it at the end.

```
lock(global_mutex)
  → check if key exists
  → insert if not
unlock(global_mutex)
```

**Why it's correct:** Only one thread can be inside `add_entry` at any time, so the read-then-write is atomic with respect to all other threads. No two threads ever touch any bucket simultaneously.

**Why it's slow:** Even threads that hash to completely different buckets have to wait for each other. You have N threads but they all serialize through the one lock — effectively single-threaded. This is why V1 is expected to be *slower* than the base (single-threaded) version: you pay mutex overhead with no parallelism gain.

---

## V2 — Per-Bucket Mutexes (Correctness + Performance)

**Idea:** Give each bucket its own `pthread_mutex_t`. A thread only locks the mutex for the specific bucket it's about to touch.

```
bucket = hash(key) % CAPACITY
lock(bucket->mutex)
  → check if key exists in this bucket
  → insert if not
unlock(bucket->mutex)
```

**Why it's correct:** Two threads hashing to the *same* bucket still serialize — only one can hold that bucket's lock. The read-then-write within a single bucket is still atomic.

**Why it's fast:** Two threads hashing to *different* buckets don't block each other at all. With `HASH_TABLE_CAPACITY` buckets, up to that many threads can run `add_entry` truly in parallel. This is **fine-grained locking**, and it's the standard technique for scaling concurrent hash tables.

---

## Key Concepts

### Mutex (Mutual Exclusion Lock)
`pthread_mutex_lock` / `pthread_mutex_unlock` — only one thread holds the lock at a time. Any other thread that calls `lock` blocks until the holder calls `unlock`.

### Critical Section
The code between lock and unlock. Must be as short as possible — holding a lock longer than necessary kills parallelism.

### Data Race
When two threads access shared memory concurrently and at least one access is a write, with no synchronization. The result is undefined behavior.

### Granularity
- **Coarse-grained** (V1): one lock for everything — safe but slow.
- **Fine-grained** (V2): one lock per bucket — safe and fast because unrelated work proceeds in parallel.

### Deadlock
Not a concern here since each `add_entry` acquires exactly one lock and releases it before returning. Deadlock requires at least two locks acquired in inconsistent orders.

---

## Summary Comparison

| | Base | V1 | V2 |
|---|---|---|---|
| Thread safe | No | Yes | Yes |
| Locks | None | 1 global | 1 per bucket |
| Parallelism | N/A | None | High |
| Expected speed vs base | Fastest | Slower (lock overhead, no parallelism) | Faster (true parallelism) |

---

## What You Need to Implement

- **V1** ([hash-table-v1.c](hash-table-v1.c)): Add one `pthread_mutex_t` to `struct hash_table_v1`. Initialize it in `_create`, lock/unlock around the body of `hash_table_v1_add_entry`, and destroy it in `_destroy`.

- **V2** ([hash-table-v2.c](hash-table-v2.c)): Add one `pthread_mutex_t` to `struct hash_table_entry` (each bucket gets its own). Initialize each in `_create`, lock/unlock only the relevant bucket's mutex in `hash_table_v2_add_entry`, and destroy all of them in `_destroy`.

Both must call `pthread_mutex_destroy` for every mutex created (valgrind will catch leaks).
