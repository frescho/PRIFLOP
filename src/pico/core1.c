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


   core1.c: RP2350's second core ("core 1
   WiFi/HTTP offload" -- see the approved plan for the full design).

   STEP 3 of the staged rollout (step 1: a smoke test proving
   multicore_launch_core1() itself works, in isolation. step 2: WiFi/
   lwIP/AP/DHCP running here instead of on the old single-core design,
   verified separately before this one). httpd_init() now runs here too
   -- LIST/MKDIR/RENAME/DELETE go through src/pico/fs_bridge.h/.c so
   this core still never touches FatFs directly; streaming file
   download/upload are temporarily stubbed to 501 in httpd.c until the
   bridge grows the open-handle support that needs (its own follow-up
   step, see the approved plan). httpd_poll() must be polled here just
   like cyw43_arch_poll() -- it drains fs_bridge responses for pending
   connections and services the Reset/BOOTSEL buttons' deferred actions.

   cyw43_arch_init() ALSO moved here (was in system.c's leds_init(),
   called early in main() on core 0) -- confirmed on real hardware that
   splitting cyw43_arch_init() (core 0) from the actual AP/DHCP/poll
   calls (core 1, this file) broke DHCP while leaving basic AP
   association working: pico_cyw43_driver internally asserts
   get_core_num() == async_context_core_num(...), and this build's
   -DNDEBUG strips that assert instead of catching the violation loudly.
   Every cyw43 call -- init included -- has to happen on the same core
   now. See system.c's leds_init()/led_sync_pending() comments for the
   LED-status side of this same move.

   CROSS-CORE FLASH SAFETY: tried routing ata.c/arch-eeprom.c's flash
   writes through pico_flash's flash_safe_execute() (which needs this
   core to call flash_safe_execute_core_init() too), reverted 2026-09-03
   -- confirmed on real hardware that flash_safe_execute() badly disrupts
   core 0's real-time IEC bus timing, so both files are back to a plain
   save_and_disable_interrupts() and this core no longer needs to
   register at all. See ata.c's commit_slot() comment for the full story.

   CONFIG HANDOFF: network_cfg (network.h) is fully populated by core
   0's network_init() (which reads /PRIFLOP.CFG via FatFs) before
   main.c calls core1_launch() -- core 1 doesn't start executing until
   that call, so this is a safe one-time handoff, not a live race. */

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "uart.h"

#include "network.h"
#include "netconfig.h"
#include "dhcpserver.h"
#include "httpd.h"
#include "flash_lockout.h"
#include "health.h"

#include "hardware/regs/addressmap.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

/* Not including system.h here -- see the identical note in the previous
   revision of this file (and uart_hw_shim.h) for why: it declares
   disable_interrupts()/enable_interrupts() in a way that collides with
   hardware/sync.h's static inline functions of the same name, pulled in
   transitively by pico/multicore.h. A local prototype is enough --
   _led_state/_led_state_dirty (arch-config.h, pulled in via config.h)
   don't have this problem, they're plain extern volatile bools. */
void core1_launch(void);

/* How long to wait for a station-mode join before giving up and falling
   back to AP mode. Generous: a slow router or a busy 2.4GHz band can
   take several seconds, and the cost of being wrong is a device that
   looks bricked, whereas the cost of waiting is a one-off boot delay. */
#ifndef WIFI_STA_CONNECT_TIMEOUT_MS
#define WIFI_STA_CONNECT_TIMEOUT_MS 20000
#endif

/* How many times to try the join before giving up and starting the AP.
   One attempt is too brittle: cyw43_arch_wifi_connect_*() retries
   internally only for CYW43_LINK_NONET, and returns immediately on
   BADAUTH -- so a single transient rejection from the AP (seen on a
   MikroTik after many rapid re-associations, with a config and password
   confirmed correct and other clients connecting fine) dropped the
   device into AP mode for the whole session. Retrying costs nothing when
   the first attempt succeeds. */
/* NOTE: this is multiplied by the auth ladder length below, so the number
   of actual join attempts is twice this -- 8 here means 16 tries. Worth
   spelling out because the name reads like a total and is not one.

   16 rather than the previous 12 because the arithmetic changed. An
   attempt now costs about 3.5s (roughly 3s for the AP to answer plus a
   flat 500ms backoff) instead of up to 8s, so sixteen tries fit in about
   a minute where twelve used to take longer than that -- and the outcome
   of each is an independent coin flip, so more tries is simply a better
   chance of catching a good one. The thing being avoided is the AP-mode
   fallback, which costs a person walking over to the device. */
#ifndef WIFI_STA_CONNECT_ATTEMPTS
#define WIFI_STA_CONNECT_ATTEMPTS 8
#endif

/* Backoff between attempts, growing to a cap. A rejected join returns
   almost immediately (BADAUTH does not wait out the timeout), so without
   a deliberate pause the whole ladder burns through in a few seconds and
   gives an AP that is merely slow no chance at all -- which is what was
   happening: the same network that refused every attempt one minute
   accepted the connection when given longer. Total wall time here is
   roughly 45s before falling back to AP mode. */
/* Flat and short, because the failure is a per-attempt coin flip rather
   than the AP needing to settle. Measured 2026-09-06: every attempt
   resolves in about 3s, BADAUTH or NOIP, with no dependence on how long
   the previous one waited -- so the old 1s..5s growing ladder spent 15s
   of a 45s budget doing nothing. At 500ms the same six attempts cost 3s
   of backoff instead, and more attempts fit before the AP fallback. */
#define WIFI_STA_RETRY_BACKOFF_MS     500
#define WIFI_STA_RETRY_BACKOFF_MAX_MS 500

/* Moved from system.c -- see that file's leds_init() comment. Pushes
   set_led()/toggle_led()'s in-RAM shadow (arch-config.h; settable from
   core 0's ISR context, since that's cheap and IRQ-safe) out to the
   real LED. Must run on this core now that cyw43_arch_init() does --
   see this file's header. Still not wired into anything that calls it
   repeatedly ("LED isn't a live status
   indicator") except this file's own idle loop below, which finally
   closes that gap as a side effect of this move. */
static bool led_hw_ok = false;

/* Not static -- arch-config.h declares this with external linkage
   (matching every other arch's led_sync_pending()), so this must match
   or the two declarations conflict at compile time. */
void led_sync_pending(void) {
  if (!led_hw_ok || !_led_state_dirty) {
    return;
  }
  _led_state_dirty = false;
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, _led_state);
}

/* Set by core 0's IEC state machine (src/iec.c) for the duration of a bus
   transaction. */
extern volatile bool iec_bus_busy;

/* Must live in RAM, not flash -- see the call site in core1_entry().

   Bounded deliberately. A real IEC transaction is milliseconds, so this
   timeout should never be reached; it exists so that a bug which leaves
   iec_bus_busy stuck true degrades into "core 1 contends a bit again"
   rather than "core 1 never runs httpd again", which is what an
   unbounded wait cost us once already (2026-09-04, a stray ATN edge
   latched the flag while core 0 sat in BUS_IDLE's ATN wait loop, and the
   web UI hung mid-request forever). The flag itself is fixed; this keeps
   the failure mode survivable if it ever regresses. */
#define CORE1_BUS_GATE_TIMEOUT_US 100000

/* Corrected 2026-09-20: this was marked __not_in_flash_func() and was NOT in
   RAM. It is static and called once, so the compiler inlined it into
   core1_entry(), which is flash -- checked by building the committed source
   with the same flags: no symbol of its own. And even a RAM copy would have
   spun on flash, because time_reached() calls time_us_64(), a flash function,
   every pass. So: __no_inline_not_in_flash_func keeps it a real function in
   RAM, and the timeout is measured with time_us_32(), which is a single read
   of a hardware register. The loop now touches nothing but iec_bus_busy and
   that register. Check with `nm`: 0x2000xxxx is RAM, 0x1000xxxx is flash. */
static void __no_inline_not_in_flash_func(core1_wait_for_bus_idle)(void) {
  const uint32_t start = time_us_32();
  while (iec_bus_busy) {
    if ((uint32_t)(time_us_32() - start) >= CORE1_BUS_GATE_TIMEOUT_US) {
      /* Counted (health.h): under a long, unbroken transfer this is how
         often core 1 gave up waiting and went on to poll anyway. */
      health.core1_gate_timeouts++;
      return;
    }
    tight_loop_contents();
  }
}

/* --- What core 1 reports about itself (health.h) ---------------------------

   Core 1's stack was 2048 bytes and is now PICO_CORE1_STACK_SIZE = 4096
   (scripts/pico/CMakeLists.txt). The receive path can be deep: lwIP's mDNS
   responder keeps 1100 bytes of locals in mdns_handle_response() -- run for
   every mDNS response ANY host on the LAN multicasts -- and 2060 in
   mdns_handle_probe_tiebreaking(), which by itself was bigger than the whole
   old stack. Summed from the disassembly, the plain call chain from
   core1_entry() to mdns_handle_response() is already 1768 bytes before the
   280 that check_request() adds. The memory below the old stack happens to be
   unused (the top of main SRAM, under the C heap), so an overflow landed
   harmlessly; that was luck, not design.

   The stack is painted once, and the deepest point it has reached is read
   back from time to time (/api/stats: c1_stack_used of c1_stack_size). It
   answers, at last, how deep the path really goes -- and a result equal to the
   whole stack would mean even 4096 was not enough. */
#define CORE1_STACK_PAINT 0xC1C1C1C1u

static uint32_t core1_stack_base;
static uint32_t core1_stack_top;

static void core1_stack_paint(void) {
  uint32_t sp;
  __asm volatile ("mov %0, sp" : "=r"(sp));
  /* The SDK places core 1's stack (.stack1) at the start of SCRATCH_X,
     size-aligned. If that ever stops being true, measure nothing rather than
     paint memory that belongs to someone else. */
  uint32_t top  = (sp + (PICO_CORE1_STACK_SIZE - 1u)) & ~(uint32_t)(PICO_CORE1_STACK_SIZE - 1u);
  uint32_t base = top - PICO_CORE1_STACK_SIZE;
  if (base < SRAM8_BASE || top > SRAM9_BASE || sp < base + 128u)
    return;
  /* Interrupts off while painting: an exception taken now would push its
     frame onto the very words being overwritten -- the SIO FIFO interrupt
     that core 0's flash lockout uses is enabled by this point. */
  uint32_t irq = save_and_disable_interrupts();
  for (uint32_t *p = (uint32_t *)base; (uint32_t)p + 64u < sp; p++)
    *p = CORE1_STACK_PAINT;
  restore_interrupts(irq);
  core1_stack_base = base;
  core1_stack_top  = top;
}

static void core1_stack_measure(void) {
  if (!core1_stack_top)
    return;
  const uint32_t *p = (const uint32_t *)core1_stack_base;
  while ((uint32_t)p < core1_stack_top && *p == CORE1_STACK_PAINT)
    p++;
  uint32_t used = core1_stack_top - (uint32_t)p;
  if (used > health.core1_stack_used)
    health.core1_stack_used = used;
}

/* Counts every frame the chip hands to lwIP, on either interface, then passes
   it on. cyw43_lwip.c delivers frames with netif->input(), so replacing that
   pointer is enough and nothing in the SDK has to change.

   WHEN it can be replaced matters, and the first version got it wrong: the
   netif is not created by cyw43_arch_init() but by cyw43_wifi_set_up() -- i.e.
   by cyw43_arch_enable_sta_mode() / enable_ap_mode() -- through
   cyw43_cb_tcpip_init(), and it is created again whenever the interface is
   brought down and up. Installed straight after cyw43_arch_init(), the input
   pointer was still NULL, nothing was installed, wifi_rx stayed 0 and the
   observer reported "wifi UP but no frame for 180s" on a device that was
   serving HTTP. So this is idempotent and runs on every pass of the main loop:
   whatever input pointer the netif has right now, if it is not ours, take it. */
static netif_input_fn wifi_orig_input[2];

static err_t wifi_counting_input(struct pbuf *p, struct netif *nif) {
  health.wifi_rx_frames++;
  health.wifi_rx_last_ms = to_ms_since_boot(get_absolute_time());
  return wifi_orig_input[nif == &cyw43_state.netif[CYW43_ITF_AP] ? CYW43_ITF_AP : CYW43_ITF_STA](p, nif);
}

static void wifi_count_frames(void) {
  for (int i = 0; i < 2; i++) {
    struct netif *n = &cyw43_state.netif[i];
    if (n->input && n->input != wifi_counting_input) {
      wifi_orig_input[i] = n->input;
      n->input = wifi_counting_input;
    }
  }
}

/* Advertise <mdns_name>.local and an _http._tcp record, so the device can
   be reached by name instead of by whatever address DHCP handed out.

   STATION MODE ONLY, deliberately. An earlier build recorded this
   as "genuinely broken and reverted": mdns_resp_add_netif() joins an IPv4
   multicast group, and doing that on the cyw43 AP-mode netif broke
   association and DHCP outright, whether called immediately or deferred.
   That result stands -- but it was measured before station mode existed,
   and it was specific to the AP netif. The STA netif is a different
   interface in a different driver state, and it is where mDNS is actually
   useful. So this attaches only there, and AP mode stays exactly as it
   was: no responder, no IGMP join. */
static void start_mdns(struct netif *nif, const char *name) {
  static bool started = false;
  if (started || !nif || !name || !name[0]) {
    return;
  }
  mdns_resp_init();
  if (mdns_resp_add_netif(nif, name) != ERR_OK) {
    uart_puts_P(PSTR("mDNS: add_netif failed, continuing without it\r\n"));
    return;
  }
  mdns_resp_add_service(nif, name, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
  mdns_resp_announce(nif);
  started = true;
}

/* Re-join a station link that goes away mid-session.
 *
 * The join ladder above runs once, at boot. After that a link that drops
 * -- the AP rebooting, the device carried out of range, or simply the
 * flakiness this network has shown more than once in testing -- left the
 * drive working and the web UI gone until a power cycle. Nothing noticed,
 * because nothing was looking.
 *
 * Only in station mode: in AP mode this device *is* the network, and
 * there is no link to lose.
 *
 * Deliberately unhurried. cyw43_arch_wifi_connect_timeout_ms() blocks
 * this core for as long as it takes, and this core also serves the web UI
 * and, through core1_wait_for_bus_idle(), must stay out of the way of the
 * C64. Checking every few seconds and backing off to half a minute costs
 * nothing when the link is healthy and does not turn a flapping AP into a
 * busy loop. */
#define WIFI_LINK_CHECK_MS      5000
#define WIFI_REJOIN_BACKOFF_MS  5000
#define WIFI_REJOIN_MAX_MS     30000

/* Result of picking an access point -- see wifi_pick_best_ap() below. */
typedef struct {
  const char *want_ssid;
  uint8_t     bssid[6];
  int16_t     rssi;
  uint16_t    channel;
  uint8_t     auth_mode;   /* what the AP advertises, see below */
  bool        found;
} wifi_pick_t;

static bool wifi_pick_best_ap(const char *ssid, wifi_pick_t *pick);

static const uint8_t *wifi_pick_bssid(wifi_pick_t *pick) {
  return pick->bssid;
}

static void wifi_check_link(const net_config_t *cfg, bool sta) {
  static uint32_t next_check_ms = WIFI_LINK_CHECK_MS;
  static uint32_t backoff_ms    = WIFI_REJOIN_BACKOFF_MS;
  static bool     was_down      = false;
  char msg[128];

  if (!sta)
    return;                       /* AP mode, or never joined at all */

  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (now < next_check_ms)
    return;

  int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  health.wifi_link_status = st;
  if (st == CYW43_LINK_UP) {
    if (was_down) {
      struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
      snprintf(msg, sizeof(msg), "WiFi: link back, http://%s\r\n",
               ip4addr_ntoa(netif_ip4_addr(nif)));
      uart_puts_P(msg);
      was_down = false;
    }
    backoff_ms    = WIFI_REJOIN_BACKOFF_MS;
    next_check_ms = now + WIFI_LINK_CHECK_MS;
    return;
  }

  if (!was_down) {
    snprintf(msg, sizeof(msg), "WiFi: link lost (status=%d), rejoining\r\n", st);
    uart_puts_P(msg);
    was_down = true;
    health.wifi_link_drops++;
  }

  uint32_t auth = cfg->password[0] ? CYW43_AUTH_WPA2_MIXED_PSK
                                   : CYW43_AUTH_OPEN;

  /* Re-scan on every rejoin rather than reusing the AP chosen at boot.

     This is a CAPsMAN network: several access points share the SSID and
     the controller may take one out of service at any time. Reusing a
     stale BSSID would turn "the AP I liked went away" into a permanent
     failure, and an undirected rejoin -- which is what this used to do --
     goes straight back to the coin flip the boot-time scan exists to
     avoid. The router's log shows exactly that cycle: dropped by the
     near AP, reconnected to the far one at -70, dropped again.

     Scanning costs a few seconds, but the link is already down when this
     runs, so there is nothing to delay. */
  static wifi_pick_t rejoin_pick;
  const uint8_t *bssid = wifi_pick_best_ap(cfg->ssid, &rejoin_pick)
                       ? wifi_pick_bssid(&rejoin_pick) : NULL;

  int rc = cyw43_arch_wifi_connect_bssid_timeout_ms(
             cfg->ssid, bssid, cfg->password[0] ? cfg->password : NULL, auth,
             WIFI_STA_CONNECT_TIMEOUT_MS);

  now = to_ms_since_boot(get_absolute_time());
  if (rc == 0) {
    next_check_ms = now + WIFI_LINK_CHECK_MS;   /* the UP branch reports it */
    backoff_ms    = WIFI_REJOIN_BACKOFF_MS;
  } else {
    next_check_ms = now + backoff_ms;
    backoff_ms   *= 2;
    if (backoff_ms > WIFI_REJOIN_MAX_MS)
      backoff_ms = WIFI_REJOIN_MAX_MS;
  }
}


/* Pick which access point to join, rather than letting the chip decide.

   THE PROBLEM THIS SOLVES, from the router's own log on 2026-09-06:

       16:45:34  cap-wifi6  disconnected, not responding   -41
       16:46:07  cap-wifi5  connected                      -70
       16:46:47  cap-wifi5  disconnected, not responding   -68
       16:47:05  cap-wifi6  connected                      -38

   Two access points, one SSID, roughly 30 dB apart, and the drive was
   bouncing between them. Landing on the far one produced a marginal
   association that the AP later dropped as "not responding" -- and an
   undirected join picks whichever the chip happens to like at that
   instant, which is precisely the coin flip that showed up as
   "half of first attempts fail". It was never a credentials problem, and
   no amount of retry tuning could have fixed it: the retries were
   re-rolling the same dice.

   So: scan, take the strongest BSSID whose SSID matches, and join THAT
   one specifically with cyw43_arch_wifi_connect_bssid_async(). On a
   single-AP network this changes nothing except adding one scan.

   Deliberately scanned once per boot rather than per attempt: a scan
   costs seconds, and if the strongest AP rejects us the answer is to try
   again, not to go looking for a weaker one. */
#ifndef WIFI_SCAN_TIMEOUT_MS
#define WIFI_SCAN_TIMEOUT_MS 6000
#endif

static int wifi_scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
  wifi_pick_t *pick = (wifi_pick_t *)env;

  if (!r || r->ssid_len == 0 || r->ssid_len > 32)
    return 0;
  if (strlen(pick->want_ssid) != r->ssid_len ||
      memcmp(pick->want_ssid, r->ssid, r->ssid_len) != 0)
    return 0;

  /* Strongest wins. RSSI is negative dBm, so "greater" is "closer". */
  if (!pick->found || r->rssi > pick->rssi) {
    memcpy(pick->bssid, r->bssid, 6);
    pick->rssi      = r->rssi;
    pick->channel   = r->channel;
    pick->auth_mode = r->auth_mode;
    pick->found     = true;
  }
  return 0;
}

/* Returns true and fills `pick` if an AP for this SSID was seen. */
static bool wifi_pick_best_ap(const char *ssid, wifi_pick_t *pick) {
  char msg[96];
  cyw43_wifi_scan_options_t opts;

  memset(pick, 0, sizeof(*pick));
  pick->want_ssid = ssid;

  memset(&opts, 0, sizeof(opts));
  if (cyw43_wifi_scan(&cyw43_state, &opts, pick, wifi_scan_cb) != 0) {
    uart_puts_P(PSTR("WiFi: scan failed, joining undirected\r\n"));
    return false;
  }

  absolute_time_t deadline = make_timeout_time_ms(WIFI_SCAN_TIMEOUT_MS);
  while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(deadline)) {
    cyw43_arch_poll();
    sleep_ms(20);
  }

  if (!pick->found) {
    uart_puts_P(PSTR("WiFi: scan saw no AP for this SSID, joining "
                     "undirected\r\n"));
    return false;
  }

  /* auth_mode is logged because the join keeps failing with BADAUTH on
     credentials that are demonstrably correct, and the ladder this code
     offers (WPA2-AES then WPA2-MIXED) is a guess. What the AP actually
     advertises is the thing to compare it against -- and if it does not
     match either, that is the answer. */
  snprintf(msg, sizeof(msg),
           "WiFi: best AP %02x:%02x:%02x:%02x:%02x:%02x ch%u %ddBm auth=0x%02x\r\n",
           pick->bssid[0], pick->bssid[1], pick->bssid[2],
           pick->bssid[3], pick->bssid[4], pick->bssid[5],
           (unsigned)pick->channel, (int)pick->rssi,
           (unsigned)pick->auth_mode);
  uart_puts_P(msg);
  return true;
}

/* Join, and actually honour the timeout.

   cyw43_arch_wifi_connect_timeout_ms() does not. Its loop is

       while (status >= 0 && status != CYW43_LINK_UP) { ... }

   and CYW43_LINK_BADAUTH is NEGATIVE (-3), so the moment the driver
   reports a bad auth the loop exits and the function returns
   PICO_ERROR_BADAUTH -- immediately, with the timeout untouched. Passing
   it 20000ms bought nothing at all on the failure path this device
   actually hits (pico-sdk 2.1.1, cyw43_arch.c).

   That matters because a BADAUTH here is frequently transient rather than
   a wrong password. The cyw43 driver carries explicit handling for "a
   good-auth follows a bad-auth" (georgerobotics/cyw43-driver issue 62),
   and this drive's own boot logs show it plainly: the same SSID, password
   and auth mode that report BADAUTH on one attempt connect on the next,
   with nothing changed in between. On 2026-09-06 one boot burned twelve
   attempts and fell back to AP mode; the next connected on the ninth.

   So instead of asking the SDK to block, this issues the async connect
   once and polls the link itself, giving a BADAUTH a bounded grace period
   before believing it.

   The grace period for a BADAUTH is deliberately ZERO, and that is a
   measured result that contradicts the theory this function was written
   for. Tolerating BADAUTH was supposed to ride out a transient one. It
   does not, because on this AP a BADAUTH is never transient WITHIN an
   attempt -- instrumented on 2026-09-06:

       13.4s  link status 1  (JOIN)
       16.4s  link status -3 (BADAUTH)   exactly 3.0s later
       19.4s  attempt failed             3s of grace, wasted
       ... four more identical attempts ...
       51.4s  link status 1  (JOIN)
       54.5s  link status 2  (NOIP)      3.1s later, succeeded

   The AP answers in about three seconds every time and the outcome is a
   coin flip. The randomness is BETWEEN attempts, not within one, so
   waiting after a BADAUTH is pure delay -- a 20s grace turned a 45s
   recovery into 132s, and even 3s cost six seconds per attempt.

   What is worth keeping from this is the polling itself: the SDK helper
   hides the link-status transitions, and those transitions are the only
   reason any of the above is knowable. The timeout still needs to be ours
   because the SDK's is not honoured on the BADAUTH path.

   So: fail the attempt the moment BADAUTH appears, and retry quickly.

   CYW43_LINK_NONET is re-issued rather than waited on, matching what the
   SDK helper does for that case: no network in range means the request
   itself needs repeating. */
#ifndef WIFI_BADAUTH_GRACE_MS
#define WIFI_BADAUTH_GRACE_MS 0
#endif

static const uint8_t *wifi_target_bssid;   /* NULL = let the chip choose */

static int wifi_join_wait(const char *ssid, const char *pw, uint32_t auth,
                          uint32_t timeout_ms) {
  char msg[96];
  absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
  absolute_time_t badauth_deadline = nil_time;
  int last_logged = 99;

  int rc = cyw43_arch_wifi_connect_bssid_async(ssid, wifi_target_bssid,
                                               pw, auth);
  if (rc)
    return rc;

  for (;;) {
    cyw43_arch_poll();

    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (st == CYW43_LINK_UP)
      return 0;

    if (st != last_logged) {
      /* Logged because these transitions were completely invisible before,
         and they are what distinguishes "wrong password" from "this AP is
         slow to associate". */
      snprintf(msg, sizeof(msg), "WiFi: link status %d\r\n", st);
      uart_puts_P(msg);
      last_logged = st;
    }

    if (st == CYW43_LINK_NONET) {
      rc = cyw43_arch_wifi_connect_bssid_async(ssid, wifi_target_bssid,
                                               pw, auth);
      if (rc)
        return rc;
    }

    if (st == CYW43_LINK_BADAUTH) {
      /* Start the clock on the first BADAUTH; give up on this attempt if
         it is still there when the grace expires. */
      if (is_nil_time(badauth_deadline))
        badauth_deadline = make_timeout_time_ms(WIFI_BADAUTH_GRACE_MS);
      if (time_reached(badauth_deadline))
        return PICO_ERROR_BADAUTH;
    } else {
      badauth_deadline = nil_time;   /* recovered -- it was transient */
    }

    if (time_reached(deadline))
      return PICO_ERROR_TIMEOUT;

    sleep_ms(50);
  }
}

/* Bug fix, 2026-09-07. wifi_check_link() above returns immediately
   whenever sta is false ("AP mode, or never joined at all"), so once
   the boot-time join fails and the device falls back to AP mode,
   nothing ever attempts station mode again for the rest of that boot --
   confirmed on real hardware repeatedly this session, each time needing
   a full manual reboot to get a fresh attempt.

   This is the fix: a periodic, lighter-weight retry that runs only
   while still in AP fallback (sta_connected false), reusing the same
   AP-selection and join primitives as the boot-time attempt via
   wifi_join_wait() and wifi_pick_best_ap() rather than duplicating that
   whole block verbatim. Deliberately a single pass through the auth
   ladder per call, not WIFI_STA_CONNECT_ATTEMPTS retries internally --
   the periodic cadence below is itself the retry loop, and this runs on
   core 1 alongside httpd and bus-idle checking, so it should not tie
   the core up for longer than one join attempt needs.

   Deliberately does NOT tear down the AP on success: the cyw43 chip
   supports STA+AP concurrently, and leaving the AP up alongside a newly
   -recovered station link is a safe, inert outcome. Cleanly tearing the
   AP down mid-operation once STA recovers would be a real improvement
   but is a separate, riskier change not attempted here -- this fix's
   scope is "stop needing a manual reboot," not "make the transition
   cosmetically clean."

   Unverified on real hardware as of this commit: whether a station join
   attempted after cyw43_arch_enable_ap_mode() has already run (rather
   than fresh at boot, before AP was ever brought up) behaves identically
   to the boot-time path. Concurrent STA+AP is a documented chip
   capability, but this specific sequencing -- join attempted *after* AP
   is already active -- had not been exercised by this codebase before
   this fix. */
#define WIFI_AP_REJOIN_RETRY_MS 30000

static bool wifi_retry_join_from_ap(net_config_t *cfg) {
  char msg[128];
  bool use_wpa2 = cfg->password[0] != 0;

  wifi_pick_t pick;
  wifi_target_bssid = wifi_pick_best_ap(cfg->ssid, &pick) ? wifi_pick_bssid(&pick) : NULL;
  const uint8_t pick_auth = pick.found ? pick.auth_mode : 0;

  static const uint32_t auth_ladder[] = {
    CYW43_AUTH_WPA2_AES_PSK,
    CYW43_AUTH_WPA2_MIXED_PSK,
  };
  int ladder_len = use_wpa2
      ? (int)(sizeof(auth_ladder) / sizeof(auth_ladder[0])) : 1;
  /* Narrow to WPA2-AES-only if the scan says the AP is WPA2-only --
     same heuristic as the boot-time join, see its comment for why. */
  if (wifi_target_bssid && (pick_auth & 0x04) && !(pick_auth & 0x02))
    ladder_len = 1;

  uart_puts_P(PSTR("WiFi: retrying station join from AP fallback\r\n"));

  for (int i = 0; i < ladder_len; i++) {
    uint32_t try_auth = use_wpa2 ? auth_ladder[i] : CYW43_AUTH_OPEN;
    int rc = wifi_join_wait(cfg->ssid, use_wpa2 ? cfg->password : NULL,
                            try_auth, WIFI_STA_CONNECT_TIMEOUT_MS);
    if (rc == 0) {
      struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
      snprintf(msg, sizeof(msg),
               "WiFi: rejoined from AP fallback, browse to http://%s\r\n",
               ip4addr_ntoa(netif_ip4_addr(sta_netif)));
      uart_puts_P(msg);
      start_mdns(sta_netif, cfg->mdns_name);
      snprintf(msg, sizeof(msg), "mDNS: also reachable as http://%s.local\r\n",
               cfg->mdns_name);
      uart_puts_P(msg);
      return true;
    }
  }
  uart_puts_P(PSTR("WiFi: retry from AP fallback failed, staying in AP mode\r\n"));
  return false;
}

static void core1_entry(void) {
  /* Register FIRST, before anything that could take a while. Core 0 must
     be able to park this core from its very first instruction here:
     cyw43_arch_init() and the blocking station-mode join below both run
     for a long time inside SDK/driver code executing from flash, and a
     commit landing during either of those would otherwise kill this core.
     See flash_lockout.h. */
  multicore_lockout_victim_init();

  core1_stack_paint();

  /* Country code from PRIFLOP.CFG, empty meaning worldwide.

     Worldwide is the SDK default and the most conservative regulatory
     domain the chip has: it restricts channels and forces PASSIVE
     scanning on the ones it is unsure about -- listening for beacons
     rather than probing for them. The SDK's own header says worldwide
     "may not give the best performance" and points at this call.

     That matters here because of what the boot logs showed: roughly half
     of first association attempts rejected with BADAUTH on credentials
     that are demonstrably correct, and a first attempt taking ~8.7s
     against ~3s for every later one. A slow first attempt followed by
     intermittent rejection is the shape of a scan problem, not a
     credentials problem, and passive scanning is the most likely reason
     the chip takes that long to find an AP it has not seen yet. */
  {
    /* network_cfg is fully populated by core 0 before this core starts --
       see the CONFIG HANDOFF note at the top of this file. */
    const net_config_t *ccfg = &network_cfg;
    if (ccfg && ccfg->country[0]) {
      char msg[64];
      snprintf(msg, sizeof(msg), "WiFi: regulatory domain %s\r\n",
               ccfg->country);
      uart_puts_P(msg);
      led_hw_ok = (cyw43_arch_init_with_country(
                     CYW43_COUNTRY(ccfg->country[0], ccfg->country[1], 0)) == 0);
    } else {
      uart_puts_P(PSTR("WiFi: regulatory domain worldwide (set COUNTRY= in "
                       "PRIFLOP.CFG for faster, more reliable joins)\r\n"));
      led_hw_ok = (cyw43_arch_init() == 0);
    }
  }
  if (led_hw_ok) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    /* Power saving off. The chip defaults to CYW43_PERFORMANCE_PM, which
       buys battery life this device does not need -- it is a disk drive
       wired to a C64, permanently mains-powered -- and costs reliability
       on access points that are sensitive to powersave DTIM buffering,
       which is a documented cause of exactly the intermittent
       association and stall failures seen here. Nothing to trade away. */
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);
  } else {
    uart_puts_P(PSTR("WARNING: cyw43_arch_init() failed (core 1), "
                      "WiFi and status LED both unavailable\r\n"));
    for (;;) {
      tight_loop_contents();
    }
  }

  net_config_t *cfg = &network_cfg;
  /* WPA2-PSK needs >= 8 chars; anything shorter means "open network".
     Computed separately per mode now that joining and broadcasting have
     their own credentials (see netconfig.h). */
  bool use_wpa2 = strlen(cfg->password) >= 8;
  bool ap_wpa2  = strlen(cfg->ap_password) >= 8;
  bool sta_connected = false;
  uint32_t auth = use_wpa2 ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;

  /* One buffer, one uart_puts_P() call -- see main.c's identical fix and
     uart.c's console_lock comment for why: this used to be built from
     several separate uart_puts_P() calls, each individually locked, so
     core 0's own boot banner could (and, confirmed on real hardware,
     did) land in the middle of this message. */
  char msg[128];

  /* Station mode: join an existing network instead of being one. Falls
     back to AP below if the join fails, so a wrong password or an
     out-of-range network can never leave the device unreachable -- the
     only way back in would otherwise be re-flashing or pulling the
     config file off the flash volume by hand.

     cyw43_arch_wifi_connect_timeout_ms() blocks, but it calls
     cyw43_arch_poll() internally (SDK cyw43_arch.c), so it works under
     pico_cyw43_arch_lwip_poll. Blocking core 1 here is fine: core 0 owns
     the IEC bus and is unaffected, and this only happens once at boot. */
  if (!cfg->ap_mode) {
    snprintf(msg, sizeof(msg), "\r\nWiFi: joining network \"%s\"...\r\n",
             cfg->ssid);
    uart_puts_P(msg);

    cyw43_arch_enable_sta_mode();

    /* The station MAC, so the network's own admin tools can be checked
       for a stale registration or an access-list entry for this device --
       the first thing worth looking at when a join is rejected with
       credentials known to be right. */
    uint8_t mac[6];
    if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) == 0) {
      snprintf(msg, sizeof(msg),
               "WiFi: station MAC %02x:%02x:%02x:%02x:%02x:%02x\r\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      uart_puts_P(msg);
    }

    /* Choose the access point before trying to join it -- see
       wifi_pick_best_ap(). On a single-AP network this is one extra scan
       and nothing else changes; on this network it is the whole fix. */
    static wifi_pick_t pick;
    wifi_target_bssid = wifi_pick_best_ap(cfg->ssid, &pick) ? pick.bssid
                                                            : NULL;
    const uint8_t pick_auth = pick.found ? pick.auth_mode : 0;

    /* Try more than one cipher suite. rc=-7 is PICO_ERROR_BADAUTH, which
       the driver reports for a *cipher/auth-mode* mismatch as well as a
       genuinely wrong password -- so offering only WPA2-AES gets a flat
       rejection from an AP configured for WPA2/WPA3 mixed, or one that
       still expects TKIP, even though the password is perfectly correct.
       Observed on a MikroTik that accepted other clients fine while
       rejecting three identical WPA2-AES attempts (2026-09-04).

       WPA-TKIP is deliberately NOT in this list. It was, and it made
       things strictly worse: once a TKIP attempt had run, every
       subsequent attempt returned rc=-2 (PICO_ERROR_GENERIC) instead of
       the honest rc=-7 (BADAUTH), including modes that had just failed
       cleanly -- i.e. it leaves the driver in a state where the remaining
       retries are worthless. Observed on hardware 2026-09-04:

           attempt 1 (AES)   rc=-7      attempt 4 (AES)   rc=-2
           attempt 2 (MIXED) rc=-7      attempt 5 (MIXED) rc=-2
           attempt 3 (TKIP)  rc=-2      attempt 6 (TKIP)  rc=-2

       No loss: WPA1/TKIP is long obsolete, and nothing that only speaks
       TKIP is worth poisoning the retry path for.

       Ordered strongest first; an open network skips the ladder. */
    static uint32_t auth_ladder[] = {
      CYW43_AUTH_WPA2_AES_PSK,
      CYW43_AUTH_WPA2_MIXED_PSK,
    };
    int ladder_len = use_wpa2
        ? (int)(sizeof(auth_ladder) / sizeof(auth_ladder[0])) : 1;

    /* If the scan told us what the AP actually offers, believe it instead
       of alternating guesses.

       The scan's auth_mode is a bitmask built in cyw43_ll.c: bit 0 is
       privacy, bit 1 is WPA (a WPA IE was present), bit 2 is WPA2 (an RSN
       IE was present). This network reports 0x05 -- privacy and WPA2,
       with the WPA bit CLEAR -- so it is WPA2-only, and offering
       WPA2_MIXED_PSK on every second attempt was advertising support for
       a legacy protocol the AP does not run. Half the retries were spent
       on a mode that could not have been right.

       Only narrows the ladder, never widens it: if the scan found nothing
       or the AP reports WPA as well, the original pair is kept. */
    if (wifi_target_bssid && (pick_auth & 0x04) && !(pick_auth & 0x02)) {
      uart_puts_P(PSTR("WiFi: AP advertises WPA2 only, using WPA2-AES\r\n"));
      ladder_len = 1;                    /* WPA2_AES_PSK, the first entry */
    }

    int rc = 0;
    for (int attempt = 0; attempt < WIFI_STA_CONNECT_ATTEMPTS * ladder_len;
         attempt++) {
      uint32_t try_auth = use_wpa2 ? auth_ladder[attempt % ladder_len] : auth;
      rc = wifi_join_wait(cfg->ssid, use_wpa2 ? cfg->password : NULL,
                          try_auth, WIFI_STA_CONNECT_TIMEOUT_MS);
      if (rc == 0) {
        break;
      }
      uint32_t backoff = WIFI_STA_RETRY_BACKOFF_MS;
      snprintf(msg, sizeof(msg),
               "WiFi: attempt %d failed (auth=0x%lx rc=%d), retrying in %lums\r\n",
               attempt + 1, (unsigned long)try_auth, rc,
               (unsigned long)backoff);
      uart_puts_P(msg);
      sleep_ms(backoff);
    }

    if (rc == 0) {
      struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
      snprintf(msg, sizeof(msg),
               "WiFi: connected, browse to http://%s\r\n",
               ip4addr_ntoa(netif_ip4_addr(sta_netif)));
      uart_puts_P(msg);

      sta_connected = true;

      start_mdns(sta_netif, cfg->mdns_name);
      snprintf(msg, sizeof(msg), "mDNS: also reachable as http://%s.local\r\n",
               cfg->mdns_name);
      uart_puts_P(msg);
      /* No dhcp_server_init(): the network's own DHCP server assigned
         us the address above. */
    } else {
      snprintf(msg, sizeof(msg),
               "WiFi: join failed (last rc=%d), falling back to "
               "access point\r\n", rc);
      uart_puts_P(msg);
      cfg->ap_mode = true; /* reflect reality for anything reading it later */
    }
  }

  if (!sta_connected) {
    snprintf(msg, sizeof(msg), "\r\nWiFi: starting access point \"%s\"%s\r\n",
             cfg->ap_ssid, ap_wpa2 ? "" : " (open, no password)");
    uart_puts_P(msg);

    cyw43_arch_enable_ap_mode(cfg->ap_ssid, ap_wpa2 ? cfg->ap_password : NULL,
                              ap_wpa2 ? CYW43_AUTH_WPA2_AES_PSK
                                      : CYW43_AUTH_OPEN);

    /* Keep in sync with the literal in the uart_puts_P() below -- see the
       old network.c's identical comment for the hex-vs-decimal bug this
       note exists to prevent repeating. */
    ip4_addr_t ip, mask;
    IP4_ADDR(&ip, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    struct netif *ap_netif = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(ap_netif, &ip, &mask, &ip);
    netif_set_up(ap_netif);

    dhcp_server_init(&ip, &mask, ap_netif);

    uart_puts_P(PSTR("WiFi: AP up (core 1), connect to it and browse to "
                      "http://192.168.4.1\r\n"));
  }

  httpd_init();

  uint32_t last_pass_ms = 0;
  uint32_t last_stack_ms = 0;
  for (;;) {
    /* Heartbeat for core 0's observer (health.h), and the longest gap between
       two passes -- which includes the up-to-100 ms wait for the bus below and
       any blocking rejoin. */
    uint32_t pass_ms = to_ms_since_boot(get_absolute_time());
    if (last_pass_ms && pass_ms - last_pass_ms > health.core1_gap_max_ms)
      health.core1_gap_max_ms = pass_ms - last_pass_ms;
    last_pass_ms = pass_ms;
    health.core1_loops++;
    wifi_count_frames();
    if (pass_ms - last_stack_ms >= 5000u) {
      last_stack_ms = pass_ms;
      core1_stack_measure();
    }

    /* Stay off the shared XIP/flash path while core 0 has a live bus
       transaction -- see iec.c's iec_bus_busy comment for the full why.
       Deliberately __not_in_flash_func(): a spin loop living in flash
       would itself be fetching across the bus we are trying to keep
       clear, which is the whole problem. A transaction is milliseconds,
       and this is exactly when the pre-item-4 design did no network work
       either, so nothing is starved that wasn't already. */
    core1_wait_for_bus_idle();
    cyw43_arch_poll();
    httpd_poll();
    wifi_check_link(cfg, sta_connected);
    if (!sta_connected) {
      static uint32_t next_ap_rejoin_ms = WIFI_AP_REJOIN_RETRY_MS;
      uint32_t now = to_ms_since_boot(get_absolute_time());
      if (now >= next_ap_rejoin_ms) {
        next_ap_rejoin_ms = now + WIFI_AP_REJOIN_RETRY_MS;
        if (wifi_retry_join_from_ap(cfg))
          sta_connected = true;
      }
    }
    led_sync_pending();
  }
}

void core1_launch(void) {
  multicore_launch_core1(core1_entry);
}
