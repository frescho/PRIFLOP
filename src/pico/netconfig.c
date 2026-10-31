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

/* netconfig.c: see netconfig.h for the design note on why the parsing
   logic below is split out as pure functions (host-unit-testable, see
   tests/netconfig_test.c) from netconfig_load()'s FatFs/hardware use.
*/
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#include "netconfig.h"
#include "sha256.h"

/* Salt source. It does not need to be secret -- it exists so that two
   devices with the same password do not share a digest, and so a
   precomputed table cannot be reused across devices. */
#ifdef NETCONFIG_HOST_TEST
#  include <stdlib.h>
static uint32_t nc_rand32(void) {
  return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}
#else
#  include "pico/rand.h"
static uint32_t nc_rand32(void) { return get_rand_32(); }
#endif

#define NC_SALT_LEN   8
#define NC_HASH_PREFIX "sha256:"

static void nc_tohex(const uint8_t *in, unsigned int len, char *out) {
  static const char hexd[] = "0123456789abcdef";
  for (unsigned int i = 0; i < len; i++) {
    out[i * 2]     = hexd[in[i] >> 4];
    out[i * 2 + 1] = hexd[in[i] & 15];
  }
  out[len * 2] = 0;
}

static int nc_fromhex(const char *in, unsigned int bytes, uint8_t *out) {
  for (unsigned int i = 0; i < bytes; i++) {
    int hi = -1, lo = -1;
    char a = in[i * 2], b = in[i * 2 + 1];
    if (a >= '0' && a <= '9') hi = a - '0';
    else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
    else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
    if (b >= '0' && b <= '9') lo = b - '0';
    else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
    else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1;
}

static void nc_digest(const uint8_t *salt, const char *plaintext,
                      uint8_t out[SHA256_DIGEST_LEN]) {
  sha256_ctx_t c;
  sha256_init(&c);
  sha256_update(&c, salt, NC_SALT_LEN);
  sha256_update(&c, plaintext, strlen(plaintext));
  sha256_final(&c, out);
}

#ifndef WIFI_SSID
#define WIFI_SSID "PRIFLOP"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
/* The web UI is password-protected out of the box. Note the consequence
   of set_field()'s empty-value rule: because this default is non-empty,
   an "ADMIN=" line with no value does NOT disable authentication -- it
   means "keep the default", i.e. "admin". Auth can be changed but not
   turned off from the config file; that is deliberate, since the file is
   itself downloadable through the very interface it protects. */
#ifndef ADMIN_PASSWORD
#define ADMIN_PASSWORD "admin"
#endif

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  if (*s) {
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r')) {
      *end = 0;
      end--;
    }
  }
  return s;
}

static void set_field(char *field, unsigned int field_size, const char *value) {
  if (!*value) return; /* empty value: leave the existing/default content alone */
  size_t n = field_size - 1;
  size_t vlen = strlen(value);
  if (vlen < n) n = vlen;
  memcpy(field, value, n);
  field[n] = 0;
}

void netconfig_set_defaults(net_config_t *cfg) {
  cfg->ap_mode = true;
  /* Zero every field first. set_field() deliberately ignores an empty
     value (so a stray "SSID=" line can't blank a field), which means a
     compiled-in default of "" -- WIFI_PASSWORD's default -- would
     otherwise leave that field holding whatever junk the caller's
     memory happened to contain. Only admin_password used to be zeroed;
     found 2026-09-04 when a host test finally called this with a dirty
     stack. */
  cfg->ssid[0] = 0;
  cfg->password[0] = 0;
  cfg->ap_ssid[0] = 0;
  cfg->ap_password[0] = 0;
  cfg->mdns_name[0] = 0;
  cfg->admin_password[0] = 0;

  set_field(cfg->ssid, sizeof(cfg->ssid), WIFI_SSID);
  set_field(cfg->password, sizeof(cfg->password), WIFI_PASSWORD);
  set_field(cfg->ap_ssid, sizeof(cfg->ap_ssid), WIFI_SSID);
  set_field(cfg->ap_password, sizeof(cfg->ap_password), WIFI_PASSWORD);
  set_field(cfg->mdns_name, sizeof(cfg->mdns_name), "PRIFLOP");
  set_field(cfg->admin_password, sizeof(cfg->admin_password), ADMIN_PASSWORD);
  /* Empty means worldwide -- the SDK default, and the right default for a
     firmware that does not know where it is. */
  cfg->country[0] = 0;
  cfg->pfm_autostart = -1;
}

bool netconfig_admin_is_hashed(const net_config_t *cfg) {
  return strncmp(cfg->admin_password, NC_HASH_PREFIX,
                 sizeof(NC_HASH_PREFIX) - 1) == 0;
}

void netconfig_set_admin(net_config_t *cfg, const char *plaintext) {
  uint8_t salt[NC_SALT_LEN], digest[SHA256_DIGEST_LEN];
  char saltx[NC_SALT_LEN * 2 + 1], digx[SHA256_DIGEST_LEN * 2 + 1];

  for (unsigned int i = 0; i < NC_SALT_LEN; i += 4) {
    uint32_t r = nc_rand32();
    salt[i]     = (uint8_t)(r);
    salt[i + 1] = (uint8_t)(r >> 8);
    salt[i + 2] = (uint8_t)(r >> 16);
    salt[i + 3] = (uint8_t)(r >> 24);
  }
  nc_digest(salt, plaintext, digest);
  nc_tohex(salt, NC_SALT_LEN, saltx);
  nc_tohex(digest, SHA256_DIGEST_LEN, digx);
  snprintf(cfg->admin_password, sizeof(cfg->admin_password),
           NC_HASH_PREFIX "%s:%s", saltx, digx);
}

void netconfig_rehash_admin(net_config_t *cfg) {
  char plain[NETCONFIG_ADMIN_MAX];
  if (netconfig_admin_is_hashed(cfg)) return;
  /* Copy first: set_admin() writes into the same field it reads. */
  snprintf(plain, sizeof(plain), "%s", cfg->admin_password);
  netconfig_set_admin(cfg, plain);
}

bool netconfig_admin_matches(const net_config_t *cfg, const char *supplied) {
  if (!supplied) return false;

  if (!netconfig_admin_is_hashed(cfg))
    return strcmp(cfg->admin_password, supplied) == 0;

  /* "sha256:<16 hex>:<64 hex>" and nothing else. Every check below fails
     CLOSED -- a malformed stored form rejects every password rather than
     accepting any. A parser that fell through leaving a zeroed digest
     would leave the device wide open, which is the one outcome worth
     writing a test for (see tests/netconfig_test.c). */
  const char *p = cfg->admin_password + sizeof(NC_HASH_PREFIX) - 1;
  uint8_t salt[NC_SALT_LEN], want[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];

  if (strlen(p) != NC_SALT_LEN * 2 + 1 + SHA256_DIGEST_LEN * 2) return false;
  if (p[NC_SALT_LEN * 2] != ':') return false;
  if (!nc_fromhex(p, NC_SALT_LEN, salt)) return false;
  if (!nc_fromhex(p + NC_SALT_LEN * 2 + 1, SHA256_DIGEST_LEN, want)) return false;

  nc_digest(salt, supplied, got);
  return memcmp(got, want, SHA256_DIGEST_LEN) == 0;
}

bool netconfig_admin_is_default(const net_config_t *cfg) {
  /* Cannot strcmp against ADMIN_PASSWORD once the stored form is a
     digest -- ask the comparison function, which handles both forms. */
  return netconfig_admin_matches(cfg, ADMIN_PASSWORD);
}

void netconfig_apply_line(net_config_t *cfg, char *line) {
  char *eq = strchr(line, '=');
  if (!eq) return;

  /* Room for a key with spaces before its '=' as well: the Primo's PFM
     and its boot loader accept "PFM_AUTOSTART   = ON", and a key
     buffer of 16 used to drop that line here unseen. */
  char key[32];
  size_t keylen = (size_t)(eq - line);
  if (keylen == 0 || keylen >= sizeof(key)) return;
  memcpy(key, line, keylen);
  key[keylen] = 0;
  char *trimmed_key = trim(key);

  char *value = trim(eq + 1);

  if (strcasecmp(trimmed_key, "MODE") == 0) {
    if (strcasecmp(value, "STA") == 0) {
      cfg->ap_mode = false;
    } else if (strcasecmp(value, "AP") == 0) {
      cfg->ap_mode = true;
    }
    /* anything else (including garbage): leave ap_mode at whatever it
       already was -- an unrecognised mode should never silently flip
       the device into a mode nobody asked for. */
  } else if (strcasecmp(trimmed_key, "SSID") == 0) {
    set_field(cfg->ssid, sizeof(cfg->ssid), value);
  } else if (strcasecmp(trimmed_key, "PASSWORD") == 0) {
    set_field(cfg->password, sizeof(cfg->password), value);
  } else if (strcasecmp(trimmed_key, "AP_SSID") == 0) {
    set_field(cfg->ap_ssid, sizeof(cfg->ap_ssid), value);
  } else if (strcasecmp(trimmed_key, "AP_PASSWORD") == 0) {
    set_field(cfg->ap_password, sizeof(cfg->ap_password), value);
  } else if (strcasecmp(trimmed_key, "MDNS") == 0) {
    set_field(cfg->mdns_name, sizeof(cfg->mdns_name), value);
  } else if (strcasecmp(trimmed_key, "ADMIN") == 0) {
    set_field(cfg->admin_password, sizeof(cfg->admin_password), value);
  } else if (strcasecmp(trimmed_key, "COUNTRY") == 0) {
    /* Exactly two letters or nothing. A malformed code is worse than none:
       the chip would take an unknown regulatory domain rather than fall
       back, so anything that is not two letters is discarded and the
       device stays on worldwide. */
    if (value[0] && value[1] && !value[2]) {
      cfg->country[0] = (char)toupper((unsigned char)value[0]);
      cfg->country[1] = (char)toupper((unsigned char)value[1]);
      cfg->country[2] = 0;
    } else {
      cfg->country[0] = 0;
    }
  } else if (strcasecmp(trimmed_key, "PFM_AUTOSTART") == 0) {
    cfg->pfm_autostart = (strcasecmp(value, "ON") == 0) ? 1 : 0;
  }
  /* unknown key: ignored, not an error */
}

unsigned int netconfig_serialize(const net_config_t *cfg, char *buf,
                                 unsigned int bufsize) {
  if (!cfg || !buf || bufsize == 0) return 0;

  /* PFM_AUTOSTART last, and only if the file had it (or it was set). */
  const char *keys[] = { "MODE", "SSID", "PASSWORD", "AP_SSID",
                         "AP_PASSWORD", "MDNS", "ADMIN", "COUNTRY",
                         "PFM_AUTOSTART" };
  const char *vals[] = { cfg->ap_mode ? "AP" : "STA", cfg->ssid,
                         cfg->password, cfg->ap_ssid, cfg->ap_password,
                         cfg->mdns_name, cfg->admin_password,
                         cfg->country, cfg->pfm_autostart ? "ON" : "OFF" };
  const unsigned int n = sizeof(keys) / sizeof(keys[0])
                         - (cfg->pfm_autostart < 0 ? 1 : 0);

  /* Measure before writing anything. A single-pass "write until it
     overflows" would leave a half-written buffer behind on failure --
     and a truncated PRIFLOP.CFG still parses, silently dropping whatever
     fell off the end, which is worse than not writing at all. */
  unsigned int total = 0;
  for (unsigned int i = 0; i < n; i++) {
    total += (unsigned int)strlen(keys[i]) + 1        /* key + '=' */
           + (unsigned int)strlen(vals[i]) + 2;       /* value + CRLF */
  }
  if (total + 1 > bufsize) return 0;                  /* + NUL */

  unsigned int pos = 0;
  for (unsigned int i = 0; i < n; i++) {
    unsigned int klen = (unsigned int)strlen(keys[i]);
    unsigned int vlen = (unsigned int)strlen(vals[i]);
    memcpy(buf + pos, keys[i], klen); pos += klen;
    buf[pos++] = '=';
    memcpy(buf + pos, vals[i], vlen); pos += vlen;
    buf[pos++] = '\r';
    buf[pos++] = '\n';
  }
  buf[pos] = 0;
  return pos;
}

void netconfig_parse_buffer(net_config_t *cfg, char *buf, unsigned int len) {
  unsigned int start = 0;
  for (unsigned int i = 0; i <= len; i++) {
    if (i == len || buf[i] == '\n') {
      buf[i] = 0;
      if (i > start) {
        char *line = trim(&buf[start]);
        if (*line) netconfig_apply_line(cfg, line);
      }
      start = i + 1;
    }
  }
}

unsigned int netconfig_set_pfm_line(char *buf, unsigned int len,
                                    unsigned int bufsize, signed char value) {
  static const char key[] = "PFM_AUTOSTART=";
  const unsigned int klen = sizeof(key) - 1;

  if (len + 1 > bufsize) return 0;      /* no room for even the NUL */

  /* serialize() writes it last: drop that line if it is there */
  unsigned int start = len;
  if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') {
    start = len - 2;
    while (start > 0 && buf[start - 1] != '\n') start--;
    if (len - start >= klen && memcmp(buf + start, key, klen) == 0)
      len = start;
  }
  if (value < 0) {
    buf[len] = 0;
    return len;
  }
  const char *v = value ? "ON" : "OFF";
  unsigned int need = len + klen + (unsigned int)strlen(v) + 2;
  if (need + 1 > bufsize) return 0;
  memcpy(buf + len, key, klen); len += klen;
  memcpy(buf + len, v, strlen(v)); len += (unsigned int)strlen(v);
  buf[len++] = '\r';
  buf[len++] = '\n';
  buf[len] = 0;
  return len;
}

unsigned int netconfig_diff(const net_config_t *old, const net_config_t *now,
                            char *buf, unsigned int bufsize) {
  if (!old || !now || !buf || bufsize == 0) return 0;

  /* PFM_AUTOSTART is never the page's, so it is not compared. */
  const char *keys[] = { "MODE", "SSID", "PASSWORD", "AP_SSID",
                         "AP_PASSWORD", "MDNS", "ADMIN", "COUNTRY" };
  const char *was[] = { old->ap_mode ? "AP" : "STA", old->ssid,
                        old->password, old->ap_ssid, old->ap_password,
                        old->mdns_name, old->admin_password, old->country };
  const char *is[] = { now->ap_mode ? "AP" : "STA", now->ssid,
                       now->password, now->ap_ssid, now->ap_password,
                       now->mdns_name, now->admin_password, now->country };

  unsigned int pos = 0;
  for (unsigned int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (strcmp(was[i], is[i]) == 0) continue;
    unsigned int klen = (unsigned int)strlen(keys[i]);
    unsigned int vlen = (unsigned int)strlen(is[i]);
    if (pos + klen + 1 + vlen + 2 + 1 > bufsize) return 0;
    memcpy(buf + pos, keys[i], klen); pos += klen;
    buf[pos++] = '=';
    memcpy(buf + pos, is[i], vlen); pos += vlen;
    buf[pos++] = '\r';
    buf[pos++] = '\n';
  }
  buf[pos] = 0;
  return pos;
}

unsigned int netconfig_merge(char *file, unsigned int flen,
                             char *buf, unsigned int len,
                             unsigned int bufsize) {
  /* Static: about 400 bytes, and this runs on core 0's stack. Both
     buffers need a byte past their length, which the parser zeroes. */
  static net_config_t cfg;
  netconfig_set_defaults(&cfg);
  netconfig_parse_buffer(&cfg, file, flen);
  netconfig_parse_buffer(&cfg, buf, len);
  return netconfig_serialize(&cfg, buf, bufsize);
}

#ifndef NETCONFIG_HOST_TEST

#include "config.h"
#include "lfs.h"
#include "lfsops.h"
#include "parser.h"


void netconfig_load(net_config_t *cfg) {
  netconfig_set_defaults(cfg);

  if (max_part < 1) return;

  /* Own cache buffer: LFS_NO_MALLOC means littlefs cannot allocate one.
     Static rather than on the stack -- this runs from main() before the
     IEC mainloop, but 256 bytes of stack for a boot-time config read is
     needless. */
  static lfs_file_t f;
  static uint8_t cache[LFS_FILE_CACHE_SIZE];
  struct lfs_file_config fcfg;

  memset(&fcfg, 0, sizeof(fcfg));
  fcfg.buffer = cache;

  if (lfs_file_opencfg(lfsops_volume(), &f, NETCONFIG_PATH,
                       LFS_O_RDONLY, &fcfg) < 0) {
    return;
  }

  static char buf[NETCONFIG_FILE_MAX];
  lfs_ssize_t bytesread = lfs_file_read(lfsops_volume(), &f, buf,
                                        sizeof(buf) - 1);
  lfs_file_close(lfsops_volume(), &f);
  if (bytesread < 0) {
    return; /* read error partway through: keep pure defaults rather
                than apply a possibly-inconsistent partial read */
  }

  buf[bytesread] = 0;
  netconfig_parse_buffer(cfg, buf, (unsigned int)bytesread);
}

signed char netconfig_pfm_autostart(void) {
  static net_config_t now;
  netconfig_load(&now);
  return now.pfm_autostart;
}

#endif /* NETCONFIG_HOST_TEST */
