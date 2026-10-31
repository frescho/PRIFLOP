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

/* dhcpserver.c: minimal DHCP server -- see dhcpserver.h for why this
   exists. Implements just enough of RFC 2131 for a laptop/phone to get
   an address from this device's Wi-Fi access point: DISCOVER->OFFER,
   REQUEST->ACK, nothing else (no RENEW/RELEASE handling beyond treating
   every REQUEST the same way, no relay-agent support, no options this
   device doesn't need). A small fixed lease table keyed by MAC address
   is enough for a hobby device serving a handful of clients at most.
*/
#include <string.h>

#include "lwip/udp.h"
#include "lwip/netif.h"

#include "dhcpserver.h"

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY   2

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS           6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_END         255

#define DHCP_MAGIC_COOKIE 0x63825363UL

/* Fixed (non-options) part of a DHCP/BOOTP packet. */
#define DHCP_HDR_OP        0
#define DHCP_HDR_HTYPE     1
#define DHCP_HDR_HLEN      2
#define DHCP_HDR_HOPS      3
#define DHCP_HDR_XID       4   /* 4 bytes */
#define DHCP_HDR_SECS      8   /* 2 bytes */
#define DHCP_HDR_FLAGS    10   /* 2 bytes */
#define DHCP_HDR_CIADDR   12   /* 4 bytes */
#define DHCP_HDR_YIADDR   16   /* 4 bytes */
#define DHCP_HDR_SIADDR   20   /* 4 bytes */
#define DHCP_HDR_GIADDR   24   /* 4 bytes */
#define DHCP_HDR_CHADDR   28   /* 16 bytes */
#define DHCP_HDR_SNAME    44   /* 64 bytes */
#define DHCP_HDR_FILE    108   /* 128 bytes */
#define DHCP_HDR_COOKIE  236   /* 4 bytes */
#define DHCP_HDR_OPTIONS 240

#define DHCP_LEASE_SECONDS (24u * 60u * 60u)
#define DHCP_MAX_LEASES 8

typedef struct {
  bool     in_use;
  uint8_t  mac[6];
  ip4_addr_t ip;
} lease_t;

static struct udp_pcb *dhcp_pcb;
static ip4_addr_t server_ip;
static ip4_addr_t server_mask;
static lease_t leases[DHCP_MAX_LEASES];

static void put_u32(uint8_t *buf, uint32_t v) {
  buf[0] = (uint8_t)(v >> 24);
  buf[1] = (uint8_t)(v >> 16);
  buf[2] = (uint8_t)(v >> 8);
  buf[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *buf) {
  return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
         ((uint32_t)buf[2] << 8) | buf[3];
}

/* Finds `opt` in the options area of a received packet. Returns a
   pointer to its length byte (value follows immediately), or NULL. */
static const uint8_t *find_option(const uint8_t *buf, uint16_t len, uint8_t opt) {
  uint16_t i = DHCP_HDR_OPTIONS;
  while (i < len) {
    uint8_t code = buf[i];
    if (code == DHCP_OPT_END) break;
    if (code == DHCP_OPT_PAD) { i++; continue; }
    if (i + 1 >= len) break;
    uint8_t optlen = buf[i + 1];
    if (code == opt) return &buf[i + 1];
    i += 2 + optlen;
  }
  return NULL;
}

/* Finds an existing lease for this MAC, or claims a free slot for it.
   Returns NULL only if every slot is already leased to a *different*
   MAC -- fine for this device's expected handful of clients. */
static lease_t *find_or_claim_lease(const uint8_t *mac) {
  lease_t *free_slot = NULL;
  for (int i = 0; i < DHCP_MAX_LEASES; i++) {
    if (leases[i].in_use && !memcmp(leases[i].mac, mac, 6)) {
      return &leases[i];
    }
    if (!leases[i].in_use && !free_slot) {
      free_slot = &leases[i];
    }
  }
  if (free_slot) {
    memcpy(free_slot->mac, mac, 6);
    /* Address = server_ip + (slot index + 1), i.e. .2, .3, .4, ... for
       a typical /24 -- simple and sufficient for a small fixed pool. */
    uint32_t base = lwip_ntohl(server_ip.addr);
    uint32_t offset = (uint32_t)(free_slot - leases) + 1;
    free_slot->ip.addr = lwip_htonl(base + offset);
    free_slot->in_use = true;
  }
  return free_slot;
}

/* Builds a DHCP OFFER or ACK reply into `out` (must be at least
   DHCP_HDR_OPTIONS + ~64 bytes) and returns its length. `msg_type` is
   DHCP_MSG_OFFER or DHCP_MSG_ACK; both use the same fixed fields. */
static uint16_t build_reply(uint8_t *out, const uint8_t *req, uint16_t req_len,
                             uint8_t msg_type, const ip4_addr_t *yiaddr) {
  memset(out, 0, DHCP_HDR_OPTIONS);
  out[DHCP_HDR_OP] = DHCP_OP_BOOTREPLY;
  out[DHCP_HDR_HTYPE] = 1; /* Ethernet */
  out[DHCP_HDR_HLEN] = 6;
  memcpy(out + DHCP_HDR_XID, req + DHCP_HDR_XID, 4);
  memcpy(out + DHCP_HDR_FLAGS, req + DHCP_HDR_FLAGS, 2);
  put_u32(out + DHCP_HDR_YIADDR, lwip_ntohl(yiaddr->addr));
  put_u32(out + DHCP_HDR_SIADDR, lwip_ntohl(server_ip.addr));
  memcpy(out + DHCP_HDR_CHADDR, req + DHCP_HDR_CHADDR, 16);
  put_u32(out + DHCP_HDR_COOKIE, DHCP_MAGIC_COOKIE);

  uint16_t pos = DHCP_HDR_OPTIONS;
  out[pos++] = DHCP_OPT_MSG_TYPE; out[pos++] = 1; out[pos++] = msg_type;

  out[pos++] = DHCP_OPT_SERVER_ID; out[pos++] = 4;
  put_u32(out + pos, lwip_ntohl(server_ip.addr)); pos += 4;

  out[pos++] = DHCP_OPT_LEASE_TIME; out[pos++] = 4;
  put_u32(out + pos, DHCP_LEASE_SECONDS); pos += 4;

  out[pos++] = DHCP_OPT_SUBNET_MASK; out[pos++] = 4;
  put_u32(out + pos, lwip_ntohl(server_mask.addr)); pos += 4;

  out[pos++] = DHCP_OPT_ROUTER; out[pos++] = 4;
  put_u32(out + pos, lwip_ntohl(server_ip.addr)); pos += 4;

  /* No DNS offered -- this device isn't a resolver or an internet
     uplink, and the file manager is reached by IP (mDNS is a tracked
     gap). Omitting option 6 is valid; clients just
     won't have a DNS server configured, which is fine for reaching a
     bare IP address directly. */

  out[pos++] = DHCP_OPT_END;

  (void)req_len;
  return pos;
}

static void send_reply(struct udp_pcb *pcb, struct netif *netif,
                        const uint8_t *reply, uint16_t len) {
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  if (!p) return;
  memcpy(p->payload, reply, len);
  udp_sendto_if(pcb, p, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT, netif);
  pbuf_free(p);
}

static void on_dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port) {
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(addr);
  LWIP_UNUSED_ARG(port);

  if (!p) return;
  if (p->tot_len < DHCP_HDR_OPTIONS + 4 || p->tot_len > 576) {
    pbuf_free(p);
    return;
  }

  /* Copy into a flat local buffer rather than reading p->payload
     directly. FOUND BY COMPARISON AGAINST THE REFERENCE IMPLEMENTATION
     (Raspberry Pi's pico-examples dhcpserver.c) after this server was
     found to never respond to anything on real hardware, 2026-08-23: an
     earlier revision assumed a DHCP request always arrives as one
     contiguous pbuf and outright dropped anything else
     (`if (p->next != NULL) { pbuf_free(p); return; }`). That assumption
     is wrong -- with no override, lwIP's default PBUF_POOL buffer size
     is well under a ~300+ byte DHCP packet, so cyw43's Wi-Fi RX path
     commonly delivers one in a *chain* of several small pbufs, and every
     single request was being silently dropped before its message type
     was ever even inspected, regardless of any bug elsewhere. This is
     almost certainly the primary reason "there is no DHCP server" was
     observed. pbuf_copy_partial() gathers a chained pbuf's payload into
     one buffer regardless of how many segments it's split across --
     the same technique the reference implementation uses for exactly
     this reason. */
  uint8_t req[DHCP_HDR_OPTIONS + 320];
  uint16_t req_len = (uint16_t)pbuf_copy_partial(p, req, sizeof(req), 0);
  if (req_len < DHCP_HDR_OPTIONS + 4) {
    pbuf_free(p);
    return;
  }

  if (req[DHCP_HDR_OP] != DHCP_OP_BOOTREQUEST) {
    pbuf_free(p);
    return;
  }

  const uint8_t *msgtype_opt = find_option(req, req_len, DHCP_OPT_MSG_TYPE);
  if (!msgtype_opt) {
    pbuf_free(p);
    return;
  }
  uint8_t msg_type = msgtype_opt[1];

  uint8_t mac[6];
  memcpy(mac, req + DHCP_HDR_CHADDR, 6);

  struct netif *netif = ip_current_input_netif();

  if (msg_type == DHCP_MSG_DISCOVER) {
    lease_t *lease = find_or_claim_lease(mac);
    if (lease) {
      uint8_t reply[DHCP_HDR_OPTIONS + 32];
      uint16_t len = build_reply(reply, req, req_len, DHCP_MSG_OFFER, &lease->ip);
      send_reply(pcb, netif, reply, len);
    }
  } else if (msg_type == DHCP_MSG_REQUEST) {
    /* Accept whatever was requested/leased without further checking --
       there's no competing DHCP server on this link (it's a Wi-Fi AP
       this device itself created) and no third party to conflict with. */
    lease_t *lease = find_or_claim_lease(mac);
    if (lease) {
      uint8_t reply[DHCP_HDR_OPTIONS + 32];
      uint16_t len = build_reply(reply, req, req_len, DHCP_MSG_ACK, &lease->ip);
      send_reply(pcb, netif, reply, len);
    }
  }

  pbuf_free(p);
}

void dhcp_server_init(const ip4_addr_t *ip, const ip4_addr_t *netmask,
                       struct netif *netif) {
  server_ip = *ip;
  server_mask = *netmask;
  memset(leases, 0, sizeof(leases));

  dhcp_pcb = udp_new();
  if (!dhcp_pcb) return;

  udp_bind(dhcp_pcb, IP_ADDR_ANY, DHCP_SERVER_PORT);
  if (netif) {
    udp_bind_netif(dhcp_pcb, netif);
  }
  udp_recv(dhcp_pcb, on_dhcp_recv, NULL);
}
