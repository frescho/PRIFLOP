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


   arch-config.h: The main architecture-specific config header (RP2350 /
   Pico 2 W)

   STATUS: IEC signalling here is plain GPIO
   bit-banging via gpio_put/gpio_get + gpio_set_irq_enabled_with_callback,
   the same technique every non-ESP arch in this tree uses (AVR, lpc17xx)
   for the base (non-fastloader) IEC protocol -- it is NOT the PIO engine
   described in the design notes. That's deliberate: the base
   protocol's timing tolerances are loose enough for plain GPIO+IRQ, so
   this should be enough to get LOAD/SAVE/directory listing working
   against real hardware without waiting on Phase 2's PIO work, which is
   specifically about the tight timing fastloaders (fl-*.c) need and
   about surviving flash-XIP stalls during OTA writes. Expect set_atn/set_data/set_clock and the interrupt wiring
   here to be replaced (not just extended) once PIO lands -- don't build
   more on top of the plain-GPIO version than necessary.
*/

#ifndef ARCH_CONFIG_H
#define ARCH_CONFIG_H

/* Poison Pico SDK's hardware/uart.h before anything transitively
   includes it (pico/stdlib.h does). It declares uart_init/uart_putc/
   uart_getc with different signatures than sd2iec's own src/uart.h --
   included right after this header by every .c file via config.h, in
   the SAME translation unit -- so a conflicting-redeclaration error is
   otherwise unavoidable no matter how deep in the include graph
   hardware/uart.h shows up. Defining its include guard here makes the
   preprocessor skip its body wherever it's reached from this file.
   Safe because the only file that legitimately needs the real
   hardware/uart.h -- uart_hw_shim.c (see its header comment) -- never
   includes arch-config.h/config.h, so it never sees this poisoned
   guard.

   Other SDK headers (pico/stdio_uart.h, reached the same transitive
   way) declare functions taking a "uart_inst_t *" parameter without
   needing the complete type -- an opaque forward declaration is enough
   for those to still compile. Redeclaring the same typedef the real
   header would have provided (a pointer to an incomplete struct) keeps
   that working without pulling in the colliding function declarations
   the struct's real header also carries. */
#define _HARDWARE_UART_H
typedef struct uart_inst uart_inst_t;

#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
/* Deliberately NOT including pico/cyw43_arch.h here even though the LED
   comments below reference cyw43_arch_gpio_put()/cyw43_arch_init() --
   arch-config.h itself doesn't call them, only declares plain bool-based
   wrappers (set_led()/led_sync_pending()) that system.c and network.c
   implement, and those two files already include cyw43_arch.h directly.
   Found 2026-08-23: once network.c switched this build from
   pico_cyw43_arch_none to pico_cyw43_arch_lwip_poll, CYW43_LWIP=1 made
   cyw43_arch.h (via cyw43.h -> lwip/netif.h -> ... -> arch/cc.h)
   transitively pull in the toolchain's real <sys/time.h> -> <time.h>,
   which every single translation unit then got for free through
   config.h -- and any of them (main.c) that also #include "time.h"
   (sd2iec's own, unrelated, quote-form file) got two conflicting
   `struct tm` definitions in the same TU. Not including this header
   this broadly avoids dragging lwIP's transitive includes into every
   file that includes config.h. */

#define _BV(x) (1<<(x))

#define _USE_IOCTL 0
#define _READONLY 0
#define _USE_LESS_BUF 0
#define _MCU_ENDIAN 2

/* ESP8266's marker for "place this function in RAM, not flash", and on
   this port it means that again.

   It was a no-op until 2026-09-05, on the reasoning that "RP2350 code
   runs fine from flash (XIP + cache)". That was true when written and
   false ever since core 1 started running Wi-Fi out of the same flash:
   core 0's bit-banging then shares the XIP path with lwIP, and an
   instruction fetch that misses the cache stalls for a flash read of
   unpredictable length. During an erase or program the XIP window is
   gone entirely. The IEC receive path has no timing margin for either,
   and this macro sat on exactly the six functions that needed it while
   expanding to nothing.

   The old comment said to use __not_in_flash_func(name) at the call site
   because it wraps a name rather than being a prefix attribute. That is
   true of that macro but not of the underlying mechanism: the section
   attribute it applies is perfectly legal in prefix position, so the
   ESP-style spelling works unchanged. .time_critical* is placed in RAM
   by the SDK's default linker script.

   noinline is deliberate. Without it the compiler may inline one of
   these into a flash-resident caller, which silently puts the code back
   in flash and reintroduces the stall this exists to prevent. */
#define IRAM_ATTR __attribute__((section(".time_critical.iec"), noinline))

/* ---------------------------------------------------------------- */
/* Flash partition map                                              */
/* ---------------------------------------------------------------- */
/* Firmware 1MB, then the littlefs volume, then the settings sector at
   the end of the chip: flash_layout.h, sized from the board's flash.
   The volume grew from 1MB to 3MB on 2026-09-05: a D81 format rewrites
   every sector and littlefs is copy-on-write, so it needs 800KB free. */
#include "flash_layout.h"   /* sizes follow the board's flash: PICO_FLASH_SIZE_BYTES */

#if CONFIG_HARDWARE_VARIANT==1

/*** Device address selection ***/
static inline uint8_t device_hw_address(void)
{
  /* OPTIONAL: wire a DIP switch / jumper pair to override this. */
  return 8;
}

static inline void device_hw_address_init(void)
{
}


/*** LEDs ***/
/* Pico W/2 W's only LED is wired through the CYW43439 (Wi-Fi chip), not
   a plain GPIO. Talking to it means a
   blocking SPI-like transaction via cyw43_arch_gpio_put(), which is NOT
   safe to call from interrupt context. src/timer.c's SYSTEM_TICK_HANDLER
   (100Hz) calls set_led()/toggle_led() unconditionally on every tick, so
   a naive direct cyw43_arch_gpio_put() call here would run a blocking
   cyw43 transaction from inside an ISR every 10ms -- cyw43_arch
   explicitly does not support being called from IRQ context and this
   will hang or corrupt driver state.
   Fix: set_led()/toggle_led() only update an in-RAM shadow flag here
   (cheap, ISR-safe). led_sync_from_isr() -- called from non-ISR context,
   e.g. once per bus_mainloop() iteration once that exists -- pushes the
   shadow value out to the real LED only when it changed. Until
   something calls led_sync_from_isr(), the LED will reflect whatever it
   was last explicitly set to at init, not live status -- acceptable for
   Phase 1 (no bus loop yet) but revisit once Phase 2's mainloop exists. */

#define SINGLE_LED

/* Real implementation in system.c, not inlined here like most other
   arches' leds_init(): it has to call cyw43_arch_init(), which is
   nontrivial/stateful (see the comment above). */
void leds_init(void);

extern volatile bool _led_state;
extern volatile bool _led_state_dirty;

/* No IRAM_ATTR: these are static inline in a header, so they are compiled
   into whichever function calls them and already live wherever that
   caller does. Marking them would also be self-contradictory now that
   IRAM_ATTR carries noinline, and would emit a separate out-of-line copy
   into .time_critical for every translation unit that includes this. */
static inline void set_led(int state)
{
  bool new_state = state != 0;
  if (new_state != _led_state) {
    _led_state = new_state;
    _led_state_dirty = true;
  }
}

static inline void toggle_led(void)
{
  _led_state = !_led_state;
  _led_state_dirty = true;
}

/* Pushes any pending LED state (set via set_led()/toggle_led(), possibly
   from ISR context) out to the real hardware. Call from non-ISR context
   only -- e.g. once per bus_mainloop() iteration once that exists.
   Defined in system.c. */
void led_sync_pending(void);

/*** IEC signals ***/

  // Pin assignment is a Phase 1 placeholder -- adjust to match your
  // actual wiring, then update this comment. Chosen to avoid:
  //   GPIO 0/1  - PICO_DEFAULT_UART (debug UART)
  //   GPIO 4/5  - PICO_DEFAULT_I2C (reserved for a future display)
  //
  //   GPIO 16-19 - PICO_DEFAULT_SPI
  //   GPIO 23,24,25,29 - wired to the CYW43439, not available at all
  // A real IEC bus needs a level-shifter or equivalent between these
  // pins and the 5V bus; the "5V tolerance"
  // caveat is not resolved by this file.
#define IEC_PIN_ATN   2
#define IEC_PIN_CLOCK 3
#define IEC_PIN_DATA  6
#define IEC_PIN_SRQ   7

/*** User interface ***/
/* Not wired yet -- Phase 1 placeholder pins, no debouncing hardware
   assumed. Buttons could come back (the
   ESP8266 arch dropped them for lack of free pins; this board doesn't
   have that excuse). */
#define BUTTON_NEXT _BV(0)
#define BUTTON_PREV _BV(1)
#define BUTTON_NEXT_PIN 8
#define BUTTON_PREV_PIN 9

typedef unsigned int rawbutton_t;

static inline rawbutton_t buttons_read(void)
{
  rawbutton_t result = 0;
  if (gpio_get(BUTTON_NEXT_PIN)) result |= BUTTON_NEXT;
  if (gpio_get(BUTTON_PREV_PIN)) result |= BUTTON_PREV;
  return result;
}

static inline void buttons_init(void)
{
  gpio_init(BUTTON_NEXT_PIN);
  gpio_set_dir(BUTTON_NEXT_PIN, GPIO_IN);
  gpio_pull_up(BUTTON_NEXT_PIN);

  gpio_init(BUTTON_PREV_PIN);
  gpio_set_dir(BUTTON_PREV_PIN, GPIO_IN);
  gpio_pull_up(BUTTON_PREV_PIN);
}

/* No set_button_irq(): nothing calls it. Its only caller was an ESP8266
   branch that returned from the sleep loop and relied on a pin-change IRQ
   to resume; that arch is gone, and this port's sleep loop simply polls
   key_pressed(KEY_SLEEP) around system_sleep(). If wake-from-sleep ever
   needs a real interrupt, add it next to set_atn_irq() below rather than
   as an empty macro that reads like a wired-up feature. */

/*** board-specific initialisation ***/
#define HAVE_BOARD_INIT
static inline void board_init(void)
{
}

#else
#  error "CONFIG_HARDWARE_VARIANT is unset or set to an unknown value."
#endif


/* ---------------- End of user-configurable options ---------------- */

#ifdef CONFIG_ADD_SD
/* Not used by this port -- SD support is dropped entirely. Kept only because config.h expects these to
   exist whenever CONFIG_ADD_SD is defined; config-pico2w never defines
   it, so this block never compiles in practice. */
static inline void sdcard_interface_init(void)
{
}

static inline uint8_t sdcard_detect(void)
{
  return true;
}

static inline uint8_t sdcard_wp(void)
{
  return false;
}
#endif


#if !defined(CONFIG_HAVE_IEC) && !defined(CONFIG_HAVE_IEEE)
#  error Need CONFIG_HAVE_IEC and/or CONFIG_HAVE_IEEE
#endif

#if defined(CONFIG_HAVE_IEC) && defined(CONFIG_HAVE_IEEE)
#  error Sorry, dual-interface devices must select only one interface at compile time!
#endif


/* --- IEC --- */
#ifdef CONFIG_HAVE_IEC

typedef uint8_t iec_bus_t;

#define IEC_BUS_H

#define IEC_BIT_ATN      _BV(IEC_PIN_ATN)
#define IEC_BIT_DATA     _BV(IEC_PIN_DATA)
#define IEC_BIT_CLOCK    _BV(IEC_PIN_CLOCK)
#define IEC_BIT_SRQ      _BV(IEC_PIN_SRQ)

void iec_interrupts_init(void);

/* IEC lines are open-drain: gpio_put() is only ever left at 0, and
   set_*() toggles direction between GPIO_IN (released, external
   pull-up wins, reads high) and GPIO_OUT (driven low). This matches
   the technique every other arch in this tree uses for the base
   protocol; see the file header for why this isn't PIO yet. */
static inline iec_bus_t iec_bus_read(void) {
  iec_bus_t val = 0;
  if (gpio_get(IEC_PIN_ATN))   val |= IEC_BIT_ATN;
  if (gpio_get(IEC_PIN_CLOCK)) val |= IEC_BIT_CLOCK;
  if (gpio_get(IEC_PIN_DATA))  val |= IEC_BIT_DATA;
  return val;
}

/* Single-line current-state accessors used throughout the shared core
   (iec.c etc., e.g. "if (!IEC_ATN)") -- easy to miss porting since
   nothing fails to *link* without them, only to compile once something
   actually references one. */
#define IEC_ATN   (gpio_get(IEC_PIN_ATN))
#define IEC_CLOCK (gpio_get(IEC_PIN_CLOCK))
#define IEC_DATA  (gpio_get(IEC_PIN_DATA))

static inline __attribute__((always_inline)) void set_atn(uint8_t state) {
  if (state) {
    gpio_set_dir(IEC_PIN_ATN, GPIO_IN);
  } else {
    gpio_set_dir(IEC_PIN_ATN, GPIO_OUT);
  }
}

static inline __attribute__((always_inline)) void set_data(uint8_t state) {
  if (state) {
    gpio_set_dir(IEC_PIN_DATA, GPIO_IN);
  } else {
    gpio_set_dir(IEC_PIN_DATA, GPIO_OUT);
  }
}

static inline __attribute__((always_inline)) void set_clock(uint8_t state) {
  if (state) {
    gpio_set_dir(IEC_PIN_CLOCK, GPIO_IN);
  } else {
    gpio_set_dir(IEC_PIN_CLOCK, GPIO_OUT);
  }
}

static inline __attribute__((always_inline)) void set_srq(uint8_t state) {
  if (state) {
    gpio_set_dir(IEC_PIN_SRQ, GPIO_IN);
  } else {
    gpio_set_dir(IEC_PIN_SRQ, GPIO_OUT);
  }
}

#endif /* CONFIG_HAVE_IEC */


/* Interrupt handler for system tick -- must match the weak vector-table
   symbol name Pico SDK 2.3.0's pico_crt0/crt0.S expects for SysTick,
   confirmed while writing arch-timer.c (Phase 1). */
#define SYSTEM_TICK_HANDLER void isr_systick(void)

/* Unlike AVR/lpc17xx, RP2350 GPIO IRQs share one dispatch mechanism
   (gpio_set_irq_enabled_with_callback()) rather than having distinct
   per-pin hardware vectors, so -- same as the ESP8266 arch -- these are
   plain named functions that system.c's shared GPIO callback calls
   manually after checking which pin fired, not real vector names. */
#define SD_CHANGE_HANDLER  void sdcard_change_handler(void)
#define IEC_ATN_HANDLER    void iec_atn_handler(void)
#define IEC_CLOCK_HANDLER  void iec_clock_handler(void)
#ifdef PARALLEL_ENABLED
#define PARALLEL_HANDLER   void parallel_handler(void)
#endif

/* Enable/disable ATN interrupt */
static inline void set_atn_irq(uint8_t state) {
  gpio_set_irq_enabled(IEC_PIN_ATN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, state);
}

/* Enable/disable CLOCK interrupt */
#define HAVE_CLOCK_IRQ
static inline void set_clock_irq(uint8_t state) {
  gpio_set_irq_enabled(IEC_PIN_CLOCK, GPIO_IRQ_EDGE_FALL, state);
}

/* Display interrupt pin */
#ifdef CONFIG_REMOTE_DISPLAY
static inline void display_intrq_init(void) {
  // TODO once a display is actually wired up
}

static inline uint8_t display_intrq_active(void) {
  return 0;
}
#endif

/* disable_interrupts()/enable_interrupts() (the real symbols main.c
   calls) are implemented directly in system.c using cpsid/cpsie i --
   deliberately NOT defining __disable_irq()/__enable_irq() macros here:
   Pico SDK's CMSIS core_cm33.h already declares those names as real
   inline functions, and shadowing them with object-like macros anywhere
   they might get included would produce very confusing errors for no
   benefit, since nothing in the shared core actually calls
   __disable_irq()/__enable_irq() directly -- only this arch's own
   system.c needs the underlying mechanism, so it uses it directly. */

#endif
