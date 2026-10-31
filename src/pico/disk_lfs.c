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

/* disk_lfs.c: the diskio block layer for a littlefs host volume.
 *
 * Replaces ata.c, which existed to make 4096-byte flash erase blocks look
 * like the 512-byte sectors FatFs insisted on. That translation needed a
 * read-modify-write cache with dirty slots and deferred commits, and it
 * was the direct cause of two corrupted volumes
 * and of the ~43ms interrupts-disabled commit that iec.c had to debounce
 * out of live C64 transactions.
 *
 * None of that applies now. littlefs addresses the flash in its own erase
 * blocks through lfs_flash.c, so there is no sector emulation, no cache,
 * and nothing to flush at idle. What remains is this: generic code still
 * calls the disk_* interface (doscmd.c's U1/U2 and M-R, main.c's
 * disk_init(), iec.c's disk_idle_sync()), so the interface has to exist
 * and answer honestly.
 *
 * The honest answers for a fixed, write-through, internal-flash volume:
 * the medium is always present and never changes, and it has no
 * C64-addressable sector geometry at all -- littlefs is a filesystem, not
 * an image. Direct block reads and writes therefore fail rather than
 * inventing a mapping, which is what lfsops.c's read/write_sector already
 * does one layer up.
 */

#include "config.h"
#include "diskio.h"
#include "lfsops.h"

/* Never DISK_CHANGED or DISK_REMOVED: the flash is soldered down. The
   hotplug paths in iec.c and fileops.c are therefore dead code here, but
   they cost nothing and keep the shared source identical. */
volatile enum diskstates disk_state = DISK_OK;

void disk_init(void) {
  /* lfs_flash.c owns the hardware and lfsops_init() does the mount. */
}

DSTATUS disk_initialize(uint8_t drv) {
  (void)drv;
  return 0;   /* no STA_NOINIT: memory-mapped flash needs no spin-up */
}

DSTATUS disk_status(uint8_t drv) {
  (void)drv;
  return 0;   /* present, initialised, writable */
}

/* No sector geometry to expose. A C64 program asking for one is asking
   about a disk that does not exist, and answering with the raw flash
   layout would let it scribble over the filesystem. */
DRESULT disk_read(uint8_t drv, uint8_t *buffer, uint32_t sector, uint8_t count) {
  (void)drv; (void)buffer; (void)sector; (void)count;
  return RES_ERROR;
}

DRESULT disk_write(uint8_t drv, const uint8_t *buffer, uint32_t sector, uint8_t count) {
  (void)drv; (void)buffer; (void)sector; (void)count;
  return RES_ERROR;
}

DRESULT disk_getinfo(uint8_t drv, uint8_t page, void *buffer) {
  (void)drv;
  if (page != 0)
    return RES_ERROR;

  diskinfo0_t *di = (diskinfo0_t *)buffer;
  di->validbytes  = sizeof(diskinfo0_t);
  di->maxpage     = 0;
  di->disktype    = DISK_TYPE_ATA;   /* "fixed, non-removable" */
  di->sectorsize  = 2;               /* 512 bytes, /256 */
  di->sectorcount = fs_total_blocks();
  return RES_OK;
}

/* Nothing is ever deferred. lfs_flash.c's prog() has committed to flash by
   the time it returns, so there is no write-back cache to drain here.
   ata.c's version did ~43ms of work with interrupts disabled, which is why
   iec.c debounces this call out of live transactions -- that debounce is
   now protecting a no-op, and is left in place because the contract
   (diskio.h) still allows an arch to defer. */
void disk_idle_sync(void) {
}
