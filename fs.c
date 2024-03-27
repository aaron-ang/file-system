#include "fs.h"
#include "disk.h"

#define MAX_FILES 64
#define MAX_FILE_SIZE 1024
#define MAX_FILE_NAME_CHAR 16
#define DIRECT_OFFSETS_PER_INODE 14
#define INODE_SIZE sizeof(struct inode)
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define MAX_FD 32

struct super_block {
  uint16_t inode_metadata_blocks;
  uint16_t inode_metadata_offset;
  uint16_t used_block_bitmap_count;
  uint16_t used_block_bitmap_offset;
};

enum file_type { REGULAR, DIRECTORY };

struct inode {
  enum file_type file_type;
  int direct_offset[DIRECT_OFFSETS_PER_INODE];
  int single_indirect_offset;
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
  uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
  uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
  struct inode inodes[BLOCK_SIZE / sizeof(struct inode)];
  char data[BLOCK_SIZE];
};

// on-disk and in-memory
struct super_block sb;
uint8_t inode_bitmap[MAX_FILES / CHAR_BIT];
uint8_t used_block_bitmap[DISK_BLOCKS / CHAR_BIT];
struct inode inodes[MAX_FILES];

// in-memory
bool is_mounted = false;
struct dir_entry dir[MAX_FILES];
struct file_descriptor fds[MAX_FD];

// helper functions

int get_file_dir_index(const char *name) {
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

// library functions

int make_fs(const char *disk_name) {
  if (make_disk(disk_name)) {
    fprintf(stderr, "make_fs: make_disk failed\n");
    return -1;
  }
  if (open_disk(disk_name)) {
    fprintf(stderr, "make_fs: open_disk failed\n");
    return -1;
  }

  sb.inode_metadata_blocks =
      (sizeof(inode_bitmap) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  sb.inode_metadata_offset = 1; // super block is at block 0
  sb.used_block_bitmap_count =
      (sizeof(used_block_bitmap) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  sb.used_block_bitmap_offset = 1 + sb.inode_metadata_blocks;

  if (block_write(0, &sb)) {
    fprintf(stderr, "make_fs: super block write failed\n");
    return -1;
  }

  // directory inode
  inodes[0] = (struct inode){
      .file_type = DIRECTORY,
      .direct_offset = 0,
      .single_indirect_offset = -1,
      .file_size = BLOCK_SIZE,
  };
  inode_bitmap[0] |= 1;
  used_block_bitmap[0] |= 1;

  int inode_blocks = (MAX_FILES + (INODES_PER_BLOCK - 1)) / INODES_PER_BLOCK;

  // root directory entry
  dir[0] = (struct dir_entry){
      .is_used = true,
      .inode_number = 0,
      .name = ".",
  };

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
  union fs_block block;
  if (block_read(0, &block.data)) {
    fprintf(stderr, "mount_fs: super block read failed\n");
    return -1;
  }

  struct super_block sb = block.super;
  if (sb.inode_metadata_offset == 0 || sb.used_block_bitmap_offset == 0) {
    fprintf(stderr, "mount_fs: file system not initialized\n");
    return -1;
  }
  sb = block.super;
  if (block_read(sb.inode_metadata_offset, &block.data)) {
    fprintf(stderr, "mount_fs: inode bitmap read failed\n");
    return -1;
  }
  memcpy(inode_bitmap, block.inode_bitmap, sizeof(inode_bitmap));
  if (block_read(sb.used_block_bitmap_offset, &block.data)) {
    fprintf(stderr, "mount_fs: used block bitmap read failed\n");
    return -1;
  }
  memcpy(used_block_bitmap, block.used_block_bitmap, sizeof(used_block_bitmap));

  is_mounted = true;
  return 0;
}

int umount_fs(const char *disk_name) {
  // TODO: write back all metadata and file data to disk

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
  int file_index = get_file_dir_index(name);
  if (file_index < 0) {
    fprintf(stderr, "fs_open: file not found\n");
    return -1;
  }
  for (int j = 0; j < MAX_FD; j++) {
    if (fds[j].is_used == false) {
      fds[j] = (struct file_descriptor){
          .is_used = true,
          .inode_number = dir[file_index].inode_number,
          .offset = 0,
      };
      return j;
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
  if (get_file_dir_index(name) >= 0) {
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
  return 0;
}
