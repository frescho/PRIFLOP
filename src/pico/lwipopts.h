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

/* lwipopts.h: lwIP configuration for the sd2iec_pico2w port.

   NO_SYS=1 / poll mode:
   everything (IEC bus, disk I/O, and now the web
   server) runs cooperatively on core 0, so lwIP is driven the same way
   -- cyw43_arch_poll() is called once per src/iec.c mainloop iteration,
   only when the IEC bus is idle (same safe point already used for
   deferred flash commits, see diskio.h's disk_idle_sync()). This keeps
   the whole system single-threaded: no locking is needed between the
   HTTP file-service code and the shared FatFs state fatops.c/d64ops.c
   use for real IEC transactions, because the two never run at the same
   time, only interleaved in strict sequence.

   Buffer sizes below are deliberately modest (this is a hobby device
   serving one or two clients on a home LAN, not a real server) --
   tuned for "a disk image transfers in a reasonable time" rather than
   raw throughput.
*/
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    20000
/* Not set before, so lwIP used its default of 5 -- which silently capped
   concurrent connections below HTTP_MAX_CONNS however high that was set.
   Kept above HTTP_MAX_CONNS so a slot freed by the httpd is not held up
   by a PCB still in TIME_WAIT. */
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              32

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1

#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3

/* mDNS responder, STATION MODE ONLY -- see core1.c's start_mdns().
   Enabling the library and IGMP is harmless on its own; that was
   established on the device, having confirmed that
   LWIP_IGMP=1 plus the whole mdns.c linked but never invoked changed
   nothing. What broke Wi-Fi was calling mdns_resp_add_netif() on the
   cyw43 *AP* netif, whose IGMP join wrecked association and DHCP. That
   investigation predates station mode, so the responder is now attached
   only to the STA netif and never to the AP one. */
#define LWIP_IGMP                   1
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define MEMP_NUM_UDP_PCB            6
/* The responder arms its own timers (announce, probe, cache flush) on top
   of TCP's, DHCP's and the ARP/IGMP ones. lwIP's computed default did not
   cover them and the first announce panicked with
   "sys_timeout: ... pool MEMP_SYS_TIMEOUT is empty" -- a hard stop, not a
   degraded mDNS. Sized with headroom rather than exactly, since the pool
   is a handful of bytes per entry and running out is fatal. */
#define MEMP_NUM_SYS_TIMEOUT        16

#define LWIP_DEBUG                  0

#endif
