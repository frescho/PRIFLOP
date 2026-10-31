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


   spi.h: Definitions for the low-level SPI routines (RP2350)

   This port has no SPI-attached peripheral at all: SD support is
   dropped entirely, and the internal flash used
   by ata.c is accessed via memory-mapped XIP + the Pico SDK's
   hardware_flash API, not a software SPI driver -- the RP2350 boot ROM/
   QMI hardware handles the flash chip's own SPI bus invisibly to us.
   The only reason this file exists is that src/main.c unconditionally
   calls spi_init(SPI_SPEED_SLOW) (it's arch-specific, not shared code,
   in every other arch too). If a real SPI peripheral is ever added --
   an external SD slot "optional external SD"
   idea, or an SPI display -- this needs actual content and sdcard.c
   would need to come back into configs/config-pico2w's SRC list.
*/
#ifndef SPI_H
#define SPI_H

#include "config.h"

typedef enum { SPI_SPEED_FAST, SPI_SPEED_SLOW } spi_speed_t;

void spi_init(spi_speed_t speed);

#endif
