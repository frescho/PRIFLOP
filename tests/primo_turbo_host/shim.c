/* Host build of src/pico/primo_turbo.c for tests/primo_turbo_csim.py.

   The firmware's PIO, GPIO and time calls are forwarded to Python, which
   runs the real .pio program in tests/pio_model.py's terms against the
   Primo's real Z80 code. sd2iec's file layer is replaced by one file in
   memory, read or written through 256-byte buffers the way the real ones
   behave (position .. lastused, sendeoi on the last, mustflush on a wrap).
*/
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "shim/config.h"
#include "shim/buffers.h"
#include "shim/hardware/pio.h"
#include "shim/hardware/structs/io_bank0.h"

/* ---- set by Python ---- */
typedef uint32_t (*fn_time)(void);
typedef bool     (*fn_gpio)(uint);
typedef void     (*fn_int)(int);
typedef void     (*fn_put)(uint32_t);
typedef bool     (*fn_bool)(void);
typedef uint32_t (*fn_get)(void);
typedef void     (*fn_jmp)(uint);

fn_time py_time;
fn_gpio py_gpio;
fn_int  py_set_data;      /* SIO drives DATA: 1 released, 0 pulled */
fn_int  py_pindir;        /* the PIO's DATA pindir: 1 pulled */
fn_int  py_enable;
fn_bool py_rx_empty;
fn_get  py_get;
fn_put  py_put;
fn_jmp  py_jmp;
fn_int  py_clear;

/* A few files, by name: a save ("@:name" replaces) stores one, a load
   finds it. Python fills and reads them through shim_store_*(). */
#define NFILES 16
static struct { uint8_t name[32]; int nlen; uint8_t data[20000]; int len; int used; } store[NFILES];
static int cur_file = -1;                       /* the file a load reads */
static uint8_t out_name[32];
static int out_nlen;
int file_closed;

static int store_find(const uint8_t *n, int nl) {
  for (int i = 0; i < NFILES; i++)
    if (store[i].used && store[i].nlen == nl && !memcmp(store[i].name, n, nl))
      return i;
  return -1;
}
int shim_store_put(const uint8_t *n, int nl, const uint8_t *d, int len) {
  int i = store_find(n, nl);
  if (i < 0)
    for (i = 0; i < NFILES && store[i].used; i++) ;
  if (i >= NFILES || len > (int)sizeof(store[0].data))
    return -1;
  memcpy(store[i].name, n, nl);
  store[i].nlen = nl;
  memcpy(store[i].data, d, len);
  store[i].len = len;
  store[i].used = 1;
  return 0;
}
int shim_store_get(const uint8_t *n, int nl, uint8_t *d) {
  int i = store_find(n, nl);
  if (i < 0)
    return -1;
  memcpy(d, store[i].data, store[i].len);
  return store[i].len;
}
static uint8_t file_out[20000];
static int file_out_len;

/* ---- firmware globals primo_turbo.c uses ---- */
uint8_t command_buffer[120];
uint8_t globalflags;
uint8_t command_length;
uint8_t error_buffer[46];
uint8_t current_error;
volatile unsigned char iec_pio_pins_held;
volatile bool iec_bus_busy;
io_bank0_hw_t shim_io_bank0;
buffer_t buffers[2];
static uint8_t bufdata[256];
static buffer_t fb;
static int fb_in_use, fb_sec, file_pos;

/* littlefs keeps every open file on a list until it is closed, and
   sd2iec's buffer holds that file: only cleanup_and_free_buffer() closes
   it; free_buffer() just marks the buffer free. A file opened into a
   buffer whose file is still open puts it on the list a second time --
   the list then points at itself and the next lfs_remove() walks it for
   ever (the drive's watchdog reset of 2026-10-05). Counted here; the
   tests require both to stay 0. */
static int fb_file_open;
int shim_open_files;      /* files opened and not closed */
int shim_reopened;        /* opened into a buffer whose file was still open */

uint32_t time_us_32(void) { return py_time(); }
bool shim_gpio_get(uint pin) { return py_gpio(pin); }
void shim_set_data(int v) { py_set_data(v); }
void system_watchdog_pet(void) {}

void set_error(uint8_t e) {
  current_error = e;
  memset(error_buffer, 0, sizeof(error_buffer));
  const char *t = e == 0 ? "00, OK,00,00" : e == 30 ? "30,SYNTAX ERROR,00,00"
                : e == 52 ? "52,FILE TOO LARGE,00,00" : e == 62 ? "62,FILE NOT FOUND,00,00"
                : "99,OTHER,00,00";
  strcpy((char *)error_buffer, t);
  error_buffer[strlen(t)] = 13;
  buffers[0].lastused = strlen(t);
}

void set_error_ts(uint8_t e, uint8_t track, uint8_t sector) {
  set_error(e);                                   /* "..,00,00\r" */
  int n = (int)strlen((char *)error_buffer);
  error_buffer[n - 6] = (uint8_t)('0' + track / 10);
  error_buffer[n - 5] = (uint8_t)('0' + track % 10);
  error_buffer[n - 3] = (uint8_t)('0' + sector / 10);
  error_buffer[n - 2] = (uint8_t)('0' + sector % 10);
}

/* PRIFLOP.CFG's PFM_AUTOSTART, as netconfig.c would read it: set by Python */
signed char shim_pfm_autostart = -1;
signed char netconfig_pfm_autostart(void) { return shim_pfm_autostart; }

static uint8_t refill_read(buffer_t *b) {
  int n = store[cur_file].len - file_pos;
  if (n > 256) n = 256;
  memcpy(bufdata, store[cur_file].data + file_pos, n);
  file_pos += n;
  b->position = 0;
  b->lastused = n - 1;
  b->sendeoi = file_pos >= store[cur_file].len;
  return 0;
}

static uint8_t refill_write(buffer_t *b) {
  memcpy(file_out + file_out_len, bufdata, 256);
  file_out_len += 256;
  b->position = 0;
  b->lastused = 0;
  b->mustflush = 0;
  return 0;
}

void file_open(unsigned char secondary) {
  const uint8_t *n = command_buffer;
  int nl = command_length;
  if (nl > 2 && n[0] == '@' && n[1] == ':') {   /* save and replace */
    n += 2;
    nl -= 2;
  }
  if (fb_file_open)
    shim_reopened++;
  memset(&fb, 0, sizeof(fb));
  fb.data = bufdata;
  if (secondary == 0) {
    cur_file = store_find(n, nl);
    if (cur_file < 0) {
      set_error(62);
      return;
    }
    file_pos = 0;
    fb.read = 1;
    fb.refill = refill_read;
    refill_read(&fb);
  } else {
    memcpy(out_name, n, nl);
    out_nlen = nl;
    file_out_len = 0;
    file_closed = 0;
    fb.write = 1;
    fb.refill = refill_write;
  }
  fb_in_use = 1;
  fb_sec = secondary;
  fb_file_open = 1;
  shim_open_files++;
}

buffer_t *find_buffer(uint8_t secondary) {
  return fb_in_use && fb_sec == secondary ? &fb : NULL;
}
void free_buffer(buffer_t *b) { (void)b; fb_in_use = 0; }
void cleanup_and_free_buffer(buffer_t *b) {
  if (b->write) {
    int n = b->mustflush ? 256 : b->position;
    memcpy(file_out + file_out_len, bufdata, n);
    file_out_len += n;
    shim_store_put(out_name, out_nlen, file_out, file_out_len);
    file_closed = 1;
  }
  if (fb_file_open) {
    fb_file_open = 0;
    shim_open_files--;
  }
  fb_in_use = 0;
}
void mark_buffer_dirty(buffer_t *b) { (void)b; }

/* ---- PIO ---- */
bool pio_claim_free_sm_and_add_program_for_gpio_range(const pio_program_t *p, PIO *pio, uint *sm,
                                                      uint *off, uint base, uint count, bool sb) {
  (void)p; (void)base; (void)count; (void)sb;
  *pio = 0; *sm = 0; *off = 0;
  return true;
}
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t v, uint32_t mask) { (void)pio; (void)sm; (void)v; (void)mask; }
void pio_sm_set_enabled(PIO pio, uint sm, bool on) { (void)pio; (void)sm; py_enable(on); }
void pio_sm_clear_fifos(PIO pio, uint sm) { (void)pio; (void)sm; py_clear(0); }
void pio_sm_restart(PIO pio, uint sm) { (void)pio; (void)sm; }
void pio_sm_exec(PIO pio, uint sm, uint instr) { (void)pio; (void)sm; py_jmp(instr); }
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out) {
  (void)pio; (void)sm; (void)pin; (void)count; py_pindir(out);
}
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm) { (void)pio; (void)sm; return false; }
void pio_sm_put(PIO pio, uint sm, uint32_t w) { (void)pio; (void)sm; py_put(w); }
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) { (void)pio; (void)sm; return py_rx_empty(); }
uint32_t pio_sm_get(PIO pio, uint sm) { (void)pio; (void)sm; return py_get(); }
