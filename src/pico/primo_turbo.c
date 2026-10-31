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


   primo_turbo.c: the drive's half of the Primo turbo. See primo_turbo.h.

   SHAPE. The Primo sets the pace and the drive gives the orders. The PIO
   program (primo_turbo.pio) answers every CLOCK edge by itself; this code
   feeds it a word per edge and decides only at the "holds", where the PIO
   waits for it: H (the Primo has sent a frame number) and Q (the Primo has
   sent a write). `order()` sees one order through all the protocol's cases
   -- resend, refusal, a duplicate write, a write lost to a misread verdict
   -- and the brain above it (load, verify, save) is straight-line code that
   reads the file through sd2iec's own buffers, as the talk and listen
   handlers do.

   Everything here is a port of `Drive` in tests/primo_turbo_sim.py, where
   it was run against the Primo's real code with faults injected. Keep the
   two in step.

   INVARIANTS. This runs from iec.c's BUS_CLEANUP, never from
   the ATN interrupt. While the PIO holds DATA, iec_pio_pins_held is set, so
   iec_atn_handler() takes DATA back through iec_pio_atn_reclaim() before
   answering ATN, exactly as during a PIO transmit: T_AT is untouched. On
   ATN this code gives up at once and leaves DATA to the interrupt. No
   console output: counters only.
*/

#include <string.h>
#include "config.h"
#include "buffers.h"
#include "doscmd.h"
#include "errormsg.h"
#include "fileops.h"
#include "flags.h"
#include "system.h"
#include "iec_pio.h"
#include "netconfig.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/structs/io_bank0.h"
#include "primo_turbo.h"
#include "primo_turbo.pio.h"

_Static_assert(IEC_PIN_CLOCK == 3, "primo_turbo.pio waits on gpio 3");

volatile uint8_t primo_turbo_enabled = 0;
volatile primo_turbo_stats_t primo_turbo_stats;
volatile int8_t   primo_turbo_last_reason;
volatile uint32_t primo_turbo_last_edges;
volatile primo_turbo_selftest_t primo_turbo_selftest;
volatile uint8_t primo_turbo_selftest_req;

/* Breadcrumbs that survive a watchdog reset: RAM the SDK does not clear.
   A session writes its phase here at each step; after a restart
   /api/stats shows where the last one was (pt_prev_*). The watchdog's
   scratch registers are all taken (system.c). */
#define CRUMB_MAGIC 0x5054u            /* "PT" */
typedef struct { uint32_t magic, phase, cur, aux, t, sp, sp_min, deep, deep_n, deep_t; } pt_crumb_t;
static pt_crumb_t __uninitialized_ram(crumb);
volatile uint32_t primo_turbo_prev[9];  /* phase, frame, detail, valid, SP, lowest SP,
                                           deep step, deep marks, us since the phase */
#define CRUMB(p, a) do { uint32_t sp_ = (uint32_t)(uintptr_t)__builtin_frame_address(0); \
    crumb.magic = CRUMB_MAGIC; crumb.phase = (p); crumb.cur = cur; crumb.aux = (a); \
    crumb.t = time_us_32(); crumb.sp = sp_; crumb.deep = crumb.deep_n = crumb.deep_t = 0; \
    if (sp_ < crumb.sp_min) crumb.sp_min = sp_; } while (0)

/* The step below a phase: file_open, littlefs and the flash write path
   mark where they are (primo_turbo.h), so a reset inside file_open says
   inside what. Only written while a crumb is live. */
void primo_turbo_deep(uint32_t step) {
  if (crumb.magic != CRUMB_MAGIC)
    return;
  crumb.deep = step;
  crumb.deep_n++;
  crumb.deep_t = time_us_32() - crumb.t;
}

#define W_REL   0u
#define W_PULL  1u
#define W_HOLD  2u
#define REQ_LEN 17              /* 16 edges for the number, then the H hold */
#define IDLE_RESYNC_US 2000     /* the Primo waits 3 ms; see xfer.asm */
#define SILENCE_US 15000000u    /* the Primo's 8 tries x 1.3 s, and some  */
#define RETRIES 8

static PIO  pio;
static uint sm, offset;
static bool ready;

/* the session armed by XT: 1 = its own transaction, 2 = run at the next */
static uint8_t  armed;
static uint8_t  s_idx;
static uint16_t s_hl, s_de, s_bc;

/* the words for the PIO, the samples back from it */
static uint8_t  q[2 + 8 * (4 + 255 + 2) + 1 + REQ_LEN + 8];
static uint16_t qlen, qpos;
static uint32_t edges_q, edges_s;           /* edge words queued, samples seen */
static uint8_t  samp[8 * 255 + 16 + 64];
static uint16_t nsamp;                      /* samples since the last decision */
static uint32_t last_act, last_edge;
static bool     got_go;                     /* the Primo's first edge has come */

static uint8_t  cur;                        /* the frame in hand */
static int16_t  last_write = -1;            /* the last stored write's number */
static uint8_t  last_write_len;
static uint8_t  req;                        /* the number the Primo asked for */

/* The session's buffers, static rather than on the stack: a session calls
   sd2iec's file code and littlefs from inside BUS_CLEANUP, and littlefs's
   commit/compact paths are deep (scripts/pico/CMakeLists.txt). With these
   ~1.1 KB on core 0's 4 KB stack the first real turbo save reset the drive
   inside file_open(1) (2026-10-05). Never used re-entrantly. */
static uint8_t  s_data[257];                /* receive_write */
static uint8_t  s_body[4 + 255];            /* order */
static uint8_t  s_name[CONFIG_COMMAND_BUFFER_SIZE];
static uint8_t  s_chunk[255];               /* load/save: a chunk of the file */
static uint8_t  s_mem[255];                 /* verify: the Primo's bytes */
static bool     req_ok;                     /* ... and whether it passed */

/* ------------------------------------------------------------------ */

bool primo_turbo_init(void) {
  if (crumb.magic == CRUMB_MAGIC) {     /* left by a session the reset ended */
    primo_turbo_prev[0] = crumb.phase;
    primo_turbo_prev[1] = crumb.cur;
    primo_turbo_prev[2] = crumb.aux;
    primo_turbo_prev[3] = 1;
    primo_turbo_prev[4] = crumb.sp;
    primo_turbo_prev[5] = crumb.sp_min;
    primo_turbo_prev[6] = crumb.deep;
    primo_turbo_prev[7] = crumb.deep_n;
    primo_turbo_prev[8] = crumb.deep_t;
  }
  crumb.magic = 0;
  if (ready)
    return true;
  if (!pio_claim_free_sm_and_add_program_for_gpio_range(
          &primo_turbo_program, &pio, &sm, &offset, IEC_PIN_DATA, 1, false))
    return false;
  primo_turbo_program_init(pio, sm, offset, IEC_PIN_DATA);
  pio_sm_set_pins_with_mask(pio, sm, 0, 1u << IEC_PIN_DATA);
  ready = true;
  return true;
}

bool primo_turbo_ready(void) {
  return ready;
}

void primo_turbo_apply_setting(void) {
  primo_turbo_enabled = ready && (globalflags & PRIMO_TURBO);
}

static void set_error_text(const char *t) {
  set_error(ERROR_OK);
  size_t n = strlen(t);
  memset(error_buffer, 0, sizeof(error_buffer));
  memcpy(error_buffer, t, n);
  error_buffer[n] = 13;
  buffers[ERRORBUFFER_IDX].lastused = n;
}

void primo_turbo_command(void) {
  /* `XT+` / `XT-`: turn the turbo on or off, and the flag `XW` saves
     (primo_turbo.h). Until 2026-10-07 the switch was never saved -- the
     user's choice of 2026-10-05, that it follow the DOS that is loaded;
     changed for our own DOS, which needs the turbo. */
  if (command_length == 3 && (command_buffer[2] == '+' || command_buffer[2] == '-')) {
    if (command_buffer[2] == '+' && !ready) {
      set_error(ERROR_SYNTAX_UNKNOWN);
      return;
    }
    primo_turbo_enabled = command_buffer[2] == '+';
    if (primo_turbo_enabled)
      globalflags |= PRIMO_TURBO;
    else
      globalflags &= (uint8_t)~PRIMO_TURBO;
    /* `XT+` is what our DOS's boot loader sends first. Its answer says
       whether PRIFLOP.CFG asks for PFM at boot: `00, OK,01,00` with
       PFM_AUTOSTART=ON, read from the file now (PFM may have changed it
       since the drive started), else `00, OK,00,00` as before. The loader
       then starts PFM (tools/primo/dos/loader.asm); an older loader looks only at the 00. */
    if (primo_turbo_enabled && netconfig_pfm_autostart() == 1)
      set_error_ts(ERROR_OK, 1, 0);
    else
      set_error(ERROR_OK);
    return;
  }
  /* `XT`, the service index, HL, DE, BC, `.` (turbo.asm's xtbuf) */
  if (!primo_turbo_enabled || !ready || command_length != 10 || command_buffer[9] != '.') {
    set_error(ERROR_SYNTAX_UNKNOWN);
    return;
  }
  s_idx = command_buffer[2];
  s_hl  = command_buffer[3] | command_buffer[4] << 8;
  s_de  = command_buffer[5] | command_buffer[6] << 8;
  s_bc  = command_buffer[7] | command_buffer[8] << 8;
  armed = 1;
  set_error_text("00,TURBO,01,00");
}

/* ------------------------------------------------------------------ */
/* The PIO side */

static inline void q_reset(void) { qlen = qpos = 0; }
static inline void q_imm(uint8_t w) { q[qlen++] = w; }              /* no edge */
static inline void q_edge(uint8_t w) { q[qlen++] = w; edges_q++; }
static void q_req(void) {
  for (int i = 0; i < 16; i++)
    q_edge(W_REL);
  q_edge(W_REL | W_HOLD);
}
static void q_bytes(const uint8_t *d, unsigned n) {   /* data, LSB first */
  for (unsigned i = 0; i < n; i++)
    for (int b = 0; b < 8; b++)
      q_edge((d[i] >> b) & 1 ? W_REL : W_PULL);
}

static void pins_take(bool pulled) {
  pio_sm_set_enabled(pio, sm, false);
  pio_sm_clear_fifos(pio, sm);
  pio_sm_restart(pio, sm);
  pio_sm_exec(pio, sm, pio_encode_jmp(offset +
              (gpio_get(IEC_PIN_CLOCK) ? 0 : primo_turbo_offset_released)));
  pio_sm_set_consecutive_pindirs(pio, sm, IEC_PIN_DATA, 1, pulled);
  iec_pio_pins_held = 1;
  io_bank0_hw->io[IEC_PIN_DATA].ctrl = (uint)PIO_FUNCSEL_NUM(pio, IEC_PIN_DATA)
                                       << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
  pio_sm_set_enabled(pio, sm, true);
}

/* Hand DATA back released, unless ATN took it already. */
static void pins_give(void) {
  pio_sm_set_enabled(pio, sm, false);
  if (iec_pio_pins_held) {
    iec_pio_pins_held = 0;
    set_data(1);
    io_bank0_hw->io[IEC_PIN_DATA].ctrl = GPIO_FUNC_SIO << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
  }
}

/* Set while a 'K' order has gone out and the Primo has not yet asked for
   the next frame: it is running the call, which may take any time (a
   PRINT that scrolls the screen: ~70 idle resets), so the resets then are
   counted as call_waits. A K frame lost on the way counts there too; its
   resend still shows in `resent`. */
static bool s_in_call;

/* The idle reset: the Primo has gone quiet, so start over with a request,
   from whatever level CLOCK is at. */
static void resync(void) {
  if (s_in_call)
    primo_turbo_stats.call_waits++;
  else
    primo_turbo_stats.resyncs++;
  edges_q = edges_s = 0;
  nsamp = 0;
  q_reset();
  if (!got_go) {
    /* The Primo has not started yet: keep saying "ready" and wait for its
       go edge, or a slow start would find DATA released and wait 1.3 s. */
    pins_take(true);
    q_edge(W_REL);
  } else {
    pins_take(false);
  }
  q_req();
}

/* Run until the PIO has sampled the last edge queued (a hold).
   0 = at the hold; 1 = resynced, now waiting for a request; <0 = abort. */
static int run_to_hold(void) {
  CRUMB(5, edges_s);                   /* waiting on the Primo */
  uint32_t pet = time_us_32();
  last_act = time_us_32();
  for (;;) {
    while (qpos < qlen && !pio_sm_is_tx_fifo_full(pio, sm))
      pio_sm_put(pio, sm, q[qpos++]);
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
      uint32_t v = pio_sm_get(pio, sm);
      if (nsamp < sizeof(samp))
        samp[nsamp++] = v >> 31;
      edges_s++;
      got_go = true;
      last_act = last_edge = time_us_32();
    }
    if (edges_s == edges_q && qpos == qlen)
      return 0;
    if (!IEC_ATN)
      return -1;
    uint32_t now = time_us_32();
    if (now - last_edge > SILENCE_US)
      return -2;
    if (now - last_act > IDLE_RESYNC_US) {
      resync();
      last_act = now;
      return 1;
    }
    if (now - pet > 100000) {
      system_watchdog_pet();
      pet = now;
    }
  }
}

static uint8_t bits_byte(const uint8_t *s) {
  uint8_t v = 0;
  for (int b = 0; b < 8; b++)
    v |= (s[b] & 1) << b;
  return v;
}

static void fletcher(const uint8_t *d, unsigned n, uint8_t *s1, uint8_t *s2) {
  for (unsigned i = 0; i < n; i++) {
    *s1 += d[i];
    *s2 += *s1;
  }
}

/* Wait for the Primo's next frame number. <0 = abort. */
static int await_request(void) {
  for (;;) {
    int r = run_to_hold();
    if (r < 0)
      return r;
    if (r == 1)
      continue;                 /* resynced: a request is queued for */
    if (nsamp < REQ_LEN) {
      req_ok = false;
    } else {
      uint8_t n = bits_byte(samp + nsamp - 17), c = bits_byte(samp + nsamp - 9);
      req = n;
      req_ok = (uint8_t)(n ^ c) == 0xFF;
    }
    nsamp = 0;
    q_reset();
    return 0;
  }
}

static void answer_refused(bool garbled) {
  if (garbled) primo_turbo_stats.garbled++; else primo_turbo_stats.refused++;
  q_imm(W_PULL);
  q_edge(W_PULL);               /* G: refused */
  q_req();
}

/* Receive a write of `len` bytes for frame `n` into `out` (if not NULL).
   0 = checked and good, 1 = bad, <0 abort, 2 = resynced meanwhile. */
static int receive_write(uint8_t n, uint8_t len, uint8_t *out) {
  CRUMB(6, len);
  q_imm(W_PULL);                /* ready */
  q_edge(W_REL);                /* G: accepted */
  for (unsigned i = 0; i < 8u * len + 16; i++)
    q_edge(W_REL);
  q_edge(W_REL | W_HOLD);       /* Q */
  int r = run_to_hold();
  if (r)
    return r < 0 ? r : 2;
  uint8_t *data = s_data;
  const uint8_t *s = samp + nsamp - (8u * len + 16) - 1;
  for (unsigned i = 0; i < len + 2u; i++)
    data[i] = bits_byte(s + 8 * i);
  nsamp = 0;
  q_reset();
  uint8_t s1 = n, s2 = 0;
  fletcher(data, len, &s1, &s2);
  bool ok = s1 == data[len] && s2 == data[len + 1];
  if (ok && out)
    memcpy(out, data, len);
  q_imm(W_PULL);                /* ready */
  q_edge(ok ? W_REL : W_PULL);  /* V */
  q_edge(W_REL);                /* D */
  q_req();
  return ok ? 0 : 1;
}

/* One order as frame `cur`, seen through. For 'R', `reply` gets the bytes.
   For 'E' and 'J' the session is over when it returns 0. <0 = abort. */
static int order(uint8_t op, uint16_t addr, const uint8_t *data, uint8_t len, uint8_t *reply) {
  uint8_t *body = s_body;
  unsigned blen = 4;
  body[0] = op;
  body[1] = addr & 0xFF;
  body[2] = addr >> 8;
  body[3] = len;
  if (op == 'W') {
    memcpy(body + 4, data, len);
    blen += len;
  }
  CRUMB(4, (uint32_t)op << 16 | len);
  enum { READ, WRITE, STORED } phase = READ;
  bool sent = false;

  for (;;) {
    if (!req_ok) {
      answer_refused(true);
    } else if (phase == READ && req == cur) {
      if (sent) primo_turbo_stats.resent++;
      sent = true;
      s_in_call = op == 'K';
      uint8_t s1 = cur, s2 = 0;
      fletcher(body, blen, &s1, &s2);
      q_imm(W_PULL);            /* ready */
      q_edge(W_REL);            /* G: accepted */
      q_bytes(body, blen);
      uint8_t ck[2] = { s1, s2 };
      q_bytes(ck, 2);
      q_edge(W_REL);            /* D */
      q_req();
    } else if (phase == READ && req == (uint8_t)(cur + 1)) {
      s_in_call = false;
      cur++;                    /* the Primo has it */
      primo_turbo_stats.exchanges++;
      if (op == 'W') primo_turbo_stats.bytes_to += len;
      if (op == 'E' || op == 'J') {
        q_imm(W_PULL);
        q_edge(W_REL);          /* G of the quit: then leave */
        int r = run_to_hold();
        return r < 0 ? r : 0;
      }
      if (op != 'R')
        return 0;               /* the next order answers this request */
      phase = WRITE;
      continue;                 /* this request is the write in hand */
    } else if (phase == WRITE && req == cur) {
      int r = receive_write(cur, len, reply);
      if (r < 0)
        return r;
      if (r == 0) {
        last_write = cur;
        last_write_len = len;
        cur++;
        primo_turbo_stats.exchanges++;
        primo_turbo_stats.bytes_from += len;
        phase = STORED;
      } else if (r == 1) {
        primo_turbo_stats.bad_writes++;
      }
    } else if (phase == WRITE && req == (uint8_t)(cur + 1)) {
      /* the Primo took "again" for "stored": give the same R order again */
      primo_turbo_stats.reissued++;
      cur = req;
      phase = READ;
      sent = false;
      continue;
    } else if (phase == STORED && req == cur) {
      return 0;
    } else if (last_write >= 0 && req == (uint8_t)last_write) {
      primo_turbo_stats.dup_writes++;
      int r = receive_write(req, last_write_len, NULL);
      if (r < 0)
        return r;
    } else {
      answer_refused(false);
    }
    int r = await_request();
    if (r < 0) {
      s_in_call = false;
      return r;
    }
  }
}

/* ------------------------------------------------------------------ */
/* The brain: what CDOS's own routine would have done, as orders */

static int prim_read(uint16_t addr, unsigned n, uint8_t *out) {
  while (n) {
    uint8_t k = n > 255 ? 255 : n;
    int r = order('R', addr, NULL, k, out);
    if (r < 0)
      return r;
    addr += k;
    out += k;
    n -= k;
  }
  return 0;
}

static buffer_t *fbuf;
static bool fb_taken;           /* every byte up to lastused has been read */

/* One byte of the file, as the talk handler sends them: data[position] up
   to and including data[lastused], then refill unless sendeoi. Not
   `position > lastused`: both are uint8_t, and a full buffer has lastused
   255, so position would wrap to 0 and the same 256 bytes come round
   again (found by tests/primo_turbo_csim.py, 2026-10-05). */
static int fget(void) {
  if (fb_taken) {
    if (fbuf->sendeoi)
      return -1;
    system_watchdog_pet();
    CRUMB(10, fbuf->lastused);         /* reading the file on */
    if (fbuf->refill(fbuf))
      return -2;
    fb_taken = false;
  }
  uint8_t c = fbuf->data[fbuf->position];
  if (fbuf->position == fbuf->lastused)
    fb_taken = true;
  else
    fbuf->position++;
  return c;
}

/* The number of bytes read: n, or fewer where the file ends */
static unsigned fread_n(uint8_t *d, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    int c = fget();
    if (c < 0)
      return i;
    d[i] = c;
  }
  return n;
}

static int fput(uint8_t c) {
  if (fbuf->mustflush) {
    system_watchdog_pet();
    CRUMB(8, 0);                       /* writing the file on */
    if (fbuf->refill(fbuf))
      return -1;
    CRUMB(9, 0);
  }
  fbuf->data[fbuf->position] = c;
  mark_buffer_dirty(fbuf);
  if (fbuf->lastused < fbuf->position)
    fbuf->lastused = fbuf->position;
  fbuf->position++;
  if (fbuf->position == 0)
    fbuf->mustflush = 1;
  return 0;
}

static int open_named(uint8_t secondary) {
  uint8_t nl;
  int r = prim_read(s_hl, 1, &nl);
  if (r < 0)
    return r;
  if (nl > CONFIG_COMMAND_BUFFER_SIZE - 1)
    nl = CONFIG_COMMAND_BUFFER_SIZE - 1;
  uint8_t *name = s_name;
  if (nl && (r = prim_read(s_hl + 1, nl, name)) < 0)
    return r;
  memcpy(command_buffer, name, nl);
  memset(command_buffer + nl, 0, sizeof(command_buffer) - nl);
  command_length = nl;
  set_error(ERROR_OK);
  CRUMB(7, secondary);                 /* opening */
  file_open(secondary);
  CRUMB(13, current_error);            /* opened (or refused) */
  command_length = 0;
  fbuf = find_buffer(secondary);
  fb_taken = false;
  return fbuf ? 0 : 1;
}

static int load(bool verify) {
  int r = open_named(0);
  if (r)
    return r < 0 ? r : order('E', 0, NULL, 0, NULL);

  uint8_t *b = s_chunk, w[2];
  if ((r = prim_read(0x4174, 2, w)) < 0) return r;
  uint16_t base = w[0] | w[1] << 8;
  if ((r = prim_read(base + 0x0B8F, 2, w)) < 0) return r;
  uint32_t limit = w[0] | w[1] << 8;
  uint8_t scr;
  if ((r = prim_read(0x404A, 1, &scr)) < 0) return r;

  /* A file that ends before its 0xC9 is 99: A = 0x63 on the
     Primo, CDOS's code for an EOI inside a record, and the bytes that came
     are stored. CDOS has a bus error inside a header or with no 0xC9. Until
     2026-10-10 such a load ended with no error at the last whole chunk. */
  uint8_t mask = s_bc & 0xFF, last = 0, mism = 0;
  uint16_t end = s_de;
  bool early = false;
  for (;;) {
    int t = fget();
    if (t < 0) {
      early = true;
      break;
    }
    if (t == 0xC9)
      break;
    if (t == 0xC3 || t == 0xCD) {
      if (fread_n(w, 2) < 2) {
        early = true;
        break;
      }
      uint16_t a = w[0] | w[1] << 8;
      if (!verify && t == 0xC3 && (mask & 8)) {
        cleanup_and_free_buffer(fbuf);
        return order('J', a, NULL, last, NULL);   /* returned to CDOS-style */
      }
      if (!verify && t == 0xCD && (mask & 16) && (r = order('K', a, NULL, 0, NULL)) < 0)
        return r;
      continue;
    }
    uint8_t h[4];
    if (fread_n(h, 4) < 4) {
      early = true;
      break;
    }
    uint16_t off = h[0] | h[1] << 8, ln = h[2] | h[3] << 8;
    uint8_t bit = t == 0xD9 ? 1 : t == 0xD1 ? 2 : t == 0xD5 ? 4 : 0;
    bool use = bit && (mask & bit);
    uint32_t dest = t == 0xD1 ? (uint32_t)s_de + off : t == 0xD5 ? scr * 256u + off : off;
    if (use && !verify && (dest >> 8) < scr && dest + ln > limit) {
      cleanup_and_free_buffer(fbuf);
      set_error(ERROR_FILE_TOO_LARGE);
      return order('E', 0, NULL, 0, NULL);
    }
    if (use)
      last = t;
    for (unsigned i = 0; i < ln; i += 255) {
      uint8_t k = ln - i > 255 ? 255 : ln - i;
      uint8_t got = fread_n(b, k);
      if (got < k)
        early = true;
      k = got;
      if (!use)
        continue;               /* CDOS's handler skips it */
      if (verify) {
        uint8_t *m = s_mem;
        if ((r = prim_read(dest + i, k, m)) < 0) return r;
        for (unsigned j = 0; j < k; j++)
          if (m[j] != b[j] && mism < 255)
            mism++;
      } else if (k && (r = order('W', dest + i, b, k, NULL)) < 0) {
        return r;
      }
      if (early)
        goto done;
    }
    if (use)
      end = dest + ln;
  }
done:
  /* cleanup_and_free_buffer, never free_buffer: only the cleanup closes
     the file, and littlefs keeps an unclosed one on its open-file list.
     The next load opened the same buffer's file again, the list pointed
     at itself, and the next save's lfs_remove() looped until the watchdog
     reset the drive (2026-10-05; tests/primo_turbo_host/shim.c counts it). */
  cleanup_and_free_buffer(fbuf);
  if (early)
    set_error_text("99,FILE ENDS EARLY,00,00");
  return order('E', end, NULL, verify ? mism : last, NULL);
}

static int save(void) {
  int r = open_named(1);
  if (r)
    return r < 0 ? r : order('E', 0, NULL, 0, NULL);
  uint16_t desc = s_de;
  uint8_t *b = s_chunk;
  for (;;) {
    uint8_t t;
    if ((r = prim_read(desc, 1, &t)) < 0) return r;
    if (fput(t)) break;
    if (t == 0xC9)
      break;
    if (t == 0xC3) {            /* the file ends here, as CDOS's does */
      if ((r = prim_read(desc + 1, 2, b)) < 0) return r;
      if (!fput(b[0]))
        fput(b[1]);
      break;
    }
    uint8_t h[6];
    if ((r = prim_read(desc + 1, 6, h)) < 0) return r;
    for (int i = 0; i < 4; i++)
      if (fput(h[i])) goto done;
    uint16_t ln = h[2] | h[3] << 8, src = h[4] | h[5] << 8;
    for (unsigned i = 0; i < ln; i += 255) {
      uint8_t k = ln - i > 255 ? 255 : ln - i;
      if ((r = prim_read(src + i, k, b)) < 0) return r;
      for (unsigned j = 0; j < k; j++)
        if (fput(b[j])) goto done;
    }
    desc += 7;
  }
done:
  CRUMB(11, 0);                        /* closing the saved file */
  cleanup_and_free_buffer(fbuf);       /* sets the error channel if it fails */
  CRUMB(12, 0);
  return order('E', 0, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ */

static void session(void) {
  uint32_t t0 = time_us_32();
  primo_turbo_stats.sessions++;
  s_in_call = false;
  crumb.sp_min = 0xFFFFFFFFu;
  CRUMB(1, s_idx);
  /* This runs in BUS_CLEANUP right after the Primo's UNTALK, and CDOS
     releases ATN a little after sending it (body 0x0714). Starting while
     ATN is still low read as "the Primo wants the bus" and ended every
     session at once (the first real test, 2026-10-05). So wait for the
     release first; if ATN stays low, it really is a new transaction. */
  /* ... and CLOCK: CDOS's UNTALK releases ATN first and CLOCK some 50 us
     later (body 0x0714). Starting in between, the PIO took CLOCK's release
     for the Primo's go edge and dropped the ready pull before the Primo had
     looked (the third real test: 1.3 s, then DEVICE NOT PRESENT). */
  while (!IEC_ATN || !IEC_CLOCK) {
    if (time_us_32() - t0 > 20000) {
      primo_turbo_stats.aborted++;
      primo_turbo_last_reason = 1;
      return;
    }
  }
  set_error(ERROR_OK);                 /* the Primo has read 00,TURBO */
  last_edge = last_act = time_us_32(); /* it was 0: "silent" for the whole
                                          uptime, every session aborted at its
                                          first check (2026-10-05, the Primo) */
  cur = 0;
  got_go = false;
  last_write = -1;
  edges_q = edges_s = 0;
  nsamp = 0;
  pins_take(true);                     /* DATA pulled: "in turbo" */
  q_reset();
  q_edge(W_REL);                       /* the Primo's go edge */
  q_req();
  int r = await_request();
  if (r == 0)
    r = s_idx == 0x0E ? save() : load(s_idx == 0x12);
  primo_turbo_last_reason = r == 0 ? 0 : r == -1 ? 2 : 3;
  primo_turbo_last_edges = edges_s;
  if (r == 0) {
    primo_turbo_stats.completed++;
    pins_give();
  } else {
    primo_turbo_stats.aborted++;
    if (fbuf && find_buffer(s_idx == 0x0E ? 1 : 0) == fbuf)
      cleanup_and_free_buffer(fbuf);   /* closes it; a save keeps what came */
    pins_give();                       /* leaves DATA alone if ATN took it */
  }
  fbuf = NULL;
  primo_turbo_stats.us += time_us_32() - t0;
  crumb.magic = 0;                     /* ended normally */
}

/* Self-test 8: test 5 from where a session runs -- BUS_CLEANUP, the turbo
   PIO holding DATA -- at the next transaction's end. */
static uint8_t selftest_cleanup;
static void selftest_save(bool pio, bool busy, const char *name, int nbytes);

void primo_turbo_poll(uint8_t last_cmd) {
  if (selftest_cleanup) {
    selftest_cleanup = 0;
    selftest_save(true, true, "@:PT0", 6);
  }
  if (armed == 1) {
    armed = 2;                         /* the command's own transaction */
  } else if (armed == 2 && last_cmd == 0x5F) {
    armed = 0;                         /* the answer read, and UNTALK: go */
    session();
  } else if (armed == 2 && last_cmd != 0x6F) {
    armed = 0;                         /* not the read of the answer (TALK's
                                          secondary 0x6F): drop the session */
  }
}

/* ------------------------------------------------------------------ */
/* Self-test: see primo_turbo.h. Runs only with the bus idle. */

static void selftest(void) {
  volatile primo_turbo_selftest_t *r = &primo_turbo_selftest;
  r->runs++;
  r->err = r->ready = r->levels = r->samples = r->imm = 0;
  if (!ready) { r->err = 1; return; }
  if (!IEC_ATN || !IEC_CLOCK || !IEC_DATA) { r->err = 2; return; }

  edges_q = edges_s = 0;
  nsamp = 0;
  pins_take(true);
  busy_wait_us_32(10);
  r->ready = !IEC_DATA;

  const uint8_t pat = 0x5A;
  q_reset();
  q_edge(W_REL);                               /* the go edge */
  for (int b = 0; b < 8; b++)
    q_edge((pat >> b) & 1 ? W_REL : W_PULL);
  q_edge(W_REL | W_HOLD);
  q_imm(W_PULL);                               /* given after the hold */

  for (int e = 0; e < 10; e++) {
    while (qpos < qlen && !pio_sm_is_tx_fifo_full(pio, sm))
      pio_sm_put(pio, sm, q[qpos++]);
    set_clock(e & 1);                          /* pull on even edges, release on odd */
    busy_wait_us_32(15);
    if (IEC_DATA)
      r->levels |= 1u << e;
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
      (void)pio_sm_get(pio, sm);
      r->samples++;
    }
  }
  /* the hold: the PIO waits for the word after it, already queued above */
  while (qpos < qlen && !pio_sm_is_tx_fifo_full(pio, sm))
    pio_sm_put(pio, sm, q[qpos++]);
  busy_wait_us_32(10);
  r->imm = !IEC_DATA;

  set_clock(1);
  pins_give();
}

/* Self-test 2 and 3: what a turbo save does to the file system, without a
   Primo -- file_open(1) with "@:PTSELF" (replacing it), 300 bytes, close --
   with the PIO holding DATA as in a session (2) or not (3). The first real
   turbo save hung inside file_open(1) (breadcrumb phase 7, 2026-10-05). */
extern volatile bool iec_bus_busy;

static void selftest_save(bool pio, bool busy, const char *name, int nbytes) {
  volatile primo_turbo_selftest_t *r = &primo_turbo_selftest;
  r->runs++;
  r->err = r->ready = r->levels = r->samples = r->imm = 0;
  if (!ready) { r->err = 1; return; }
  uint32_t t0 = time_us_32();
  const bool was_busy = iec_bus_busy;
  if (busy)
    iec_bus_busy = true;               /* as inside BUS_CLEANUP: core 1 gated */
  if (pio)
    pins_take(false);
  size_t nl = strlen(name);
  memcpy(command_buffer, name, nl);
  memset(command_buffer + nl, 0, sizeof(command_buffer) - nl);
  command_length = nl;
  CRUMB(7, 0x1000 | pio << 1 | busy);
  set_error(ERROR_OK);
  file_open(1);
  command_length = 0;
  fbuf = find_buffer(1);
  r->levels = (time_us_32() - t0) / 1000;   /* ms to open */
  if (!fbuf) {
    r->err = 3;
  } else {
    for (int i = 0; i < nbytes; i++)
      if (fput(i)) { r->err = 4; break; }
    CRUMB(11, 0);
    cleanup_and_free_buffer(fbuf);
    fbuf = NULL;
  }
  r->samples = current_error;
  r->imm = (time_us_32() - t0) / 10000;     /* x 10 ms in all */
  if (pio)
    pins_give();
  iec_bus_busy = was_busy;
  crumb.magic = 0;
}

/* Self-test 7: test 5 with ~1.2 KB of stack in use first -- what the
   session's buffers took before they were made static -- to show on the
   drive whether that depth alone overflows core 0's stack in file_open. */
static void __attribute__((noinline)) deep_save(int depth) {
  volatile uint8_t ballast[1200];
  ballast[0] = (uint8_t)depth;
  ballast[sizeof(ballast) - 1] = ballast[0];
  selftest_save(false, false, "@:PT0", 6);
  primo_turbo_selftest.err |= ballast[sizeof(ballast) - 1] & 0;
}

void primo_turbo_idle(void) {
  if (primo_turbo_selftest_req == 1)
    selftest();
  else if (primo_turbo_selftest_req == 2)
    selftest_save(true, false, "@:PTSELF", 300);
  else if (primo_turbo_selftest_req == 3)
    selftest_save(false, false, "@:PTSELF", 300);
  else if (primo_turbo_selftest_req == 4)
    selftest_save(true, true, "@:PTSELF", 300);
  else if (primo_turbo_selftest_req == 5)
    selftest_save(false, false, "@:PT0", 6);    /* PTURBO's first file */
  else if (primo_turbo_selftest_req == 6)
    selftest_save(false, false, "@:PTZ", 6);    /* another name, same size */
  else if (primo_turbo_selftest_req == 7)
    deep_save(7);
  else if (primo_turbo_selftest_req == 8)
    selftest_cleanup = 1;              /* run at the next BUS_CLEANUP */
  primo_turbo_selftest_req = 0;
}
