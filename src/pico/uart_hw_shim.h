/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2026 sd2iec_pico2w contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   uart_hw_shim.h: differently-named UART primitives for src/pico/uart.c
   to call.

   Why this exists: sd2iec's src/uart.h declares "void uart_init(void)",
   "void uart_putc(char)" and "unsigned char uart_getc(void)" -- names
   Pico SDK's hardware/uart.h also declares, with different signatures
   ("uint uart_init(uart_inst_t*, uint)", etc). That's a compile-time
   conflicting-redeclaration error if both headers are visible in one
   translation unit. It gets worse than a header problem, though: even
   with declarations kept apart, hardware_uart's compiled uart.c and
   sd2iec's own src/pico/uart.c both define a real symbol literally
   named "uart_init", and hardware_uart is an unconditional dependency
   of pico_stdlib (not something disabled by pico_enable_stdio_uart) --
   so linking both is a hard "multiple definition" error regardless of
   what any header declares. uart_hw_shim.c sidesteps this entirely by
   programming the UART registers directly (hardware/structs/uart.h,
   which is just a struct layout, not function symbols) instead of
   calling the SDK's uart_init()/etc -- see that file for the register
   sequence and its provenance. This header just gives src/pico/uart.c
   something non-colliding to call.
*/

#ifndef UART_HW_SHIM_H
#define UART_HW_SHIM_H

#include <stdbool.h>

void pico_hw_uart_init(unsigned baudrate);
bool pico_hw_uart_is_writable(void);
void pico_hw_uart_putc_raw(char c);
bool pico_hw_uart_is_readable(void);
char pico_hw_uart_getc_raw(void);

/* USB CDC debug console -- mirrors everything sent to the physical
   UART. Exists because testing (2026-08-22) has no
   debug probe or USB-serial adapter available: the same USB cable used
   to flash the board can also carry a serial console this way, with no
   extra hardware. See uart_hw_shim.c for why this doesn't reintroduce
   the uart_init collision (pico_stdio_usb doesn't depend on
   hardware_uart at all). */
void pico_hw_usb_stdio_init(void);
void pico_hw_usb_putc(char c);

#endif
