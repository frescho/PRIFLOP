/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

   Inspired by MMC2IEC by Lars Pontoppidan et al.

   Historical credit: FAT filesystem access was based on code from ChaN
   and Jim Brain. No code of theirs remains -- storage is littlefs, which
   carries its own BSD-3-Clause licence in src/littlefs/LICENSE.md.

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


   lfsops.h: Definitions for the FAT operations

*/

#ifndef LFSOPS_H
#define LFSOPS_H

#include <stdbool.h>

#include "buffers.h"
#include "dirent.h"
#include "wrapops.h"

/* API */
void     lfsops_init(uint8_t preserve_dir);
void     parse_error(int res, uint8_t readflag);
uint8_t  lfsop_delete(path_t *path, cbmdirent_t *dent);
uint8_t  lfsop_chdir(path_t *path, cbmdirent_t *dent);
void     lfsop_mkdir(path_t *path, uint8_t *dirname);
void     lfsop_open_read(path_t *path, cbmdirent_t *filename, buffer_t *buf);
void     lfsop_open_write(path_t *path, cbmdirent_t *filename, uint8_t type, buffer_t *buf, uint8_t append);
uint8_t  lfsop_getdirlabel(path_t *path, uint8_t *label);
uint8_t  lfsop_getid(path_t *path, uint8_t *id);
uint16_t lfsop_freeblocks(uint8_t part);
uint8_t  lfsop_opendir(dh_t *dh, path_t *dir);
int8_t   lfsop_readdir(dh_t *dh, cbmdirent_t *dent);
void     lfsop_closedir(dh_t *dh);
void     lfsop_read_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector);
void     lfsop_write_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector);
void     format_dummy(uint8_t drive, uint8_t *name, uint8_t *id);

extern const fileops_t lfsops;
extern uint8_t file_extension_mode;

/* Generic helpers */
uint8_t image_unmount(uint8_t part);
/* Size of the mounted image, and whether it was opened for writing.
   d64ops.c used to read both straight out of a FatFs FIL. */
uint32_t image_size(uint8_t part);
/* Size of the whole host volume, in 512-byte blocks. */
uint32_t fs_total_blocks(void);
uint32_t fs_total_bytes(void);
uint32_t fs_free_bytes(void);
uint8_t  fs_reformatted(void);
uint8_t  image_writable(uint8_t part);
uint8_t image_chdir(path_t *path, cbmdirent_t *dent);
void    image_mkdir(path_t *path, uint8_t *dirname);
uint8_t image_read(uint8_t part, uint32_t offset, void *buffer, uint16_t bytes);
uint8_t image_write(uint8_t part, uint32_t offset, void *buffer, uint16_t bytes, uint8_t flush);

typedef enum { IMG_UNKNOWN, IMG_IS_DISK } imgtype_t;

imgtype_t check_imageext(uint8_t *name);

/* The single mounted volume. hostfs_lfs.c needs it too, and passing it
   around would mean threading an lfs_t through every generic call site
   for no benefit -- there is exactly one. */
lfs_t *lfsops_volume(void);

/* True if the given absolute path is an image a partition has mounted.
   Both delete paths consult this; see lfsops.c. */
bool lfsops_mounted_image(const char *fullpath);

/* Deferred filesystem housekeeping. Call ONLY from BUS_IDLE: this exists
   precisely so the work it does cannot land inside a C64 transaction. */
void lfsops_idle_maintenance(void);
/* Marks the cached BLOCKS FREE count stale. */
void lfsops_freeblocks_invalidate(void);

#endif
