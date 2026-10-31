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


   diskchange.c: Disk image changer

*/

#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "buffers.h"
#include "display.h"
#include "doscmd.h"
#include "errormsg.h"
#include "lfsops.h"
#include "flags.h"
#include "hostfs.h"
#include "led.h"
#include "parser.h"
#include "progmem.h"
#include "timer.h"
#include "utils.h"
#include "ustring.h"
#include "diskchange.h"

_Static_assert(CONFIG_COMMAND_BUFFER_SIZE < 256, "command buffer too large");

#define MAX_LINE_LEN (CONFIG_COMMAND_BUFFER_SIZE - 1)

static const char PROGMEM autoswap_lst_name[] = "AUTOSWAP.LST";
static const char PROGMEM autoswap_gen_name[] = "AUTOSWAP.GEN"; // FIXME: must be 15 chars or less
static const char PROGMEM petscii_marker[8]   = "#PETSCII";

static hostfile_t swaplist;
/* FatFs signalled "handle in use" via swaplist.fs != NULL. hostfile_t is
   opaque and has no such field, so track it explicitly -- which is
   clearer anyway than reading a filesystem internal. */
static uint8_t    swaplist_open;
static path_t  swappath;
static uint8_t linenum;

#define BLINK_BACKWARD 1
#define BLINK_FORWARD  2
#define BLINK_HOME     3

static void confirm_blink(uint8_t type) {
  uint8_t i;

  for (i=0;i<2;i++) {
    tick_t targettime;

#ifdef SINGLE_LED
    set_dirty_led(1);
#else
    if (!i || type & 1)
      set_dirty_led(1);
    if (!i || type & 2)
      set_busy_led(1);
#endif
    targettime = ticks + MS_TO_TICKS(100);
    while (time_before(ticks,targettime)) ;

    set_dirty_led(0);
    set_busy_led(0);
    targettime = ticks + MS_TO_TICKS(100);
    while (time_before(ticks,targettime)) ;
  }
}

static bool mount_line(void) {
  uint16_t bytesread;
  uint8_t *buffer_start;
  uint16_t curpos; /* offset of current line in swaplist */
  bool got_colon = false;
  bool seen_nonwhite = false;
  bool is_comment = false;
  uint8_t olderror = current_error;
  current_error = ERROR_OK;

  /* Kill all buffers */
  free_multiple_buffers(FMB_USER_CLEAN);

  /* allocate work area */
  buffer_t* readbuf = alloc_buffer();

  if (!readbuf) {
    return false;
  }

  curpos = 0;
  buffer_start = readbuf->data;
  globalflags |= SWAPLIST_ASCII;
  buffer_start[MAX_LINE_LEN] = 0;

  uint8_t effective_line = 0;
  while (effective_line <= linenum) {
    uint8_t* strend = NULL;
    uint8_t* str = buffer_start;

    if (hostfs_seek(&swaplist, curpos)) {
      parse_error(ERROR_READ_NOHEADER, 1);
      return false;
    }

    if (hostfs_read(&swaplist, str, MAX_LINE_LEN, &bytesread)) {
      parse_error(ERROR_READ_NOHEADER, 1);
      return false;
    }

    /* Terminate string in buffer */
    if (bytesread < MAX_LINE_LEN)
      str[bytesread] = 0;

    if (bytesread == 0) {
      if (linenum == 255) {
        /* Last entry requested, found it */
        linenum = effective_line - 1;
      } else {
        /* End of file - restart loop to read the first entry */
        linenum = 0;
      }
      effective_line = 0;
      curpos = 0;
      continue;
    }

    /* parse line */
    is_comment = false;
    got_colon = false;
    seen_nonwhite = false;

    while (*str && *str != '\r' && *str != '\n') {
      if (*str == ':')
        got_colon = true;

      if (*str == ';' && !seen_nonwhite) {
        is_comment = true;
      }

      if (*str != ' ' && *str != '\t') {
        seen_nonwhite = true;
      }

      str++;
    }

    strend = str;

    /* Skip line terminator */
    while (*str == '\r' || *str == '\n') str++;

    /* check for PETSCII marker */
    if (curpos == 0) {
      if (!memcmp_P(buffer_start, petscii_marker, sizeof(petscii_marker))) {
        /* swaplist is in PETSCII, ignore this line */
        globalflags &= ~SWAPLIST_ASCII;
        is_comment = true;
      }
    }

    curpos += str - buffer_start;

    if (!is_comment) {
      /* an actual entry, copy it in case it is the last one in the file */
      effective_line += 1;
      *strend = 0;
      memcpy(command_buffer + 1, buffer_start, strend - buffer_start + 1);
    }
  }

  if (partition[swappath.part].fop != &lfsops)
    image_unmount(swappath.part);

  /* Start in the partition+directory of the swap list */
  current_part = swappath.part;
  display_current_part(current_part);
  partition[current_part].current_dir = swappath.dir;

  /* add a colon if neccessary */
  buffer_start = command_buffer + 1;
  if (!got_colon && *buffer_start != '/') {
    command_buffer[0] = ':';
    buffer_start--;
  }

  /* recode entry if neccessary */
  if (globalflags & SWAPLIST_ASCII)
    asc2pet(buffer_start);

  /* parse and change */
  do_chdir(buffer_start);

  if (current_error != 0 && current_error != ERROR_DOSVERSION) {
    current_error = olderror;
    return false;
  }

  return true;
}

/**
 * create_changelist - create a swap list in a directory
 * @path    : path where the swap list should be created
 * @filename: name of the swap list file
 *
 * This function creates a swap list in @path by scanning that
 * directory and writing the names of all disk images it finds
 * into @filename. Returns nonzero if at least one image was found.
 */
static uint8_t create_changelist(path_t *path, uint8_t *filename) {
  hostdir_t  dh;
  hostfile_t fh;
  hostinfo_t info;
  uint16_t   byteswritten;
  uint8_t    found = 0;

  /* open directory */
  if (hostfs_opendir(&dh, path->part, &path->dir))
    return 0;

  /* open file */
  if (hostfs_open(&fh, path->part, &path->dir, filename, HOSTFS_CREATE)) {
    hostfs_closedir(&dh);   /* opened above; do not leak it on this exit */
    return 0;
  }

  /* scan directory */
  set_busy_led(1);

  while (1) {
    if (hostfs_readdir(&dh, &info))
      break;

    if (info.name[0] == 0)
      break;

    if (!info.is_dir && check_imageext(info.name) == IMG_IS_DISK) {
      /* write the name of disk image to file */
      found = 1;

      if (hostfs_write(&fh, info.name, ustrlen(info.name), &byteswritten))
        break;

      /* add line terminator */
      static const uint8_t crlf[2] = { 0x0d, 0x0a };
      if (hostfs_write(&fh, crlf, 2, &byteswritten))
        break;
    }
  }

  hostfs_close(&fh);
  hostfs_closedir(&dh);

  set_busy_led(0);

  return found;
}

static void set_changelist_internal(path_t *path, uint8_t *filename, uint8_t at_end) {

  /* Assume this isn't the auto-swap list */
  globalflags &= (uint8_t)~AUTOSWAP_ACTIVE;

  /* Remove the old swaplist */
  if (swaplist_open) {
    hostfs_close(&swaplist);
    memset(&swaplist,0,sizeof(swaplist));
  swaplist_open = 0;
    swaplist_open = 0;
  }

  if (ustrlen(filename) == 0)
    return;

  /* Open a new swaplist */
  if (hostfs_open(&swaplist, path->part, &path->dir, filename, HOSTFS_READ)) {
    parse_error(ERROR_FILE_NOT_FOUND,1);
    return;
  }
  swaplist_open = 1;

  /* Remember its directory so relative paths work */
  swappath = *path;

  if (at_end)
    linenum = 255;
  else
    linenum = 0;

  if (mount_line())
    confirm_blink(BLINK_HOME);
}

void set_changelist(path_t *path, uint8_t *filename) {
  set_changelist_internal(path, filename, 0);
}

void change_disk(void) {
  path_t path;

  if (!swaplist_open) {
    /* No swaplist active, try using AUTOSWAP.LST */
    /* change_disk is called from the IEC idle loop, so ops_scratch is free */
    ustrcpy_P(ops_scratch, autoswap_lst_name);
    path.dir  = partition[current_part].current_dir;
    path.part = current_part;
    if (key_pressed(KEY_PREV))
      set_changelist_internal(&path, ops_scratch, 1);
    else
      set_changelist_internal(&path, ops_scratch, 0);

    if (!swaplist_open) {
      /* No swap list found, create one if key was "home" */
      if (key_pressed(KEY_HOME)) {
        uint8_t swapname[16]; // FIXME: magic constant

        ustrcpy_P(swapname, autoswap_gen_name);

        if (create_changelist(&path, swapname)) {
          set_changelist_internal(&path, swapname, 0);
          globalflags |= AUTOSWAP_ACTIVE;
        }
      }

      /* reset error and exit */
      set_error(ERROR_OK);
      reset_key(0xff); // <- lazy
      return;
    } else {
      /* Autoswaplist found, mark it as active                */
      /* and exit because the first image is already mounted. */
      globalflags |= AUTOSWAP_ACTIVE;
      reset_key(0xff); // <- lazy
      return;
    }
  }

  /* Mount the next image in the list */
  if (key_pressed(KEY_NEXT)) {
    linenum++;
    reset_key(KEY_NEXT);
    if (mount_line())
      confirm_blink(BLINK_FORWARD);
  } else if (key_pressed(KEY_PREV)) {
    linenum--;
    reset_key(KEY_PREV);
    if (mount_line())
      confirm_blink(BLINK_BACKWARD);
  } else if (key_pressed(KEY_HOME)) {
    linenum = 0;
    reset_key(KEY_HOME);
    if (mount_line())
      confirm_blink(BLINK_HOME);
  }
}

void change_init(void) {
  memset(&swaplist,0,sizeof(swaplist));
  swaplist_open = 0;
  globalflags &= (uint8_t)~AUTOSWAP_ACTIVE;
}
