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


   ramops.c: fileops_t backend for drive 9, the 128KB RAM disk

   2026-09-10: added to test whether the >4KB LOAD/SAVE watchdog fix generalises, without repeatedly wearing out
   real flash to do it, and to give the reliability-checksum BASIC test
   somewhere to point that isn't drive 8's real files. Real hardware
   answering as device 8 (flash, unchanged) AND device 9 (this file, RAM)
   at once -- see iec.c's device_address+1 match and parser.c's
   parse_path() override for how a transaction ends up here.

   Deliberately narrow scope, agreed before writing any of this: plain
   sequential files and a directory listing only. No D64/DNP image
   mounting, no REL files, no subdirectories. (Web UI visibility was
   also out of scope originally -- fs_bridge.c/httpd.c gained a
   fs_volume_t selector on 2026-09-10 so the Files page can reach this
   volume too; see fs_bridge.h's header for that protocol.) Everything
   this drive needs to do, it does against a second, independent littlefs
   instance (lfs_ram) backed by ram_flash.c -- SEPARATE from lfsops.c's
   global `lfs` on purpose, so nothing here can regress the flash path
   the previous session's fix just proved on hardware. buffer_t's pvt.lfs
   union member (fh/cache/cfg) is reused as-is: littlefs's own API is
   already fully instance-parameterised (every call takes an explicit
   lfs_t*), so the only thing that made lfsops.c's version
   single-instance was that file hardcoding `&lfs` by name throughout --
   nothing in buffers.h/wrapops.h needed to change for a second backend
   to exist.

   Stubbed on purpose, matching this scope: open_rel, mkdir, rename's
   sibling operations that don't apply to a flat root. format is
   lfsops.h's own format_dummy(), reused as-is -- littlefs already
   doesn't support a CBM-style reformat-in-place either, so this needed
   no new stub. read_sector/write_sector never fire in normal use
   (nothing here is ever a mounted image), kept only because fileops_t
   requires every field non-NULL.

   No P00 support, no extension-hiding: this is a scratch volume for a
   test, not a compatibility target for an existing file collection the
   way drive 8 is. A file's CBM type (PRG/SEQ/USR/REL) is read straight
   off a literal ".PRG"/".SEQ"/".USR"/".REL" suffix, defaulting to PRG --
   simpler than lfsops.c's check_extension()/build_name(), which also
   handle P00 and the EXTENSION_HIDING global this drive doesn't need. */

#include <string.h>

#include "config.h"
#include "buffers.h"
#include "dirent.h"
#include "errormsg.h"
#include "lfsops.h"
#include "parser.h"
#include "ram_flash.h"
#include "ramops.h"
#include "ustring.h"
#include "uart.h"

static lfs_t lfs_ram;
static uint8_t ram_mounted;

/* True once ramops_init() has a working volume. Nothing currently
   checks this before dispatching through partition[RAM_PARTITION].fop
   -- a failed mount leaves fop set anyway, so a device-9 command would
   fail with a generic littlefs error from whichever call hit the
   unmounted instance first, rather than a clear "no drive 9" message.
   Exposed in case a caller wants to check first; not yet consulted
   anywhere. */
uint8_t ramops_available(void) {
  return ram_mounted;
}

/* The RAM volume, for callers outside the IEC path (fs_bridge.c's web
   file manager) that need the raw lfs_t* -- same shape as
   lfsops_volume(). */
lfs_t *ramops_volume(void) {
  return &lfs_ram;
}

/* Free/total bytes for the web UI's RAM tab, mirroring lfsops.c's
   fs_free_bytes()/fs_total_bytes(). Unlike the flash pair, this is
   computed fresh on every call rather than cached: lfs_fs_size() is a
   plain in-RAM traversal of a 128KB volume, cheap enough that flash's
   whole reason for caching (avoiding a real block-device traversal
   from the fs_bridge pump) doesn't apply here. */
uint32_t ramops_free_bytes(void) {
  lfs_ssize_t used = lfs_fs_size(&lfs_ram);
  if (used < 0)
    return 0;
  const struct lfs_config *cfg = ram_flash_config();
  if ((lfs_size_t)used > cfg->block_count)
    return 0;
  return (cfg->block_count - (lfs_size_t)used) * cfg->block_size;
}

uint32_t ramops_total_bytes(void) {
  const struct lfs_config *cfg = ram_flash_config();
  return (uint32_t)cfg->block_count * cfg->block_size;
}

/* littlefs cannot allocate a file's cache under LFS_NO_MALLOC -- same
   constraint lfsops.c's lfs_open_cached() exists for, reimplemented
   here against lfs_ram instead of the flash lfs. */
static int ram_open_cached(lfs_file_t *f, struct lfs_file_config *cfg,
                           uint8_t *cache, const char *path, int flags) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->buffer = cache;
  return lfs_file_opencfg(&lfs_ram, f, path, flags, cfg);
}

void ramops_init(void) {
  /* Always format fresh. Unlike lfsops_init()'s flash volume, there is
     never a valid filesystem here to try to recover -- ram_flash.c's
     backing array is whatever was in SRAM at power-on, and even a valid-
     looking littlefs image in it would be leftover noise from a
     previous, unrelated boot's stack/heap contents, not real data. */
  int res = lfs_format(&lfs_ram, ram_flash_config());
  if (res >= 0)
    res = lfs_mount(&lfs_ram, ram_flash_config());
  ram_mounted = (res >= 0);

  if (!ram_mounted) {
    /* Silent failure here would be worse than the thing it's guarding
       against: a RAM disk that never worked would look identical, from
       the C64 side, to one that just hasn't been tried yet. Say so on
       the console -- same reasoning as lfsops_init()'s own bad-mount
       trace -- rather than leaving partition[RAM_PARTITION].fop pointed
       at an instance nothing ever mounted. */
    uart_puts_P(PSTR("\r\nram9: format/mount failed, drive 9 unavailable\r\n"));
    return;
  }

  partition[RAM_PARTITION].fop = &ramops;
  ustrcpy((uint8_t *)partition[RAM_PARTITION].current_dir.lfs.path,
          (const uint8_t *)"/");
  if (max_part < RAM_PARTITION + 1)
    max_part = RAM_PARTITION + 1;
}

/* ------------------------------------------------------------------------- */
/*  File type from a literal suffix -- see this file's header comment.       */
/* ------------------------------------------------------------------------- */

static uint8_t type_from_name(const uint8_t *name) {
  uint8_t len = ustrlen(name);
  if (len < 4 || name[len-4] != '.')
    return TYPE_PRG;

  const uint8_t *ext = name + len - 3;
  if (!ustrcmp(ext, (const uint8_t *)"PRG")) return TYPE_PRG;
  if (!ustrcmp(ext, (const uint8_t *)"SEQ")) return TYPE_SEQ;
  if (!ustrcmp(ext, (const uint8_t *)"USR")) return TYPE_USR;
  if (!ustrcmp(ext, (const uint8_t *)"REL")) return TYPE_REL;
  return TYPE_PRG;
}

/* ------------------------------------------------------------------------- */
/*  Refill/write/close callbacks -- the buf->refill()/cleanup() pointers     */
/*  iec_talk_handler()/iec_listen_handler() already call generically.        */
/* ------------------------------------------------------------------------- */

static uint8_t ramop_file_read(buffer_t *buf) {
  lfs_ssize_t bytesread = lfs_file_read(&lfs_ram, &buf->pvt.lfs.fh,
                                        buf->data+2, 254);
  if (bytesread < 0) {
    parse_error((int)bytesread, 1);
    return 1;
  }

  if (bytesread == 0) {
    bytesread = 1;
    buf->data[2] = 13;
  }

  buf->position = 2;
  buf->lastused = (uint8_t)bytesread + 1;
  buf->sendeoi  = (bytesread < 254 ||
                   (lfs_file_size(&lfs_ram, &buf->pvt.lfs.fh) -
                    (uint32_t)lfs_file_tell(&lfs_ram, &buf->pvt.lfs.fh)) == 0)
                ? 1 : 0;
  return 0;
}

static uint8_t ramop_file_write(buffer_t *buf) {
  if (!buf->mustflush)
    buf->lastused = buf->position - 1;

  lfs_ssize_t n = lfs_file_write(&lfs_ram, &buf->pvt.lfs.fh,
                                 buf->data+2, buf->lastused - 1);
  if (n < 0) {
    parse_error((int)n, 1);
    lfs_file_close(&lfs_ram, &buf->pvt.lfs.fh);
    free_buffer(buf);
    return 1;
  }
  if ((lfs_size_t)(buf->lastused - 1) != (lfs_size_t)n) {
    set_error(ERROR_DISK_FULL);
    lfs_file_close(&lfs_ram, &buf->pvt.lfs.fh);
    free_buffer(buf);
    return 1;
  }

  mark_buffer_clean(buf);
  buf->mustflush = 0;
  buf->position  = 2;
  buf->lastused  = 2;
  return 0;
}

static uint8_t ramop_file_close(buffer_t *buf) {
  if (!buf->allocated) return 0;

  if (buf->write) {
    if (buf->refill(buf))
      return 1;
  }

  int res = lfs_file_close(&lfs_ram, &buf->pvt.lfs.fh);
  parse_error(res, 1);
  buf->cleanup = callback_dummy;

  return (res < 0) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*  fileops_t                                                                */
/* ------------------------------------------------------------------------- */

static void ramop_open_read(path_t *path, cbmdirent_t *dent, buffer_t *buf) {
  (void)path;
  char fullpath[LFS_REALNAME_MAX + 2];
  fullpath[0] = '/';
  ustrcpy((uint8_t *)fullpath + 1,
          dent->pvt.host.realname[0] ? dent->pvt.host.realname : dent->name);

  int res = ram_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                            buf->pvt.lfs.cache, fullpath, LFS_O_RDONLY);
  if (res < 0) {
    parse_error(res, 1);
    return;
  }

  buf->pvt.lfs.headersize = 0;
  buf->read    = 1;
  buf->cleanup = ramop_file_close;
  buf->refill  = ramop_file_read;
  buf->seek    = NULL;   /* RECORD#/P: not supported -- doscmd.c checks for NULL */

  stick_buffer(buf);
  buf->refill(buf);
}

static void ramop_open_write(path_t *path, cbmdirent_t *dent, uint8_t type,
                             buffer_t *buf, uint8_t append) {
  (void)path; (void)type;
  char fullpath[LFS_REALNAME_MAX + 2];
  fullpath[0] = '/';
  /* dent->pvt.host.realname is only meaningful when the file already
     exists -- populated by a prior directory lookup, same as
     lfsop_open_write()'s append branch (lfsops.c). A brand-new file
     (append==0, the ordinary SAVE case) has no such lookup behind it;
     dent->pvt.host is whatever this cbmdirent_t last held, and trusting
     realname[0]'s truthiness there reads stale/uninitialised bytes
     instead of the name actually being created. Confirmed on real
     hardware 2026-09-10: every C64-created RAM file's on-disk name had
     its first two bytes replaced with garbage, invisible until the web
     GUI's directory listing (added this session) exposed the raw
     littlefs entry -- LOAD"$",9's own listing path never showed it. */
  ustrcpy((uint8_t *)fullpath + 1,
          (append && dent->pvt.host.realname[0]) ? dent->pvt.host.realname : dent->name);

  int flags = append ? LFS_O_WRONLY
                     : (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
  int res = ram_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                            buf->pvt.lfs.cache, fullpath, flags);
  if (res >= 0 && append)
    res = lfs_file_seek(&lfs_ram, &buf->pvt.lfs.fh,
                        lfs_file_size(&lfs_ram, &buf->pvt.lfs.fh), LFS_SEEK_SET);
  if (res < 0) {
    parse_error(res, 0);
    return;
  }

  buf->pvt.lfs.headersize = 0;
  mark_write_buffer(buf);
  buf->position = 2;
  buf->lastused = 2;
  buf->cleanup  = ramop_file_close;
  buf->refill   = ramop_file_write;
  buf->seek     = NULL;
  buf->data[2]  = 13;   /* if nothing is ever written, the file ends up one 0x0d byte */
}

static void ramop_open_rel(path_t *path, cbmdirent_t *dent, buffer_t *buf,
                           uint8_t recordlen, uint8_t mode) {
  (void)path; (void)dent; (void)buf; (void)recordlen; (void)mode;
  set_error(ERROR_SYNTAX_UNABLE);
}

static uint8_t ramop_delete(path_t *path, cbmdirent_t *dent) {
  (void)path;
  char fullpath[CBM_NAME_LENGTH + 2];
  fullpath[0] = '/';
  ustrcpy((uint8_t *)fullpath + 1, dent->name);

  int res = lfs_remove(&lfs_ram, fullpath);
  return (res >= 0) ? 1 : 0;
}

static uint8_t ramop_disk_label(uint8_t part, uint8_t *label) {
  (void)part;
  /* wrapops.h: "up to 17 bytes, 0-terminated" -- unlike dir_label below,
     which wants fixed-width space padding instead. */
  ustrcpy(label, (const uint8_t *)"RAMDISK 9");
  return 0;
}

static uint8_t ramop_dir_label(path_t *path, uint8_t *label) {
  (void)path;
  memset(label, ' ', CBM_NAME_LENGTH);
  memcpy(label, "RAMDISK 9", 9);
  return 0;
}

static uint8_t ramop_disk_id(path_t *path, uint8_t *id) {
  (void)path;
  memcpy(id, "R9 2A", 5);
  return 0;
}

static uint16_t ramop_disk_free(uint8_t part) {
  (void)part;
  /* Small volume (256 x 512-byte blocks) -- unlike lfsops.c's cached,
     idle-recomputed version for the 3MB flash volume, a direct
     lfs_fs_size() here is cheap enough to call synchronously, mid
     transaction, every time. In CBM 254-byte blocks. */
  lfs_ssize_t used = lfs_fs_size(&lfs_ram);
  if (used < 0) return 0;
  uint32_t total_bytes = (uint32_t)ram_flash_config()->block_count *
                          ram_flash_config()->block_size;
  uint32_t used_bytes  = (uint32_t)used * ram_flash_config()->block_size;
  if (used_bytes > total_bytes) return 0;
  return (uint16_t)((total_bytes - used_bytes) / 254);
}

static void ramop_read_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector) {
  (void)buf; (void)part;
  set_error_ts(ERROR_READ_NOHEADER, track, sector);
}

static void ramop_write_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector) {
  (void)buf; (void)part;
  set_error_ts(ERROR_READ_NOHEADER, track, sector);
}

static void ramop_closedir(dh_t *dh) {
  if (dh->dir.lfs.open) {
    dh->dir.lfs.open = 0;
    lfs_dir_close(&lfs_ram, &dh->dir.lfs.d);
  }
}

static uint8_t ramop_opendir(dh_t *dh, path_t *path) {
  dh->part = path->part;
  int res = lfs_dir_open(&lfs_ram, &dh->dir.lfs.d, "/");
  dh->dir.lfs.open = (res >= 0);
  if (res < 0) {
    parse_error(res, 1);
    return 1;
  }
  return 0;
}

static int8_t ramop_readdir(dh_t *dh, cbmdirent_t *dent) {
  struct lfs_info finfo;
  int res;

  do {
    res = lfs_dir_read(&lfs_ram, &dh->dir.lfs.d, &finfo);
    if (res < 0) {
      parse_error(res, 1);
      return 1;
    }
    if (res == 0) {
      finfo.name[0] = 0;
      ramop_closedir(dh);
      break;
    }
  } while (finfo.name[0] == '.');   /* skip "." and ".." */

  memset(dent, 0, sizeof(cbmdirent_t));
  if (!finfo.name[0])
    return -1;

  dent->opstype = OPSTYPE_HOST;
  ustrcpy(dent->pvt.host.realname, (const uint8_t *)finfo.name);

  if (finfo.type == LFS_TYPE_DIR) {
    /* Never actually created (mkdir is a stub), but littlefs's root
       always has "." and ".." which are already filtered above -- this
       branch is unreachable in normal use and kept only so a readdir on
       a hand-crafted/corrupt volume doesn't misreport a directory as a
       zero-byte PRG. */
    dent->typeflags = TYPE_DIR;
  } else {
    uint8_t len = ustrlen((const uint8_t *)finfo.name);
    if (len > CBM_NAME_LENGTH)
      len = CBM_NAME_LENGTH;
    memcpy(dent->name, finfo.name, len);
    dent->typeflags = type_from_name((const uint8_t *)finfo.name);
  }

  if (finfo.size > 16255746)
    dent->blocksize = 63999;
  else
    dent->blocksize = (finfo.size + 253) / 254;
  dent->remainder = finfo.size % 254;

  dent->date.year = 80; dent->date.month = 1; dent->date.day = 1;
  return 0;
}

static void ramop_mkdir(path_t *path, uint8_t *dirname) {
  (void)path; (void)dirname;
  set_error(ERROR_SYNTAX_UNABLE);
}

static uint8_t ramop_chdir(path_t *path, cbmdirent_t *dent) {
  /* Flat root only -- CD anywhere is a harmless no-op, same convention
     eefs-ops.c uses for the same reason. */
  (void)path; (void)dent;
  return 0;
}

static void ramop_rename(path_t *path, cbmdirent_t *oldname, uint8_t *newname) {
  (void)path;
  char oldpath[CBM_NAME_LENGTH + 2], newpath[CBM_NAME_LENGTH + 2];
  oldpath[0] = '/';
  ustrcpy((uint8_t *)oldpath + 1, oldname->name);
  newpath[0] = '/';
  ustrcpy((uint8_t *)newpath + 1, newname);

  int res = lfs_rename(&lfs_ram, oldpath, newpath);
  parse_error(res, 0);
}

const PROGMEM fileops_t ramops = {
  ramop_open_read,
  ramop_open_write,
  ramop_open_rel,
  ramop_delete,
  ramop_disk_label,
  ramop_dir_label,
  ramop_disk_id,
  ramop_disk_free,
  ramop_read_sector,
  ramop_write_sector,
  format_dummy,   /* lfsops.h -- littlefs has no CBM-style reformat-in-place either */
  ramop_opendir,
  ramop_readdir,
  ramop_closedir,
  ramop_mkdir,
  ramop_chdir,
  ramop_rename
};
