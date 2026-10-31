/* The host build drives the Python PIO model, which runs the real .pio
   source; only the label offset is needed here (pioasm: 8). */
static const pio_program_t primo_turbo_program;
#define primo_turbo_offset_released 8u
static inline void primo_turbo_program_init(PIO pio, uint sm, uint offset, uint pin) {
  (void)pio; (void)sm; (void)offset; (void)pin;
}
