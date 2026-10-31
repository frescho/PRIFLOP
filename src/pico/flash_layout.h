/* flash_layout.h: where the firmware, the volume and the settings sit in flash.

   Derived from one number, PICO_FLASH_SIZE_BYTES: the SDK's board header
   sets it (pico2_w: 4MB), and a build for a bigger board overrides it with
   -DPRIFLOP_FLASH_MB=16 (scripts/pico/CMakeLists.txt), which sets it for the
   linker and the compiler alike. Plain macros, no SDK calls, so host tests
   and tools can include it (tests/flash_layout_test.c).

     0                    FLASH_FIRMWARE_MAX (1MB)    total - 4KB      total
     | firmware, UF2 abs  | littlefs volume           | settings (XW) |
     |   block at 1MB-256 |                           |               |

   The 16MB layout is theoretical until such a board is tested (
   R7c). CHANGING THE SIZE ERASES THE VOLUME on a board flashed before:
   littlefs keeps its block count in the superblock. */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must come from the board header or the build"
#endif
#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096u          /* hardware/flash.h, for host builds */
#endif

#define PICO_FLASH_TOTAL_SIZE (PICO_FLASH_SIZE_BYTES)
#define FLASH_EEPROM_SIZE     FLASH_SECTOR_SIZE /* 4KB, one sector */
#define FLASH_EEPROM_OFFSET   (PICO_FLASH_TOTAL_SIZE - FLASH_EEPROM_SIZE)

/* The firmware's region; the volume starts where it ends. */
#define FLASH_FIRMWARE_MAX    (1024 * 1024)
#define FLASH_ATA_OFFSET      FLASH_FIRMWARE_MAX
#define FLASH_ATA_SIZE        (FLASH_EEPROM_OFFSET - FLASH_ATA_OFFSET)
#define FLASH_ATA_BLOCKS      (FLASH_ATA_SIZE / FLASH_SECTOR_SIZE)

/* littlefs's lookahead: one bit per block, whole multiples of 8 bytes, so
   it finds free space in one pass. 4MB: 767 blocks -> 96 bytes; 16MB:
   3839 blocks -> 480 bytes. */
#define LFS_LOOKAHEAD_SIZE    (((FLASH_ATA_BLOCKS) + 63) / 64 * 8)

/* The UF2's absolute block (RP2350-A2 erratum E10), passed to picotool in
   scripts/pico/CMakeLists.txt: the last page of the firmware region. The
   SDK's default, 0x10ffff00, is the last page of 16MB -- on a 16MB board
   that is exactly the settings sector, on 4MB it wraps onto it. */
#define FLASH_UF2_ABS_BLOCK   (FLASH_FIRMWARE_MAX - 256)

_Static_assert(PICO_FLASH_TOTAL_SIZE % FLASH_SECTOR_SIZE == 0, "flash size not whole sectors");
_Static_assert(PICO_FLASH_TOTAL_SIZE >= 2 * 1024 * 1024, "flash too small for firmware and a volume");
_Static_assert(PICO_FLASH_TOTAL_SIZE <= 16 * 1024 * 1024, "RP2350 maps 16MB per chip select");
_Static_assert(FLASH_UF2_ABS_BLOCK < FLASH_ATA_OFFSET, "UF2 abs block must stay out of the volume");

#endif
