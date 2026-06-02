#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t i16;
typedef int32_t i32;

#define BLOCK_SIZE 1024 // 1 KiB block size
#define BLOCK_OFFSET(i) (i * BLOCK_SIZE)
#define NUM_BLOCKS 1024
#define NUM_INODES 128

#define LOST_AND_FOUND_INO 11 // inode 11
#define HELLO_WORLD_INO 12    // inode 12
#define HELLO_INO 13          // inode 13
#define LAST_INO HELLO_INO

#define SUPERBLOCK_BLOCKNO 1
#define BLOCK_GROUP_DESCRIPTOR_BLOCKNO 2
#define BLOCK_BITMAP_BLOCKNO 3
#define INODE_BITMAP_BLOCKNO 4
#define INODE_TABLE_BLOCKNO 5
#define ROOT_DIR_BLOCKNO 21
#define LOST_AND_FOUND_DIR_BLOCKNO 22
#define HELLO_WORLD_FILE_BLOCKNO 23
#define LAST_BLOCK HELLO_WORLD_FILE_BLOCKNO

#define NUM_FREE_BLOCKS (NUM_BLOCKS - LAST_BLOCK - 1)
#define NUM_FREE_INODES (NUM_INODES - LAST_INO)

/// more macros
#define BLOCKS_PER_GROUP 8192 // 1024 * 8 bits
#define FRAGS_PER_GROUP 8192
/// end

#define EXT2_SUPER_MAGIC 0xEF53

/* http://www.nongnu.org/ext2-doc/ext2.html */
/* http://www.science.smith.edu/~nhowe/262/oldlabs/ext2.html */

#define EXT2_BAD_INO 1  // inode 1
#define EXT2_ROOT_INO 2 // inode 2
#define EXT2_GOOD_OLD_FIRST_INO 11

#define EXT2_GOOD_OLD_REV 0

/// File types (bits 15-12)
#define EXT2_S_IFSOCK 0xC000 // socket
#define EXT2_S_IFLNK 0xA000  // symlink
#define EXT2_S_IFREG 0x8000  // regular file
#define EXT2_S_IFBLK 0x6000  // block device (e.g /dev/sda)
#define EXT2_S_IFDIR 0x4000  // directory
#define EXT2_S_IFCHR 0x2000  // character device (e.g. /dev/tty)
#define EXT2_S_IFIFO 0x1000  // FIF0 / named pipe

/// Special permission bits (bits 11-9)
#define EXT2_S_ISUID 0x0800 // set uid: run as file owner's UID
#define EXT2_S_ISGID 0x0400 // set gid: run as file owner's GID
#define EXT2_S_ISVTX 0x0200 // sticky bit (on directories), only owner can delete their own files

/// Permission bits (bits 8-0)
#define EXT2_S_IRUSR 0x0100 // owner read
#define EXT2_S_IWUSR 0x0080 // owner write
#define EXT2_S_IXUSR 0x0040 // owner execute
#define EXT2_S_IRGRP 0x0020 // group read
#define EXT2_S_IWGRP 0x0010 // group write
#define EXT2_S_IXGRP 0x0008 // group execute
#define EXT2_S_IROTH 0x0004 // others read
#define EXT2_S_IWOTH 0x0002 // others write
#define EXT2_S_IXOTH 0x0001 // others execute

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS (EXT2_TIND_BLOCK + 1)

#define EXT2_NAME_LEN 255

/**
 * superblock is a data structure that describes the overall state of the file system,
 * including information about the total number of inodes and blocks, free blocks and
 * inodes, block size, and other metadata. It is typically stored at a fixed location
 * on the disk and is essential for the file system to function correctly.
 *
 */
struct ext2_superblock {
  u32 s_inodes_count;      // Total # of inodes in the file system, each file corresponds to an inode
  u32 s_blocks_count;      // Total # of blocks in the file system, 1 file can span multiple blocks
  u32 s_r_blocks_count;    // Number of blocks reserved for the superuser (root)
  u32 s_free_blocks_count; // Number of free blocks available in the file system
  u32 s_free_inodes_count; // Number of free inodes available in the file system
  u32 s_first_data_block;  // Block number of the first data block (usually 1 for 1024-byte block size)
  u32 s_log_block_size;    // Block size is 1024 << s_log_block_size
  i32 s_log_frag_size;     // Fragment size is 1024 << s_log_frag_size
  u32 s_blocks_per_group;  // Number of blocks per group
  u32 s_frags_per_group;   // Number of fragments per group
  u32 s_inodes_per_group;  // Number of inodes per group
  u32 s_mtime;             // Last mount time (in seconds since the Unix epoch)
  u32 s_wtime;             // Last write time (in seconds since the Unix epoch)
  u16 s_mnt_count;         // Number of times the file system has been mounted
  i16 s_max_mnt_count;     // Maximum number of mounts before a check is required
  u16 s_magic;             // Magic signature to identify the file system
  u16 s_state;             // File system state
  u16 s_errors;            // Behavior when detecting errors
  u16 s_minor_rev_level;   // Minor revision level
  u32 s_lastcheck;         // Time of last check
  u32 s_checkinterval;     // Maximum time between checks
  u32 s_creator_os;        // OS that created the file system
  u32 s_rev_level;         // Revision level
  u16 s_def_resuid;        // Default UID for reserved blocks
  u16 s_def_resgid;        // Default GID for reserved blocks
  u32 s_pad[5];            // Padding
  u8 s_uuid[16];           // Unique identifier for the file system
  u8 s_volume_name[16];    // Volume name
  u32 s_reserved[229];     // Padding to make the superblock 1024 bytes
}; // total size: 1024 bytes

struct ext2_block_group_descriptor {
  u32 bg_block_bitmap;      // Block number of the block bitmap for this group
  u32 bg_inode_bitmap;      // Block number of the inode bitmap for this group
  u32 bg_inode_table;       // Block number of the inode table for this group
  u16 bg_free_blocks_count; // Number of free blocks in this group
  u16 bg_free_inodes_count; // Number of free inodes in this group
  u16 bg_used_dirs_count;   // Number of directories in this group
  u16 bg_pad;
  u32 bg_reserved[3];
}; // total size: 32 bytes

struct ext2_inode {
  u16 i_mode;                 // File mode (directory, regular file, etc.) and permissions
  u16 i_uid;                  // User ID of owner
  u32 i_size;                 // Size in bytes
  u32 i_atime;                // Access time
  u32 i_ctime;                // Creation time
  u32 i_mtime;                // Modification time
  u32 i_dtime;                // Deletion time
  u16 i_gid;                  // Group ID of owner
  u16 i_links_count;          // Number of links (hard links) to this inode
  u32 i_blocks;               // Number of 512-byte blocks allocated to the file
  u32 i_flags;                // File flags (e.g., secure deletion, undelete, etc.)
  u32 i_reserved1;            // Reserved for future use
  u32 i_block[EXT2_N_BLOCKS]; // Pointers to blocks (direct, indirect, double indirect, triple indirect)
  u32 i_version;              // File version (used by NFS)
  u32 i_file_acl;             // Access control list (ACL) for the file
  u32 i_dir_acl;              // Directory ACL
  u32 i_faddr;                // Fragment address
  u8 i_frag;                  // Fragment number
  u8 i_fsize;                 // Fragment size
  u16 i_pad1;                 // Padding
  u32 i_reserved2[2];
}; // total size: 128 bytes

struct ext2_dir_entry {
  u32 inode;              // Inode number of the file/directory this entry refers to
  u16 rec_len;            // Length of this record (including the name)
  u16 name_len;           // Length of the name
  u8 name[EXT2_NAME_LEN]; // Name of the file/directory
}; // total size: 8 + name_len (rounded up to a multiple of 4 bytes)

#define errno_exit(str) \
  do {                  \
    int err = errno;    \
    perror(str);        \
    exit(err);          \
  } while (0)

#define dir_entry_set(entry, inode_num, str) \
  do {                                       \
    char *s = str;                           \
    size_t len = strlen(s);                  \
    entry.inode = inode_num;                 \
    entry.name_len = len;                    \
    memcpy(&entry.name, s, len);             \
    if ((len % 4) != 0) {                    \
      entry.rec_len = 12 + len / 4 * 4;      \
    } else {                                 \
      entry.rec_len = 8 + len;               \
    }                                        \
  } while (0)

#define dir_entry_write(entry, fd)         \
  do {                                     \
    size_t size = entry.rec_len;           \
    if (write(fd, &entry, size) != size) { \
      errno_exit("write");                 \
    }                                      \
  } while (0)

u32 get_current_time() {
  time_t t = time(NULL);
  if (t == ((time_t)-1)) {
    errno_exit("time");
  }
  return t;
}

void write_superblock(int fd) {
  off_t off = lseek(fd, BLOCK_OFFSET(1), SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  u32 current_time = get_current_time();

  struct ext2_superblock superblock = {0};

  // TODO It's all yours
  // TODO finish the superblock number setting
  superblock.s_inodes_count = NUM_INODES;
  superblock.s_blocks_count = NUM_BLOCKS;
  superblock.s_r_blocks_count = 0;
  superblock.s_free_blocks_count = NUM_FREE_BLOCKS;
  superblock.s_free_inodes_count = NUM_FREE_INODES;

  // should be 1 since the block
  superblock.s_first_data_block = SUPERBLOCK_BLOCKNO; /* First Data Block */
  superblock.s_log_block_size = 0;                    /* 1024 */
  superblock.s_log_frag_size = 0;                     /* 1024 */
  superblock.s_blocks_per_group = BLOCKS_PER_GROUP;
  superblock.s_frags_per_group = FRAGS_PER_GROUP;
  superblock.s_inodes_per_group = NUM_INODES;
  superblock.s_mtime = 0;                /* Mount time */
  superblock.s_wtime = current_time;     /* Write time */
  superblock.s_mnt_count = 0;            /* Number of times mounted so far */
  superblock.s_max_mnt_count = -1;       /* Make this unlimited */
  superblock.s_magic = EXT2_SUPER_MAGIC; /* ext2 Signature magic number */
  superblock.s_state = 1;                /* File system is clean (EXT2_VALID_FS) */
  superblock.s_errors = 1;               /* Continue on error (EXT2_ERRORS_CONTINUE) */
  superblock.s_minor_rev_level = 0;      /* Leave this as 0 */
  superblock.s_lastcheck = current_time; /* Last check time */
  superblock.s_checkinterval = 0;        /* Force checks by making them every 1 second */
  superblock.s_creator_os = 0;           /* Linux */
  superblock.s_rev_level = 0;            /* Leave this as 0 */
  superblock.s_def_resuid = 0;           /* root */
  superblock.s_def_resgid = 0;           /* root */

  /* You can leave everything below this line the same, delete this
     comment when you're done the lab */
  superblock.s_uuid[0] = 0x5A;
  superblock.s_uuid[1] = 0x1E;
  superblock.s_uuid[2] = 0xAB;
  superblock.s_uuid[3] = 0x1E;
  superblock.s_uuid[4] = 0x13;
  superblock.s_uuid[5] = 0x37;
  superblock.s_uuid[6] = 0x13;
  superblock.s_uuid[7] = 0x37;
  superblock.s_uuid[8] = 0x13;
  superblock.s_uuid[9] = 0x37;
  superblock.s_uuid[10] = 0xC0;
  superblock.s_uuid[11] = 0xFF;
  superblock.s_uuid[12] = 0xEE;
  superblock.s_uuid[13] = 0xC0;
  superblock.s_uuid[14] = 0xFF;
  superblock.s_uuid[15] = 0xEE;

  memcpy(&superblock.s_volume_name, "cs111-base", 10);

  ssize_t size = sizeof(superblock);
  if (write(fd, &superblock, size) != size) {
    errno_exit("write");
  }
}

void write_block_group_descriptor_table(int fd) {
  off_t off = lseek(fd, BLOCK_OFFSET(BLOCK_GROUP_DESCRIPTOR_BLOCKNO), SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  struct ext2_block_group_descriptor block_group_descriptor = {0};

  // TODO It's all yours
  // TODO finish the block group descriptor number setting
  block_group_descriptor.bg_block_bitmap = BLOCK_BITMAP_BLOCKNO;
  block_group_descriptor.bg_inode_bitmap = INODE_BITMAP_BLOCKNO;
  block_group_descriptor.bg_inode_table = INODE_TABLE_BLOCKNO;
  block_group_descriptor.bg_free_blocks_count = NUM_FREE_BLOCKS;
  block_group_descriptor.bg_free_inodes_count = NUM_FREE_INODES;
  block_group_descriptor.bg_used_dirs_count = 2; // there's two dirs: / and lost+found

  ssize_t size = sizeof(block_group_descriptor);
  if (write(fd, &block_group_descriptor, size) != size) {
    errno_exit("write");
  }
}

void write_block_bitmap(int fd) {
  off_t off = lseek(fd, BLOCK_OFFSET(BLOCK_BITMAP_BLOCKNO), SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  // TODO It's all yours
  u8 map_value[BLOCK_SIZE];

  // set n (BLOCK_SIZE) bytes of map_value to 0
  memset(map_value, 0, BLOCK_SIZE);

  // The group covers blocks 1-1023; bit N in the bitmap = block (N+1).
  // Blocks 1-23 are used (bits 0-22), block 24+ are free.
  map_value[0] = 0xFF; // blocks 1-8 
  map_value[1] = 0xFF; // blocks 9-16

  // blocks 17-23 used (bits 16-22), block 24 free (bit 23=0)
  map_value[2] = 0x7F; // 0b01111111

  // Bit 1023 (byte 127 bit 7) = block 1024, which doesn't exist -> padding = 1
  map_value[127] = 0x80;
  // Bytes 128-1023: blocks 1025+ don't exist -> padding = 0xFF
  memset(map_value + 128, 0xFF, BLOCK_SIZE - 128);

  if (write(fd, map_value, BLOCK_SIZE) != BLOCK_SIZE) {
    errno_exit("write");
  }
}

void write_inode_bitmap(int fd) {
  off_t off = lseek(fd, BLOCK_OFFSET(INODE_BITMAP_BLOCKNO), SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  // TODO It's all yours
  u8 map_value[BLOCK_SIZE];

  // set n (BLOCK_SIZE) bytes of map_value to 0
  memset(map_value, 0, BLOCK_SIZE);

  map_value[0] = 0xFF; // inodes 1-8
  map_value[1] = 0x1F; // inodes 9-13

  // Inodes 129+ don't exist; bytes 16-1023 are padding -> 0xFF
  memset(map_value + 16, 0xFF, BLOCK_SIZE - 16);

  if (write(fd, map_value, BLOCK_SIZE) != BLOCK_SIZE) {
    errno_exit("write");
  }
}

void write_inode(int fd, u32 index, struct ext2_inode *inode) {
  off_t off = BLOCK_OFFSET(INODE_TABLE_BLOCKNO) + (index - 1) * sizeof(struct ext2_inode);
  off = lseek(fd, off, SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  ssize_t size = sizeof(struct ext2_inode);
  if (write(fd, inode, size) != size) {
    errno_exit("write");
  }
}

// lost+found directory
void write_lost_and_found_inode(int fd, u32 current_time) {
  struct ext2_inode lost_and_found_inode = {0};
  lost_and_found_inode.i_mode = EXT2_S_IFDIR                                 // it's a directory
                                | EXT2_S_IRUSR | EXT2_S_IWUSR | EXT2_S_IXUSR // rwx for owner
                                | EXT2_S_IRGRP | EXT2_S_IXGRP                // r-x for group
                                | EXT2_S_IROTH | EXT2_S_IXOTH;               // r-x for others (755)
  lost_and_found_inode.i_uid = 0;
  lost_and_found_inode.i_size = BLOCK_SIZE;
  lost_and_found_inode.i_atime = current_time;
  lost_and_found_inode.i_ctime = current_time;
  lost_and_found_inode.i_mtime = current_time;
  lost_and_found_inode.i_dtime = 0;
  lost_and_found_inode.i_gid = 0;
  lost_and_found_inode.i_links_count = 2;
  lost_and_found_inode.i_blocks = 2; /* These are oddly 512 blocks */
  lost_and_found_inode.i_block[0] = LOST_AND_FOUND_DIR_BLOCKNO;
  write_inode(fd, LOST_AND_FOUND_INO, &lost_and_found_inode);
}

// root directory
void write_root_inode(int fd, u32 current_time) {
  struct ext2_inode root_inode = {0};
  root_inode.i_mode = EXT2_S_IFDIR                                 // it's a directory
                      | EXT2_S_IRUSR | EXT2_S_IWUSR | EXT2_S_IXUSR // rwx for owner
                      | EXT2_S_IRGRP | EXT2_S_IXGRP                // r-x for group
                      | EXT2_S_IROTH | EXT2_S_IXOTH;               // r-x for others (755)
  root_inode.i_uid = 0;
  root_inode.i_size = BLOCK_SIZE; // one full 1024-byte block of dir entries
  root_inode.i_atime = current_time;
  root_inode.i_ctime = current_time;
  root_inode.i_mtime = current_time;
  root_inode.i_dtime = 0;
  root_inode.i_gid = 0;
  // link count for a directory = 2 base (. and ..) + one per subdirectory
  // root's '.' points to itself, root's '..' also points to itself,
  // and lost+found's '..' points back to root -> total 3
  root_inode.i_links_count = 3;
  root_inode.i_blocks = 2;                  // one 1024-byte block = 2 × 512-byte sectors
  root_inode.i_block[0] = ROOT_DIR_BLOCKNO; // block 21 holds the root dir entries
  write_inode(fd, EXT2_ROOT_INO, &root_inode);
}

// regular file
void write_hello_world_inode(int fd, u32 current_time) {
  struct ext2_inode hello_world_inode = {0};
  hello_world_inode.i_mode = EXT2_S_IFREG                  // it's a regular file
                             | EXT2_S_IRUSR | EXT2_S_IWUSR // rw- for owner
                             | EXT2_S_IRGRP                // r-- for group
                             | EXT2_S_IROTH;               // r-- for others (644)
  hello_world_inode.i_uid = 1000; // specified in the lab requirements
  hello_world_inode.i_size = 12; // strlen("Hello world\n") == 12
  hello_world_inode.i_atime = current_time;
  hello_world_inode.i_ctime = current_time;
  hello_world_inode.i_mtime = current_time;
  hello_world_inode.i_dtime = 0;
  hello_world_inode.i_gid = 1000;
  hello_world_inode.i_links_count = 1;                     // one directory entry points to this inode
  hello_world_inode.i_blocks = 2;                          // one 1024-byte block = 2 × 512-byte sectors
  hello_world_inode.i_block[0] = HELLO_WORLD_FILE_BLOCKNO; // block 23 holds the content
  write_inode(fd, HELLO_WORLD_INO, &hello_world_inode);
}

// hello symbolic link
void write_hello_inode(int fd, u32 current_time) {
  struct ext2_inode hello_inode = {0};
  hello_inode.i_mode = EXT2_S_IFLNK              // it's a symbolic link
                       | EXT2_S_IRUSR | EXT2_S_IWUSR // rw- for owner
                       | EXT2_S_IRGRP               // r-- for group
                       | EXT2_S_IROTH;              // r-- for others (lrw-r--r--)
  hello_inode.i_uid = 1000; // specified in lab requirements
  hello_inode.i_size = 11; // strlen("hello-world") == 11
  hello_inode.i_atime = current_time;
  hello_inode.i_ctime = current_time;
  hello_inode.i_mtime = current_time;
  hello_inode.i_dtime = 0;
  hello_inode.i_gid = 1000;
  hello_inode.i_links_count = 1;
  // Fast symlink: target string fits in 60 bytes, so it's stored directly
  // in the i_block array itself — no data block is allocated.
  // i_blocks stays 0 because no disk block is used.
  hello_inode.i_blocks = 0;
  memcpy(hello_inode.i_block, "hello-world", 11);
  write_inode(fd, HELLO_INO, &hello_inode);
}

void write_inode_table(int fd) {
  u32 current_time = get_current_time();
  write_lost_and_found_inode(fd, current_time); // Inode 11: lost+found directory

  // TODO It's all yours
  // TODO finish the inode entries for the other files

  write_root_inode(fd, current_time);        // Inode 2: root directory '/'
  write_hello_world_inode(fd, current_time); // Inode 12: hello-world regular file
  write_hello_inode(fd, current_time);       // Inode 13: hello symlink --> "hello-world"
}

void write_root_dir_block(int fd) {
  // TODO It's all yours
  off_t off = BLOCK_OFFSET(ROOT_DIR_BLOCKNO);
  off = lseek(fd, off, SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  ssize_t bytes_remaining = BLOCK_SIZE;

  struct ext2_dir_entry current_entry = {0};
  dir_entry_set(current_entry, EXT2_ROOT_INO, ".");
  dir_entry_write(current_entry, fd);

  bytes_remaining -= current_entry.rec_len;

  struct ext2_dir_entry parent_entry = {0};
  dir_entry_set(parent_entry, EXT2_ROOT_INO, "..");
  dir_entry_write(parent_entry, fd);

  bytes_remaining -= parent_entry.rec_len;

  struct ext2_dir_entry lost_and_found_entry = {0};
  dir_entry_set(lost_and_found_entry, LOST_AND_FOUND_INO, "lost+found");
  dir_entry_write(lost_and_found_entry, fd);

  bytes_remaining -= lost_and_found_entry.rec_len;

  struct ext2_dir_entry hello_world_entry = {0};
  dir_entry_set(hello_world_entry, HELLO_WORLD_INO, "hello-world");
  dir_entry_write(hello_world_entry, fd);

  bytes_remaining -= hello_world_entry.rec_len;

  struct ext2_dir_entry hello_symlink_entry = {0};
  dir_entry_set(hello_symlink_entry, HELLO_INO, "hello");
  dir_entry_write(hello_symlink_entry, fd);

  bytes_remaining -= hello_symlink_entry.rec_len;

  struct ext2_dir_entry fill_entry = {0};
  fill_entry.rec_len = bytes_remaining;
  dir_entry_write(fill_entry, fd);
}

void write_lost_and_found_dir_block(int fd) {
  off_t off = BLOCK_OFFSET(LOST_AND_FOUND_DIR_BLOCKNO);
  off = lseek(fd, off, SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }

  ssize_t bytes_remaining = BLOCK_SIZE;

  // write the . entry
  struct ext2_dir_entry current_entry = {0};
  dir_entry_set(current_entry, LOST_AND_FOUND_INO, ".");
  dir_entry_write(current_entry, fd);

  bytes_remaining -= current_entry.rec_len;

  // write the .. entry
  struct ext2_dir_entry parent_entry = {0};
  dir_entry_set(parent_entry, EXT2_ROOT_INO, "..");
  dir_entry_write(parent_entry, fd);

  bytes_remaining -= parent_entry.rec_len;

  // write the .. entry, pointing to the root inode (inode 2)
  // since lost+found lives under /
  struct ext2_dir_entry fill_entry = {0};
  fill_entry.rec_len = bytes_remaining;
  dir_entry_write(fill_entry, fd);
}

// regular file's data block
void write_hello_world_file_block(int fd) {
  // TODO It's all yours
  off_t off = BLOCK_OFFSET(HELLO_WORLD_FILE_BLOCKNO);
  off = lseek(fd, off, SEEK_SET);
  if (off == -1) {
    errno_exit("lseek");
  }
  if (write(fd, "Hello world\n", 12) != 12) {
    errno_exit("write");
  }

  u8 padding[BLOCK_SIZE - 12] = {0};
  if (write(fd, padding, sizeof(padding)) != sizeof(padding)) {
    errno_exit("write");
  }
}

int main(int argc, char *argv[]) {
  int fd = open("cs111-base.img", O_CREAT | O_WRONLY, 0666);
  if (fd == -1) {
    errno_exit("open");
  }

  if (ftruncate(fd, 0)) {
    errno_exit("ftruncate");
  }
  if (ftruncate(fd, NUM_BLOCKS * BLOCK_SIZE)) {
    errno_exit("ftruncate");
  }

  write_superblock(fd);
  write_block_group_descriptor_table(fd);
  write_block_bitmap(fd);
  write_inode_bitmap(fd);
  write_inode_table(fd);
  write_root_dir_block(fd);
  write_lost_and_found_dir_block(fd);
  write_hello_world_file_block(fd);

  if (close(fd)) {
    errno_exit("close");
  }
  return 0;
}
