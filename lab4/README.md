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


## Cleaning up

```bash
sudo umount mnt # unmount the filesystem
rmdir mnt # delete the directory used for mounting
make clean
```
