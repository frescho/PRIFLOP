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


   bootlog.h: console history that outlives a reset.

   WHY THIS EXISTS. On 2026-09-05 a firmware change hung core 0, the
   watchdog reset the board, Wi-Fi did not come back, and the drive sat in
   AP mode where the build machine -- ethernet only -- could not see it.
   From outside, that was indistinguishable from a board that had died:
   silence on the network and silence on the console.

   The console was silent for a stupid reason. The banner is printed once,
   early, and every attempt to capture it opened /dev/ttyACM0 too late,
   or across the BOOTSEL re-enumeration that flashing causes, or on a
   stale file descriptor. Not one boot banner was captured in a whole day
   of work. An hour went into distinguishing a hang from a Wi-Fi failure,
   and a wrong conclusion was published on the way (the transmit code was
   declared exonerated when it was not) -- purely because the only
   observation channel was the one thing that had failed.

   So console output is now also written to a ring buffer in memory that
   survives a reset, and can be asked for later, over either channel:

     - GET /api/bootlog -- the channel that works when the network is up;
     - the USB console, which dumps the log automatically the moment a
       host attaches (and on 'd'), and is the one that matters when the
       network is not up.

   THE USB CONSOLE NEEDS DTR, AND THAT IS ALL IT NEEDS. This file
   previously claimed the console was dead and that TinyUSB's device task
   was not being serviced. That was wrong, and the measurements that
   settled it are worth keeping because the question will come back:
   stdio_init_all() returns true, tud_inited() and tud_mounted() are both
   1 -- which could not happen if the device task were not running -- and
   asserting DTR from the host flips stdio_usb_connected() from 0 to 1 on
   the device.

   The SDK's stdio_usb simply discards output until a host asserts DTR.
   Every capture attempt that "proved" the console dead had used cat, dd
   or head, none of which assert it. A whole day of silence was host-side
   tooling, not firmware. Use a terminal that raises DTR (pyserial's
   p.dtr = True, or any normal terminal emulator).

   The counters are still in /api/stats -- usb_conn among them -- because
   this is exactly the kind of thing that gets re-diagnosed from scratch
   otherwise.

   Never cleared. Each boot appends a marker, so the buffer holds the tail
   of history ACROSS resets: what the firmware said before it died is
   still there after the watchdog has restarted it, which is exactly the
   evidence that was missing.
*/

#ifndef BOOTLOG_H
#define BOOTLOG_H

#include <stdint.h>

#define BOOTLOG_SIZE 2048

/* Validate (or create) the buffer and append a boot marker. Call early,
   before anything worth logging happens. */
void bootlog_init(void);

/* Append one character. Called from uart_putc(), so everything the
   console says is captured. Cheap enough for the bus path: a bounds
   check, a store, and an index bump. */
void bootlog_putc(char c);

/* Copy out, oldest first. Returns the number of bytes written to `out`,
   at most `max`. */
uint32_t bootlog_read(char *out, uint32_t max);

/* Poll the USB console for a request to dump the log. Call from BUS_IDLE
   only -- it touches stdio, which takes a mutex, and that has no business
   running inside a live bus transaction. */
void bootlog_poll_console(void);

#endif
