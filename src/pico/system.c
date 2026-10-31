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


   system.c: System-specific initialisation (RP2350 / Pico 2 W)
*/

#include <stdint.h>
#include "config.h"
#include "atomic.h"
#include "system.h"
#include "uart.h"

#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/scb.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"

/* --- LED shadow state, see the long comment in arch-config.h for why --- */
volatile bool _led_state = false;
volatile bool _led_state_dirty = false;

/* cyw43_arch_init()/cyw43_arch_gpio_put() moved to src/pico/core1.c
   (2026-09-02): the pico_cyw43_driver
   library asserts get_core_num() == async_context_core_num(...) inside
   itself, i.e. every cyw43 call must happen on whichever core called
   cyw43_arch_init() -- confirmed the hard way on real hardware (AP/
   association worked, DHCP silently didn't, because this build's
   -DNDEBUG strips those asserts instead of catching the violation
   loudly). Since core 1 now owns WiFi/lwIP entirely, it has to be the
   core that calls cyw43_arch_init() too, not core 0 via leds_init()
   here. leds_init() itself is now a no-op: the Pico W/2 W's only LED is
   wired through the cyw43439 chip (see arch-config.h), so there's
   nothing left for core 0 to initialize. led_sync_pending() (pushes
   set_led()/toggle_led()'s in-RAM shadow -- safe to set from core 0's
   ISR context -- out to the real LED) now lives in core1.c for the same
   reason: it ends in a cyw43_arch_gpio_put() call, so it must run on
   core 1. _led_state/_led_state_dirty above stay here and stay
   core-0-writable -- they're a plain single-writer(core 0)/single-
   reader(core 1) flag pair, not a cyw43 call themselves. */
void leds_init(void) {
}

/* --- Early/late init, called from main.c in that order --- */

void system_init_early(void) {
  /* IEC pins must be gpio_init()'d before bus_interface_init() (which
     runs before system_init_late() in main.c's fixed call order) calls
     set_atn()/set_data()/set_clock()/set_srq() to toggle direction.
     Latch the output value to 0 once here so every later set_*(0) just
     has to flip direction to GPIO_OUT to drive the line low, matching
     the open-drain technique described in arch-config.h. */
  const uint iec_pins[] = { IEC_PIN_ATN, IEC_PIN_CLOCK, IEC_PIN_DATA, IEC_PIN_SRQ };
  for (unsigned i = 0; i < sizeof(iec_pins) / sizeof(iec_pins[0]); i++) {
    gpio_init(iec_pins[i]);
    gpio_put(iec_pins[i], 0);
    gpio_set_dir(iec_pins[i], GPIO_IN);
    /* Internal weak pull-up (~50-80kOhm), re-added 2026-09-02 after it was removed 2026-09-01 on the theory
       that the real C64/IEC bus's own ~1kOhm pull-ups make it redundant
       (still true whenever a C64 is actually attached and powered --
       ~1kOhm utterly dominates ~50-80kOhm). The removal's real cost
       showed up the next time Wi-Fi got tested *without* the C64 powered
       on: with no pull-up anywhere, these pins float, and a floating
       IEC_ATN/IEC_CLOCK reading as spuriously "active" sends the bus
       state machine out of BUS_IDLE and into BUS_FOUNDATN, where it
       blocks waiting for a real C64 response that never comes --
       starving network_poll() (which only ever runs from inside
       BUS_IDLE, see iec.c) completely, silently, indefinitely. That's
       exactly what an entire night's session chased as a WiFi/DHCP
       "regression" before realizing the WiFi code was never broken --
       every test that night happened without the C64 powered. Restoring
       the pull-up costs nothing with a real C64 attached (same dominance
       argument as before) and makes standalone Wi-Fi testing/operation
       actually possible again -- for
       the full back-and-forth. */
    gpio_pull_up(iec_pins[i]);
  }
}

/* Nothing left to do here: leds_init() above is a no-op (cyw43_arch_init()
   moved to core1.c, see its comment), and ata.c/arch-eeprom.c went back
   to a plain save_and_disable_interrupts() for their flash writes (see
   ata.c's commit_slot() comment for why the pico_flash flash_safe_execute()
   attempt was reverted), so there's no more flash_safe_execute_core_init()
   registration to do here either. Kept as an empty function rather than
   removed -- main.c calls it unconditionally as part of the fixed init
   order every other arch also follows. */
void system_init_late(void) {
}

/* --- Interrupt control, used by main.c and shared core code --- */

void disable_interrupts(void) {
  __asm volatile ("cpsid i" ::: "memory");
}

void enable_interrupts(void) {
  __asm volatile ("cpsie i" ::: "memory");
}

/* --- IEC GPIO interrupt demux ---
   RP2350 GPIO IRQs share one dispatch mechanism
   (gpio_set_irq_enabled_with_callback()) rather than per-pin hardware
   vectors; unlike the ESP8266 arch (which doesn't check which pin fired,
   see arch-config.h's IEC_ATN_HANDLER/IEC_CLOCK_HANDLER comment), the
   Pico SDK callback tells us exactly which GPIO and edge fired, so this
   dispatches precisely instead of calling both handlers unconditionally.
*/

IEC_ATN_HANDLER;
#ifdef CONFIG_LOADER_DREAMLOAD
/* iec_clock_handler() is only actually defined by llfl-dreamload.c
   (src/esp/llfl-dreamload.c and src/lpc17xx/llfl-dreamload.c both gate
   it this way too) -- referencing/calling it outside that config would
   be an undefined-reference link error, not just dead code. */
IEC_CLOCK_HANDLER;
#endif

static void gpio_irq_dispatch(uint gpio, uint32_t events) {
  (void)events;
  if (gpio == IEC_PIN_ATN) {
    iec_atn_handler();
  }
#ifdef CONFIG_LOADER_DREAMLOAD
  else if (gpio == IEC_PIN_CLOCK) {
    iec_clock_handler();
  }
#endif
}

/*--------------------------------------------------------------------------
   Flash interrupt shield
   ----------------------
   A 4KB erase takes ~38ms and the CPU cannot fetch from XIP while it runs,
   which is why lfs_flash.c used to disable interrupts across it. But the
   published limit for answering ATN by pulling DATA is 1000us
   (T_AT) -- 38ms is 38 times the whole budget, so the
   C64 rightly reports DEVICE NOT PRESENT, and that is what made writing
   into a mounted D64 impossible.

   Interrupts therefore have to stay ON through the erase, and the only
   code reachable while they are must live in RAM. That is now true of
   every piece: the vector table (the SDK relocates it to
   ram_vector_table), flash_range_erase() itself, iec_atn_handler(), and
   the ATN-only ISR below.

   It cannot be done by simply installing a permanent exclusive handler.
   IO_IRQ_BANK0 is shared with the Wi-Fi chip:

     cyw43_driver.c  gpio_set_irq_enabled(CYW43_PIN_WL_HOST_WAKE,
                                          GPIO_IRQ_LEVEL_HIGH, enabled)

   Displacing cyw43's handler leaves its LEVEL-triggered interrupt
   asserted for ever, which re-enters the ISR until nothing else runs --
   observed as a device that booted fine and wedged the moment the radio
   had traffic. Chaining does not help either, because the SDK's chain
   calls every handler on the line and cyw43's is in flash.

   So the takeover is transient and covers the erase only: mask every
   interrupt source whose handler lives in flash, run the erase, put them
   all back. Everything here except the erase runs with XIP available, so
   ordinary SDK calls are fine.
---------------------------------------------------------------------------*/

/* ATN interrupts actually taken, so "did it fire" stays a reading rather
   than an assumption -- last time that was assumed the NVIC line had
   never been enabled and the true answer was zero. /api/stats. */
volatile uint32_t iec_atn_irqs;

/* Used ONLY while the shield is up; the normal path keeps the SDK's
   shared handler so cyw43 still works. */
static void __not_in_flash_func(iec_atn_only_isr)(void) {
  io_bank0_irq_ctrl_hw_t *ctrl = &io_bank0_hw->proc0_irq_ctrl;
  const uint     atn_idx = IEC_PIN_ATN >> 3;
  const uint32_t atn_m   = 0xfu << (4 * (IEC_PIN_ATN & 7));

  if (ctrl->ints[atn_idx] & atn_m) {
    io_bank0_hw->intr[atn_idx] = atn_m;   /* clear the edge */
    iec_atn_irqs++;
    iec_atn_handler();                    /* RAM-resident, see iec.c */
  }

  /* Anything else on this line has a handler in flash and cannot be
     serviced right now. Silence just that pin rather than leaving it
     asserted: cyw43's host-wake is LEVEL triggered, so returning without
     either servicing or masking it re-enters this ISR for ever and the
     erase never finishes.
     flash_shield_end() restores inte, and a level that is still asserted
     then re-fires immediately, so the radio is serviced as soon as the
     erase is over. */
  for (uint i = 0; i < 6; i++) {
    uint32_t pending = ctrl->ints[i];
    if (i == atn_idx)
      pending &= ~atn_m;
    if (pending)
      ctrl->inte[i] &= ~pending;
  }
}

#define SHIELD_INTE_WORDS 6          /* io_bank0 inte[6] on RP2350 */

static uint32_t      shield_inte[SHIELD_INTE_WORDS];
static uint8_t       shield_nvic[NUM_IRQS];
static uint32_t      shield_systick;
static irq_handler_t shield_vector;
static bool          shield_up;

void flash_shield_begin(void) {
  io_bank0_irq_ctrl_hw_t *ctrl = &io_bank0_hw->proc0_irq_ctrl;
  const uint     idx = IEC_PIN_ATN >> 3;
  const uint32_t m   = 0xfu << (4 * (IEC_PIN_ATN & 7));

  /* 1. Save inte so it can be put back, but do NOT pre-mask anything.
        An earlier version cleared every pin except ATN for the whole
        erase, which left the Wi-Fi chip unserviced for ~40ms on EVERY
        erase whether or not it had anything to say. The ISR now silences
        a pin only if it actually fires, so the radio is blind only for
        the tail of an erase it happened to coincide with, instead of all
        of them. */
  for (uint i = 0; i < SHIELD_INTE_WORDS; i++)
    shield_inte[i] = ctrl->inte[i];
  (void)idx; (void)m;

  /* 2. Point the (RAM) vector straight at the RAM-resident ISR. */
  shield_vector = irq_get_vtable_handler(IO_IRQ_BANK0);
  ((irq_handler_t *)(uintptr_t)scb_hw->vtor)[16 + IO_IRQ_BANK0] = iec_atn_only_isr;
  __asm volatile ("dsb" ::: "memory");
  __asm volatile ("isb" ::: "memory");

  /* 3. Every other NVIC line, and SysTick, have handlers in flash. */
  for (uint i = 0; i < NUM_IRQS; i++) {
    shield_nvic[i] = irq_is_enabled(i) ? 1 : 0;
    if (i != IO_IRQ_BANK0 && shield_nvic[i])
      irq_set_enabled(i, false);
  }
  shield_systick = systick_hw->csr;
  systick_hw->csr = shield_systick & ~(1u << 1);   /* TICKINT off */

  shield_up = true;
}

void flash_shield_end(void) {
  if (!shield_up)
    return;
  shield_up = false;

  systick_hw->csr = shield_systick;
  for (uint i = 0; i < NUM_IRQS; i++)
    if (i != IO_IRQ_BANK0 && shield_nvic[i])
      irq_set_enabled(i, true);

  ((irq_handler_t *)(uintptr_t)scb_hw->vtor)[16 + IO_IRQ_BANK0] = shield_vector;
  __asm volatile ("dsb" ::: "memory");
  __asm volatile ("isb" ::: "memory");

  io_bank0_irq_ctrl_hw_t *ctrl = &io_bank0_hw->proc0_irq_ctrl;
  for (uint i = 0; i < SHIELD_INTE_WORDS; i++)
    ctrl->inte[i] = shield_inte[i];
}

void iec_interrupts_init(void) {
  /* Registers the shared callback via IEC_PIN_ATN but leaves both
     pins' interrupts disabled -- set_atn_irq()/set_clock_irq()
     (arch-config.h) independently enable/disable each afterward. */
  gpio_set_irq_enabled_with_callback(IEC_PIN_ATN, 0, false, &gpio_irq_dispatch);
  gpio_set_irq_enabled(IEC_PIN_CLOCK, GPIO_IRQ_EDGE_FALL, false);

  /* MUST be here: gpio_set_irq_enabled_with_callback() only enables the
     NVIC IO_IRQ_BANK0 line when its `enabled` argument is true (SDK
     hardware_gpio/gpio.c: "if (enabled) irq_set_enabled(...)"), and we
     deliberately pass false above so the two pins start masked. The
     later set_atn_irq()/set_clock_irq() calls use gpio_set_irq_enabled(),
     which flips only the *per-pin* INTE bit -- neither of them ever
     enables the NVIC line, so without this the ATN interrupt can never
     reach the CPU at all.

     Confirmed on real hardware 2026-09-04: an ISR counter read exactly 0
     ATN interrupts since boot, while the BUS_IDLE wait loop ran at ~102
     iterations/2s -- i.e. purely the 10ms systick waking it out of
     system_sleep()'s wfi. A C64 expects a response within ~1ms, so at
     10ms polling granularity ATN comes and goes entirely between two
     polls and the drive never answers ("DEVICE NOT PRESENT"). This was
     masked for a long time: while ATN happened to read asserted at idle,
     the wait loop exited before ever reaching wfi and thus spun fast
     enough to catch the bus by polling alone. */
  irq_set_enabled(IO_IRQ_BANK0, true);
}

/* --- Sleep / reset ---
   Deliberately no panic(const char *) helper of our own here: nothing
   in the shared core actually calls one (grep confirms panic() is
   purely an ESP8266-arch-internal convention, not a shared symbol from
   system.h), and Pico SDK's own pico/platform_panic.h already declares
   a variadic "void panic(const char *fmt, ...)" (pulled in transitively
   via pico/stdlib.h) -- defining a second, differently-signatured
   panic() here would conflict with it exactly like the uart_putc/
   uart_getc/uart_init collisions documented in uart_hw_shim.h. If a
   future phase wants a UART-based fatal-error helper, give it a
   distinct name instead of reusing "panic". */

/* Stack painting, so "how much stack does this actually use" is a
   measurement rather than a guess.

   The SDK default is 2048 bytes per core. littlefs is not shy with stack
   -- lfs_dir_commit()/lfs_dir_compact() nest several frames deep, each
   with its own block-sized locals -- and writing into a mounted D64 adds
   d64ops on top of that, then lfs_file_sync() on a 174KB file underneath.
   A reset with no watchdog armed means a fault, and blowing a 2KB stack
   is the obvious candidate. Reported by /api/stats. */
extern char __StackBottom, __StackTop;
#define STACK_PAINT 0xc5u

void stack_paint(void) {
  volatile uint8_t *p = (uint8_t *)&__StackBottom;
  uint8_t *sp;
  __asm volatile ("mov %0, sp" : "=r" (sp));
  /* Leave this frame alone; paint everything below it. */
  while ((uintptr_t)p < (uintptr_t)sp - 64u)
    *p++ = STACK_PAINT;
}

uint32_t stack_high_water(void) {
  const uint8_t *p = (const uint8_t *)&__StackBottom;
  const uint8_t *top = (const uint8_t *)&__StackTop;
  while (p < top && *p == STACK_PAINT)
    p++;
  return (uint32_t)(top - p);          /* bytes ever used */
}

uint32_t stack_total(void) {
  return (uint32_t)((const uint8_t *)&__StackTop - (const uint8_t *)&__StackBottom);
}

/* Fault capture that survives the reset.
 *
 * Writing into a mounted D64 reset the board with no watchdog armed --
 * i.e. a fault -- and the reset takes the USB console with it, so the
 * evidence disappears exactly when it is needed. The watchdog scratch
 * registers survive a soft reset, so the handler records where it died
 * and boot reports it through /api/stats.
 */
#define FAULT_MAGIC 0xFA0170ADu

static uint32_t g_fault[4];

void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile (
    "movs r0, #4          \n"
    "mov  r1, lr          \n"
    "tst  r0, r1          \n"
    "beq  1f              \n"
    "mrs  r0, psp         \n"
    "b    2f              \n"
  "1: mrs  r0, msp        \n"
  "2: ldr  r1, [r0, #24]  \n"   /* stacked PC  */
    "ldr  r2, [r0, #20]   \n"   /* stacked LR  */
    "b    hardfault_record\n"
  );
}

void hardfault_record(uint32_t sp, uint32_t pc, uint32_t lr) {
  watchdog_hw->scratch[0] = FAULT_MAGIC;
  watchdog_hw->scratch[1] = pc;
  watchdog_hw->scratch[2] = lr;
  watchdog_hw->scratch[5] = sp;
  watchdog_reboot(0, 0, 0);
  while (1) tight_loop_contents();
}

/* Called early in main(), before anything can fault again. */
/* Boots since power-on. Watchdog scratch survives a reset but not a power
   cycle, so this counts resets rather than boots -- which is exactly what
   is wanted to tell "the watchdog recovered us" from "nothing happened". */
static uint32_t g_boots;
#define BOOTCOUNT_MAGIC 0xB0071234u

uint32_t boot_count(void) { return g_boots; }

/* So "is the watchdog actually armed" is a reading, not a belief. */
uint32_t watchdog_ctrl_raw(void) { return watchdog_hw->ctrl; }

void boot_count_init(void) {
  if (watchdog_hw->scratch[3] >> 8 == BOOTCOUNT_MAGIC >> 8)
    g_boots = (watchdog_hw->scratch[3] & 0xff) + 1;
  else
    g_boots = 1;
  watchdog_hw->scratch[3] = (BOOTCOUNT_MAGIC & ~0xffu) | (g_boots & 0xff);
}

void fault_check_previous(void) {
  /* NOT scratch[4]: the SDK's watchdog_enable() writes
     WATCHDOG_NON_REBOOT_MAGIC there, so a fault record kept in it is
     overwritten the moment the watchdog is armed. */
  if (watchdog_hw->scratch[0] == FAULT_MAGIC) {
    g_fault[0] = 1;
    g_fault[1] = watchdog_hw->scratch[1];
    g_fault[2] = watchdog_hw->scratch[2];
    g_fault[3] = watchdog_hw->scratch[5];
    watchdog_hw->scratch[0] = 0;
  }
}

uint32_t fault_last(unsigned int which) {
  return (which < 4) ? g_fault[which] : 0;
}

/* Same idea as the fault handler above, for a hang investigation instead
   of a crash: real values from right before a hang need to survive the
   watchdog reset that recovers from it, or the diagnostic is gone
   before it can ever be read over HTTP. scratch[6] -- [0]/[1]/[2]/[5]
   are the fault record above, [3] is the boot counter, [4] is the SDK's
   own WATCHDOG_NON_REBOOT_MAGIC (see fault_check_previous()'s comment).
   Originally held the worst single littlefs read duration this boot;
   repurposed 2026-09-07 (Phase B1) once that
   stat had answered its question three times over -- the read itself is
   fast (47-49us), not the bottleneck. Now packs two values instead of
   one: refill_max_us (src/iec.c's buf->refill(buf) call, upper 16 bits)
   and findbuf_max_us (the find_buffer() call right after it, lower 16
   bits), splitting the still-unexplained ~280us put-gap (below) between
   "inside the refill" and "after it returns". */
#define REFILL_SCRATCH_IDX 6
/* scratch[7]: the last one free -- [0]/[1]/[2]/[5] are the fault record
   above, [3] is the boot counter, [4] is the SDK's own
   WATCHDOG_NON_REBOOT_MAGIC (see fault_check_previous()'s comment).
   Repurposed 2026-09-07 from a first breadcrumb (the file-offset of the
   last lfsop_file_read() attempted this boot) once that one had done its
   job -- confirmed twice, byte-for-byte, that a 4K-load hang's last read
   is at offset 4064 (exactly 16*254, the refill that first crosses the
   littlefs block boundary) and that the read itself is fast (49us). Held
   the worst gap seen before an iec_putc() send next.

   Repurposed again, same day: a real hang showed refill_phase_prev_boot
   coming back IDLE, ruling out buf->refill(buf)/find_buffer() as the
   site of *that* particular hang -- which means it's most likely inside
   _iec_putc()'s own handshake, in one of its five unbounded
   `do { if (ATN) return; } while (!condition)` loops (Phase B2). Same blind spot as before: a duration is
   only recorded once its loop exits, so if the loop itself never exits,
   its identity -- not its duration -- is the only thing worth keeping.
   Packed putc_loop_id (4 bits, upper) alongside put_gap_max_us
   (28 bits, lower) until 2026-09-07's tx_position result (see
   iec_refill_scratch_note() below): two files of very different sizes
   died at the identical byte offset under PIO, and a different offset
   under software, clustering near littlefs's 4096-byte block boundary
   without hard-triggering at one fixed byte -- the standing theory
   being that littlefs's own behaviour changes *approaching* that
   boundary, not just when a read spans across it, plausibly in the
   lfs_file_tell()/lfs_file_size() metadata calls lfsop_file_read()'s
   EOF-fixup makes (src/lfsops.c's lfsread_pre_max_us/
   lfsread_post_max_us, added Phase B1 -- current-boot only until now).
   put_gap_max_us shrunk to 16 bits (still ample: observed values are
   under 1000us) to make room for the two lfsread timings at 6 bits each
   (0-63us -- ample: observed values are single digits). Unlike the
   *_max_us fields elsewhere, these two are unconditionally OVERWRITTEN
   each call, not maxed: the question here is "what did the most recent
   refill's read look like", the one immediately before whatever the
   transfer does next, not "what was the worst ever this boot". */
#define PUTC_SCRATCH_IDX 7

static uint32_t g_iec_refill_max_us_prev_boot;
static uint32_t g_iec_tx_position_prev_boot;
static uint32_t g_iec_refill_phase_prev_boot;
static uint32_t g_iec_put_gap_max_us_prev_boot;
static uint32_t g_iec_putc_loop_prev_boot;
static uint32_t g_lfsread_pre_last_us_prev_boot;
static uint32_t g_lfsread_post_last_us_prev_boot;

void hang_diag_scratch_init(void) {
  uint32_t packed = watchdog_hw->scratch[REFILL_SCRATCH_IDX];
  g_iec_refill_max_us_prev_boot = packed >> 16;
  g_iec_tx_position_prev_boot = (packed >> 2) & 0x3fffu;
  g_iec_refill_phase_prev_boot = packed & 0x3u;

  uint32_t putc_packed = watchdog_hw->scratch[PUTC_SCRATCH_IDX];
  g_iec_putc_loop_prev_boot = putc_packed >> 28;
  g_iec_put_gap_max_us_prev_boot = (putc_packed >> 12) & 0xffffu;
  g_lfsread_pre_last_us_prev_boot = (putc_packed >> 6) & 0x3fu;
  g_lfsread_post_last_us_prev_boot = putc_packed & 0x3fu;

  watchdog_hw->scratch[REFILL_SCRATCH_IDX] = 0;
  watchdog_hw->scratch[PUTC_SCRATCH_IDX] = 0;
}

/* Called from iec_talk_handler() (src/iec.c), which is IRAM_ATTR --
   same reasoning as iec_put_gap_scratch_note() below: this has to be
   RAM-resident too so timing it doesn't itself cost a cold-cache flash
   fetch.

   Packing, in a 32-bit word: refill_max_us (16 bits, unchanged range),
   tx_position (14 bits, 0-16383 bytes -- ample for anything tested so
   far) and a 2-bit phase.

   tx_position replaces an earlier findbuf_max_us duration (2026-09-07):
   that stat turned out uninformative -- always single-digit
   microseconds, on both successful and crashing boots, so it never
   distinguished anything. What a crash actually needed to answer was
   "how far into the transfer did this get" -- a real hang left
   refill_max_us/put_gap_max_us looking perfectly normal (tens to a few
   hundred us) while iec_pio_tx_byte()'s own timeout (Phase D2) never
   fired despite being armed, which rules out one single call blocking
   for the full 8s watchdog window but doesn't say what *did* consume
   it. tx_position is iec_stat_tx_bytes at the moment of the call --
   unlike the durations, it is monotonic within one transfer, so it is
   always OVERWRITTEN, not maxed: "the latest position we reached"
   is the useful answer, not "the largest position ever" (which a
   multi-file boot could put in a different, misleading, file).

   The phase field exists for the same reason as before: a *duration* is
   only ever recorded after its call returns, so a call that never
   returns leaves the fields as of the LAST write going INTO it --
   phase says whether that was "about to refill", "about to find the
   buffer", or neither, and tx_position says how far the transfer had
   gotten by then. REFILL_PHASE_* constants live in system.h, shared
   with the call sites in src/iec.c. */
IRAM_ATTR
void iec_refill_scratch_note(uint32_t refill_us, uint32_t tx_position, uint8_t phase) {
  uint32_t packed = watchdog_hw->scratch[REFILL_SCRATCH_IDX];
  uint32_t cur_refill = packed >> 16;
  if (refill_us > cur_refill) cur_refill = (refill_us > 0xffffu) ? 0xffffu : refill_us;
  uint32_t pos = (tx_position > 0x3fffu) ? 0x3fffu : tx_position;
  watchdog_hw->scratch[REFILL_SCRATCH_IDX] =
    (cur_refill << 16) | (pos << 2) | (phase & 0x3u);
}

uint32_t iec_refill_max_us_prev_boot(void) {
  return g_iec_refill_max_us_prev_boot;
}

uint32_t iec_tx_position_prev_boot(void) {
  return g_iec_tx_position_prev_boot;
}

uint32_t iec_refill_phase_prev_boot(void) {
  return g_iec_refill_phase_prev_boot;
}

/* Called from iec_putc() (src/iec.c), which is IRAM_ATTR -- timing
   this precise cannot afford to fetch this function's own code from
   flash on a cold cache line, so it has to be RAM-resident too. A plain
   watchdog scratch write costs nothing beyond that either way: it is a
   memory-mapped register, not a flash access. Preserves the loop-id
   nibble and the two lfsread fields -- read-modify-write, same
   reasoning as iec_refill_scratch_note(). */
IRAM_ATTR
void iec_put_gap_scratch_note(uint32_t us) {
  uint32_t packed = watchdog_hw->scratch[PUTC_SCRATCH_IDX];
  uint32_t cur_gap = (packed >> 12) & 0xffffu;
  if (us > cur_gap) {
    cur_gap = (us > 0xffffu) ? 0xffffu : us;
    watchdog_hw->scratch[PUTC_SCRATCH_IDX] =
      (packed & 0xf0000fffu) | (cur_gap << 12);
  }
}

uint32_t iec_put_gap_max_us_prev_boot(void) {
  return g_iec_put_gap_max_us_prev_boot;
}

/* Phase B2: _iec_putc() has five unbounded
   `do { if (iec_check_atn()) return -1; } while (!condition)` loops
   (E925, E937, E941, the post-clock-low wait, and tx_done) -- any of
   which could be where an 8s hang actually lives, since none of them
   have any exit besides ATN or the condition becoming true. Called on
   entry to each, with PUTC_LOOP_IDLE once it exits normally, so a hang
   leaves behind which one it was in. IRAM_ATTR for the same reason as
   every other function called from _iec_putc(): it must not cost a
   flash fetch. Preserves the put_gap and lfsread fields in the low 28
   bits. */
IRAM_ATTR
void iec_putc_loop_note(uint8_t loop_id) {
  uint32_t packed = watchdog_hw->scratch[PUTC_SCRATCH_IDX];
  watchdog_hw->scratch[PUTC_SCRATCH_IDX] =
    ((uint32_t)loop_id << 28) | (packed & 0x0fffffffu);
}

uint32_t iec_putc_loop_prev_boot(void) {
  return g_iec_putc_loop_prev_boot;
}

/* Phase B1 continuation (2026-09-07): the
   tx_position result (iec_refill_scratch_note() above) points at
   littlefs behaving differently *approaching* its 4096-byte block
   boundary, not just when a read spans across it -- plausibly in the
   lfs_file_tell()/lfs_file_size() metadata calls src/lfsops.c's
   lfsop_file_read() makes around the read itself (already timed as
   lfsread_max_us, current-boot only, confirmed fast). These two survive
   a crash where that current-boot stat doesn't. Unconditionally
   overwritten, not maxed -- see the comment on PUTC_SCRATCH_IDX above
   for why "the most recent refill's read" is the useful question here,
   not "the worst ever this boot". Called from lfsop_file_read(), which
   is NOT RAM-resident (it's reached via buf->refill(buf), timed as a
   whole from iec_talk_handler(), an already-accepted flash call) so
   this one is not itself IRAM_ATTR either -- consistent with everything
   else in that function. */
void lfsread_prepost_scratch_note(uint32_t pre_us, uint32_t post_us) {
  uint32_t packed = watchdog_hw->scratch[PUTC_SCRATCH_IDX];
  uint32_t pre  = (pre_us  > 0x3fu) ? 0x3fu : pre_us;
  uint32_t post = (post_us > 0x3fu) ? 0x3fu : post_us;
  watchdog_hw->scratch[PUTC_SCRATCH_IDX] =
    (packed & 0xfffff000u) | (pre << 6) | post;
}

uint32_t lfsread_pre_last_us_prev_boot(void) {
  return g_lfsread_pre_last_us_prev_boot;
}

uint32_t lfsread_post_last_us_prev_boot(void) {
  return g_lfsread_post_last_us_prev_boot;
}

/* Hang recovery.
 *
 * If core 0 stops making progress there is currently no way back: core 1
 * is parked in the flash lockout waiting for a flash_lockout_end() that
 * never comes, the bus is dead, and the web UI is gone. That happened
 * three times while the flash shield was being developed and each needed
 * a BOOTSEL reflash -- fine on a bench, useless in a machine.
 *
 * Letting core 1 release ITSELF is not the answer, however tempting: it
 * is parked precisely so it cannot execute from XIP while an erase has
 * the window closed, so a self-releasing victim would fault instead of
 * hang. Resetting the whole device is the recoverable outcome, and it is
 * safe to do at any moment because littlefs is power-fail safe by
 * construction -- an interrupted write leaves the previous version
 * intact.
 *
 * Petted on progress rather than on a timer: the mainloop, the bridge,
 * each flash and image operation, and each byte moved across the IEC
 * bus. A D81 full format is thousands of sector writes and runs for
 * minutes without returning to the mainloop, so a timer-based pet would
 * either reset mid-format or be too long to be useful.
 *
 * That last entry -- the per-byte pets in iec.c's iec_talk_handler()
 * and iec_listen_handler() -- was missing until 2026-09-09 and is the
 * reason this list must be kept honest. A transfer handler owns the bus
 * until its last byte and never reaches the mainloop's pet; a plain
 * sequential littlefs read is neither a flash write nor an image op, so
 * it hit no pet site at all. The watchdog was therefore timing the
 * *duration of the transfer*, and killed a perfectly healthy drive at
 * 8 s x ~520 B/s ~= 4.1 KB. It presented for two days as a mysterious
 * ">4KB LOAD hang" with a suspiciously round size threshold; the file
 * size never mattered, only the clock.
 *
 *
 * The rule to apply when adding any new long-running path: if it can
 * run for 8 s without returning to iec_mainloop(), it needs its own pet
 * on a real unit of progress -- and the pet must sit *after* the work,
 * so that a path which stalls instead of progressing still resets. */
#define WATCHDOG_TIMEOUT_MS 8000

void system_watchdog_start(void) {
  /* pause_on_debug = 0: with it set the watchdog stops whenever the chip
     believes a debugger is attached, which is not a state this device can
     usefully depend on. */
  watchdog_enable(WATCHDOG_TIMEOUT_MS, 0);
}

void system_watchdog_pet(void) {
  watchdog_update();
}

void system_sleep(void) {
  __asm volatile ("wfi" ::: "memory");
}

__attribute__((noreturn)) void system_reset(void) {
  uart_flush();
  watchdog_reboot(0, 0, 0);
  while (1) {
    tight_loop_contents();
  }
}
