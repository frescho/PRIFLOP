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


   arch-eeprom.h: EEPROM-emulation access functions (RP2350 internal flash)

   STATUS: backing store is a single 4KB flash sector
   at the tail of the 4MB chip (FLASH_EEPROM_OFFSET/SIZE, arch-config.h) --
   a placeholder location; the real partition map (firmware A/B slots,
   FatFs storage region) is Phase 3 work,
   and won't need to move this sector since it's anchored to the end of
   flash rather than relative to the front.

   Why this file's contract looks like lpc17xx's (byte/word/block, "void
   *addr") rather than esp's (block-only, "uint32_t addr"): src/eeprom-conf.c
   has two calling conventions selected by "#ifdef __ets__" (an ESP8266
   toolchain predefine). The ESP8266 arch takes the "__ets__" branch, which
   keeps a live RAM copy of the whole config struct and syncs it to flash
   in one block read/write. Every other arch (AVR, lpc17xx, and this one)
   takes the other branch, which calls eeprom_read_byte/word(&storedconfig.field)
   per field -- i.e. it uses the FIELD'S ADDRESS as a stand-in for its
   EEPROM offset. That only works if storedconfig is placed at an address
   whose low bits equal 0 (so "(uintptr_t)&storedconfig.field & (SIZE-1)"
   recovers the field's true struct offset) -- otherwise it silently
   disagrees with the raw-integer-offset access eeprom-conf.c's checksum
   loop also does ("eeprom_read_byte((uint8_t *) i)" for i = 2..sizeof-1).
   EEMEM below forces that alignment via a plain GCC attribute (no custom
   linker script needed, unlike lpc17xx's approach of a dedicated NOLOAD
   section) -- see the EEMEM definition for why aligned() is the load-
   bearing part, not a nicety.

   Deliberately NOT touching src/eeprom-conf.c to avoid this: it's shared
   core code every other arch also depends on, and this arch can satisfy
   its existing contract without modification.
*/

#ifndef ARCH_EEPROM_H
#define ARCH_EEPROM_H

#include "arch-config.h" /* FLASH_EEPROM_OFFSET / FLASH_EEPROM_SIZE */

/* Must be aligned to FLASH_EEPROM_SIZE (a power of two): see the file
   comment above for why this alignment is a correctness requirement,
   not just a nicety. This struct is never read/written directly through
   normal RAM accesses -- it exists purely so "&storedconfig.field"
   evaluates to an address whose low bits equal that field's true offset
   within the struct; all real reads/writes go through the eeprom_read_
   and eeprom_write_ functions below, which redirect to actual flash
   storage. */
#define EEMEM __attribute__((aligned(FLASH_EEPROM_SIZE)))

/* No safety required */
#define eeprom_safety() do {} while (0)

uint8_t  eeprom_read_byte(void *addr);
uint16_t eeprom_read_word(void *addr);
void     eeprom_read_block(void *destptr, void *addr, unsigned int length);
void     eeprom_write_byte(void *addr, uint8_t value);
void     eeprom_write_word(void *addr, uint16_t value);
void     eeprom_write_block(void *srcptr, void *addr, unsigned int length);

#endif
