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

/* netconfig.h: runtime Wi-Fi/admin config for the sd2iec_pico2w port,
   read from /PRIFLOP.CFG on the flash-backed FAT volume.

   Design note (2026-09-02, after a rewrite of this file broke AP join
   and DHCP and had to be reverted): the
   line-parsing logic here (netconfig_apply_line/netconfig_parse_buffer)
   is deliberately a pure function of an in-memory buffer, with zero
   dependency on FatFs or any pico-sdk header, specifically so it can be
   compiled and unit-tested on the host (see tests/netconfig_test.c) --
   this is the actual verification for "handles a missing or damaged
   file safely" that real hardware testing can't easily exercise (you'd
   need to physically corrupt the flash to test the corrupt-file case).
   Only netconfig_load() itself touches FatFs/hardware.
*/
#ifndef NETCONFIG_H
#define NETCONFIG_H

#include <stdbool.h>

#define NETCONFIG_SSID_MAX      33   /* 32-byte SSID + NUL, per 802.11 */
#define NETCONFIG_PASSWORD_MAX  64   /* 63-byte WPA2-PSK max + NUL */
#define NETCONFIG_MDNS_MAX      32
/* Big enough for the stored form "sha256:<16 hex salt>:<64 hex digest>"
   (88 chars) as well as a plaintext password. */
#define NETCONFIG_ADMIN_MAX     96

/* The config file itself. Written by the web UI as an ordinary file
   upload (POST /files/PRIFLOP.CFG), not through a dedicated API -- see
   httpd.c's Wi-Fi section. */
#define NETCONFIG_PATH "/PRIFLOP.CFG"
#define NETCONFIG_FILE_MAX 1024   /* the most of the file that is read */

typedef struct {
  bool ap_mode;

  /* ssid/password are the network to JOIN in station mode. ap_ssid/
     ap_password are what we BROADCAST in access-point mode. They are
     separate on purpose: sharing one field meant an AP-mode fallback
     broadcast an open network with the same name as the user's real
     one, which clients can silently prefer -- observed 2026-09-04 after
     a station config half-survived a reset ("WiFi: starting access point
     \"<home network>\""). Keeping them apart also
     means a failed join never leaks the home network's password onto a
     broadcast AP. */
  char ssid[NETCONFIG_SSID_MAX];
  char password[NETCONFIG_PASSWORD_MAX];
  char ap_ssid[NETCONFIG_SSID_MAX];
  char ap_password[NETCONFIG_PASSWORD_MAX];
  char mdns_name[NETCONFIG_MDNS_MAX];
  char admin_password[NETCONFIG_ADMIN_MAX];

  /* Two-letter ISO country code for the Wi-Fi regulatory domain, or empty
     for the SDK's default of "worldwide".

     Worth having as a setting rather than a build constant because it
     changes association behaviour, not just legality. Worldwide is the
     most conservative regulatory domain the chip has: it restricts
     channels and forces passive scanning on those it is unsure about,
     which means listening for beacons instead of probing. The Pico SDK's
     own header says worldwide "may not give the best performance" and
     recommends setting a real one. This drive was seeing roughly half of
     its first association attempts rejected, with the first attempt
     taking ~8.7s against ~3s for later ones -- the shape of a scan
     problem rather than a credentials problem. */
  char country[3];

  /* PFM_AUTOSTART: the Primo DOS's boot loader starts PFM, the Primo's
     file selector, when this is ON (tools/primo/dos/loader.asm). The firmware does nothing with it; it
     is kept only so that a save from the web page, which rewrites the
     whole file from these fields, does not drop it. -1: no such line,
     and none is written; 0: off ("OFF" written); 1: on. Read as the
     loader reads it: "ON" in any case is on, any other value off. */
  signed char pfm_autostart;
} net_config_t;

/* Fills cfg with the compiled-in defaults: AP mode, SSID/mDNS name
   "PRIFLOP" (or the WIFI_SSID/WIFI_PASSWORD CMake overrides), and admin
   password "admin". Never fails. */
void netconfig_set_defaults(net_config_t *cfg);

/* Applies one "KEY=VALUE" line to cfg (mutated in place: this trims
   leading/trailing whitespace off the value by writing a NUL into
   `line`, same as netconfig_parse_buffer does for line boundaries).
   Unknown keys, malformed lines (no '='), and empty values are all
   silently ignored -- an empty value never blanks out an existing/
   default field, since e.g. a stray "SSID=" line in a hand-edited or
   partially-corrupted file should leave the SSID alone, not produce a
   nameless AP. */
void netconfig_apply_line(net_config_t *cfg, char *line);

/* Parses a whole in-memory buffer of "KEY=VALUE" lines (CRLF or LF,
   mixed is fine) into cfg, which must already hold valid defaults --
   this only ever refines cfg, never invalidates it. `buf` must be at
   least `len + 1` bytes (buf[len] is written as a NUL terminator for
   the final line; NETCONFIG_FILE_MAX-sized buffers from
   netconfig_load() always have this spare byte). Handles any content
   safely, including binary garbage: lines that don't parse as
   "KEY=VALUE" are just skipped, never treated as an error. */
void netconfig_parse_buffer(net_config_t *cfg, char *buf, unsigned int len);

/* The real entry point: sets defaults, then tries to read and parse
   /PRIFLOP.CFG from the flash FAT volume. Always leaves cfg fully and
   validly populated -- a missing file, an unmounted filesystem, a
   read error, or a file full of garbage all just fall back to
   defaults (per-field, via netconfig_apply_line's empty-value rule
   above) rather than failing or leaving cfg partially set. */
void netconfig_load(net_config_t *cfg);

/* Renders cfg back out as the "KEY=VALUE" lines netconfig_parse_buffer()
   reads, into buf (NUL-terminated), and returns the number of bytes
   written excluding the NUL -- or 0 if bufsize is too small, in which
   case buf is left untouched rather than half-written.

   Deliberately pure and FatFs-free for the same reason the parser is
   (see the header note above): round-tripping serialize -> parse is
   checked on the host in tests/netconfig_test.c, which is where the
   "a saved config reloads identically" guarantee actually comes from.

   Every field is always emitted, including empty ones -- the parser's
   empty-value rule means a blank line can never clear a field, so an
   omitted key and an empty key both mean "keep the default". Writing
   them all keeps the file self-documenting for hand-editing. */
/* The last line of a serialized config (buf, len bytes) set to
   PFM_AUTOSTART=ON or OFF for `value` 1 or 0, or removed for -1: a line
   that serialize() wrote there is replaced. Returns the new length, or 0
   if bufsize is too small. Used when the web page saves the Wi-Fi
   settings: the page does not own the key, so the value the file holds
   at that moment is kept (fs_bridge.c, do_write_file), not the one read
   at boot -- PFM may have changed it since, over the bus. */
unsigned int netconfig_set_pfm_line(char *buf, unsigned int len,
                                    unsigned int bufsize, signed char value);

/* PFM_AUTOSTART as PRIFLOP.CFG holds it now (-1, 0 or 1, as the field
   above), read from the flash. Core 0 only: it touches littlefs. Not in
   the host build. */
signed char netconfig_pfm_autostart(void);

/* True when the admin password is still whatever the firmware was built
   with (ADMIN_PASSWORD, "admin" unless overridden at build time). A
   device in that state hands its Wi-Fi password to anyone on the network
   who tries the obvious thing, so the UI says so loudly. */
bool netconfig_admin_is_default(const net_config_t *cfg);

/* Verifies a password supplied over HTTP against the stored one.
   Everything that checks the admin password goes through here.

   That indirection is the seam for storing a hash instead of the
   plaintext: the admin password is the one secret on this device that
   CAN be hashed, because it only ever needs verifying, whereas the Wi-Fi
   passphrase has to be handed back to cyw43_wifi_join() verbatim on
   every boot and so cannot be. When that lands, only this function and
   netconfig_set_admin() change; no caller does. */
bool netconfig_admin_matches(const net_config_t *cfg, const char *supplied);

/* Replaces the admin password, storing it salted and hashed. The
   plaintext is never kept. */
void netconfig_set_admin(net_config_t *cfg, const char *plaintext);

/* True when the stored password is already a digest. Configs written
   before hashing existed hold a plaintext password; they keep working,
   and netconfig_rehash_admin() converts one in place so the next save
   writes the hashed form. Migration happens on write rather than on load
   so that upgrading does not cost a flash write on every boot. */
bool netconfig_admin_is_hashed(const net_config_t *cfg);
void netconfig_rehash_admin(net_config_t *cfg);

unsigned int netconfig_serialize(const net_config_t *cfg, char *buf,
                                 unsigned int bufsize);

/* The keys whose values differ between `old` and `now`, as KEY=VALUE
   lines (PFM_AUTOSTART never: it is not the web page's). Returns the
   length, or 0 if bufsize is too small; an empty diff is length 0 with
   buf[0] = 0. The web page's Wi-Fi save sends only these, so a key it did
   not change keeps the value the file holds when the save happens -- not
   the one read at boot, which PFM may have changed over the bus since
   (open step 10). */
unsigned int netconfig_diff(const net_config_t *old, const net_config_t *now,
                            char *buf, unsigned int bufsize);

/* PRIFLOP.CFG as written by a save: the file as it is now (`file`, flen
   bytes) with the lines in `buf` (len bytes) applied over it, serialized
   back into buf. A whole file in buf gives that file; a diff changes only
   its keys. Both inputs are parsed in place. Returns the new length, or 0
   if bufsize is too small. */
unsigned int netconfig_merge(char *file, unsigned int flen,
                             char *buf, unsigned int len,
                             unsigned int bufsize);

#endif
