#include "../fs.h"
#include <assert.h>
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

  int non_zero_bytes = 0;
  char byte;
  while ((byte = fgetc(fp)) != EOF) {
    if (byte != 0) {
      non_zero_bytes++;
    }
  }
  assert(non_zero_bytes > 0);

  assert(fclose(fp) == 0);
  assert(unlink(disk_name) == 0);
}
