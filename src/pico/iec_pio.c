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


   iec_pio.c: PIO assist for the IEC bus -- see iec_pio.h for status
   (shadow mode) for the design.
*/

#include <string.h>

#include "config.h"
#include "arch-config.h"
#include "flags.h"
#include "iec_pio.h"
void system_watchdog_pet(void);   /* src/pico/system.c; flash, called once a second at most */

#include "hardware/pio.h"
#include "hardware/structs/iobank0.h"
#include "hardware/timer.h" /* time_us_32() -- a bare timerawl register
                                read (see its own header comment), safe
                                to call from this file's RAM-resident
                                (__not_in_flash_func) functions unlike
                                start_timeout()/has_timed_out(), which
                                are themselves flash-resident. */
#include "iec_rx.pio.h"
#include "iec_tx.pio.h"

volatile uint32_t iec_pio_match;
volatile uint32_t iec_pio_mismatch;
volatile uint32_t iec_pio_miss;
volatile uint8_t  iec_pio_last_sw;
volatile uint8_t  iec_pio_last_pio;

/* Default for iec_pio_rx_owns. On, because shadow mode earned it: 8692
   data bytes decoded beside the software path with zero disagreements and
   zero misses (2026-09-05). Not on faith -- on a measurement that is
   still running, since the counters below keep working in either mode. */
#define IEC_PIO_RX_OWNS_DEFAULT 1
/* Transmit defaults ON as of 2026-09-06. It was off while unproven --
   unlike receive it cannot be shadow-tested, because two drivers cannot
   share a line, so the only test was to switch it on against a real C64.
   That test has now run: IECTEST passed with all 10835 transmitted bytes
   going through PIO, zero listener aborts, zero ATN-truncated bytes, zero
   faults, and atn_bails still 0 across 1030 ATN aborts -- which is the
   number that matters, because it says the T_AT path kept its deadline
   every time the ISR had to snatch CLOCK and DATA back from PIO
   mid-byte. POST /api/pio?tx=0 still returns the software loop. */
#define IEC_PIO_TX_OWNS_DEFAULT 1
volatile uint8_t iec_pio_rx_owns = IEC_PIO_RX_OWNS_DEFAULT;

static PIO  rx_pio;
static uint rx_sm;
static uint rx_offset;
static bool rx_ready;

volatile uint8_t  iec_pio_tx_owns = IEC_PIO_TX_OWNS_DEFAULT;
volatile uint8_t  iec_pio_pins_held;
volatile uint32_t iec_pio_tx_bytes;
volatile uint32_t iec_pio_tx_abort;
volatile uint32_t iec_pio_tx_atn;

/* Phase D2 -- see the comment on these in iec_pio.h. Default off/1s. */
volatile uint8_t  iec_tx_timeout_enabled = 0;
volatile uint32_t iec_tx_timeout_us = 1000000;
volatile uint32_t iec_tx_timeouts;
volatile uint32_t iec_tx_long_waits;

static PIO  tx_pio;
static uint tx_sm;
static uint tx_offset;
static bool tx_ready;
static uint tx_funcsel;                 /* GPIO_FUNC_PIOn for tx_pio */

#define IEC_TX_IRQ_ABORT 4
#define IEC_TX_IRQ_DONE  5

/* Switch one pin's function select, by direct register write.

   Not gpio_set_function(): this is called from iec_atn_handler(), which is
   the T_AT path and must stay RAM-resident (the
   whole flash-shield design depends on nothing there touching flash). A
   single store to the CTRL register is all gpio_set_function() does that
   matters here; the pad setup was done once at boot by gpio_init() and
   does not change when the function does. */
static inline void __not_in_flash_func(pin_funcsel)(uint pin, uint fn) {
  io_bank0_hw->io[pin].ctrl = fn << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
}

void __not_in_flash_func(iec_pio_atn_reclaim)(void) {
  if (!iec_pio_pins_held)
    return;
  /* Order matters: clear the flag first so a second entry cannot double
     up, then take the pins back. Both stores are single instructions and
     neither can fail. */
  iec_pio_pins_held = 0;
  pin_funcsel(IEC_PIN_CLOCK, GPIO_FUNC_SIO);
  pin_funcsel(IEC_PIN_DATA,  GPIO_FUNC_SIO);
}

bool iec_pio_available(void) {
  return rx_ready;
}

bool iec_pio_init(void) {
  if (rx_ready)
    return true;

  /* Let the SDK pick the block and state machine rather than pinning
     PIO0/SM0. cyw43's SPI driver claims one the same way
     (pio_claim_free_sm_and_add_program_for_gpio_range in
     cyw43_bus_pio_spi.c), so hard-coding a block here would be a race
     with the Wi-Fi bring-up for no benefit -- there are three blocks and
     twelve state machines on RP2350 and this port was using none of them.

     The gpio_range arguments matter on RP2350: a PIO block can only reach
     a 32-pin window, and claiming with the range makes the SDK pick a
     block that can actually see IEC_PIN_DATA.

     set_gpio_base is FALSE, deliberately. Passing true lets the SDK MOVE
     a block's 32-pin window to fit the request, and these claims happen
     before cyw43_arch_init() runs on core 1 -- so a claim that relocated
     a window could leave the Wi-Fi driver unable to reach its own pins
     (23/24/25/29 on a Pico 2 W). False means "only use a block that
     already covers this range", which costs nothing here because the IEC
     pins are 2..7 and the default base of 0 covers 0..31 along with
     everything cyw43 needs. There is no reason for this port to move a
     window, and a good reason not to.

     Honesty about the history: a second PIO claim WAS suspected of
     breaking Wi-Fi bring-up on 2026-09-05. The boot log later showed that
     outage was the AP rejecting twelve join attempts -- the same
     association flakiness measured and fixed on 2026-09-06 -- and not the
     claim at all. This is left as false because it is the correct
     argument regardless, not because it fixed anything. */
  if (!pio_claim_free_sm_and_add_program_for_gpio_range(
          &iec_rx_program, &rx_pio, &rx_sm, &rx_offset,
          IEC_PIN_DATA, 1, false)) {
    /* No PIO available. Not fatal and not worth failing the boot over:
       every other entry point checks rx_ready and does nothing, so the
       software path -- which is still the authoritative one -- carries on
       exactly as before. */
    return false;
  }

  iec_rx_program_init(rx_pio, rx_sm, rx_offset, IEC_PIN_DATA, IEC_PIN_CLOCK);
  rx_ready = true;
  return true;
}

/* Arm for one byte.

   The restart is what keeps PIO and software in step. PIO has no idea
   where a byte begins -- it just counts CLOCK edges -- so left free
   running it would drift by one bit after any aborted transfer and then
   disagree with software for ever after, which would look like a decoding
   bug rather than a framing one. Resetting the shift counter and emptying
   the FIFO at a known point (CLOCK low, before bit 0) makes each byte
   independent of whatever happened before it.

   pio_sm_restart() clears the ISR and its bit counter; clear_fifos drops
   any stale word. Both are single register writes. */
void __not_in_flash_func(iec_pio_rx_arm)(void) {
  if (!rx_ready)
    return;

  pio_sm_set_enabled(rx_pio, rx_sm, false);
  pio_sm_clear_fifos(rx_pio, rx_sm);
  pio_sm_restart(rx_pio, rx_sm);
  pio_sm_exec(rx_pio, rx_sm, pio_encode_jmp(rx_offset));
  pio_sm_set_enabled(rx_pio, rx_sm, true);
}

bool iec_pio_tx_init(void) {
  if (tx_ready)
    return true;

  /* Needs to reach both CLOCK (GPIO3) and DATA (GPIO6), so the claim asks
     for that whole range -- on RP2350 a PIO block only sees a 32-pin
     window and the SDK uses this to pick one that can. */
  if (!pio_claim_free_sm_and_add_program_for_gpio_range(
          &iec_tx_program, &tx_pio, &tx_sm, &tx_offset,
          IEC_PIN_CLOCK, IEC_PIN_DATA - IEC_PIN_CLOCK + 1, false))
    return false;

  iec_tx_program_init(tx_pio, tx_sm, tx_offset, IEC_PIN_CLOCK, IEC_PIN_DATA);

  /* The output REGISTER stays 0 for both lines for ever; only the
     direction is ever changed. That is the whole open-drain emulation --
     drive low, or release and let the pull-up win. If these were ever
     non-zero the state machine would drive the bus high, which on a
     shared open-collector bus means fighting every other device on it. */
  pio_sm_set_pins_with_mask(tx_pio, tx_sm, 0,
                            (1u << IEC_PIN_CLOCK) | (1u << IEC_PIN_DATA));

  tx_funcsel = (uint)PIO_FUNCSEL_NUM(tx_pio, IEC_PIN_CLOCK);
  tx_ready = true;
  return true;
}

bool iec_pio_tx_enabled(void) {
  return tx_ready && iec_pio_tx_owns && !(globalflags & VC20MODE);
}

int __not_in_flash_func(iec_pio_tx_byte)(uint8_t data) {
  /* Pack sixteen bits: (inverted data bit, 0) per bit period. The inverse
     because a data bit of 1 means "release the line" = pindir 0; the
     trailing 0 is the set_data(1) that ends each bit in the C loop. */
  uint32_t w = 0;
  for (int i = 0; i < 8; i++)
    if (!(data & (1u << i)))
      w |= 1u << (i * 2);          /* bit 0 -> drive low -> pindir 1 */

  pio_interrupt_clear(tx_pio, IEC_TX_IRQ_ABORT);
  pio_interrupt_clear(tx_pio, IEC_TX_IRQ_DONE);

  pio_sm_set_enabled(tx_pio, tx_sm, false);
  pio_sm_clear_fifos(tx_pio, tx_sm);
  pio_sm_restart(tx_pio, tx_sm);
  pio_sm_exec(tx_pio, tx_sm, pio_encode_jmp(tx_offset));

  /* Match the line state the CPU is handing over: CLOCK low (driven),
     DATA released. Setting these before the pins are PIO's avoids a
     glitch at the handover. */
  pio_sm_set_consecutive_pindirs(tx_pio, tx_sm, IEC_PIN_CLOCK, 1, true);
  pio_sm_set_consecutive_pindirs(tx_pio, tx_sm, IEC_PIN_DATA,  1, false);

  pio_sm_put(tx_pio, tx_sm, w);

  iec_pio_pins_held = 1;
  pin_funcsel(IEC_PIN_CLOCK, tx_funcsel);
  pin_funcsel(IEC_PIN_DATA,  tx_funcsel);
  pio_sm_set_enabled(tx_pio, tx_sm, true);

  int rc = 0;
  /* Phase D2: opt-in, see the comment on iec_tx_timeout_enabled in
     iec_pio.h. time_us_32() only, not start_timeout()/has_timed_out()
     -- those are flash-resident and this function is not. */
  uint32_t deadline = iec_tx_timeout_enabled
                     ? time_us_32() + iec_tx_timeout_us : 0;
  uint32_t pet = time_us_32();
  bool long_wait = false;
  while (!pio_interrupt_get(tx_pio, IEC_TX_IRQ_DONE)) {
    if (pio_interrupt_get(tx_pio, IEC_TX_IRQ_ABORT)) { rc = -1; break; }
    if (!IEC_ATN)                                    { rc = -2; break; }
    /* If the ATN interrupt fired it has already taken the pins back, and
       the state machine is now driving nothing. Stop waiting. */
    if (!iec_pio_pins_held)                          { rc = -2; break; }
    if (iec_tx_timeout_enabled &&
        (int32_t)(time_us_32() - deadline) >= 0) {
      iec_tx_timeouts++;
      rc = -3;
      break;
    }
    if (time_us_32() - pet >= IEC_TX_PET_US) {   /* iec_pio.h, decision 11b */
      if (!long_wait) { long_wait = true; iec_tx_long_waits++; }
      system_watchdog_pet();
      pet = time_us_32();
    }
  }

  pio_sm_set_enabled(tx_pio, tx_sm, false);
  if (iec_pio_pins_held) {
    iec_pio_pins_held = 0;
    pin_funcsel(IEC_PIN_CLOCK, GPIO_FUNC_SIO);
    pin_funcsel(IEC_PIN_DATA,  GPIO_FUNC_SIO);
  }

  if (rc == 0)       iec_pio_tx_bytes++;
  else if (rc == -1) iec_pio_tx_abort++;
  else               iec_pio_tx_atn++;
  return rc;
}

bool __not_in_flash_func(iec_pio_rx_ready)(void) {
  return rx_ready && !pio_sm_is_rx_fifo_empty(rx_pio, rx_sm);
}

uint8_t __not_in_flash_func(iec_pio_rx_get)(void) {
  /* Shift right with an autopush threshold of 8 leaves the byte in the
     top octet, first-received bit (the bus sends LSB first) at bit 24. */
  uint8_t v = (uint8_t)(pio_sm_get(rx_pio, rx_sm) >> 24);
  iec_pio_match++;
  return v;
}

void __not_in_flash_func(iec_pio_rx_check)(uint8_t sw_val) {
  if (!rx_ready)
    return;

  if (pio_sm_is_rx_fifo_empty(rx_pio, rx_sm)) {
    /* Nothing decoded. Expected whenever the byte was cut short -- an ATN
       abort mid-transfer leaves the software path returning -1 and PIO
       still waiting for edges that will not come. Counted separately from
       a mismatch because it says something completely different. */
    iec_pio_miss++;
    return;
  }

  /* Shift right with an autopush threshold of 8 leaves the byte in the
     top octet, first-received bit (the bus sends LSB first) at bit 24. */
  uint8_t pio_val = (uint8_t)(pio_sm_get(rx_pio, rx_sm) >> 24);

  if (pio_val == sw_val) {
    iec_pio_match++;
  } else {
    iec_pio_mismatch++;
    iec_pio_last_sw  = sw_val;
    iec_pio_last_pio = pio_val;
  }
}
