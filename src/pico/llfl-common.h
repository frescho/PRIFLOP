/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

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


   llfl-common.h: Timing primitives shared by the fastloader back ends.

   The model, unchanged from the other architectures: capture a reference
   instant at a bus edge, then place every subsequent line change at a
   fixed offset from it, measured in units of 100 ns. Fastloaders are
   specified that way -- as absolute offsets from an edge, not as a chain
   of delays -- because errors must not accumulate across a byte.
*/

#ifndef LLFL_COMMON_H
#define LLFL_COMMON_H

#include <stdint.h>

typedef enum { NO_ATNABORT, ATNABORT } llfl_atnabort_t;

typedef struct {
  uint32_t pairtimes[4];
  uint8_t  clockbits[4];
  uint8_t  databits[4];
  uint8_t  eorvalue;
} generic_2bit_t;

/* One-time init, from iec_init(): reads the system clock, which lives in
   flash and so must not be done per byte. */
void llfl_init(void);

/* Per-byte, RAM-resident: ensures the cycle counter is running. */
void llfl_setup(void);
#define llfl_teardown() do{}while(0)

void llfl_wait_atn(unsigned int state);
void llfl_wait_clock(unsigned int state, llfl_atnabort_t atnabort);
void llfl_wait_data(unsigned int state, llfl_atnabort_t atnabort);

void llfl_set_2bit_at(uint32_t time, unsigned int clock_state, unsigned int data_state);
void llfl_set_clock_at(uint32_t time, unsigned int state);
void llfl_set_data_at(uint32_t time, unsigned int state);
uint32_t llfl_read_bus_at(uint32_t time);
void llfl_set_reference(uint32_t time);
void llfl_generic_load_2bit(const generic_2bit_t *def, uint8_t byte);
uint8_t llfl_generic_save_2bit(const generic_2bit_t *def);

#endif
