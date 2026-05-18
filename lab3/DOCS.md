# Hash Hash Hash

This lab makes a hash table implementation safe for concurrent use. The hash table uses separate chaining: each of 4096 buckets holds a singly-linked list of key-value pairs. Two thread-safe versions are implemented using `pthread_mutex_t` — one prioritizing correctness only, and one prioritizing both correctness and performance.

## Building
```shell
make
```

## Running
```shell
./hash-table-tester -t 8 -s 50000
```

```text
Generation: 123,779 usec
Hash table base: 504,348 usec
  - 0 missing
Hash table v1: 1,988,886 usec
  - 0 missing
Hash table v2: 77,219 usec
  - 0 missing
```

## First Implementation

In `hash_table_v1_add_entry`, I added a single global `pthread_mutex_t` to `struct hash_table_v1`. The mutex is locked at the start of `hash_table_v1_add_entry` and unlocked before every return path — both when updating an existing entry and when inserting a new one.

This is correct because the mutex enforces mutual exclusion over the entire `add_entry` operation. The critical section covers both the lookup (`get_list_entry`) and the insertion (`SLIST_INSERT_HEAD`), so no two threads can interleave their read-then-write sequences on any bucket. Only one thread can be inside `add_entry` at a time, eliminating the data race entirely.

### Performance
```shell
./hash-table-tester -t 8 -s 50000
```

```text
Hash table base: 504,348 usec
  - 0 missing
Hash table v1: 1,988,886 usec
  - 0 missing
```

Version 1 is approximately 3.9x **slower** than the base single-threaded implementation. This is expected for two reasons. First, thread creation itself carries overhead — spawning 8 threads and joining them adds latency that the serial base avoids entirely. Second, the single global mutex forces all threads to serialize: even threads operating on completely different buckets must wait for the lock, so no parallelism is gained. The result is effectively single-threaded execution plus the cost of mutex contention and thread management overhead.

## Second Implementation

In `hash_table_v2_add_entry`, I added one `pthread_mutex_t` per bucket by embedding a mutex in `struct hash_table_entry`. Each mutex is initialized in `hash_table_v2_create` and destroyed in `hash_table_v2_destroy`. In `add_entry`, only the mutex for the specific target bucket is locked, covering both the lookup and insertion, then unlocked before every return path.

This is correct because two threads hashing to the same bucket still serialize on that bucket's mutex — the read-then-write is still atomic per bucket. Two threads hashing to different buckets acquire different mutexes and never block each other, so they run truly in parallel. This is fine-grained locking: the critical section is as narrow as possible (one bucket), maximizing concurrency.

### Performance
```shell
./hash-table-tester -t 8 -s 50000
```

```text
Hash table base: 504,348 usec
  - 0 missing
Hash table v1: 1,988,886 usec
  - 0 missing
Hash table v2: 77,219 usec
  - 0 missing
```

Version 2 is approximately **6.5x faster** than the base and **25.8x faster** than version 1. With 8 threads and 4096 buckets, most threads hash to distinct buckets and proceed without contention, achieving near-linear speedup. The performance improvement over V1 comes entirely from lock granularity: instead of one global bottleneck, each bucket has its own independent lock, allowing up to 4096 threads to run `add_entry` in parallel.

## Cleaning up
```shell
make clean
```
