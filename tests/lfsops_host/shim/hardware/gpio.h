/* Host build of lfsops.c: the GPIO calls arch-config.h makes, as no-ops. */
#include <stdint.h>
#include <stdbool.h>
#define GPIO_IN 0
#define GPIO_OUT 1
#define GPIO_FUNC_SIO 5
static inline void gpio_init(unsigned p){(void)p;}
static inline void gpio_set_dir(unsigned p,bool o){(void)p;(void)o;}
static inline void gpio_put(unsigned p,bool v){(void)p;(void)v;}
static inline bool gpio_get(unsigned p){(void)p;return 1;}
static inline void gpio_pull_up(unsigned p){(void)p;}
static inline void gpio_disable_pulls(unsigned p){(void)p;}
static inline void gpio_set_function(unsigned p,int f){(void)p;(void)f;}
typedef unsigned uint;
#define GPIO_IRQ_EDGE_RISE 8
#define GPIO_IRQ_EDGE_FALL 4
static inline void gpio_set_irq_enabled(unsigned p,unsigned e,bool o){(void)p;(void)e;(void)o;}
static inline void gpio_acknowledge_irq(unsigned p,unsigned e){(void)p;(void)e;}
