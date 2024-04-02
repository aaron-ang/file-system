#include "../fs.h"
#include <assert.h>

int main() {
  const char *disk_name = "test_fs";
  const char *file_name = "test_file";

  remove(disk_name); // remove disk if it exists
  assert(make_fs(disk_name) == 0);
  assert(mount_fs(disk_name) == 0);

  assert(fs_create(file_name) == 0);
  int fd = fs_open(file_name);
  assert(fd >= 0);
  assert(fs_delete(file_name) == -1); // file is open
  assert(fs_close(fd) == 0);
  assert(fs_delete(file_name) == 0);
  assert(fs_delete(file_name) == -1); // file does not exist

  assert(fs_create(file_name) == 0);
  assert(umount_fs(disk_name) == 0);
  assert(fs_delete(file_name) == -1); // disk is not mounted
  assert(remove(disk_name) == 0);
}
