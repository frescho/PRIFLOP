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


   flags.h: Definitions for some global flags

*/

#ifndef FLAGS_H
#define FLAGS_H

#ifdef __AVR__
/* GPIOR0 is a bit-addressable register reserved for user data */
#  define globalflags (GPIOR0)
#else
/* Global flags, variable defined in doscmd.c */
extern uint8_t globalflags;
#endif

/** flag values **/
/* transient flags */
#define VC20MODE         (1<<0)
#define AUTOSWAP_ACTIVE  (1<<2)
#define SWAPLIST_ASCII   (1<<5)

/* permanent (EEPROM-saved) flags */
/* 1<<1 was JIFFY_ENABLED */
#define EXTENSION_HIDING (1<<3)
#define POSTMATCH        (1<<4)
/* The Primo turbo is on after a restart: XT+ then XW, or the web UI's
   Settings tab (2026-10-07; src/pico/primo_turbo.h). Not 1<<1, which older
   firmware may have saved as JIFFY_ENABLED. */
#define PRIMO_TURBO      (1<<6)

/* Disk image-as-directory mode, defined in fileops.c */
extern uint8_t image_as_dir;

#define IMAGE_DIR_NORMAL 0
#define IMAGE_DIR_DIR    1
#define IMAGE_DIR_BOTH   2

#endif
