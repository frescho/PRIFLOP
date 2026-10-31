/* lfsops_name_test.c: host-side tests for the names R: and @: use in
   src/lfsops.c, on the real littlefs over a RAM block device.

   A file's real name can hold capitals (an upload over HTTP keeps them; the
   Primo's own names arrive as ASCII capitals). The listing shows it in
   capitals either way, and a name sent over IEC goes through pet2asc, so
   capitals become small letters. Two bugs came out of that (open step 9): R: built its source path from the shown name and missed a
   file whose real name has capitals, and @: recreated a file under the
   shown name, in small letters and without a hidden .PRG.

     gcc -I tests/lfsops_host/shim -I obj-pico2w-config -I src/pico -I src \
         -I src/littlefs -o lfsnametest tests/lfsops_name_test.c src/lfsops.c \
         src/littlefs/lfs.c src/littlefs/lfs_util.c && ./lfsnametest
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "buffers.h"
#include "d64ops.h"
#include "errormsg.h"
#include "fileops.h"
#include "flags.h"
#include "led.h"
#include "parser.h"
#include "lfs.h"
#include "lfs_flash.h"
#include "lfsops.h"

/* --- what lfsops.c links against, as stubs --- */
uint8_t ops_scratch[33];
partition_t partition[CONFIG_MAX_PARTITIONS];
uint8_t current_part, max_part, globalflags;
volatile uint8_t led_state;
cbmdirent_t previous_file_dirent;
const PROGMEM uint8_t filetypes[] = { 'D','E','L', 'S','E','Q', 'P','R','G', 'U','S','R', 'R','E','L', 'C','B','M', 'D','I','R' };
const fileops_t d64ops;
uint8_t lfsread_prepost_scratch_note;
static uint8_t last_error;
void set_error(uint8_t e) { last_error = e; }
void set_error_ts(uint8_t e, uint8_t t, uint8_t s) { (void)t; (void)s; last_error = e; }
uint8_t callback_dummy(buffer_t *b) { (void)b; return 0; }
void d64_invalidate(void) {}
uint8_t d64_mount(path_t *p, uint8_t *n) { (void)p; (void)n; return 1; }
void d64_unmount(uint8_t p) { (void)p; }
void free_buffer(buffer_t *b) { (void)b; }
uint8_t free_multiple_buffers(uint8_t f) { (void)f; return 0; }
void mark_buffer_clean(buffer_t *b) { (void)b; }
void set_changelist(path_t *p, uint8_t *n) { (void)p; (void)n; }
void primo_turbo_deep(uint32_t s) { (void)s; }
void system_watchdog_pet(void) {}
uint32_t time_us_32(void) { return 0; }
void uart_puts_P(const char *s) { (void)s; }
void update_leds(void) {}

/* --- a RAM block device, the drive's geometry in small --- */
#define BS 4096
#define BC 64
static uint8_t ram[BS * BC];
static int rd(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *buf, lfs_size_t n) { (void)c; memcpy(buf, ram + b * BS + o, n); return 0; }
static int pg(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *buf, lfs_size_t n) { (void)c; memcpy(ram + b * BS + o, buf, n); return 0; }
static int er(const struct lfs_config *c, lfs_block_t b) { (void)c; memset(ram + b * BS, 0xFF, BS); return 0; }
static int sy(const struct lfs_config *c) { (void)c; return 0; }
static const struct lfs_config cfg = {
  .read = rd, .prog = pg, .erase = er, .sync = sy,
  .read_size = 1, .prog_size = 256, .block_size = BS, .block_count = BC,
  .cache_size = 256, .lookahead_size = 16, .block_cycles = 500,
};
const struct lfs_config *lfs_flash_config(void) { return &cfg; }

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } else printf("ok   %s\n", msg); } while (0)

static bool exists(const char *name) {
  struct lfs_info i;
  char p[64];
  snprintf(p, sizeof p, "/%s", name);
  return lfs_stat(lfsops_volume(), p, &i) >= 0 && strcmp(i.name, name) == 0;
}

static void put(const char *name) {
  lfs_file_t f;
  char p[64];
  snprintf(p, sizeof p, "/%s", name);
  lfs_file_open(lfsops_volume(), &f, p, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
  lfs_file_write(lfsops_volume(), &f, "x", 1);
  lfs_file_close(lfsops_volume(), &f);
}

/* the dent a directory match fills for a plain file: shown in capitals,
   the real name kept beside it */
static cbmdirent_t dent_for(const char *shown, const char *real) {
  cbmdirent_t d;
  memset(&d, 0, sizeof d);
  strcpy((char *)d.name, shown);
  strcpy((char *)d.pvt.host.realname, real);
  d.opstype = OPSTYPE_HOST;
  d.typeflags = TYPE_PRG;
  return d;
}

static void fresh(void) {
  memset(ram, 0xFF, sizeof ram);
  lfs_format(lfsops_volume(), &cfg);
  lfs_mount(lfsops_volume(), &cfg);
}

/* @: as fileops.c does it: delete the match, then open for writing with it */
static void replace(cbmdirent_t *d) {
  static buffer_t buf;
  path_t path;
  memset(&path, 0, sizeof path);
  cbmdirent_t copy = *d;
  lfsop_delete(&path, &copy);
  memset(&buf, 0, sizeof buf);
  buf.data = malloc(256);
  lfsop_open_write(&path, d, TYPE_PRG, &buf, 0);
  lfs_file_close(lfsops_volume(), &buf.pvt.lfs.fh);
  free(buf.data);
}

int main(void) {
  path_t path;
  memset(&path, 0, sizeof path);

  /* R: on a file whose real name has capitals; the new name arrives
     through pet2asc as the Primo sends it, ASCII capitals */
  fresh();
  put("PT0");
  cbmdirent_t d = dent_for("PT0", "PT0");
  uint8_t newname[17] = "NEW";
  last_error = 0;
  lfsop_rename(&path, &d, newname);
  CHECK(!exists("PT0"), "R: renames a file whose real name has capitals");
  CHECK(exists("new"), "R: the new name as sd2iec stores a typed name (small letters)");

  /* R: on a small-letter file, as before: unchanged */
  fresh();
  put("old");
  d = dent_for("OLD", "old");
  strcpy((char *)newname, "NEW");
  lfsop_rename(&path, &d, newname);
  CHECK(!exists("old") && exists("new"), "R: on a small-letter name still works");

  /* @: keeps the real name: capitals stay capitals */
  fresh();
  put("PT0");
  d = dent_for("PT0", "PT0");
  replace(&d);
  CHECK(exists("PT0") && !exists("pt0"), "@: keeps a real name in capitals");

  /* @: keeps a hidden .PRG */
  fresh();
  put("HELLO.PRG");
  d = dent_for("HELLO", "HELLO.PRG");
  replace(&d);
  CHECK(exists("HELLO.PRG") && !exists("hello"), "@: keeps a hidden .PRG extension");

  /* a new file (no match: no real name) is named as before */
  fresh();
  d = dent_for("NEWFILE", "");
  {
    static buffer_t buf;
    memset(&buf, 0, sizeof buf);
    buf.data = malloc(256);
    lfsop_open_write(&path, &d, TYPE_PRG, &buf, 0);
    lfs_file_close(lfsops_volume(), &buf.pvt.lfs.fh);
    free(buf.data);
  }
  CHECK(exists("newfile"), "a new file is still named through pet2asc (small letters)");

  printf(fails ? "lfsops_name_test: %d FAILED\n" : "lfsops_name_test: all passed\n", fails);
  return fails != 0;
}
