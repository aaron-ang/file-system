#include "../fs.h"
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>

int main() {
  const char *disk_name = "test_fs";
  assert(mount_fs(disk_name) == -1); // disk doesn't exist
  assert(make_fs(disk_name) == 0);
  assert(umount_fs(disk_name) == -1); // disk is not mounted
  assert(mount_fs(disk_name) == 0);
  assert(umount_fs(disk_name) == 0);

  FILE *fp;
  fp = fopen(disk_name, "r");
  assert(fp != NULL);

  bool non_zero_byte = false;
  char byte;
  while ((byte = fgetc(fp)) != EOF) {
    if (byte != 0) {
      non_zero_byte = true;
      break;
    }
  }
  assert(non_zero_byte == true);

  assert(fclose(fp) == 0);
  assert(unlink(disk_name) == 0);
}
