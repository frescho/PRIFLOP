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

/* network.c: Wi-Fi config load for the sd2iec_pico2w port (core-0 half).

   CORE-1 OFFLOAD (2026-09-02): the actual
   WiFi/lwIP bring-up and polling that used to live in this file's
   network_init()/network_poll() has moved to src/pico/core1.c, running
   continuously on core 1 instead of being gated on src/iec.c reporting
   the IEC bus idle (see this file's git history for the previous
   single-core version and its reasoning). What's left here is the one
   piece that must still run on core 0: netconfig_load() reads
   /PRIFLOP.CFG via FatFs, and core 1 never touches FatFs directly (see
   network.h's header comment on network_cfg for why, and the approved
   plan for backlog item 4's full design) -- so core 0 loads the config
   here, in network_init(), and hands the result to core 1 via
   network_cfg *before* main.c calls core1_launch(). httpd_set_admin_password()
   also happens here for the same reason: it's config, not FatFs I/O,
   but it's most naturally done right alongside the config load.

   network_poll() still does no networking -- core 0 doesn't poll the
   stack at all, core 1's own loop (core1.c) does. Since 2026-09-20 it is
   used for something else: core 0 is the one place that stays alive when
   core 1 or the Wi-Fi link wedges, so the call site in iec.c's BUS_IDLE loop
   now runs the observer in health.h, which writes what it sees to the boot
   log. (Until then this function was empty and kept only because
   system.h's weak default lives in iec.c.)
*/
#include "config.h"
#include "system.h"
#include "uart.h"

#include "network.h"
#include "httpd.h"
#include "netconfig.h"
#include "health.h"

net_config_t network_cfg;

void network_init(void) {
  netconfig_load(&network_cfg);
  httpd_set_admin_password(network_cfg.admin_password);
}

/* Still no networking here -- core 1 does that. What core 0 does is watch
   core 1 and the Wi-Fi link from outside and put it in the boot log, because
   when the web UI is gone that is the only place left to look. See health.h;
   this runs from the BUS_IDLE loop, where console output is allowed. */
void network_poll(void) {
  health_poll();
}
