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

/* hostfs_lfs.c: hostfs.h backed by littlefs. See hostfs.h for why the
   abstraction exists, and hostfs_fat.c for the FatFs version this
   replaces.

   Simpler than the FatFs one in the way that matters: FatFs resolved a
   relative name against a per-volume "current directory" that had to be
   set first, so every open was two coupled operations. littlefs takes an
   absolute path, so dir_t (a path here) and the name just get joined.
*/
#include <string.h>

#include "config.h"
#include "dirent.h"
#include "hostfs.h"
#include "lfs.h"
#include "lfsops.h"
#include "ustring.h"

/* Same join rule as lfsops.c's lfs_path_join(); kept local rather than
   shared because the two files are the only users and a header for one
   small string operation is not worth the coupling. */
static void join(char *out, size_t outlen, const char *dir, const uint8_t *name) {
  size_t n = 0;

  if (dir && dir[0] && !(dir[0] == '/' && dir[1] == 0)) {
    while (dir[n] && n < outlen - 2) { out[n] = dir[n]; n++; }
  }
  if (n == 0 || out[n-1] != '/') {
    if (n < outlen - 1) out[n++] = '/';
  }
  while (name && *name && n < outlen - 1) {
    out[n++] = (char)*name++;
  }
  out[n] = 0;
}

uint8_t hostfs_open(hostfile_t *f, uint8_t part, const dir_t *dir,
                    const uint8_t *name, uint8_t mode) {
  char path[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
  int flags;

  (void)part;   /* one volume: see lfsops.c */

  if (mode & HOSTFS_CREATE)     flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
  else if (mode & HOSTFS_WRITE) flags = LFS_O_WRONLY;
  else                          flags = LFS_O_RDONLY;

  join(path, sizeof(path), dir->lfs.path, name);
  memset(&f->cfg, 0, sizeof(f->cfg));
  f->cfg.buffer = f->cache;
  return lfs_file_opencfg(lfsops_volume(), &f->fh, path, flags, &f->cfg) < 0;
}

uint8_t hostfs_read(hostfile_t *f, void *buf, uint16_t len, uint16_t *got) {
  lfs_ssize_t n = lfs_file_read(lfsops_volume(), &f->fh, buf, len);
  if (got) *got = (n < 0) ? 0 : (uint16_t)n;
  return n < 0;
}

uint8_t hostfs_write(hostfile_t *f, const void *buf, uint16_t len, uint16_t *put) {
  lfs_ssize_t n = lfs_file_write(lfsops_volume(), &f->fh, buf, len);
  if (put) *put = (n < 0) ? 0 : (uint16_t)n;
  return n < 0 || (uint16_t)n != len;
}

uint8_t hostfs_seek(hostfile_t *f, uint32_t pos) {
  return lfs_file_seek(lfsops_volume(), &f->fh, pos, LFS_SEEK_SET) < 0;
}

uint32_t hostfs_size(hostfile_t *f) {
  lfs_soff_t n = lfs_file_size(lfsops_volume(), &f->fh);
  return (n < 0) ? 0 : (uint32_t)n;
}

void hostfs_close(hostfile_t *f) {
  lfs_file_close(lfsops_volume(), &f->fh);
}

uint8_t hostfs_opendir(hostdir_t *d, uint8_t part, const dir_t *dir) {
  (void)part;
  return lfs_dir_open(lfsops_volume(), &d->dh,
                      dir->lfs.path[0] ? dir->lfs.path : "/") < 0;
}

void hostfs_closedir(hostdir_t *d) {
  lfs_dir_close(lfsops_volume(), &d->dh);
}

uint8_t hostfs_readdir(hostdir_t *d, hostinfo_t *info) {
  struct lfs_info li;
  int res;

  /* Skip "." and ".." -- FatFs's f_readdir did not surface them to this
     caller, and the swaplist scan would otherwise treat them as files. */
  do {
    res = lfs_dir_read(lfsops_volume(), &d->dh, &li);
    if (res < 0)
      return 1;
    if (res == 0) {           /* end of directory */
      info->name[0] = 0;
      return 0;
    }
  } while (li.name[0] == '.' &&
           (li.name[1] == 0 || (li.name[1] == '.' && li.name[2] == 0)));

  ustrcpy(info->name, (const uint8_t *)li.name);
  info->size   = li.size;
  info->is_dir = (li.type == LFS_TYPE_DIR);
  return 0;
}
