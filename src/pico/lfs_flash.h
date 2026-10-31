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

/* lfs_flash.h: littlefs block device on the RP2350's internal flash.

   Replaces ata.c's role for the littlefs build. The two are very
   different shapes, and that difference is the point:

   ata.c had to present 512-byte sectors to FatFs over 4096-byte flash
   erase blocks, which forced a read-modify-write cache with dirty slots,
   deferred commits, and a debounce in iec.c's BUS_IDLE to keep those
   commits out of live C64 transactions. That machinery is the direct
   cause of the torn multi-sector updates that corrupted the volume twice.

   littlefs addresses the flash in its own erase blocks, so none of that
   translation exists here: read / prog / erase map one-to-one onto the
   hardware. What remains is the one thing that genuinely must be handled
   -- keeping core 1 off the flash bus during an erase or program, via
   flash_lockout.h.
*/
#ifndef LFS_FLASH_H
#define LFS_FLASH_H

#include "lfs.h"

/* Ready-to-use configuration for the FLASH_ATA_OFFSET/SIZE region
   (arch-config.h). The returned pointer is to static storage and stays
   valid for the life of the program. */
const struct lfs_config *lfs_flash_config(void);

#endif
