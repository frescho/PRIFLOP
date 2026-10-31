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

/* ram_flash.h: littlefs block device on a plain static RAM array --
   drive 9, see ramops.c. Mirrors lfs_flash.h's shape; see ram_flash.c
   for why the two implementations are not equally careful. */
#ifndef RAM_FLASH_H
#define RAM_FLASH_H

#include "lfs.h"

/* Ready-to-use configuration for the 128KB RAM disk. The returned
   pointer is to static storage and stays valid for the life of the
   program. */
const struct lfs_config *ram_flash_config(void);

#endif
