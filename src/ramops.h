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


   ramops.h: Definitions for the RAM-disk operations (drive 9)

*/

#ifndef RAMOPS_H
#define RAMOPS_H

#include "buffers.h"
#include "dirent.h"
#include "wrapops.h"

/* Fixed partition slot for the RAM drive (lfsops_init() always claims
   slot 0 for flash). Not independently configurable -- see the design
   note for why device
   number and partition slot are both fixed rather than settable. */
#define RAM_PARTITION 1

/* Plain files only: OPEN/READ/WRITE/CLOSE and a directory listing.
   No D64/DNP image mounting (read_sector/write_sector/format are
   stubs), no REL files (open_rel is a stub), no subdirectories
   (mkdir/chdir are stubs) -- see ramops.c's header comment for the
   full reasoning. Every field is non-NULL, as fileops_t requires. */
extern const fileops_t ramops;

/* Formats (always -- RAM never has anything worth trying to preserve
   across a boot) and mounts the RAM volume into partition[RAM_PARTITION].
   Call once at boot, after lfsops_init() has claimed partition 0. */
void ramops_init(void);

/* False if ramops_init() never got a working volume -- checked by
   parser.c/fileops.c before honouring iec_ram_drive_active, so a failed
   RAM mount reports ERROR_DRIVE_NOT_READY instead of routing a
   transaction onto partition[RAM_PARTITION] with fop left NULL. */
uint8_t ramops_available(void);

/* The RAM volume itself, for callers outside the IEC path (fs_bridge.c)
   that need the raw lfs_t* -- same shape as lfsops_volume(). Check
   ramops_available() first; this returns a pointer regardless of
   whether ramops_init() actually got a working mount. */
lfs_t *ramops_volume(void);

/* Free/total bytes, for the web UI's RAM tab -- mirrors lfsops.h's
   fs_free_bytes()/fs_total_bytes(), computed fresh rather than cached
   (see ramops.c). */
uint32_t ramops_free_bytes(void);
uint32_t ramops_total_bytes(void);

#endif
