#include "fs.h"
#include "disk.h"

#define MAX_FILES 64
#define MAX_FILE_SIZE ((1 << 20) * 20) // 20 MB
#define MAX_FILE_NAME_CHAR 16
#define DIRECT_OFFSETS_PER_INODE 12
#define DIRECT_OFFSETS_PER_BLOCK (BLOCK_SIZE / sizeof(uint16_t))
#define INODE_SIZE sizeof(struct inode)
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define DIR_ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(struct dir_entry))
#define METADATA_BLOCKS 5
#define MAX_FD 32
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

struct super_block {
  uint16_t dir_table_offset;
  uint16_t inode_metadata_offset;
  uint16_t used_block_bitmap_offset;
  uint16_t inode_offset;
  uint16_t data_offset;
};

struct inode {
  uint16_t direct_offset[DIRECT_OFFSETS_PER_INODE];
  uint16_t single_indirect_offset;
  uint16_t double_indirect_offset;
  int file_size;
};

struct dir_entry {
  bool is_used;
  uint16_t inode_number;
  const char *name;
};

struct file_descriptor {
  bool is_used;
  uint16_t inode_number;
  int offset;
};

union fs_block {
  struct super_block super;
  struct dir_entry dir_table[DIR_ENTRIES_PER_BLOCK];
  uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
  uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
  struct inode inode_table[INODES_PER_BLOCK];
  uint16_t block_offsets[DIRECT_OFFSETS_PER_BLOCK];
  char data[BLOCK_SIZE];
};

// on-disk and in-memory
struct super_block sb;
struct dir_entry dir_table[MAX_FILES];
uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
struct inode inode_table[MAX_FILES];

// in-memory
bool is_mounted = false;
struct file_descriptor fds[MAX_FD];

// Helper functions

struct dir_entry *get_dentry(const char *name) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir_table[i].is_used && strcmp(dir_table[i].name, name) == 0)
      return &dir_table[i];
  }
  return NULL;
}

struct dir_entry *get_unused_dentry() {
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir_table[i].is_used == false) {
      return &dir_table[i];
    }
  }
  return NULL;
}

bool bitmap_test(uint8_t *bitmap, int idx) {
  return bitmap[idx / CHAR_BIT] & (1 << (idx % CHAR_BIT));
}

void bitmap_set(uint8_t *bitmap, int idx, bool val) {
  if (bitmap_test(bitmap, idx) == val)
    return;
  bitmap[idx / CHAR_BIT] ^= 1 << (idx % CHAR_BIT);
}

int get_unused_inum() {
  for (int i = 0; i < MAX_FILES; i++) {
    if (bitmap_test(inode_bitmap, i) == 0)
      return i;
  }
  return -1;
}

// Returns the block number of the data block at the given file offset.
// Returns -1 if the block is not allocated
int get_data_block_offset(uint16_t inum, int file_offset) {
  assert(inum >= 0 && inum < MAX_FILES);
  assert(file_offset >= 0 && file_offset < MAX_FILE_SIZE);

  struct inode inode = inode_table[inum];
  uint16_t block_num;
  int block_offset = file_offset / BLOCK_SIZE;

  // direct offset
  if (block_offset < DIRECT_OFFSETS_PER_INODE) {
    block_num = inode.direct_offset[block_offset];
    return block_num == 0 ? -1 : block_num;
  }

  union fs_block block_buffer;
  block_offset -= DIRECT_OFFSETS_PER_INODE;

  // single indirect
  if (block_offset < DIRECT_OFFSETS_PER_BLOCK) {
    assert(inode.single_indirect_offset != 0);
    if (block_read(inode.single_indirect_offset, &block_buffer)) {
      fprintf(stderr,
              "get_data_block_offset: failed to read single indirect block\n");
      return -1;
    }
    block_num = block_buffer.block_offsets[block_offset];
    return block_num == 0 ? -1 : block_num;
  }

  // double indirect
  block_offset -= DIRECT_OFFSETS_PER_BLOCK;
  assert(inode.double_indirect_offset != 0);
  if (block_read(inode.double_indirect_offset, &block_buffer)) {
    fprintf(stderr,
            "get_data_block_offset: failed to read double indirect block\n");
    return -1;
  }
  block_offset /= DIRECT_OFFSETS_PER_BLOCK;
  if (block_read(block_buffer.block_offsets[block_offset], &block_buffer)) {
    fprintf(stderr,
            "get_data_block_offset: failed to read double indirect block\n");
    return -1;
  }
  block_offset %= DIRECT_OFFSETS_PER_BLOCK;
  block_num = block_buffer.block_offsets[block_offset];
  return block_num == 0 ? -1 : block_num;
}

// Library functions

int make_fs(const char *disk_name) {
  if (make_disk(disk_name)) {
    fprintf(stderr, "make_fs: make_disk failed\n");
    return -1;
  }
  if (open_disk(disk_name)) {
    fprintf(stderr, "make_fs: open_disk failed\n");
    return -1;
  }

  sb.dir_table_offset = 1;
  sb.inode_metadata_offset = 2;
  sb.used_block_bitmap_offset = 3;
  sb.inode_offset = 4;
  sb.data_offset = 5;

  // write super block
  union fs_block block_buffer;
  block_buffer.super = sb;
  if (block_write(0, &block_buffer)) {
    fprintf(stderr, "make_fs: failed to write super block\n");
    return -1;
  }

  // write used block bitmap
  for (int i = 0; i < METADATA_BLOCKS; i++) {
    bitmap_set(used_block_bitmap, i, 1);
  }
  memset(&block_buffer, 0, BLOCK_SIZE);
  memcpy(block_buffer.used_block_bitmap, used_block_bitmap,
         sizeof(used_block_bitmap));
  if (block_write(sb.used_block_bitmap_offset, &block_buffer)) {
    fprintf(stderr, "make_fs: failed to write used block bitmap\n");
    return -1;
  }

  if (close_disk()) {
    fprintf(stderr, "make_fs: close_disk failed\n");
    return -1;
  }

  return 0;
}

int mount_fs(const char *disk_name) {
  if (open_disk(disk_name)) {
    fprintf(stderr, "mount_fs: open_disk failed\n");
    return -1;
  }

  union fs_block block_buffer;

  // read super block
  if (block_read(0, &block_buffer)) {
    fprintf(stderr, "mount_fs: failed to read super block\n");
    return -1;
  }
  struct super_block sb_slice = block_buffer.super;
  if (sb_slice.dir_table_offset == 0) {
    fprintf(stderr, "mount_fs: file system not initialized\n");
    return -1;
  }
  sb = sb_slice;

  // read directory table
  if (block_read(sb_slice.dir_table_offset, &block_buffer)) {
    fprintf(stderr, "mount_fs: failed to read directory table\n");
    return -1;
  }
  memcpy(dir_table, block_buffer.dir_table, sizeof(dir_table));

  // read inode bitmap
  if (block_read(sb_slice.inode_metadata_offset, &block_buffer)) {
    fprintf(stderr, "mount_fs: failed to read inode bitmap\n");
    return -1;
  }
  memcpy(inode_bitmap, block_buffer.inode_bitmap, sizeof(inode_bitmap));

  // read used block bitmap
  if (block_read(sb_slice.used_block_bitmap_offset, &block_buffer)) {
    fprintf(stderr, "mount_fs: failed to read used block bitmap\n");
    return -1;
  }
  memcpy(used_block_bitmap, block_buffer.used_block_bitmap,
         sizeof(used_block_bitmap));

  // read inode table
  if (block_read(sb_slice.inode_offset, &block_buffer)) {
    fprintf(stderr, "mount_fs: failed to read inodes\n");
    return -1;
  }
  memcpy(inode_table, block_buffer.inode_table, sizeof(inode_table));

  is_mounted = true;
  return 0;
}

int umount_fs(const char *disk_name) {
  if (is_mounted == false) {
    fprintf(stderr, "umount_fs: file system not mounted\n");
    return -1;
  }

  union fs_block block_buffer;

  // write super block
  block_buffer.super = sb;
  if (block_write(0, &block_buffer)) {
    fprintf(stderr, "umount_fs: failed to write super block\n");
    return -1;
  }

  // write directory table
  memset(&block_buffer, 0, BLOCK_SIZE);
  memcpy(block_buffer.dir_table, dir_table, sizeof(dir_table));
  if (block_write(sb.dir_table_offset, &block_buffer)) {
    fprintf(stderr, "umount_fs: failed to write directory table\n");
    return -1;
  }

  // write inode bitmap
  memset(&block_buffer, 0, BLOCK_SIZE);
  memcpy(block_buffer.inode_bitmap, inode_bitmap, sizeof(inode_bitmap));
  if (block_write(sb.inode_metadata_offset, &block_buffer)) {
    fprintf(stderr, "umount_fs: failed to write inode bitmap\n");
    return -1;
  }

  // write used block bitmap
  memset(&block_buffer, 0, BLOCK_SIZE);
  memcpy(block_buffer.used_block_bitmap, used_block_bitmap,
         sizeof(used_block_bitmap));
  if (block_write(sb.used_block_bitmap_offset, &block_buffer)) {
    fprintf(stderr, "umount_fs: failed to write used block bitmap\n");
    return -1;
  }

  // write inode table
  memset(&block_buffer, 0, BLOCK_SIZE);
  memcpy(block_buffer.inode_table, inode_table, sizeof(inode_table));
  if (block_write(sb.inode_offset, &block_buffer)) {
    fprintf(stderr, "umount_fs: failed to write inodes\n");
    return -1;
  }

  if (close_disk()) {
    fprintf(stderr, "umount_fs: close_disk failed\n");
    return -1;
  }

  memset(fds, 0, sizeof(fds));
  is_mounted = false;
  return 0;
}

int fs_open(const char *name) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_open: file system not mounted\n");
    return -1;
  }
  struct dir_entry *dentry = get_dentry(name);
  if (dentry == NULL) {
    fprintf(stderr, "fs_open: file not found\n");
    return -1;
  }
  for (int fildes = 0; fildes < MAX_FD; fildes++) {
    struct file_descriptor fd = fds[fildes];
    if (fd.is_used == false) {
      fd.is_used = true;
      fd.inode_number = dentry->inode_number;
      fd.offset = 0;
      return fildes;
    }
  }
  fprintf(stderr, "fs_open: no available file descriptors\n");
  return -1;
}

int fs_close(int fildes) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_close: file system not mounted\n");
    return -1;
  }
  struct file_descriptor fd = fds[fildes];
  if (fd.is_used == false) {
    fprintf(stderr, "fs_close: file descriptor not in use\n");
    return -1;
  }
  fd.is_used = false;
  fd.inode_number = -1;
  fd.offset = 0;
  return 0;
}

int fs_create(const char *name) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
    return -1;
  }
  if (strlen(name) == 0 || strlen(name) > MAX_FILE_NAME_CHAR) {
    fprintf(stderr, "fs_create: invalid file name\n");
    return -1;
  }
  if (get_dentry(name)) {
    fprintf(stderr, "fs_create: file already exists\n");
    return -1;
  }
  if (memcmp(inode_bitmap, "\xff", sizeof(inode_bitmap)) == 0) {
    fprintf(stderr, "fs_create: root directory is full\n");
    return -1;
  }

  int inum = get_unused_inum();
  assert(inum != -1);
  bitmap_set(inode_bitmap, inum, 1);

  struct dir_entry *dentry = get_unused_dentry();
  dentry->is_used = true;
  dentry->inode_number = inum;
  dentry->name = name;
  return 0;
}

int fs_delete(const char *name) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_delete: file system not mounted\n");
    return -1;
  }
  struct dir_entry *dentry = get_dentry(name);
  if (dentry == NULL) {
    fprintf(stderr, "fs_delete: file not found\n");
    return -1;
  }
  for (int fildes = 0; fildes < MAX_FD; fildes++) {
    struct file_descriptor fd = fds[fildes];
    if (fd.is_used && dentry->inode_number == fd.inode_number) {
      fprintf(stderr, "fs_delete: file is open\n");
      return -1;
    }
  }
  // TODO: free metadata and data blocks
  return 0;
}

int fs_read(int fildes, void *buf, size_t nbyte) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_read: file system not mounted\n");
    return -1;
  }
  struct file_descriptor fd = fds[fildes];
  if (fd.is_used == false) {
    fprintf(stderr, "fs_read: invalid file descriptor\n");
    return -1;
  }
  int block_offset = get_data_block_offset(fd.inode_number, fd.offset);
  if (block_offset < 0) {
    fprintf(stderr, "fs_read: no data block found for offset\n");
    return -1;
  }

  union fs_block block_buffer;
  if (block_read(block_offset, &block_buffer)) {
    fprintf(stderr, "fs_read: failed to read data block\n");
    return -1;
  }
  uint16_t offset_in_block = fd.offset % BLOCK_SIZE;
  size_t bytes_read = 0;
  while (bytes_read < nbyte) {
    if (offset_in_block == BLOCK_SIZE) {
      // read next data block
      block_offset = get_data_block_offset(fd.inode_number, block_offset);
      if (block_offset < 0) {
        break;
      }
      if (block_read(block_offset, &block_buffer)) {
        fprintf(stderr, "fs_read: failed to read data block\n");
        return -1;
      }
      offset_in_block = 0;
    }
    size_t bytes_to_read = nbyte - bytes_read;
    bytes_to_read = MIN(bytes_to_read, BLOCK_SIZE - offset_in_block);
    memcpy((char *)buf + bytes_read, block_buffer.data + offset_in_block,
           bytes_to_read);
    bytes_read += bytes_to_read;
    offset_in_block += bytes_to_read;
  }
  fd.offset += bytes_read;
  return bytes_read;
}

int fs_write(int fildes, void *buf, size_t nbyte) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_write: file system not mounted\n");
    return -1;
  }
  // TODO: read first before writing
  return 0;
}

int fs_get_filesize(int fildes) {
  struct file_descriptor fd = fds[fildes];
  if (fd.is_used == false) {
    fprintf(stderr, "fs_get_filesize: invalid file descriptor\n");
    return -1;
  }
  return inode_table[fd.inode_number].file_size;
}

int fs_listfiles(char ***files) {
  char *file = **files;
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir_table[i].is_used) {
      const char *file_name = dir_table[i].name;
      if (file_name == NULL) {
        fprintf(stderr, "fs_listfiles: invalid file name\n");
        return -1;
      }
      strcpy(file, file_name);
      file++;
    }
  }
  file = NULL;
  return 0;
}

int fs_lseek(int fildes, off_t offset) {
  if (offset < 0) {
    fprintf(stderr, "fs_lseek: invalid offset\n");
    return -1;
  };
  struct file_descriptor fd = fds[fildes];
  if (fd.is_used == false) {
    fprintf(stderr, "fs_lseek: invalid file descriptor\n");
    return -1;
  }
  if (offset > inode_table[fd.inode_number].file_size) {
    fprintf(stderr, "fs_lseek: offset exceeds file size\n");
    return -1;
  }
  fd.offset = offset;
  return 0;
}

int fs_truncate(int fildes, off_t length) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_truncate: file system not mounted\n");
    return -1;
  }
  struct file_descriptor fd = fds[fildes];
  if (fd.is_used == false) {
    fprintf(stderr, "fs_truncate: invalid file descriptor\n");
    return -1;
  }
  int file_size = inode_table[fd.inode_number].file_size;
  if (length < 0 || length > file_size) {
    fprintf(stderr, "fs_truncate: invalid length\n");
    return -1;
  }
  // TODO: free data blocks
  return 0;
}
