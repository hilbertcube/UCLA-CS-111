# Round Robin Scheduling — Concept Notes

## What is Round Robin?

Round Robin (RR) is a preemptive CPU scheduling algorithm designed for time-sharing systems. Every process gets an equal, fixed slice of CPU time called a **quantum** (or time slice). Once a process uses up its quantum, it is preempted and sent to the back of the ready queue, giving the next process a turn.

This cycles through all runnable processes in order — hence the name "round robin."

---

## Key Concepts

### Burst Time

The total amount of CPU time a process needs to complete its work, assuming uninterrupted access. It is fixed at the start — set in the input file alongside the PID and arrival time. For example, `1, 0, 7` means PID 1 arrives at t=0 and needs 7 time units to finish.

In the code, `burst_time` stores the original total and `remaining_time` tracks how much is left. When `remaining_time` reaches 0, the process is done.

### Quantum Length
The quantum is how long a process runs before being preempted. A smaller quantum gives better responsiveness but causes more context switches. A larger quantum reduces overhead but makes the scheduler behave more like FCFS (First-Come, First-Served).

### Ready Queue
A FIFO queue holds all processes that have arrived and are waiting for CPU time. When a process is preempted or a new process arrives, it joins the tail of the queue. The scheduler always picks from the head.

### Preemption
Preemption means the OS **forcibly stops a process mid-execution** — even though it still has work left to do — and removes it from the CPU. The process didn't choose to stop; it was kicked out.

In round robin, this happens when a process uses up its full quantum. For example, if P1 has 7 units of burst time and the quantum is 3, after 3 units the scheduler preempts P1 and puts it at the back of the ready queue. P1 will get CPU time again on its next turn.

This is the key difference from **non-preemptive** schedulers like FCFS, where a process runs until it voluntarily finishes or blocks. Preemption is what makes round robin fair — no single process can hog the CPU.

### Completion
If a process finishes before its quantum expires, it leaves the system immediately. The CPU then picks the next process from the head of the ready queue.

---

## Metrics

### Waiting Time

The total time a process spends in the ready queue, not running.

```text
waiting_time = finish_time - arrival_time - burst_time
```

### Response Time

The time from a process's arrival until it first gets CPU time.

```text
response_time = first_run_time - arrival_time
```

---

## Step-by-Step Example

Using `processes.txt` with quantum = 3:

| PID | Arrival | Burst |
|-----|---------|-------|
| 1   | 0       | 7     |
| 2   | 2       | 4     |
| 3   | 4       | 1     |
| 4   | 5       | 4     |

**Simulation timeline:**

| Time | Running | Event |
|------|---------|-------|
| 0–2  | P1      | P1 starts; P2 arrives at t=2 |
| 3    | —       | P1's quantum expires; P2 and P3 not yet arrived fully, P1 re-queues |
| 3–5  | P2      | P2 gets CPU |
| 5–6  | P3      | P3 runs for 1 unit (its full burst), completes |
| 6–8  | P4      | P4 runs for its quantum |
| ...  | ...     | continues cycling until all complete |

**Result:** Average waiting time: 7.00, Average response time: 2.75

---

## How the Code Implements It (`rr.c`)

1. **Initialization** — Each process gets a `remaining_time` (copy of `burst_time`) and a `started` flag.
2. **Main loop** — Advances `current_time` one tick at a time until all processes complete.
3. **Arrivals** — At each tick, any process whose `arrival_time == current_time` is inserted at the tail of the TAILQ.
4. **Quantum expiry** — If the running process used its full quantum but isn't done, it is re-inserted at the tail *after* new arrivals for the same tick. This ensures newly arrived processes don't get unfairly skipped.
5. **Dispatch** — If the CPU is idle, the head of the queue is removed and scheduled. Its `response_time` contribution is recorded on first run.
6. **Tick** — `remaining_time` and `quantum_left` are decremented. If `remaining_time` hits 0, the process is done and its `waiting_time` contribution is recorded.

The ordering of steps 3 and 4 within the same tick is critical: arrivals are enqueued *before* a preempted process is re-queued, so a process that arrives exactly when a quantum expires gets in line ahead of the preempted one.

---

## Why Round Robin?

| Property | RR |
|---|---|
| Fairness | High — every process gets equal CPU slices |
| Starvation | None — every process eventually runs |
| Responsiveness | Good for interactive workloads |
| Overhead | Context-switch cost grows with smaller quantums |
| Optimality | Not optimal for turnaround time |

Round Robin is widely used in operating system process schedulers because it is simple, fair, and prevents any single process from starving others.
