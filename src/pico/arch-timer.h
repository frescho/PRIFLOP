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


   arch-timer.h: Architecture-specific system timer definitions (RP2350)

   STATUS: coarse delay/timeout functions only, using
   the RP2350's always-on 1 MHz hardware timer (time_us_32/busy_wait_us),
   matching what the ESP8266 arch provides. This is NOT the fastloader
   pulse-capture timer the AVR/LPC17xx arches implement via dedicated
   capture-mode hardware timers wired to the IEC lines -- that capability
   is deliberately deferred to Phase 2, where it
   is expected to be built on PIO instead of a capture timer, since PIO
   is immune to flash-XIP stalls.
   Until Phase 2 lands, fastloaders requiring precise pulse capture will
   not work on this arch, same as the current state of the ESP8266 port.
*/

#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

#include <stdint.h>

/* Types for unsigned and signed tick values */
typedef uint32_t tick_t;
typedef int32_t stick_t;

/* Delay functions - pure busy-wait on the RP2350's always-on 1 MHz
   hardware timer (hardware/timer.h busy_wait_us*), NOT the SDK's
   sleep_us/sleep_ms (those may use WFE/low-power wait paths that are
   not appropriate to call with interrupts disabled, which is exactly
   when delay_us/delay_ms tend to get used in this codebase). */
void delay_us(unsigned int time);
void delay_ms(unsigned int time);

/* Timeout functions */
void start_timeout(unsigned int usecs);
unsigned int has_timed_out(void);

#endif
