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


   main.c: Lots of init calls for the submodules

*/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "buffers.h"
#include "bus.h"
#include "diskchange.h"
#include "diskio.h"
#include "bootlog.h"
#include "display.h"
#include "eeprom-conf.h"
#include "errormsg.h"
#include "lfsops.h"
#include "filesystem.h"
#include "ramops.h"
#include "primo_turbo.h"
#include "i2c.h"
#include "led.h"
#include "time.h"
#include "rtc.h"
#include "spi.h"
#include "system.h"
#include "timer.h"
#include "uart.h"
#include "ustring.h"
#include "utils.h"


#if defined(__AVR__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 1))
int main(void) __attribute__((OS_main));
#endif

/* Default no-op: most arches have no networking and nothing to init here.
   __attribute__((weak)) lets src/pico/network.c override it with a real
   Wi-Fi connect + httpd_init() (see system.h). */
void network_init(void) __attribute__((weak));
void network_init(void) {
}

/* Default no-op: most arches have no second core. __attribute__((weak))
   lets src/pico/core1.c override it with a real multicore_launch_core1()
   call (see system.h). */
void core1_launch(void) __attribute__((weak));
void core1_launch(void) {
}

int main(void) {
  stack_paint();
  fault_check_previous();
  boot_count_init();
  hang_diag_scratch_init();
  /* Early system initialisation */
  board_init();
  system_init_early();
  bootlog_init();   /* before anything worth logging */
  leds_init();

  set_busy_led(1);
  set_dirty_led(0);

  /* Due to an erratum in the LPC17xx chips anything that may change */
  /* peripheral clock scalers must come before system_init_late()    */
  uart_init();
  /* Timestamped because stdio_init_all() sits inside here and blocks for
     up to PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS waiting for a USB host
     that, on a drive plugged into a C64 rather than a laptop, is never
     going to arrive. If that wait is the bulk of the pre-bus delay then
     the drive is unavailable to the C64 for two seconds every boot for
     no reason at all. */
  uart_puts_P(PSTR("init: uart/stdio done\r\n"));
#ifndef SPI_LATE_INIT
  spi_init(SPI_SPEED_SLOW);
#endif
  timer_init();
  bus_interface_init();
  i2c_init();

  /* Second part of system initialisation, switches to full speed on ARM */
  system_init_late();
  enable_interrupts();

  /* Internal-only initialisation, called here because it's faster */
  buffers_init();
  buttons_init();

  /* Anything that does something which needs the system clock */
  /* should be placed after system_init_late() */
  uart_puts_P(PSTR("init: pre-bus_init\r\n"));
  bus_init();    // needs delay
  rtc_init();    // accesses I2C
  disk_init();   // accesses card
  read_configuration();
  primo_turbo_apply_setting();   /* the saved turbo switch (primo_turbo.h) */

  uart_puts_P(PSTR("init: disk/config done\r\n"));
  filesystem_init(0);
  change_init();
  /* Drive 9, 2026-09-10: a second, independent littlefs volume in RAM --
     see ramops.c's header comment. Deliberately after filesystem_init(0)
     so lfsops_init() has already claimed partition 0 and set max_part;
     ramops_init() only ever raises max_part, never resets it. Runs
     before system_watchdog_start() (iec_mainloop(), reached later via
     bus_mainloop() below) for the same reason the flash format-on-bad-
     mount path does: formatting is real work and there is no reason to
     make the watchdog stand guard over one-time boot setup. */
  ramops_init();
  uart_puts_P(PSTR("init: filesystem done\r\n"));
  network_init();
  core1_launch();

  /* Composed into one buffer and sent with a single uart_puts_P() call
     (rather than three separate calls, each individually lock/unlocked
     -- see uart.c's console_lock comment) so core 1's own boot-time
     logging (core1.c) can't land in the middle of this banner. Confirmed
     on real hardware that it otherwise does,
     2026-09-02. */
  char banner[48];
  snprintf(banner, sizeof(banner), "\r\nsd2iec " VERSION " #%02x\r\n", device_address);
  uart_puts_P(banner);

#ifdef CONFIG_REMOTE_DISPLAY
  /* at this point all buffers should be free, */
  /* so just use the data area of the first to build the string */
  uint8_t *strbuf = buffers[0].data;
  ustrcpy_P(strbuf, versionstr);
  ustrcpy_P(strbuf+ustrlen(strbuf), longverstr);
  if (display_init(ustrlen(strbuf), strbuf)) {
    display_address(device_address);
    display_current_part(0);
  }
#endif

  set_busy_led(0);

#if defined(HAVE_SD) && BUTTON_PREV != 0
  /* card switch diagnostic aid - hold down PREV button to use */
  if (!(buttons_read() & BUTTON_PREV)) {
    while (buttons_read() & BUTTON_NEXT) {
      set_dirty_led(sdcard_detect());
# ifndef SINGLE_LED
      set_busy_led(sdcard_wp());
# endif
    }
    reset_key(0xff);
  }
#endif

  bus_mainloop();

  while (1);
}
