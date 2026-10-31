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

/* base64.h: minimal base64 decoder, used by httpd.c's HTTP Basic Auth
   (2026-09-02). Split out as its own dependency-free module (same
   reasoning as netconfig.h's design note) so it can be unit-tested on
   the host -- see tests/base64_test.c -- independent of lwIP/pico-sdk.
*/
#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

/* Decodes a base64 string into a NUL-terminated `out`, bounded strictly
   by outsize regardless of how malformed `in` is (this exists to decode
   attacker-controlled HTTP header content) -- stops cleanly at '='
   padding, any non-base64 character, or end of output space. Never
   writes outside out[0..outsize). */
void base64_decode(const char *in, char *out, size_t outsize);

#endif
