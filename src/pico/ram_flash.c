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

/* ram_flash.c: 2026-09-10, drive 9 (see ramops.c). littlefs block device
   backed by a plain static RAM array instead of physical flash --
   compare lfs_flash.c, which this deliberately mirrors in shape.

   Everything that makes lfs_flash.c careful is a property of real flash
   hardware and does not apply here: there is no XIP window to stall, no
   ~39ms erase, no cross-core lockout to negotiate, nothing for the
   watchdog to miss. A "block erase" is a memset finishing in a few
   dozen cycles. So this file has no flash_shield, no flash_lockout, no
   system_watchdog_pet() call, and no IRAM_ATTR -- there is no flash
   fetch here to be slow on a cold cache line, and nothing here runs
   from an ISR. If any of that ever turns out to be wrong, the
   corresponding lfs_flash.c logic is exactly what to port back in.

   Volatile by construction: the array is zero-initialised RAM, not
   backed by anything, so it holds whatever was there since power-on and
   is gone on every reset. ramops_init() always formats fresh rather
   than attempting a mount first -- there is never a valid filesystem to
   recover here, unlike lfsops_init()'s flash path. */

#include <string.h>

#include "ram_flash.h"

#define RAM_DISK_SIZE   (128 * 1024)
#define RAM_BLOCK_SIZE  512
#define RAM_BLOCK_COUNT (RAM_DISK_SIZE / RAM_BLOCK_SIZE)

static uint8_t ram_disk[RAM_DISK_SIZE];

static int ram_flash_read(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, void *buffer, lfs_size_t size) {
  (void)c;
  memcpy(buffer, ram_disk + (size_t)block * RAM_BLOCK_SIZE + off, size);
  return LFS_ERR_OK;
}

static int ram_flash_prog(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, const void *buffer, lfs_size_t size) {
  (void)c;
  memcpy(ram_disk + (size_t)block * RAM_BLOCK_SIZE + off, buffer, size);
  return LFS_ERR_OK;
}

static int ram_flash_erase(const struct lfs_config *c, lfs_block_t block) {
  (void)c;
  /* 0xff, matching real NOR flash's erased state -- littlefs does not
     require this (it tracks used/free itself), but keeping it means a
     block dump reads the same way a flash one would if anyone ever
     compares the two by eye. */
  memset(ram_disk + (size_t)block * RAM_BLOCK_SIZE, 0xff, RAM_BLOCK_SIZE);
  return LFS_ERR_OK;
}

static int ram_flash_sync(const struct lfs_config *c) {
  (void)c;
  return LFS_ERR_OK;
}

/* Byte-granular in both directions -- there is no physical page/sector
   constraint to respect, unlike lfs_flash.c's FLASH_PAGE_SIZE prog
   requirement. cache_size(16) must be a multiple of read_size(1) and
   prog_size(16), and a factor of block_size(512): all satisfied.
   lookahead_size must be a multiple of 8 and cover block_count/8 bits
   in one pass: 256 blocks / 8 = 32 bytes. block_cycles -1 disables
   littlefs's wear-levelling relocation entirely -- wear is not a
   concept that applies to RAM, and every byte of it resets to garbage
   on the next boot regardless. */
#define RAM_CACHE_SIZE     16
#define RAM_LOOKAHEAD_SIZE 32

static uint8_t read_buffer[RAM_CACHE_SIZE];
static uint8_t prog_buffer[RAM_CACHE_SIZE];
static uint8_t lookahead_buffer[RAM_LOOKAHEAD_SIZE] __attribute__((aligned(8)));

static const struct lfs_config cfg = {
  .read  = ram_flash_read,
  .prog  = ram_flash_prog,
  .erase = ram_flash_erase,
  .sync  = ram_flash_sync,

  .read_size   = 1,
  .prog_size   = RAM_CACHE_SIZE,
  .block_size  = RAM_BLOCK_SIZE,
  .block_count = RAM_BLOCK_COUNT,
  .block_cycles = -1,

  .cache_size     = RAM_CACHE_SIZE,
  .lookahead_size = RAM_LOOKAHEAD_SIZE,

  .read_buffer      = read_buffer,
  .prog_buffer      = prog_buffer,
  .lookahead_buffer = lookahead_buffer,
};

const struct lfs_config *ram_flash_config(void) {
  return &cfg;
}
