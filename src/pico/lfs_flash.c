/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>
   Copyright (C) 2026  sd2iec_pico2w contributors

   RP2350 / Raspberry Pi Pico 2 W port by frescho.
   This file was written for that port.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

/* lfs_flash.c: see lfs_flash.h for why this replaces ata.c's cache. */

#include <string.h>

#include "config.h"
#include "flash_lockout.h"
#include "lfs_flash.h"

/* Declared here rather than via system.h: that header also declares
   disable_interrupts()/enable_interrupts(), which clash with a Pico SDK
   declaration pulled in above. See system.c. */
void flash_shield_begin(void);
void flash_shield_end(void);
void system_watchdog_pet(void);

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/stdlib.h"
#include "iec.h"
#include "uart.h"
void primo_turbo_deep(uint32_t step);   /* src/pico/primo_turbo.h */

/* Longest single flash operation, in microseconds. This used to be the
   longest stretch with interrupts DISABLED, which was the bug: at ~38ms
   it was 38 times the 1000us limit for answering ATN (T_AT), so the C64 reported DEVICE NOT PRESENT. The erase still takes
   just as long -- that is the hardware -- but interrupts now stay on
   through it and ATN is still answered. Reported by /api/stats. */
volatile uint32_t flash_op_max_us;
volatile uint32_t flash_ops;

/* Reads come straight out of the memory-mapped XIP window -- no driver
   call, no buffering. littlefs asks for arbitrary offsets and lengths,
   which is exactly what a memory mapping gives for free. */
static const uint8_t *flash_base(void) {
  return (const uint8_t *)(XIP_BASE + FLASH_ATA_OFFSET);
}

static int lfs_flash_read(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, void *buffer, lfs_size_t size) {
  (void)c;
  memcpy(buffer, flash_base() + (size_t)block * FLASH_SECTOR_SIZE + off, size);
  return LFS_ERR_OK;
}

static int lfs_flash_prog(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, const void *buffer, lfs_size_t size) {
  (void)c;
  uint32_t addr = FLASH_ATA_OFFSET + block * FLASH_SECTOR_SIZE + off;
  primo_turbo_deep(30);

  /* Core 1 runs lwIP/httpd out of this same flash and dies on its first
     cache miss while XIP is unavailable. See flash_lockout.h -- this is
     the failure that took a whole session to find. Deferring (rather than
     programming anyway) on a failed lockout is deliberate: littlefs
     treats the error as a write failure and retries, which is safe,
     whereas programming under a live core 1 is not. */
  if (!flash_lockout_begin()) {
    uart_puts_P(PSTR("\r\nlfs: flash lockout timed out, write deferred\r\n"));
    return LFS_ERR_IO;
  }
  /* Interrupts stay ON: the shield masks every source whose handler
     lives in flash and leaves ATN answerable. See flash_shield_begin(). */
  uint32_t t0 = time_us_32();
  primo_turbo_deep(31);
  flash_shield_begin();
  flash_range_program(addr, (const uint8_t *)buffer, size);
  /* REQUIRED on RP2350: without it a later memory-mapped read of this
     range can be served stale pre-write data from the XIP cache. See
     ata.c's identical note. */
  xip_cache_invalidate_range(addr, size);
  flash_shield_end();
  system_watchdog_pet();
  { uint32_t d = time_us_32() - t0;
    if (d > flash_op_max_us) flash_op_max_us = d;
    flash_ops++; }
  primo_turbo_deep(32);
  flash_lockout_end();
  primo_turbo_deep(33);

  return LFS_ERR_OK;
}

static int lfs_flash_erase(const struct lfs_config *c, lfs_block_t block) {
  (void)c;
  uint32_t addr = FLASH_ATA_OFFSET + block * FLASH_SECTOR_SIZE;

  primo_turbo_deep(40);
  if (!flash_lockout_begin()) {
    uart_puts_P(PSTR("\r\nlfs: flash lockout timed out, write deferred\r\n"));
    return LFS_ERR_IO;
  }
  /* Interrupts stay ON: the shield masks every source whose handler
     lives in flash and leaves ATN answerable. See flash_shield_begin(). */
  uint32_t t0 = time_us_32();
  primo_turbo_deep(41);
  flash_shield_begin();
  flash_range_erase(addr, FLASH_SECTOR_SIZE);
  xip_cache_invalidate_range(addr, FLASH_SECTOR_SIZE);
  flash_shield_end();
  system_watchdog_pet();
  { uint32_t d = time_us_32() - t0;
    if (d > flash_op_max_us) flash_op_max_us = d;
    flash_ops++; }
  primo_turbo_deep(42);
  flash_lockout_end();
  primo_turbo_deep(43);

  return LFS_ERR_OK;
}

static int lfs_flash_sync(const struct lfs_config *c) {
  (void)c;
  /* Nothing buffered below littlefs: prog() has already committed to
     flash by the time it returns. */
  return LFS_ERR_OK;
}

/* Static buffers rather than malloc: littlefs will happily allocate, but
   this firmware has no heap pressure budget and a failed allocation deep
   inside a filesystem call is a poor failure mode. Sizes must satisfy
   littlefs's rules -- cache_size a multiple of both read_size and
   prog_size and a factor of block_size; lookahead_size a multiple of 8. */
#define LFS_CACHE_SIZE     FLASH_PAGE_SIZE   /* 256 */
/* LFS_LOOKAHEAD_SIZE: flash_layout.h, one bit per block of the volume. */

static uint8_t read_buffer[LFS_CACHE_SIZE];
static uint8_t prog_buffer[LFS_CACHE_SIZE];
static uint8_t lookahead_buffer[LFS_LOOKAHEAD_SIZE] __attribute__((aligned(8)));

static const struct lfs_config cfg = {
  .read  = lfs_flash_read,
  .prog  = lfs_flash_prog,
  .erase = lfs_flash_erase,
  .sync  = lfs_flash_sync,

  /* Reads are memory-mapped, so any granularity is fine; programs must be
     whole flash pages, which is what FLASH_PAGE_SIZE is. */
  .read_size   = 1,
  .prog_size   = FLASH_PAGE_SIZE,
  .block_size  = FLASH_SECTOR_SIZE,
  .block_count = FLASH_ATA_BLOCKS,

  /* The whole reason for this migration: after this many erases littlefs
     relocates a block's contents elsewhere, so the metadata that FAT
     pinned to fixed offsets no longer wears out ~1.2% of the region while
     the rest stays pristine. 500 is
     upstream's recommended starting point; lower trades more copying for
     more even wear. */
  .block_cycles = 500,

  .cache_size     = LFS_CACHE_SIZE,
  .lookahead_size = LFS_LOOKAHEAD_SIZE,

  .read_buffer      = read_buffer,
  .prog_buffer      = prog_buffer,
  .lookahead_buffer = lookahead_buffer,
};

const struct lfs_config *lfs_flash_config(void) {
  return &cfg;
}
