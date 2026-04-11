# A Kernel Seedling

This is a Linux kernel module that creates a `/proc/count` file which reports the number of currently running processes (tasks) on the system.

## Building
```shell
make
```

## Running
```shell
sudo insmod proc_count.ko
cat /proc/count
```
The output is a single integer representing the number of running processes. For example, `150`.

## Cleaning Up
```shell
make clean
```

## Testing
```python
python -m unittest
```
Test output:
```shell
...
----------------------------------------------------------------------
Ran 3 tests in 2.293s

OK
```


Report which kernel release version you tested your module on
(hint: use `uname`, check for options with `man uname`).
It should match release numbers as seen on https://www.kernel.org/.

```shell
uname -r -s -v
```
Kernel version:
```shell
Linux 5.14.8-arch1-1 #1 SMP PREEMPT Sun, 26 Sep 2021 19:36:15 +0000
```