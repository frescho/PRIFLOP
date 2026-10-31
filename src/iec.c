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


   iec.c: IEC handling code, stateful version

   This code is a close reimplementation of the bus handling in a 1571
   to be as compatible to original drives as possible. Hex addresses in
   comments refer to the part of the 1571 rom that particular section
   is based on.

*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "atomic.h"
#include "buffers.h"
#include "d64ops.h"
#include "diskchange.h"
#include "diskio.h"
#include "display.h"
#include "doscmd.h"
#include "eeprom-conf.h"
#include "errormsg.h"
#include "fastloader.h"
#include "fastloader-ll.h"
#include "lfsops.h"
#include "flags.h"
#include "fileops.h"
#include "filesystem.h"
#include "parser.h"
#include "iec-bus.h"
#include "led.h"
#include "system.h"
#include "timer.h"
#include "uart.h"
#include "iec.h"
#include "iec_pio.h"
#include "primo_turbo.h"
#include "bootlog.h"
#include "llfl-common.h"

/* See BUS_ATNFINISH below: should stay zero for the life of the device. */
volatile uint32_t iec_atnfinish_bails;

/* The last eight command bytes received under ATN, and a copy frozen at the
   most recent bail, oldest first (/api/stats "bail_cmds"), with the device
   state the drive was in ("bail_dev": 0 idle, 1 listen, 2 talk, as
   DEVICE_*). Added 2026-10-03: with a ground loop on the bench a bail came
   every two or three IECTEST rounds, i.e. the drive took a
   command sequence for finished while the C64 was still in it -- one byte
   misread, but which? A bail means "waited a second for ATN to go away", so
   the byte that sent us there is the last one in the copy. A hang without
   a bail -- the C64 waiting while the drive idles -- leaves no copy, so the
   live ring is reported too ("last_cmds"), with the bus state. Plain RAM writes
   in the main loop, nothing on the console (invariant 2). */
volatile uint8_t iec_cmd_ring[8];      /* also read live: /api/stats "last_cmds" */
volatile uint8_t iec_cmd_head;
volatile uint8_t iec_bail_cmds[8];
volatile uint8_t iec_bail_dev;

/* ------------------------------------------------------------------------- */
/*  Global variables                                                         */
/* ------------------------------------------------------------------------- */

/* Current device address */
uint8_t device_address;

/* Cross-core request to change device_address and persist it, set by
   httpd.c's web UI (core 1) and applied here at BUS_IDLE (core 0) --
   never applied directly from core 1. write_configuration() ends in an
   EEPROM commit (arch-eeprom.c's eeprom_patch()), which parks core 1
   internally while it erases/reprograms flash; core 1 cannot be the one
   calling it without trying to park itself. -1 means no change is
   pending; a valid device address is always >= 0 (see doscmd.c's `& 0x1f`
   masking of the wire format), so that sentinel can't collide with a
   real request. */
volatile int16_t pending_device_address = -1;

/* Same protocol for extension hiding (`XE+`/`XE-` from the C64): 1 on, 0
   off, -1 nothing pending. Applied at BUS_IDLE and persisted, i.e. `XE+`
   followed by `XW`. Added when every reflash still reset this setting and
   restoring it took typing two DOS commands on the C64 -- without SHIFT on
   the plus; the reflash part is fixed since. */
volatile int8_t pending_ext_hiding = -1;

/* Same for the Primo turbo (`XT+`/`XT-` and `XW`; primo_turbo.h). */
volatile int8_t pending_primo_turbo = -1;

iec_data_t iec_data;

/* Settings changed from the web UI (core 1), applied and saved here on core
   0 -- write_configuration()'s EEPROM commit has to run on this core. Called
   on entry to BUS_IDLE and on every pass of its wait-for-ATN loop. It used
   to run on entry only, so with the bus quiet a change sat queued until the
   C64's next transaction: "takes effect immediately" in the web UI was not
   true until 2026-10-02. Two volatile reads when nothing is pending. */
static void apply_web_settings(void) {
  primo_turbo_idle();     /* a requested turbo self-test (primo_turbo.h) */
  if (pending_device_address >= 0) {
    device_address = (uint8_t)pending_device_address;
    pending_device_address = -1;
    display_address(device_address);
    write_configuration();
  }
  if (pending_ext_hiding >= 0) {
    if (pending_ext_hiding)
      globalflags |= EXTENSION_HIDING;
    else
      globalflags &= (uint8_t)~EXTENSION_HIDING;
    pending_ext_hiding = -1;
    write_configuration();
  }
  if (pending_primo_turbo >= 0) {
    if (pending_primo_turbo)
      globalflags |= PRIMO_TURBO;
    else
      globalflags &= (uint8_t)~PRIMO_TURBO;
    pending_primo_turbo = -1;
    primo_turbo_apply_setting();
    write_configuration();
  }
}

/* Set while a bus transaction is in progress, so a second core (if the
   arch has one) can stay off the shared flash/XIP path until we are back
   at BUS_IDLE. Only src/pico/core1.c reads it today; harmless elsewhere.

   This restores a constraint the original single-core design had for
   free: network_poll() was only ever called from BUS_IDLE, so Wi-Fi work
   could never overlap a live transaction. Backlog item 4 moved that work
   to core 1 but did not carry the constraint across -- core 1 polled
   lwIP/cyw43 flat out, and its constant flash fetches slowed core 0
   enough to miss the C64's TALK phase entirely (confirmed on real
   hardware 2026-09-04: with core 1 parked in __wfi(), LOAD"$",8 completes
   perfectly; with it polling, it stalls at SEARCHING/LOADING). */
volatile bool iec_bus_busy = false;

/* IEC_TRACE removed 2026-09-06.

   It printed per-command markers ('L', 'T', 'A28', 'C') to the console
   from inside live bus transactions, guarded by -DCONFIG_IEC_TRACE. Two
   reasons it is gone rather than merely still off by default.

   It could not be built. Its probe blocks referenced iec_probe_getc_max
   and friends, which were replaced by the always-on iec_stat_* counters
   below; anyone who actually passed -DCONFIG_IEC_TRACE got compile
   errors, so it was not a working tool, it was a trap wearing one's
   clothes.

   And what it did is now a documented way to hang the drive. A console
   write from the bus path can block for PICO_STDIO_USB_STDOUT_TIMEOUT_US
   per character when nothing drains the endpoint -- the same mechanism
   that produced a watchdog reset loop from a 2KB log dump earlier today.
   The always-on counters below cost three timer reads per byte, print
   nothing, and answer the questions this was used for. */

/* Bus statistics, always on.

   These print nothing and allocate nothing -- three timer reads per byte
   against a byte time of roughly a millisecond, so the cost is noise. The
   reads are time_us_32(), which start_timeout()/has_timed_out() already
   call from inside _iec_getc(), so nothing new is being trusted on the
   time-critical path.

   They exist to answer one question with numbers instead of opinion:
   how much of core 0 is spent bit-banging, and how close the worst case
   runs to the published limits. That is the case for or against moving
   the bus to PIO, and until now the project had no measurement of it --
   the counters that were here were declared but never incremented, and
   compiled out besides. */
volatile uint32_t iec_stat_rx_bytes;      /* bytes received            */
volatile uint32_t iec_stat_tx_bytes;      /* bytes sent                */
volatile uint32_t iec_stat_getc_max_us;   /* worst single byte in      */
volatile uint32_t iec_stat_putc_max_us;   /* worst single byte out     */
volatile uint32_t iec_stat_gap_max_us;    /* worst gap between bytes   */
/* Same idea as the receive-side gap above, for the send side -- added
   2026-09-07, investigating a real hang. iec_getc() has always measured
   the gap before a receive; iec_putc() never measured the gap before a
   send at all, so a refill-triggered delay right before transmitting the
   next chunk was invisible. See system.h's iec_put_gap_scratch_note()
   comment for the full chain of reasoning this is testing. */
volatile uint32_t iec_stat_put_gap_max_us;
/* Phase B1 (2026-09-07): the put gap above
   times the whole span between two iec_putc() sends but doesn't say
   which part of it is slow. These split it: the whole buf->refill(buf)
   call in iec_talk_handler()'s LOAD loop, and the find_buffer() lookup
   right after it. Whichever one accounts for most of the ~280us gap
   that lfs_file_read() alone doesn't explain is the thing to make fast. */
volatile uint32_t iec_stat_refill_max_us;
volatile uint32_t iec_stat_findbuf_max_us;
volatile uint32_t iec_stat_busy_ms;       /* total time in the bit loops */
volatile uint32_t iec_stat_atn_aborts;    /* transfers cut short by ATN */

/* JiffyDOS traffic, counted separately from the slow protocol.

   It has to be separate, because mixing the two produces a number that
   means nothing. The first measurement after JiffyDOS started working
   read 574 B/s against a 552 B/s slow-protocol baseline, which looks like
   "the fastloader bought nothing" -- but only one of the three jiffy call
   sites sits inside the counted iec_putc() wrapper. The other two, the
   receive path and the bulk LOAD send in iec_talk_handler(), were not
   measured at all, and the bulk send is where the entire speed advantage
   lives. The figure was averaging a fast path it could not see with a
   slow one it could. */
volatile uint32_t iec_stat_jf_rx;         /* bytes received via JiffyDOS */
volatile uint32_t iec_stat_jf_tx;         /* bytes sent via JiffyDOS     */
volatile uint32_t iec_stat_jf_us;         /* microseconds inside those   */

/* The flash-overlap experiment is over; its counters are gone.
   Question: could a sector erase stall a byte mid-transmission? A talker
   that holds CLOCK released past T_NE (200us) is telling the C64 "EOI",
   so a 38ms erase landing inside iec_putc() would be a protocol
   violation rather than a pause -- and putc_max and flash_op_max_us came
   back 31us apart out of 38ms, which looked like exactly that.
   Answer (2026-09-05): no. Zero of 15787 bytes contained a flash
   operation, across 47783 of them. The "flash only between bytes"
   invariant holds and the near-match was coincidence -- 38ms is simply
   how long a 4096-byte sector erase takes. */
extern volatile uint32_t flash_ops;
static   uint32_t iec_stat_busy_us_frac;  /* sub-ms remainder of the above */
static   uint32_t iec_stat_last_end;

/* Fold a completed byte's duration into the accumulators. Kept out of
   line of the two callers so they read the same way. */
static void iec_stat_account(uint32_t dt, volatile uint32_t *max) {
  if (dt > *max && dt < 1000000u)
    *max = dt;
  iec_stat_busy_us_frac += dt;
  if (iec_stat_busy_us_frac >= 1000u) {
    iec_stat_busy_ms      += iec_stat_busy_us_frac / 1000u;
    iec_stat_busy_us_frac %= 1000u;
  }
}

/* ------------------------------------------------------------------------- */
/*  Very low-level bus handling                                              */
/* ------------------------------------------------------------------------- */

/* Root-cause investigation, 2026-09-07 (B):
   the >4KB LOAD hang traced to _iec_putc()'s tx_done wait, which never
   returns because the listener never releases DATA -- but PRIFLOP's own
   send just before that point is the more likely place for something to
   have gone wrong first. Checked with `nm`/objdump: delay_us() (this
   whole file's timing primitive, called from every one of these
   RAM-resident bus functions, several times per byte) is NOT
   RAM-resident. It is a real out-of-line call through a linker veneer
   into flash -- confirmed in the disassembly, not just the symbol table
   -- because it forwards to the SDK's busy_wait_us_32(), which itself
   calls time_us_64(), also flash-resident. Any one of those calls can
   stall if core 1 is fetching Wi-Fi/lwIP code from flash at that exact
   moment, silently stretching a `delay_us(22)` into something longer --
   invisible to PRIFLOP's own execution, but exactly the kind of
   corrupted bit timing that could desync a listener's own cycle-counted
   receive routine without PRIFLOP itself ever appearing to hang for it.

   time_us_32() has none of this exposure: pico-sdk's hardware/timer.h
   defines it (and the timer_time_us_32() it forwards to) as `static
   inline`, bottoming out in `return timer->timerawl;` -- a bare
   memory-mapped register read, no flash involved at all. Building this
   port's own busy-wait on that instead removes the exposure entirely.
   `static inline` so it costs nothing the old delay_us() didn't already
   cost, and so it inlines directly into whichever IRAM_ATTR caller uses
   it -- no out-of-line branch of its own to worry about either. Used in
   place of delay_us() everywhere within this file's RAM-resident bus
   path (iec_debounced(), _iec_getc(), _iec_putc(),
   iec_listen_handler(), iec_talk_handler()); left alone elsewhere (e.g.
   iec_mainloop()), which is not RAM-resident and so was never exposed
   to this in the first place. */
static inline void iec_bus_delay_us(uint32_t us) {
  uint32_t start = time_us_32();
  while ((uint32_t)(time_us_32() - start) < us) ;
}

/// Debounce IEC input - see E9C0
IRAM_ATTR
static iec_bus_t iec_debounced(void) {
  iec_bus_t tmp;

  do {
    tmp = iec_bus_read();
    iec_bus_delay_us(2); /* 1571 uses LDA/CMP/BNE, approximate by waiting 2us */
  } while (tmp != iec_bus_read());
  return tmp;
}

/* CLOCK went high: is it still high 2 us later? The bit loop below took a
   single read as the talker's "bit valid", while the falling edge has always
   gone through iec_debounced(). Command bytes under ATN are received only
   here, never by PIO, so on 2026-10-03, with a ground loop on the bench, a spike on CLOCK could still clock in a false bit of a
   TALK: the drive missed it, and the C64 waited for ever in the KERNAL's
   turnaround -- while the data bytes, through PIO's confirmed edges
   (iec_rx.pio), came through clean. DATA is still taken from the first high
   read, at the edge, as before: the confirmation only decides whether that
   read counts. 2 us of the at least 20 us a bit stays valid (T_V). */
IRAM_ATTR
static uint8_t iec_clock_high_confirmed(void) {
  iec_bus_delay_us(2);
  return (iec_bus_read() & IEC_BIT_CLOCK) != 0;
}

/// Checks if ATN has changed and changes state to match (EA59)
/* A change of ATN counts only if a second read 2 us later agrees (2026-10-03).
   This is called from inside every byte wait, so a single read that caught a
   spike aborted the byte being received: with a ground loop on the bench the
   drive got a TALK, lost the secondary address after it, and the C64 waited
   for ever in the turnaround -- the same confirm-before-trust as
   iec_clock_high_confirmed() and the PIO receive program. Costs nothing while
   ATN is unchanged: the second read happens only when the first sees a
   change. Answering ATN within T_AT is the ATN interrupt's job, not this. */
IRAM_ATTR
uint8_t iec_check_atn(void) {
  if (iec_data.bus_state == BUS_ATNACTIVE)
    if (IEC_ATN) {
      iec_bus_delay_us(2);
      if (!IEC_ATN)
        return 0;                          // a spike, not a release
      iec_data.bus_state = BUS_ATNPROCESS; // A9AC
      iec_stat_atn_aborts++;
      return 1;
    } else
      return 0;
  else
    if (!IEC_ATN) {
      iec_bus_delay_us(2);
      if (IEC_ATN)
        return 0;                          // a spike, not an assertion
      iec_data.bus_state = BUS_FOUNDATN;   // A7B3
      iec_stat_atn_aborts++;
      return 1;
    } else
      return 0;
}

/* Hold the bus across a flash operation.
 *
 * A flash erase runs with interrupts disabled for ~38ms (measured; see
 * /api/stats' flash_blocked_max_us), because the RP2350 cannot fetch from
 * XIP while it is in progress. The ATN handler therefore cannot run, and
 * the published limit for answering ATN by pulling DATA is 1000us
 * (T_AT) -- exceed it and the host reports DEVICE NOT
 * PRESENT. 38ms is 38 times the entire budget.
 *
 * Pulling DATA *before* the erase satisfies the requirement in advance:
 * ATN asserted during the window finds DATA already low, which is exactly
 * what it is waiting for. This is what a real 1541 does while it is busy,
 * and it is why a drive can take its time on a slow operation at all.
 *
 * Not done while this device is the talker -- then DATA belongs to the
 * listener and driving it would corrupt the transfer. Writes that happen
 * mid-receive were already safe without this, because a listener may hold
 * off indefinitely (T_H is 0..infinite), which is why plain file writes
 * never showed the problem and writing into a mounted image did. */
/* Set once the bus pins are configured and the mainloop owns them. Before
   that, driving DATA is both meaningless and unsafe -- flash writes can
   happen during boot, long before iec_interface_init(). */
static volatile bool iec_bus_ready = false;

void iec_flash_hold_ready(void) {
  iec_bus_ready = true;
}

bool iec_flash_hold_begin(void) {
  if (!iec_bus_ready)
    return false;
  if (iec_data.device_state == DEVICE_TALK)
    return false;
  set_data(0);
  return true;
}

void iec_flash_hold_end(bool held) {
  if (!held)
    return;
  /* ALWAYS release. An earlier version kept DATA asserted whenever ATN
     read low, reasoning that the command which arrived during the blind
     window now owned the bus. That is wrong whenever nothing is driving
     ATN: a powered-off C64 holds ATN low through its own drivers, so DATA
     was asserted on the first flash write and never released, and the
     drive wedged with the bus held. Cost one recovery flash, 2026-09-05.

     Releasing unconditionally is safe: if a real ATN is active the state
     machine re-asserts DATA on its next pass, which is well inside T_AT
     because that path is RAM-resident and takes microseconds. */
  set_data(1);
}

/* IEC ATN handler (if Dreamload is not used).
 *
 * IRAM_ATTR is load-bearing here, not decoration: this runs from an
 * interrupt that must be able to fire while a flash erase has the XIP
 * window turned off. Anything it touches must be in RAM too -- set_data()
 * and IEC_ATN are static inline register accesses, so they compile in
 * here rather than calling out to flash. */
#ifndef CONFIG_LOADER_DREAMLOAD
IRAM_ATTR
IEC_ATN_HANDLER {
  /* Earliest possible signal to core 1 -- the mainline flag update below
     only happens on the next state-machine iteration, which is already
     too late if core 1 is mid-poll. */
  iec_bus_busy = true;

  /* If PIO transmit is mid-byte it owns CLOCK and DATA, and set_data(0)
     below -- a gpio_set_dir() -- would write SIO's register while the pin
     is wired to PIO, doing precisely nothing. Take the pins back first.

     This is the T_AT path, so it is two register stores and no function
     calls, and it costs nothing at all when PIO transmit is off or idle:
     iec_pio_pins_held is a single byte and the whole thing compiles to a
     load, a branch, and out. It is placed BEFORE the IEC_ATN test on
     purpose -- reclaiming the pins is right whether or not we then pull
     DATA, and doing it first keeps the ordering obvious. */
  iec_pio_atn_reclaim();

  if (!IEC_ATN) {
    set_data(0);
  }
}
#endif

/* ------------------------------------------------------------------------- */
/*  Byte transfer routines                                                   */
/* ------------------------------------------------------------------------- */

/**
 * _iec_getc - receive one byte from the CBM serial bus (E9C9)
 *
 * This function tries receives one byte from the serial bus and returns it
 * if successful. Returns -1 instead if the device state has changed, the
 * caller should return to the main loop immediately in that case.
 */
IRAM_ATTR
static int16_t _iec_getc(void) {
  uint8_t i,val;
  iec_bus_t tmp;

  val = 0;

  iec_putc_loop_note(GETC_LOOP_CLOCKWAIT);
  do {                                                 // E9CD-E9D5
    if (iec_check_atn()) return -1;
  } while (!(iec_debounced() & IEC_BIT_CLOCK));

  set_data(1);                                         // E9D7
  /* Wait until all other devices released the data line. No iec_check_atn()
     here in the original routine either -- unlike every other wait in
     this function, ATN cannot break out of this one. Marked separately
     (GETC_LOOP_DATAWAIT) because a stall here can only ever be ended by
     the watchdog. */
  iec_putc_loop_note(GETC_LOOP_DATAWAIT);
  while (!IEC_DATA) ;                                  // FF20

  /* Timer for EOI detection */
  start_timeout(256);

  do {
    if (iec_check_atn()) return -1;                    // E9DF
    tmp = has_timed_out();                             // E9EE
  } while ((iec_debounced() & IEC_BIT_CLOCK) && !tmp);

  /* See if timeout happened -> EOI */
  if (tmp) {
    set_data(0);                                       // E9F2
    /* T_EI, the EOI acknowledge hold. Was 73us, the time the 1541 ROM's
       own instruction sequence takes between these two IO accesses
       (E9F5-E9F8), which is what upstream sd2iec reproduces.

       The published table (from the 1541-III docs) gives
       T_EI a minimum of 60us, but note 5 raises that to 80us specifically
       "for an external device to be a listener" -- which is exactly this
       code's situation. The note exists because a third-party device is
       not a 1541: different drive strength, rise times and cabling. So the
       ROM's 73us is evidence about a 1541, not about this board.

       Raised to 80us on 2026-09-05. Safe in one direction only, which is
       why it is worth doing without a failing test to point at: T_EI has
       NO specified maximum, and the listener hold-off T_H is 0..infinite,
       so a longer hold cannot overshoot anything -- it only delays the
       talker's next CLOCK by the difference, once per transfer. */
    iec_bus_delay_us(80);
    set_data(1);

    iec_putc_loop_note(GETC_LOOP_EOIACK);
    do {
      if (iec_check_atn())                             // E9FD
        return -1;
    } while (iec_debounced() & IEC_BIT_CLOCK);

    iec_data.iecflags|=EOI_RECVD;                      // EA07
  }

  /* Shadow the eight-bit loop with the PIO receive state machine.

     Armed here, with CLOCK already low (the EOI check above exits on
     CLOCK falling), so the state machine's first rising edge is bit 0 and
     the two decoders start on the same bit.

     Only for data bytes. Command bytes take the JiffyDOS branch below,
     whose extra handshake inserts a DATA pulse the PIO program knows
     nothing about; shadowing those would produce disagreements that mean
     nothing. See iec_pio.h. */
  const uint8_t pio_shadow = (iec_data.bus_state != BUS_ATNACTIVE);
  if (pio_shadow)
    iec_pio_rx_arm();

  if (pio_shadow && iec_pio_rx_owns && iec_pio_available()) {
    /* PIO decodes the byte; this loop only watches for ATN.

       The wait is unbounded on purpose, exactly as the software loop it
       replaces is: the eight `while (!(tmp & IEC_BIT_CLOCK))` spins below
       have no timeout either, and iec_check_atn() is what gets us out of
       both. Adding a timeout here and not there would change behaviour
       under the guise of moving it. */
    iec_putc_loop_note(GETC_LOOP_PIOWAIT);
    while (!iec_pio_rx_ready()) {
      if (iec_check_atn())
        return -1;
    }
    val = iec_pio_rx_get();

    /* The byte is in the FIFO as soon as the eighth bit is sampled, which
       is one edge EARLIER than the software path finishes: that path ends
       each bit by waiting for CLOCK to fall, so after bit 8 it has already
       seen the falling edge before it acknowledges. Acknowledging here
       without waiting would pull DATA while CLOCK is still high -- a byte
       ahead of the talker, and a protocol error rather than an
       optimisation. So wait for the same edge the software path waits
       for. */
    iec_putc_loop_note(GETC_LOOP_PIOPOSTBYTE);
    do {
      if (iec_check_atn()) return -1;
    } while (iec_debounced() & IEC_BIT_CLOCK);

    goto byte_done;
  }

  for (i=0;i<8;i++) {
    /* Check for JiffyDOS                                       */
    /*   Source: http://home.arcor.de/jochen.adler/ajnjil-t.htm */
    if (iec_data.bus_state == BUS_ATNACTIVE && i == 7) {
      start_timeout(218);

      /* No GETC_LOOP_BITWAIT_HIGH marker here deliberately: this branch
         is command-byte-only (BUS_ATNACTIVE), a handful of bytes at
         most, and has_timed_out() only flags a JiffyDOS delay -- it does
         not bound the loop (the while condition below ignores tmp), so
         a stall here is exactly as unbounded as the general case. Left
         unmarked rather than adding an eighth value; if this specific
         branch turns out to matter it gets its own marker then. */
      do {
        do {
          tmp = iec_bus_read();

          /* If there is a delay before the last bit, the controller uses JiffyDOS */
          if (!(iec_data.iecflags & JIFFY_ACTIVE) && has_timed_out()) {
            if ((val>>1) < 0x60 && ((val>>1) & 0x1f) == device_address) {
              /* If it's for us, notify controller that we support Jiffy too */
              set_data(0);
              iec_bus_delay_us(101); // nlq says 405us, but the code shows only 101
              set_data(1);
              iec_data.iecflags |= JIFFY_ACTIVE;
            }
          }
        } while (!(tmp & IEC_BIT_CLOCK));
      } while (!iec_clock_high_confirmed());
    } else {
      /* Capture data on rising edge. No iec_check_atn() here, same as
         GETC_LOOP_DATAWAIT above -- only CLOCK rising ends this wait. */
      iec_putc_loop_note(GETC_LOOP_BITWAIT_HIGH);
      do {
        do {                                           // EA0B
          tmp = iec_bus_read();
        } while (!(tmp & IEC_BIT_CLOCK));
      } while (!iec_clock_high_confirmed());
    }

    val = (val >> 1) | (!!(tmp & IEC_BIT_DATA) << 7);  // EA18

    iec_putc_loop_note(GETC_LOOP_BITWAIT_LOW);
    do {                                               // EA1A
      if (iec_check_atn()) return -1;
    } while (iec_debounced() & IEC_BIT_CLOCK);
  }

  if (pio_shadow)
    iec_pio_rx_check(val);

byte_done:
  iec_putc_loop_note(PUTC_LOOP_IDLE);
  iec_bus_delay_us(5); // Test
  set_data(0);                                         // EA28
  iec_bus_delay_us(50);  /* Slow down a little bit, may or may not fix some problems */
  return val;
}

/**
 * iec_getc - wrapper around _iec_getc to disable interrupts
 *
 * This function wraps iec_getc to disable interrupts there and is completely
 * inlined by the compiler. It could be inlined in the C code too, but is kept
 * seperately for clarity.
 */
IRAM_ATTR
static int16_t iec_getc(void) {
  int16_t val;
  uint32_t t0 = time_us_32();

  /* Byte-level position for a receive-side hang, same reasoning as
     iec_putc()'s equivalent call below: iec_stat_rx_bytes only
     increments on success, so this is "how many bytes are confirmed
     received so far", written before the call that might not return.
     Shares the tx_position/PUTC_LOOP_IDLE field with the send side --
     see the GETC_LOOP_* comment in system.h for why. */
  iec_refill_scratch_note(0, iec_stat_rx_bytes, REFILL_PHASE_IDLE);

  /* Gap since the previous byte finished: this is where any filesystem
     work on the transaction path shows up, and it is invisible from
     inside _iec_getc() itself. */
  if (iec_stat_last_end) {
    uint32_t gap = t0 - iec_stat_last_end;
    if (gap > iec_stat_gap_max_us && gap < 1000000u)
      iec_stat_gap_max_us = gap;
  }

  ATOMIC_BLOCK( ATOMIC_FORCEON ) {
    val = _iec_getc();
  }

  uint32_t end = time_us_32();
  uint32_t dt  = end - t0;
  iec_stat_account(dt, &iec_stat_getc_max_us);
  if (val >= 0)
    iec_stat_rx_bytes++;
  else
    iec_putc_loop_note(GETC_LOOP_ABORTED);   /* not in a byte wait any more */
  iec_stat_last_end = end;

  return val;
}


/**
 * iec_putc - send a byte over the serial bus (E916)
 * @data    : byte to be sent
 * @with_eoi: Flags if the byte should be send with an EOI condition
 *
 * This function sends the byte data over the serial bus, optionally including
 * a marker for the EOI condition. Returns 0 normally or -1 if the bus state has
 * changed, the caller should return to the main loop in that case.
 */
IRAM_ATTR
static uint8_t _iec_putc(uint8_t data, const uint8_t with_eoi);

/* Timing wrapper, mirroring iec_getc()'s. iec_putc() has eight separate
   `return -1` exits for ATN; wrapping is the only way to time all of them
   without eight copies of the same three lines. */
IRAM_ATTR
static uint8_t iec_putc(uint8_t data, const uint8_t with_eoi) {
  uint32_t t0 = time_us_32();

  /* Gap since the previous byte finished -- mirrors iec_getc()'s, but
     for the send side, which never had this measured before. This is
     exactly where a refill's own overhead (not the read itself, already
     confirmed fast) would show up. */
  if (iec_stat_last_end) {
    uint32_t gap = t0 - iec_stat_last_end;
    if (gap > iec_stat_put_gap_max_us && gap < 1000000u) {
      iec_stat_put_gap_max_us = gap;
      iec_put_gap_scratch_note(gap);   /* survives a hang's reset -- system.c */
    }
  }

  /* Byte-level position, not just the once-per-chunk snapshot in
     iec_talk_handler() -- iec_stat_tx_bytes only increments below, on
     success, so this is "how many bytes are confirmed sent so far",
     written before the call that might not return. Phase IDLE is
     correct here: we're not inside refill()/find_buffer(), we're
     inside _iec_putc() itself (see PUTC_LOOP_* / putc_loop_prev_boot
     for which of *its* waits was running). */
  iec_refill_scratch_note(0, iec_stat_tx_bytes, REFILL_PHASE_IDLE);

  uint8_t  r  = _iec_putc(data, with_eoi);
  uint32_t end = time_us_32();
  uint32_t dt  = end - t0;

  iec_stat_account(dt, &iec_stat_putc_max_us);
  if (r == 0)
    iec_stat_tx_bytes++;
  else
    iec_putc_loop_note(PUTC_LOOP_ABORTED);   /* not in a byte wait any more */
  iec_stat_last_end = end;
  return r;
}

IRAM_ATTR
static uint8_t _iec_putc(uint8_t data, const uint8_t with_eoi) {
  uint8_t i;

  if (iec_check_atn()) return -1;                      // E916

  if (iec_data.iecflags & JIFFY_ACTIVE) {
    /* This is the non-load Jiffy case */
    if (jiffy_send(data, with_eoi, 0)) {
      iec_check_atn();
      return -1;
    }
    return 0;
  }

  i = iec_debounced();

  iec_bus_delay_us(60); // Fudged delay
  set_clock(1);

  if (i & IEC_BIT_DATA) { // E923
    /* The 1571 jumps to E937 at this point, but I think            */
    /* this is not necessary - the following loop will fall through */
  }

  iec_putc_loop_note(PUTC_LOOP_E925);
  do {
    if (iec_check_atn()) return -1;                    // E925
  } while (!(iec_debounced() & IEC_BIT_DATA));

  if (with_eoi || (i & IEC_BIT_DATA)) {
    iec_putc_loop_note(PUTC_LOOP_E937);
    do {
      if (iec_check_atn()) return -1;                  // E937
    } while (!(iec_debounced() & IEC_BIT_DATA));

    iec_putc_loop_note(PUTC_LOOP_E941);
    do {
      if (iec_check_atn()) return -1;                  // E941
    } while (iec_debounced() & IEC_BIT_DATA);
  }

  set_clock(0);                                        // E94B
  iec_bus_delay_us(40); // estimated
  iec_putc_loop_note(PUTC_LOOP_PRECLOCK);
  do {
    if (iec_check_atn()) return -1;
  } while (!(iec_debounced() & IEC_BIT_DATA));
  iec_bus_delay_us(21); // calculated - E951 (best case after bus read) - E95B

  iec_putc_loop_note(PUTC_LOOP_TXBITS);
  if (iec_pio_tx_enabled()) {
    /* PIO drives the eight bits. Same sequence, same delays -- see
       iec_tx.pio, whose header carries the C loop this replaces. */
    int r = iec_pio_tx_byte(data);
    if (r == -1 || r == -3) {
      /* -1: listener dropped DATA mid-byte, exactly the case the
         software loop's first test below catches, same exit. -3
         (Phase D2, opt-in): the listener never signalled DONE at all --
         same class of failure, same recovery. */
      iec_data.bus_state = BUS_CLEANUP;
      return -1;
    }
    if (r == -2) {
      iec_check_atn();
      return -1;
    }
    goto tx_done;
  }

  for (i=0;i<8;i++) {
    if (!(iec_debounced() & IEC_BIT_DATA)) { // E95C
      iec_data.bus_state = BUS_CLEANUP;
      return -1;
    }
    iec_bus_delay_us(45);     // calculated

    set_data(data & 1<<i);
    iec_bus_delay_us(22);     // calculated
    set_clock(1);
    if (globalflags & VC20MODE)
      iec_bus_delay_us(34);   // Calculated delay
    else
      iec_bus_delay_us(75);   // Calculated delay

    set_clock(0);     // FEFB
    iec_bus_delay_us(22);     // calculated
    set_data(1);      // FEFE
    iec_bus_delay_us(14);     // Settle time, approximate
  }

tx_done:
  iec_putc_loop_note(PUTC_LOOP_TXDONE);
  /* Phase D2 (2026-09-07): this wait directly
     caused a real hang -- the listener never released DATA, and PRIFLOP
     waited forever (T_H is spec-legal 0..infinity, but only if the
     *other* side is also willing to wait). Opt-in timeout, same flag as
     iec_pio_tx_byte()'s equivalent wait (iec_pio.c) -- see the comment
     on iec_tx_timeout_enabled in iec_pio.h. time_us_32() directly, not
     start_timeout()/has_timed_out(), which are flash-resident and this
     function is not. */
  {
    uint32_t deadline = iec_tx_timeout_enabled
                       ? time_us_32() + iec_tx_timeout_us : 0;
    uint32_t pet = time_us_32();
    bool long_wait = false;
    do {
      if (iec_check_atn()) return -1;
      if (iec_tx_timeout_enabled &&
          (int32_t)(time_us_32() - deadline) >= 0) {
        iec_tx_timeouts++;
        iec_data.bus_state = BUS_CLEANUP;
        return -1;
      }
      if (time_us_32() - pet >= IEC_TX_PET_US) {   /* iec_pio.h, decision 11b */
        if (!long_wait) { long_wait = true; iec_tx_long_waits++; }
        system_watchdog_pet();
        pet = time_us_32();
      }
    } while (iec_debounced() & IEC_BIT_DATA);
  }

  /* More stuff that's not in the original rom:
   *   Wait for 250us or until DATA is high or ATN is low.
   * This fixes a problem with Castle Wolfenstein.
   * Bus traces seem to indicate that a real 1541 needs
   * about 350us between two bytes, sd2iec is usually WAY faster.
   */
  start_timeout(250);
  while (!IEC_DATA && IEC_ATN && !has_timed_out()) ;

  iec_putc_loop_note(PUTC_LOOP_IDLE);
  return 0;
}


/* ------------------------------------------------------------------------- */
/*  Listen+Talk-Handling                                                     */
/* ------------------------------------------------------------------------- */

/**
 * iec_listen_handler - handle an incoming LISTEN request (EA2E)
 * @cmd: command byte received from the bus
 *
 * This function handles a listen request from the computer.
 */
IRAM_ATTR
static uint8_t iec_listen_handler(const uint8_t cmd) {
  int16_t c;
  buffer_t *buf;


  buf = find_buffer(cmd & 0x0f);

  /* Abort if there is no buffer or it's not open for writing */
  /* and it isn't an OPEN command                             */
  if ((buf == NULL || !buf->write) && (cmd & 0xf0) != 0xf0) {
    iec_data.bus_state = BUS_CLEANUP;
    return 1;
  }

  while (1) {
    if (iec_data.iecflags & JIFFY_ACTIVE) {
      iec_bus_t flags;
      set_atn_irq(1);
      uint32_t jf_t0 = time_us_32();
      c = jiffy_receive(&flags);
      iec_stat_jf_us += time_us_32() - jf_t0;
      iec_stat_jf_rx++;
      if (!(flags & IEC_BIT_ATN))
        /* ATN was active at the end of the transfer */
        c = iec_getc();
      else
        if(flags & IEC_BIT_CLOCK)
          iec_data.iecflags |= EOI_RECVD;
        else
          iec_data.iecflags &= (uint8_t)~EOI_RECVD;
    } else {
      if (iec_data.iecflags & DOLPHIN_ACTIVE)
        c = dolphin_getc();
      else
        c = iec_getc();
    }
    if (c < 0) return 1;

    if ((cmd & 0x0f) == 0x0f || (cmd & 0xf0) == 0xf0) {
      if (command_length < CONFIG_COMMAND_BUFFER_SIZE)
        command_buffer[command_length++] = c;
      if (iec_data.iecflags & EOI_RECVD)
        // Filenames are just a special type of command =)
        iec_data.iecflags |= COMMAND_RECVD;
    } else {
      /* Flush buffer if full */
      if (buf->mustflush) {
        if (buf->refill(buf))
          return 1;
        /* Symmetric with iec_talk_handler(): a flush is a chunk (up to
           254 bytes) of confirmed progress, so the watchdog must see
           it -- see the comment there for why this loop needs its own
           pet at all. Per *chunk* rather than per byte (2026-09-10):
           one chunk at measured bus speeds is a small fraction of even
           the original 8s timeout, so this is enough margin to never
           mistake a long SAVE for a hang, while a byte-level stall
           inside _iec_getc() still gets nothing here and still times
           out correctly -- same worst-case detection latency as the
           per-byte version. */
        system_watchdog_pet();
        /* Search the buffer again, it can change when using large buffers. */
        buf = find_buffer(cmd & 0x0f);
      }

      buf->data[buf->position] = c;
      mark_buffer_dirty(buf);

      if (buf->lastused < buf->position)
        buf->lastused = buf->position;
      buf->position++;

      /* Mark buffer for flushing if position wrapped */
      if (buf->position == 0)
        buf->mustflush = 1;

      /* REL files must be syncronized on EOI */
      if(buf->recordlen && (iec_data.iecflags & EOI_RECVD)) {
        if (buf->refill(buf))
          return 1;
        system_watchdog_pet();
      }
    }
  }
}

/**
 * iec_talk_handler - handle an incoming TALK request (E909)
 * @cmd: command byte received from the bus
 *
 * This function handles a talk request from the computer.
 */
/* Default no-op: most arches have no networking and nothing to poll here.
   __attribute__((weak)) lets src/pico/network.c override it with a real
   cyw43_arch_poll() call (see system.h). Called only from BUS_IDLE below,
   the same safe point disk_idle_sync() uses. */
void network_poll(void) __attribute__((weak));
void network_poll(void) {
}

/* Default no-op: most arches have no second core and nothing to bridge
   FatFs requests from. __attribute__((weak)) lets src/pico/fs_bridge.c
   override it with the real implementation (see system.h and
   fs_bridge.h) -- core 1's httpd.c never touches FatFs directly, this
   is what services its requests. Called only from BUS_IDLE below, the
   same safe point disk_idle_sync() uses. */
void fs_bridge_poll(void) __attribute__((weak));
void fs_bridge_poll(void) {
}

IRAM_ATTR
static uint8_t iec_talk_handler(uint8_t cmd) {
  buffer_t *buf;


  buf = find_buffer(cmd & 0x0f);
  if (buf == NULL)
    return 0; /* 0 because we didn't change the state here */

  if (iec_data.iecflags & JIFFY_ACTIVE)
    /* wait 360us (J1541 E781) to make sure the C64 is at fbb7/fb0c */
    iec_bus_delay_us(360);

  if (iec_data.iecflags & JIFFY_LOAD) {
    /* See if the C64 has passed fb06 or if we should abort */
    do {                /* J1541 FF30 - wait until DATA inactive/high */
      if (iec_check_atn()) return -1;
    } while (!IEC_DATA);
    /* The LOAD path is only used after the first two bytes have been */
    /* read. Reset the buffer position because there is a chance that */
    /* the third byte has slipped through.                            */
    buf->position = 4;

    /* Ready-signal for the first block */
    set_data(0);
    set_clock(1);
    /* FFA0 - this delay is required so the C64 can see data low even */
    /*        if it hits a badline at the worst possible moment       */
    iec_bus_delay_us(50);
  }

  while (buf->read) {
    do {
      uint8_t finalbyte = (buf->position == buf->lastused);
      if (iec_data.iecflags & JIFFY_LOAD) {
        /* Send a byte using the LOAD protocol variant */
        /* The final byte in the buffer must be sent with Clock low   */
        /* to signal that the next transfer will take some time.      */
        /* The C64 samples this just after it has set Data Low before */
        /* the first bitpair. If this marker is not set the time      */
        /* between two bytes outside the assembler function must not  */
        /* exceed ~38 C64 cycles (estimated) or the computer may      */
        /* see a previous data bit as the marker.                     */
        {
          uint32_t jf_t0 = time_us_32();
          uint8_t  jf_rc = jiffy_send(buf->data[buf->position],
                                      0, 128 | !finalbyte);
          iec_stat_jf_us += time_us_32() - jf_t0;
          iec_stat_jf_tx++;
          if (jf_rc) {
            /* Abort if ATN was seen */
            iec_check_atn();
            return -1;
          }
        }

        if (finalbyte && buf->sendeoi) {
          /* Send EOI marker */
          iec_bus_delay_us(100);
          set_clock(1);
          iec_bus_delay_us(100);
          set_clock(0);
          iec_bus_delay_us(100);
          set_clock(1);
        }
      } else {
        uint8_t res;

        if (finalbyte && buf->sendeoi) {
          /* Send with EOI */
          if (iec_data.iecflags & DOLPHIN_ACTIVE)
            res = dolphin_putc(buf->data[buf->position], 1);
          else
            res = iec_putc(buf->data[buf->position], 1);

          if (iec_data.iecflags & JIFFY_ACTIVE) {
            /* Jiffy resets the EOI condition on the bus after 30-40us. */
            /* We use 50 to play it safe.                               */
            iec_bus_delay_us(50);
            set_data(1);
            set_clock(0);
          }
          if (res) {
            return 1;
          }
        } else {
          /* Send without EOI */
          if (iec_data.iecflags & DOLPHIN_ACTIVE)
            res = dolphin_putc(buf->data[buf->position], 0);
          else
            res = iec_putc(buf->data[buf->position], 0);

          if (res) {
            return 1;
          }
        }
      }

    } while (buf->position++ < buf->lastused);

    /* A whole chunk (up to 254 bytes) has just been sent -- confirmed
       progress, and the watchdog must see it. Without this, the 8 s
       timeout measures the wall-clock length of the whole transfer
       rather than liveness: this handler owns the bus until the last
       byte is sent and never returns to iec_mainloop()'s pet, and a
       sequential littlefs read hits none of the other pet sites (no
       flash write, no erase, not an image op). That reset a healthy
       drive mid-LOAD at ~8 s x ~520 B/s ~= 4.1 KB and was the entire
       ">4KB LOAD hang".

       Per *chunk* rather than per byte (2026-09-10 test): at measured
       bus speeds one chunk is ~0.5s, a small fraction of even the
       original 8s timeout, so this still keeps the watchdog fed with a
       wide margin during a healthy transfer of any length. A
       byte-level stall inside iec_putc() (a listener that never
       releases DATA) used to get no pet and reset the drive; since
       decision 11b (2026-10-15) iec_putc()'s own waits feed the
       watchdog once a second and wait for ATN instead (iec_pio.h,
       IEC_TX_PET_US), counted in tx_long_waits. */
    system_watchdog_pet();

    if (buf->sendeoi &&
        (cmd & 0x0f) != 0x0f &&
        !buf->recordlen &&
        buf->refill != directbuffer_refill) {
      buf->read = 0;
      break;
    }

    {
      /* Set *before* the call, not after -- a duration can only be
         recorded once its call returns, so if buf->refill() itself is
         what hangs, this is the only trace of it that ever reaches the
         next boot. See system.c's iec_refill_scratch_note() comment. */
      iec_refill_scratch_note(0, iec_stat_tx_bytes, REFILL_PHASE_REFILL);
      uint32_t refill_t0 = time_us_32();
      uint8_t  refill_failed = buf->refill(buf);
      uint32_t refill_dt = time_us_32() - refill_t0;
      if (refill_dt > iec_stat_refill_max_us) iec_stat_refill_max_us = refill_dt;
      iec_refill_scratch_note(refill_dt, iec_stat_tx_bytes,
        refill_failed ? REFILL_PHASE_IDLE : REFILL_PHASE_FINDBUF);

      if (refill_failed) {
        iec_data.bus_state = BUS_CLEANUP;
        return 1;
      }
    }

    /* Search the buffer again, it can change when using large buffers */
    {
      uint32_t fb_t0 = time_us_32();
      buf = find_buffer(cmd & 0x0f);
      uint32_t fb_dt = time_us_32() - fb_t0;
      if (fb_dt > iec_stat_findbuf_max_us) iec_stat_findbuf_max_us = fb_dt;
      iec_refill_scratch_note(0, iec_stat_tx_bytes, REFILL_PHASE_IDLE);
    }

    if (iec_data.iecflags & JIFFY_LOAD) {
      /* wait until the C64 is at FB06, use timeout in case the STOP key is pressed */
      start_timeout(120);
      while (!IEC_DATA && !has_timed_out()) ;

      /* check if ATN changed */
      if (iec_check_atn())
        return -1;

      /* Signal to the C64 that we're ready to send the next block */
      set_data(0);
      set_clock(1);

      /* FFA0 - this delay is required so the C64 can see data low even */
      /*        if it hits a badline at the worst possible moment       */
      iec_bus_delay_us(50);
    }
  }

  return 0;
}



/* ------------------------------------------------------------------------- */
/*  Initialization and main loop                                             */
/* ------------------------------------------------------------------------- */


void iec_init(void) {
  /* Keep DATA low if there is already a request on the bus */
  if (!IEC_ATN)
    set_data(0);

  /* Prepare IEC interrupts */
  iec_interrupts_init();

  /* Read the hardware-set device address */
  device_hw_address_init();
  delay_ms(1);
  device_address = device_hw_address();

  /* PIO receive, currently shadowing the software path rather than
     replacing it (iec_pio.h). Failure is not fatal: if no state machine
     can be claimed, every iec_pio_* call becomes a no-op and the bus
     behaves exactly as it did before. */
  iec_pio_init();
  iec_pio_tx_init();
  primo_turbo_init();   /* the Primo turbo's state machine, see primo_turbo.h */
  llfl_init();          /* fastloader timebase, see llfl-common.c */
}
void bus_init(void) __attribute__((weak, alias("iec_init")));

/* How long the bus must stay continuously idle (ATN inactive) before
   BUS_IDLE's disk_idle_sync() call is allowed to fire -- see that
   case's own comment for why an
   immediate-on-entry commit isn't safe: a single kernal operation like
   SAVE spans several back-to-back ATN transactions (OPEN, then DATA,
   then CLOSE) with only a brief gap between them, and a ~43ms
   interrupts-disabled commit landing in that gap makes us unresponsive
   for long enough that the kernal abandons the next transaction
   entirely. 50ms is comfortably longer than that gap (confirmed via
   real-hardware testing) while being imperceptible as an extra delay
   before a write becomes durable. */
#define BUS_IDLE_COMMIT_DEBOUNCE_US 50000u

void iec_mainloop(void) {
  /* The moment the drive starts answering the C64. Logged because the
     interesting question is not when the banner printed but when the bus
     became usable relative to the Wi-Fi join -- they run on different
     cores and the answer decides whether a slow join delays the drive at
     all. */
  uart_puts_P(PSTR("IEC: bus mainloop running\r\n"));

  int16_t cmd = 0; // make gcc happy...

  set_error(ERROR_DOSVERSION);

  iec_data.bus_state = BUS_IDLE;
  iec_flash_hold_ready();
  system_watchdog_start();

  while (1) {
    /* Gate for core 1 (see the iec_bus_busy comment above). BUS_IDLE is
       the only state where it is safe for the other core to touch flash;
       that includes BUS_IDLE's own ATN wait loop, which is where this
       mainloop spends nearly all of its time. */
    iec_bus_busy = (iec_data.bus_state != BUS_IDLE);
    system_watchdog_pet();   /* progress: see system.c */


    switch (iec_data.bus_state) {
    case BUS_SLEEP:
      set_atn_irq(0);
      set_data(1);
      set_clock(1);
      set_error(ERROR_OK);
      set_busy_led(0);
      set_dirty_led(1);

      /* Wait until the sleep key is used again */
      while (!key_pressed(KEY_SLEEP)) {
        system_sleep();
      }
      reset_key(KEY_SLEEP);

      update_leds();

      iec_data.bus_state = BUS_IDLE;
      break;

    case BUS_IDLE: {

      /* No transaction in progress here -- safe point for an arch to
         flush any writes it deferred to avoid stalling a live
         transaction (see diskio.h). Default is a no-op for arches that
         don't need it. Added 2026-08-22.
         

         DEBOUNCED, not called immediately -- found 2026-09-01 via a direct A/B test: a SAVE is not
         one ATN transaction, it's several in quick succession (OPEN,
         then a separate DATA transaction, then CLOSE), and this
         ~43ms-interrupts-disabled commit was correctly deferred past
         the *first* of those (OPEN) but then fired immediately on
         re-entering BUS_IDLE in the brief gap before the kernal's very
         next transaction (DATA). During that ~43ms window we can't see
         or respond to the C64 reasserting ATN at all -- confirmed by
         disabling this call entirely and watching the previously-never-
         seen DATA transaction (and correct byte count) finally appear.
         The fix keeps eventual durability but only commits once the bus
         has been *continuously* idle for a short debounce period,
         comfortably longer than the gap between a kernal's own
         back-to-back transactions but imperceptible to a human at the
         keyboard. If ATN goes active before the debounce elapses, the
         commit is skipped for this idle period entirely and simply
         retried the next time the bus goes idle -- data already in the
         write-back cache isn't lost, just not committed to flash yet.

         RE-ARMED, not one-shot (2026-09-02): with no
         real C64 attached, ATN never goes active, so this case is
         entered exactly once and then the while(IEC_ATN) loop below
         never exits -- a one-shot "commit once, ever" flag meant any
         write made more than one debounce period after boot (e.g. any
         realistic HTTP upload through httpd.c) sat in the cache forever
         and was lost on the next reset/power-cycle. Restarting the
         timeout after every commit instead keeps checking every
         BUS_IDLE_COMMIT_DEBOUNCE_US for as long as the bus stays idle --
         free when nothing is dirty (commit_slot() just checks a flag per
         slot), and no different from a real C64's perspective since
         ATN going active still exits this loop immediately regardless
         of where the timer is. */
      start_timeout(BUS_IDLE_COMMIT_DEBOUNCE_US);
bootlog_poll_console();   /* BUS_IDLE only: touches stdio */
      network_poll();
      fs_bridge_poll();

      apply_web_settings();

      /* Wait for ATN */
      parallel_set_dir(PARALLEL_DIR_IN);
      set_atn_irq(1);
      while (IEC_ATN) {
        /* Re-clear the core-1 gate here, not just at the top of the outer
           loop. ATN is released for as long as this loop runs, so the bus
           is idle by definition -- but the ATN ISR sets iec_bus_busy on
           every edge, and this loop is where core 0 spends nearly all of
           its time. Without this, a single stray edge latches the flag
           true forever and core 1 spins in core1_wait_for_bus_idle()
           instead of ever calling httpd_poll() again: the web UI accepts
           a request, core 0 completes it, and the reply is never sent.
           Observed exactly that as a Wi-Fi "Saving..." that never
           returned, 2026-09-04.

           A real ATN assertion still wins: it ends this loop on the very
           next test, and the outer loop immediately re-raises the flag
           for BUS_FOUNDATN. */
        iec_bus_busy = false;

        /* Here too, not only on entry: a quiet bus never leaves this loop,
           so a change made from the web UI would wait for the C64's next
           transaction (found 2026-10-02 -- see apply_web_settings()). */
        apply_web_settings();

        if (key_pressed(KEY_NEXT | KEY_PREV | KEY_HOME)) {
          change_disk();
        } else if (key_pressed(KEY_SLEEP)) {
          reset_key(KEY_SLEEP);
          iec_data.bus_state = BUS_SLEEP;
          break;
        } else if (display_found && key_pressed(KEY_DISPLAY)) {
          display_service();
          reset_key(KEY_DISPLAY);
        }
        /* network_poll() is called every iteration, not just once on
           entry to BUS_IDLE -- the C64 typically sits at READY (ATN
           inactive) far longer than any single bus transaction takes,
           and this loop is where that time is actually spent; polling
           only once before the loop would leave incoming HTTP
           connections unserviced almost all the time. Found and fixed
           while adding the web file-manager, 2026-08-23.

           disk_idle_sync(), by contrast, only fires once the debounce
           timer has elapsed, then re-arms itself -- see the comment
           above this case for why immediate-on-entry (like
           network_poll()) was wrong for this specific call, and why a
           one-shot-per-entry flag was also wrong. */
        if (has_timed_out()) {
          disk_idle_sync();
          /* Same debounce, same reason: lfs_fs_size() walks the whole
             filesystem metadata to answer BLOCKS FREE, and dir_footer()
             used to call it from inside a live transaction. It runs here
             instead, and only when a write has marked the count stale. */
          lfsops_idle_maintenance();
          start_timeout(BUS_IDLE_COMMIT_DEBOUNCE_US);
        }
        network_poll();
        fs_bridge_poll();
        system_watchdog_pet();
        system_sleep();
      }

      if (iec_data.bus_state != BUS_SLEEP)
        iec_data.bus_state = BUS_FOUNDATN;
      break;
    }

    case BUS_FOUNDATN: // E85B
      /* Pull data low to say we're here */
      set_clock(1);
      set_data(0);
      set_atn_irq(0);

      iec_data.device_state = DEVICE_IDLE;
      iec_data.bus_state    = BUS_ATNACTIVE;
      iec_data.iecflags &= (uint8_t)~(EOI_RECVD | JIFFY_ACTIVE | JIFFY_LOAD);

      /* Slight protocol violation:                        */
      /*   Wait until clock is low or 250us have passed    */
      /*   The C64 doesn't always pull down the clock line */
      /*   before ATN, this loop should keep us in sync.   */

      start_timeout(250);
      while (IEC_CLOCK && !has_timed_out())
        if (IEC_ATN)
          iec_data.bus_state = BUS_ATNPROCESS;

      /* This second wait had no timeout at all until 2026-09-02: with no real C64 attached to ever
         release/assert the clock line, it span forever, with zero
         recovery path -- and since network_poll() (WiFi/lwIP/HTTP) is
         only ever called from BUS_IDLE, that permanently starved
         networking the moment the bus state machine wandered in here
         (e.g. from a floating-pin false ATN trigger before this file's
         IEC pull-up was restored, or simply from being tested standalone
         with no C64 powered at all). A generous 1-second timeout costs
         nothing against a real C64 -- an actual bus handshake resolves
         in low milliseconds, nowhere close to this -- but bounds the
         worst case when nothing real is on the bus at all. */
      start_timeout(1000000);
      /* Service core 1 while waiting this out. The timeout above bounds
         the "nothing real on the bus" case, but it does not stop that
         case from starving the network: a C64 that is switched off holds
         ATN low through its own drivers, so BUS_IDLE's "wait for ATN"
         loop exits immediately, we land here, spin for a second, and go
         back -- forever, at about 1Hz.

         fs_bridge_poll() then runs once per second, which is once per
         bridge round trip. Measured 2026-09-05: a directory listing took
         1.00s, opening and reading a file 4.00s, and a 170KB disk image
         downloaded at 1KB/s -- three minutes, all of it this.

         Only after 50ms, and only then: a genuine handshake resolves in
         low milliseconds (see the comment above), so past that point
         nothing real is in progress and it is safe both to drop the
         core-1 gate and to let core 0 do filesystem work. Before 50ms
         this stays exactly as it was, so a real transaction is untouched. */
      {
        uint32_t fa_start = time_us_32();
        while (!IEC_CLOCK && !has_timed_out()) {
          if (IEC_ATN)
            iec_data.bus_state = BUS_ATNPROCESS;
          if (time_us_32() - fa_start > 50000u) {
            iec_bus_busy = false;
            network_poll();
            fs_bridge_poll();
          }
        }
      }

      if (!IEC_CLOCK && iec_data.bus_state == BUS_ATNACTIVE) {
        /* Timed out: clock was never released and no real ATN event
           redirected us either -- nobody real responded on the bus at
           all. Bail back to BUS_IDLE (where WiFi/HTTP/flash-idle-sync
           actually run) rather than hang here indefinitely; a real C64,
           if/when attached, will simply start a fresh BUS_FOUNDATN entry
           via ATN the normal way. */
        iec_data.bus_state = BUS_IDLE;
      }

      break;

    case BUS_ATNACTIVE: // E884
      cmd = iec_getc();

      if (cmd < 0) {
        /* iec_check_atn changed our state */
        break;
      }
      iec_cmd_ring[iec_cmd_head++ & 7] = (uint8_t)cmd;   /* see iec_bail_cmds */


      if (cmd == 0x3f) { /* Unlisten */
        if (iec_data.device_state == DEVICE_LISTEN)
          iec_data.device_state = DEVICE_IDLE;
        iec_data.bus_state = BUS_ATNFINISH;
      } else if (cmd == 0x5f) { /* Untalk */
        if (iec_data.device_state == DEVICE_TALK)
          iec_data.device_state = DEVICE_IDLE;
        iec_data.bus_state = BUS_ATNFINISH;
      } else if (cmd == 0x40+device_address) { /* Talk */
        iec_data.device_state = DEVICE_TALK;
        iec_data.bus_state = BUS_FORME;
        iec_ram_drive_active = 0;
      } else if (cmd == 0x20+device_address) { /* Listen */
        iec_data.device_state = DEVICE_LISTEN;
        iec_data.bus_state = BUS_FORME;
        iec_ram_drive_active = 0;
      } else if (cmd == 0x40+device_address+1) { /* Talk, drive 9 (ramops.c) */
        iec_data.device_state = DEVICE_TALK;
        iec_data.bus_state = BUS_FORME;
        iec_ram_drive_active = 1;
      } else if (cmd == 0x20+device_address+1) { /* Listen, drive 9 (ramops.c) */
        iec_data.device_state = DEVICE_LISTEN;
        iec_data.bus_state = BUS_FORME;
        iec_ram_drive_active = 1;
      } else if ((cmd & 0x60) == 0x60) {
        /* Check for OPEN/CLOSE/DATA */
        /* JiffyDOS uses a slightly modified protocol for LOAD that */
        /* is activated by using 0x61 instead of 0x60 in the TALK   */
        /* state. The original floppy code has additional checks    */
        /* that force the non-load Jiffy protocol for file types    */
        /* other than SEQ and PRG.                                  */
        /* Please note that $ is special-cased in the kernal so it  */
        /* will never trigger this.                                 */
        if (cmd == 0x61 && iec_data.device_state == DEVICE_TALK) {
          cmd = 0x60;
          iec_data.iecflags |= JIFFY_LOAD;
        }

        iec_data.secondary_address = cmd & 0x0f;
        /* 1571 handles close (0xe0-0xef) here, so we do that too. */
        if ((cmd & 0xf0) == 0xe0) {
          if (cmd == 0xef) {
            /* Close all buffers if sec. 15 is closed */
            if (free_multiple_buffers(FMB_USER_CLEAN)) {
              /* The 1571 error generator/handler always jumps to BUS_CLEANUP */
              iec_data.bus_state = BUS_CLEANUP;
              break;
            }
          } else {
            /* Close a single buffer */
            buffer_t *buf;
            buf = find_buffer(iec_data.secondary_address);
            if (buf != NULL) {
              if (buf->cleanup(buf)) {
                free_buffer(buf);
                iec_data.bus_state = BUS_CLEANUP;
                break;
              }
              /* Free the buffer */
              free_buffer(buf);
            }
          }
          iec_data.bus_state = BUS_FORME;
        } else {
          iec_data.bus_state = BUS_ATNFINISH;
        }
      } else {
        // Not me
        iec_data.bus_state = BUS_NOTFORME;
      }
      break;

    case BUS_FORME: // E8D2
      if (!IEC_ATN)
        iec_data.bus_state = BUS_ATNACTIVE;
      else
        iec_data.bus_state = BUS_ATNPROCESS;
      break;

    case BUS_NOTFORME: // E8FD
      set_atn_irq(0);
      set_clock(1);
      set_data(1);
      iec_data.bus_state = BUS_ATNFINISH;
      break;

    case BUS_ATNFINISH: // E902 + DolphinDOS A7CC
      iec_data.iecflags &= ~DOLPHIN_ACTIVE;
      parallel_clear_rxflag();

      /* Bounded, mirroring what de834c4 did for BUS_FOUNDATN's identical
         unconditional wait. Observed hanging here forever on real
         hardware (2026-09-04): after UNLISTEN the C64 kept ATN asserted
         while we held DATA low, and neither side moved -- trace showed
         "W010" repeating indefinitely, i.e. ATN=0/CLOCK=1/DATA=0 with no
         recovery path short of a power cycle. A real handshake resolves
         in low milliseconds, so 1s is far beyond any legitimate wait
         while still bounding the deadlock.

         The underlying cause was core 1 starving core 0 (see
         iec_bus_busy above); with that gated, this path no longer
         triggers -- a full LOAD"$",8 and a PRG load both complete with
         no bail. It is kept as a safety net so a missed handshake can
         never again wedge the drive until power-cycle, which is exactly
         what de834c4 concluded for the BUS_FOUNDATN wait. */
      start_timeout(1000000);
      while (!IEC_ATN && !has_timed_out()) {
        if (iec_data.device_state != DEVICE_IDLE &&
            parallel_rxflag) {
          /* parallel byte received */
          parallel_clear_rxflag();
          iec_data.iecflags |= DOLPHIN_ACTIVE;
          parallel_send_handshake();

          if (iec_data.device_state == DEVICE_TALK)
            parallel_set_dir(PARALLEL_DIR_OUT);
          else
            parallel_set_dir(PARALLEL_DIR_IN);
        }
      }

      if (!IEC_ATN) {
        /* Timed out with ATN still asserted. Falling through to
           BUS_ATNPROCESS here would just resume a transaction the
           controller has plainly abandoned, so release the lines we hold
           and go back to BUS_IDLE -- the same recovery shape as
           BUS_FOUNDATN's timeout bail. A real C64 restarts cleanly with a
           fresh ATN whenever it is ready. */

        /* Counted, and reported by /api/stats as "ATN bails".

           This bail is not supposed to happen. It was added after a real
           wedge (2026-09-04) whose cause -- core 1 starving core 0 -- was
           then fixed, so with the gate in place nothing reaches here: a
           full LOAD"$",8 and a PRG load both complete without it. That
           makes it a safety net that recovers *silently*, which is the
           problem. If the deadlock ever comes back, the drive would go on
           quietly bailing once a second and the only visible symptom
           would be that the C64 feels wrong.

           A non-zero count is therefore a real signal: the handshake
           failed and the net caught it. Zero, which is what it should
           read, is the evidence that the underlying fix still holds.
           Unlike BUS_FOUNDATN's timeout -- which fires about once a
           second whenever no C64 is powered on, and so would only be
           noise -- this one is meaningful whenever it moves at all. */
        iec_atnfinish_bails++;
        for (uint8_t k = 0; k < 8; k++)
          iec_bail_cmds[k] = iec_cmd_ring[(uint8_t)(iec_cmd_head + k) & 7];
        iec_bail_dev = (uint8_t)iec_data.device_state;
        set_clock(1);
        set_data(1);
        iec_data.device_state = DEVICE_IDLE;
        iec_data.bus_state = BUS_IDLE;
        break;
      }

      iec_data.bus_state = BUS_ATNPROCESS;
      break;

    case BUS_ATNPROCESS: // E8D7
      set_atn_irq(1);

      if (iec_data.device_state == DEVICE_LISTEN) {
        if (iec_listen_handler(cmd))
          break;
      } else if (iec_data.device_state == DEVICE_TALK) {
        set_data(1);
        delay_us(50);    // Implicit delay, fudged
        set_clock(0);
        delay_us(70);    // Implicit delay, estimated

        if (iec_talk_handler(cmd))
          break;

      }
      iec_data.bus_state = BUS_CLEANUP;
      break;

    case BUS_CLEANUP:
      set_atn_irq(1);
      // 836B
      set_clock(1);
      set_data(1);

      {

      //   0x255 -> A61C
      /* Handle commands and filenames */
      if (iec_data.iecflags & COMMAND_RECVD) {

#ifdef HAVE_HOTPLUG
        /* This seems to be a nice point to handle card changes */
        if (disk_state != DISK_OK) {
          set_busy_led(1);
          /* If the disk was changed the buffer contents are useless */
          if (disk_state == DISK_CHANGED || disk_state == DISK_REMOVED) {
            free_multiple_buffers(FMB_ALL);
            change_init();
            filesystem_init(0);
          } else
            /* Disk state indicated an error, try to recover by initialising */
            filesystem_init(1);

          update_leds();
        }
#endif

        if (iec_data.secondary_address == 0x0f) {
          /* Command channel */
          parse_doscommand();
        } else {
          /* Filename in command buffer */
          datacrc = 0xffff;
          file_open(iec_data.secondary_address);
        }
        command_length = 0;
        iec_data.iecflags &= (uint8_t)~COMMAND_RECVD;
      }

      /* We're done, clean up unused buffers */
      free_multiple_buffers(FMB_UNSTICKY);
      d64_bam_commit();

      /* A Primo that has read `00,TURBO` and sent UNTALK starts its turbo
         exchanges now; this returns when they are over (primo_turbo.h). */
      primo_turbo_poll((uint8_t)cmd);

      }

      iec_data.bus_state = BUS_IDLE;
      break;
    }
  }
}
void bus_mainloop(void) __attribute__ ((weak, alias("iec_mainloop")));
