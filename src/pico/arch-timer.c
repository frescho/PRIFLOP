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


   arch-timer.c: Architecture-specific timer functions (RP2350)

   Provides the HZ=100 system tick (src/timer.c's SYSTEM_TICK_HANDLER,
   which arch-config.h must expand to "void isr_systick(void)" so the
   Pico SDK's weak vector table in pico_crt0/crt0.S picks it up -- no
   explicit registration call is needed, unlike the ESP8266 arch which
   has to hand a function pointer to hw_timer_set_func()) plus the
   delay_us/delay_ms/start_timeout/has_timed_out contract from
   arch-timer.h. See arch-timer.h for what this deliberately does NOT
   provide yet (fastloader pulse-capture timing -- Phase 2/PIO work).
*/

#include "config.h"
#include "timer.h"

#include "hardware/clocks.h"
#include "hardware/structs/systick.h"
#include "hardware/timer.h"

/* M33_SYST_CSR bits (ARMv8-M architectural SysTick, see systick.h) */
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_TICKINT   (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2) /* 1 = processor clock */

void timer_init(void) {
  /* SysTick reload is 24-bit; at 150MHz a 24-bit counter would need to
     wrap in <112ms, so we don't need to worry about the reload value
     overflowing 0xFFFFFF for a 10ms (100Hz) period on any RP2350 clock
     configuration this project uses. */
  uint32_t sys_clk_hz = clock_get_hz(clk_sys);
  uint32_t reload = sys_clk_hz / HZ - 1;

  systick_hw->rvr = reload;
  systick_hw->cvr = 0;
  systick_hw->csr = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

void delay_us(unsigned int time) {
  busy_wait_us_32(time);
}

void delay_ms(unsigned int time) {
  busy_wait_ms(time);
}

static uint32_t timeout_target;

/**
 * start_timeout - start a timeout
 * @usecs: number of microseconds before timeout
 *
 * This function sets up a timer so it times out after the specified
 * number of microseconds.
 */
void start_timeout(unsigned int usecs) {
  timeout_target = time_us_32() + usecs;
}

/**
 * has_timed_out - returns true if timeout was reached
 *
 * This function returns true if the timer started by start_timeout
 * has reached its timeout value.
 */
unsigned int has_timed_out(void) {
  /* Wraparound-safe: true once (now - target) no longer looks negative
     when reinterpreted as a signed 32-bit delta. Mirrors the ESP8266
     arch's start_timeout/has_timed_out pair. */
  return (int32_t)(time_us_32() - timeout_target) >= 0;
}
