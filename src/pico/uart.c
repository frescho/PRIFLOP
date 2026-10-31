/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2026 sd2iec_pico2w contributors

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


   uart.c: UART access routines (RP2350 / Pico 2 W)

   Implemented via uart_hw_shim.h rather than including Pico SDK's
   hardware/uart.h directly here -- see that header's comment for why
   (uart_init/uart_putc/uart_getc name-collide with sd2iec's own uart.h
   declarations, just with different signatures).

   No software TX ring buffer here (unlike the ESP8266 arch, which hand-
   rolls one sized by CONFIG_UART_TX_BUF_SHIFT): RP2350's UART has a
   32-byte hardware FIFO and pico_hw_uart_putc_raw() blocks until there's
   room, which is simple, correct, and plenty for a debug/trace console.
   CONFIG_UART_TX_BUF_SHIFT in configs/config-pico2w is consequently
   unused by this arch -- harmless, just inherited from the ESP8266
   config template.

   CROSS-CORE CONSOLE LOCKING (2026-09-02):
   added when src/pico/core1.c's very first smoke test made both cores
   print at boot at the same time -- the two messages came back
   byte-for-byte interleaved on the wire (confirmed on real hardware),
   since nothing serialized access to the shared UART/USB-CDC console
   between cores. console_lock (a claimed hardware spinlock, so it works
   across cores, unlike a plain flag) is held for the duration of one
   whole multi-character call (a full line), not per-byte -- per-byte
   locking would still let the two cores' *lines* interleave with each
   other between characters, just less often. Deliberately NOT wrapping
   uart_putc() itself: a few call sites (e.g. iec.c's IRQ-context single-
   char trace markers) call it directly outside any locked call, and
   this scoping only fixes the *new* cross-core hazard core1.c
   introduced, not a preexisting (and separately tolerated) single-core
   IRQ-vs-mainloop interleaving edge case that predates this change.

   Uses spin_lock_unsafe_blocking()/spin_unlock_unsafe(), not the
   default spin_lock_blocking()/spin_unlock() pair -- confirmed on real
   hardware (2026-09-02) that the "safe" variant, which also does
   save_and_disable_interrupts() for the duration held, broke IEC bus
   timing so badly a real C64 got DEVICE NOT PRESENT on every LOAD:
   iec.c's BUS_ATNACTIVE calls uart_puthex()/uart_putcrlf() on every
   addressed command byte, and pico_hw_usb_putc() (uart_hw_shim.c) can
   block up to PICO_STDIO_USB_STDOUT_TIMEOUT_US (500ms) if no USB host
   is actively draining the CDC console -- that's exactly the "as long
   as interrupts stay enabled" assumption uart_hw_shim.c's own comment
   documents, and disabling them here for the length of a stalled USB
   write blew straight through it. The unsafe variant keeps the actual
   cross-core mutual exclusion this lock exists for without touching
   the interrupt mask. */
#include "config.h"
#include "uart.h"
#include "bootlog.h"
#include "uart_hw_shim.h"

#include "hardware/sync.h"

static spin_lock_t *console_lock;

void uart_init(void) {
  pico_hw_uart_init(CONFIG_UART_BAUDRATE);
  /* USB CDC debug console, mirrored alongside the physical UART -- see
     uart_hw_shim.h. Temporary until a real UART adapter or debug probe
     is available (testing notes, 2026-08-22);
     harmless to leave in afterward, it's just an extra output sink. */
  pico_hw_usb_stdio_init();
  console_lock = spin_lock_init(spin_lock_claim_unused(true));
}

void uart_putc(char c) {
  /* Into the surviving ring first, so a character is recorded even if the
     USB write below blocks or is never drained -- which is the normal
     state of affairs with no terminal attached, and was exactly the
     situation on 2026-09-05 when the console had nothing to say about a
     hang because nobody was listening at the time. */
  bootlog_putc(c);
  pico_hw_uart_putc_raw(c);
  pico_hw_usb_putc(c);
}

unsigned char uart_getc(void) {
  if (!pico_hw_uart_is_readable()) {
    return (unsigned char)-1;
  }
  return (unsigned char)pico_hw_uart_getc_raw();
}

/* Internal, non-locking bodies -- used both by the locked public
   wrappers below and by uart_trace(), which needs to call these without
   re-acquiring console_lock (it's not reentrant, and it already holds
   the lock for its own whole multi-line dump). */
static void uart_puthex_nolock(uint8_t num) {
  uint8_t tmp;

  tmp = (num & 0xf0) >> 4;
  uart_putc(tmp < 10 ? '0' + tmp : 'a' + tmp - 10);

  tmp = num & 0x0f;
  uart_putc(tmp < 10 ? '0' + tmp : 'a' + tmp - 10);
}

static void uart_putcrlf_nolock(void) {
  uart_putc('\r');
  uart_putc('\n');
}

void uart_puthex(uint8_t num) {
  spin_lock_unsafe_blocking(console_lock);
  uart_puthex_nolock(num);
  spin_unlock_unsafe(console_lock);
}

void uart_putcrlf(void) {
  spin_lock_unsafe_blocking(console_lock);
  uart_putcrlf_nolock();
  spin_unlock_unsafe(console_lock);
}

void uart_puts_P(const char *text) {
  spin_lock_unsafe_blocking(console_lock);
  char ch;
  while ((ch = *text++)) {
    uart_putc(ch);
  }
  spin_unlock_unsafe(console_lock);
}

void uart_flush(void) {
  /* Nothing buffered on our side to flush -- see file comment. */
}

void uart_trace(void *ptr, uint16_t start, uint16_t len) {
  uint16_t i;
  uint8_t j;
  uint8_t ch;
  uint8_t *data = ptr;
  spin_lock_unsafe_blocking(console_lock);

  data += start;
  for (i = 0; i < len; i += 16) {
    uart_puthex_nolock(start >> 8);
    uart_puthex_nolock(start & 0xff);
    uart_putc('|');
    uart_putc(' ');
    for (j = 0; j < 16; j++) {
      if (i + j < len) {
        ch = *(data + j);
        uart_puthex_nolock(ch);
      } else {
        uart_putc(' ');
        uart_putc(' ');
      }
      uart_putc(' ');
    }
    uart_putc('|');
    for (j = 0; j < 16; j++) {
      if (i + j < len) {
        ch = *(data++);
        if (ch < 32 || ch > 0x7e)
          ch = '.';
        uart_putc(ch);
      } else {
        uart_putc(' ');
      }
    }
    uart_putc('|');
    uart_putcrlf_nolock();
    start += 16;
  }

  spin_unlock_unsafe(console_lock);
}
