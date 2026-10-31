/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

   RP2350 / Raspberry Pi Pico 2 W port by frescho.

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

/* hostfs.h: plain file access on the *host* filesystem.
 *
 * WHY THIS EXISTS. sd2iec has a filesystem abstraction already --
 * fileops_t (wrapops.h), implemented by lfsops/d64ops/eefs-ops --
 * but it speaks CBM semantics: CBM names, buffers, disk labels, track and
 * sector. A few pieces of code do not want any of that. They want to open
 * an ordinary file by name and read or write bytes:
 *
 *   - diskchange.c   builds and reads a swaplist (a text file of image names)
 *   - doscmd.c       loads a ROM file
 *
 * Those three call FatFs directly today -- f_open(), f_readdir(),
 * l_opendir() and a FIL on the stack -- which is the only reason generic
 * code depends on FatFs at all. Everything else is already behind
 * fileops_t. This header is that dependency, named and made swappable.
 *
 * DIRECTORY IDENTITY. Functions take `const dir_t *`, never a path
 * string, and never look inside it. That matters: dir_t is a union whose
 * meaning belongs to the backend (a FAT cluster today, a path for
 * littlefs), and it is precisely the leak of that representation into
 * generic code that made replacing FatFs look expensive.
 * Passing it opaquely keeps the choice with
 * the implementation.
 *
 * There is deliberately no vtable here. Unlike fileops_t, only one host
 * filesystem exists at a time -- the volume is either FAT or littlefs, not
 * both -- so a single implementation selected at build time is honest
 * about that and cheaper than indirection nobody uses.
 */
#ifndef HOSTFS_H
#define HOSTFS_H

#include <stdint.h>
#include "dirent.h"

/* Open modes for hostfs_open(). */
#define HOSTFS_READ    0x01
#define HOSTFS_WRITE   0x02
#define HOSTFS_CREATE  0x04   /* create, truncating any existing file */

/* Opaque handles. Sized by the backing implementation; callers only ever
   hold them and pass their address. */
typedef struct hostfile_s hostfile_t;
typedef struct hostdir_s  hostdir_t;

#include "hostfs_impl.h"      /* defines the two structs above */

/* One directory entry. `name` is the host filesystem's own name, not a
   CBM-mangled one -- the swaplist stores host names so it can be
   hand-edited on a PC. */
typedef struct {
  uint8_t  name[HOSTFS_NAME_MAX];
  uint32_t size;
  uint8_t  is_dir;
} hostinfo_t;

/* All functions return 0 on success, nonzero on failure. */

uint8_t hostfs_open(hostfile_t *f, uint8_t part, const dir_t *dir,
                    const uint8_t *name, uint8_t mode);
uint8_t hostfs_read(hostfile_t *f, void *buf, uint16_t len, uint16_t *got);
uint8_t hostfs_write(hostfile_t *f, const void *buf, uint16_t len, uint16_t *put);
uint8_t hostfs_seek(hostfile_t *f, uint32_t pos);
/* Size of an open file. The ROM loader needs it to locate the image's
   payload past any header. */
uint32_t hostfs_size(hostfile_t *f);
void    hostfs_close(hostfile_t *f);

uint8_t hostfs_opendir(hostdir_t *d, uint8_t part, const dir_t *dir);
/* Returns 0 and sets info->name[0] == 0 at end of directory. */
uint8_t hostfs_readdir(hostdir_t *d, hostinfo_t *info);
/* MUST be called when a scan finishes, including an early exit: littlefs
   keeps open directories in an internal list and an abandoned handle
   leaves it pointing at dead memory. The FatFs implementation makes this
   a no-op. */
void    hostfs_closedir(hostdir_t *d);

#endif
