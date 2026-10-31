typedef struct { struct { volatile unsigned ctrl; } io[48]; } io_bank0_hw_t;
extern io_bank0_hw_t shim_io_bank0;
#define io_bank0_hw (&shim_io_bank0)
#define IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB 0
