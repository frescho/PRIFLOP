/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2017  Ingo Korb <ingo@akana.de>
   Copyright (C) 2026  sd2iec_pico2w contributors

   RP2350 / Raspberry Pi Pico 2 W port by frescho.
   This file was written for that port.

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

*/

/* httpd.h: minimal HTTP file-manager server for the sd2iec_pico2w port.

   Serves the FAT volume that backs internal-flash storage (src/pico/ata.c)
   over plain HTTP on port 80: a single embedded page plus a small REST-ish
   API for upload/download/delete/rename/mkdir. See httpd.c for the full
   design notes. This is
   a deliberately simplified first cut (no HTTPS).
   
*/
#ifndef HTTPD_H
#define HTTPD_H

/* Starts listening on port 80. Call once, after Wi-Fi is up and the FAT
   volume is mounted (filesystem_init() must have already run). */
void httpd_init(void);

/* Sets the password required (as the password half of HTTP Basic Auth,
   any username accepted) to use any part of the web UI/API. Pass ""
   (the default) to leave it open, matching this server's original
   behavior before auth existed. Call before httpd_init(). */
void httpd_set_admin_password(const char *password);

/* Call every network_poll() tick (see network.c) -- services the
   deferred Reset/BOOTSEL requests from the web UI's buttons. A no-op
   almost always; see httpd.c's handle_reboot()/handle_bootsel(). */
void httpd_poll(void);

#endif
