#include "../fs.h"
#include <assert.h>
#include <stdlib.h>

#define BYTES_KB 1024
#define BYTES_MB (1024 * BYTES_KB)
#define BYTES_30MB (30 * BYTES_MB)
#define BYTES_40MB (40 * BYTES_MB)
#define NUM_FILES 21

int main() {
  const char *disk_name = "test_fs";
  char write_buf0[BYTES_MB];
  char write_buf1[BYTES_MB];
  char *medium_buf;
  char *big_buf;
  char *read_buf;
  const char *file_names[NUM_FILES] = {
      "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10", "11",
      "12", "13", "14", "15", "16", "17", "18", "19", "20", "21",
  };
  int file_index = 0;
  int fds[NUM_FILES];

  medium_buf = malloc(BYTES_30MB);
  big_buf = malloc(BYTES_40MB);
  read_buf = malloc(BYTES_40MB);
  memset(write_buf0, 'a', sizeof(write_buf0));
  memset(write_buf1, 'b', sizeof(write_buf1));
  memset(medium_buf, 'c', BYTES_30MB);
  memset(big_buf, 'd', BYTES_40MB);

  remove(disk_name); // remove disk if it exists
  assert(make_fs(disk_name) == 0);
  assert(mount_fs(disk_name) == 0);

  // 9.3) Create 1 MiB file, write 1 MiB data --> check size
  assert(fs_create(file_names[file_index]) == 0);
  fds[file_index] = fs_open(file_names[file_index]);
  assert(fds[file_index] >= 0);
  assert(fs_write(fds[file_index], write_buf0, BYTES_MB) == BYTES_MB);
  assert(fs_get_filesize(fds[file_index]) == BYTES_MB);
  assert(fs_close(fds[file_index]) == 0);
  file_index++;

  // 9.4) Write {1 KiB, 4 KiB, 1 MiB} data to file
  assert(fs_create(file_names[file_index]) == 0);
  fds[file_index] = fs_open(file_names[file_index]);
  assert(fds[file_index] >= 0);
  assert(fs_write(fds[file_index], write_buf0, BYTES_KB) == BYTES_KB);
  assert(fs_lseek(fds[file_index], 0) == 0);
  assert(fs_write(fds[file_index], write_buf0, 4 * BYTES_KB) == 4 * BYTES_KB);
  assert(fs_lseek(fds[file_index], 0) == 0);
  assert(fs_write(fds[file_index], write_buf0, BYTES_MB) == BYTES_MB);
  assert(fs_close(fds[file_index]) == 0);
  file_index++;

  // 9.6) Write 1 MiB data to file / write bytes 500-600 / read whole file
  assert(fs_create(file_names[file_index]) == 0);
  fds[file_index] = fs_open(file_names[file_index]);
  assert(fds[file_index] >= 0);
  assert(fs_write(fds[file_index], write_buf0, BYTES_MB) == BYTES_MB);
  assert(fs_lseek(fds[file_index], 500) == 0);
  assert(fs_write(fds[file_index], write_buf1, 100) == 100);
  assert(fs_lseek(fds[file_index], 0) == 0);
  assert(fs_read(fds[file_index], read_buf, BYTES_MB) == BYTES_MB);
  assert(memcmp(read_buf, write_buf0, 500) == 0);       // first 500 bytes
  assert(memcmp(read_buf + 500, write_buf1, 100) == 0); // bytes 500-600
  assert(memcmp(read_buf + 600, write_buf0 + 600, BYTES_MB - 600) == 0); // rest
  assert(fs_close(fds[file_index]) == 0);
  file_index++;

  // 9.7) Write 16 files, 1 MiB each. Verify their contents
  for (int i = file_index; i < file_index + 16; i++) {
    assert(fs_create(file_names[i]) == 0);
    fds[i] = fs_open(file_names[i]);
    assert(fds[i] >= 0);
    if (i % 2 == 0) {
      assert(fs_write(fds[i], write_buf0, BYTES_MB) == BYTES_MB);
    } else {
      assert(fs_write(fds[i], write_buf1, BYTES_MB) == BYTES_MB);
    }
  }
  for (int i = 0; i < 16; i++) {
    assert(fs_lseek(fds[file_index], 0) == 0);
    memset(read_buf, 0, BYTES_40MB);
    assert(fs_read(fds[file_index], read_buf, BYTES_MB) == BYTES_MB);
    if (i % 2 == 0) {
      assert(memcmp(read_buf, write_buf0, BYTES_MB) == 0);
    } else {
      assert(memcmp(read_buf, write_buf1, BYTES_MB) == 0);
    }
    assert(fs_close(fds[file_index]) == 0);
    file_index++;
  }

  // 14.1) [EXTRA CREDIT] Write a 30 MiB file
  assert(fs_create(file_names[file_index]) == 0);
  fds[file_index] = fs_open(file_names[file_index]);
  assert(fds[file_index] >= 0);
  assert(fs_write(fds[file_index], medium_buf, BYTES_30MB) == BYTES_30MB);
  assert(fs_get_filesize(fds[file_index]) == BYTES_30MB);
  assert(fs_lseek(fds[file_index], 0) == 0);
  memset(read_buf, 0, BYTES_40MB);
  assert(fs_read(fds[file_index], read_buf, BYTES_30MB) == BYTES_30MB);
  assert(memcmp(read_buf, medium_buf, BYTES_30MB) == 0);
  assert(fs_close(fds[file_index]) == 0);
  file_index++;

  // 14.2) [EXTRA CREDIT] Write a 40 MiB file
  assert(fs_create(file_names[file_index]) == 0);
  fds[file_index] = fs_open(file_names[file_index]);
  assert(fds[file_index] >= 0);
  assert(fs_write(fds[file_index], big_buf, BYTES_40MB) == BYTES_40MB);
  assert(fs_get_filesize(fds[file_index]) == BYTES_40MB);
  assert(fs_lseek(fds[file_index], 0) == 0);
  memset(read_buf, 0, BYTES_40MB);
  assert(fs_read(fds[file_index], read_buf, BYTES_40MB) == BYTES_40MB);
  assert(memcmp(read_buf, big_buf, BYTES_40MB) == 0);
  assert(fs_close(fds[file_index]) == 0);
  file_index++;

  // Clean up
  assert(umount_fs(disk_name) == 0);
  assert(remove(disk_name) == 0);
  free(medium_buf);
  free(big_buf);
  free(read_buf);
}
