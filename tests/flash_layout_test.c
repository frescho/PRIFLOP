/* flash_layout.h on 4MB and 16MB: the 4MB numbers must stay what the drives
   in the field have (a change erases their volume), and 16MB must keep the
   settings sector and the UF2 abs block off the volume.
     for m in 4 16; do gcc -DMB=$m -DPICO_FLASH_SIZE_BYTES="($m*1024*1024)" \
       -I src/pico -o fltest tests/flash_layout_test.c && ./fltest; done
   tests/run_flash_layout_test.sh runs both, and a 1MB build that must not compile. */
#include <stdio.h>
#include "flash_layout.h"

static int bad;
#define EXPECT(what, got, want) do { if ((long)(got) != (long)(want)) { \
  printf("FAIL %s: %ld, want %ld\n", what, (long)(got), (long)(want)); bad++; } } while (0)

int main(void) {
#if MB == 4
  EXPECT("blocks", FLASH_ATA_BLOCKS, 767);
  EXPECT("lookahead", LFS_LOOKAHEAD_SIZE, 96);
  EXPECT("settings", FLASH_EEPROM_OFFSET, 0x3ff000);
#elif MB == 16
  EXPECT("blocks", FLASH_ATA_BLOCKS, 3839);
  EXPECT("lookahead", LFS_LOOKAHEAD_SIZE, 480);
  EXPECT("settings", FLASH_EEPROM_OFFSET, 0xfff000);
#endif
  EXPECT("volume start", FLASH_ATA_OFFSET, 0x100000);
  EXPECT("abs block", FLASH_UF2_ABS_BLOCK, 0x0fff00);
  EXPECT("volume ends at settings", FLASH_ATA_OFFSET + FLASH_ATA_BLOCKS * FLASH_SECTOR_SIZE, FLASH_EEPROM_OFFSET);
  EXPECT("lookahead covers blocks", LFS_LOOKAHEAD_SIZE * 8 >= FLASH_ATA_BLOCKS, 1);
  EXPECT("lookahead multiple of 8", LFS_LOOKAHEAD_SIZE % 8, 0);
  printf("%d MB: %ld blocks, lookahead %ld: %s\n", MB, (long)FLASH_ATA_BLOCKS,
         (long)LFS_LOOKAHEAD_SIZE, bad ? "FAIL" : "ok");
  return bad != 0;
}
