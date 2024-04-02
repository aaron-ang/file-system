#include "../fs.h"
#include <assert.h>
#include <stdlib.h>

#define BYTES_KB 1024
#define BYTES_MB (1024 * BYTES_KB)
#define BYTES_30MB (30 * BYTES_MB)
#define BYTES_40MB (40 * BYTES_MB)

int main() {
  const char *disk_name = "test_fs";
  const char *file_name = "test_file";
  char *medium_buf;
  char *big_buf;
  char *read_buf;
  int fd;

  medium_buf = malloc(BYTES_30MB);
  big_buf = malloc(BYTES_40MB);
  read_buf = malloc(BYTES_40MB);
  memset(medium_buf, 'a', BYTES_30MB);
  memset(big_buf, 'b', BYTES_40MB);

  remove(disk_name); // remove disk if it exists
  assert(make_fs(disk_name) == 0);
  assert(mount_fs(disk_name) == 0);

  // 14.1) [EXTRA CREDIT] Write a 30 MiB file
  assert(fs_create(file_name) == 0);
  fd = fs_open(file_name);
  assert(fd >= 0);
  assert(fs_write(fd, medium_buf, BYTES_30MB) == BYTES_30MB);
  assert(fs_get_filesize(fd) == BYTES_30MB);
  assert(fs_lseek(fd, 0) == 0);
  memset(read_buf, 0, BYTES_40MB);
  assert(fs_read(fd, read_buf, BYTES_30MB) == BYTES_30MB);
  assert(memcmp(read_buf, medium_buf, BYTES_30MB) == 0);
  assert(fs_close(fd) == 0);
  assert(fs_delete(file_name) == 0);

  // 14.2) [EXTRA CREDIT] Write a 40 MiB file
  assert(fs_create(file_name) == 0);
  fd = fs_open(file_name);
  assert(fd >= 0);
  assert(fs_write(fd, big_buf, BYTES_40MB) == BYTES_40MB);
  assert(fs_get_filesize(fd) == BYTES_40MB);
  assert(fs_lseek(fd, 0) == 0);
  memset(read_buf, 0, BYTES_40MB);
  assert(fs_read(fd, read_buf, BYTES_40MB) == BYTES_40MB);
  assert(memcmp(read_buf, big_buf, BYTES_40MB) == 0);
  assert(fs_close(fd) == 0);

  assert(umount_fs(disk_name) == 0);
  assert(remove(disk_name) == 0);
}
