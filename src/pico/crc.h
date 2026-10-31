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


   crc.h: Definitions for CRC calculation routines

   Needed even though SD support is dropped:
   doscmd.c's fastloader detection table (fl_crc_table) uses
   crc_xmodem_block regardless of storage backend. Pure portable C, no
   hardware dependency, so this is copied from src/esp/crc.c near
   verbatim rather than genuinely re-architected for this port.
*/

#ifndef CRC_H
#define CRC_H

#include <stdint.h>

#define CRC16_CCITT_INIT_VALUE 0

uint8_t crc7update(uint8_t crc, uint8_t data);
uint16_t crc_xmodem_update(uint16_t crc, uint8_t data);
uint16_t crc_xmodem_block(uint16_t crc, const uint8_t *data, uint32_t length);
uint16_t crc16_update(uint16_t crc, uint8_t data);

#endif
