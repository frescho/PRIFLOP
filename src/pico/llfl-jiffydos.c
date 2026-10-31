/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

   RP2350 / Raspberry Pi Pico 2 W port by frescho.
   Ported from the ESP8266 back end (src/esp/llfl-jiffydos.c in the
   pre-fatfs-removal tree). The protocol and its timing tables are Ingo
   Korb's; only the timebase underneath is new.

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


   llfl-jiffydos.c: JiffyDOS transfer support.

   WAS A STUB UNTIL 2026-09-06, and the stub was load-bearing in an
   unfortunate way: src/iec.c calls jiffy_send()/jiffy_receive()
   unconditionally whenever a host negotiates JiffyDOS at runtime, with no
   CONFIG_LOADER_JIFFY to gate it out, so the file had to exist for the
   firmware to link. It reported real bus state but moved no data. A host
   that negotiated JiffyDOS therefore got a drive that agreed to the
   protocol and then said nothing -- which is exactly what FB1K did on
   real hardware: loaded, cleared the screen, asked for the directory, and
   hung on a grey screen for ever.

   FOUR BITS PER EDGE, TWO LINES. JiffyDOS sends two bits at a time on
   CLOCK and DATA together, four such pairs per byte, with every transition
   placed at a fixed offset from one reference edge rather than as a chain
   of delays -- so timing error cannot accumulate across a byte. The
   offsets below are in units of 100 ns and are unchanged from the
   reference implementation; they describe what a 1541 does, and software
   depends on the details.

   NOT ON PIO, and that is a considered choice rather than an omission.
   The protocol is a short, strictly sequential sequence of "wait for an
   edge, then place bits at four fixed offsets", which a 150 MHz core with
   interrupts off does exactly and readably. PIO's advantage is autonomy
   from the CPU, and there is nothing to be autonomous from here: the CPU
   has nothing else to do during the microseconds a byte takes. What PIO
   bought on the standard bus -- freeing core 0 during the millisecond-long
   waits of the slow protocol -- has no equivalent in a transfer this
   short.
*/

#include "config.h"
#include "arch-config.h"
#include "iec-bus.h"
#include "llfl-common.h"
#include "system.h"
#include "timer.h"
#include "fastloader-ll.h"

static const generic_2bit_t jiffy_receive_def = {
  .pairtimes = {185, 315, 425, 555},
  .clockbits = {4, 6, 3, 2},
  .databits  = {5, 7, 1, 0},
  .eorvalue  = 0xff
};

static const generic_2bit_t jiffy_send_def = {
  .pairtimes = {100, 200, 310, 410},
  .clockbits = {0, 2, 4, 6},
  .databits  = {1, 3, 5, 7},
  .eorvalue  = 0
};

IRAM_ATTR
uint8_t jiffy_receive(iec_bus_t *busstate) {
  uint8_t result;

  llfl_setup();
  disable_interrupts();

  /* Initial handshake - wait for rising clock, but emulate ATN-ACK */
  set_clock(1);
  set_data(1);
  do {
    llfl_wait_clock(1, ATNABORT);
    if (!IEC_ATN)
      set_data(0);
  } while (!IEC_CLOCK);

  /* receive byte */
  result = llfl_generic_save_2bit(&jiffy_receive_def);

  /* read EOI info */
  *busstate = llfl_read_bus_at(670);

  /* exit with data low */
  llfl_set_data_at(730, 0);
  delay_us(10);

  enable_interrupts();
  llfl_teardown();
  return result;
}

IRAM_ATTR
uint8_t jiffy_send(uint8_t value, uint8_t eoi, uint8_t loadflags) {
  unsigned int loadmode = loadflags & 0x80;
  unsigned int skipeoi  = loadflags & 0x7f;

  llfl_setup();
  disable_interrupts();

  /* Initial handshake */
  set_data(1);
  set_clock(1);
  delay_us(3);

  if (loadmode) {
    /* LOAD mode: start marker is data low */
    while (!IEC_DATA) ; // wait until data actually is high again
    llfl_wait_data(0, ATNABORT);
  } else {
    /* single byte mode: start marker is data high */
    llfl_wait_data(1, ATNABORT);
  }

  /* transmit data */
  llfl_generic_load_2bit(&jiffy_send_def, value);

  /* Send EOI info */
  if (!skipeoi) {
    if (eoi) {
      llfl_set_2bit_at(520, 1, 0);
    } else {
      /* LOAD mode also uses this for the final byte of a block */
      llfl_set_2bit_at(520, 0, 1);
    }

    /* wait until data is low */
    delay_us(3); // allow for slow rise time
    while (IEC_DATA && IEC_ATN) ;
  }

  /* hold time */
  delay_us(10);

  enable_interrupts();
  llfl_teardown();
  return !IEC_ATN;
}
