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

/* fs_bridge.c: see fs_bridge.h for the design. This file's functions
   run on both cores -- fs_bridge_try_submit()/fs_bridge_try_take_response()
   from core 1 (httpd.c), fs_bridge_poll() from core 0 (src/iec.c's
   BUS_IDLE, alongside disk_idle_sync()). The FatFs-touching logic in
   fs_bridge_poll() below is ported from httpd.c's old handle_list()/
   handle_mkdir()/handle_rename()/handle_delete() (which used to run
   inline on whichever core called them, back when that was always core
   0) -- see this codebase's git history for the pre-bridge originals.

   NOT using pico_multicore's hardware FIFO as the doorbell (2026-09-02,
   found on real hardware: every request just hung forever) --
   flash_safe_execute_core_init() (system.c/core1.c, called on both
   cores for the unrelated cross-core flash-safety work, backlog item
   4/8) calls multicore_lockout_victim_init() internally, which installs
   an IRQ handler on *both* cores that claims the entire FIFO for its
   own lockout handshake. Any word this file pushed was getting silently
   consumed by that handler before fs_bridge_poll()/
   fs_bridge_try_take_response()'s own multicore_fifo_rvalid() checks
   ever saw it. Since both sides already poll cooperatively and
   frequently (core 0 from iec.c's BUS_IDLE loop, core 1 from its own
   tight loop in core1.c), a plain shared state flag needs no interrupt-
   driven doorbell at all -- each side only ever transitions the state
   *out of* the value it's watching for, so there's no window where both
   cores write it at once. */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "lfs.h"
#include "lfsops.h"
#include "ramops.h"
#include "parser.h"
#include "diskio.h"

#include "pico/time.h"
#include <strings.h>
#include "netconfig.h"
#include "fs_bridge.h"
#include "health.h"

void system_watchdog_pet(void);

static fs_request_t shared_req;
static volatile enum { BRIDGE_IDLE, BRIDGE_REQUEST_PENDING, BRIDGE_RESPONSE_READY } bridge_state = BRIDGE_IDLE;

/* Set for a request whose sender will never collect the reply, so
   fs_bridge_poll() recycles the slot itself instead of parking it in
   BRIDGE_RESPONSE_READY forever. See fs_bridge_submit_discard(). */
static volatile bool discard_response = false;

/* Who submitted the in-flight request. Written by core 1 (the only
   submitter), read by core 1; core 0 never touches it. */
static const void * volatile slot_owner = NULL;

/* When core 1 posted the request in the slot, for health.fsb_wait_max_ms:
   how long a web request waits for core 0 to get round to it. Written by
   core 1 before it marks the slot pending, read by core 0 after it sees
   that -- the same handshake as shared_req itself. */
static volatile uint32_t submit_ms;

bool fs_bridge_try_submit(const fs_request_t *req, const void *owner) {
  /* Reclaim a reply whose owner abandoned it after core 0 had already
     finished -- the one race fs_bridge_abandon() cannot close by itself.
     Doing it here means the next submitter always cleans up. */
  if (bridge_state == BRIDGE_RESPONSE_READY && discard_response) {
    discard_response = false;
    slot_owner = NULL;
    bridge_state = BRIDGE_IDLE;
  }
  if (bridge_state != BRIDGE_IDLE) {
    return false;
  }
  discard_response = false;
  slot_owner = owner;
  submit_ms = to_ms_since_boot(get_absolute_time());
  shared_req = *req; /* whole-struct copy -- simplest way to carry
                         whichever fields this op actually needs (path,
                         path2, handle, and for FS_OP_WRITE, data/
                         data_len too) without enumerating them here */
  bridge_state = BRIDGE_REQUEST_PENDING;
  return true;
}

bool fs_bridge_submit_discard(const fs_request_t *req) {
  if (bridge_state != BRIDGE_IDLE) {
    return false;
  }
  submit_ms = to_ms_since_boot(get_absolute_time());
  shared_req = *req;
  slot_owner = NULL;
  discard_response = true;
  bridge_state = BRIDGE_REQUEST_PENDING;
  return true;
}

bool fs_bridge_try_take_response(fs_request_t *req, const void *owner) {
  if (bridge_state != BRIDGE_RESPONSE_READY) {
    return false;
  }
  if (slot_owner != owner) {
    return false;   /* somebody else's reply -- never steal it */
  }
  *req = shared_req;
  slot_owner = NULL;
  bridge_state = BRIDGE_IDLE;
  return true;
}

void fs_bridge_abandon(const void *owner) {
  if (slot_owner != owner) {
    return;
  }
  if (bridge_state == BRIDGE_RESPONSE_READY) {
    /* Already finished: just drop it. */
    slot_owner = NULL;
    discard_response = false;
    bridge_state = BRIDGE_IDLE;
    return;
  }
  /* Still being serviced by core 0: let fs_bridge_poll() recycle it when
     it completes, rather than yanking the slot out from under it. */
  slot_owner = NULL;
  discard_response = true;
}

/* Resolves the current request's target volume. shared_req.volume ==
   FS_VOLUME_RAM only reaches here past the fs_bridge_poll() guard above
   that already confirmed ramops_available(), so this never needs to
   handle a missing RAM volume itself. */
static lfs_t *req_volume(void) {
  return shared_req.volume == FS_VOLUME_RAM ? ramops_volume() : lfsops_volume();
}

/* Ported from httpd.c's resolve_dir_cluster() -- see its original
   comment (still in httpd.c's git history) for why root is special-
   cased rather than f_stat()'d, and why this walks by cluster number
   rather than path string (this FatFs fork has f_opendir() compiled
   out). */
/* littlefs opens directories by path, so the cluster indirection FatFs
   needed here is gone entirely -- see lfsops.c's header. Kept as a
   validity check so a bad path still reports NOT_FOUND rather than
   failing later in the listing. */
static int resolve_dir(const char *path) {
  struct lfs_info info;

  if (!strcmp(path, "/"))
    return 0;

  if (lfs_stat(req_volume(), path, &info) < 0)
    return -1;
  if (info.type != LFS_TYPE_DIR)
    return -1;
  return 0;
}

static void do_list(void) {
  if (resolve_dir(shared_req.path) < 0) {
    shared_req.result = FS_RESULT_NOT_FOUND;
    return;
  }

  lfs_dir_t dh;
  if (lfs_dir_open(req_volume(), &dh, shared_req.path) < 0) {
    shared_req.result = FS_RESULT_NOT_FOUND;
    return;
  }

  struct lfs_info info;
  char *json = shared_req.data;
  int cap = (int)sizeof(shared_req.data);
  int pos = snprintf(json, (size_t)cap, "{\"path\":\"%s\",\"entries\":[", shared_req.path);
  bool first = true;

  /* Paging, added 2026-09-05. This used to `break` when the buffer filled
     and say nothing about it, so a directory larger than ~4KB of JSON
     just lost its tail -- with the UI showing a short list that looked
     complete. Now the caller is told where to resume. */
  uint32_t skip = shared_req.offset;
  uint32_t index = 0;
  uint32_t next = 0;

  while (lfs_dir_read(req_volume(), &dh, &info) > 0) {
    if (info.name[0] == '.') continue;   /* "." and ".." */

    if (index++ < skip) continue;

    if (pos > cap - 220) {   /* 160 for one entry + room for the free/total tail */
      /* Out of room: resume from this entry on the next request. */
      next = index - 1;
      break;
    }

    /* "protected" marks what httpd.c's path_is_protected() will refuse to
       serve, so the page can show it as held rather than offering a
       download link that would 403. Only the config file qualifies. */
    bool prot = (strcasecmp(info.name, NETCONFIG_PATH + 1) == 0) &&
                (strcmp(shared_req.path, "/") == 0);

    pos += snprintf(json + pos, (size_t)(cap - pos),
                     "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lu%s}",
                     first ? "" : ",", info.name,
                     (info.type == LFS_TYPE_DIR) ? "true" : "false",
                     (unsigned long)info.size,
                     prot ? ",\"protected\":true" : "");
    first = false;
  }
  lfs_dir_close(req_volume(), &dh);

  /* Free/total ride along with every listing rather than getting their
     own endpoint: the page wants them exactly when it draws a directory,
     and both are reads of numbers already computed (fs_free_bytes() is a
     cache lookup, fs_total_bytes() is geometry), so this costs nothing
     the listing was not paying anyway. Repeated on each page of a paged
     listing, which is harmless -- the UI just keeps the last one. */
  uint32_t free_b = (shared_req.volume == FS_VOLUME_RAM)
                        ? ramops_free_bytes() : fs_free_bytes();
  uint32_t total_b = (shared_req.volume == FS_VOLUME_RAM)
                        ? ramops_total_bytes() : fs_total_bytes();
  if (next)
    pos += snprintf(json + pos, (size_t)(cap - pos),
                    "],\"next\":%lu,\"free\":%lu,\"total\":%lu}",
                    (unsigned long)next,
                    (unsigned long)free_b,
                    (unsigned long)total_b);
  else
    pos += snprintf(json + pos, (size_t)(cap - pos),
                    "],\"free\":%lu,\"total\":%lu}",
                    (unsigned long)free_b,
                    (unsigned long)total_b);

  shared_req.offset = next;
  shared_req.data_len = (uint32_t)pos;
  shared_req.result = FS_RESULT_OK;
}

/* Create-or-truncate, write, close -- see FS_OP_WRITE_FILE in fs_bridge.h. */
static void do_write_file(void) {
  static lfs_file_t f;
  static uint8_t cache[LFS_FILE_CACHE_SIZE];
  struct lfs_file_config fcfg;

  /* A write of PRIFLOP.CFG is applied over the file as it is now: the
     web page's Wi-Fi save (httpd.c, handle_wifi_post) sends only the keys
     it changed, so the others -- PFM_AUTOSTART, and anything PFM changed
     over the bus since the boot -- keep the file's values (open step 10). A whole file sent here gives that
     file. */
  if (shared_req.volume == FS_VOLUME_FLASH &&
      strcmp(shared_req.path, NETCONFIG_PATH) == 0) {
    static char now[NETCONFIG_FILE_MAX + 1];
    lfs_ssize_t got = 0;
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.buffer = cache;
    int opened = lfs_file_opencfg(req_volume(), &f, NETCONFIG_PATH, LFS_O_RDONLY, &fcfg);
    if (opened >= 0) {
      got = lfs_file_read(req_volume(), &f, now, NETCONFIG_FILE_MAX);
      lfs_file_close(req_volume(), &f);
    } else if (opened != LFS_ERR_NOENT) {
      got = -1;               /* only a missing file starts from the defaults */
    }
    if (got < 0) {
      shared_req.result = FS_RESULT_IO_ERROR;
      return;
    }
    now[got] = 0;
    if (shared_req.data_len + 1 > sizeof(shared_req.data)) {
      shared_req.result = FS_RESULT_IO_ERROR;
      return;
    }
    shared_req.data[shared_req.data_len] = 0;
    unsigned int n = netconfig_merge(now, (unsigned int)got,
                                     shared_req.data, shared_req.data_len,
                                     sizeof(shared_req.data));
    if (n == 0) {
      shared_req.result = FS_RESULT_IO_ERROR;
      return;
    }
    shared_req.data_len = n;
  }

  memset(&fcfg, 0, sizeof(fcfg));
  fcfg.buffer = cache;

  if (lfs_file_opencfg(req_volume(), &f, shared_req.path,
                       LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fcfg) < 0) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }

  lfs_ssize_t put = lfs_file_write(req_volume(), &f,
                                   shared_req.data, shared_req.data_len);
  int closed = lfs_file_close(req_volume(), &f);

  shared_req.result = (put == (lfs_ssize_t)shared_req.data_len && closed >= 0)
                    ? FS_RESULT_OK : FS_RESULT_IO_ERROR;
}

/* None of do_mkdir()/do_rename()/do_delete() call disk_idle_sync()
   directly, unlike httpd.c's pre-bridge originals -- see this file's
   header for why that durability-on-every-write behavior doesn't
   transfer safely to the bridge. fs_bridge_poll() only ever runs from
   within src/iec.c's BUS_IDLE loop, which already calls
   disk_idle_sync() itself, but on its own *debounced* schedule (a short
   continuous-idle timer, re-armed after every commit -- see iec.c's
   comment). That debounce exists specifically because an immediate,
   undebounced ~43ms-interrupts-disabled flash commit landing in the
   brief gap between a real C64 SAVE's own back-to-back ATN transactions
   (OPEN, then DATA, then CLOSE) made the kernal abandon the next one. Calling disk_idle_sync() directly here
   would bypass that debounce for exactly the same kind of gap, just
   triggered by a web UI request instead of a C64 command. Letting the
   write-back cache's normal dirty-marking (ata.c, already happens
   automatically via FatFs's own disk_write() calls) fall through to the
   existing debounced call keeps one single, already-proven-safe commit
   policy instead of two different ones racing on the same hardware. */

static void do_mkdir(void) {
  int res = lfs_mkdir(req_volume(), shared_req.path);
  shared_req.result = (res >= 0) ? FS_RESULT_OK : FS_RESULT_IO_ERROR;
}

static void do_rename(void) {
  int res = lfs_rename(req_volume(), shared_req.path, shared_req.path2);
  shared_req.result = (res >= 0) ? FS_RESULT_OK : FS_RESULT_IO_ERROR;
}

static void do_delete(void) {
  /* Never unlink an image a partition still has open -- see
     lfsops_mounted_image(). Only meaningful on the flash volume: the
     RAM drive never mounts D64/DNP images (ramops.c's read_sector/
     write_sector are stubs), so this check would just compare a RAM
     path against flash's mounted-image list for no reason. */
  if (shared_req.volume == FS_VOLUME_FLASH && lfsops_mounted_image(shared_req.path)) {
    shared_req.result = FS_RESULT_IN_USE;
    return;
  }

  int res = lfs_remove(req_volume(), shared_req.path);
  if (res >= 0) {
    shared_req.result = FS_RESULT_OK;
  } else if (res == LFS_ERR_NOTEMPTY) {
    shared_req.result = FS_RESULT_NOT_EMPTY;
  } else {
    shared_req.result = FS_RESULT_NOT_FOUND;
  }
}

/* Empty a directory, then remove it.
 *
 * One level deep only, deliberately. littlefs has no directory walker of
 * its own, so recursing would mean either an explicit stack (this device
 * has 4KB of it) or reopening the directory at each level. A drive whose
 * volume is a flat collection of images and PRGs does not need arbitrary
 * depth, and refusing loudly is better than a half-deleted tree: if a
 * subdirectory is found, nothing is removed and the caller is told the
 * directory is not empty, exactly as before.
 *
 * Restarts the scan after each unlink rather than deleting while
 * iterating, which littlefs does not promise is safe. */
/* Delete a directory and everything under it, to any depth.

   ITERATIVE, NOT RECURSIVE, and that is forced rather than stylistic.
   littlefs is built with LFS_NO_MALLOC, so every open directory handle is
   a caller-provided lfs_dir_t, and struct lfs_info carries a
   LFS_NAME_MAX+1 name buffer -- roughly 264 bytes of stack per level
   before anything else. Core 0's whole stack is 4KB (SCRATCH_Y, and 4KB
   is the ceiling without a custom linker script),
   with a measured peak already around 1.4KB. A recursive walk would put
   the depth of a user-supplied directory tree in charge of whether the
   drive overflows its stack, which is not a decision to hand to whoever
   uploaded the files.

   So instead: walk down from the root to a directory that contains no
   subdirectories, delete one entry there, and start again from the top.
   Two path buffers and one directory handle at a time, regardless of how
   deep the tree goes. Re-walking is O(depth) per deletion rather than
   O(1), which for a device that deletes at flash speed is not a cost
   worth trading stack safety for.

   Previously this refused any nested subdirectory outright, deleting only
   flat directories. */
#define DELETE_TREE_MAX_OPS   4096   /* total entries removed */
#define DELETE_TREE_MAX_DEPTH 16     /* nesting levels walked */

static void do_delete_tree(void) {
  char cur[FS_BRIDGE_PATH_MAX];
  char victim[FS_BRIDGE_PATH_MAX];

  if (shared_req.volume == FS_VOLUME_FLASH && lfsops_mounted_image(shared_req.path)) {
    shared_req.result = FS_RESULT_IN_USE;
    return;
  }

  for (unsigned int ops = 0; ops < DELETE_TREE_MAX_OPS; ops++) {
    lfs_dir_t dh;
    struct lfs_info info;
    bool descended;
    unsigned int depth = 0;

    /* Descend to the deepest directory with no subdirectories left. */
    snprintf(cur, sizeof(cur), "%s", shared_req.path);
    do {
      descended = false;

      if (lfs_dir_open(req_volume(), &dh, cur) < 0) {
        shared_req.result = FS_RESULT_NOT_FOUND;
        return;
      }
      while (lfs_dir_read(req_volume(), &dh, &info) > 0) {
        if (info.name[0] == '.') continue;        /* "." and ".." */
        if (info.type == LFS_TYPE_DIR) {
          char deeper[FS_BRIDGE_PATH_MAX];
          int n = snprintf(deeper, sizeof(deeper), "%s/%s", cur, info.name);
          if (n < 0 || (size_t)n >= sizeof(deeper)) {
            /* Path too long to address. Refusing beats deleting the
               wrong thing after a silent truncation. */
            lfs_dir_close(req_volume(), &dh);
            shared_req.result = FS_RESULT_IO_ERROR;
            return;
          }
          snprintf(cur, sizeof(cur), "%s", deeper);
          descended = true;
          break;
        }
      }
      lfs_dir_close(req_volume(), &dh);
    } while (descended && ++depth < DELETE_TREE_MAX_DEPTH);

    if (descended) {
      /* Still finding subdirectories at the depth cap. */
      shared_req.result = FS_RESULT_NOT_EMPTY;
      return;
    }

    /* `cur` holds no subdirectories. Remove one file from it, or if it is
       empty, remove the directory itself. */
    bool found_file = false;
    if (lfs_dir_open(req_volume(), &dh, cur) < 0) {
      shared_req.result = FS_RESULT_NOT_FOUND;
      return;
    }
    while (lfs_dir_read(req_volume(), &dh, &info) > 0) {
      if (info.name[0] == '.') continue;
      snprintf(victim, sizeof(victim), "%s/%s", cur, info.name);
      found_file = true;
      break;
    }
    lfs_dir_close(req_volume(), &dh);

    const char *target = found_file ? victim : cur;

    /* Checked for every entry, not just the top: a mounted image sitting
       three levels down is exactly as unsafe to delete as one at the
       root, and it is the deep one nobody remembers. */
    if (shared_req.volume == FS_VOLUME_FLASH && lfsops_mounted_image(target)) {
      shared_req.result = FS_RESULT_IN_USE;
      return;
    }
    if (lfs_remove(req_volume(), target) < 0) {
      shared_req.result = FS_RESULT_IO_ERROR;
      return;
    }

    if (!found_file && strcmp(cur, shared_req.path) == 0) {
      shared_req.result = FS_RESULT_OK;         /* removed the root */
      return;
    }
  }

  /* Ran out of budget. Partially deleted, and saying so is better than
     reporting success on a tree that is still there. */
  shared_req.result = FS_RESULT_IO_ERROR;
}

/*--------------------------------------------------------------------------
   Streaming file I/O -- an open FIL, unlike LIST/MKDIR/RENAME/DELETE
   above, has to survive across many separate request/response round
   trips (one open, many read-or-write chunks, one close). Core 0 owns
   a small fixed table of open handles; core 1 (httpd.c) only ever holds
   the integer index, matching this file's core rule that FatFs itself
   never gets touched from core 1. FS_BRIDGE_MAX_HANDLES matches
   httpd.c's HTTP_MAX_CONNS -- at most one handle per live connection.
---------------------------------------------------------------------------*/

#define FS_BRIDGE_MAX_HANDLES 3

static struct {
  lfs_file_t             file;
  /* LFS_NO_MALLOC: littlefs cannot allocate a file's cache, so each
     handle carries its own. See LFS_FILE_CACHE_SIZE in dirent.h. */
  uint8_t                cache[LFS_FILE_CACHE_SIZE];
  struct lfs_file_config cfg;
  bool                   used;
  /* Set at open time from shared_req.volume. A handle spans many
     round trips with no path or request context left to consult, so
     READ/WRITE/CLOSE need this to know which lfs_t* the file actually
     belongs to -- lfs_file_t itself carries no back-pointer to its
     volume. */
  bool                   is_ram;
} handles[FS_BRIDGE_MAX_HANDLES];

static int alloc_handle(void) {
  for (int i = 0; i < FS_BRIDGE_MAX_HANDLES; i++) {
    if (!handles[i].used) {
      handles[i].used = true;
      return i;
    }
  }
  return -1;
}

/* Bounds-checks a request's handle field against the table -- every
   READ/WRITE/CLOSE goes through this first, since the value crossed
   from core 1 and this file has no other reason to trust it. */
static lfs_file_t *lookup_handle(int h) {
  if (h < 0 || h >= FS_BRIDGE_MAX_HANDLES || !handles[h].used) {
    return NULL;
  }
  return &handles[h].file;
}

/* Which volume an already-validated handle belongs to. Call only after
   lookup_handle(h) has returned non-NULL. */
static lfs_t *handle_volume(int h) {
  return handles[h].is_ram ? ramops_volume() : lfsops_volume();
}

static void do_open_read(void) {
  int h = alloc_handle();
  if (h < 0) {
    shared_req.result = FS_RESULT_IO_ERROR; /* all handle slots in use */
    return;
  }
  memset(&handles[h].cfg, 0, sizeof(handles[h].cfg));
  handles[h].cfg.buffer = handles[h].cache;
  handles[h].is_ram = (shared_req.volume == FS_VOLUME_RAM);
  int res = lfs_file_opencfg(req_volume(), &handles[h].file,
                             shared_req.path, LFS_O_RDONLY, &handles[h].cfg);
  if (res < 0) {
    handles[h].used = false;
    shared_req.result = FS_RESULT_NOT_FOUND;
    return;
  }
  shared_req.handle = h;
  shared_req.file_size =
      (uint32_t)lfs_file_size(req_volume(), &handles[h].file);
  shared_req.result = FS_RESULT_OK;
}

static void do_open_write(void) {
  int h = alloc_handle();
  if (h < 0) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  memset(&handles[h].cfg, 0, sizeof(handles[h].cfg));
  handles[h].cfg.buffer = handles[h].cache;
  handles[h].is_ram = (shared_req.volume == FS_VOLUME_RAM);
  int res = lfs_file_opencfg(req_volume(), &handles[h].file, shared_req.path,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                             &handles[h].cfg);
  if (res < 0) {
    handles[h].used = false;
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  shared_req.handle = h;
  shared_req.result = FS_RESULT_OK;
}

static void do_read(void) {
  lfs_file_t *f = lookup_handle(shared_req.handle);
  if (!f) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  uint32_t want = shared_req.data_len; /* input: how much the caller wants */
  if (want > sizeof(shared_req.data)) want = sizeof(shared_req.data);
  lfs_ssize_t got = lfs_file_read(handle_volume(shared_req.handle), f, shared_req.data, want);
  if (got < 0) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  shared_req.data_len = (uint32_t)got; /* output: bytes read, 0 = EOF */
  shared_req.result = FS_RESULT_OK;
}

/* See do_mkdir()/do_rename()/do_delete()'s comment above -- same
   reasoning, no disk_idle_sync() call here either: this only marks
   ata.c's write-back cache dirty (via FatFs's own disk_write()), the
   existing debounced flush in iec.c's BUS_IDLE loop still picks it up. */
static void do_write(void) {
  lfs_file_t *f = lookup_handle(shared_req.handle);
  if (!f) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  lfs_ssize_t wrote = lfs_file_write(handle_volume(shared_req.handle), f, shared_req.data,
                                     shared_req.data_len);
  if (wrote < 0 || (uint32_t)wrote != shared_req.data_len) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  shared_req.result = FS_RESULT_OK;
}

static void do_close(void) {
  int h = shared_req.handle;
  lfs_file_t *f = lookup_handle(h);
  if (!f) {
    shared_req.result = FS_RESULT_IO_ERROR;
    return;
  }
  lfs_file_close(handle_volume(h), f);
  handles[h].used = false;
  shared_req.result = FS_RESULT_OK;
}

void fs_bridge_poll(void) {
  system_watchdog_pet();
  if (bridge_state != BRIDGE_REQUEST_PENDING) {
    return;
  }

  {
    uint32_t waited = to_ms_since_boot(get_absolute_time()) - submit_ms;
    if (waited > health.fsb_wait_max_ms)
      health.fsb_wait_max_ms = waited;
  }

  if (shared_req.op == FS_OP_SYNC) {
    /* Unlike do_mkdir()/do_rename()/do_delete() above, this one calls
       disk_idle_sync() directly, undebounced -- see httpd.c's handle_reboot()/
       handle_bootsel(): by the time this request exists, a reboot is
       already imminent and nothing is (or ever will be again, before
       this boot ends) mid-transaction, so the concern the debounce
       exists for doesn't apply. */
    disk_idle_sync();
    shared_req.result = FS_RESULT_OK;
  } else if (max_part < 1) {
    shared_req.result = FS_RESULT_NO_FS;
  } else if (shared_req.volume == FS_VOLUME_RAM && !ramops_available()) {
    /* FS_OP_READ/WRITE/CLOSE never reach here with volume == RAM unless
       httpd.c set it, which it doesn't -- those go through a handle
       that already remembers its volume from a successful open. */
    shared_req.result = FS_RESULT_NO_FS;
  } else {
    switch (shared_req.op) {
      case FS_OP_LIST:       do_list();       break;
      case FS_OP_MKDIR:      do_mkdir();      break;
      case FS_OP_RENAME:     do_rename();     break;
      case FS_OP_DELETE:     do_delete();     break;
      case FS_OP_DELETE_TREE: do_delete_tree(); break;
      case FS_OP_OPEN_READ:  do_open_read();  break;
      case FS_OP_OPEN_WRITE: do_open_write(); break;
      case FS_OP_READ:       do_read();       break;
      case FS_OP_WRITE:      do_write();      break;
      case FS_OP_CLOSE:      do_close();      break;
      case FS_OP_WRITE_FILE: do_write_file(); break;
      default:               shared_req.result = FS_RESULT_IO_ERROR; break;
    }

    /* Anything that changed how much of the volume is in use makes the
       cached free-space figure stale.

       This was missing entirely until 2026-09-05. lfsops.c invalidates on
       its own writes, but those are the *IEC* paths -- close-after-write,
       delete, mkdir -- and nothing on the web path did. So a file uploaded
       or deleted through the browser left the cache untouched, and the
       C64's BLOCKS FREE footer kept reporting the space as it was before,
       until some unrelated IEC-side write happened to invalidate it. The
       Files page's free-space badge would have inherited exactly the same
       bug, which is how it was found.

       Done here, once, rather than at the end of each do_*: the set of
       mutating ops is a property of the protocol, and a new op added to
       the enum without a thought for cache invalidation is precisely the
       mistake this file already made once.

       RAM has no equivalent cache (ramops_free_bytes() computes fresh
       every call, see its header comment), so this only fires for an
       op that actually touched the flash volume. WRITE/CLOSE are
       handle-based rather than path-based -- shared_req.volume isn't
       set for them, so which volume they touched has to come from the
       handle itself (handles[].is_ram survives do_close()'s used=false,
       since that only marks the slot free, it doesn't clear the
       struct). */
    switch (shared_req.op) {
      case FS_OP_MKDIR:
      case FS_OP_RENAME:
      case FS_OP_DELETE:
      case FS_OP_DELETE_TREE:
      case FS_OP_WRITE_FILE:
        if (shared_req.volume == FS_VOLUME_FLASH)
          lfsops_freeblocks_invalidate();
        break;
      case FS_OP_WRITE:
      case FS_OP_CLOSE:       /* a write handle's bytes land at close */
        if (shared_req.handle >= 0 && shared_req.handle < FS_BRIDGE_MAX_HANDLES &&
            !handles[shared_req.handle].is_ram)
          lfsops_freeblocks_invalidate();
        break;
      default:
        break;
    }
  }

  /* Recycle immediately when nobody is going to collect this. Without
     this, httpd.c's fire-and-forget FS_OP_CLOSE (sent when a connection
     dies with a file still open) left the single slot in
     BRIDGE_RESPONSE_READY permanently: every later filesystem request
     blocked forever, including the FS_OP_SYNC that /api/reboot defers
     behind -- so a single failed upload bricked the device until it was
     physically power-cycled. Observed exactly that, 2026-09-04. */
  if (discard_response) {
    discard_response = false;
    bridge_state = BRIDGE_IDLE;
    return;
  }

  bridge_state = BRIDGE_RESPONSE_READY;
}
