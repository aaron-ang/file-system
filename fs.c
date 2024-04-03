#include "fs.h"
#include "disk.h"

#define MAX_FILES 64
#define MAX_FILE_SIZE ((1 << 20) * 40) // 40 MiB
#define MAX_FILE_NAME_CHAR 16
#define DIRECT_OFFSETS_PER_INODE 12
#define DIRECT_OFFSETS_PER_BLOCK (BLOCK_SIZE / sizeof(uint16_t))
#define INODE_SIZE sizeof(struct inode)
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define DIR_ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(struct dir_entry))
#define METADATA_BLOCKS 5
#define MAX_FD 32
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

struct super_block {
  uint16_t dir_table_offset;
  uint16_t inode_metadata_offset;
  uint16_t used_block_bitmap_offset;
  uint16_t inode_offset;
  uint16_t data_offset;
};

struct dir_entry {
  bool is_used;
  uint16_t inode_number;
  const char *name;
};

struct inode {
  uint16_t direct_offset[DIRECT_OFFSETS_PER_INODE];
  uint16_t single_indirect_offset;
  uint16_t double_indirect_offset;
  int file_size;
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

struct file_descriptor {
  bool is_used;
  uint16_t inode_number;
  int offset;
};

enum indirection_level {
  SINGLE_INDIRECTION,
  DOUBLE_INDIRECTION,
};

// in-memory and on-disk
struct super_block sb;
struct dir_entry dir_table[MAX_FILES];
uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
struct inode inode_table[MAX_FILES];

// in-memory only
bool is_mounted = false;
struct file_descriptor fds[MAX_FD];

/*
 * Helper functions
 */

static struct dir_entry *get_dentry(const char *name);
static struct dir_entry *claim_dentry(uint16_t inum, const char *name);
static void clear_dentry(struct dir_entry *dentry);
static bool bitmap_test(const uint8_t *bitmap, int idx);
static void bitmap_set(uint8_t *bitmap, int idx, bool val);
static bool bitmap_full(const uint8_t *bitmap, int size);
static int claim_inum_from_bitmap();
static int claim_unused_data_block();
static int add_inode_data_block(uint16_t inum, int block_num);
static int get_data_block_num(uint16_t inum, int file_offset);
static size_t read_bytes(int block_num, struct file_descriptor *fd, void *buf,
                         size_t nbyte);
static size_t write_bytes(int block_num, struct file_descriptor *fd,
                          const void *buf, size_t nbyte);
static int clear_indirect_block(int block_num, int indirection_level);

struct dir_entry *get_dentry(const char *name) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir_table[i].is_used && strcmp(dir_table[i].name, name) == 0)
      return &dir_table[i];
  }
  return NULL;
}

struct dir_entry *claim_dentry(uint16_t inum, const char *name) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir_table[i].is_used == false) {
      dir_table[i].is_used = true;
      dir_table[i].inode_number = inum;
      dir_table[i].name = name;
      return &dir_table[i];
    }
  }
  return NULL;
}

void clear_dentry(struct dir_entry *dentry) {
  if (dentry == NULL)
    return;
  dentry->is_used = false;
  dentry->inode_number = -1;
  dentry->name = NULL;
}

bool bitmap_test(const uint8_t *bitmap, int idx) {
  return bitmap[idx / CHAR_BIT] & (1 << (idx % CHAR_BIT));
}

void bitmap_set(uint8_t *bitmap, int idx, bool val) {
  if (bitmap_test(bitmap, idx) == val)
    return;
  bitmap[idx / CHAR_BIT] ^= 1 << (idx % CHAR_BIT);
}

bool bitmap_full(const uint8_t *bitmap, int size) {
  uint8_t full_bitmap[size];
  memset(full_bitmap, 0xff, size);
  return memcmp(bitmap, full_bitmap, size) == 0;
}

int claim_inum_from_bitmap() {
  for (int i = 0; i < MAX_FILES; i++) {
    if (bitmap_test(inode_bitmap, i) == 0) {
      bitmap_set(inode_bitmap, i, 1);
      return i;
    }
  }
  return -1;
}

int claim_unused_data_block() {
  for (int i = sb.data_offset; i < DISK_BLOCKS; i++) {
    if (bitmap_test(used_block_bitmap, i) == 0) {
      bitmap_set(used_block_bitmap, i, 1);
      return i;
    }
  }
  return -1;
}

int add_inode_data_block(uint16_t inum, int block_num) {
  struct inode *inode = &inode_table[inum];
  for (int i = 0; i < DIRECT_OFFSETS_PER_INODE; i++) {
    if (inode->direct_offset[i] == 0) {
      inode->direct_offset[i] = block_num;
      return 0;
    }
  }
  union fs_block indirect_block_buffer;
  memset(&indirect_block_buffer, 0, BLOCK_SIZE);
  if (inode->single_indirect_offset == 0) {
    int new_block_num = claim_unused_data_block();
    if (new_block_num == -1) {
      return -1;
    }
    indirect_block_buffer.block_offsets[0] = block_num;
    if (block_write(new_block_num, &indirect_block_buffer) == -1) {
      fprintf(stderr, "add_inode_data_block: block_write failed\n");
      return -1;
    }
    inode->single_indirect_offset = new_block_num;
    return 0;
  }
  if (block_read(inode->single_indirect_offset, &indirect_block_buffer) == -1) {
    fprintf(stderr, "add_inode_data_block: block_read failed\n");
    return -1;
  }
  for (int i = 0; i < DIRECT_OFFSETS_PER_BLOCK; i++) {
    if (indirect_block_buffer.block_offsets[i] == 0) {
      indirect_block_buffer.block_offsets[i] = block_num;
      if (block_write(inode->single_indirect_offset, &indirect_block_buffer) ==
          -1) {
        fprintf(stderr, "add_inode_data_block: block_write failed\n");
        return -1;
      }
      return 0;
    }
  }
  memset(&indirect_block_buffer, 0, BLOCK_SIZE);
  if (inode->double_indirect_offset == 0) {
    int first_indirect_block_num = claim_unused_data_block();
    int second_indirect_block_num = claim_unused_data_block();
    if (first_indirect_block_num <= 0 || second_indirect_block_num <= 0) {
      return -1;
    }
    inode->double_indirect_offset = first_indirect_block_num;
    // first indirect block
    indirect_block_buffer.block_offsets[0] = second_indirect_block_num;
    if (block_write(first_indirect_block_num, &indirect_block_buffer) == -1) {
      fprintf(stderr, "add_inode_data_block: block_write failed\n");
      return -1;
    }
    // second indirect block
    indirect_block_buffer.block_offsets[0] = block_num;
    if (block_write(second_indirect_block_num, &indirect_block_buffer) == -1) {
      fprintf(stderr, "add_inode_data_block: block_write failed\n");
      return -1;
    }
    return 0;
  }
  if (block_read(inode->double_indirect_offset, &indirect_block_buffer) == -1) {
    fprintf(stderr, "add_inode_data_block: block_read failed\n");
    return -1;
  }
  union fs_block second_indirect_block;
  memset(&second_indirect_block, 0, BLOCK_SIZE);
  for (int i = 0; i < DIRECT_OFFSETS_PER_BLOCK; i++) {
    if (indirect_block_buffer.block_offsets[i] == 0) {
      int indirect_block_num = claim_unused_data_block();
      if (indirect_block_num == -1) {
        return -1;
      }
      indirect_block_buffer.block_offsets[i] = indirect_block_num;
      if (block_write(inode->double_indirect_offset, &indirect_block_buffer) ==
          -1) {
        fprintf(stderr, "add_inode_data_block: block_write failed\n");
        return -1;
      }
      second_indirect_block.block_offsets[0] = block_num;
      if (block_write(indirect_block_num, &second_indirect_block) == -1) {
        fprintf(stderr, "add_inode_data_block: block_write failed\n");
        return -1;
      }
      return 0;
    }
    if (block_read(indirect_block_buffer.block_offsets[i],
                   &second_indirect_block) == -1) {
      fprintf(stderr, "add_inode_data_block: block_read failed\n");
      return -1;
    }
    for (int j = 0; j < DIRECT_OFFSETS_PER_BLOCK; j++) {
      if (second_indirect_block.block_offsets[j] == 0) {
        second_indirect_block.block_offsets[j] = block_num;
        if (block_write(indirect_block_buffer.block_offsets[i],
                        &second_indirect_block) == -1) {
          fprintf(stderr, "add_inode_data_block: block_write failed\n");
          return -1;
        }
        return 0;
      }
    }
  }
  return -1;
}

// Returns the block number of the data block at the given file offset.
// Returns 0 if the block is not allocated.
// Returns -1 on read/write error.
int get_data_block_num(uint16_t inum, int file_offset) {
  assert(inum >= 0 && inum < MAX_FILES);
  assert(file_offset >= 0 && file_offset < MAX_FILE_SIZE);

  struct inode *inode = &inode_table[inum];
  int block_idx = file_offset / BLOCK_SIZE;

  // direct offset
  if (block_idx < DIRECT_OFFSETS_PER_INODE) {
    return inode->direct_offset[block_idx];
  }

  union fs_block block_buffer;
  block_idx -= DIRECT_OFFSETS_PER_INODE;

  // single indirect
  if (block_idx < DIRECT_OFFSETS_PER_BLOCK) {
    if (inode->single_indirect_offset == 0) {
      return 0;
    }
    if (block_read(inode->single_indirect_offset, &block_buffer)) {
      fprintf(stderr,
              "get_data_block_num: failed to read single indirect block\n");
      return -1;
    }
    return block_buffer.block_offsets[block_idx];
  }

  // double indirect
  block_idx -= DIRECT_OFFSETS_PER_BLOCK;
  if (inode->double_indirect_offset == 0) {
    return 0;
  }
  if (block_read(inode->double_indirect_offset, &block_buffer)) {
    fprintf(stderr,
            "get_data_block_num: failed to read double indirect block\n");
    return -1;
  }
  int block_offset = block_idx / DIRECT_OFFSETS_PER_BLOCK;
  if (block_read(block_buffer.block_offsets[block_offset], &block_buffer)) {
    fprintf(stderr,
            "get_data_block_num: failed to read single indirect block\n");
    return -1;
  }
  block_idx %= DIRECT_OFFSETS_PER_BLOCK;
  return block_buffer.block_offsets[block_idx];
}

size_t read_bytes(int block_num, struct file_descriptor *fd, void *buf,
                  size_t nbyte) {
  union fs_block block_buffer;
  if (block_read(block_num, &block_buffer)) {
    fprintf(stderr, "read_bytes: failed to read data block %d\n", block_num);
    return -1;
  }
  uint16_t offset_in_block = fd->offset % BLOCK_SIZE;
  int file_size = inode_table[fd->inode_number].file_size;
  size_t bytes_read = 0;
  nbyte = MIN(nbyte, file_size - fd->offset);
  while (bytes_read < nbyte) {
    if (offset_in_block == BLOCK_SIZE) {
      block_num = get_data_block_num(fd->inode_number, fd->offset);
      assert(block_num >= sb.data_offset);
      if (block_read(block_num, &block_buffer)) {
        fprintf(stderr, "read_bytes: failed to read data block %d\n",
                block_num);
        return -1;
      }
      offset_in_block = 0;
    }
    size_t bytes_to_read =
        MIN(nbyte - bytes_read, BLOCK_SIZE - offset_in_block);
    memcpy((char *)buf + bytes_read, block_buffer.data + offset_in_block,
           bytes_to_read);
    bytes_read += bytes_to_read;
    offset_in_block += bytes_to_read;
    fd->offset += bytes_to_read;
  }
  return bytes_read;
}

size_t write_bytes(int block_num, struct file_descriptor *fd, const void *buf,
                   size_t nbyte) {
  union fs_block block_buffer;
  if (block_read(block_num, &block_buffer)) {
    fprintf(stderr, "write_bytes: failed to read data block\n");
    return -1;
  }
  uint16_t inum = fd->inode_number;
  uint16_t offset_in_block = fd->offset % BLOCK_SIZE;
  size_t bytes_written = 0;
  nbyte = MIN(nbyte, MAX_FILE_SIZE - fd->offset);
  while (bytes_written < nbyte) {
    if (offset_in_block == BLOCK_SIZE) {
      if (block_write(block_num, &block_buffer)) {
        fprintf(stderr, "write_bytes: failed to write data block\n");
        return -1;
      }
      if (bitmap_full(used_block_bitmap, sizeof(used_block_bitmap))) {
        break;
      }
      // TODO: somehow block number is not updated in double indirect block
      block_num = get_data_block_num(inum, fd->offset);
      if (block_num == -1) {
        fprintf(stderr, "write_bytes: failed to get data block number\n");
        return -1;
      }
      if (block_num == 0) { // allocate new data block
        block_num = claim_unused_data_block();
        // TODO: somehow block number is not updated in double indirect block
        if (add_inode_data_block(inum, block_num)) {
          fprintf(stderr,
                  "write_bytes: failed to assign data block %d to inode\n",
                  block_num);
          return -1;
        }
      }
      assert(block_num >= sb.data_offset);
      offset_in_block = 0;
    }
    size_t bytes_to_write =
        MIN(nbyte - bytes_written, BLOCK_SIZE - offset_in_block);
    memcpy(block_buffer.data + offset_in_block, (char *)buf + bytes_written,
           bytes_to_write);
    bytes_written += bytes_to_write;
    offset_in_block += bytes_to_write;
    fd->offset += bytes_to_write;
  }
  if (block_write(block_num, &block_buffer)) {
    fprintf(stderr, "write_bytes: failed to write data block\n");
    return -1;
  }
  inode_table[inum].file_size = MAX(inode_table[inum].file_size, fd->offset);
  return bytes_written;
}

// Recursively clear indirect blocks. indirection_level > 0 means entries in
// block_num point to indirect blocks.
int clear_indirect_block(int block_num, int indirection_level) {
  union fs_block block_buffer;
  if (block_read(block_num, &block_buffer)) {
    fprintf(stderr, "clear_indirect_block: failed to read indirect block\n");
    return -1;
  }
  union fs_block empty_block;
  memset(&empty_block, 0, BLOCK_SIZE);
  for (int i = 0; i < DIRECT_OFFSETS_PER_BLOCK; i++) {
    if (block_buffer.block_offsets[i]) {
      if (indirection_level > SINGLE_INDIRECTION) {
        if (clear_indirect_block(block_buffer.block_offsets[i],
                                 indirection_level - 1)) {
          fprintf(stderr,
                  "clear_indirect_block: failed to clear indirect block\n");
          return -1;
        }
      } else if (block_write(block_buffer.block_offsets[i], &empty_block)) {
        fprintf(stderr, "clear_indirect_block: failed to clear data block %d\n",
                block_buffer.block_offsets[i]);
        return -1;
      }
      bitmap_set(used_block_bitmap, block_buffer.block_offsets[i], 0);
    }
  }
  if (block_write(block_num, &empty_block)) {
    fprintf(stderr, "clear_indirect_block: failed to clear indirect block\n");
    return -1;
  }
  bitmap_set(used_block_bitmap, block_num, 0);
  return 0;
}

/*
 * Library functions
 */

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
  memset(&block_buffer, 0, BLOCK_SIZE);
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
  memset(&block_buffer, 0, BLOCK_SIZE);

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
    struct file_descriptor *fd = &fds[fildes];
    if (fd->is_used == false) {
      fd->is_used = true;
      fd->inode_number = dentry->inode_number;
      fd->offset = 0;
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
  struct file_descriptor *fd = &fds[fildes];
  if (fd->is_used == false) {
    fprintf(stderr, "fs_close: file descriptor not in use\n");
    return -1;
  }
  fd->is_used = false;
  fd->inode_number = -1;
  fd->offset = 0;
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
  if (bitmap_full(inode_bitmap, sizeof(inode_bitmap))) {
    fprintf(stderr, "fs_create: root directory is full\n");
    return -1;
  }
  int inum = claim_inum_from_bitmap();
  assert(inum != -1);
  struct dir_entry *dentry = claim_dentry(inum, name);
  assert(dentry != NULL);
  int free_block_num = claim_unused_data_block();
  if (free_block_num == -1) {
    fprintf(stderr, "fs_create: no free blocks\n");
    return -1;
  }
  assert(free_block_num >= sb.data_offset);
  struct inode *inode = &inode_table[inum];
  inode->direct_offset[0] = free_block_num;
  inode->file_size = 0;
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
    struct file_descriptor *fd = &fds[fildes];
    if (fd->is_used && dentry->inode_number == fd->inode_number) {
      fprintf(stderr, "fs_delete: file is open\n");
      return -1;
    }
  }
  struct inode *inode = &inode_table[dentry->inode_number];
  union fs_block empty_block;
  memset(&empty_block, 0, BLOCK_SIZE);
  for (int i = 0; i < DIRECT_OFFSETS_PER_INODE; i++) {
    if (inode->direct_offset[i]) {
      if (block_write(inode->direct_offset[i], &empty_block)) {
        fprintf(stderr, "fs_delete: failed to clear data block %d\n",
                inode->direct_offset[i]);
        return -1;
      }
      inode->direct_offset[i] = 0;
      bitmap_set(used_block_bitmap, inode->direct_offset[i], 0);
    }
  }
  if (inode->single_indirect_offset) {
    if (clear_indirect_block(inode->single_indirect_offset,
                             SINGLE_INDIRECTION)) {
      fprintf(stderr, "fs_delete: failed to clear single indirect block\n");
      return -1;
    }
    inode->single_indirect_offset = 0;
  }
  if (inode->double_indirect_offset) {
    if (clear_indirect_block(inode->double_indirect_offset,
                             DOUBLE_INDIRECTION)) {
      fprintf(stderr, "fs_delete: failed to clear double indirect block\n");
      return -1;
    }
    inode->double_indirect_offset = 0;
  }
  bitmap_set(inode_bitmap, dentry->inode_number, 0);
  clear_dentry(dentry);
  inode->file_size = 0;
  return 0;
}

int fs_read(int fildes, void *buf, size_t nbyte) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_read: file system not mounted\n");
    return -1;
  }
  struct file_descriptor *fd = &fds[fildes];
  if (fd->is_used == false) {
    fprintf(stderr, "fs_read: invalid file descriptor\n");
    return -1;
  }
  int start_block = get_data_block_num(fd->inode_number, fd->offset);
  if (start_block <= 0) {
    fprintf(stderr, "fs_read: no data block found for offset\n");
    return -1;
  }
  size_t bytes_read = read_bytes(start_block, fd, buf, nbyte);
  return bytes_read;
}

int fs_write(int fildes, void *buf, size_t nbyte) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_write: file system not mounted\n");
    return -1;
  }
  struct file_descriptor *fd = &fds[fildes];
  if (fd->is_used == false) {
    fprintf(stderr, "fs_read: invalid file descriptor\n");
    return -1;
  };
  int start_block = get_data_block_num(fd->inode_number, fd->offset);
  if (start_block == -1) {
    fprintf(stderr, "fs_write: failed to get data block number\n");
    return -1;
  }
  if (start_block == 0) {
    start_block = claim_unused_data_block();
    if (start_block <= 0) {
      fprintf(stderr, "fs_write: failed to get unused data block\n");
      return -1;
    }
    add_inode_data_block(fd->inode_number, start_block);
  }
  size_t bytes_written = write_bytes(start_block, fd, buf, nbyte);
  return bytes_written;
}

int fs_get_filesize(int fildes) {
  struct file_descriptor *fd = &fds[fildes];
  if (fd->is_used == false) {
    fprintf(stderr, "fs_get_filesize: invalid file descriptor\n");
    return -1;
  }
  return inode_table[fd->inode_number].file_size;
}

int fs_listfiles(char ***files) {
  // TODO: not passing the test
  char **file_name_ptr = *files;
  for (int i = 0; i < MAX_FILES; i++) {
    if (dir_table[i].is_used) {
      const char *file_name = dir_table[i].name;
      if (file_name == NULL) {
        fprintf(stderr, "fs_listfiles: invalid file name\n");
        return -1;
      }
      *file_name_ptr = strdup(file_name);
      file_name_ptr++;
    }
  }
  *file_name_ptr = NULL;
  return 0;
}

int fs_lseek(int fildes, off_t offset) {
  if (offset < 0) {
    fprintf(stderr, "fs_lseek: invalid offset\n");
    return -1;
  };
  struct file_descriptor *fd = &fds[fildes];
  if (fd->is_used == false) {
    fprintf(stderr, "fs_lseek: invalid file descriptor\n");
    return -1;
  }
  if (offset > inode_table[fd->inode_number].file_size) {
    fprintf(stderr, "fs_lseek: offset exceeds file size\n");
    return -1;
  }
  fd->offset = offset;
  return 0;
}

int fs_truncate(int fildes, off_t length) {
  if (is_mounted == false) {
    fprintf(stderr, "fs_truncate: file system not mounted\n");
    return -1;
  }
  struct file_descriptor *fd = &fds[fildes];
  if (fd->is_used == false) {
    fprintf(stderr, "fs_truncate: invalid file descriptor\n");
    return -1;
  }
  struct inode *inode = &inode_table[fd->inode_number];
  int file_size = inode->file_size;
  if (length < 0 || length > file_size) {
    fprintf(stderr, "fs_truncate: invalid length\n");
    return -1;
  }
  // free data block starting from length
  off_t offset = length;
  int cur_block_num;
  union fs_block block_buffer;
  while ((cur_block_num = get_data_block_num(fd->inode_number, offset)) > 0) {
    assert(cur_block_num >= sb.data_offset);
    uint16_t offset_in_block = offset % BLOCK_SIZE;
    if (block_read(cur_block_num, &block_buffer)) {
      fprintf(stderr, "fs_truncate: failed to read data block %d\n",
              cur_block_num);
      return -1;
    }
    memset(block_buffer.data + offset_in_block, 0,
           BLOCK_SIZE - offset_in_block);
    if (offset_in_block == 0) {
      bitmap_set(used_block_bitmap, cur_block_num, 0);
    }
    offset += BLOCK_SIZE - offset_in_block;
  }
  // walk inode table and free entries starting from length
  int block_idx = length / BLOCK_SIZE;
  off_t offset_in_block = length % BLOCK_SIZE;
  while (block_idx < DIRECT_OFFSETS_PER_INODE) {
    if (offset_in_block == 0) {
      inode->direct_offset[block_idx] = 0;
      bitmap_set(used_block_bitmap, inode->direct_offset[block_idx], 0);
    }
    block_idx++;
    offset_in_block = 0;
  }
  if (inode->single_indirect_offset) {
    if (clear_indirect_block(inode->single_indirect_offset,
                             SINGLE_INDIRECTION)) {
      fprintf(stderr, "fs_truncate: failed to clear single indirect block\n");
      return -1;
    }
    inode->single_indirect_offset = 0;
  }
  if (inode->double_indirect_offset) {
    if (clear_indirect_block(inode->double_indirect_offset,
                             DOUBLE_INDIRECTION)) {
      fprintf(stderr, "fs_truncate: failed to clear double indirect block\n");
      return -1;
    }
    inode->double_indirect_offset = 0;
  }
  fd->offset = MIN(fd->offset, length);
  inode->file_size = length;
  return 0;
}
