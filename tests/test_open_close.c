#include "../fs.h"
#include <assert.h>

int main() {
  const char *disk_name = "test_fs";
  const char *file_name = "test_file";

  remove(disk_name); // remove disk if it exists
  assert(make_fs(disk_name) == 0);
  assert(mount_fs(disk_name) == 0);
  assert(fs_open(file_name) == -1); // file does not exist
  assert(fs_create(file_name) == 0);

  int max_fd = 32;
  int fds[max_fd];
  for (int i = 0; i < max_fd; i++) {
    fds[i] = fs_open(file_name);
    assert(fds[i] >= 0);
  }
  assert(fs_open(file_name) == -1); // fd limit reached
  for (int i = 0; i < max_fd; i++) {
    assert(fs_close(fds[i]) == 0);
  }
  assert(fs_close(fds[0]) == -1); // fd not in use
  assert(umount_fs(disk_name) == 0);
  assert(fs_open(file_name) == -1); // disk not mounted
  assert(remove(disk_name) == 0);
}
