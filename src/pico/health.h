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


   health.h: is core 1 alive, and is the Wi-Fi link carrying traffic?

   WHY THIS EXISTS. A failure has been seen
   more than once: the drive keeps working on the IEC bus, the USB console
   answers, and the web UI is simply gone until a power cycle. 2026-09-20 it
   was caught in the act -- the device had joined its network at boot
   (bootlog: "connected, browse to http://<address>"), was unreachable
   from the LAN for the next six hours (ARP for its address unanswered),
   and had logged no "link lost" line, so its own link monitor never saw a
   problem. From outside that fits several different failures:

     1. core 1 is stuck or starved (nothing runs the network stack);
     2. core 1 is fine and the chip believes it is joined, but the link
        carries nothing -- an association the access point dropped without
        the chip noticing;
     3. the link really dropped and the rejoin logic is not getting anywhere.

   The existing counters cannot tell them apart, and the only way to ask the
   device -- /api/stats -- is the very thing that is gone. So the evidence
   has to be gathered from core 0, which is alive in every one of these
   cases, and written to the boot log, which survives and is readable over
   USB without any network.

   Core 1 keeps a few counters (`health`, below); core 0 samples them about
   once a second from the BUS_IDLE loop (network_poll() in network.c, which
   was an empty function) and calls health_watch_step(), which is plain C
   with no SDK in it so that tests/health_test.c can run it on the host.

   It says at most one thing per call and repeats a persistent condition only
   every HEALTH_REPEAT_MS, so a wedged device fills the 2 KB boot log slowly
   and never pushes out the boot messages that came first.
*/
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stdint.h>

/* wifi_link_status before core 1 has read it, and for as long as the device
   is an access point rather than a station: there is no upstream link to
   watch, so nothing below reports on it. */
#define HEALTH_LINK_UNKNOWN  99
/* == CYW43_LINK_UP; restated so this file needs no SDK header. */
#define HEALTH_LINK_UP       3

/* Core 1 passes through its main loop many times a second. Its slowest
   legitimate stall is a rejoin (a scan of up to 6 s plus a 20 s join), so
   anything beyond this is not that. */
#define HEALTH_CORE1_STALL_MS  45000u
/* Before core 1's first pass the boot-time join may legitimately take a
   minute or more (sixteen attempts), so "never started" waits longer. */
#define HEALTH_CORE1_BOOT_MS   300000u
/* A live Wi-Fi network delivers something -- an ARP, an mDNS announcement --
   every few seconds. Three minutes of nothing while the chip says UP is not
   a quiet LAN. */
#define HEALTH_WIFI_SILENT_MS  180000u
#define HEALTH_REPEAT_MS       1800000u

/* Counters written by one core and read by the other. Plain volatile
   words: each has a single writer, and a slightly stale read only delays a
   report.

   Every millisecond value here, and every `now_ms` given to
   health_watch_step(), is to_ms_since_boot(): 32 bits of milliseconds, which
   wrap after 49.7 days and which the unsigned subtractions below survive.
   NOT time_us_32() / 1000 -- that is a microsecond counter wrapping after
   71.6 minutes, divided, so it jumps from 4294967 back to 0. It was used
   until 2026-09-30, and after the first jump health_poll()'s once-a-second
   gate never opened again: the observer went silent 72 minutes after boot,
   and c1_gap_max_ms read 4290672329 (2^32 - 4294967). */
typedef struct {
  volatile uint32_t core1_loops;          /* passes through core 1's main loop */
  volatile uint32_t core1_gap_max_ms;     /* longest time between two passes */
  volatile uint32_t core1_gate_timeouts;  /* waits for the bus that hit their limit */
  volatile uint32_t core1_stack_used;     /* high-water mark of core 1's stack, bytes; 0 = not measured */
  volatile uint32_t wifi_rx_frames;       /* frames the chip has handed to lwIP */
  volatile uint32_t wifi_rx_last_ms;      /* time since boot of the most recent one */
  volatile int32_t  wifi_link_status;     /* cyw43_tcpip_link_status(), or HEALTH_LINK_UNKNOWN */
  volatile uint32_t wifi_link_drops;      /* times the link was seen to leave UP */
  volatile uint32_t fsb_wait_max_ms;      /* longest a web filesystem request waited for core 0 */
} health_counters_t;

extern health_counters_t health;

typedef struct {
  uint32_t core1_loops;
  uint32_t wifi_rx_frames;
  int32_t  wifi_link_status;
} health_snapshot_t;

/* State private to the observer (core 0). Zero-initialise it. */
typedef struct {
  bool     started;
  bool     stalled;
  bool     silent;
  uint32_t seen_loops;
  uint32_t seen_loops_at_ms;
  uint32_t stalled_since_ms;
  uint32_t last_stall_report_ms;
  uint32_t seen_frames;
  uint32_t frames_changed_at_ms;
  uint32_t last_silent_report_ms;
} health_watch_t;

typedef enum {
  HEALTH_NONE = 0,
  HEALTH_CORE1_STALLED,     /* detail = seconds without progress */
  HEALTH_CORE1_RESUMED,     /* detail = seconds it was stalled */
  HEALTH_WIFI_SILENT,       /* detail = seconds since the last frame */
  HEALTH_WIFI_HEARD,        /* traffic is back */
} health_event_t;

/* The once-a-second gate health_poll() runs in front of health_watch_step():
   true, and moves *next_ms on, when a second has passed since the last true.
   Start *next_ms at 0: the first call is always due.
   Kept here rather than in health_poll.c so that the host test can run it
   against the firmware's real clock -- this is the code that stopped for good
   once the clock it was given jumped back (see the note on the counters). */
bool health_tick_due(uint32_t *next_ms, uint32_t now_ms);

/* Call about once a second with a fresh snapshot and the time since boot.
   Returns the one event worth reporting now, or HEALTH_NONE. `detail` is
   set for every event except HEALTH_NONE and HEALTH_WIFI_HEARD. */
health_event_t health_watch_step(health_watch_t *w, const health_snapshot_t *s,
                                 uint32_t now_ms, uint32_t *detail);

/* Firmware side (health_poll.c): sample, decide, report to the console. Call
   from core 0's BUS_IDLE loop only -- see that file. */
void health_poll(void);

#endif
