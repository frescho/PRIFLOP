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


   primo_turbo.h: the drive's half of the Primo turbo.

   The protocol is described in docs/user/PRIMO.md; the Primo's half is
   tools/primo/turbo/turbo.asm. This file's logic is a port of `Drive` in
   tests/primo_turbo_sim.py, which is where it was proved against the
   Primo's real code under injected faults -- change the two together.

   Runtime switch, default OFF, like PIO transmit was until it had driven a
   real C64: with it off, `XT` answers 30,SYNTAX ERROR and the Primo falls
   back to stock CDOS, which is also how the fallback gets tested. The
   Primo's TURBO installer turns it on with the command `XT+` (and `XT-`
   off); POST /api/pio?primo=1|0 does the same from the web.
*/

#ifndef PRIMO_TURBO_H
#define PRIMO_TURBO_H

#include <stdint.h>
#include <stdbool.h>

/* Claim a state machine and load the program. Not fatal if none is free:
   `XT` is then refused, as when switched off. */
bool primo_turbo_init(void);

/* The `XT` command (doscmd.c's parse_xcommand). Sets the error channel to
   `00,TURBO,01,00` and arms a session, or refuses. */
void primo_turbo_command(void);

/* Called from iec.c's BUS_CLEANUP after every transaction, with the last
   command byte received under ATN. Runs the armed session after the UNTALK
   (0x5F) that ends the Primo's read of the answer to `XT`, and returns when
   it is over. Not earlier: the talk handler itself ends in BUS_CLEANUP
   before the UNTALK arrives, and a session started there leaves the
   UNTALK unanswered (found reading iec.c, 2026-10-05). */
void primo_turbo_poll(uint8_t last_cmd);

/* The switch. `XT+` / `XT-` turn it on and off and set PRIMO_TURBO in
   globalflags with it, as `XE+` / `XE-` do for extension hiding: `XW` saves
   it. The web UI's Settings tab sets and saves it at once. At start the
   saved value applies (primo_turbo_apply_setting(), after
   read_configuration()). /api/pio?primo= changes only the running switch,
   for diagnostics. Saved since 2026-10-07: our own DOS needs
   the turbo, and a drive restart used to turn it off. */
extern volatile uint8_t primo_turbo_enabled;
void primo_turbo_apply_setting(void);
bool primo_turbo_ready(void);   /* its PIO state machine was claimed */

/* Counters for /api/stats. A retry the protocol hid stays visible here. */
typedef struct {
  uint32_t sessions;      /* XT accepted and turbo entered          */
  uint32_t completed;     /* ended by the Primo's quit              */
  uint32_t aborted;       /* ATN, silence, or no PIO                */
  uint32_t exchanges;     /* frames confirmed                       */
  uint32_t resent;        /* a read frame asked for again           */
  uint32_t garbled;       /* a frame number that failed its check   */
  uint32_t refused;       /* a number neither in hand nor next      */
  uint32_t bad_writes;    /* a write that failed its check          */
  uint32_t dup_writes;    /* a stored write sent again, dropped     */
  uint32_t reissued;      /* an R order given again (lost verdict)  */
  uint32_t resyncs;       /* the 2 ms idle reset                    */
  uint32_t bytes_to;      /* W bytes to the Primo                   */
  uint32_t bytes_from;    /* R bytes from the Primo                 */
  uint32_t us;            /* time inside sessions                   */
  uint32_t call_waits;    /* idle resets while the Primo ran a 0xCD
                             call ('K'): designed waits, not faults,
                             so kept out of `resyncs`  */
} primo_turbo_stats_t;

extern volatile primo_turbo_stats_t primo_turbo_stats;

/* Why the last session ended, and how far it got: 0 completed, 1 ATN never
   released, 2 ATN during the session, 3 silence. */
extern volatile int8_t   primo_turbo_last_reason;
extern volatile uint32_t primo_turbo_last_edges;

/* SELF-TEST, no Primo needed (POST /api/pio?primotest=1, at BUS_IDLE): the
   drive's CPU plays the Primo -- it flips CLOCK itself and reads DATA -- and
   checks that the PIO program answers on the real pins. `levels` has bit k
   set if DATA read high after edge k (expected 0x0B5: the go edge high, then
   0x5A's bits, then low -- the word after the hold edge is already queued, so
   the PIO drives it at once); `ready` and `imm` are 1 if DATA read low where
   it must (the ready pull; that word). `samples` should be 10. */
typedef struct {
  uint32_t runs;
  uint8_t  err;           /* 1 no PIO, 2 the bus was not idle */
  uint8_t  ready;
  uint16_t levels;
  uint8_t  samples;
  uint8_t  imm;
} primo_turbo_selftest_t;
extern volatile primo_turbo_selftest_t primo_turbo_selftest;
extern volatile uint8_t primo_turbo_selftest_req;

/* Where the last session was when a reset ended it: phase (1 start, 4 an
   order -- detail op<<16|length, 5 waiting on the Primo -- detail edges,
   6 a write, 7 opening, 8/9 before/after a write refill, 10 a read refill,
   11/12 before/after closing a saved file, 13 file_open returned), frame
   number, detail, valid, the stack pointer there and the lowest one seen,
   then the last deep step (primo_turbo_deep), how many were marked, and
   the microseconds from the phase to that step. */
extern volatile uint32_t primo_turbo_prev[9];

/* Marks a step inside file_open, littlefs or the flash write path while a
   session's breadcrumb is live: 1x file_open, 2x create_file, 3x flash
   program, 4x flash erase, 5x uart_trace (the numbers at the call sites). */
void primo_turbo_deep(uint32_t step);

/* Called at BUS_IDLE (iec.c's apply_web_settings): runs a requested self-test. */
void primo_turbo_idle(void);

#endif
