/* Host build of src/pico/primo_turbo.c (tests/primo_turbo_csim.py): the
   few definitions it takes from the firmware's config and arch headers. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef unsigned int uint;
#define IEC_PIN_ATN   2
#define IEC_PIN_CLOCK 3
#define IEC_PIN_DATA  6
#define CONFIG_COMMAND_BUFFER_SIZE 120
#define GPIO_FUNC_SIO 5
bool shim_gpio_get(uint pin);
void shim_set_data(int v);
uint32_t time_us_32(void);
#define gpio_get(p) shim_gpio_get(p)
#define IEC_ATN (shim_gpio_get(IEC_PIN_ATN))
#define set_data(v) shim_set_data(v)
#define IEC_CLOCK (shim_gpio_get(IEC_PIN_CLOCK))
#define IEC_DATA  (shim_gpio_get(IEC_PIN_DATA))
#define set_clock(v) ((void)(v))
static inline void busy_wait_us_32(uint32_t us) { (void)us; }
#define __uninitialized_ram(x) x
