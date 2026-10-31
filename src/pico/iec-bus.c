/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2026 sd2iec_pico2w contributors

   Inspired by MMC2IEC by Lars Pontoppidan et al.

   Historical credit: FAT filesystem access was based on code from ChaN
   and Jim Brain. No code of theirs remains -- storage is littlefs, which
   carries its own BSD-3-Clause licence in src/littlefs/LICENSE.md.

   RP2350 / Raspberry Pi Pico 2 W port by frescho.

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


   iec-bus.c: Architecture-specific IEC bus initialisation

   This is not in arch-config.h because using the set_* functions
   from iec-bus.h simplifies the code and the ARM version isn't
   space-constrained yet.
*/

#include "config.h"
#include "iec-bus.h"

void iec_interface_init(void) {
  /* Set up outputs before switching the pins */
  set_atn(1);
  set_data(1);
  set_clock(1);
  set_srq(1);

  /* SRQ is special-cased because it may be unconnected */
}
void bus_interface_init(void) __attribute__ ((weak, alias("iec_interface_init")));
