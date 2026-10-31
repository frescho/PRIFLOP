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

/* sha256.c: FIPS 180-4 SHA-256.
 *
 * Vendored rather than pulled from mbedtls: mbedtls ships with the Pico
 * SDK but is not linked into this firmware, and adding it for one hash
 * would cost far more than these 150 lines. Vendoring also means the host
 * tests (tests/netconfig_test.c, built with plain gcc and no SDK) run the
 * exact code the device runs, rather than checking one implementation
 * against another -- which for a password hash is the difference between
 * a real test and a tautology.
 *
 * Used only by netconfig.c, to store the admin password as a salted
 * digest instead of in clear.
 */

#include <string.h>

#include "sha256.h"

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define BSIG1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SSIG0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_block(sha256_ctx_t *c, const uint8_t *p) {
  uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
  int i;

  for (i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (; i < 64; i++)
    w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g  = c->h[6]; h = c->h[7];

  for (i = 0; i < 64; i++) {
    t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
    t2 = BSIG0(a) + MAJ(a, b, cc);
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }

  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

void sha256_init(sha256_ctx_t *c) {
  c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
  c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
  c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
  c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
  c->len = 0;
  c->buflen = 0;
}

void sha256_update(sha256_ctx_t *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;

  c->len += (uint64_t)len;
  while (len > 0) {
    size_t n = 64 - c->buflen;
    if (n > len) n = len;
    memcpy(c->buf + c->buflen, p, n);
    c->buflen += n;
    p += n;
    len -= n;
    if (c->buflen == 64) {
      sha256_block(c, c->buf);
      c->buflen = 0;
    }
  }
}

void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]) {
  uint64_t bits = c->len * 8;
  int i;

  /* Padding: 0x80, then zeroes, then the 64-bit big-endian bit count. */
  c->buf[c->buflen++] = 0x80;
  if (c->buflen > 56) {
    memset(c->buf + c->buflen, 0, 64 - c->buflen);
    sha256_block(c, c->buf);
    c->buflen = 0;
  }
  memset(c->buf + c->buflen, 0, 56 - c->buflen);
  for (i = 0; i < 8; i++)
    c->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
  sha256_block(c, c->buf);

  for (i = 0; i < 8; i++) {
    out[i * 4]     = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->h[i]);
  }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
  sha256_ctx_t c;
  sha256_init(&c);
  sha256_update(&c, data, len);
  sha256_final(&c, out);
}
