#include "fs.h"
#include "disk.h"

#define MAX_FILES 64
#define MAX_FILE_SIZE ((1 << 20) * 20) // 20 MB
#define MAX_FILE_NAME_CHAR 16
#define DIRECT_OFFSETS_PER_INODE 10
#define INODE_SIZE sizeof(struct inode)
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define MAX_FD 32

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
  int inode_number;
  const char *name;
};

struct file_descriptor {
  bool is_used;
  int inode_number;
  int offset;
};

union fs_block {
  struct super_block super;
  struct dir_entry dir[BLOCK_SIZE / sizeof(struct dir_entry)];
  uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
  uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
  struct inode inodes[BLOCK_SIZE / INODE_SIZE];
  char data[BLOCK_SIZE];
};

// on-disk and in-memory
struct super_block sb;
struct dir_entry dir[MAX_FILES];
uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
struct inode inodes[MAX_FILES];

// in-memory
bool is_mounted = false;
struct file_descriptor fds[MAX_FD];

// Helper functions

int get_file_index(const char *name) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir[i].is_used && strcmp(dir[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

bool bitmap_test(uint8_t *bitmap, int idx) {
  return bitmap[idx / CHAR_BIT] & (1 << (idx % CHAR_BIT));
}

void bitmap_set(uint8_t *bitmap, int idx, bool val) {
  if (bitmap_test(bitmap, idx) == val)
    return;
  bitmap[idx / CHAR_BIT] ^= 1 << (idx % CHAR_BIT);
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
  union fs_block block;
  block.super = sb;
  if (block_write(0, &block)) {
    fprintf(stderr, "make_fs: failed to write super block\n");
    return -1;
  }

  // write used block bitmap
  int metadata_blocks = 4;
  for (int i = 0; i < metadata_blocks; i++) {
    bitmap_set(used_block_bitmap, i, 1);
  }
  memset(&block, 0, BLOCK_SIZE);
  memcpy(block.used_block_bitmap, used_block_bitmap, sizeof(used_block_bitmap));
  if (block_write(sb.used_block_bitmap_offset, &block)) {
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

  // read super block
  union fs_block block;
  if (block_read(0, &block)) {
    fprintf(stderr, "mount_fs: failed to read super block\n");
    return -1;
  }
  struct super_block sb_slice = block.super;
  if (sb_slice.dir_table_offset == 0) {
    fprintf(stderr, "mount_fs: file system not initialized\n");
    return -1;
  }
  sb = sb_slice;

  // read directory table
  memset(&block, 0, BLOCK_SIZE);
  if (block_read(sb_slice.dir_table_offset, &block)) {
    fprintf(stderr, "mount_fs: failed to read directory table\n");
    return -1;
  }
  memcpy(dir, block.dir, sizeof(dir));

  // read inode bitmap
  memset(&block, 0, BLOCK_SIZE);
  if (block_read(sb_slice.inode_metadata_offset, &block)) {
    fprintf(stderr, "mount_fs: failed to read inode bitmap\n");
    return -1;
  }
  memcpy(inode_bitmap, block.inode_bitmap, sizeof(inode_bitmap));

  // read used block bitmap
  memset(&block, 0, BLOCK_SIZE);
  if (block_read(sb_slice.used_block_bitmap_offset, &block)) {
    fprintf(stderr, "mount_fs: failed to read used block bitmap\n");
    return -1;
  }
  memcpy(used_block_bitmap, block.used_block_bitmap, sizeof(used_block_bitmap));

  // read inodes
  memset(&block, 0, BLOCK_SIZE);
  if (block_read(sb_slice.inode_offset, &block)) {
    fprintf(stderr, "mount_fs: failed to read inodes\n");
    return -1;
  }
  memcpy(inodes, block.inodes, sizeof(inodes));

  is_mounted = true;
  return 0;
}

int umount_fs(const char *disk_name) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
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
    fprintf(stderr, "fs_create: file system not mounted\n");
    return -1;
  }
  int file_index = get_file_index(name);
  if (file_index < 0) {
    fprintf(stderr, "fs_open: file not found\n");
    return -1;
  }
  for (int fildes = 0; fildes < MAX_FD; fildes++) {
    struct file_descriptor fd = fds[fildes];
    if (fd.is_used == false) {
      fd.is_used = true;
      fd.inode_number = dir[file_index].inode_number;
      fd.offset = 0;
      return fildes;
    }
  }
  fprintf(stderr, "fs_open: no available file descriptors\n");
  return -1;
}

int fs_close(int fildes) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
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
  if (get_file_index(name) >= 0) {
    fprintf(stderr, "fs_create: file already exists\n");
    return -1;
  }
  if (memcmp(inode_bitmap, "\xff", sizeof(inode_bitmap)) == 0) {
    fprintf(stderr, "fs_create: root directory is full\n");
    return -1;
  }

  // TODO: allocate inode and data blocks
  // int block_num = sb.inode_metadata_offset + (inumber / INODES_PER_BLOCK);
  // int block_offset = inumber % INODES_PER_BLOCK;
  return 0;
}

int fs_delete(const char *name) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
    return -1;
  }
  int i = get_file_index(name);
  if (i < 0) {
    fprintf(stderr, "fs_delete: file not found\n");
    return -1;
  }
  for (int fildes = 0; fildes < MAX_FD; fildes++) {
    struct file_descriptor fd = fds[fildes];
    if (fd.is_used && dir[i].inode_number == fd.inode_number) {
      fprintf(stderr, "fs_delete: file is open\n");
      return -1;
    }
  }
  // TODO: free metadata and data blocks
  return 0;
}

int fs_read(int fildes, void *buf, size_t nbyte) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
    return -1;
  }
  return 0;
}

int fs_write(int fildes, void *buf, size_t nbyte) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
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
  return inodes[fd.inode_number].file_size;
}

int fs_listfiles(char ***files) {
  char *file = **files;
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir[i].is_used) {
      const char *file_name = dir[i].name;
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
  if (offset > inodes[fd.inode_number].file_size) {
    fprintf(stderr, "fs_lseek: offset exceeds file size\n");
    return -1;
  }
  fd.offset = offset;
  return 0;
}

int fs_truncate(int fildes, off_t length) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_create: file system not mounted\n");
    return -1;
  }
  // TODO: truncate file
  return 0;
}
