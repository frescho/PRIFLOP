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

/* flash_lockout.h: keep core 1 off the flash bus while core 0 erases or
   programs it.

   THE PROBLEM. flash_range_erase()/flash_range_program() put the QSPI
   flash into a state where it cannot serve XIP reads. Core 1 runs
   lwIP/httpd out of that same flash, so the first instruction-cache miss
   it takes during a commit fetches from a flash that isn't answering --
   and core 1 dies. Core 0 is unaffected: the SDK's flash routines are
   themselves RAM-resident and restore XIP before returning.

   Confirmed on real hardware 2026-09-04, not theorised: with a core-1
   heartbeat printing every 2s, a single config write produced
   "[c1:0][c1:1][c1:2]" and then silence forever -- and the device
   stopped answering ping and HTTP (lwIP gone) while the C64 side kept
   working perfectly. This affects ANY flash write, so it predates the
   Wi-Fi config UI; that feature just made it trivial to trigger from a
   browser.

   WHY NOT pico_flash's flash_safe_execute(). That is the SDK's answer to
   exactly this, and it was tried (commit ca074fe) and reverted
   (2026-09-03) because it badly disrupts core 0's real-time IEC bit-
   banging -- see ata.c's commit_slot() comment. This is the same idea
   reduced to what this project actually needs: a two-flag handshake with
   no SIO FIFO IRQ, no lockout victim registration, and nothing running
   on core 0's critical path.

   THE MECHANISM. core1.c calls multicore_lockout_victim_init() so core 0
   can park it from its SIO FIFO interrupt -- see flash_lockout.c for why
   an interrupt is required and a cooperative flag is not enough, and for
   why this is not the flash_safe_execute() that was reverted.
*/
#ifndef FLASH_LOCKOUT_H
#define FLASH_LOCKOUT_H

#include <stdbool.h>

/* Core 0, around a flash erase/program. begin() parks core 1; end()
   releases it. Call end() only if begin() returned true.

   begin() returns false if core 1 is alive but did not acknowledge in
   time. THE CALLER MUST THEN NOT TOUCH FLASH -- leave the data dirty and
   retry on a later idle pass. Erasing anyway is what an earlier version
   did, and it simply reintroduced the bug this file exists to fix: core 1
   was still executing from flash and died. Deferring costs nothing; the
   commit is already debounced and retried from BUS_IDLE.

   Returns true immediately when core 1 is not running at all, so a
   single-core build or a failed cyw43 init never blocks a commit. */
bool flash_lockout_begin(void);
void flash_lockout_end(void);

#endif
