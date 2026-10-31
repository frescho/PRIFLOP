#include <stdint.h>
typedef struct buffer_s {
  uint8_t *data;
  uint8_t position, lastused, sendeoi, mustflush, read, write;
  uint8_t (*refill)(struct buffer_s *);
} buffer_t;
#define ERRORBUFFER_IDX 0
extern buffer_t buffers[2];
buffer_t *find_buffer(uint8_t secondary);
void free_buffer(buffer_t *b);
void cleanup_and_free_buffer(buffer_t *b);
void mark_buffer_dirty(buffer_t *b);
