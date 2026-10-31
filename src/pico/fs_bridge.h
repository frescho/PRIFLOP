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

/* fs_bridge.h: request/response bridge letting core 1 (WiFi/httpd.c)
   get FatFs work done without ever touching FatFs itself.

   The core 1 Wi-Fi/HTTP offload: core 0 is the
   only core that may call into FatFs (fatops.c/d64ops.c's C64-facing
   code already assumes this, and FatFs itself isn't configured for
   reentrant/cross-core use -- see the approved plan). This is the
   mechanism that lets httpd.c (running on core 1, see core1.c) still
   serve directory listings, mkdir, rename, delete, and (as of this
   revision) streamed file download/upload: it fills in an fs_request_t,
   hands it to core 0 via fs_bridge_try_submit(), and polls
   fs_bridge_try_take_response() on later httpd_poll() ticks until the
   answer comes back -- never blocking core 1 while core 0 is busy (e.g.
   mid-IEC-transaction).

   PROTOCOL: a single shared fs_request_t slot, one outstanding request
   at a time, handed back and forth via a plain shared state flag (NOT
   pico_multicore's hardware FIFO -- see fs_bridge.c's file header for
   the real bug that caused, and why). Each side only reads or writes
   the shared struct strictly after observing the other side's state
   transition, so there is no window where both cores touch it at once.

   STREAMING FILE I/O: FS_OP_OPEN_READ/FS_OP_OPEN_WRITE hand back a
   `handle` (an index into core 0's own small table of open FIL
   handles, see fs_bridge.c) that subsequent FS_OP_READ/FS_OP_WRITE/
   FS_OP_CLOSE requests reference -- an open file spans many round
   trips, unlike LIST/MKDIR/RENAME/DELETE/SYNC's one-request-one-
   response shape. FS_OP_READ's request uses data_len as an *input*
   (how many bytes to read, capped at sizeof(data)); its response reuses
   the same field as the actual byte count read (0 means EOF).
   FS_OP_WRITE's request uses data/data_len as input (bytes to write);
   its response doesn't use them. See httpd.c's pump_send_file()/
   on_recv()/finish_upload() for the per-connection state machines built
   on top of this. */
#ifndef FS_BRIDGE_H
#define FS_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#define FS_BRIDGE_PATH_MAX 200   /* matches httpd.c's HTTP_PATH_MAX */
#define FS_BRIDGE_BUF_MAX  4096  /* matches httpd.c's HTTP_JSON_BUF_MAX; also the max single READ/WRITE chunk */

typedef enum {
  FS_OP_NONE = 0,
  FS_OP_LIST,
  FS_OP_MKDIR,
  FS_OP_RENAME,
  FS_OP_DELETE,
  FS_OP_SYNC,       /* force disk_idle_sync() now -- see httpd.c's handle_reboot()/handle_bootsel() */
  FS_OP_OPEN_READ,
  FS_OP_OPEN_WRITE,
  FS_OP_READ,
  FS_OP_WRITE,
  FS_OP_CLOSE,
  /* Write a whole small file in one round trip (create/truncate, write
     data[0..data_len], close). The streaming OPEN_WRITE/WRITE/CLOSE
     sequence is right for uploads of unknown length, but a caller that
     already holds the entire content -- PRIFLOP.CFG is under 1KB -- would
     otherwise need a three-state machine on core 1 to save it. */
  FS_OP_WRITE_FILE,
  /* DELETE that empties a directory first. Separate from FS_OP_DELETE so
     the destructive version is never what a plain request gets by
     accident -- the web UI only sends it after the user confirms. */
  FS_OP_DELETE_TREE,
} fs_op_t;

typedef enum {
  FS_RESULT_OK = 0,
  FS_RESULT_NO_FS,        /* no filesystem mounted (max_part < 1) */
  FS_RESULT_NOT_FOUND,
  FS_RESULT_NOT_EMPTY,    /* DELETE on a non-empty directory */
  FS_RESULT_IN_USE,       /* DELETE on an image a partition has mounted */
  FS_RESULT_IO_ERROR,
} fs_result_t;

/* Which littlefs volume a request targets. FS_VOLUME_FLASH is 0 so
   every existing caller that never sets this field keeps today's
   behaviour unchanged. Irrelevant for FS_OP_READ/WRITE/CLOSE, which
   go through a handle that already remembers which volume it opened
   on -- see fs_bridge.c's handles[]. */
typedef enum {
  FS_VOLUME_FLASH = 0,
  FS_VOLUME_RAM,
} fs_volume_t;

typedef struct {
  fs_op_t op;
  fs_volume_t volume;
  char path[FS_BRIDGE_PATH_MAX];
  char path2[FS_BRIDGE_PATH_MAX]; /* FS_OP_RENAME's "to" only */

  /* handle: input for FS_OP_READ/WRITE/CLOSE (which open handle to use);
     output for FS_OP_OPEN_READ/OPEN_WRITE (the newly opened handle) --
     never both at once for a single op, same dual-use pattern as
     data_len below. */
  int handle;

  /* Response, valid once fs_bridge_try_take_response() returns true. */
  fs_result_t result;
  uint32_t file_size;              /* FS_OP_OPEN_READ response only: for Content-Length */
  /* FS_OP_LIST: input = how many entries to skip, output = the offset to
     ask for next, or 0 when the listing is complete. A directory bigger
     than one JSON buffer used to be silently truncated. */
  uint32_t offset;
  uint32_t data_len;                /* LIST: JSON bytes. READ: input = bytes wanted (capped
                                        at sizeof(data)), output = bytes actually read (0 = EOF).
                                        WRITE: input = bytes to write from data[]. */
  char data[FS_BRIDGE_BUF_MAX];
} fs_request_t;

/* --- Core 1 (httpd.c) side --- */

/* Copies *req wholesale into the shared slot and marks it pending for
   core 0. Returns false (does nothing) if another request is already
   in flight -- caller should try again on a later poll tick, not block. */
/* `owner` identifies the caller (httpd.c passes the connection). Only the
   same owner may collect the reply, and fs_bridge_abandon(owner) reclaims
   the slot if that caller goes away first -- without which a connection
   dying mid-request strands the single slot and blocks ALL filesystem
   access, including the FS_OP_SYNC /api/reboot defers behind. */
bool fs_bridge_try_submit(const fs_request_t *req, const void *owner);

/* Same, but for a request whose reply the caller will never collect --
   the slot is recycled by fs_bridge_poll() instead of waiting for a
   fs_bridge_try_take_response() that is never coming. Use this and NEVER
   plain fs_bridge_try_submit() for fire-and-forget work, or the single
   slot wedges and takes all filesystem access with it. */
bool fs_bridge_submit_discard(const fs_request_t *req);

/* Once fs_bridge_try_submit() has returned true, poll this until it
   returns true too -- copies the finished result wholesale into *req
   and frees the shared slot for the next caller. Returns false (does
   nothing) if the response isn't ready yet. */
bool fs_bridge_try_take_response(fs_request_t *req, const void *owner);

/* Call when `owner` is going away. Reclaims the slot whether the request
   is still being serviced or its reply is already waiting. Safe to call
   when the owner holds nothing. */
void fs_bridge_abandon(const void *owner);

/* --- Core 0 (src/iec.c's BUS_IDLE) side --- */

/* Services at most one pending request per call -- cheap no-op if
   nothing is waiting. Call alongside disk_idle_sync(), same safe
   point. */
void fs_bridge_poll(void);

#endif
