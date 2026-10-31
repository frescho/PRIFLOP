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


   bootlog.c: see bootlog.h for why this exists.
*/

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "bootlog.h"
#include "uart.h"

#include "pico/platform/sections.h"
#include "hardware/watchdog.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"

/* The buffer lives in .uninitialized_data, which the linker marks NOLOAD
   and crt0 does not clear -- so it keeps its contents through a watchdog
   reset, which is the whole point. It does NOT survive a power cycle, and
   is not meant to: the case this serves is "the board reset itself and I
   need to know why", not long-term storage.

   The magic is what makes a cold start distinguishable from a warm one.
   Uninitialised SRAM after power-on is arbitrary, and printing arbitrary
   bytes as if they were a log would be worse than printing nothing. */
#define BOOTLOG_MAGIC 0xB0071067u

typedef struct {
  uint32_t magic;
  uint32_t head;      /* next write position */
  uint32_t wrapped;   /* non-zero once the ring has come round */
  char     buf[BOOTLOG_SIZE];
} bootlog_t;

/* The attribute has to sit on the VARIABLE, not on an anonymous struct
   type -- gcc warns "'section' attribute does not apply to types" and
   silently puts it in .bss, where crt0 zeroes it on every boot and the
   log never survives anything. Caught by the warning; it would otherwise
   have looked like it worked and quietly recorded nothing. */
static bootlog_t __uninitialized_ram(bootlog);

static uint8_t bootlog_ready;
static uint8_t bootlog_muted;   /* see bootlog_poll_console() */

static void bootlog_puts(const char *s) {
  while (*s)
    bootlog_putc(*s++);
}

static void bootlog_raw(char c) {
  bootlog.buf[bootlog.head++] = c;
  if (bootlog.head >= BOOTLOG_SIZE) {
    bootlog.head = 0;
    bootlog.wrapped = 1;
  }
}

/* Timestamps go into the ring only, not out to the console.

   The console stream stays byte-for-byte what it always was -- anyone
   reading it live is unaffected -- while the recorded copy gains the one
   thing that makes a boot log answer questions instead of raising them:
   when each line happened. "WiFi: attempt 3 failed" is a fact; "at
   14.2s, WiFi: attempt 3 failed" tells you the join ladder is eating
   fifteen seconds before the AP ever accepts. */
static uint8_t at_line_start = 1;

void bootlog_putc(char c) {
  if (!bootlog_ready || bootlog_muted)
    return;

  if (at_line_start && c != '\r' && c != '\n') {
    uint32_t ms = time_us_32() / 1000u;
    char ts[10];
    int n = 0;
    ts[n++] = '[';
    if (ms >= 100000u) ts[n++] = (char)('0' + (ms / 100000u) % 10);
    if (ms >= 10000u)  ts[n++] = (char)('0' + (ms / 10000u) % 10);
    if (ms >= 1000u)   ts[n++] = (char)('0' + (ms / 1000u) % 10);
    ts[n++] = '.';
    ts[n++] = (char)('0' + (ms / 100u) % 10);
    ts[n++] = 's';
    ts[n++] = ']';
    ts[n++] = ' ';
    for (int i = 0; i < n; i++)
      bootlog_raw(ts[i]);
    at_line_start = 0;
  }

  bootlog_raw(c);
  if (c == '\n')
    at_line_start = 1;
}

void bootlog_init(void) {
  if (bootlog.magic != BOOTLOG_MAGIC ||
      bootlog.head >= BOOTLOG_SIZE) {
    /* Cold start, or memory that has never held a log. */
    bootlog.magic   = BOOTLOG_MAGIC;
    bootlog.head    = 0;
    bootlog.wrapped = 0;
    memset(bootlog.buf, 0, sizeof(bootlog.buf));
  }
  bootlog_ready = 1;

  /* A marker rather than a wipe. Keeping the previous boot's output is
     the entire reason this exists -- clearing here would throw away the
     evidence at precisely the moment it became interesting. The watchdog
     bit says whether this boot follows a reset or a deliberate one. */
  bootlog_puts("\r\n--- boot");
  if (watchdog_caused_reboot())
    bootlog_puts(" (after WATCHDOG reset)");
  bootlog_puts(" ---\r\n");
}

uint32_t bootlog_read(char *out, uint32_t max) {
  uint32_t n = 0;

  if (!bootlog_ready)
    return 0;

  if (bootlog.wrapped) {
    /* Oldest data sits just after the head. */
    for (uint32_t i = bootlog.head; i < BOOTLOG_SIZE && n < max; i++)
      if (bootlog.buf[i])
        out[n++] = bootlog.buf[i];
  }
  for (uint32_t i = 0; i < bootlog.head && n < max; i++)
    if (bootlog.buf[i])
      out[n++] = bootlog.buf[i];

  return n;
}

volatile uint32_t bootlog_polls;      /* proves this is reached at all */
volatile uint32_t bootlog_dumps;

/* The dump is spread across idle passes rather than done in one go.

   Writing it in a single loop was a serious mistake and is worth spelling
   out, because it looked completely reasonable: 2KB of uart_putc() from
   the bus mainloop. Each of those can block for
   PICO_STDIO_USB_STDOUT_TIMEOUT_US when the host is not draining the
   endpoint, so at the SDK's 500ms default the loop's worst case was about
   a thousand seconds against an eight-second watchdog. Any dump into a
   stalled console was a guaranteed reset, and a reset loop at that.

   Two changes, because either alone is thin. The timeout is now 2ms (see
   CMakeLists), and no single pass emits more than BOOTLOG_CHUNK
   characters -- so the worst case is bounded at chunk x 2ms whatever the
   host does, and a console that disconnects mid-dump abandons it. */
#define BOOTLOG_CHUNK 32

static char     dump_buf[BOOTLOG_SIZE];
static uint32_t dump_len;
static uint32_t dump_pos;
static uint8_t  dump_active;

static void bootlog_dump_start(void) {
  if (dump_active)
    return;
  dump_len = bootlog_read(dump_buf, sizeof(dump_buf));
  dump_pos = 0;
  dump_active = 1;
  bootlog_dumps++;

  /* Muted for the whole dump: the output goes back through uart_putc()
     into this same ring, so an unmuted dump would overwrite the history
     it is in the middle of printing. Reading must not destroy. */
  bootlog_muted = 1;
  uart_puts_P(PSTR("\r\n===== bootlog, "));
  uart_puthex((uint8_t)(dump_len >> 8));
  uart_puthex((uint8_t)(dump_len & 0xff));
  uart_puts_P(PSTR(" bytes =====\r\n"));
}

static void bootlog_dump_step(void) {
  uint32_t end = dump_pos + BOOTLOG_CHUNK;

  if (end > dump_len)
    end = dump_len;
  while (dump_pos < end)
    uart_putc(dump_buf[dump_pos++]);

  if (dump_pos >= dump_len) {
    uart_puts_P(PSTR("\r\n===== end =====\r\n"));
    bootlog_muted = 0;
    dump_active = 0;
  }
}

static void bootlog_dump_abort(void) {
  if (!dump_active)
    return;
  dump_active = 0;
  bootlog_muted = 0;
}

void bootlog_poll_console(void) {
  static uint8_t was_connected;
  uint8_t now;

  bootlog_polls++;

  /* Dump on the rising edge of "a console attached".

     Waiting for the operator to press a key was the obvious design and
     the wrong one: it needs CDC *input* to work, which is a second
     mechanism to get right, and it needs someone watching at the moment
     it matters. Triggering on attach needs neither -- open the port with
     DTR asserted and the history arrives on its own. Which is exactly
     what a person does when they come to find out why the drive is
     unhappy.

     DTR is what "connected" means here: the SDK's stdio_usb discards
     everything until the host asserts it, which is why this port looked
     for a whole day like it had no console at all. It has one; nothing
     was ever listening properly. */
  now = stdio_usb_connected() ? 1 : 0;

  if (!now) {
    /* Nobody listening: abandon any dump in progress and do not start
       one. Writing into a disconnected console is the case that costs
       time and buys nothing. */
    bootlog_dump_abort();
    was_connected = 0;
    return;
  }

  if (!was_connected)
    bootlog_dump_start();
  was_connected = 1;

  if (dump_active) {
    bootlog_dump_step();
    return;              /* one chunk per pass, nothing else this time */
  }

  if (getchar_timeout_us(0) == 'd')
    bootlog_dump_start();
}
