/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2026 sd2iec_pico2w contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   uart_hw_shim.c: see uart_hw_shim.h for the original reason this file
   is separate (sd2iec's uart.h vs Pico SDK's hardware/uart.h declare
   uart_init/uart_putc/uart_getc with different signatures).

   That header-collision problem turned out to have a second, deeper
   layer: even with the *declarations* kept apart, sd2iec's own
   uart_init() (src/pico/uart.c) and the Pico SDK's real uart_init()
   (compiled into the hardware_uart library's uart.c) are both genuine
   compiled symbols named "uart_init" -- C has no signature-based
   overloading/mangling, so linking both into the same executable is a
   hard "multiple definition of `uart_init'" error regardless of which
   headers declared what. And hardware_uart turns out to be an
   unconditional dependency of pico_stdlib (see its CMakeLists.txt),
   not something pico_enable_stdio_uart(target 0) opts out of.

   So this file does NOT call the SDK's uart_init()/uart_set_baudrate()
   at all -- it programs the PL011 UART registers directly (via
   hardware/structs/uart.h, which only defines a struct layout, not
   function symbols, so it can't collide with anything). The sequence
   below is transcribed from hardware_uart's own uart_init()/
   uart_set_baudrate() (BSD-3-Clause, Raspberry Pi Ltd) with the
   already-enabled/reconfigure-safety dance removed, since this only
   ever runs once at boot on a freshly-reset (i.e. already disabled)
   UART -- see that source for the general case this is narrowed from.
*/

#include <stdio.h>   /* putchar(); was relying on an implicit declaration */
#include "uart_hw_shim.h"

#include "hardware/structs/uart.h"
#include "hardware/regs/uart.h"
#include "hardware/regs/resets.h"
#include "hardware/resets.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/stdio.h"

#define SHIM_UART_HW uart0_hw
#define SHIM_UART_TX_PIN PICO_DEFAULT_UART_TX_PIN
#define SHIM_UART_RX_PIN PICO_DEFAULT_UART_RX_PIN

void pico_hw_uart_init(unsigned baudrate) {
  reset_block(RESETS_RESET_UART0_BITS);
  unreset_block_wait(RESETS_RESET_UART0_BITS);

  /* Baud rate: 16-bit integer + 6-bit fractional divisor, PL011-style.
     Same formula as uart_set_baudrate(). */
  uint32_t clk = clock_get_hz(clk_peri);
  uint32_t baud_rate_div = (8 * clk / baudrate) + 1;
  uint32_t baud_ibrd = baud_rate_div >> 7;
  uint32_t baud_fbrd;

  if (baud_ibrd == 0) {
    baud_ibrd = 1;
    baud_fbrd = 0;
  } else if (baud_ibrd >= 65535) {
    baud_ibrd = 65535;
    baud_fbrd = 0;
  } else {
    baud_fbrd = (baud_rate_div & 0x7f) >> 1;
  }

  SHIM_UART_HW->ibrd = baud_ibrd;
  SHIM_UART_HW->fbrd = baud_fbrd;

  /* 8 data bits, 1 stop bit, no parity, FIFOs enabled. This LCR_H write
     also latches the IBRD/FBRD divisor written above (PL011
     requirement). Safe to write directly (not via a disable-then-
     restore dance) because the UART is still disabled post-reset. */
  SHIM_UART_HW->lcr_h =
      ((8u - 5u) << UART_UARTLCR_H_WLEN_LSB) |
      UART_UARTLCR_H_FEN_BITS;

  SHIM_UART_HW->cr = UART_UARTCR_UARTEN_BITS | UART_UARTCR_TXE_BITS | UART_UARTCR_RXE_BITS;

  gpio_set_function(SHIM_UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(SHIM_UART_RX_PIN, GPIO_FUNC_UART);
}

bool pico_hw_uart_is_writable(void) {
  return !(SHIM_UART_HW->fr & UART_UARTFR_TXFF_BITS);
}

void pico_hw_uart_putc_raw(char c) {
  while (!pico_hw_uart_is_writable()) {
    tight_loop_contents();
  }
  SHIM_UART_HW->dr = (uint8_t)c;
}

bool pico_hw_uart_is_readable(void) {
  return !(SHIM_UART_HW->fr & UART_UARTFR_RXFE_BITS);
}

char pico_hw_uart_getc_raw(void) {
  return (char)(SHIM_UART_HW->dr & 0xff);
}

/* pico_stdio_usb depends on pico_stdio/pico_time/pico_unique_id/
   tinyusb_device_unmarked -- none of which touch hardware_uart, so this
   doesn't reintroduce the uart_init collision documented above. It
   self-services via a real USB IRQ + a claimed low-priority IRQ (see
   stdio_usb.c), not manual polling, so it keeps working from inside
   sd2iec's tight bus_mainloop() as long as interrupts stay enabled. */
/* Recorded because stdio_init_all() can fail and say nothing: on a
   failure path stdio_usb_init() returns false (the assert that would have
   caught it is compiled out in a release build) and the USB driver is
   simply never enabled. Nothing downstream would notice -- putchar()
   still succeeds, it just goes nowhere, which is precisely the symptom
   this port has. See bootlog.h. */
volatile uint8_t pico_usb_stdio_ok;

void pico_hw_usb_stdio_init(void) {
  pico_usb_stdio_ok = stdio_init_all() ? 1 : 0;
}

void pico_hw_usb_putc(char c) {
  putchar(c);
}
