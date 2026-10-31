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


   iec_pio.h: PIO assist for the IEC bus.

   STATUS: SHADOW MODE. The state machine decodes the same bytes the
   software loop in _iec_getc() is decoding, and the two are compared.
   Nothing on the bus depends on the result yet -- the software path is
   still authoritative and still drives every line.

   That is deliberate and is the whole point of this step. A PIO receive
   path that is "probably right" is worth nothing: the failure mode is a
   corrupted byte in the middle of a loaded game, which looks like a bad
   disk image rather than a bus bug. Running both against real traffic and
   counting disagreements turns "probably" into a number, at zero risk,
   before anything is allowed to depend on it.

   The split is bit-level PIO plus
   byte-level CPU, and command bytes are not handled here.
*/

#ifndef IEC_PIO_H
#define IEC_PIO_H

#include <stdint.h>
#include <stdbool.h>

/* Set up the receive state machine. Safe to call when no PIO resources
   are free: it fails quietly and every other entry point becomes a no-op,
   so the software path keeps working. Returns true if PIO is available. */
bool iec_pio_init(void);

/* True once iec_pio_init() has succeeded. */
bool iec_pio_available(void);

/* Start decoding a byte. Call immediately before the eight-bit loop, with
   CLOCK already low, so the state machine's first rising edge is bit 0.
   Resets the state machine and empties the FIFO, so a byte abandoned
   part-way (an ATN abort) cannot desynchronise the next one. */
void iec_pio_rx_arm(void);

/* Compare what PIO decoded against what the software loop decoded, and
   account for it. `sw_val` is the software result. Counts a match, a
   mismatch, or a miss (PIO produced no byte -- normally an ATN abort). */
void iec_pio_rx_check(uint8_t sw_val);

/* True once the state machine has a byte ready. */
bool iec_pio_rx_ready(void);

/* Take the decoded byte. Only valid after iec_pio_rx_ready() returns
   true; draining an empty FIFO would stall. */
uint8_t iec_pio_rx_get(void);

/* Which decoder is authoritative.

   Runtime rather than compile-time, and that is worth the byte it costs:
   it makes an A/B comparison on the same firmware possible, and it means
   falling back after a bad result is a POST rather than a reflash --
   which matters when the thing being switched sits between a C64 and its
   disks. Defaults per IEC_PIO_RX_OWNS_DEFAULT below. */
extern volatile uint8_t iec_pio_rx_owns;

/* ---- transmit -------------------------------------------------------
   Unlike receive, this DRIVES the bus, which makes it a different kind of
   change: PIO can read a pin without owning it but cannot write one, so
   for the duration of the eight bits the pin function select is handed to
   PIO and gpio_set_dir() stops working on CLOCK and DATA.

   That window is kept as small as possible -- claimed immediately before
   the bit loop, released immediately after -- because iec_atn_handler()
   pulls DATA to answer ATN, and that is the T_AT path the whole
   flash-shield design exists to protect. Outside the window nothing has
   changed. Inside it, the ISR takes the pin back first; see
   iec_pio_atn_reclaim().

   Defaults ON since 2026-09-06, having been proved against a real C64 --
   see IEC_PIO_TX_OWNS_DEFAULT in iec_pio.c for the numbers.
   POST /api/pio?tx=0 returns the software loop at runtime. */

/* Set up the transmit state machine. Independent of receive: either can
   be available without the other. */
bool iec_pio_tx_init(void);

/* True when PIO transmit is usable AND selected. VC20 mode reports false
   -- it shortens one delay from 75us to 34us and the program has no
   branch for it, so that case keeps the software loop. */
bool iec_pio_tx_enabled(void);

/* Send the eight data bits of one byte.
   Returns  0 on success,
           -1 if the listener dropped DATA mid-byte (caller: BUS_CLEANUP),
           -2 if ATN went active (caller: return to the mainloop),
           -3 if iec_tx_timeout_enabled and the listener never signalled
              DONE at all (caller: BUS_CLEANUP, same as -1 -- a stuck
              listener is the same class of failure as a dropped one). */
int iec_pio_tx_byte(uint8_t data);

/* Called from the ATN interrupt BEFORE it pulls DATA. If PIO is holding
   the lines, hands them straight back to SIO so set_data(0) works. Two
   register writes, no function calls, RAM-resident. */
void iec_pio_atn_reclaim(void);

/* True while PIO holds CLOCK and DATA. Read by the ATN interrupt. */
extern volatile uint8_t iec_pio_pins_held;

/* Selects PIO for transmit. Defaults 0. */
extern volatile uint8_t iec_pio_tx_owns;

/* Transmit counters, reported by /api/stats. */
extern volatile uint32_t iec_pio_tx_bytes;    /* bytes sent by PIO        */
extern volatile uint32_t iec_pio_tx_abort;    /* listener dropped DATA    */
extern volatile uint32_t iec_pio_tx_atn;      /* ATN cut a byte short     */

/* Phase D2 (2026-09-07): both this function's
   own DONE-wait and _iec_putc()'s tx_done wait (src/iec.c) are unbounded
   waits for the listener to finish acknowledging a sent byte, with no
   protection at all against the listener simply never responding --
   confirmed directly, not theorized, as the site of two real hangs, one
   in each of these two byte-send implementations. Opt-in and default
   OFF so C64 behaviour cannot silently change: POST /api/txtimeout?
   enabled=1 toggles it at runtime, the same pattern as /api/pio.
   iec_tx_timeout_us is deliberately generous (default 1s -- orders of
   magnitude past any real handshake, comfortably under the 8s
   watchdog) so it only fires on a genuine stuck listener, never on
   ordinary bus jitter. Does not (yet) cover this file's/_iec_putc()'s
   other unbounded waits (E925/E937/E941/the post-clock-low wait) --
   scoped to the two waits actually caught hanging so far. */
extern volatile uint8_t  iec_tx_timeout_enabled;
extern volatile uint32_t iec_tx_timeout_us;
extern volatile uint32_t iec_tx_timeouts;    /* times it actually fired  */

/* With the timeout off (the default, decision 11b of 2026-10-15: option A),
   a listener that never acknowledges -- a hung C64 holding DATA -- is waited
   for as long as it takes, and both waits feed the watchdog once a second
   while they do, so the drive stays up instead of being reset after 8 s.
   ATN still ends the wait: resetting the C64 frees the drive. This counts
   the waits that lasted longer than a second. */
#define IEC_TX_PET_US 1000000u
extern volatile uint32_t iec_tx_long_waits;

/* Shadow-mode results, reported by /api/stats. */
extern volatile uint32_t iec_pio_match;     /* PIO agreed with software  */
extern volatile uint32_t iec_pio_mismatch;  /* PIO disagreed             */
extern volatile uint32_t iec_pio_miss;      /* PIO had no byte to offer  */
extern volatile uint8_t  iec_pio_last_sw;   /* last disagreement, sw side */
extern volatile uint8_t  iec_pio_last_pio;  /* last disagreement, PIO side */

#endif
