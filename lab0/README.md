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
TODO: paste your test output here after running on the VM

Test output:
```shell

```


Report which kernel release version you tested your module on
(hint: use `uname`, check for options with `man uname`).
It should match release numbers as seen on https://www.kernel.org/.

```shell
uname -r -s -v
```
TODO: paste your kernel version here after running `uname -r -s -v` on the VM

Kernel version:
```shell

```