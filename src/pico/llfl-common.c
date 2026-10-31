/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

   RP2350 / Raspberry Pi Pico 2 W port by frescho.
   Ported from the ESP8266 back end (src/esp/llfl-common.c in the
   pre-fatfs-removal tree), which is itself Ingo Korb's; the timing model
   and the generic 2-bit helpers are his, the RP2350 timebase is new.

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


   llfl-common.c: Timing primitives for the fastloader back ends.

   THE TIMEBASE. Everything here is measured in units of 100 ns from a
   reference instant, so the counter has to resolve better than that.
   Neither obvious candidate works:

     - timer_hw (the SDK's microsecond timer) has 1 us granularity. The
       JiffyDOS bit pairs sit 11 us apart and the offsets are specified to
       100 ns; rounding every one of them to the nearest microsecond would
       put the sample point most of a bit-cell away from where the C64
       puts the data.
     - SysTick is already taken. arch-timer.c runs it with TICKINT for the
       system tick, and system.c's flash shield saves and restores its
       control register. Borrowing it would mean fighting two owners.

   So this uses the Cortex-M33 DWT cycle counter: free-running, 32 bits,
   one count per system clock. At 150 MHz that is 6.67 ns -- fifteen
   counts per 100 ns unit, which is resolution to spare.

   INTERRUPTS. Not disabled here. Each back end does it around the part
   that must not be interrupted, exactly as the other architectures do,
   because the right span is protocol-specific -- and because this file's
   waits are also used while deliberately watching for ATN.
*/

#include <stdint.h>

#include "config.h"
#include "arch-config.h"
#include "iec-bus.h"
#include "llfl-common.h"

#include "hardware/structs/systick.h"
#include "hardware/clocks.h"
#include "pico/platform.h"

/* Cortex-M33 DWT, addressed directly rather than through CMSIS headers so
   this file does not depend on which CMSIS stub the SDK selected. */
#define DWT_CTRL     (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT   (*(volatile uint32_t *)0xE0001004u)
#define DEMCR        (*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_TRCENA        (1u << 24)
#define DWT_CTRL_CYCCNTENA  (1u << 0)

static uint32_t llfl_reference_time;
static uint32_t clocks_per_100ns = 15;   /* 150 MHz default, fixed up in setup */

/* Called once, from iec_init(). Kept separate from llfl_setup() because
   clock_get_hz() lives in flash: doing it per byte would put an XIP fetch
   inside a fastloader, which is both slower than the protocol can afford
   and unsafe if a flash operation is ever in flight.

   Derived rather than assumed: a firmware that changed the system clock
   would otherwise silently skew every fastloader, and the failure would
   present as a flaky C64 rather than as a clock change. */
void llfl_init(void) {
  clocks_per_100ns = clock_get_hz(clk_sys) / 10000000u;
  if (clocks_per_100ns == 0)
    clocks_per_100ns = 1;
}

/* Per-byte, so RAM-resident and as close to free as possible: one
   register read in the common case. */
void __not_in_flash_func(llfl_setup)(void) {
  if (!(DWT_CTRL & DWT_CTRL_CYCCNTENA)) {
    DEMCR     |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL  |= DWT_CTRL_CYCCNTENA;
  }
}

static inline uint32_t __not_in_flash_func(ccount)(void) {
  return DWT_CYCCNT;
}

void __not_in_flash_func(llfl_set_reference)(uint32_t time) {
  llfl_reference_time = ccount() + time * clocks_per_100ns;
}

/* Wraparound-safe: compares a signed delta rather than ordering two
   unsigned counter reads, so a reference taken just below 2^32 still
   works. */
static inline void __not_in_flash_func(wait_to)(uint32_t time) {
  uint32_t target = llfl_reference_time + time * clocks_per_100ns;
  while ((int32_t)(target - ccount()) > 0)
    ;
}

void __not_in_flash_func(llfl_wait_atn)(unsigned int state) {
  while (!IEC_ATN != !state)
    ;
  llfl_reference_time = ccount();
}

void __not_in_flash_func(llfl_wait_clock)(unsigned int state,
                                          llfl_atnabort_t atnabort) {
  if (atnabort == ATNABORT) {
    while (IEC_ATN && !IEC_CLOCK != !state)
      ;
  } else {
    while (!IEC_CLOCK != !state)
      ;
  }
  llfl_reference_time = ccount();
}

void __not_in_flash_func(llfl_wait_data)(unsigned int state,
                                         llfl_atnabort_t atnabort) {
  if (atnabort == ATNABORT) {
    while (IEC_ATN && !IEC_DATA != !state)
      ;
  } else {
    while (!IEC_DATA != !state)
      ;
  }
  llfl_reference_time = ccount();
}

void __not_in_flash_func(llfl_set_2bit_at)(uint32_t time,
                                           unsigned int clock_state,
                                           unsigned int data_state) {
  wait_to(time);
  set_clock(clock_state);
  set_data(data_state);
}

void __not_in_flash_func(llfl_set_clock_at)(uint32_t time, unsigned int state) {
  wait_to(time);
  set_clock(state);
}

void __not_in_flash_func(llfl_set_data_at)(uint32_t time, unsigned int state) {
  wait_to(time);
  set_data(state);
}

uint32_t __not_in_flash_func(llfl_read_bus_at)(uint32_t time) {
  wait_to(time);
  return iec_bus_read();
}

void __not_in_flash_func(llfl_generic_load_2bit)(const generic_2bit_t *def,
                                                 uint8_t byte) {
  byte ^= def->eorvalue;

  for (unsigned int i = 0; i < 4; i++) {
    uint8_t clock_state = byte & (1 << def->clockbits[i]);
    uint8_t data_state  = byte & (1 << def->databits[i]);
    wait_to(def->pairtimes[i]);
    set_clock(clock_state);
    set_data(data_state);
  }
}

uint8_t __not_in_flash_func(llfl_generic_save_2bit)(const generic_2bit_t *def) {
  uint8_t result = 0;

  for (unsigned int i = 0; i < 4; i++) {
    uint32_t bus = llfl_read_bus_at(def->pairtimes[i]);

    result |= (!!(bus & IEC_BIT_CLOCK)) << def->clockbits[i];
    result |= (!!(bus & IEC_BIT_DATA))  << def->databits[i];
  }

  return result ^ def->eorvalue;
}
