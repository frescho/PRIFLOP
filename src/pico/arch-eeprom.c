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


   arch-eeprom.c: EEPROM-emulation access functions (RP2350 internal flash)

   See arch-eeprom.h for why the "convert a field's address into an
   EEPROM offset" scheme below has to work the way it does.

   Unlike a real EEPROM (or the AVR/lpc17xx targets' backing stores),
   RP2350 NOR flash cannot rewrite individual bytes in place -- every
   write to a sector requires erasing the whole sector first. So every
   eeprom_write_byte/word/block call here does a full read-modify-erase-
   write of the entire FLASH_EEPROM_SIZE sector: read the current sector
   into RAM, patch in just the changed bytes, erase, reprogram. That's
   correct (write_configuration() in eeprom-conf.c makes ~9 of these
   calls back to back and each one preserves what the previous calls
   wrote) but not efficient -- config writes are rare (only on the "XW"
   DOS command), so this hasn't been worth optimizing into a single
   batched write. Revisit if that assumption stops holding.

   CROSS-CORE SAFETY: tried switching to pico_flash's flash_safe_execute()
   for backlog item 4 (core 1 WiFi/HTTP offload), reverted 2026-09-03 --
   see eeprom_patch()'s comment below and ata.c's commit_slot() for why.
   Still just a plain save_and_disable_interrupts(), same as before that
   attempt.
*/

#include "config.h"
#include "flash_lockout.h"
#include "arch-eeprom.h"
#include "uart.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/platform.h"
#include <string.h>

static_assert((FLASH_EEPROM_SIZE & (FLASH_EEPROM_SIZE - 1)) == 0,
              "FLASH_EEPROM_SIZE must be a power of two");
static_assert(FLASH_EEPROM_SIZE % FLASH_SECTOR_SIZE == 0,
              "FLASH_EEPROM_SIZE must be a whole number of flash sectors");

/* Recover a field's EEPROM-relative offset from its (aligned, see
   arch-eeprom.h) struct address. */
static inline uint32_t convert_address(void *addr) {
  return ((uintptr_t)addr) & (FLASH_EEPROM_SIZE - 1);
}

static const uint8_t *flash_base(void) {
  return (const uint8_t *)(XIP_BASE + FLASH_EEPROM_OFFSET);
}

/* Read-modify-erase-write: patch `length` bytes at `offset` into the
   FLASH_EEPROM_SIZE region, preserving everything else in it.

   CROSS-CORE SAFETY -- TRIED AND REVERTED: switching this to pico_flash's
   flash_safe_execute() (see ata.c's commit_slot() for the full story --
   same reasoning, same real-hardware finding) badly disrupted core 0's
   real-time IEC bus timing, regardless of whether core 1 was registered
   and responsive or not. Reverted to the plain interrupt-disable this
   file used before backlog item 4, knowingly reopening the latent risk
   flash_safe_execute() was meant to close (core 1 reading flash mid-
   write) as an accepted tradeoff -- see the regression writeup. */
static void eeprom_patch(uint32_t offset, const void *data, unsigned int length) {
  static uint8_t sector_buf[FLASH_EEPROM_SIZE];

  if (offset >= FLASH_EEPROM_SIZE || length > FLASH_EEPROM_SIZE - offset) {
    return;
  }

  memcpy(sector_buf, flash_base(), FLASH_EEPROM_SIZE);
  memcpy(sector_buf + offset, data, length);

  /* Park core 1 first -- same reason as ata.c's commit_slot(), see
     flash_lockout.h. */
  if (!flash_lockout_begin()) {
    return;   /* see ata.c's commit_slot() -- defer, never erase unsafely */
  }
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(FLASH_EEPROM_OFFSET, FLASH_EEPROM_SIZE);
  flash_range_program(FLASH_EEPROM_OFFSET, sector_buf, FLASH_EEPROM_SIZE);
  /* REQUIRED on RP2350, not optional -- see ata.c's commit_slot() for why
     (gotcha #13): without this, a later flash_base() read
     (including this function's own next read-modify-write) can be served
     stale pre-write data from the XIP cache. */
  xip_cache_invalidate_range(FLASH_EEPROM_OFFSET, FLASH_EEPROM_SIZE);
  restore_interrupts(ints);
  flash_lockout_end();
}

uint8_t eeprom_read_byte(void *addr) {
  return flash_base()[convert_address(addr)];
}

uint16_t eeprom_read_word(void *addr) {
  uint16_t val;
  memcpy(&val, flash_base() + convert_address(addr), sizeof(val));
  return val;
}

void eeprom_read_block(void *destptr, void *addr, unsigned int length) {
  uint32_t offset = convert_address(addr);
  if (offset >= FLASH_EEPROM_SIZE || length > FLASH_EEPROM_SIZE - offset) {
    return;
  }
  memcpy(destptr, flash_base() + offset, length);
}

void eeprom_write_byte(void *addr, uint8_t value) {
  eeprom_patch(convert_address(addr), &value, sizeof(value));
}

void eeprom_write_word(void *addr, uint16_t value) {
  eeprom_patch(convert_address(addr), &value, sizeof(value));
}

void eeprom_write_block(void *srcptr, void *addr, unsigned int length) {
  eeprom_patch(convert_address(addr), srcptr, length);
}
