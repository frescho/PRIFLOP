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

/* dhcpserver.h: minimal DHCP server for the sd2iec_pico2w AP-mode Wi-Fi
   (see network.c). lwIP itself only ships a DHCP *client* -- running as
   an access point means we're the one handing out addresses to whatever
   connects (a laptop, typically), so something has to speak DHCP server
   side. Not part of the Pico SDK proper (only Raspberry Pi's separate
   pico-examples repo has an equivalent, as vendored example code, not an
   SDK library) -- hand-rolled here to match this codebase's existing
   "no big frameworks" style, same as httpd.c.
*/
#ifndef DHCPSERVER_H
#define DHCPSERVER_H

#include "lwip/ip4_addr.h"

struct netif;

/* Starts serving DHCP on the given interface. `ip`/`netmask` must match
   whatever was already set on that interface (network.c sets a static
   AP address before calling this -- a DHCP *server* doesn't get its own
   address from DHCP). Leases are handed out from ip+1 upward within the
   netmask, for as many clients as DHCP_SERVER_MAX_LEASES allows. `netif`
   restricts the server's socket to that interface (via udp_bind_netif())
   -- matters once a second netif could exist (e.g. Phase 4's original
   STA-mode plan alongside this AP), so it doesn't answer/leak requests
   on the wrong one; harmless with only one netif, as today. */
void dhcp_server_init(const ip4_addr_t *ip, const ip4_addr_t *netmask,
                       struct netif *netif);

#endif
