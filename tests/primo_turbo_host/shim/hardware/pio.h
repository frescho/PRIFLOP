#include <stdint.h>
#include <stdbool.h>
typedef int PIO;
typedef struct { int dummy; } pio_program_t;
#define PIO_FUNCSEL_NUM(pio, pin) 6
bool pio_claim_free_sm_and_add_program_for_gpio_range(const pio_program_t *p, PIO *pio, uint *sm, uint *off, uint base, uint count, bool set_base);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t v, uint32_t mask);
void pio_sm_set_enabled(PIO pio, uint sm, bool on);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_restart(PIO pio, uint sm);
void pio_sm_exec(PIO pio, uint sm, uint instr);
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out);
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm);
void pio_sm_put(PIO pio, uint sm, uint32_t w);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
uint32_t pio_sm_get(PIO pio, uint sm);
static inline uint pio_encode_jmp(uint addr) { return addr; }
