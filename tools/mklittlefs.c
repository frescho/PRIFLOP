/* mklittlefs.c: build a littlefs image for the Pico's internal flash.
 *
 * The littlefs counterpart of make_fat_image.py, and the reason that
 * script existed: recovering a corrupted volume has needed a host-built
 * image twice now, and the first time the builder had been lost.
 *
 * Built against the SAME vendored littlefs the firmware uses
 * (src/littlefs), with the SAME geometry as src/pico/lfs_flash.c. Those
 * two facts are load-bearing: littlefs writes its geometry into the
 * superblock, so an image built with different block/prog sizes will not
 * mount on the device. If lfs_flash.c's config changes, change it here
 * too -- the CONFIG MUST MATCH comment below marks the block.
 *
 *   cc -I../src/littlefs -o mklittlefs mklittlefs.c \
 *      ../src/littlefs/lfs.c ../src/littlefs/lfs_util.c
 *   ./mklittlefs [-m MB] out.img FILE...
 *
 * -m gives the board's flash size (default 4); the volume's block count
 * follows src/pico/flash_layout.h, which this includes.
 *
 * Then flash at XIP base + FLASH_ATA_OFFSET; ./make_fat_image.py --where
 * still prints that address (it is a property of the partition map, not
 * of the filesystem).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

#include "lfs.h"

#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)   /* the largest; -m picks */
#include "../src/pico/flash_layout.h"
#define BLOCKS_FOR_MB(mb)   (((mb) * 1024 * 1024 - FLASH_EEPROM_SIZE - FLASH_FIRMWARE_MAX) / FLASH_SECTOR_SIZE)

/* ---- CONFIG MUST MATCH src/pico/lfs_flash.c ------------------------ */
#define BLOCK_SIZE      4096            /* FLASH_SECTOR_SIZE            */
#define MAX_BLOCKS      FLASH_ATA_BLOCKS /* FLASH_ATA_BLOCKS, 16MB      */
#define READ_SIZE       1
#define PROG_SIZE       256             /* FLASH_PAGE_SIZE              */
#define CACHE_SIZE      256
#define LOOKAHEAD_SIZE  LFS_LOOKAHEAD_SIZE /* 16MB's; the same littlefs
                                           finds space either way      */
#define BLOCK_CYCLES    500
/* -------------------------------------------------------------------- */

static uint8_t image[(size_t)BLOCK_SIZE * MAX_BLOCKS];
static int block_count;

static int img_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t o,
                    void *buf, lfs_size_t n) {
  (void)c; memcpy(buf, image + (size_t)b * BLOCK_SIZE + o, n); return 0;
}
static int img_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t o,
                    const void *buf, lfs_size_t n) {
  (void)c; memcpy(image + (size_t)b * BLOCK_SIZE + o, buf, n); return 0;
}
static int img_erase(const struct lfs_config *c, lfs_block_t b) {
  (void)c; memset(image + (size_t)b * BLOCK_SIZE, 0xff, BLOCK_SIZE); return 0;
}
static int img_sync(const struct lfs_config *c) { (void)c; return 0; }

static uint8_t rbuf[CACHE_SIZE], pbuf[CACHE_SIZE];
static uint8_t lbuf[LOOKAHEAD_SIZE] __attribute__((aligned(8)));

int main(int argc, char **argv) {
  int mb = 4;
  if (argc > 2 && strcmp(argv[1], "-m") == 0) {
    mb = atoi(argv[2]);
    argv += 2; argc -= 2;
  }
  if (mb != 2 && mb != 4 && mb != 8 && mb != 16) {
    fprintf(stderr, "-m: 2, 4, 8 or 16\n"); return 1;
  }
  block_count = BLOCKS_FOR_MB(mb);
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s [-m MB] <out.img> [file ...]\n"
            "  Builds a %d KB littlefs image matching src/pico/lfs_flash.c.\n",
            argv[0], (BLOCK_SIZE * block_count) / 1024);
    return 1;
  }

  struct lfs_config cfg = {
    .read = img_read, .prog = img_prog, .erase = img_erase, .sync = img_sync,
    .read_size = READ_SIZE, .prog_size = PROG_SIZE,
    .block_size = BLOCK_SIZE, .block_count = block_count,
    .block_cycles = BLOCK_CYCLES,
    .cache_size = CACHE_SIZE, .lookahead_size = LOOKAHEAD_SIZE,
    .read_buffer = rbuf, .prog_buffer = pbuf, .lookahead_buffer = lbuf,
  };

  /* Erased flash is 0xff, and starting from anything else would produce an
     image that differs from what the device sees after a chip erase. */
  size_t image_size = (size_t)BLOCK_SIZE * block_count;
  memset(image, 0xff, image_size);

  lfs_t lfs;
  int err = lfs_format(&lfs, &cfg);
  if (err) { fprintf(stderr, "format failed: %d\n", err); return 1; }
  err = lfs_mount(&lfs, &cfg);
  if (err) { fprintf(stderr, "mount failed: %d\n", err); return 1; }

  for (int i = 2; i < argc; i++) {
    /* SRC=DEST stores SRC at DEST, making DEST's folders first (the
       volume has folders since PFM; measuring a library needs them).
       A plain SRC is stored under its basename, as before. */
    char *dup = strdup(argv[i]);
    char *eq = strchr(dup, '=');
    const char *src = dup, *name;
    if (eq) {
      *eq = 0;
      name = eq + 1;
      char dir[256];
      for (const char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(s - name), name);
        err = lfs_mkdir(&lfs, dir);
        if (err && err != LFS_ERR_EXIST) { fprintf(stderr, "mkdir %s failed: %d\n", dir, err); return 1; }
      }
    } else {
      name = basename(dup);
    }
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", src); return 1; }

    lfs_file_t f;
    err = lfs_file_open(&lfs, &f, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) { fprintf(stderr, "create %s failed: %d\n", name, err); return 1; }

    static uint8_t chunk[4096];
    size_t got, total = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0) {
      lfs_ssize_t w = lfs_file_write(&lfs, &f, chunk, (lfs_size_t)got);
      if (w < 0 || (size_t)w != got) {
        fprintf(stderr, "write %s failed: %d (image full?)\n", name, (int)w);
        return 1;
      }
      total += got;
    }
    lfs_file_close(&lfs, &f);
    fclose(in);
    printf("  added %-16s %zu bytes\n", name, total);
    free(dup);
  }

  lfs_ssize_t used = lfs_fs_size(&lfs);
  lfs_unmount(&lfs);

  FILE *out = fopen(argv[1], "wb");
  if (!out) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
  fwrite(image, 1, image_size, out);
  fclose(out);

  printf("wrote %s (%zu bytes), %d of %d blocks used\n",
         argv[1], image_size, (int)used, block_count);
  return 0;
}
