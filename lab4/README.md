# Hey! I'm Filing Here

In this lab, I implemented a program that creates a valid ext2 filesystem image (`cs111-base.img`). The filesystem contains a root directory, a `lost+found` directory, a regular file `hello-world` with the content `Hello world\n`, and a symbolic link `hello` pointing to `hello-world`.

The following were implemented:

- Superblock with correct field values (block/inode counts, bitmap sizes, magic number, etc.)
- Block group descriptor table pointing to the block bitmap, inode bitmap, and inode table
- Block bitmap marking all used blocks (0–23)
- Inode bitmap marking all used inodes (1–13)
- Inode table entries for the root directory, `lost+found`, `hello-world`, and `hello` symlink
- Root directory block with `.`, `..`, `lost+found`, `hello-world`, and `hello` entries
- `lost+found` directory block with `.` and `..` entries
- `hello-world` file data block containing `Hello world\n`

## Building

```bash
make
```

## Running

```bash
./ext2-create # run the executable to create cs111-base.img
mkdir mnt # create a directory to mount the file system to
sudo mount -o loop cs111-base.img mnt # mount filesystem to directory mnt
```

## Testing

### 1. Check filesystem integrity with fsck

After building and running, verify the image has no errors:

```bash
./ext2-create
fsck.ext2 -f -n cs111-base.img
```

Output (no errors):

```text
e2fsck 1.47.0 (5-Feb-2023)
Pass 1: Checking inodes, blocks, and sizes
Pass 2: Checking directory structure
Pass 3: Checking directory connectivity
Pass 4: Checking reference counts
Pass 5: Checking group summary information
cs111-base: 13/128 files (0.0% non-contiguous), 24/1024 blocks
```

### 2. Inspect filesystem metadata with dumpe2fs

```bash
dumpe2fs cs111-base.img
```

This dumps superblock fields (block count, inode count, free counts, UUID, etc.) and the block group descriptor. Useful for verifying individual field values.

### 3. Mount and inspect files manually

```bash
./ext2-create
mkdir mnt
sudo mount -o loop cs111-base.img mnt
ls -ain mnt/
```

`ls -ain` output (my machine):

```text
total 7
       2 drwxr-xr-x 3    0    0 1024 Jun  1 13:38 .
28180766 drwxrwxrwx 6 1000 1000 4096 Jun  1 13:38 ..
      13 lrw-r--r-- 1 1000 1000   11 Jun  1 13:38 hello -> hello-world
      12 -rw-r--r-- 1 1000 1000   12 Jun  1 13:38 hello-world
      11 drwxr-xr-x 2    0    0 1024 Jun  1 13:38 lost+found
```

Check the symlink target and file content:

```bash
readlink mnt/hello          # should print: hello-world
cat mnt/hello-world         # should print: Hello world
```

Then unmount:

```bash
sudo umount mnt
rmdir mnt
```

### 4. Run the provided unit tests

The unit tests automate the mount/inspect/umount cycle (requires sudo):

```bash
python3 -m unittest
```

Expected output:

```text
..
----------------------------------------------------------------------
Ran 2 tests in 0.109s

OK
```

### 5. Inspect the raw binary (optional)

```bash
hexdump -C cs111-base.img | less
```

Useful for verifying the exact byte layout of superblock fields, bitmaps, inodes, and directory entries.

## Cleaning up

```bash
sudo umount mnt # unmount the filesystem
rmdir mnt # delete the directory used for mounting
make clean
```
