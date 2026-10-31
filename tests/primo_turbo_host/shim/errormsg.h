#include <stdint.h>
#define CONFIG_ERROR_BUFFER_SIZE 46
extern uint8_t error_buffer[CONFIG_ERROR_BUFFER_SIZE];
extern uint8_t current_error;
void set_error(uint8_t e);
void set_error_ts(uint8_t e, uint8_t track, uint8_t sector);
#define ERROR_OK 0
#define ERROR_SYNTAX_UNKNOWN 30
#define ERROR_FILE_TOO_LARGE 52
#define ERROR_FILE_NOT_FOUND 62
