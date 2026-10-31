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


   health.c: see health.h. Nothing in here may include an SDK header --
   tests/health_test.c compiles this file on the host. */

#include "health.h"

health_counters_t health = {
  .wifi_link_status = HEALTH_LINK_UNKNOWN,
};

/* All time comparisons in the observer are made against its own earlier
   readings of `now_ms`, never against a timestamp written by the other core:
   core 1 can stamp a frame a few milliseconds after core 0 sampled the
   clock, and an unsigned "now - then" of a time slightly in the future would
   read as fifty days. */
static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms) {
  return (uint32_t)(now_ms - since_ms) >= span_ms;
}

bool health_tick_due(uint32_t *next_ms, uint32_t now_ms) {
  /* 0 means "never opened": the first call always passes, even one made more
     than 24.8 days into the uptime, where the signed test below would call
     next_ms == 0 the future. A later next_ms that happens to be 0 costs one
     extra tick. */
  if (*next_ms != 0 && (int32_t)(now_ms - *next_ms) < 0)
    return false;
  *next_ms = now_ms + 1000u;
  return true;
}

health_event_t health_watch_step(health_watch_t *w, const health_snapshot_t *s,
                                 uint32_t now_ms, uint32_t *detail) {
  if (!w->started) {
    w->started = true;
    w->seen_loops = s->core1_loops;
    w->seen_loops_at_ms = now_ms;
    w->seen_frames = s->wifi_rx_frames;
    w->frames_changed_at_ms = now_ms;
    return HEALTH_NONE;
  }

  /* 1. Is core 1 getting round its loop? */
  if (s->core1_loops != w->seen_loops) {
    w->seen_loops = s->core1_loops;
    w->seen_loops_at_ms = now_ms;
    if (w->stalled) {
      w->stalled = false;
      /* The silence timer below must not blame the link for the time core 1
         was not there to receive anything. */
      w->frames_changed_at_ms = now_ms;
      w->seen_frames = s->wifi_rx_frames;
      *detail = (uint32_t)(now_ms - w->stalled_since_ms) / 1000u;
      return HEALTH_CORE1_RESUMED;
    }
  } else {
    uint32_t limit = (w->seen_loops == 0) ? HEALTH_CORE1_BOOT_MS : HEALTH_CORE1_STALL_MS;
    if (elapsed(now_ms, w->seen_loops_at_ms, limit)) {
      if (!w->stalled) {
        w->stalled = true;
        w->stalled_since_ms = w->seen_loops_at_ms;
        w->last_stall_report_ms = now_ms;
        *detail = (uint32_t)(now_ms - w->seen_loops_at_ms) / 1000u;
        return HEALTH_CORE1_STALLED;
      }
      if (elapsed(now_ms, w->last_stall_report_ms, HEALTH_REPEAT_MS)) {
        w->last_stall_report_ms = now_ms;
        *detail = (uint32_t)(now_ms - w->seen_loops_at_ms) / 1000u;
        return HEALTH_CORE1_STALLED;
      }
    }
  }

  /* 2. Is a link that says it is up carrying anything? Only meaningful for
     a station: in AP mode wifi_link_status stays HEALTH_LINK_UNKNOWN. */
  bool up = (s->wifi_link_status == HEALTH_LINK_UP);
  if (!up || s->wifi_rx_frames != w->seen_frames) {
    w->seen_frames = s->wifi_rx_frames;
    w->frames_changed_at_ms = now_ms;
    if (w->silent) {
      w->silent = false;
      if (up)
        return HEALTH_WIFI_HEARD;
    }
  } else if (!w->stalled && elapsed(now_ms, w->frames_changed_at_ms, HEALTH_WIFI_SILENT_MS)) {
    if (!w->silent) {
      w->silent = true;
      w->last_silent_report_ms = now_ms;
      *detail = (uint32_t)(now_ms - w->frames_changed_at_ms) / 1000u;
      return HEALTH_WIFI_SILENT;
    }
    if (elapsed(now_ms, w->last_silent_report_ms, HEALTH_REPEAT_MS)) {
      w->last_silent_report_ms = now_ms;
      *detail = (uint32_t)(now_ms - w->frames_changed_at_ms) / 1000u;
      return HEALTH_WIFI_SILENT;
    }
  }

  return HEALTH_NONE;
}
