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

/* hostfs_impl.h: backing types for hostfs.h, littlefs version.
 *
 * Split out so hostfs.h can declare opaque handles without dragging the
 * filesystem's headers into every translation unit that merely calls it.
 * The FatFs version of this file is in git history alongside
 * hostfs_fat.c.
 */
#ifndef HOSTFS_IMPL_H
#define HOSTFS_IMPL_H

#include "lfs.h"

/* Must not exceed LFS_REALNAME_MAX (dirent.h), which is what the volume's
   superblock records as name_max. */
#define HOSTFS_NAME_MAX 33

struct hostfile_s {
  lfs_file_t             fh;
  /* See LFS_FILE_CACHE_SIZE in dirent.h: LFS_NO_MALLOC means every open
     file supplies its own cache. */
  uint8_t                cache[256];
  struct lfs_file_config cfg;
};
struct hostdir_s  { lfs_dir_t  dh; uint8_t part; };

#endif
