#include "../fs.h"
#include <assert.h>
#include <unistd.h>

int main() {
  const char *disk_name = "test_fs";
  assert(make_fs(disk_name) == 0);
  assert(access(disk_name, F_OK) == 0);
  assert(unlink(disk_name) == 0);
}
