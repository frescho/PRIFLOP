/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

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


   system.h: System-specific initialisation

*/

#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>

/* Early initialisation, called immediately in main() */
void system_init_early(void);

/* Late initialisation, called after all hardware is set up */
void system_init_late(void);

/* Put MCU into low-power mode until the next interrupt */
void system_sleep(void);

/* Stack instrumentation -- see system.c. stack_paint() must run early in
   main(), before anything goes deep. */
void     stack_paint(void);
uint32_t stack_high_water(void);
uint32_t stack_total(void);

/* Records a hard fault into watchdog scratch (survives the reset) and
   reports the previous one after boot. fault_last(0) is nonzero if the
   last reset was a fault; 1=PC, 2=LR, 3=SP. */
void     fault_check_previous(void);
uint32_t fault_last(unsigned int which);
void     boot_count_init(void);
uint32_t boot_count(void);

/* Same idea, for a hang rather than a fault -- see system.c. A watchdog
   reset alone doesn't say *why* it fired, so the worst values seen this
   boot are kept live in watchdog scratch; if a hang needs the watchdog
   to recover, the values from right before it survive into the next
   boot instead of being lost with the rest of RAM. Does NOT survive a
   hard power cycle -- only a soft/watchdog reset. */
void     hang_diag_scratch_init(void);

/* Second breadcrumb in the same investigation, repurposing the same
   scratch slot: confirmed twice, byte-for-byte, that the file-read
   offset right before a 4K-load hang is 4064 -- exactly 16*254, the
   refill that first has to reach across the littlefs block boundary --
   and confirmed separately that the read itself is fast (49us). So the
   read is not the bottleneck; the next candidate is the GAP before the
   following iec_putc() send, which -- unlike iec_getc()'s equivalent
   gap (src/iec.c) -- was never measured at all. CDOS's own receive loop
   (disassembly, boot80.prg) uses a small bounded retry count where this
   codebase's iec_putc() waits unboundedly (T_H is spec-legal 0..infinity, but only if the *other* side is also willing to
   wait) -- if a refill-triggered gap here ever exceeds what CDOS's own
   bounded wait tolerates, CDOS could time out and desync while PRIFLOP
   is still waiting per spec, which would hang exactly like this. */
void     iec_put_gap_scratch_note(uint32_t us);
uint32_t iec_put_gap_max_us_prev_boot(void);

/* Phase B2 (2026-09-07): a real hang showed
   refill_phase_prev_boot (below) coming back idle, ruling out
   buf->refill(buf)/find_buffer() as *that* hang's location -- pointing
   instead at _iec_putc()'s own five unbounded wait loops, none of which
   have a timeout, only an ATN check. These IDs name which one was
   running when a hang's watchdog fires; PUTC_LOOP_IDLE otherwise.
   Packed into the same scratch slot as put_gap above, loop id in the
   upper 4 bits. */
#define PUTC_LOOP_IDLE       0
#define PUTC_LOOP_E925       1  /* wait for DATA high before an EOI check       */
#define PUTC_LOOP_E937       2  /* wait for DATA high (EOI path)                */
#define PUTC_LOOP_E941       3  /* wait for DATA low again (EOI ack)            */
#define PUTC_LOOP_PRECLOCK   4  /* wait for DATA high after set_clock(0)        */
#define PUTC_LOOP_TXDONE     5  /* wait for DATA high after the byte is sent    */
#define PUTC_LOOP_TXBITS     6  /* sending the 8 data bits (PIO or software)    */

/* 2026-09-10: _iec_getc() (the SAVE/receive path) has the same shape of
   problem as _iec_putc() above -- several unbounded waits on the C64's
   side of the handshake, most gated only by ATN, two of them (DATAWAIT,
   BITWAIT_HIGH) not even by that. It hadn't been exercised the way LOAD
   was (PUTC_LOOP_TXDONE finding, observed
   once on real hardware and not yet reproduced) -- these values let a
   receive-side stall be diagnosed with the same precision, without
   burning a scratch register neither direction can spare. Reusing
   PUTC_LOOP_IDLE and the same nibble/accessor rather than adding a
   parallel field: LOAD and SAVE never run at the same time on this
   single-threaded bus handler, so one "which unbounded wait was this
   boot in" slot, split by value range, covers both directions. Same
   split for iec_refill_scratch_note()'s tx_position field below --
   pass iec_stat_rx_bytes when a GETC_LOOP_* id is set, iec_stat_tx_bytes
   when a PUTC_LOOP_* id is set; the field means "position in whichever
   direction was active", not "tx position" specifically. */
#define GETC_LOOP_CLOCKWAIT     7  /* wait for CLOCK high, byte start              */
#define GETC_LOOP_DATAWAIT      8  /* wait for DATA high (no ATN check at all)     */
#define GETC_LOOP_EOIACK        9  /* wait for CLOCK high after EOI ack            */
#define GETC_LOOP_PIOWAIT      10  /* PIO shadow: wait for the byte to be ready    */
#define GETC_LOOP_PIOPOSTBYTE  11  /* PIO shadow: wait for CLOCK low after byte    */
#define GETC_LOOP_BITWAIT_HIGH 12  /* per-bit wait for CLOCK high (no ATN check)   */
#define GETC_LOOP_BITWAIT_LOW  13  /* per-bit wait for CLOCK low after capture     */

/* 2026-10-02: the last byte did not complete. Every early return in
   _iec_putc()/_iec_getc() -- ATN, the listener dropping DATA, a PIO abort --
   used to leave the id of the wait it returned from in place, so a hang
   anywhere AFTER an aborted byte read back as if it had been inside that
   wait. 2026-10-01's reset reported PUTC_LOOP_TXBITS, where every delay
   is bounded and PIO signals done or abort within about 1.4 ms. Set by the iec_putc()/iec_getc() wrappers on any failure: "not in a
   byte wait; look at what runs after an aborted transfer". */
#define PUTC_LOOP_ABORTED      14  /* last byte sent was aborted               */
#define GETC_LOOP_ABORTED      15  /* last byte received was aborted           */

void     iec_putc_loop_note(uint8_t loop_id);
uint32_t iec_putc_loop_prev_boot(void);

/* Phase B1 continuation (2026-09-07): two
   different files died at the same/a nearby byte offset, clustering
   near littlefs's 4096-byte block boundary -- pointing at
   lfsop_file_read()'s lfs_file_tell()/lfs_file_size() EOF-fixup calls
   (src/lfsops.c) possibly behaving differently near that boundary.
   Those durations were already timed (lfsread_pre_max_us/
   lfsread_post_max_us) but only as current-boot stats, lost on the
   crash they'd be most useful for. These two survive it -- see
   system.c for why they're overwritten each call rather than maxed. */
void     lfsread_prepost_scratch_note(uint32_t pre_us, uint32_t post_us);
uint32_t lfsread_pre_last_us_prev_boot(void);
uint32_t lfsread_post_last_us_prev_boot(void);

/* Phase B1: the >4KB LOAD hang bisects to a
   race, not a fixed boundary, and reproduces identically on a real C64
   over the same code -- so it isn't Primo/CDOS-specific either. The put
   gap (above) times the whole span between two iec_putc() sends but
   doesn't say which part of it is slow. These two split that span:
   buf->refill(buf) in full (src/iec.c's talk loop) and the find_buffer()
   call right after it. Packed into the scratch slot retired from
   lfsread_scratch_note() above -- that stat had already answered its
   question three times over (the read itself is fast).

   A real hang (2026-09-07, C64 LOAD of an 8200-byte file) showed both
   durations coming back small (59us, 4us) despite the crash -- because a
   duration is only recorded once its call *returns*, and if the hang IS
   that call never returning, its time is never written. The phase field
   is the fix: written going INTO buf->refill(buf) or find_buffer(),
   before either has a chance to hang, so the next boot can tell which of
   the two was running even when it can't tell for how long.

   The second parameter started as a findbuf_us duration; repurposed the
   same day once that turned out uninformative (always single-digit
   microseconds, on both successful and crashing boots) to instead
   carry tx_position -- iec_stat_tx_bytes at the moment of the call,
   unconditionally overwritten rather than maxed, since a transfer
   position is monotonic within one file and "latest reached" is the
   useful answer, not "largest ever this boot" (which a multi-file boot
   could put in a different, misleading, file). Exists because a second
   real hang left every duration in this file looking perfectly
   ordinary (refill/put-gap both tens-to-low-hundreds of us) and D2's
   own opt-in timeout (iec_pio.h) never fired despite being armed --
   ruling out one call blocking for the full 8s watchdog window without
   explaining what did consume it. Knowing how far the transfer got
   settles whether that's an infinite retry (a small, unchanging
   position) or something else (a position close to the file end). See
   system.c for the exact bit packing and the REFILL_PHASE_* values. */
#define REFILL_PHASE_IDLE    0
#define REFILL_PHASE_REFILL  1
#define REFILL_PHASE_FINDBUF 2

void     iec_refill_scratch_note(uint32_t refill_us, uint32_t tx_position, uint8_t phase);
uint32_t iec_refill_max_us_prev_boot(void);
uint32_t iec_tx_position_prev_boot(void);
uint32_t iec_refill_phase_prev_boot(void);

/* Keep ATN answerable across a flash erase -- see system.c. Everything
   reachable while the shield is up must be RAM-resident. */
void flash_shield_begin(void);
void flash_shield_end(void);

/* Hang recovery -- see system.c. Pet wherever progress is made, not on a
   timer: long operations (a D81 format) never return to the mainloop. */
void system_watchdog_start(void);
void system_watchdog_pet(void);

/* Reset MCU */
__attribute__((noreturn)) void system_reset(void);

/* Enable/disable interrupts */
void disable_interrupts(void);
void enable_interrupts(void);

/* Optional hooks for an arch with networking (currently only src/pico):
   network_init() is called once from main() after the filesystem is
   mounted; network_poll() is called from src/iec.c's mainloop only when
   the IEC bus is idle (see the BUS_IDLE case), the same safe point
   disk_idle_sync() uses -- see src/pico/httpd.c's file header for why
   that matters (no locking needed against fatops.c/d64ops.c's FatFs
   usage because the two never run concurrently). Both have weak no-op
   defaults (in main.c/iec.c respectively) so every other arch is
   unaffected. Added 2026-08-23 for the web file-manager. */
void network_init(void);
void network_poll(void);

/* Optional hook for an arch with a second core (currently only
   src/pico, RP2350's core 1): called once from main() right after
   network_init(), for whatever that arch wants to launch on the other
   core. Weak no-op default (in main.c) so every other arch is
   unaffected. Added 2026-09-02 for backlog item 4 -- core
   1 will eventually own WiFi/lwIP/the web UI so that work isn't gated on
   the IEC bus being idle (see network_poll()'s doc above); this hook is
   step one of that: proving a second core can be launched at all before
   anything meaningful runs on it. */
void core1_launch(void);

/* Optional hook, called from src/iec.c's BUS_IDLE alongside
   disk_idle_sync() (same safe point, same reasoning): services one
   pending FatFs request from core 1's httpd.c per call, if any. Weak
   no-op default (in iec.c) so every other arch is unaffected. See
   src/pico/fs_bridge.h for the full request/response design -- added
   2026-09-02 for backlog item 4: core 1 never touches
   FatFs directly, this is what lets it get FatFs work done anyway. */
void fs_bridge_poll(void);

#endif
