# You Spin Me Round Robin

A simple simulator of the round-robin (RR) CPU scheduling algorithm written in
C. Given a workload file describing a set of processes (PID, arrival time, and
burst time) and a quantum length, the program simulates RR scheduling and
prints the resulting average waiting time and average response time.

The program reads its workload from a plain-text file. The first integer in
the file is the number of processes, followed by one line per process with
three integers: `pid, arrival_time, burst_time`. Tokens may be separated by
any non-digit characters (commas, spaces, newlines).

## Building

Compile the `rr` executable with `make`:

```shell
make
```

## Running

Run the scheduler by passing the workload file path and the quantum length
(in time units) on the command line:

```shell
./rr processes.txt 3
```

With the provided `processes.txt` workload and a quantum of `3`, the output
is:

```shell
Average waiting time: 7.00
Average response time: 2.75
```

To run the included test suite:

```shell
python -m unittest
```

Output:

```shell
..
----------------------------------------------------------------------
Ran 2 tests in 0.074s

OK
```

## Cleaning up

Remove the compiled binary and object files:

```shell
make clean
```
