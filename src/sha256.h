/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

   RP2350 / Raspberry Pi Pico 2 W port by frescho.

   sha256.c/h are an original implementation of FIPS 180-4 SHA-256 written
   for this port -- not derived from any existing codebase -- and are
   covered by the same licence as the rest of sd2iec.

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

/* sha256.h: FIPS 180-4 SHA-256. See sha256.c for why this is vendored. */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

typedef struct {
  uint32_t h[8];
  uint64_t len;       /* total bytes fed in */
  uint8_t  buf[64];
  size_t   buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience wrapper. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

#endif
