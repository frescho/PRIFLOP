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

/* network.h: shared Wi-Fi config handoff between core 0 and core 1.

   Core 0 owns FatFs (design decision:
   core 1 never touches FatFs directly), so network_init() -- which
   reads /PRIFLOP.CFG via netconfig_load() -- must run on core 0, even
   though the WiFi bring-up it used to do inline now happens on core 1
   (src/pico/core1.c). network_cfg is the handoff: network_init() fully
   populates it and *only then* does main.c call core1_launch() --
   multicore_launch_core1() doesn't start core 1 executing until that
   call is made, so there's no race to guard here, just a one-time,
   before-core-1-exists write followed by a read-only use afterward. Not
   an instance of the general FatFs bridge backlog item 4 needs for
   httpd.c's ongoing file operations -- this is a single one-shot value,
   not a live request/response protocol.
*/
#ifndef NETWORK_H
#define NETWORK_H

#include "netconfig.h"

extern net_config_t network_cfg;

#endif
