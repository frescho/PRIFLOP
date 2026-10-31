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

/* flash_lockout.c: see flash_lockout.h for the problem this solves.

   Implemented with the SDK's multicore_lockout primitives rather than a
   hand-rolled flag handshake, because a cooperative flag fundamentally
   cannot do the job: core 1 only notices a request when it comes back
   around its poll loop, so anything that keeps it inside SDK code for a
   while -- cyw43_arch_init(), the up-to-20s blocking station-mode join,
   a long lwIP callback -- leaves it unparked and still executing from
   flash. A first attempt here did exactly that and, on timeout,
   "proceeded anyway", which simply recreated the crash it existed to
   prevent (2026-09-04).

   multicore_lockout parks core 1 from its SIO FIFO *interrupt*, so it
   works regardless of what core 1 is doing, and its handler is
   __not_in_flash_func() so the parked core never touches flash.

   WHY THIS IS NOT THE REVERTED flash_safe_execute(). Commit ca074fe
   routed these writes through pico_flash's flash_safe_execute() and was
   reverted 2026-09-03 for badly disrupting core 0's IEC bit-banging --
   but that investigation was explicit that the harm came from
   flash_safe_execute() *being invoked*, and that "merely calling
   flash_safe_execute_core_init() is harmless". That init call is exactly
   multicore_lockout_victim_init(). So this uses the harmless half
   directly and skips the wrapper: it claims the SIO FIFO IRQ on core 1
   only, never touches core 0's NVIC, and adds nothing to core 0's
   critical path.
*/
#include <stdbool.h>

#include "pico/multicore.h"
#include "flash_lockout.h"

/* Long enough that a healthy core 1 always makes it (the lockout is
   interrupt-driven, so this is microseconds in practice), short enough
   that a wedged core 1 doesn't stall the bus indefinitely. Safe to wait
   here at all because every commit happens at BUS_IDLE -- both the
   debounced disk_idle_sync() and fs_bridge_poll()'s write eviction are
   called from there, never mid-transaction. */
#define FLASH_LOCKOUT_TIMEOUT_US 100000

bool flash_lockout_begin(void) {
  /* Nobody to lock out: core 1 was never launched, or never got as far
     as registering. Proceed -- this is the single-core case. */
  if (!multicore_lockout_victim_is_initialized(1)) {
    return true;
  }
  return multicore_lockout_start_timeout_us(FLASH_LOCKOUT_TIMEOUT_US);
}

void flash_lockout_end(void) {
  if (!multicore_lockout_victim_is_initialized(1)) {
    return;
  }
  multicore_lockout_end_timeout_us(FLASH_LOCKOUT_TIMEOUT_US);
}
