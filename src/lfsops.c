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

/* lfsops.c: the fileops_t backend for a littlefs host volume.
 *
 * A port of fatops.c, not a rewrite. Every piece of CBM semantics --
 * name mangling, P00 containers, wildcards, REL files, image mounting,
 * directory formats -- is kept as it was, because that logic is the hard,
 * well-tested part and none of it depends on which filesystem is
 * underneath. What changed is only the storage calls and how a directory
 * is identified.
 *
 * THE ONE REAL DIFFERENCE: directory identity. FatFs (this fork) can
 * reopen a directory from a cluster number, via the non-standard
 * l_opendir()/l_opencluster() extensions, and fatops.c stored that number
 * in dir_t.fat. littlefs has no cluster concept and no stable numeric
 * directory id, so a directory here is its path (dir_t.lfs.path) and
 * reopening means lfs_dir_open() on that string. Everything that used to
 * pass a cluster around now passes a path; nothing outside this file
 * notices, because generic code never looks inside dir_t (see hostfs.h).
 *
 * Why the switch was made: FAT keeps its
 * metadata at fixed offsets and rewrites it on every operation, so ~1.2%
 * of the flash absorbed essentially all wear, and it has no levelling at
 * any sector or cluster size.
 */

#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "buffers.h"
#include "d64ops.h"
#include "diskchange.h"
#include "diskio.h"
#include "display.h"
#include "doscmd.h"
#include "errormsg.h"
#include "fileops.h"
#include "flags.h"
#include "led.h"
#include "p00cache.h"
#include "parser.h"
#include "progmem.h"
#include "uart.h"

/* Declared here rather than via system.h, which clashes with a Pico SDK
   declaration pulled in by this file. See system.c. */
void system_watchdog_pet(void);
void lfsread_prepost_scratch_note(uint32_t pre_us, uint32_t post_us);
#include "utils.h"
#include "ustring.h"
#include "wrapops.h"
#include "lfs.h"
#include "lfs_flash.h"
#include "lfsops.h"
void primo_turbo_deep(uint32_t step);   /* src/pico/primo_turbo.h */

/* ------------------------------------------------------------------ */
/*  The volume, and paths                                             */
/* ------------------------------------------------------------------ */

/* One volume. FatFs carried a FATFS per partition because it could mount
   several; the flash region holds exactly one littlefs, so partitions
   above 0 simply do not exist here. */
static lfs_t    lfs;
static uint8_t  lfs_mounted;

/* Evidence that this volume was wiped by the auto-reformat above, and
   that the wipe has not been acknowledged.

   Deliberately a FILE on the volume rather than a flag in RAM or in the
   emulated EEPROM. RAM forgets across a power cycle, which is exactly
   when the user is most likely to first notice; the EEPROM is upstream's
   checksummed structure with a "do not remove fields" contract that is
   not worth disturbing for this. A file survives power cycles, shows up
   in the web UI *and* in the C64's LOAD"$" listing without either side
   needing to know about it, explains itself when read, and is dismissed
   by deleting it -- no acknowledge API, no extra state. */
#define REFORMAT_MARKER "/REFORMATTED.TXT"
static uint8_t reformat_marker;           /* marker present right now */
static uint8_t reformat_marker_pending;   /* wrote it this boot */

static const char reformat_marker_text[] =
  "THE DRIVE COULD NOT MOUNT ITS FILESYSTEM AND REFORMATTED IT.\r\n"
  "EVERY FILE WAS LOST, INCLUDING PRIFLOP.CFG, WHICH HELD THE\r\n"
  "WI-FI SETTINGS -- THAT IS WHY THE DRIVE MAY BE BACK IN ACCESS\r\n"
  "POINT MODE.\r\n\r\n"
  "THE FLASH WAS REWRITTEN, SO THE OLD FILES CANNOT BE RECOVERED\r\n"
  "FROM THE DEVICE.\r\n\r\n"
  "DELETE THIS FILE TO DISMISS THE WARNING.\r\n";

static uint8_t marker_exists(void) {
  struct lfs_info info;
  return lfs_stat(&lfs, REFORMAT_MARKER, &info) >= 0;
}

static void write_reformat_marker(void) {
  static lfs_file_t f;
  static uint8_t cache[LFS_FILE_CACHE_SIZE];
  struct lfs_file_config fcfg;

  memset(&fcfg, 0, sizeof(fcfg));
  fcfg.buffer = cache;

  if (lfs_file_opencfg(&lfs, &f, REFORMAT_MARKER,
                       LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fcfg) < 0)
    return;
  lfs_file_write(&lfs, &f, reformat_marker_text,
                 sizeof(reformat_marker_text) - 1);
  lfs_file_close(&lfs, &f);
}

/* Reported by /api/stats so the web UI can warn until it is dismissed.

   Returns a cached byte and touches littlefs NOT AT ALL, because httpd.c
   calls this from core 1. Everything else the web server needs from the
   filesystem goes through fs_bridge for exactly that reason; a stray
   lfs_stat() here would race core 0 mid-transaction. The flag is
   refreshed on core 0 by lfsops_idle_maintenance() instead -- see there
   for why that catches the dismissal promptly. */
uint8_t fs_reformatted(void) {
  return reformat_marker;
}

/* littlefs cannot allocate a file's cache under LFS_NO_MALLOC, so every
   open goes through here with a caller-provided buffer. */
static int lfs_open_cached(lfs_file_t *f, struct lfs_file_config *cfg,
                           uint8_t *cache, const char *path, int flags) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->buffer = cache;
  return lfs_file_opencfg(&lfs, f, path, flags, cfg);
}

/* littlefs links every open directory into lfs->mlist and REQUIRES
   lfs_dir_close(); FatFs's DIR needed no such thing, so fatops.c had
   nothing here to port and the port inherited the omission.

   An unclosed handle leaves that list pointing at memory the caller is
   about to reuse -- dh_t lives in a buffer or on the stack -- and the
   next operation that walks the list (opening a file for write, say)
   follows a dangling pointer and never returns, doing no flash I/O at
   all. That was the write hang found 2026-09-05.

   fileops_t gained a closedir entry for this; every scan must end in one.
   See wrapops.h. */

/* True if `fullpath` is an image some partition currently has mounted.
   Deleting one leaves the drive holding an open handle to a file that no
   longer exists, and nothing else notices until the next access fails --
   there is no way back except a reset. Checked by both delete paths: the
   C64's S: command and the web UI's DELETE. */
bool lfsops_mounted_image(const char *fullpath) {
  for (uint8_t p = 0; p < CONFIG_MAX_PARTITIONS; p++)
    if (partition[p].imagepath[0] &&
        strcmp(partition[p].imagepath, fullpath) == 0)
      return true;
  return false;
}

lfs_t *lfsops_volume(void) {
  return &lfs;
}

/* Build an absolute path from a directory and a name. littlefs paths are
   slash-separated with "/" as root, so this is the whole of what used to
   be FatFs's curr_dir mechanism: fatops.c set fatfs.curr_dir and then
   passed a bare name, and every one of those pairs becomes a join here. */
static void lfs_path_join(char *out, size_t outlen,
                          const char *dir, const uint8_t *name) {
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

/* Convenience: the path of a directory reference. */
static const char *lfs_dirpath(const path_t *path) {
  return path->dir.lfs.path[0] ? path->dir.lfs.path : "/";
}

/* littlefs open flags from the FatFs-style ones fatops.c used. */
static int lfs_open_flags(uint8_t read, uint8_t write, uint8_t create) {
  int f = 0;
  if (read && write) f = LFS_O_RDWR;
  else if (write)    f = LFS_O_WRONLY;
  else               f = LFS_O_RDONLY;
  if (create)        f |= LFS_O_CREAT | LFS_O_TRUNC;
  return f;
}


#define P00_HEADER_SIZE       26
#define P00_CBMNAME_OFFSET    8
#define P00_RECORDLEN_OFFSET  25

#define BOOTSECTOR_FILE       "bootsect.128"

static const PROGMEM char p00marker[] = "C64File";
#define P00MARKER_LENGTH 7

typedef enum { EXT_UNKNOWN, EXT_IS_X00, EXT_IS_TYPE } exttype_t;

uint8_t file_extension_mode;

/* ------------------------------------------------------------------------- */
/*  Utility functions                                                        */
/* ------------------------------------------------------------------------- */

/**
 * parse_error - translates a ff int into a commodore error message
 * @res     : int to be translated
 * @readflag: Flags if it was a read operation
 *
 * This function sets the error channel according to the problem given in
 * res. readflag specifies if a READ ERROR or WRITE ERROR should be used
 * if the error is FR_RW_ERROR.
 */
void parse_error(int res, uint8_t readflag) {
  /* littlefs error codes are negative and share nothing with FatFs's
     FRESULT enum, so fatops.c's switch matched none of them and every
     failure fell through to a generic error. */
  switch (res) {
  case LFS_ERR_OK:
    set_error(ERROR_OK);
    break;

  case LFS_ERR_NOENT:
    set_error_ts(readflag ? ERROR_FILE_NOT_FOUND : ERROR_FILE_NOT_FOUND_39,
                 (uint8_t)(-res), 0);
    break;

  case LFS_ERR_EXIST:
    set_error_ts(ERROR_FILE_EXISTS, (uint8_t)(-res), 0);
    break;

  case LFS_ERR_NOTDIR:
    set_error_ts(ERROR_FILE_NOT_FOUND_39, (uint8_t)(-res), 0);
    break;

  case LFS_ERR_NOTEMPTY:
    set_error_ts(ERROR_DIR_ERROR, (uint8_t)(-res), 0);
    break;

  case LFS_ERR_NOSPC:
    set_error_ts(ERROR_DISK_FULL, (uint8_t)(-res), 0);
    break;

  case LFS_ERR_NAMETOOLONG:
  case LFS_ERR_INVAL:
    set_error_ts(ERROR_SYNTAX_JOKER, (uint8_t)(-res), 0);
    break;

  case LFS_ERR_CORRUPT:
    set_error_ts(ERROR_READ_NOHEADER, (uint8_t)(-res), 0);
    break;

  case LFS_ERR_NOMEM:
  case LFS_ERR_NOATTR:
  default:
    if (res < 0)
      set_error_ts(readflag ? ERROR_READ_NODATA : ERROR_WRITE_VERIFY,
                   (uint8_t)(-res), 0);
    else
      set_error(ERROR_OK);
    break;
  }
}

/**
 * check_extension - check for known file-type-based name extensions
 * @name: pointer to the file name
 * @ext : pointer to pointer to the file extension
 *
 * This function checks if the given file name has an extension that
 * indicates a specific file type like PRG/SEQ/P00/S00/... The ext
 * pointer will be set to the first character of the extension if
 * any is present or NULL if not. Returns EXT_IS_X00 for x00,
 * EXT_IS_TYPE for PRG/SEQ/... or EXT_UNKNOWN for an unknown file extension.
 */
static exttype_t check_extension(uint8_t *name, uint8_t **ext) {
  uint8_t f,s,t;

  /* Search for the file extension */
  if ((*ext = ustrrchr(name, '.')) != NULL) {
    /* toupper() because a littlefs name keeps whatever case it was
       created with -- "prog.prg" is a perfectly ordinary filename here,
       where FAT's 8.3 short names were always upper-case. check_imageext()
       below already did this; this one did not, so a lower-case .prg was
       not recognised as a type extension. */
    f = toupper(*(++(*ext)));
    s = toupper(*(*ext+1));
    t = toupper(*(*ext+2));
    if ((f == 'P' || f == 'S' ||
         f == 'U' || f == 'R') &&
        isdigit(s) && isdigit(t))
      return EXT_IS_X00;
    else if ((f=='P' && s == 'R' && t == 'G') ||
             (f=='S' && s == 'E' && t == 'Q') ||
             (f=='R' && s == 'E' && t == 'L') ||
             (f=='U' && s == 'S' && t == 'R'))
      return EXT_IS_TYPE;
  }
  return EXT_UNKNOWN;
}

/**
 * check_imageext - check for a known image file extension
 * @name: pointer to the file name
 *
 * This function checks if the given file name has an extension that
 * indicates a known image file type. Returns IMG_IS_DISK for
 * D64/D41/D71/D81/DNP files or IMG_UNKNOWN for an unknown file
 * extension.
 */
imgtype_t check_imageext(uint8_t *name) {
  uint8_t f,s,t;
  uint8_t *ext = ustrrchr(name, '.');

  if (ext == NULL)
    return IMG_UNKNOWN;

  if (ustrlen(ext) != 4)
    return IMG_UNKNOWN;

  f = toupper(*++ext);
  s = toupper(*++ext);
  t = toupper(*++ext);

  if (f == 'D') {
    if ((s == '6' && t == '4') ||
        (s == 'N' && t == 'P') ||
        ((s == '4' || s == '7' || s == '8') &&
         (t == '1'))) {
      return IMG_IS_DISK;
    }
  }

  return IMG_UNKNOWN;
}

/**
 * should_save_raw - check if the file should be saved header-free
 * @name: pointer to the file name
 *
 * This function checks if the file of the given name should be saved
 * without any header for improved PC compatiblity.
 */
static bool should_save_raw(uint8_t* name) {
  if (check_imageext(name) != IMG_UNKNOWN)
    return true;

  uint8_t* ext = ustrrchr(name, '.');
  if (ext == NULL)
    return false;

  ext++;

  if (*ext == 't' || *ext == 'T')
    ext++;

  if (ustrlen(ext) != 3)
    return false;

  uint8_t ucext[3];

  for (int i = 0; i < 3; i++) {
    ucext[i] = toupper(*ext++);
  }

  if (ucext[0] == 'C' && ucext[1] == 'R' && ucext[2] == 'T')
    return true;

  return false;
}

/**
 * pet2asc - convert string from PETSCII to ASCII
 * @buf: pointer to the string to be converted
 *
 * This function converts the string in the given buffer from PETSCII to
 * ASCII in-place.
 */
static void pet2asc(uint8_t *buf) {
  uint8_t ch;
  while (*buf) {
    ch = *buf;
    if (ch > (128+64) && ch < (128+91))
      ch -= 128;
    else if (ch > (96-32) && ch < (123-32))
      ch += 32;
    else if (ch > (192-128) && ch < (219-128))
      ch += 128;
    else if (ch == 255)
      ch = '~';
    *buf = ch;
    buf++;
  }
}

/**
 * is_valid_fat_char - checks if a character is valid on FAT
 * @c: character to check
 *
 * This function checks if @c is a valid character for a FAT
 * file name. Returns true if it is, false if not.
 */
static bool is_valid_fat_char(const uint8_t c) {
  if (isalnum(c) || c == '!' || c == ' ' ||
      (c >= '#' && c <= ')') ||
      c == '-' || c == '.')
    return true;
  else
    return false;
}

/**
 * is_valid_fat_name - checks if a file name is valid on FAT
 * @name: name to check
 *
 * This function checks if @name is a valid name for a FAT
 * file. Returns true if it is, false if not.
 */
static bool is_valid_fat_name(const uint8_t *name) {
  const uint8_t *ptr = name;
  unsigned char dots = 0;

  /* check all characters for validity */
  while (*ptr) {
    if (*ptr == '.')
      dots++;

    if (!is_valid_fat_char(*ptr++))
      return false;
  }

  if (dots > 1)
    return false;

  /* check the last character */
  ptr--;

  if (*ptr == ' ')
    return false;

  if (*ptr == '.')
    return false;

  return true;
}

/**
 * build_name - convert PETSCII file name to valid FAT name
 * @name: pointer to a PETSCII file name to be converted
 * @type: file type
 *
 * This function converts a PETSCII file name to a suitable
 * FAT file name in-place. Returns a pointer to the last
 * character of the PC64 file extension if it was
 * created or NULL if not.
 */
static uint8_t* build_name(uint8_t *name, uint8_t type) {
  /* convert to PETSCII */
  pet2asc(name);

  /* known disk-image extensions are always without header or suffix */
  if (type == TYPE_PRG && should_save_raw(name))
    return NULL;

  /* PC64 mode or invalid FAT name? */
  if ((file_extension_mode == 1 && type != TYPE_PRG) ||
      file_extension_mode == 2 ||
      !is_valid_fat_name(name)) {

      uint8_t *x00ext = NULL;

      /* Append .[PSUR]00 suffix to the file name */
      while (*name) {
        if (is_valid_fat_char(*name)) {
          name++;
        } else {
          *name++ = '_';
        }
      }
      *name++ = '.';
      *name++ = pgm_read_byte(filetypes+3*type);
      *name++ = '0';
      x00ext = name;
      *name++ = '0';
      *name   = 0;

      return x00ext;
  }

  /* type-suffix mode? */
  if ((file_extension_mode == 3 && type != TYPE_PRG) ||
      (file_extension_mode == 4)) {
    /* Append type suffix to the file name */
    while (*name) name++;
    *name++ = '.';
    memcpy_P(name, filetypes + TYPE_LENGTH * (type & EXT_TYPE_MASK), TYPE_LENGTH);
    *(name+3) = 0;

    return NULL;
  }

  /* extension mode 0 and no special case */
  return NULL;
}

/* ------------------------------------------------------------------------- */
/*  Callbacks                                                                */
/* ------------------------------------------------------------------------- */

/**
 * lfsop_file_read - read the next data block into the buffer
 * @buf: buffer to be worked on
 *
 * This function reads the next block of data from the associated file into
 * the given buffer. Used as a refill-callback when reading files
 */
/* Investigating a real-hardware failure (Primo/CDOS LOAD of a file
   crossing a 4096-byte littlefs block boundary -- exactly one block --
   fails; anything within one block does not). lfs_flash_read() is a
   plain memcpy from XIP-mapped flash (see lfs_flash.c), not a slow
   explicit flash op, so the obvious "flash erase stalls a byte, same as
   the 2026-09-05 write-side investigation" theory doesn't fit here
   without measuring it -- that investigation was about lfs_flash_prog()
   (flash_ops/flash_op_max_us), which reads never touch. Measuring the
   read side the same way, rather than guessing from that precedent. */
volatile uint32_t lfsread_max_us;
volatile uint32_t lfsread_ops;
/* Phase B1 (2026-09-07): lfsread_max_us above
   only ever timed the lfs_file_read() call itself (confirmed fast,
   47-49us) -- not the lfs_file_tell()/header arithmetic before it, or
   the EOF/sendeoi fixup after it (which calls lfs_file_size() and
   lfs_file_tell() again). Timing those two separately, since either
   could be where the still-unexplained part of the send-side gap
   (iec_stat_put_gap_max_us, src/iec.c) actually goes. Current-boot only
   -- no free watchdog scratch slot for these; src/iec.c's
   refill_max_us/findbuf_max_us cover the crash-surviving version at a
   coarser grain (the whole buf->refill(buf) call vs. what's after it). */
volatile uint32_t lfsread_pre_max_us;
volatile uint32_t lfsread_post_max_us;

static uint8_t lfsop_file_read(buffer_t *buf) {
  int res;
  lfs_size_t bytesread;


  uint32_t pre_t0 = time_us_32();
  buf->fptr = (uint32_t)lfs_file_tell(&lfs, &buf->pvt.lfs.fh) - buf->pvt.lfs.headersize;
  uint32_t pre_d = time_us_32() - pre_t0;
  if (pre_d > lfsread_pre_max_us) lfsread_pre_max_us = pre_d;

  uint32_t t0 = time_us_32();
  bytesread = lfs_file_read(&lfs, &buf->pvt.lfs.fh, buf->data+2, (buf->recordlen ? buf->recordlen : 254));
  { uint32_t d = time_us_32() - t0;
    if (d > lfsread_max_us) lfsread_max_us = d;
    lfsread_ops++; }
  res = (bytesread < 0) ? (int)bytesread : 0;
  if (res < 0) {
    parse_error(res,1);
    free_buffer(buf);
    return 1;
  }

  uint32_t post_t0 = time_us_32();

  /* The bus protocol can't handle 0-byte-files */
  if (bytesread == 0) {
    bytesread = 1;
    /* Experimental data suggests that this may be correct */
    buf->data[2] = (buf->recordlen ? 255 : 13);
  }

  buf->position = 2;
  buf->lastused = bytesread+1;
  if(buf->recordlen) // strip nulls from end of REL record.
    while(!buf->data[buf->lastused] && --(buf->lastused) > 1);
  if (bytesread < 254
      || (lfs_file_size(&lfs, &buf->pvt.lfs.fh) - (uint32_t)lfs_file_tell(&lfs, &buf->pvt.lfs.fh)) == 0
      || buf->recordlen
     )
    buf->sendeoi = 1;
  else
    buf->sendeoi = 0;

  uint32_t post_d = time_us_32() - post_t0;
  if (post_d > lfsread_post_max_us) lfsread_post_max_us = post_d;

  /* Survives a crash where lfsread_pre_max_us/lfsread_post_max_us above
     don't -- see system.h's comment on this function. */
  lfsread_prepost_scratch_note(pre_d, post_d);

  return 0;
}

/**
 * lfs_write_data - write the current buffer data
 * @buf: buffer to be worked on
 *
 * This function writes the current contents of the given buffer into its
 * associated file.
 */
static uint8_t lfs_write_data(buffer_t *buf) {
  int res;
  lfs_size_t byteswritten;


  if(!buf->mustflush)
    buf->lastused = buf->position - 1;

  if(buf->recordlen > buf->lastused - 1)
    memset(buf->data + buf->lastused + 1,0,buf->recordlen - (buf->lastused - 1));

  if(buf->recordlen)
    buf->lastused = buf->recordlen + 1;

  byteswritten = lfs_file_write(&lfs, &buf->pvt.lfs.fh, buf->data+2, buf->lastused-1);
  res = (byteswritten < 0) ? (int)byteswritten : 0;
  if (res < 0) {
    parse_error(res,1);
    lfs_file_close(&lfs, &buf->pvt.lfs.fh);
    free_buffer(buf);
    return 1;
  }

  if (byteswritten != buf->lastused-1U) {
    set_error(ERROR_DISK_FULL);
    lfs_file_close(&lfs, &buf->pvt.lfs.fh);
    free_buffer(buf);
    return 1;
  }

  mark_buffer_clean(buf);
  buf->mustflush = 0;
  buf->position  = 2;
  buf->lastused  = 2;
  buf->fptr      = (uint32_t)lfs_file_tell(&lfs, &buf->pvt.lfs.fh) - buf->pvt.lfs.headersize;

  return 0;
}

/**
 * lfsop_file_write - refill-callback for files opened for writing
 * @buf: target buffer
 *
 * This function writes the contents of buf to the associated file.
 */
static uint8_t lfsop_file_write(buffer_t *buf) {
  int res = 0;
  uint32_t fptr;
  uint32_t i = 0;

  fptr = lfs_file_size(&lfs, &buf->pvt.lfs.fh) - buf->pvt.lfs.headersize;

  // on a REL file, the fptr will be be at the end of the record we just read.  Reposition.
  if (buf->fptr != fptr) {
    res = lfs_file_seek(&lfs, &buf->pvt.lfs.fh, buf->pvt.lfs.headersize + buf->fptr, LFS_SEEK_SET);
    if (res < 0) {
      parse_error(res,1);
      lfs_file_close(&lfs, &buf->pvt.lfs.fh);
      free_buffer(buf);
      return 1;
    }
  }

  if(buf->fptr > fptr)
    i = buf->fptr - fptr;

  if (res >= 0) {
    if (lfs_write_data(buf))
      return 1;
  }

  if(i) {
    // we need to fill bytes.
    // position to old end of file.
    res = lfs_file_seek(&lfs, &buf->pvt.lfs.fh, buf->pvt.lfs.headersize + fptr, LFS_SEEK_SET);
    buf->mustflush = 0;
    buf->fptr = fptr;
    buf->data[2] = (buf->recordlen?255:0);
    memset(buf->data + 3,0,253);
    while(res >= 0 && i) {
      if (buf->recordlen)
        buf->lastused = buf->recordlen;
      else
        buf->lastused = (i>254 ? 254 : (uint8_t) i);

      i -= buf->lastused;
      buf->position = buf->lastused + 2;

      if(lfs_write_data(buf))
        return 1;
    }
    res = lfs_file_seek(&lfs, &buf->pvt.lfs.fh, lfs_file_size(&lfs, &buf->pvt.lfs.fh), LFS_SEEK_SET);
    if (res < 0) {
      parse_error(res,1);
      lfs_file_close(&lfs, &buf->pvt.lfs.fh);
      free_buffer(buf);
      return 1;
    }
    buf->fptr = (uint32_t)lfs_file_tell(&lfs, &buf->pvt.lfs.fh) - buf->pvt.lfs.headersize;
  }

  return 0;
}

/**
 * lfsop_file_seek - callback for seek
 * @buf     : buffer to be worked on
 * @position: offset to seek to
 * @index   : offset within the record to seek to
 *
 * This function seeks to the offset position in the file associated
 * with the given buffer and sets the read pointer to the byte given
 * in index, effectively seeking to (position+index) for normal files.
 * Returns 1 if an error occured, 0 otherwise.
 */
uint8_t lfsop_file_seek(buffer_t *buf, uint32_t position, uint8_t index) {
  uint32_t pos = position + buf->pvt.lfs.headersize;

  if (buf->dirty)
    if (lfsop_file_write(buf))
      return 1;

  if (lfs_file_size(&lfs, &buf->pvt.lfs.fh) >= pos) {
    int res = lfs_file_seek(&lfs, &buf->pvt.lfs.fh, pos, LFS_SEEK_SET);
    if (res < 0) {
      parse_error(res,0);
      lfs_file_close(&lfs, &buf->pvt.lfs.fh);
      free_buffer(buf);
      return 1;
    }

    if (lfsop_file_read(buf))
      return 1;
  } else {
    buf->data[2]  = (buf->recordlen ? 255:13);
    buf->lastused = 2;
    buf->fptr     = position;
    set_error(ERROR_RECORD_MISSING);
  }

  buf->position = index + 2;
  if(index + 2 > buf->lastused)
    buf->position = buf->lastused;

  return 0;
}

/**
 * lfsop_file_sync - synchronize the current REL file.
 * @buf: buffer to be worked on
 *
 */
static uint8_t lfsop_file_sync(buffer_t *buf) {
  return lfsop_file_seek(buf,buf->fptr + buf->recordlen,0);
}

/**
 * lfsop_file_close - close the file associated with a buffer
 * @buf: buffer to be worked on
 *
 * This function closes the file associated with the given buffer. If the buffer
 * was opened for writing the data contents will be stored if required.
 * Additionally the buffer will be marked as free.
 * Used as a cleanup-callback for reading and writing.
 */
static uint8_t lfsop_file_close(buffer_t *buf) {
  lfsops_freeblocks_invalidate();
  int res;

  if (!buf->allocated) return 0;

  if (buf->write) {
    /* Write the remaining data using the callback */
    if (buf->refill(buf)) {
      return 1;
    }
  }

  res = lfs_file_close(&lfs, &buf->pvt.lfs.fh);
  parse_error(res,1);
  buf->cleanup = callback_dummy;

  if (res < 0)
    return 1;
  else
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  Internal handlers for the various operations                             */
/* ------------------------------------------------------------------------- */

/**
 * lfsop_open_read - opens a file for reading
 * @path: path of the file
 * @dent: pointer to cbmdirent with name of the file
 * @buf : buffer to be used
 *
 * This functions opens a file in the FAT filesystem for reading and sets up
 * buf to access it.
 */
void lfsop_open_read(path_t *path, cbmdirent_t *dent, buffer_t *buf) {
  int res;
  uint8_t *name;

  pet2asc(dent->name);
  if (dent->pvt.host.realname[0])
    name = dent->pvt.host.realname;
  else
    name = dent->name;

  char fullpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
  lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), name);
  res = lfs_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                          buf->pvt.lfs.cache, fullpath, lfs_open_flags(1, 0, 0));
  if (res < 0) {
    parse_error(res,1);
    return;
  }

  if (dent->opstype == OPSTYPE_HOST_X00) {
    /* It's a [PSUR]00 file, skip the header */
    /* If anything goes wrong here, refill will notice too */
    lfs_file_seek(&lfs, &buf->pvt.lfs.fh, P00_HEADER_SIZE, LFS_SEEK_SET);
    buf->pvt.lfs.headersize = P00_HEADER_SIZE;
  }

  buf->read      = 1;
  buf->cleanup   = lfsop_file_close;
  buf->refill    = lfsop_file_read;
  buf->seek      = lfsop_file_seek;

  stick_buffer(buf);

  /* Call the refill once for the first block of data */
  buf->refill(buf);
}

/**
 * create_file - creates a file
 * @path     : path of the file
 * @dent     : name of the file
 * @type     : type of the file
 * @buf      : buffer to be used
 * @recordlen: length of record, if REL file.
 *
 * This function opens a file in the FAT filesystem for writing and sets up
 * buf to access it. type is ignored here because FAT has no equivalent of
 * file types.
 */
int create_file(path_t *path, cbmdirent_t *dent, uint8_t type, buffer_t *buf, uint8_t recordlen) {
  int res;
  uint8_t *name, *x00ext;

  if (dent->opstype == OPSTYPE_HOST && dent->pvt.host.realname[0] &&
      (dent->typeflags & TYPE_MASK) == type) {
    /* @: over a plain file of the same type: recreated under its real
       name, so its capitals and a hidden .PRG survive. The shown name
       through build_name() came out in small letters (open step 9). */
    ustrcpy(ops_scratch, dent->pvt.host.realname);
    x00ext = NULL;
  } else {
    ustrcpy(ops_scratch, dent->name);
    x00ext = build_name(ops_scratch, type);
  }
  name   = ops_scratch;

  char fullpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
  const int createflags = (recordlen ? LFS_O_RDWR : LFS_O_WRONLY)
                        | LFS_O_CREAT | LFS_O_EXCL;

  lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), name);
  primo_turbo_deep(20);
  res = lfs_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                        buf->pvt.lfs.cache, fullpath, createflags);
  primo_turbo_deep(21);
  while (x00ext != NULL && res == LFS_ERR_EXIST) {
    /* File exists, increment extension */
    *x00ext += 1;
    if (*x00ext == '9'+1) {
      *x00ext = '0';
      *(x00ext-1) += 1;
      if (*(x00ext-1) == '9'+1)
        break;
    }
    lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), name);
    res = lfs_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                          buf->pvt.lfs.cache, fullpath, createflags);
  }
  if (res < 0)
    return res;

  if (x00ext != NULL || recordlen) {
    lfs_size_t byteswritten;

    if(x00ext != NULL) {
      /* Write a [PSUR]00 header */

      memset(ops_scratch, 0, P00_HEADER_SIZE);
      ustrcpy_P(ops_scratch, p00marker);
      memcpy(ops_scratch+P00_CBMNAME_OFFSET, dent->name, CBM_NAME_LENGTH);
      if(recordlen)
        ops_scratch[P00_RECORDLEN_OFFSET] = recordlen;
      buf->pvt.lfs.headersize = P00_HEADER_SIZE;
    } else if(recordlen) {
      ops_scratch[0] = recordlen;
      buf->pvt.lfs.headersize = 1;
    }
    primo_turbo_deep(22);
    byteswritten = lfs_file_write(&lfs, &buf->pvt.lfs.fh, ops_scratch, buf->pvt.lfs.headersize);
    primo_turbo_deep(23);
  res = (byteswritten < 0) ? (int)byteswritten : 0;
    if (res < 0 || byteswritten != buf->pvt.lfs.headersize) {
      return res;
    }
  }

  return 0;
}

/**
 * lfsop_open_write - opens a file for writing
 * @path  : path of the file
 * @dent  : name of the file
 * @type  : type of the file
 * @buf   : buffer to be used
 * @append: Flags if the new data should be appended to the end of file
 *
 * This function opens a file in the FAT filesystem for writing and sets up
 * buf to access it. type is ignored here because FAT has no equivalent of
 * file types.
 */
void lfsop_open_write(path_t *path, cbmdirent_t *dent, uint8_t type, buffer_t *buf, uint8_t append) {
  int res;

  if (append) {
    char fullpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
    lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), dent->pvt.host.realname);
    res = lfs_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                          buf->pvt.lfs.cache, fullpath, LFS_O_WRONLY);
    if (dent->opstype == OPSTYPE_HOST_X00)
      /* It's a [PSUR]00 file */
      buf->pvt.lfs.headersize = P00_HEADER_SIZE;
    if (res >= 0)
      res = lfs_file_seek(&lfs, &buf->pvt.lfs.fh, lfs_file_size(&lfs, &buf->pvt.lfs.fh), LFS_SEEK_SET);
    buf->fptr = lfs_file_size(&lfs, &buf->pvt.lfs.fh) - buf->pvt.lfs.headersize;
  } else
    res = create_file(path, dent, type, buf, 0);

  if (res < 0) {
    parse_error(res,0);
    return;
  }

  mark_write_buffer(buf);
  buf->position  = 2;
  buf->lastused  = 2;
  buf->cleanup   = lfsop_file_close;
  buf->refill    = lfsop_file_write;
  buf->seek      = lfsop_file_seek;

  /* If no data is written the file should end up with a single 0x0d byte */
  buf->data[2] = 13;
}

/**
 * lfsop_open_rel - creates a rel file.
 * @path  : path of the file
 * @dent  : name of the file
 * @buf   : buffer to be used
 * @length: record length
 * @mode  : select between new or existing file
 *
 * This function opens a rel file and prepares it for access.
 * If the mode parameter is 0, create a new file. If it is != 0,
 * open an existing file.
 */
void lfsop_open_rel(path_t *path, cbmdirent_t *dent, buffer_t *buf, uint8_t length, uint8_t mode) {
  int res;
  lfs_size_t bytesread;

  if(!mode) {
    res = create_file(path, dent, TYPE_REL, buf, length);
    bytesread = 1;
    ops_scratch[0] = length;
  } else {
    char fullpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
    lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), dent->pvt.host.realname);
    res = lfs_open_cached(&buf->pvt.lfs.fh, &buf->pvt.lfs.cfg,
                          buf->pvt.lfs.cache, fullpath, LFS_O_RDWR);
    if (res >= 0) {
      if (dent->opstype == OPSTYPE_HOST_X00) {
        res = lfs_file_seek(&lfs, &buf->pvt.lfs.fh, P00_RECORDLEN_OFFSET, LFS_SEEK_SET);
      }
      if(res >= 0)
        /* read record length */
        bytesread = lfs_file_read(&lfs, &buf->pvt.lfs.fh, ops_scratch, 1);
  res = (bytesread < 0) ? (int)bytesread : 0;
      if(!length)
        length = ops_scratch[0];
    }
  }

  if (res < 0 || bytesread != 1) {
    parse_error(res,0);
    return;
  }

  buf->pvt.lfs.headersize = (uint8_t)(uint32_t)lfs_file_tell(&lfs, &buf->pvt.lfs.fh);
  buf->recordlen  = length;
  mark_write_buffer(buf);
  buf->read      = 1;
  buf->cleanup   = lfsop_file_close;
  buf->refill    = lfsop_file_sync;
  buf->seek      = lfsop_file_seek;

  /* read the first record */
  if (!lfsop_file_read(buf) && length != ops_scratch[0])
    set_error(ERROR_RECORD_MISSING);
}

/* ------------------------------------------------------------------------- */
/*  External interface for the various operations                            */
/* ------------------------------------------------------------------------- */

/* Idempotent by design: callers close on every exit path, and some of
   those overlap (a scan that reaches the end is also closed by its
   caller). See fileops_t.closedir. */
void lfsop_closedir(dh_t *dh) {
  if (dh->dir.lfs.open) {
    dh->dir.lfs.open = 0;
    lfs_dir_close(&lfs, &dh->dir.lfs.d);
  }
}

uint8_t lfsop_opendir(dh_t *dh, path_t *path) {
  int res;

  ustrcpy((uint8_t *)dh->dir.lfs.path, (const uint8_t *)lfs_dirpath(path));
  /* Deliberately does NOT close a previously-open handle here. buffer_t's
     pvt is a UNION -- pvt.dir.dh shares storage with pvt.lfs.fh -- so on a
     buffer that last held a file, the `open` flag below is not a flag at
     all, it is a few bytes of a stale lfs_file_t. Testing it would close a
     fabricated handle and corrupt the very list this is here to protect.

     The flag is only ever trustworthy between an opendir and the matching
     closedir on the same handle, so every scan closes explicitly instead;
     see fileops_t.closedir in wrapops.h. */
  res = lfs_dir_open(&lfs, &dh->dir.lfs.d, dh->dir.lfs.path);
  dh->dir.lfs.open = (res >= 0);
  dh->part = path->part;
  if (res < 0) {
    parse_error(res,1);
    return 1;
  }
  return 0;
}

/**
 * lfsop_readdir - readdir wrapper for FAT
 * @dh  : directory handle as set up by opendir
 * @dent: CBM directory entry for returning data
 *
 * This function reads the next directory entry into dent.
 * Returns 1 if an error occured, -1 if there are no more
 * directory entries and 0 if successful.
 */
int8_t lfsop_readdir(dh_t *dh, cbmdirent_t *dent) {
  int res;
  struct lfs_info finfo;
  uint8_t *ptr,*nameptr;
  uint8_t typechar;

  do {
    res = lfs_dir_read(&lfs, &dh->dir.lfs.d, &finfo);
    if (res < 0) {
      if (res == LFS_ERR_CORRUPT)
        set_error(ERROR_DIR_ERROR);
      else
        parse_error(res,1);
      return 1;
    }
    if (res == 0) {
      /* End of directory. fatops.c relied on FatFs zeroing the name here;
         littlefs reports it in the return value instead, so make the rest
         of this function's "empty name means done" logic still hold. */
      finfo.name[0] = 0;
      lfsop_closedir(dh);
      break;
    }
  } while ((finfo.name[0] && (0 /* littlefs has no volume label */)) ||
           (finfo.name[0] == '.' && finfo.name[1] == 0) ||
           (finfo.name[0] == '.' && finfo.name[1] == '.' && finfo.name[2] == 0));

  memset(dent, 0, sizeof(cbmdirent_t));

  if (!finfo.name[0])
    return -1;


  dent->opstype = OPSTYPE_HOST;

  /* Copy name */
  ustrcpy(dent->pvt.host.realname, finfo.name);

  /* littlefs stores a single name -- there is no 8.3-versus-long-name
     split for fatops.c's rule ("convert only LFNs to PETSCII, 8.3 are
     always upper-case") to choose between.

     NO asc2pet() CONVERSION HAPPENS HERE, deliberately. fatops.c's rule
     was "convert only LFNs to PETSCII, 8.3 are always upper-case", and a
     littlefs name is the 8.3 case: already uppercase ASCII, which a C64
     displays correctly as-is. Running asc2pet() over it maps 'A'-'Z' into
     the shifted 0xc1-0xda range, which shows up as reverse-video
     characters -- seen on real hardware 2026-09-04.

     Converting in place here is doubly wrong: check_extension() below
     reads this same buffer looking for ".PRG", so a converted name makes
     every PRG unrecognisable and drops it from the C64's listing while it
     still appears over HTTP. */
  nameptr = (uint8_t *)finfo.name;

  /* File type */
  if ((finfo.type == LFS_TYPE_DIR)) {
    dent->typeflags = TYPE_DIR;
    /* Hide directories starting with . */
    if (*nameptr == '.')
      dent->typeflags |= FLAG_HIDDEN;

  } else {
    /* Search for the file extension */
    exttype_t ext = check_extension(finfo.name, &ptr);
    if (ext == EXT_IS_X00) {
      /* [PSRU]00 file - try to read the internal name */
      uint8_t *name = p00cache_lookup(dh->part, finfo.clust);
      typechar = *ptr;

      if (name != NULL) {
        /* lookup successful */
        memcpy(dent->name, name, CBM_NAME_LENGTH);
      } else {
        /* read name from file */
        lfs_size_t bytesread;

        char p00path[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
        lfs_path_join(p00path, sizeof(p00path), dh->dir.lfs.path,
                      (const uint8_t *)finfo.name);

        res = lfs_open_cached(&partition[dh->part].imagehandle,
                          &partition[dh->part].imagecfg,
                          partition[dh->part].imagecache, p00path, LFS_O_RDONLY);
        if (res < 0)
          goto notp00;

        bytesread = lfs_file_read(&lfs, &partition[dh->part].imagehandle,
                                  ops_scratch, P00_HEADER_SIZE);
        lfs_file_close(&lfs, &partition[dh->part].imagehandle);
        if ((int)bytesread < 0)
          goto notp00;

        if (memcmp_P(ops_scratch, p00marker, P00MARKER_LENGTH))
          goto notp00;

        /* Copy the internal name - dent->name is still zeroed */
        ustrcpy(dent->name, ops_scratch + P00_CBMNAME_OFFSET);

        /* Some programs pad the name with 0xa0 instead of 0 */
        ptr = dent->name;
        for (uint8_t i=0;i<16;i++,ptr++)
          if (*ptr == 0xa0)
            *ptr = 0;

        /* add name to cache */
        p00cache_add(dh->part, finfo.clust, dent->name);
      }
      finfo.size -= P00_HEADER_SIZE;
      dent->opstype = OPSTYPE_HOST_X00;

    } else if (ext == EXT_IS_TYPE && (globalflags & EXTENSION_HIDING)) {
      /* Type extension */
      typechar = *ptr;
      uint8_t i = ustrlen(nameptr)-4;
      nameptr[i] = 0;

    } else { /* ext == EXT_UNKNOWN or EXT_IS_TYPE but hiding disabled */
      /* Unknown extension: PRG */
      typechar = 'P';
    }

  notp00:
    /* Set the file type */
    switch (typechar) {
    case 'P':
      dent->typeflags = TYPE_PRG;
      break;

    case 'S':
      dent->typeflags = TYPE_SEQ;
      break;

    case 'U':
      dent->typeflags = TYPE_USR;
      break;

    case 'R':
      dent->typeflags = TYPE_REL;
      break;
    }
  }

  /* Copy file name into dirent if it fits */
  if (dent->opstype != OPSTYPE_HOST_X00) {
    if (ustrlen(nameptr) > CBM_NAME_LENGTH) {
      ustrcpy(dent->name, finfo.name);
    } else {
      ustrcpy(dent->name, nameptr);
    }

    /* Upper-case for display. dent->name is what the C64 prints in a
       directory listing, and it prints it through the default
       upper-case/graphics character set, where PETSCII 0x41-0x5a are the
       letters A-Z and 0x61-0x7a are graphics symbols. So an ASCII
       lower-case name arrives on screen as line noise: "fb" is 0x66 0x62,
       which draws two graphics blocks. Seen on real hardware 2026-09-05
       with files named "fb", "fb64" and "Commando.d64".

       This did not exist under FatFs because 8.3 short names are always
       upper-case, and the comment further up this function ("a littlefs
       name is the 8.3 case: already uppercase ASCII") assumed that
       carried over. It does not -- littlefs stores exactly the name it
       was given.

       Only the *display* name is folded. pvt.host.realname keeps the true
       name and is what actually opens the file, so "fb" on disk still
       opens when the C64 asks for "FB". */
    ptr = dent->name;
    while (*ptr) {
      if (*ptr == '~')
        *ptr = 0xff;
      else if (*ptr >= 'a' && *ptr <= 'z')
        *ptr -= 32;
      ptr++;
    }
  }

  if (finfo.size > 16255746)
    /* File too large -> size 63999 blocks */
    dent->blocksize = 63999;
  else
    dent->blocksize = (finfo.size+253) / 254;

  dent->remainder = finfo.size % 254;

  /* littlefs has no attribute bits: no read-only, hidden or system flag
     exists to map, so every entry is a plain visible read-write file. */

  /* Cluster number */

  /* littlefs does not store timestamps (they would need custom
     attributes, and this device has no clock to fill them from anyway --
     the FAT build's dates came from FatFs's get_fattime(), which this
     port has no equivalent for). Report a fixed, obviously-not-real date
     rather than uninitialised values, so directory listings stay
     well-formed. */
  dent->date.year   = 80;   /* 1980, the FAT epoch */
  dent->date.month  = 1;
  dent->date.day    = 1;
  dent->date.hour   = 0;
  dent->date.minute = 0;
  dent->date.second = 0;

  return 0;
}

/**
 * lfsop_delete - Delete a file/directory on FAT
 * @path: path to the file/directory
 * @dent: pointer to cbmdirent with name of the file/directory to be deleted
 *
 * This function deletes the file filename in path and returns
 * 0 if not found, 1 if deleted or 255 if an error occured.
 */
uint8_t lfsop_delete(path_t *path, cbmdirent_t *dent) {
  lfsops_freeblocks_invalidate();
  int res;
  uint8_t *name;

  set_dirty_led(1);
  if (dent->pvt.host.realname[0]) {
    name = dent->pvt.host.realname;
    p00cache_invalidate();
  } else {
    name = dent->name;
    pet2asc(name);
  }
  char fullpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
  lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), name);

  if (lfsops_mounted_image(fullpath)) {
    update_leds();
    set_error(ERROR_WRITE_PROTECT);
    return 255;
  }

  res = lfs_remove(&lfs, fullpath);

  update_leds();

  parse_error(res,0);
  if (res >= 0)
    return 1;
  else if (res == LFS_ERR_NOENT)
    /* Was compared against FatFs's FR_NO_FILE (a small positive number)
       while lfs_remove() returns LFS_ERR_NOENT (-2), so "not found" was
       never recognised and SCRATCH of a missing file reported a hard
       error instead of deleting nothing. */
    return 0;
  else
    return 255;
}

/**
 * lfsop_chdir - change directory in FAT and/or mount image
 * @path: path object for the location of dirname
 * @dent: Name of the directory/image to be changed into
 *
 * This function changes the directory of the path object to dirname.
 * If dirname specifies a file with a known extension (e.g. D64), the
 * current(!) directory will be changed to the directory of the file and
 * it will be mounted as an image file. Returns 0 if successful,
 * 1 otherwise.
 */
uint8_t lfsop_chdir(path_t *path, cbmdirent_t *dent) {
  int res;

  /* Left arrow moves one directory up. With paths this is a string
     operation -- truncate at the last separator -- where FatFs had to
     f_stat("..") to recover the parent's cluster number. */
  if (dent->name[0] == '_' && dent->name[1] == 0) {
    char *slash = strrchr(path->dir.lfs.path, '/');
    if (slash == NULL || slash == path->dir.lfs.path) {
      path->dir.lfs.path[0] = '/';
      path->dir.lfs.path[1] = 0;
    } else {
      *slash = 0;
    }
    return 0;
  } else if (dent->name[0] == 0) {
    /* Empty string moves to the root dir */
    path->dir.lfs.path[0] = '/';
    path->dir.lfs.path[1] = 0;
    return 0;
  }

  if ((dent->typeflags & TYPE_MASK) == TYPE_DIR) {
    /* It's a directory: descend into it by extending the path */
    char newpath[LFS_DIRPATH_MAX];
    lfs_path_join(newpath, sizeof(newpath), lfs_dirpath(path),
                  dent->pvt.host.realname[0] ? dent->pvt.host.realname
                                            : dent->name);
    ustrcpy((uint8_t *)path->dir.lfs.path, (const uint8_t *)newpath);
  } else {
    /* Changing into a file, could be a mount request */
    if (check_imageext(dent->pvt.host.realname) != IMG_UNKNOWN) {
      /* Disk-image mount request */
      free_multiple_buffers(FMB_USER_CLEAN);
      /* Open image file */
      char imgpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
      lfs_path_join(imgpath, sizeof(imgpath), lfs_dirpath(path),
                    dent->pvt.host.realname);

      res = lfs_open_cached(&partition[path->part].imagehandle,
                          &partition[path->part].imagecfg,
                          partition[path->part].imagecache, imgpath, LFS_O_RDWR);
      partition[path->part].image_writable = (res >= 0);
      /* Remembered so the file cannot be deleted while it is mounted --
         see lfsops_mounted_image(). */
      snprintf(partition[path->part].imagepath,
               sizeof(partition[path->part].imagepath), "%s", imgpath);

      /* Fall back to read-only. littlefs has no per-file write protection,
         so unlike FatFs's FR_DENIED/FR_WRITE_PROTECTED this only happens
         for a genuinely unwritable volume -- but retrying costs nothing
         and keeps a read-only mount working. */
      if (res < 0)
        res = lfs_open_cached(&partition[path->part].imagehandle,
                          &partition[path->part].imagecfg,
                          partition[path->part].imagecache, imgpath, LFS_O_RDONLY);

      if (res < 0) {
        parse_error(res,1);
        return 1;
      }

      if (d64_mount(path, dent->pvt.host.realname))
        return 1;
      partition[path->part].fop = &d64ops;

      return 0;
    }
  }
  return 0;
}

/* Create a new directory */
void lfsop_mkdir(path_t *path, uint8_t *dirname) {
  lfsops_freeblocks_invalidate();
  int res;

  char fullpath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];

  pet2asc(dirname);
  lfs_path_join(fullpath, sizeof(fullpath), lfs_dirpath(path), dirname);
  res = lfs_mkdir(&lfs, fullpath);
  parse_error(res,0);
}

/**
 * lfsop_getvolumename - Get the volume label
 * @part : partition to request
 * @label: pointer to the buffer for the label (16 characters+zero-termination)
 *
 * This function reads the FAT volume label and stores it zero-terminated
 * in label. Returns 0 if successfull, != 0 if an error occured.
 */
static uint8_t lfsop_getvolumename(uint8_t part, uint8_t *label) {
  lfs_dir_t dh;
  struct lfs_info finfo;
  int res;
  uint8_t i,j;

  memset(label, 0, CBM_NAME_LENGTH+1);
  (void)part;

  /* littlefs has no volume label -- FAT stored one as a directory entry
     with AM_VOL set, and fatops.c scanned the root for it. There is
     nothing to scan for here, so report a fixed name. The C64 shows this
     as the disk name in a directory listing. */
  ustrcpy_P(label, PSTR("PRIFLOP"));
  return 0;
}


/**
 * lfsop_getdirlabel - Get the directory label
 * @path : path object of the directory
 * @label: pointer to the buffer for the label (16 characters)
 *
 * This function reads the FAT volume label (if in root directory) or FAT
 * directory name (if not) and stored it space-padded
 * in the first 16 bytes of label.
 * Returns 0 if successfull, != 0 if an error occured.
 */
uint8_t lfsop_getdirlabel(path_t *path, uint8_t *label) {
  lfs_dir_t dh;
  struct lfs_info finfo;
  int res;
  uint8_t *name = ops_scratch;

  memset(label, ' ', CBM_NAME_LENGTH);

  /* The name of the directory we are in. fatops.c had to reopen the
     parent via ".." and scan it for the entry whose cluster matched, all
     to recover a name FAT does not store in the directory itself. With a
     path it is simply the last component -- and the root has no name, so
     it falls back to the volume name exactly as before. */
  const char *dirpath = lfs_dirpath(path);
  const char *base    = strrchr(dirpath, '/');

  if (base == NULL || base[1] == 0) {
    /* Root directory: use the volume name. It must go through the local
       buffer and be copied in by length, exactly as fatops.c did -- NOT
       passed as `label` directly. getvolumename() begins with
       memset(label, 0, ...), so handing it `label` wipes the space
       padding set above and leaves the disk name NUL-padded instead.
       That produces a malformed header line, and a C64 stops listing
       right there: observed as a lone `1 "PRIFLOP` followed by READY,
       with the entries and footer generated correctly but never shown
       (2026-09-04). */
    lfsop_getvolumename(path->part, name);
  } else {
    /* Copied as it is, NOT through asc2pet(): a littlefs name is shown
       as stored, exactly as lfsop_readdir() shows the entries (see the
       note there). asc2pet() turned the capitals of a folder's name into
       0xc1-0xda, shifted letters on a C64 and question marks on the
       Primo (seen 2026-10-11). */
    base++;   /* skip the separator */
    ustrcpy(name, (const uint8_t *)base);
  }

  /* At most the label's 16 characters: a longer folder name overran it */
  if (*name)
    memcpy(label, name, ustrlen(name) < CBM_NAME_LENGTH ? ustrlen(name) : CBM_NAME_LENGTH);

  return 0;
}

/**
 * lfsop_getid - "Read" a disk id
 * @path: path object
 * @id  : pointer to the buffer for the id (5 characters)
 *
 * This function creates a disk ID from the FAT type (12/16/32)
 * and the usual " 2A" of a 1541 in the first 5 bytes of id.
 * Always returns 0 for success.
 */
uint8_t lfsop_getid(path_t *path, uint8_t *id) {
  (void)path;

  /* fatops.c derived the two ID characters from the FAT type (12/16/32).
     There is no equivalent to report here, so use "LF" for littlefs and
     keep the " 2A" a 1541 always sends -- some software checks that. */
  *id++ = 'L';
  *id++ = 'F';
  *id++ = ' ';
  *id++ = '2';
  *id++ = 'A';

  return 0;
}

/* Returns the number of free blocks */
/* BLOCKS FREE, cached.

   lfs_fs_size() walks the whole filesystem metadata -- FatFs's l_getfree()
   read a cached cluster count, so the port turned a near-free lookup into
   an unbounded traversal. dir_footer() calls this from inside a live C64
   transaction, where there is no timing margin for it.

   So compute it off the transaction path: recomputed from BUS_IDLE via
   lfsops_idle_maintenance(), and marked stale by anything that changes
   the volume. A footer showing a slightly old count is harmless -- it is
   a number on a directory listing, not something correctness rests on --
   whereas a metadata traversal mid-transfer is not. */
/* Cached as *bytes*, not CBM blocks. The CBM footer wants 254-byte
   blocks and the web UI wants a byte count; deriving both from one
   cached number keeps a single traversal behind both of them. */
static uint32_t fb_cache_bytes;
static uint8_t  fb_stale = 1;

void lfsops_freeblocks_invalidate(void) {
  fb_stale = 1;
}

static uint32_t lfsop_freebytes_compute(void);

void lfsops_idle_maintenance(void) {
  if (fb_stale && lfs_mounted) {
    fb_cache_bytes = lfsop_freebytes_compute();
    /* Re-stat the marker on the same trigger as the free-space recompute.
       The only way the marker goes away is someone deleting it, and a
       delete invalidates this cache -- so piggybacking here notices the
       dismissal on the next idle pass without stat()ing on every one. */
    reformat_marker = marker_exists();
    fb_stale = 0;
  }
}

uint16_t lfsop_freeblocks(uint8_t part) {
  (void)part;
  /* Deliberately does NOT recompute when stale: being called at all means
     a transaction is in progress. The idle pass will catch up. */
  uint32_t blocks = fb_cache_bytes / 254;   /* CBM blocks are 254 usable bytes */
  return (blocks > 65535) ? 65535 : (uint16_t)blocks;
}

/* Free space in bytes, for the web UI's Files page. Same cache, same
   rule: this is a read of an already-computed number, never a traversal,
   so it is safe to answer from the fs_bridge pump. */
uint32_t fs_free_bytes(void) {
  return fb_cache_bytes;
}

static uint32_t lfsop_freebytes_compute(void) {
  /* NOTE: this was the last FatFs call hiding in this file. It survived
     the port because the fork spells it l_getfree(), not f_getfree(), so
     a grep for "f_" missed it -- and it was reading an unmounted FATFS,
     producing a meaningless BLOCKS FREE line. */
  lfs_ssize_t used = lfs_fs_size(&lfs);
  if (used < 0)
    return 0;

  const struct lfs_config *cfg = lfs_flash_config();
  if ((lfs_size_t)used > cfg->block_count)
    return 0;

  return (uint32_t)(cfg->block_count - (lfs_size_t)used) * cfg->block_size;
}


/**
 * lfsop_readwrite_sector - simulate direct sector access
 * @buf   : target buffer
 * @part  : partition number
 * @track : track to read
 * @sector: sector to read
 * @rwflag: read/write flag
 *
 * This function allows access to a file called bootsect.128
 * as track 1 sector 0 to enable the auto-boot function of
 * the C128 on FAT directories. If rwflag is false (0),
 * the sector will be written; otherwise it will be read.
 */
static void lfsop_readwrite_sector(buffer_t *buf, uint8_t part,
                                 uint8_t track, uint8_t sector, uint8_t rwflag) {
  int res;
  lfs_size_t bytes;
  /* int, not uint8_t: LFS_O_WRONLY|LFS_O_CREAT is 258, which truncates to
     2 in a byte -- silently dropping O_CREAT, so writing the boot sector
     to a file that does not exist yet failed instead of creating it.
     Caught by -Woverflow once the build stopped hiding warnings. */
  int mode;

  if (track != 1 || sector != 0) {
    set_error_ts(ERROR_READ_NOHEADER, track, sector);
    return;
  }

  if (rwflag)
    mode = LFS_O_RDONLY;
  else
    mode = LFS_O_WRONLY | LFS_O_CREAT;

  /* the imagehandle is unused while no image is mounted (saves a
     noticable chunk of stack compared to a local variable) */
  res = lfs_open_cached(&partition[part].imagehandle,
                          &partition[part].imagecfg,
                          partition[part].imagecache, BOOTSECTOR_FILE, mode);
  if (res < 0) {
    parse_error(res, rwflag);
    return;
  }

  if (rwflag) {
    bytes = lfs_file_read(&lfs, &partition[part].imagehandle, buf->data, 256);
  } else {
    bytes = lfs_file_write(&lfs, &partition[part].imagehandle, buf->data, 256);
  }
  res = ((int)bytes < 0) ? (int)bytes : 0;

  if (res < 0 || bytes != 256)
    parse_error(res, rwflag);

  res = lfs_file_close(&lfs, &partition[part].imagehandle);
  if (res < 0)
    parse_error(res, rwflag);

  return;
}

/**
 * lfsop_read_sector - simulate direct sector reads
 * @buf   : target buffer
 * @part  : partition number
 * @track : track to read
 * @sector: sector to read
 *
 * Wrapper for lfsop_readwrite_sector in read mode
 */
void lfsop_read_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector) {
  lfsop_readwrite_sector(buf, part, track, sector, 1);
}

/**
 * lfsop_write_sector - simulate direct sector writes
 * @buf   : source buffer
 * @part  : partition number
 * @track : track to write
 * @sector: sector to write
 *
 * Wrapper for lfsop_readwrite_sector in write mode
 */
void lfsop_write_sector(buffer_t *buf, uint8_t part, uint8_t track, uint8_t sector) {
  lfsop_readwrite_sector(buf, part, track, sector, 0);
}


/**
 * lfsop_rename - rename a file
 * @path   : path object
 * @dent   : pointer to cbmdirent with old file name
 * @newname: new file name
 *
 * This function renames the file in dent in the directory referenced by
 * path to newname.
 */
void lfsop_rename(path_t *path, cbmdirent_t *dent, uint8_t *newname) {
  uint8_t *ext;
  int res;
  lfs_size_t byteswritten;
  char frompath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];
  char topath[LFS_DIRPATH_MAX + LFS_REALNAME_MAX];

  if (dent->opstype == OPSTYPE_HOST_X00) {
    /* [PSUR]00 rename, just change the internal file name */
    p00cache_invalidate();

    lfs_path_join(frompath, sizeof(frompath), lfs_dirpath(path),
                  dent->pvt.host.realname);
    res = lfs_open_cached(&partition[path->part].imagehandle,
                          &partition[path->part].imagecfg,
                          partition[path->part].imagecache, frompath, LFS_O_RDWR);
    if (res < 0) {
      parse_error(res,0);
      return;
    }

    res = (int)lfs_file_seek(&lfs, &partition[path->part].imagehandle,
                             P00_CBMNAME_OFFSET, LFS_SEEK_SET);
    if (res < 0) {
      parse_error(res,0);
      return;
    }

    /* Copy the new name into dent->name so we can overwrite all 16 bytes */
    memset(dent->name, 0, CBM_NAME_LENGTH);
    ustrcpy(dent->name, newname);

    byteswritten = lfs_file_write(&lfs, &partition[path->part].imagehandle,
                                  dent->name, CBM_NAME_LENGTH);
    res = ((int)byteswritten < 0) ? (int)byteswritten : 0;
    if (res < 0 || byteswritten != CBM_NAME_LENGTH) {
      parse_error(res,0);
      return;
    }

    res = lfs_file_close(&lfs, &partition[path->part].imagehandle);
    if (res < 0) {
      parse_error(res,0);
      return;
    }
  } else {
    switch (check_extension(dent->pvt.host.realname, &ext)) {
    case EXT_IS_TYPE:
      /* Keep type extension */
      ustrcpy(ops_scratch, newname);
      build_name(ops_scratch, dent->typeflags & TYPE_MASK);
      lfs_path_join(frompath, sizeof(frompath), lfs_dirpath(path), dent->pvt.host.realname);
      lfs_path_join(topath,   sizeof(topath),   lfs_dirpath(path), ops_scratch);
      res = lfs_rename(&lfs, frompath, topath);
      if (res < 0)
        parse_error(res, 0);
      break;

    default:
      /* Normal rename. The source is the real name, as open and delete
         use it: the shown name through pet2asc is in small letters and
         missed a file whose real name has capitals (open step 9). */
      pet2asc(dent->name);
      pet2asc(newname);
      lfs_path_join(frompath, sizeof(frompath), lfs_dirpath(path),
                    dent->pvt.host.realname[0] ? dent->pvt.host.realname : dent->name);
      lfs_path_join(topath,   sizeof(topath),   lfs_dirpath(path), newname);
      res = lfs_rename(&lfs, frompath, topath);
      if (res < 0)
        parse_error(res, 0);
      break;
    }
  }
}

/**
 * lfsops_init - Initialize fatops module
 * @preserve_path: Preserve the current directory if non-zero
 *
 * This function will initialize the fatops module and force
 * mounting of the card. It can safely be called again if re-mounting
 * is required.
 */
void lfsops_init(uint8_t preserve_path) {
  int res;

  /* fatops.c looped over drives and partitions here. littlefs has neither:
     one volume fills the whole FLASH_ATA region, so the loop is gone
     rather than kept and made to exit after one pass.

     Two bugs came out with it. The partition-advance test compared res
     against FR_NOT_READY / FR_INVALID_OBJECT -- FatFs codes, which are
     small positive numbers, while every littlefs error is negative, so
     the test could never be false and a failed mount rescanned fifteen
     nonexistent partitions. And the preserve_path guard below was missing
     its braces, so the second line ran unconditionally and reset the
     current directory to "/" on every re-init even when the caller had
     asked for it to be preserved. */
  max_part = 0;
  partition[0].fop = &lfsops;

  res = lfs_mounted ? 0 : lfs_mount(&lfs, lfs_flash_config());
  if (res < 0) {
    /* An unformatted or corrupt volume: format and mount, which is what
       a fresh device needs and what recovery from a bad flash needs.
       FatFs had no equivalent -- a broken volume simply failed to mount
       and had to be rebuilt from the host.

       Say so on the console. This destroys every file on the volume,
       including the Wi-Fi credentials in PRIFLOP.CFG, and a silent wipe
       is indistinguishable from "the config mysteriously stopped
       working" -- which cost real time chasing on 2026-09-05 when an
       unrelated AP fallback looked exactly like it. */
    uart_puts_P(PSTR("\r\nlfs: mount failed, REFORMATTING (all files lost)\r\n"));
    res = lfs_format(&lfs, lfs_flash_config());
    if (res >= 0)
      res = lfs_mount(&lfs, lfs_flash_config());
    /* Leave evidence. The console line above is the only trace this event
       used to leave, and nothing is attached to the console in normal use
       -- so the wipe was effectively silent and turned up later as "my
       files are gone and the drive is in AP mode again", with no way to
       tell whether it had reformatted or something else had gone wrong.
       The marker is written below, once the volume is mountable. */
    reformat_marker_pending = (res >= 0);
  }

  if (res >= 0) {
    lfs_mounted = 1;
    max_part = 1;
    if (reformat_marker_pending) {
      reformat_marker_pending = 0;
      write_reformat_marker();
    }
    reformat_marker = marker_exists();
    /* Prime the BLOCKS FREE cache while the bus is certainly idle, so the
       first directory listing does not have to show a zero. */
    lfsops_idle_maintenance();
  }

  if (!preserve_path) {
    partition[0].current_dir.lfs.path[0] = '/';
    partition[0].current_dir.lfs.path[1] = 0;
  }

  if (!preserve_path) {
    current_part = 0;
    display_current_part(0);
    set_changelist(NULL, NULLSTRING);
    previous_file_dirent.name[0] = 0; // clear '*' file
  }

  /* Invalidate some caches */
  d64_invalidate();
  p00cache_invalidate();

#ifndef HAVE_HOTPLUG
  if (!max_part) {
    set_error_ts(ERROR_DRIVE_NOT_READY,0,0);
    return;
  }
#endif
}

/**
 * image_unmount - generic unmounting function for images
 * @part: partition number
 *
 * This function will clear all buffers, close the image file and
 * restore file operations to fatops. It can be used for unmounting
 * any image file types that don't require special cleanups.
 * Returns 0 if successful, 1 otherwise.
 */
uint8_t image_unmount(uint8_t part) {
  int res;

  free_multiple_buffers(FMB_USER_CLEAN);

  /* call D64 unmount function to handle BAM refcounting etc. */
  // FIXME: ops entry?
  if (partition[part].fop == &d64ops)
    d64_unmount(part);

  if (display_found) {
    /* Send current path to display */
    path_t path;

    path.part    = part;
    path.dir = partition[part].current_dir;
    lfsop_getdirlabel(&path, ops_scratch);
    display_current_directory(part, ops_scratch);
  }

  partition[part].fop = &lfsops;
  partition[part].imagepath[0] = 0;
  res = lfs_file_close(&lfs, &partition[part].imagehandle);
  if (res < 0) {
    /* Report it, but the unmount still happened: fop is already back to
       lfsops above, and lfs_file_close() unlinks the handle whatever its
       flush returns. Returning failure here made do_chdir() bail before
       update_current_dir(), so a drive whose flush failed was left
       half-transitioned -- seen as "CD:_ -> 72 DISK FULL" on a full
       volume, where the flush had nowhere to write. An unmount needs no
       free space and should not be undone by that. */
    parse_error(res,0);
  }
  return 0;
}

/**
 * image_chdir - generic chdir for image files
 * @path: path object of the location of dirname
 * @dent: directory to be changed into
 *
 * This function will ignore any names except _ (left arrow)
 * and unmount the image if that is found. It can be used as
 * chdir for all image types that don't support subdirectories
 * themselves. Returns 0 if successful, 1 otherwise.
 */
uint8_t image_chdir(path_t *path, cbmdirent_t *dent) {
  if (dent->name[0] == '_' && dent->name[1] == 0) {
    /* Unmount request */
    return image_unmount(path->part);
  }
  return 1;
}

/**
 * image_mkdir - generic mkdir for image files
 * @path   : path of the directory
 * @dirname: name of the directory to be created
 *
 * This function does nothing.
 */
void image_mkdir(path_t *path, uint8_t *dirname) {
  set_error(ERROR_SYNTAX_UNABLE);
  return;
}

/**
 * image_read - Seek to a specified image offset and read data
 * @part  : partition number
 * @offset: offset to be seeked to
 * @buffer: pointer to where the data should be read to
 * @bytes : number of bytes to read from the image file
 *
 * This function seeks to offset in the image file and reads bytes
 * byte into buffer. It returns 0 on success, 1 if less than
 * bytes byte could be read and 2 on failure.
 */
uint8_t image_read(uint8_t part, uint32_t offset, void *buffer, uint16_t bytes) {
  system_watchdog_pet();
  int res;
  lfs_size_t bytesread;

  if (offset != -1) {
    res = (int)lfs_file_seek(&lfs, &partition[part].imagehandle, offset, LFS_SEEK_SET);
    if (res < 0) {
      parse_error(res,1);
      return 2;
    }
  }

  bytesread = lfs_file_read(&lfs, &partition[part].imagehandle, buffer, bytes);
  res = ((int)bytesread < 0) ? (int)bytesread : 0;
  if (res < 0) {
    parse_error(res,1);
    return 2;
  }

  if (bytesread != bytes)
    return 1;

  return 0;
}

/**
 * image_write - Seek to a specified image offset and write data
 * @part  : partition number
 * @offset: offset to be seeked to
 * @buffer: pointer to the data to be written
 * @bytes : number of bytes to read from the image file
 * @flush : Flags if written data should be flushed to disk immediately
 *
 * This function seeks to offset in the image file and writes bytes
 * byte into buffer. It returns 0 on success, 1 if less than
 * bytes byte could be written and 2 on failure.
 */
uint8_t image_write(uint8_t part, uint32_t offset, void *buffer, uint16_t bytes, uint8_t flush) {
  system_watchdog_pet();
  int res;
  lfs_size_t byteswritten;

  if (offset != -1) {
    res = (int)lfs_file_seek(&lfs, &partition[part].imagehandle, offset, LFS_SEEK_SET);
    if (res < 0) {
      parse_error(res,0);
      return 2;
    }
  }

  byteswritten = lfs_file_write(&lfs, &partition[part].imagehandle, buffer, bytes);
  res = ((int)byteswritten < 0) ? (int)byteswritten : 0;
  if (res < 0) {
    parse_error(res,1);
    return 2;
  }

  if (byteswritten != bytes)
    return 1;

  if (flush)
    /* Was f_sync() on partition[].imagehandle -- a FatFs handle this
       backend never opens, so the flush silently did nothing. Same family
       as the l_getfree() that survived the port: a grep for "lfs_" cannot
       find a call that starts with "f_". */
    lfs_file_sync(&lfs, &partition[part].imagehandle);

  return 0;
}

/**
 * image_size - size in bytes of the mounted image file
 * @part: partition number
 *
 * d64ops selects the image geometry from this. It read
 * partition[].imagehandle.fsize directly while that was a FatFs FIL;
 * under littlefs that handle was never opened, so the size came back 0
 * and every image failed to identify.
 */
/**
 * fs_total_blocks - size of the whole host volume in 512-byte blocks
 *
 * doscmd.c's G-P reply used (max_clust - 1) * csize off the FATFS struct.
 */
uint32_t fs_total_blocks(void) {
  const struct lfs_config *cfg = lfs_flash_config();
  return ((uint64_t)cfg->block_count * cfg->block_size) / 512;
}

/* Whole-volume size in bytes, the denominator for the Files page's
   "x free of y". Always exact -- unlike free space it needs no
   traversal, it is just the geometry. */
uint32_t fs_total_bytes(void) {
  const struct lfs_config *cfg = lfs_flash_config();
  return (uint32_t)cfg->block_count * cfg->block_size;
}

uint32_t image_size(uint8_t part) {
  lfs_ssize_t sz = lfs_file_size(&lfs, &partition[part].imagehandle);
  return (sz < 0) ? 0 : (uint32_t)sz;
}

/**
 * image_writable - can the mounted image be written to?
 * @part: partition number
 *
 * Also read straight off the FatFs handle before (FA_WRITE in .flag),
 * which under littlefs was always clear -- so every write to a mounted
 * image returned WRITE PROTECT.
 */
uint8_t image_writable(uint8_t part) {
  return partition[part].image_writable;
}

/* Dummy function for format */
void format_dummy(uint8_t drive, uint8_t *name, uint8_t *id) {
  set_error(ERROR_SYNTAX_UNKNOWN);
}

const PROGMEM fileops_t lfsops = {  // These should be at bottom, to be consistent with d64ops
  &lfsop_open_read,
  &lfsop_open_write,
  &lfsop_open_rel,
  &lfsop_delete,
  &lfsop_getvolumename,
  &lfsop_getdirlabel,
  &lfsop_getid,
  &lfsop_freeblocks,
  &lfsop_read_sector,
  &lfsop_write_sector,
  &format_dummy,
  &lfsop_opendir,
  &lfsop_readdir,
  &lfsop_closedir,
  &lfsop_mkdir,
  &lfsop_chdir,
  &lfsop_rename
};
