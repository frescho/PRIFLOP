/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>

   Inspired by MMC2IEC by Lars Pontoppidan et al.

   Historical credit: FAT filesystem access was based on code from ChaN
   and Jim Brain. No code of theirs remains -- storage is littlefs, which
   carries its own BSD-3-Clause licence in src/littlefs/LICENSE.md.

   RP2350 / Raspberry Pi Pico 2 W port by frescho.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


   diskio.h: Definitions for the disk access routines

   The disk_read/disk_write/DSTATUS/DRESULT shape of this header started
   as ChaN's diskio.h from FatFs, and the credit is recorded in README.md
   for that reason. Nothing ChaN wrote is left in it: FatFs itself is gone
   (littlefs replaced it), the ATA and SD back ends are gone with it, the
   BYTE/WORD/DWORD typedefs have been replaced by <stdint.h>, and what
   remains -- diskinfo0_t, disk_getinfo(), the DISK_TYPE_* constants,
   get_default_driveconfig() -- is sd2iec's own.

*/

#ifndef DISKIO_H
#define DISKIO_H

#include <stdint.h>

/* Status of Disk Functions */
typedef uint8_t    DSTATUS;

/* Disk Status Bits (DSTATUS) */
#define STA_NOINIT              0x01    /* Drive not initialized */
#define STA_NODISK              0x02    /* No medium in the drive */
#define STA_PROTECT             0x04    /* Write protected */

/* Results of Disk Functions */
typedef enum {
        RES_OK = 0,             /* 0: Successful */
        RES_ERROR,              /* 1: R/W Error */
        RES_WRPRT,              /* 2: Write Protected */
        RES_NOTRDY,             /* 3: Not Ready */
        RES_PARERR              /* 4: Invalid Parameter */
} DRESULT;

/**
 * struct diskinfo0_t - disk info data structure for page 0
 * @validbytes : Number of valid bytes in this struct
 * @maxpage    : Highest diskinfo page supported
 * @disktype   : type of the disk (DISK_TYPE_* values)
 * @sectorsize : sector size divided by 256
 * @sectorcount: number of sectors on the disk
 *
 * This is the struct returned in the data buffer when disk_getinfo
 * is called with page=0.
 */
typedef struct {
  uint8_t  validbytes;
  uint8_t  maxpage;
  uint8_t  disktype;
  uint8_t  sectorsize;   /* divided by 256 */
  uint32_t sectorcount;  /* 2 TB should be enough... (512 byte sectors) */
} diskinfo0_t;

/*---------------------------------------*/
/* Prototypes for disk control functions */

DSTATUS disk_initialize (uint8_t);
DSTATUS disk_status (uint8_t);
DRESULT disk_read (uint8_t, uint8_t*, uint32_t, uint8_t);
DRESULT disk_write (uint8_t, const uint8_t*, uint32_t, uint8_t);
#define disk_ioctl(a,b,c) RES_OK
DRESULT disk_getinfo(uint8_t drv, uint8_t page, void *buffer);

void disk_init(void);

/* Optional hook: called by the shared IEC mainloop whenever the bus
   returns to idle (no transaction in progress). It exists so that an
   arch which defers physical writes out of disk_write() has somewhere
   to flush them, rather than stalling a live bus transaction.
   On this port
   src/pico/disk_lfs.c implements it. */
void disk_idle_sync(void);

/* Will be set to DISK_ERROR if any access on the card fails */
enum diskstates { DISK_CHANGED = 0, DISK_REMOVED, DISK_OK, DISK_ERROR };

extern volatile enum diskstates disk_state;

/* Disk type - part of the external API except for ATA2! */
#define DISK_TYPE_ATA        0
#define DISK_TYPE_ATA2       1
#define DISK_TYPE_SD         2
/* #define DISK_TYPE_DF         3 - removed */
#define DISK_TYPE_NONE       7

#ifdef NEED_DISKMUX

/* Disk mux configuration */

extern uint32_t drive_config;

uint32_t get_default_driveconfig(void);

#  define set_map_drive(drv,val) (drive_config = \
      (drive_config & (0xffffffff - (0x0f << (drv * 4)))) \
                    | (val << (drv * 4)))
#  define map_drive(drv) ((drive_config >> (4 * drv)) & 0x0f)
#  define set_drive_config(config)  drive_config = config

/* Number of bits used for the drive, the disk type */
/* uses the remainder (4 bits per entry).           */
#  define DRIVE_BITS           1

/* Calculate mask from the shift value */
#  define DRIVE_MASK           ((1 << DRIVE_BITS)-1)

#else // NEED_DISKMUX

#  define set_map_drive(drv,val) do {} while(0)
#  define map_drive(drv) (drv)
#  define set_drive_config(conf) do {} while(0)
#  define get_default_driveconfig() 0

#endif // NEED_DISKMUX

#endif
