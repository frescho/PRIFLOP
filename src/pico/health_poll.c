/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2026 sd2iec_pico2w contributors

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


   health_poll.c: the firmware half of health.h -- reads the clock, samples the
   counters and writes what health_watch_step() decides to say to the console
   (and therefore the boot log). Kept apart from health.c so that file stays
   free of the SDK and testable on the host.

   Called from network_poll(), i.e. from core 0's BUS_IDLE loop and nowhere
   else. That is the same context bootlog_poll_console() already writes to
   the console from; it is never called from a bus transaction, so the rule
   against console output on the bus path (invariant 2) holds. It
   samples about once a second and speaks only when something has changed or,
   for a condition that persists, every HEALTH_REPEAT_MS: a line is under 90
   characters, so even a console nobody is draining costs a few tens of
   milliseconds, and only on those rare occasions. */

#include <stdio.h>

#include "config.h"
#include "uart.h"
#include "health.h"

#include "pico/time.h"

void health_poll(void) {
  static health_watch_t watch;
  static uint32_t next_ms;

  uint32_t now = to_ms_since_boot(get_absolute_time());   /* not time_us_32() / 1000: see health.h */
  if (!health_tick_due(&next_ms, now))
    return;

  health_snapshot_t s = {
    .core1_loops      = health.core1_loops,
    .wifi_rx_frames   = health.wifi_rx_frames,
    .wifi_link_status = health.wifi_link_status,
  };
  uint32_t detail = 0;
  char msg[96];

  switch (health_watch_step(&watch, &s, now, &detail)) {
  case HEALTH_CORE1_STALLED:
    snprintf(msg, sizeof(msg), "HEALTH: core1 no progress %lus loops=%lu gate=%lu stk=%lu\r\n",
             (unsigned long)detail, (unsigned long)s.core1_loops,
             (unsigned long)health.core1_gate_timeouts, (unsigned long)health.core1_stack_used);
    break;
  case HEALTH_CORE1_RESUMED:
    snprintf(msg, sizeof(msg), "HEALTH: core1 running again after %lus\r\n", (unsigned long)detail);
    break;
  case HEALTH_WIFI_SILENT:
    snprintf(msg, sizeof(msg), "HEALTH: wifi UP but no frame for %lus rx=%lu drops=%lu\r\n",
             (unsigned long)detail, (unsigned long)s.wifi_rx_frames,
             (unsigned long)health.wifi_link_drops);
    break;
  case HEALTH_WIFI_HEARD:
    snprintf(msg, sizeof(msg), "HEALTH: wifi frames again rx=%lu\r\n", (unsigned long)s.wifi_rx_frames);
    break;
  default:
    return;
  }
  uart_puts_P(msg);
}
