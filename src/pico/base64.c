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

#include "base64.h"

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

void base64_decode(const char *in, char *out, size_t outsize) {
  size_t o = 0;
  int vals[4];
  int n = 0;
  if (outsize == 0) return;
  while (*in && o + 1 < outsize) {
    int v = b64val(*in);
    if (v < 0) break;
    vals[n++] = v;
    in++;
    if (n == 4) {
      if (o + 1 < outsize) out[o++] = (char)((vals[0] << 2) | (vals[1] >> 4));
      if (o + 1 < outsize) out[o++] = (char)((vals[1] << 4) | (vals[2] >> 2));
      if (o + 1 < outsize) out[o++] = (char)((vals[2] << 6) | vals[3]);
      n = 0;
    }
  }
  if (n >= 2 && o + 1 < outsize) {
    out[o++] = (char)((vals[0] << 2) | (vals[1] >> 4));
    if (n >= 3 && o + 1 < outsize) out[o++] = (char)((vals[1] << 4) | (vals[2] >> 2));
  }
  out[o] = 0;
}
