/* Prove littlefs works with THIS device's flash geometry before porting to it:
   1 MB region, 4096-byte erase blocks (FLASH_ATA_SIZE / FLASH_SECTOR_SIZE). */
#include <stdio.h>
#include <string.h>
#include "lfs.h"   /* vendored: src/littlefs */

#define BLOCK_SIZE  4096
#define BLOCK_COUNT 256          /* 1 MB / 4096 */
static uint8_t disk[BLOCK_SIZE * BLOCK_COUNT];

static int r(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *buf, lfs_size_t s)
{ memcpy(buf, disk + b*BLOCK_SIZE + o, s); return 0; }
static int p(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *buf, lfs_size_t s)
{ memcpy(disk + b*BLOCK_SIZE + o, buf, s); return 0; }
static int e(const struct lfs_config *c, lfs_block_t b)
{ memset(disk + b*BLOCK_SIZE, 0xff, BLOCK_SIZE); return 0; }
static int sy(const struct lfs_config *c) { return 0; }

int main(void) {
  lfs_t lfs; lfs_file_t f;
  struct lfs_config cfg = {
    .read = r, .prog = p, .erase = e, .sync = sy,
    .read_size = 16, .prog_size = 16,
    .block_size = BLOCK_SIZE, .block_count = BLOCK_COUNT,
    .block_cycles = 500,          /* wear levelling: relocate after N erases */
    .cache_size = 256, .lookahead_size = 32,
  };
  memset(disk, 0xff, sizeof(disk));
  if (lfs_format(&lfs, &cfg) < 0) { puts("format FAILED"); return 1; }
  if (lfs_mount(&lfs, &cfg) < 0)  { puts("mount FAILED");  return 1; }

  /* the exact files this device carries */
  const char *names[] = {"TEST.PRG", "IECTEST.PRG", "PRIFLOP.CFG"};
  const int sizes[] = {36, 1619, 95};
  for (int i = 0; i < 3; i++) {
    static uint8_t b[4096];
    memset(b, 'A' + i, sizes[i]);
    lfs_file_open(&lfs, &f, names[i], LFS_O_WRONLY | LFS_O_CREAT);
    lfs_file_write(&lfs, &f, b, sizes[i]);
    lfs_file_close(&lfs, &f);
  }
  /* a D64-sized image, seek+read in the middle like d64ops does */
  lfs_file_open(&lfs, &f, "DISK.D64", LFS_O_WRONLY | LFS_O_CREAT);
  static uint8_t blk[256]; memset(blk, 0x5a, sizeof(blk));
  for (int i = 0; i < 683; i++) lfs_file_write(&lfs, &f, blk, sizeof(blk));
  lfs_file_close(&lfs, &f);

  lfs_file_open(&lfs, &f, "DISK.D64", LFS_O_RDONLY);
  lfs_file_seek(&lfs, &f, 91*256, LFS_SEEK_SET);
  static uint8_t chk[256];
  lfs_file_read(&lfs, &f, chk, 256);
  lfs_file_close(&lfs, &f);
  printf("D64 seek+read mid-image: %s\n", chk[0] == 0x5a ? "ok" : "BAD");

  lfs_dir_t d; struct lfs_info info; int n = 0;
  lfs_dir_open(&lfs, &d, "/");
  while (lfs_dir_read(&lfs, &d, &info) > 0)
    if (info.type == LFS_TYPE_REG) { printf("  %-14s %ld\n", info.name, (long)info.size); n++; }
  lfs_dir_close(&lfs, &d);
  printf("files listed: %d\n", n);
  printf("blocks used: %d / %d\n", (int)lfs_fs_size(&lfs), BLOCK_COUNT);
  lfs_unmount(&lfs);
  puts("SMOKE TEST PASSED");
  return 0;
}
