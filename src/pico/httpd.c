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

/* httpd.c: minimal HTTP file-manager server for the sd2iec_pico2w port.

   First cut "management interface" -- scoped
   down to exactly what was asked for: a simple web UI + REST API to
   upload/download/delete/rename files and directories on the FAT volume
   that src/pico/ata.c exposes as internal-flash storage. Optional HTTP
   Basic Auth (2026-09-02, see httpd_set_admin_password() below), but
   still no HTTPS, no mDNS, no multi-partition/mounted-disk-image
   browsing (this walks the *host* FAT filesystem directly via
   partition[0].fatfs, the
   same view you'd get plugging the flash into a PC -- not the C64-facing
   D64-aware view lfsops.c presents over IEC). Those are all tracked
   as known gaps, not oversights.

   CONCURRENCY: this server is driven entirely from network_poll(),
   which src/iec.c's mainloop calls only when the IEC bus is idle (see
   system.h and the BUS_IDLE case) -- the same safe point already used
   for deferred flash commits (diskio.h's disk_idle_sync()). Nothing here
   runs concurrently with real IEC bus handling; the whole system is
   single-threaded and cooperative, so no locking is needed against
   fatops.c/d64ops.c's use of the same FATFS instance. A FIL can safely
   stay open across many network_poll() calls (streaming a large file):
   classic FatFs (the fork used here) treats each f_read()/f_write() call
   as self-contained -- it doesn't rely on state left over in its shared
   sector window between calls -- so interleaving separate FIL handles
   turn-by-turn (never truly concurrently, which our cooperative model
   guarantees) is safe. The one real edge case: if a C64 transaction
   modifies/deletes the exact file an HTTP transfer has open at the same
   time, that transfer will see inconsistent data -- a rare, documented
   limitation, not a memory-safety bug.

   Design: hand-rolled on lwIP's raw (callback) API, NO_SYS/poll mode
   (see lwipopts.h) -- matches this codebase's existing "no big
   frameworks" style. Deliberately simple HTTP/1.1 subset: no chunked
   transfer-encoding, no keep-alive (every response closes the
   connection), no multipart uploads (the web UI POSTs a file's raw
   bytes directly as the request body instead, which is all a browser's
   fetch(file) needs and avoids implementing a multipart parser).
*/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "parser.h"
#include "uart.h"
#include "base64.h"
#include "bus.h"
#include "flags.h"
#include "iec.h"
#include "diskio.h"
#include "lfsops.h"
#include "primo_turbo.h"
#include "iec_pio.h"
#include "bootlog.h"
#include "tusb.h"
#include "pico/stdio_usb.h"
#include "fs_bridge.h"
#include "health.h"
#include "network.h"
#include "netconfig.h"

#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "httpd.h"

/* Declared here rather than via system.h: that header also declares
   disable_interrupts()/enable_interrupts(), which clash with a Pico SDK
   declaration already pulled in above. See system.c. */
uint32_t stack_high_water(void);
uint32_t stack_total(void);
uint32_t fault_last(unsigned int which);
uint32_t boot_count(void);
uint32_t watchdog_ctrl_raw(void);
extern volatile uint32_t flash_op_max_us, flash_ops;
extern volatile uint32_t lfsread_max_us, lfsread_ops;
extern volatile uint32_t lfsread_pre_max_us, lfsread_post_max_us;
uint32_t lfsread_pre_last_us_prev_boot(void);
uint32_t lfsread_post_last_us_prev_boot(void);
extern volatile uint32_t iec_atn_irqs;
extern volatile uint32_t iec_atnfinish_bails;
extern volatile uint8_t iec_bail_cmds[8];
extern volatile uint8_t iec_bail_dev;
extern volatile uint8_t iec_cmd_ring[8];
extern volatile uint8_t iec_cmd_head;
/* Bus statistics -- see the block above iec_getc() in src/iec.c. */
extern volatile uint32_t iec_stat_rx_bytes, iec_stat_tx_bytes;
extern volatile uint32_t iec_stat_getc_max_us, iec_stat_putc_max_us;
extern volatile uint32_t iec_stat_gap_max_us, iec_stat_busy_ms;
extern volatile uint32_t iec_stat_put_gap_max_us;
uint32_t iec_put_gap_max_us_prev_boot(void);
uint32_t iec_putc_loop_prev_boot(void);
extern volatile uint32_t iec_stat_refill_max_us, iec_stat_findbuf_max_us;
uint32_t iec_refill_max_us_prev_boot(void);
uint32_t iec_tx_position_prev_boot(void);
uint32_t iec_refill_phase_prev_boot(void);
extern volatile uint32_t iec_stat_atn_aborts;
extern volatile uint32_t iec_stat_jf_rx, iec_stat_jf_tx, iec_stat_jf_us;
extern volatile uint8_t  pico_usb_stdio_ok;
extern volatile uint32_t bootlog_polls, bootlog_dumps;

/*--------------------------------------------------------------------------
   Embedded web UI -- a single static page, no external assets.
---------------------------------------------------------------------------*/

static const char PAGE_HTML[] =
"<!doctype html><html lang=\"en\"><head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>PRIFLOP</title>\n"
/* The favicon: the header's first keycap, P in Primo orange (the logo is
   docs/user/images/logo.svg). Inline, so it costs no request and no file. */
"<link rel=\"icon\" href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' "
"viewBox='0 0 34 34'><rect width='34' height='34' rx='6' fill='%23cf880c'/><text x='17' "
"y='25' font-family='Arial,Helvetica,sans-serif' font-weight='700' font-size='22' "
"fill='%23fff' text-anchor='middle'>P</text></svg>\">\n"
"<style>\n"
":root{\n"
"--bg:#f2f3f4;--panel:#fff;--ink:#0c0d0e;--dim:#5b6570;--line:#dfe3e6;\n"
"--accent:#0d800e;--accent-ink:#fff;--danger:#b3261e;--ok:#0d800e;\n"
"--body:#0c0d0e;--cap:#fff;--cap-ink:#0c0d0e;\n"
"--primo-orange:#cf880c;--primo-green:#0d800e;\n"
"--radius:10px;--shadow:0 1px 2px rgba(0,0,0,.06),0 4px 12px rgba(0,0,0,.05);\n"
"}\n"
"@media(prefers-color-scheme:dark){:root{\n"
"--bg:#0c0d0e;--panel:#17191b;--ink:#e9ecee;--dim:#98a2ab;--line:#282c30;\n"
"--accent:#22a824;--accent-ink:#0c0d0e;--danger:#e0604f;--ok:#22a824;\n"
"--shadow:0 1px 2px rgba(0,0,0,.4),0 4px 14px rgba(0,0,0,.3);\n"
"}}\n"
"*{box-sizing:border-box}\n"
"body{margin:0;background:var(--bg);color:var(--ink);\n"
"font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;}\n"
"header{background:var(--body);padding:20px;border-bottom:3px solid var(--accent)}\n"
".hdr{max-width:900px;margin:0 auto;display:flex;align-items:center;\n"
"gap:16px;flex-wrap:wrap}\n"
".keys{display:flex;gap:5px}\n"
".cap{width:34px;height:34px;border-radius:5px;background:var(--cap);color:var(--cap-ink);\n"
"display:flex;align-items:center;justify-content:center;font-weight:700;font-size:17px;\n"
"line-height:1;box-shadow:0 2px 0 rgba(0,0,0,.55);user-select:none}\n"
"/* The machine's own two colour variants (pdf/primo_narancs.tga,\n"
"pdf/primo_zold.tga): its function keys are orange on one and green on\n"
"the other, always with white lettering. Both are sampled exactly. */\n"
".cap.o{background:var(--primo-orange);color:#fff}\n"
".cap.g{background:var(--primo-green);color:#fff}\n"
".sub{color:#9aa4ac;font-size:12.5px;margin-top:2px}\n"
".sub b{color:#fff;font-weight:600}\n"
".dot{display:inline-block;width:7px;height:7px;border-radius:50%;\n"
"background:var(--accent);margin-right:6px;vertical-align:1px}\n"
"main{max-width:900px;margin:0 auto;padding:0 20px 40px}\n"
".card{background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);\n"
"box-shadow:var(--shadow);margin-bottom:18px;overflow:hidden}\n"
".cardhead{display:flex;align-items:center;gap:10px;flex-wrap:wrap;\n"
"padding:12px 14px;border-bottom:1px solid var(--line)}\n"
".cardhead h2{margin:0;font-size:14px;font-weight:600;letter-spacing:.3px}\n"
".grow{flex:1}\n"
".body{padding:14px}\n"
"nav#crumbs{font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;\n"
"color:var(--dim);word-break:break-all}\n"
"nav#crumbs a{color:var(--accent);text-decoration:none;cursor:pointer}\n"
"nav#crumbs a:hover{text-decoration:underline}\n"
"table{width:100%;border-collapse:collapse}\n"
"th{font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:var(--dim);\n"
"text-align:left;font-weight:600;padding:8px 14px;border-bottom:1px solid var(--line)}\n"
"td{padding:8px 14px;border-bottom:1px solid var(--line);vertical-align:middle}\n"
"tr:last-child td{border-bottom:0}\n"
"tbody tr:hover{background:color-mix(in srgb,var(--accent) 6%,transparent)}\n"
"td.size{text-align:right;color:var(--dim);font-variant-numeric:tabular-nums;\n"
"white-space:nowrap;font-size:13px}\n"
"td.act{text-align:right;white-space:nowrap;width:1%}\n"
".name{display:flex;align-items:center;gap:8px;min-width:0}\n"
".name a{color:var(--ink);text-decoration:none;overflow:hidden;\n"
"text-overflow:ellipsis;white-space:nowrap}\n"
".name a:hover{color:var(--accent);text-decoration:underline}\n"
".ic{flex:none;width:18px;text-align:center;opacity:.75}\n"
"button{font:inherit;font-size:13px;border-radius:7px;border:1px solid var(--line);\n"
"background:var(--panel);color:var(--ink);padding:6px 11px;cursor:pointer}\n"
"button:hover{border-color:var(--accent);color:var(--accent)}\n"
"button.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-ink)}\n"
"button.primary:hover{filter:brightness(1.08);color:var(--accent-ink)}\n"
"button.danger:hover{border-color:var(--danger);color:var(--danger)}\n"
"button.mini{padding:4px 8px;font-size:12px}\n"
"button.link{border:0;background:0;color:var(--accent);padding:0;text-decoration:underline}\n"
".drop{margin:14px;padding:18px;border:1.5px dashed var(--line);border-radius:var(--radius);\n"
"text-align:center;color:var(--dim);font-size:13px;transition:.15s}\n"
".drop.over{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 8%,transparent)}\n"
".bar{height:5px;border-radius:3px;background:var(--line);margin-top:10px;overflow:hidden}\n"
".bar>i{display:block;height:100%;width:0;background:var(--accent);transition:width .15s}\n"
".field{display:flex;align-items:center;gap:10px;margin-bottom:9px;flex-wrap:wrap}\n"
".field label{flex:none;width:150px;font-size:13px;color:var(--dim)}\n"
"input,select{font:inherit;font-size:14px;padding:6px 9px;border-radius:7px;\n"
"border:1px solid var(--line);background:var(--bg);color:var(--ink);min-width:0;flex:1;max-width:280px}\n"
"input:focus,select:focus{outline:2px solid color-mix(in srgb,var(--accent) 45%,transparent);\n"
"outline-offset:1px;border-color:var(--accent)}\n"
".hint{color:var(--dim);font-size:12.5px;line-height:1.5;margin:10px 0 0}\n"
".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px}\n"
".badge{font-size:12px;color:var(--dim);background:var(--bg);border:1px solid var(--line);\n"
"border-radius:999px;padding:2px 9px}\n"
".badge.warn{color:var(--danger);border-color:var(--danger)}\n"
"#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(80px);\n"
"background:var(--ink);color:var(--panel);padding:9px 16px;border-radius:8px;font-size:13px;\n"
"box-shadow:var(--shadow);opacity:0;transition:.22s;pointer-events:none;max-width:90vw}\n"
"#toast.on{opacity:1;transform:translateX(-50%) translateY(0)}\n"
"#toast.bad{background:var(--danger);color:#fff}\n"
".empty{padding:26px 14px;text-align:center;color:var(--dim);font-size:13px}\n"
".tabs{display:flex;gap:4px;margin:0 0 16px}\n"
".tabs button{border-radius:8px 8px 0 0;border-bottom-color:transparent;\n"
"padding:8px 18px;font-size:13.5px;color:var(--dim);background:0}\n"
".tabs button.on{background:var(--panel);border-color:var(--line);\n"
"border-bottom-color:var(--panel);color:var(--ink);font-weight:600}\n"
".tabs button:hover{color:var(--accent)}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}\n"
".stat{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:9px 11px}\n"
".stat b{display:block;font-size:17px;font-weight:600;font-variant-numeric:tabular-nums}\n"
".stat span{display:block;font-size:11.5px;color:var(--dim);text-transform:uppercase;\n"
"letter-spacing:.5px;margin-top:2px}\n"
".stat.warn b{color:var(--danger)}\n"
".warn{background:#8a1c12;color:#fff;border-radius:var(--radius);padding:12px 14px;\n"
"margin-bottom:18px;font-size:13.5px;line-height:1.5;box-shadow:var(--shadow)}\n"
".warn b{display:block;font-size:14px;margin-bottom:3px}\n"
".warn a{color:#fff}\n"
".lock{color:var(--dim);cursor:default}\n"
"@media(max-width:560px){\n"
".field label{width:100%}\n"
"input,select{max-width:none}\n"
"td.size{display:none}\n"
"}\n"
"</style></head><body>\n"
"<header><div class=\"hdr\">\n"
"<div class=\"keys\" aria-label=\"PRIFLOP\">\n"
"<span class=\"cap o\">P</span><span class=\"cap o\">R</span><span class=\"cap o\">I</span>\n"
"<span class=\"cap g\">F</span><span class=\"cap g\">L</span><span class=\"cap g\">O</span>\n"
"<span class=\"cap g\">P</span>\n"
"</div>\n"
"<div>\n"
"<div class=\"sub\"><b>Disk drive emulator</b></div>\n"
"<div class=\"sub\"><span class=\"dot\"></span><span id=\"netnow\">connecting...</span></div>\n"
"</div>\n"
"</div></header>\n"
"<main>\n"
"<div class=\"warn\" id=\"fmtwarn\" hidden>\n"
"<b>This drive reformatted itself.</b>\n"
"Its filesystem could not be mounted at startup, so it was rebuilt empty and\n"
"every file was lost \u2014 including <code>PRIFLOP.CFG</code>, which held the\n"
"Wi-Fi settings. The flash has been rewritten, so nothing can be recovered from\n"
"the device. Delete <code>REFORMATTED.TXT</code> to dismiss this.\n"
"</div>\n"
"<div class=\"warn\" id=\"defwarn\" hidden>\n"
"<b>The web password is still the default.</b>\n"
"Anyone who can reach this page can change the drive's settings, and the\n"
"Wi-Fi password is stored on the device. Set your own on the\n"
"<em>Settings</em> tab, under <em>Admin password</em>.\n"
"</div>\n"
"<div class=\"tabs\">\n"
"<button id=\"tabf\" class=\"on\" onclick=\"showTab('files')\">Files</button>\n"
"<button id=\"tabs2\" onclick=\"showTab('settings')\">Settings</button>\n"
"</div>\n"
"<div id=\"tab-files\">\n"
"<div class=\"tabs\">\n"
"<button id=\"volflash\" class=\"on\" onclick=\"showVol('flash')\">Flash</button>\n"
"<button id=\"volram\" onclick=\"showVol('ram')\">RAM (device 9)</button>\n"
"</div>\n"
"<section class=\"card\">\n"
"<div class=\"cardhead\">\n"
"<nav id=\"crumbs\"></nav>\n"
"<span class=\"grow\"></span>\n"
"<span class=\"badge\" id=\"count\"></span>\n"
"<span class=\"badge\" id=\"space\" title=\"Free space on the device\"></span>\n"
"<button class=\"mini\" id=\"mkdirbtn\" onclick=\"mkdir()\">New folder</button>\n"
"</div>\n"
"<div class=\"drop\" id=\"drop\">\n"
"<input type=\"file\" id=\"f\" multiple hidden>\n"
"Drop files here, or <button class=\"link\" onclick=\"document.getElementById('f').click()\">choose files</button>\n"
"<div class=\"bar\" id=\"bar\" hidden><i id=\"barfill\"></i></div>\n"
"</div>\n"
"<table>\n"
"<thead><tr><th>Name</th><th style=\"text-align:right\">Size</th><th></th></tr></thead>\n"
"<tbody id=\"rows\"></tbody>\n"
"</table>\n"
"<div class=\"empty\" id=\"empty\" hidden>This folder is empty.</div>\n"
"</section>\n"
"</div>\n"
"<div id=\"tab-settings\" hidden>\n"
"<section class=\"card\">\n"
"<div class=\"cardhead\"><h2>NETWORK</h2><span class=\"grow\"></span>\n"
"<span class=\"badge\" id=\"wifinow\">...</span></div>\n"
"<div class=\"body\">\n"
"<div class=\"field\"><label for=\"wmode\">Mode</label>\n"
"<select id=\"wmode\">\n"
"<option value=\"AP\">Create an access point</option>\n"
"<option value=\"STA\">Join a network</option>\n"
"</select></div>\n"
"<div class=\"field\"><label for=\"wssid\">Network to join</label>\n"
"<input id=\"wssid\" placeholder=\"SSID\"></div>\n"
"<div class=\"field\"><label for=\"wpass\">Its password</label>\n"
"<input id=\"wpass\" type=\"password\" placeholder=\"(unchanged)\"></div>\n"
"<div class=\"field\"><label for=\"wapssid\">Own AP name</label>\n"
"<input id=\"wapssid\" placeholder=\"PRIFLOP\"></div>\n"
"<div class=\"field\"><label for=\"wmdns\">Hostname (.local)</label>\n"
"<input id=\"wmdns\" placeholder=\"PRIFLOP\"></div>\n"
"<div class=\"field\"><label for=\"wcountry\">Country code</label>\n"
"<input id=\"wcountry\" maxlength=\"2\" placeholder=\"blank = worldwide\"></div>\n"
"<p class=\"hint\">Two letters, e.g. HU, DE, GB. Worldwide is the safe default but the\n"
"most conservative setting the radio has \u2014 it forces passive scanning on channels it\n"
"is unsure of, which makes the first join slow and sometimes fails outright. Setting\n"
"your actual country usually fixes both.</p>\n"
"<div class=\"row\"><button class=\"primary\" onclick=\"saveNet()\">Save network settings</button></div>\n"
"<p class=\"hint\">The first two are the network to <em>join</em>. The AP name is what this\n"
"device <em>broadcasts</em> when it is its own access point, including after a failed\n"
"join &mdash; keep it different from your real network's name. The hostname advertises\n"
"&lt;name&gt;.local over mDNS, but only when joined to a network, not in access-point mode,\n"
"where the multicast join is known to break Wi-Fi on this chip. A wrong password cannot\n"
"lock you out: a failed join falls back to the device's own access point. Leave a password\n"
"blank to keep the current one; a new one needs 8+ characters (WPA2 minimum).</p>\n"
"</div>\n"
"</section>\n"
"<section class=\"card\">\n"
"<div class=\"cardhead\"><h2>ADMIN PASSWORD</h2><span class=\"grow\"></span>\n"
"<span class=\"badge\" id=\"adminnow\">...</span></div>\n"
"<div class=\"body\">\n"
"<div class=\"field\"><label for=\"wadmin\">New password</label>\n"
"<input id=\"wadmin\" type=\"password\" placeholder=\"(unchanged)\"></div>\n"
"<div class=\"row\"><button class=\"primary\" onclick=\"saveAdmin()\">Change password</button></div>\n"
"<p class=\"hint\">Any username is accepted; only the password is checked. Saved to\n"
"PRIFLOP.CFG and applied after a reset. Authentication cannot be switched off here\n"
"&mdash; the config file is downloadable through this same interface, so an unprotected\n"
"device would hand out its Wi-Fi password to anyone on the network. Note that both this\n"
"password and the Wi-Fi one travel over plain HTTP; treat this interface as\n"
"LAN-only.</p>\n"
"</div>\n"
"</section>\n"
"<section class=\"card\">\n"
"<div class=\"cardhead\"><h2>DEVICE</h2><span class=\"grow\"></span>\n"
"<button class=\"mini\" onclick=\"loadStats()\">Refresh</button></div>\n"
"<div class=\"body\">\n"
"<div class=\"field\"><label for=\"waddr\">Base drive address</label>\n"
"<input id=\"waddr\" type=\"number\" min=\"4\" max=\"30\"></div>\n"
"<div class=\"row\"><button class=\"primary\" onclick=\"saveAddr()\">Change address</button></div>\n"
"<p class=\"hint\">The RAM drive (device 9, see the Files tab) always answers one\n"
"address above this one. Takes effect immediately, no reset needed.</p>\n"
"<div class=\"field\"><label for=\"wexthide\">Extension hiding</label>\n"
"<select id=\"wexthide\"><option value=\"1\">On &mdash; LOAD\"NAME\" finds NAME.PRG</option>\n"
"<option value=\"0\">Off &mdash; the full name is needed</option></select></div>\n"
"<div class=\"row\"><button class=\"primary\" onclick=\"saveExtHide()\">Save</button></div>\n"
"<p class=\"hint\">The same as XE+ / XE- followed by XW from the C64. Saved immediately,\n"
"and kept across a reset or a reflash.</p>\n"
"<div class=\"field\"><label for=\"wpturbo\">Primo turbo</label>\n"
"<select id=\"wpturbo\"><option value=\"1\">On &mdash; the Primo's DOS loads and saves fast</option>\n"
"<option value=\"0\">Off &mdash; XT requests are refused</option></select></div>\n"
"<div class=\"row\"><button class=\"primary\" onclick=\"savePTurbo()\">Save</button></div>\n"
"<p class=\"hint\">The same as XT+ / XT- followed by XW from the Primo. Saved immediately,\n"
"and kept across a reset or a reflash. Our own Primo DOS needs it on.</p>\n"
"<div class=\"grid\" id=\"stats\"></div>\n"
"<div class=\"row\">\n"
"<button onclick=\"reboot()\">Reset</button>\n"
"<button class=\"danger\" onclick=\"bootsel()\">Enter BOOTSEL</button>\n"
"</div>\n"
"<p class=\"hint\">Reset flushes pending writes first, then restarts &mdash; use it to apply\n"
"saved settings. BOOTSEL stops the drive and exposes the USB flashing volume; it stays\n"
"that way until reflashed or power-cycled.</p>\n"
"</div>\n"
"</section>\n"
"</div>\n"
"</main>\n"
"<div id=\"toast\"></div>\n"
"<script>\n"
"var curDir = '/';\n"
"var curVol = 'flash';\n"
"function $(id){return document.getElementById(id);}\n"
"function esc(s){return encodeURIComponent(s);}\n"
"function join(d,n){return (d.endsWith('/')?d:d+'/')+n;}\n"
"/* Two panes rather than one long page: the network and admin settings are\n"
"needed when first setting the drive up or when something is wrong, not\n"
"while moving files around. Purely markup -- the page is a static string\n"
"in flash, so this costs no RAM and no requests.\n"
"The page always opens on Files. It used to remember the last tab in\n"
"localStorage and jump to Settings while the default password was set,\n"
"which made every reload land on Settings; nothing is remembered now. */\n"
"function showTab(which){\n"
"var f = which !== 'settings';\n"
"$('tab-files').hidden = !f;\n"
"$('tab-settings').hidden = f;\n"
"$('tabf').className = f ? 'on' : '';\n"
"$('tabs2').className = f ? '' : 'on';\n"
"if(!f) loadStats();\n"
"}\n"
"/* Flash and RAM (device 9) are two independent littlefs volumes -- see\n"
"fs_bridge.h's fs_volume_t. RAM is deliberately flat (ramops.c stubs\n"
"mkdir/subdirectories), so switching to it always resets to root and\n"
"hides New folder rather than offering something that can only 404. */\n"
"function showVol(which){\n"
"curVol=which;\n"
"$('volflash').className = which==='flash' ? 'on' : '';\n"
"$('volram').className = which==='ram' ? 'on' : '';\n"
"$('mkdirbtn').hidden = which==='ram';\n"
"list('/');\n"
"}\n"
"var toastTimer=0;\n"
"function toast(msg,bad){\n"
"var t=$('toast'); t.textContent=msg; t.className=(bad?'on bad':'on');\n"
"clearTimeout(toastTimer); toastTimer=setTimeout(function(){t.className='';},3200);\n"
"}\n"
"function human(n){\n"
"if(n<1024) return n+' B';\n"
"if(n<1024*1024) return (n/1024).toFixed(n<10240?1:0)+' KB';\n"
"return (n/1048576).toFixed(1)+' MB';\n"
"}\n"
"/* Free space. Shown as a share of the whole volume because '2.1 MB'\n"
"alone means nothing without knowing the device holds 3 MB. Turns red\n"
"under 10% left, which is roughly one D81 image. */\n"
"function space(free,total){\n"
"var el=$('space'); if(!el) return;\n"
"if(free<0||!total){el.textContent=''; el.className='badge'; return;}\n"
"var pct=free/total;\n"
"el.textContent=human(free)+' free of '+human(total);\n"
"el.className='badge'+(pct<0.10?' warn':'');\n"
"}\n"
"function crumbs(p){\n"
"var el=$('crumbs'); el.textContent='';\n"
"var mk=function(label,target){\n"
"var a=document.createElement('a'); a.textContent=label;\n"
"a.onclick=function(){list(target);}; return a;\n"
"};\n"
"el.appendChild(mk('/', '/'));\n"
"var parts=p.split('/').filter(Boolean), acc='';\n"
"for(var i=0;i<parts.length;i++){\n"
"acc+='/'+parts[i];\n"
"el.appendChild(document.createTextNode(i?' / ':''));\n"
"el.appendChild(mk(parts[i], acc));\n"
"}\n"
"}\n"
"/* Fetches every page of a directory. The device answers with as many\n"
"entries as fit one JSON buffer and returns \"next\" when there are more;\n"
"it used to silently truncate instead. */\n"
"async function list(dir){\n"
"var all=[], offset=0, path=dir, guard=0, free=-1, total=0;\n"
"try{\n"
"while(true){\n"
"var r=await fetch('/api/list?dir='+esc(dir)+'&offset='+offset+'&vol='+curVol);\n"
"if(!r.ok){toast('List failed: '+r.status,1);return;}\n"
"var j=await r.json();\n"
"path=j.path; all=all.concat(j.entries||[]);\n"
"if(typeof j.free==='number'){free=j.free; total=j.total;}\n"
"if(!j.next || ++guard>64) break;\n"
"offset=j.next;\n"
"}\n"
"}catch(e){toast('List failed',1);return;}\n"
"curDir=path; render(path,all); space(free,total);\n"
"}\n"
"function render(path,entries){\n"
"crumbs(path);\n"
"var tb=$('rows'); tb.textContent='';\n"
"entries.sort(function(a,b){return (b.dir-a.dir)||a.name.localeCompare(b.name);});\n"
"$('count').textContent=entries.length+(entries.length===1?' item':' items');\n"
"$('empty').hidden = entries.length>0 || path!=='/';\n"
"if(path!=='/') row('..',true,0,parent_(path),true);\n"
"for(var i=0;i<entries.length;i++)\n"
"row(entries[i].name, entries[i].dir, entries[i].size, join(path,entries[i].name), false, entries[i].protected);\n"
"}\n"
"function parent_(p){var q=p.split('/').filter(Boolean);q.pop();return '/'+q.join('/');}\n"
"function row(name,isDir,size,full,isUp,prot){\n"
"var tr=document.createElement('tr');\n"
"var td=document.createElement('td');\n"
"var wrap=document.createElement('span'); wrap.className='name';\n"
"var ic=document.createElement('span'); ic.className='ic';\n"
"ic.textContent = isUp ? '↰' : (isDir ? '\\u{1F4C1}' : (prot ? '\\u{1F512}' : '\\u{1F4C4}'));\n"
"wrap.appendChild(ic);\n"
"var a=document.createElement('a');\n"
"a.textContent=name;\n"
"if(isDir){ a.href='#'; a.onclick=function(e){e.preventDefault();list(full||'/');}; }\n"
"else if(prot){ a.removeAttribute('href'); a.className='lock';\n"
"a.title='Held on the device; not downloadable because it '+\n"
"'contains the Wi-Fi password. Edit it through the '+\n"
"'settings below.'; }\n"
"else { a.href='/files'+full+'?vol='+curVol; a.download=name; }\n"
"wrap.appendChild(a); td.appendChild(wrap); tr.appendChild(td);\n"
"var s=document.createElement('td'); s.className='size';\n"
"s.textContent=isDir?'':human(size); tr.appendChild(s);\n"
"var act=document.createElement('td'); act.className='act';\n"
"if(!isUp){\n"
"var rn=document.createElement('button'); rn.className='mini'; rn.textContent='Rename';\n"
"rn.onclick=function(){renameEntry(full,name);}; act.appendChild(rn);\n"
"var dl=document.createElement('button'); dl.className='mini danger'; dl.textContent='Delete';\n"
"dl.style.marginLeft='6px';\n"
"dl.onclick=function(){delEntry(full,name);}; act.appendChild(dl);\n"
"}\n"
"tr.appendChild(act);\n"
"$('rows').appendChild(tr);\n"
"}\n"
"/* A directory with files in it comes back 409 \"notempty\" rather than\n"
"just failing, so the delete can be offered again as a recursive one --\n"
"but only after saying how many files that is. The device never\n"
"recurses unless explicitly asked. */\n"
"async function delEntry(path,name){\n"
"if(!confirm('Delete '+name+'?')) return;\n"
"var r=await fetch('/files'+path+'?vol='+curVol,{method:'DELETE'});\n"
"if(r.ok){ toast('Deleted '+name); list(curDir); return; }\n"
"var body='';\n"
"try{ body=await r.text(); }catch(e){}\n"
"if(r.status===409 && body.indexOf('inuse')>=0){\n"
"toast('Cannot delete: the drive has that image mounted. Unmount it first.',1);\n"
"return;\n"
"}\n"
"if(r.status===409 && body.indexOf('notempty')>=0){\n"
"var n='';\n"
"try{\n"
"var j=await (await fetch('/api/list?dir='+esc(path)+'&vol='+curVol)).json();\n"
"n=(j.entries||[]).length;\n"
"}catch(e){}\n"
"if(!confirm(name+' is not empty'+(n!==''?(' ('+n+' item'+(n===1?'':'s')+')'):'')+\n"
"'.\\n\\nDelete everything inside it as well?')) return;\n"
"toast('Deleting '+name+'...');\n"
"var r2=await fetch('/files'+path+'?recursive=1&vol='+curVol,{method:'DELETE'});\n"
"if(r2.ok){ toast('Deleted '+name); }\n"
"else {\n"
"var b2=''; try{ b2=await r2.text(); }catch(e){}\n"
"toast(b2.indexOf('notempty')>=0\n"
"? 'Not deleted: it contains a subdirectory. Empty that first.'\n"
": ('Delete failed: '+r2.status), 1);\n"
"}\n"
"list(curDir); return;\n"
"}\n"
"toast('Delete failed: '+r.status,1);\n"
"list(curDir);\n"
"}\n"
"async function renameEntry(path,oldName){\n"
"var n=prompt('Rename to:',oldName);\n"
"if(!n||n===oldName) return;\n"
"var r=await fetch('/api/rename?from='+esc(path)+'&to='+esc(join(curDir,n))+'&vol='+curVol,{method:'POST'});\n"
"toast(r.ok?'Renamed':('Rename failed: '+r.status), !r.ok);\n"
"list(curDir);\n"
"}\n"
"async function mkdir(){\n"
"var n=prompt('New folder name:');\n"
"if(!n) return;\n"
"var r=await fetch('/api/mkdir?dir='+esc(join(curDir,n))+'&vol='+curVol,{method:'POST'});\n"
"toast(r.ok?'Folder created':('mkdir failed: '+r.status), !r.ok);\n"
"list(curDir);\n"
"}\n"
"/* XHR rather than fetch: it reports upload progress, and on a device this\n"
"slow a 200KB PRG is long enough that a silent bar is worse than none. */\n"
"function putFile(file){\n"
"return new Promise(function(res,rej){\n"
"var x=new XMLHttpRequest();\n"
"x.open('POST','/files'+join(curDir,file.name)+'?vol='+curVol);\n"
"x.upload.onprogress=function(e){\n"
"if(e.lengthComputable) $('barfill').style.width=(100*e.loaded/e.total)+'%';\n"
"};\n"
"x.onload=function(){ (x.status>=200&&x.status<300)?res():rej(x.status); };\n"
"x.onerror=function(){rej('network');};\n"
"x.send(file);\n"
"});\n"
"}\n"
"async function upload(files){\n"
"if(!files||!files.length) return;\n"
"$('bar').hidden=false;\n"
"for(var i=0;i<files.length;i++){\n"
"$('barfill').style.width='0%';\n"
"toast('Uploading '+files[i].name+(files.length>1?(' ('+(i+1)+'/'+files.length+')'):'')+'...');\n"
"try{ await putFile(files[i]); }\n"
"catch(err){ toast('Upload failed: '+err,1); $('bar').hidden=true; list(curDir); return; }\n"
"}\n"
"$('bar').hidden=true;\n"
"toast(files.length>1?(files.length+' files uploaded'):'Uploaded');\n"
"list(curDir);\n"
"}\n"
"$('f').onchange=function(){upload(this.files); this.value='';};\n"
"var dz=$('drop');\n"
"['dragenter','dragover'].forEach(function(ev){\n"
"dz.addEventListener(ev,function(e){e.preventDefault();dz.classList.add('over');});\n"
"});\n"
"['dragleave','drop'].forEach(function(ev){\n"
"dz.addEventListener(ev,function(e){e.preventDefault();dz.classList.remove('over');});\n"
"});\n"
"dz.addEventListener('drop',function(e){ upload(e.dataTransfer.files); });\n"
"async function reboot(){\n"
"if(!confirm('Reset the device now?')) return;\n"
"toast('Resetting...');\n"
"try{ await fetch('/api/reboot',{method:'POST'}); }catch(e){}\n"
"}\n"
"async function bootsel(){\n"
"if(!confirm('Enter BOOTSEL (USB flashing) mode? The drive stops until reflashed or power-cycled.')) return;\n"
"toast('Entering BOOTSEL...');\n"
"try{ await fetch('/api/bootsel',{method:'POST'}); }catch(e){}\n"
"}\n"
"/* The counters the firmware keeps about itself. They exist because each\n"
"one settled a question during development that guesswork had got wrong:\n"
"resets told a real watchdog recovery from a fluke, the fault PC told a\n"
"crash from a hang, and ATN-during-erase is the measurement the whole\n"
"flash-shield design rests on. */\n"
"function human2(n){\n"
"if(n<1000) return ''+n;\n"
"if(n<1000000) return (n/1000).toFixed(n<10000?1:0)+'k';\n"
"return (n/1000000).toFixed(1)+'M';\n"
"}\n"
"async function loadStats(){\n"
"var el=$('stats');\n"
"try{\n"
"var d=await (await fetch('/api/stats')).json();\n"
"if(document.activeElement!==$('waddr')) $('waddr').value=d.device_address;\n"
"if(document.activeElement!==$('wexthide')) $('wexthide').value=''+d.ext_hiding;\n"
"if(document.activeElement!==$('wpturbo')) $('wpturbo').value=''+d.pt_on;\n"
"var armed=(parseInt(d.wdog,16)&0x40000000)!==0;\n"
"var cells=[\n"
"['Stack used', d.stack_used+' / '+d.stack_total+' B', d.stack_used>d.stack_total*0.8],\n"
"['Resets', d.boots, d.boots>1],\n"
"['Watchdog', armed?'armed':'OFF', !armed],\n"
"['Last fault', d.faulted? d.fault_pc : 'none', !!d.faulted],\n"
"['Flash writes', human2(d.flash_ops), false],\n"
"['Longest flash op', (d.flash_op_max_us/1000).toFixed(1)+' ms', false],\n"
"['ATN during erase', d.atn_irqs, false],\n"
"['ATN bails', d.atn_bails, d.atn_bails>0],\n"
"['Bus bytes in/out', human2(d.rx)+' / '+human2(d.tx), false],\n"
"['Worst byte in', d.getc_max+' us', false],\n"
"['Worst byte out', d.putc_max+' us', false],\n"
"['Worst gap between bytes', d.gap_max+' us', false],\n"
"['Time bit-banging', (d.busy_ms/1000).toFixed(1)+' s', false],\n"
"['IEC receive', d.pio? (d.pio_owns? 'PIO':'software (PIO shadowing)') : 'software (no PIO)', false],\n"
"['PIO bytes rx / tx', human2(d.pio_ok)+' / '+human2(d.pio_txb), d.pio_bad>0],\n"
"['IEC transmit', d.pio_tx? 'PIO':'software', false],\n"
"['PIO tx abort / atn', d.pio_txa+' / '+d.pio_txn, d.pio_txa>0],\n"
"['JiffyDOS bytes', human2(d.jf_rx+d.jf_tx), false],\n"
"['JiffyDOS rate', d.jf_us? Math.round((d.jf_rx+d.jf_tx)/(d.jf_us/1e6))+' B/s' : '-', false]\n"
"];\n"
"el.textContent='';\n"
"cells.forEach(function(c){\n"
"var w=document.createElement('div');\n"
"w.className='stat'+(c[2]?' warn':'');\n"
"var b=document.createElement('b'); b.textContent=c[1];\n"
"var s2=document.createElement('span'); s2.textContent=c[0];\n"
"w.appendChild(b); w.appendChild(s2); el.appendChild(w);\n"
"});\n"
"var fw=$('fmtwarn'); if(fw) fw.hidden = !d.reformatted;\n"
"}catch(e){ el.textContent='Statistics unavailable.'; }\n"
"}\n"
"async function loadNet(){\n"
"try{\n"
"var w=await (await fetch('/api/wifi')).json();\n"
"$('wmode').value=w.mode; $('wssid').value=w.ssid;\n"
"$('wapssid').value=w.ap_ssid; $('wmdns').value=w.mdns;\n"
"$('wcountry').value=w.country||'';\n"
"$('adminnow').textContent = w.default_admin ? 'default' : (w.has_admin?'set':'not set');\n"
"$('defwarn').hidden = !w.default_admin;\n"
"var s = (w.mode==='AP')\n"
"? ('access point “'+w.ap_ssid+'”'+(w.has_ap_password?'':' (open)'))\n"
": ('joined “'+w.ssid+'”');\n"
"$('wifinow').textContent=s; $('netnow').textContent=s;\n"
"}catch(e){ $('netnow').textContent='unknown'; }\n"
"}\n"
"/* The device owns the config file format (netconfig.c). This used to\n"
"fetch PRIFLOP.CFG, patch KEY=VALUE lines here and POST the whole file\n"
"back, which meant the layout was implemented twice. */\n"
"async function post(params){\n"
"var q=Object.keys(params).filter(function(k){return params[k]!=='';})\n"
".map(function(k){return k+'='+esc(params[k]);}).join('&');\n"
"return fetch('/api/wifi?'+q,{method:'POST'});\n"
"}\n"
"async function saveNet(){\n"
"var ssid=$('wssid').value.trim(), ap=$('wapssid').value.trim(), pass=$('wpass').value;\n"
"if(!ssid||!ap){ toast('Network name and AP name are both required.',1); return; }\n"
"if(pass && pass.length<8){ toast('Password needs 8+ characters, or leave it blank.',1); return; }\n"
"var r=await post({mode:$('wmode').value,ssid:ssid,apssid:ap,\n"
"mdns:$('wmdns').value.trim(),country:$('wcountry').value.trim(),pass:pass});\n"
"if(r.ok){ $('wpass').value=''; toast('Saved. Reset the device to apply.'); loadNet(); }\n"
"else toast('Save failed: '+r.status,1);\n"
"}\n"
"async function saveAdmin(){\n"
"var a=$('wadmin').value;\n"
"if(!a){ toast('Enter a new password first.',1); return; }\n"
"var r=await post({admin:a});\n"
"if(r.ok){ $('wadmin').value=''; toast('Password saved. Reset the device to apply.'); loadNet(); }\n"
"else toast('Save failed: '+r.status,1);\n"
"}\n"
"async function saveAddr(){\n"
"var n=parseInt($('waddr').value,10);\n"
"if(!n || n<4 || n>30){ toast('Address must be 4-30.',1); return; }\n"
"var r=await fetch('/api/deviceaddr?addr='+n,{method:'POST'});\n"
"if(r.ok){ toast('Address changed to '+n+' (RAM drive is now '+(n+1)+').'); loadStats(); }\n"
"else toast('Save failed: '+r.status,1);\n"
"}\n"
"async function saveExtHide(){\n"
"var v=$('wexthide').value;\n"
"var r=await fetch('/api/settings?exthide='+v,{method:'POST'});\n"
"if(r.ok){ toast('Extension hiding '+(v==='1'?'on':'off')+', saved.'); setTimeout(loadStats,500); }\n"
"else toast('Save failed: '+r.status,1);\n"
"}\n"
"async function savePTurbo(){\n"
"var v=$('wpturbo').value;\n"
"var r=await fetch('/api/settings?primoturbo='+v,{method:'POST'});\n"
"if(r.ok){ toast('Primo turbo '+(v==='1'?'on':'off')+', saved.'); setTimeout(loadStats,500); }\n"
"else toast('Save failed: '+r.status,1);\n"
"}\n"
"list('/');\n"
"loadNet();\n"
"loadStats();\n"
"</script>\n"
"</body></html>\n";

/*--------------------------------------------------------------------------
   Connection state
---------------------------------------------------------------------------*/

/* Raised from 3 on 2026-09-05 after measuring the cost of the old value:
   four clients stalled mid-upload made the server answer nothing at all
   for six seconds (the idle reaper's HTTP_IDLE_POLL_LIMIT * ~2s), because
   on_accept() tcp_abort()s when every slot is busy -- a dropped request,
   not a slow one. Each slot costs ~19KB, almost all of it upload_buf,
   which cannot shrink below TCP_WND without reintroducing the refusal
   loop that HTTP_UPLOAD_BUF_SIZE's comment describes. So the fix is more
   slots rather than smaller ones; at 8 the measured blackout goes away
   and BSS is still under 300KB of the RP2350's 520KB. */
#define HTTP_MAX_CONNS      8
#define HTTP_HEADER_BUF_MAX 1024
#define HTTP_PATH_MAX       200
#define HTTP_SEND_CHUNK     1024
#define HTTP_JSON_BUF_MAX   4096

/* MUST be >= TCP_WND. lwIP can hand on_recv() a coalesced pbuf chain up
   to a full receive window, and the upload backpressure below refuses any
   pbuf it cannot buffer *whole* (partial consumption would lose the
   remainder). If this were smaller than the window, a chain larger than
   it would be refused forever even with the buffer completely empty --
   draining cannot make room that does not exist. TCP_WND is 8*TCP_MSS. */
#define HTTP_UPLOAD_BUF_SIZE (TCP_WND + TCP_MSS)

/* on_poll() runs every ~2s (tcp_poll interval 4 below), so this is a
   ~6s silence before a connection is reclaimed. Long enough that no
   healthy transfer trips it, short enough that abandoned uploads free
   their slot quickly. */
#define HTTP_IDLE_POLL_LIMIT 3
#define HTTP_AUTHZ_MAX      140  /* "Basic " + a generous base64 credential */

typedef enum {
  HTTP_ST_HEADERS,
  HTTP_ST_UPLOAD_BODY,
  HTTP_ST_SEND_FILE,    /* streaming a FIL out in chunks */
  HTTP_ST_SEND_PAGE,    /* streaming PAGE_HTML out in chunks */
  HTTP_ST_FS_PENDING,   /* LIST/MKDIR/RENAME/DELETE handed to fs_bridge, awaiting the response */
  HTTP_ST_DONE,         /* everything queued; waiting for it to drain */
} http_state_t;

typedef struct {
  bool in_use;
  struct tcp_pcb *pcb;
  http_state_t state;

  /* request accumulation/parsing */
  char header_buf[HTTP_HEADER_BUF_MAX];
  uint16_t header_len;
  bool headers_seen;

  char method[8];
  char raw_path[HTTP_PATH_MAX];   /* undecoded, no query string */
  char raw_query[HTTP_PATH_MAX];  /* undecoded, everything after '?' */
  char authz[HTTP_AUTHZ_MAX];     /* raw Authorization header value, if any */

  uint32_t content_length;
  uint32_t body_received;

  /* fs_req doubles as the outgoing request (op/path/path2/handle, set
     by handle_list()/handle_download()/etc. before entering
     HTTP_ST_FS_PENDING, HTTP_ST_SEND_FILE, or HTTP_ST_UPLOAD_BODY) and,
     once fs_bridge_try_take_response() succeeds, the incoming response
     (result/data/data_len/handle/file_size) -- see pump_fs_request()
     (one-shot ops) and pump_send_file()/on_recv()'s upload handling
     (streaming ops, which reuse this same struct/flag pair across many
     round trips instead of just one). fs_handle is the open FatFs
     handle (an fs_bridge.c table index, never a real FIL -- see
     fs_bridge.h) once FS_OP_OPEN_READ/OPEN_WRITE has completed; -1
     means none open. chunk_ready is pump_send_file()'s own "have a
     read chunk in hand, still need to tcp_write() it" flag. */
  fs_request_t fs_req;
  bool fs_submitted;
  int fs_handle;
  bool chunk_ready;

  /* Upload body accumulation -- deliberately NOT c->fs_req.data: start_upload()
     submits FS_OP_OPEN_WRITE asynchronously, and on_recv() can see body
     bytes (e.g. a small file's whole POST arriving in the same TCP
     segment as its headers) before that round trip completes and
     c->state becomes HTTP_ST_UPLOAD_BODY. is_uploading is set the
     instant start_upload() runs (not once the bridge confirms the open)
     so on_recv() never has a window where it has to drop those bytes;
     they land in upload_buf, sized comfortably larger than one TCP
     segment (TCP_MSS=1460, lwipopts.h) so a single recv rarely fills
     it. pump_upload() copies from here into fs_req.data only once
     fs_req is actually free to reuse (no open/read/write/close round
     trip already borrowing it). */
  bool is_uploading;
  uint8_t upload_buf[HTTP_UPLOAD_BUF_SIZE];
  uint32_t upload_buf_len;

  /* Consecutive on_poll() ticks with no network progress. lwIP calls
     on_poll() every ~2s (tcp_poll interval 4), and any recv or ack resets
     this, so it only climbs on a connection that has genuinely stopped
     talking -- an aborted upload, or a client that vanished without a
     FIN/RST. Without a timeout those sockets sat in their slot until lwIP
     eventually noticed, and with only HTTP_MAX_CONNS of them a few
     aborted uploads made the server unreachable for ~10s. */
  uint8_t idle_polls;

  /* How much of PAGE_HTML has been handed to tcp_write. The page used to
     go out in one call, which quietly capped it at TCP_SND_BUF (11680
     bytes, lwipopts.h) -- past that tcp_write returns ERR_MEM and
     queue_write() closes the connection, so the UI would simply stop
     loading with no other symptom. At 9396 bytes it was ~2KB from that
     cliff. Streaming it removes the ceiling instead of budgeting against
     it. */
  uint32_t page_sent;

  bool close_when_drained;
  uint32_t unacked;               /* bytes handed to tcp_write, not yet acked */
} http_conn_t;

static http_conn_t conns[HTTP_MAX_CONNS];

/* Open file handles whose connection went away before their FS_OP_CLOSE
   could be handed to the bridge (submit fails when the slot is busy).
   conn_release() cannot block or retry, so the handle would simply leak
   on core 0 -- and with only FS_BRIDGE_MAX_HANDLES of them, three
   interrupted uploads made every later upload fail instantly with a 500,
   because do_open_write()'s alloc_handle() had nothing left (the bridge
   allows FS_BRIDGE_MAX_HANDLES, currently 3). Retried
   from httpd_poll() until the bridge accepts them. Confirmed on hardware
   2026-09-04: uploads that failed permanently started working again
   after a reboot cleared the table. */
static int pending_close[HTTP_MAX_CONNS];  /* at most one open handle per connection */
static uint8_t pending_close_count;

static void queue_handle_close(int handle) {
  if (handle < 0) return;
  if (pending_close_count >= (uint8_t)(sizeof(pending_close) / sizeof(pending_close[0]))) {
    return; /* cannot happen: at most one pending close per handle */
  }
  pending_close[pending_close_count++] = handle;
}

static void drain_pending_closes(void) {
  while (pending_close_count > 0) {
    fs_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = FS_OP_CLOSE;
    req.handle = pending_close[pending_close_count - 1];
    if (!fs_bridge_submit_discard(&req)) {
      return;   /* bridge busy -- try again next tick */
    }
    pending_close_count--;
  }
}

/*--------------------------------------------------------------------------
   Small helpers
---------------------------------------------------------------------------*/

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decodes %XX escapes. '+' is passed through literally -- our own UI's
   encodeURIComponent() never produces it, so treating it as space would
   be wrong here. */
static void url_decode(char *dst, const char *src, size_t dstsize) {
  size_t o = 0;
  while (*src && o + 1 < dstsize) {
    if (src[0] == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
      dst[o++] = (char)((hexval(src[1]) << 4) | hexval(src[2]));
      src += 3;
    } else {
      dst[o++] = *src++;
    }
  }
  dst[o] = 0;
}

/* Finds "key=" in a raw (undecoded) query string and url-decodes its
   value into out. Returns false if key isn't present. */
static bool get_query_param(const char *query, const char *key, char *out, size_t outsize) {
  size_t keylen = strlen(key);
  const char *p = query;
  while (p && *p) {
    if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
      p += keylen + 1;
      const char *end = strchr(p, '&');
      size_t rawlen = end ? (size_t)(end - p) : strlen(p);
      char raw[HTTP_PATH_MAX];
      if (rawlen >= sizeof(raw)) rawlen = sizeof(raw) - 1;
      memcpy(raw, p, rawlen);
      raw[rawlen] = 0;
      url_decode(out, raw, outsize);
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

/* Shared by every file-manager route: ?vol=ram selects the RAM drive
   (device 9, ramops.c); anything else, including no ?vol= at all,
   means the flash volume -- so every existing bookmark/script that
   predates this parameter keeps working unchanged. */
static fs_volume_t parse_vol(const char *query) {
  char v[8];
  if (get_query_param(query, "vol", v, sizeof(v)) && !strcmp(v, "ram"))
    return FS_VOLUME_RAM;
  return FS_VOLUME_FLASH;
}

/* Rejects anything that isn't a clean absolute path -- no "..", must
   start with '/'. Good enough for a LAN hobby device serving one
   FatFs volume with no other files to escape to. */
static bool path_is_safe(const char *path) {
  if (path[0] != '/') return false;
  if (strstr(path, "..")) return false;
  return true;
}

static const char *mime_type(const char *path) {
  const char *ext = strrchr(path, '.');
  if (!ext) return "application/octet-stream";
  if (!strcasecmp(ext, ".html")) return "text/html";
  if (!strcasecmp(ext, ".json")) return "application/json";
  if (!strcasecmp(ext, ".txt"))  return "text/plain";
  return "application/octet-stream";
}

/*--------------------------------------------------------------------------
   HTTP Basic Auth (2026-09-02, re-added after a same-night revert swept
   it up along with unrelated WiFi-config work) -- optional: only enforced once
   httpd_set_admin_password() has been called with a non-empty password.
   Deliberately compile-time only for now (ADMIN_PASSWORD, see
   scripts/pico/CMakeLists.txt), not yet wired to a runtime config file
   -- that's its own separate backlog item, landed independently so this
   step doesn't depend on it. Any username is accepted; only the
   password half is checked, with a plain (non-constant-time) strcmp --
   acceptable for this device's stated threat model (a hobby device on
   a trusted home LAN), not worth the extra
   complexity of a timing-safe compare here.
---------------------------------------------------------------------------*/

static char g_admin_password[64] = "";

void httpd_set_admin_password(const char *password) {
  strncpy(g_admin_password, password, sizeof(g_admin_password) - 1);
  g_admin_password[sizeof(g_admin_password) - 1] = 0;
}

/* True if this request is allowed to proceed: no password configured
   (open, matching pre-auth behavior), or a valid "Authorization: Basic
   <base64(user:pass)>" header whose password half matches. */
/* Case-insensitively matches the config file, whatever path spelling the
   request used to reach it. */
static bool path_is_protected(const char *path) {
  const char *p = path;
  if (*p == '/') p++;
  return strcasecmp(p, NETCONFIG_PATH + 1) == 0;
}

static bool check_auth(http_conn_t *c) {
  if (!g_admin_password[0]) return true;
  if (strncmp(c->authz, "Basic ", 6) != 0) return false;

  char decoded[96];
  base64_decode(c->authz + 6, decoded, sizeof(decoded));

  char *colon = strchr(decoded, ':');
  const char *pass = colon ? colon + 1 : decoded;
  /* Via netconfig so the stored form is defined in exactly one place --
     see netconfig_admin_matches(). g_admin_password is kept only as the
     "is authentication enabled at all" flag. */
  return netconfig_admin_matches(&network_cfg, pass);
}

/*--------------------------------------------------------------------------
   Connection lifecycle
---------------------------------------------------------------------------*/

static void conn_cleanup(http_conn_t *c) {
  if (c->fs_handle >= 0) {
    /* Best-effort: core 1 must not call FatFs directly even here, so a
       fire-and-forget FS_OP_CLOSE via the bridge is the only option --
       no synchronous fallback to reach for. If the single-slot bridge
       happens to be busy with someone else's request at this exact
       moment, this handle leaks (fs_bridge.c's small fixed table has no
       other reclaim path); an accepted, rare edge case (an abnormal
       mid-transfer disconnect colliding with another connection's
       request), not a normal code path. */
    fs_request_t close_req;
    memset(&close_req, 0, sizeof(close_req));
    close_req.op = FS_OP_CLOSE;
    close_req.handle = c->fs_handle;
    if (!fs_bridge_submit_discard(&close_req)) {
      /* Bridge busy right now -- remember it, or the handle is lost. */
      queue_handle_close(c->fs_handle);
    }
    c->fs_handle = -1;
  }

  /* Any request this connection still had in flight (typically an
     FS_OP_WRITE from an interrupted upload) would otherwise strand the
     bridge's single slot: core 0 completes it, nobody ever collects the
     reply, and every later filesystem request blocks forever. */
  fs_bridge_abandon(c);
  c->fs_submitted = false;
  c->in_use = false;
  c->pcb = NULL;
}

/* Synchronously tears down the pcb. Only safe to call from a context
   that isn't about to keep using `pcb` afterward (see on_recv, which
   guards against calling back into a pcb this may have just freed). */
static void conn_close(http_conn_t *c) {
  struct tcp_pcb *pcb = c->pcb;
  conn_cleanup(c);
  if (pcb) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_close(pcb);
  }
}

static void try_finish_close(http_conn_t *c) {
  if (c->state == HTTP_ST_DONE && c->close_when_drained && c->unacked == 0) {
    conn_close(c);
  }
}

/*--------------------------------------------------------------------------
   Response helpers
---------------------------------------------------------------------------*/

/* Queues data for sending. Every caller in this file keeps individual
   writes well under TCP_SND_BUF (see the buffer size caps throughout),
   so this always completes in a single tcp_write() -- no partial-write
   bookkeeping needed. TCP_WRITE_FLAG_COPY means lwIP copies the data
   into its own pbufs immediately, so `data` can be freed/reused by the
   caller as soon as this returns. */
static void queue_write(http_conn_t *c, const void *data, uint32_t len) {
  if (len == 0) return;
  err_t err = tcp_write(c->pcb, data, (u16_t)len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK) {
    conn_close(c);
    return;
  }
  c->unacked += len;
  tcp_output(c->pcb);
}

/* Queues a fully-formed response (headers+body already concatenated)
   and marks the connection to close once it drains. `owned` buffers are
   freed here (safe immediately: TCP_WRITE_FLAG_COPY already copied the
   bytes out). */
static void send_mem(http_conn_t *c, const char *data, uint32_t len, bool owned) {
  queue_write(c, data, len);
  if (c->in_use) {
    c->state = HTTP_ST_DONE;
    c->close_when_drained = true;
  }
  if (owned) free((void *)data);
}

static void send_status(http_conn_t *c, int code, const char *reason, const char *body) {
  size_t cap = 256 + strlen(body);
  char *buf = malloc(cap);
  if (!buf) {
    conn_close(c);
    return;
  }
  int n = snprintf(buf, cap,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n%s",
                    code, reason, (unsigned)strlen(body), body);
  send_mem(c, buf, (uint32_t)n, true);
}

static void send_unauthorized(http_conn_t *c) {
  static const char body[] = "unauthorized";
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"sd2iec_pico2w\"\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n%s",
                    (unsigned)strlen(body), body);
  send_mem(c, buf, (uint32_t)n, false); /* buf is a stack array -- queue_write
                                            copies it before this returns, and
                                            "owned=false" means send_mem won't
                                            try to free() it afterward. */
}

/*--------------------------------------------------------------------------
   Directory listing / mkdir / rename / delete -- all via fs_bridge.h
   (2026-09-02): this file now runs on core
   1 (see core1.c), and core 1 never touches FatFs directly -- these
   four handlers used to call f_stat()/l_opendir()/f_readdir()/f_mkdir()/
   f_rename()/f_unlink() inline (see this file's git history for the
   pre-bridge originals); now they just populate c->fs_req and move to
   HTTP_ST_FS_PENDING, and pump_fs_request() (called from httpd_poll(),
   every core-1 loop tick) finishes the job once fs_bridge_poll() (core
   0, src/iec.c's BUS_IDLE) has actually done the FatFs work and handed
   the result back. path_is_safe() stays a pre-submit check here since
   it's a plain string check, not FatFs I/O.

   Durability: unlike the pre-bridge originals, these no longer call
   disk_idle_sync() themselves after a successful write -- see
   fs_bridge.c's do_mkdir()/do_rename()/do_delete() comment for why an
   immediate, undebounced commit triggered from here would reintroduce
   exactly the live-C64-transaction stall bug the BUS_IDLE debounce
   exists to prevent, just via the bridge
   instead of a C64 command. The existing debounced flush in iec.c's
   BUS_IDLE loop still picks up the write within its usual short window.
---------------------------------------------------------------------------*/

static void handle_list(http_conn_t *c, const char *dir, uint32_t offset, fs_volume_t vol) {
  if (!path_is_safe(dir)) { send_status(c, 400, "Bad Request", "invalid path"); return; }
  c->fs_req.op = FS_OP_LIST;
  c->fs_req.volume = vol;
  c->fs_req.offset = offset;
  strncpy(c->fs_req.path, dir, sizeof(c->fs_req.path) - 1);
  c->fs_req.path[sizeof(c->fs_req.path) - 1] = 0;
  c->fs_submitted = false;
  c->state = HTTP_ST_FS_PENDING;
}

static void handle_mkdir(http_conn_t *c, const char *dir, fs_volume_t vol) {
  if (!path_is_safe(dir)) { send_status(c, 400, "Bad Request", "invalid path"); return; }
  c->fs_req.op = FS_OP_MKDIR;
  c->fs_req.volume = vol;
  strncpy(c->fs_req.path, dir, sizeof(c->fs_req.path) - 1);
  c->fs_req.path[sizeof(c->fs_req.path) - 1] = 0;
  c->fs_submitted = false;
  c->state = HTTP_ST_FS_PENDING;
}

static void handle_rename(http_conn_t *c, const char *from, const char *to, fs_volume_t vol) {
  if (!path_is_safe(from) || !path_is_safe(to)) { send_status(c, 400, "Bad Request", "invalid path"); return; }
  c->fs_req.op = FS_OP_RENAME;
  c->fs_req.volume = vol;
  strncpy(c->fs_req.path, from, sizeof(c->fs_req.path) - 1);
  c->fs_req.path[sizeof(c->fs_req.path) - 1] = 0;
  strncpy(c->fs_req.path2, to, sizeof(c->fs_req.path2) - 1);
  c->fs_req.path2[sizeof(c->fs_req.path2) - 1] = 0;
  c->fs_submitted = false;
  c->state = HTTP_ST_FS_PENDING;
}

static void handle_delete(http_conn_t *c, const char *path, bool recursive, fs_volume_t vol) {
  if (!path_is_safe(path)) { send_status(c, 400, "Bad Request", "invalid path"); return; }
  c->fs_req.op = recursive ? FS_OP_DELETE_TREE : FS_OP_DELETE;
  c->fs_req.volume = vol;
  strncpy(c->fs_req.path, path, sizeof(c->fs_req.path) - 1);
  c->fs_req.path[sizeof(c->fs_req.path) - 1] = 0;
  c->fs_submitted = false;
  c->state = HTTP_ST_FS_PENDING;
}

/*--------------------------------------------------------------------------
   Wi-Fi configuration (GET/POST /api/wifi)

   GET reports the *running* config from network_cfg (network.h), which
   core 1 owns after boot. Note core1.c flips ap_mode back to true if a
   station-mode join failed, so what this returns is what the device is
   actually doing right now, not necessarily what is in the file -- which
   is the useful thing to show someone wondering why they are still
   talking to the AP.

   There is no POST here. Saving is done by the page itself: it fetches
   /files/PRIFLOP.CFG, patches the keys the form owns, and POSTs the whole
   file back through the ordinary upload path. That removes a second write
   path (and a bridge op) that had to be kept correct, and it is why the
   page must read the file first -- netconfig_load() starts from
   compiled-in defaults, so any key omitted from the file would be
   silently reset, quietly clearing ADMIN or AP_PASSWORD.

   Passwords are never sent back out by this GET. The page can still
   preserve them because it reads the config file directly, which the file
   manager already exposes -- no secret is revealed here that was not
   already downloadable.
---------------------------------------------------------------------------*/

/* Minimal JSON string escaper -- enough for SSIDs, which are arbitrary
   bytes as far as 802.11 is concerned. Control characters are dropped
   rather than \u-escaped; they have no business in an SSID and this
   keeps the function small. */
static void json_escape(const char *in, char *out, size_t outsize) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < outsize; p++) {
    if (*p == '"' || *p == '\\') {
      if (o + 3 >= outsize) break;
      out[o++] = '\\';
      out[o++] = (char)*p;
    } else if (*p >= 0x20) {
      out[o++] = (char)*p;
    }
  }
  out[o] = 0;
}

static void handle_wifi_get(http_conn_t *c) {
  char ssid[NETCONFIG_SSID_MAX * 2], apssid[NETCONFIG_SSID_MAX * 2];
  char mdns[NETCONFIG_MDNS_MAX * 2];
  json_escape(network_cfg.ssid, ssid, sizeof(ssid));
  json_escape(network_cfg.ap_ssid, apssid, sizeof(apssid));
  json_escape(network_cfg.mdns_name, mdns, sizeof(mdns));

  char body[480];
  int blen = snprintf(body, sizeof(body),
                      "{\"mode\":\"%s\",\"ssid\":\"%s\",\"ap_ssid\":\"%s\","
                      "\"mdns\":\"%s\",\"country\":\"%s\","
                      "\"has_password\":%s,"
                      "\"has_ap_password\":%s,\"has_admin\":%s,"
                      "\"default_admin\":%s}",
                      network_cfg.ap_mode ? "AP" : "STA", ssid, apssid, mdns,
                      network_cfg.country,
                      network_cfg.password[0] ? "true" : "false",
                      network_cfg.ap_password[0] ? "true" : "false",
                      network_cfg.admin_password[0] ? "true" : "false",
                      netconfig_admin_is_default(&network_cfg) ? "true" : "false");

  char hdr[128];
  int hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);
  c->state = HTTP_ST_DONE;
  c->close_when_drained = true;
  queue_write(c, hdr, (uint32_t)hlen);
  if (!c->in_use) return;
  queue_write(c, body, (uint32_t)blen);
}

/* POST /api/wifi -- save the network settings.

   Replaces the page doing its own read-modify-write of PRIFLOP.CFG.
   That worked, but it meant the KEY=VALUE layout was implemented twice,
   once in netconfig.c and once in JavaScript, with only the C half under
   test (tests/netconfig_test.c). It also forced the browser to GET the
   whole config -- including both passwords -- just to change one field.

   Patching the in-RAM network_cfg and serialising the whole struct is
   what makes a single write correct: netconfig_load() starts from
   compiled-in defaults, so any key left out of the file would silently
   reset on the next boot. That is the requirement that made a read
   necessary before; holding the live config in RAM removes it.

   Blank password fields mean "keep the current one", so they are simply
   not applied. */
static void handle_wifi_post(http_conn_t *c) {
  char v[NETCONFIG_PASSWORD_MAX];
  static net_config_t before;

  /* Only what this save changes goes to core 0, which applies it over
     the file as it is then (fs_bridge.c, do_write_file): network_cfg is
     what was read at boot, and PFM may have changed the file since over
     the bus (open step 10). */
  before = network_cfg;

  if (get_query_param(c->raw_query, "mode", v, sizeof(v)))
    network_cfg.ap_mode = (v[0] == 'A' || v[0] == 'a');

  if (get_query_param(c->raw_query, "ssid", v, sizeof(v)) && v[0])
    snprintf(network_cfg.ssid, sizeof(network_cfg.ssid), "%s", v);

  if (get_query_param(c->raw_query, "apssid", v, sizeof(v)) && v[0])
    snprintf(network_cfg.ap_ssid, sizeof(network_cfg.ap_ssid), "%s", v);

  if (get_query_param(c->raw_query, "mdns", v, sizeof(v)) && v[0])
    snprintf(network_cfg.mdns_name, sizeof(network_cfg.mdns_name), "%s", v);

  /* Regulatory domain. Unlike the fields above, an EMPTY value is
     meaningful here -- it means "worldwide" -- so this one is applied
     even when blank, and anything that is not exactly two letters clears
     it rather than being stored. Settable from here specifically so that
     changing it does not require hand-editing PRIFLOP.CFG, which holds
     the Wi-Fi password in clear. */
  if (get_query_param(c->raw_query, "country", v, sizeof(v))) {
    if (v[0] && v[1] && !v[2]) {
      network_cfg.country[0] = (char)toupper((unsigned char)v[0]);
      network_cfg.country[1] = (char)toupper((unsigned char)v[1]);
      network_cfg.country[2] = 0;
    } else {
      network_cfg.country[0] = 0;
    }
  }

  /* Blank = unchanged, for all three secrets. */
  if (get_query_param(c->raw_query, "pass", v, sizeof(v)) && v[0])
    snprintf(network_cfg.password, sizeof(network_cfg.password), "%s", v);

  if (get_query_param(c->raw_query, "appass", v, sizeof(v)) && v[0])
    snprintf(network_cfg.ap_password, sizeof(network_cfg.ap_password), "%s", v);

  if (get_query_param(c->raw_query, "admin", v, sizeof(v)) && v[0]) {
    /* Stored salted and hashed; the plaintext is not kept anywhere. */
    netconfig_set_admin(&network_cfg, v);
  } else {
    /* Migrate a config written before hashing existed. A no-op once the
       stored form is already a digest, so this costs one rewrite ever --
       and it happens on a save the user asked for, not on every boot. */
    netconfig_rehash_admin(&network_cfg);
  }

  c->fs_req.data[0] = 0;
  unsigned int len = netconfig_diff(&before, &network_cfg, c->fs_req.data,
                                    (unsigned int)sizeof(c->fs_req.data));
  if (!len && c->fs_req.data[0]) {
    send_status(c, 500, "Internal Server Error", "config too large to serialize");
    return;
  }

  c->fs_req.op = FS_OP_WRITE_FILE;
  c->fs_req.data_len = len;
  snprintf(c->fs_req.path, sizeof(c->fs_req.path), "%s", NETCONFIG_PATH);
  c->fs_submitted = false;
  c->state = HTTP_ST_FS_PENDING;
}

/* Drives a connection sitting in HTTP_ST_FS_PENDING: first submits its
   request (retrying later if the single-slot bridge is currently busy
   with another connection's request), then polls for the response and
   finally formats it into the actual HTTP reply. Called from
   httpd_poll() for every such connection, every core-1 loop tick. */
static void pump_fs_request(http_conn_t *c) {
  if (!c->fs_submitted) {
    c->fs_submitted = fs_bridge_try_submit(&c->fs_req, c);
    return;
  }
  if (!fs_bridge_try_take_response(&c->fs_req, c)) {
    return;
  }

  if (c->fs_req.op == FS_OP_LIST) {
    if (c->fs_req.result != FS_RESULT_OK) {
      if (c->fs_req.result == FS_RESULT_NO_FS) {
        send_status(c, 503, "Service Unavailable", "no filesystem mounted");
      } else {
        send_status(c, 404, "Not Found", "directory not found");
      }
      return;
    }
    char header[128];
    int hlen = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                         (unsigned long)c->fs_req.data_len);
    c->state = HTTP_ST_DONE;
    c->close_when_drained = true;
    queue_write(c, header, (uint32_t)hlen);
    if (!c->in_use) return;
    queue_write(c, c->fs_req.data, c->fs_req.data_len);
    return;
  }

  if (c->fs_req.op == FS_OP_OPEN_READ) {
    if (c->fs_req.result != FS_RESULT_OK) {
      send_status(c, c->fs_req.result == FS_RESULT_NO_FS ? 503 : 404,
                  c->fs_req.result == FS_RESULT_NO_FS ? "Service Unavailable" : "Not Found",
                  c->fs_req.result == FS_RESULT_NO_FS ? "no filesystem mounted" : "file not found");
      return;
    }
    /* c->fs_req.path still holds the originally-requested path --
       fs_bridge_poll()/do_open_read() never touch it, so it survives
       the round trip unchanged. */
    c->fs_handle = c->fs_req.handle;
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                         "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                         mime_type(c->fs_req.path), (unsigned long)c->fs_req.file_size);
    c->state = HTTP_ST_SEND_FILE;
    /* Reset for pump_send_file()'s own READ/CLOSE round trips -- without
       this, it inherits fs_submitted==true left over from *this* OPEN_READ
       round trip, sees no new response ever arrive (nothing new was
       submitted), and waits forever. Found 2026-09-02 on real hardware
       via the identical bug in the upload path -- see FS_OP_OPEN_WRITE
       below for the full writeup. */
    c->fs_submitted = false;
    c->close_when_drained = true;
    queue_write(c, header, (uint32_t)hlen);
    if (!c->in_use) return; /* queue_write() closed us on a (rare) write failure */
    /* Don't kick off pump_send_file() here -- httpd_poll()'s per-
       connection loop will, on this same or the next tick, now that
       c->state is HTTP_ST_SEND_FILE. Keeps "who drives this state"
       down to one place. */
    return;
  }

  if (c->fs_req.op == FS_OP_OPEN_WRITE) {
    if (c->fs_req.result != FS_RESULT_OK) {
      c->is_uploading = false;
      send_status(c, c->fs_req.result == FS_RESULT_NO_FS ? 503 : 500,
                  c->fs_req.result == FS_RESULT_NO_FS ? "Service Unavailable" : "Internal Server Error",
                  c->fs_req.result == FS_RESULT_NO_FS ? "no filesystem mounted" : "could not create file");
      return;
    }
    c->fs_handle = c->fs_req.handle;
    c->state = HTTP_ST_UPLOAD_BODY;
    /* Reset for pump_upload()'s own WRITE/CLOSE round trips -- the real
       bug behind "upload gets stuck after the file opens" (confirmed on
       real hardware, 2026-09-02): without this, c->fs_submitted is still
       true from *this* OPEN_WRITE round trip, so pump_upload()'s first
       call sees fs_submitted==true and calls fs_bridge_try_take_response()
       expecting an answer to a request that was never submitted --
       which never arrives, so it waits forever and never even checks
       upload_buf_len, even though on_recv() already accumulated the
       whole body by this point. */
    c->fs_submitted = false;
    /* No response yet -- that happens once the whole body has arrived
       and been written, see pump_upload(). Any body bytes that arrived
       before this point are already sitting in c->upload_buf, not lost
       -- see that field's comment. */
    return;
  }

  /* MKDIR/RENAME/DELETE: all a plain OK-or-error response. */
  switch (c->fs_req.result) {
    case FS_RESULT_OK:
      send_status(c, 200, "OK", "ok");
      break;
    case FS_RESULT_NO_FS:
      send_status(c, 503, "Service Unavailable", "no filesystem mounted");
      break;
    case FS_RESULT_NOT_EMPTY:
      /* The page turns this into a confirm prompt and retries with
         ?recursive=1. "notempty" rather than prose so it can tell this
         apart from the in-use case below. */
      send_status(c, 409, "Conflict", "notempty");
      break;
    case FS_RESULT_IN_USE:
      send_status(c, 409, "Conflict",
                  "inuse: that image is mounted by the drive; "
                  "unmount it first (CD:_ on the C64)");
      break;
    case FS_RESULT_NOT_FOUND:
      send_status(c, 404, "Not Found", "not found");
      break;
    default:
      send_status(c, 500, "Internal Server Error",
                  c->fs_req.op == FS_OP_MKDIR ? "mkdir failed" : "operation failed");
      break;
  }
}

/* Streams PAGE_HTML out in whatever the send buffer currently has room
   for, resuming from on_sent() as the client acks. Same shape as
   pump_send_file() but with no filesystem behind it -- the bytes are
   already in flash, so there is nothing to wait for except TCP. */
static void pump_send_page(http_conn_t *c) {
  const uint32_t total = (uint32_t)strlen(PAGE_HTML);

  while (c->page_sent < total) {
    uint32_t room = tcp_sndbuf(c->pcb);
    if (room == 0) return;                    /* resume from on_sent() */

    uint32_t chunk = total - c->page_sent;
    if (chunk > room) chunk = room;
    if (chunk > HTTP_SEND_CHUNK) chunk = HTTP_SEND_CHUNK;

    err_t err = tcp_write(c->pcb, PAGE_HTML + c->page_sent, (u16_t)chunk,
                          TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
      if (err == ERR_MEM) return;             /* try again on the next ack */
      conn_close(c);
      return;
    }
    c->page_sent += chunk;
    c->unacked   += chunk;
  }

  tcp_output(c->pcb);
  c->state = HTTP_ST_DONE;
  c->close_when_drained = true;
}

/*--------------------------------------------------------------------------
   File download (GET /files/<path>) -- streamed through fs_bridge.h's
   FS_OP_OPEN_READ/READ/CLOSE (2026-09-02,
   follow-up to the LIST/MKDIR/RENAME/DELETE bridge work): unlike those,
   an open file spans many round trips, so pump_send_file() below is a
   small state machine instead of a single submit-then-format step.
   chunk_ready/fs_submitted (http_conn_t) track which of three phases a
   connection is in; exactly one phase makes progress per call, which is
   fine since httpd_poll() calls this every core-1 loop tick.
---------------------------------------------------------------------------*/

static void pump_send_file(http_conn_t *c) {
  /* Phase 3: a chunk already came back from the bridge -- send it. */
  if (c->chunk_ready) {
    err_t err = tcp_write(c->pcb, c->fs_req.data, (u16_t)c->fs_req.data_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
      return; /* send buffer genuinely full -- retry next tick, chunk still held in c->fs_req.data */
    }
    c->unacked += c->fs_req.data_len;
    tcp_output(c->pcb);
    c->chunk_ready = false;
    return;
  }

  /* Phase 2: a READ or CLOSE request is in flight -- poll for it. */
  if (c->fs_submitted) {
    if (!fs_bridge_try_take_response(&c->fs_req, c)) return;
    c->fs_submitted = false;

    if (c->fs_req.op == FS_OP_CLOSE) {
      c->fs_handle = -1;
      c->state = HTTP_ST_DONE;
      try_finish_close(c);
      return;
    }

    /* A completed FS_OP_READ. */
    if (c->fs_req.result != FS_RESULT_OK) {
      conn_close(c);
      return;
    }
    if (c->fs_req.data_len == 0) {
      /* EOF: close the handle via the bridge before finishing -- core 1
         still isn't allowed to just forget about it. */
      c->fs_req.op = FS_OP_CLOSE;
      c->fs_req.handle = c->fs_handle;
      c->fs_submitted = fs_bridge_try_submit(&c->fs_req, c);
      return;
    }
    c->chunk_ready = true;
    return;
  }

  /* Phase 1: idle -- issue a new READ if there's room to send its result. */
  u16_t avail = tcp_sndbuf(c->pcb);
  if (avail < 64) return;
  uint32_t want = avail < HTTP_SEND_CHUNK ? avail : HTTP_SEND_CHUNK;
  c->fs_req.op = FS_OP_READ;
  c->fs_req.handle = c->fs_handle;
  c->fs_req.data_len = want; /* input: bytes wanted */
  c->fs_submitted = fs_bridge_try_submit(&c->fs_req, c);
}

static void handle_download(http_conn_t *c, const char *path, fs_volume_t vol) {
  if (!path_is_safe(path)) { send_status(c, 400, "Bad Request", "invalid path"); return; }
  c->fs_req.op = FS_OP_OPEN_READ;
  c->fs_req.volume = vol;
  strncpy(c->fs_req.path, path, sizeof(c->fs_req.path) - 1);
  c->fs_req.path[sizeof(c->fs_req.path) - 1] = 0;
  c->fs_submitted = false;
  c->state = HTTP_ST_FS_PENDING;
}

/*--------------------------------------------------------------------------
   File upload (POST /files/<path>) -- streamed through fs_bridge.h's
   FS_OP_OPEN_WRITE/WRITE/CLOSE. Incoming body bytes accumulate in
   c->upload_buf (on_recv()), *not* c->fs_req.data -- see upload_buf's
   struct comment for why those can't be the same storage: on_recv() has
   to accept body bytes starting the instant start_upload() runs, before
   the async FS_OP_OPEN_WRITE round trip (which owns c->fs_req until it
   completes) is even done.
---------------------------------------------------------------------------*/

static void start_upload(http_conn_t *c, const char *path, fs_volume_t vol) {
  if (!path_is_safe(path)) { send_status(c, 400, "Bad Request", "invalid path"); return; }
  c->fs_req.op = FS_OP_OPEN_WRITE;
  c->fs_req.volume = vol;
  strncpy(c->fs_req.path, path, sizeof(c->fs_req.path) - 1);
  c->fs_req.path[sizeof(c->fs_req.path) - 1] = 0;
  c->fs_submitted = false;
  c->is_uploading = true; /* accept body bytes starting now -- see upload_buf's comment */
  c->upload_buf_len = 0;
  c->state = HTTP_ST_FS_PENDING;
}

/* Drives a connection sitting in HTTP_ST_UPLOAD_BODY: submits/polls
   WRITE requests for whatever on_recv() has accumulated into
   c->upload_buf, and once the full body has arrived and been written,
   closes the handle and finally responds. Called from httpd_poll()
   every core-1 loop tick, same as pump_send_file(). */
static void pump_upload(http_conn_t *c) {
  if (c->fs_submitted) {
    if (!fs_bridge_try_take_response(&c->fs_req, c)) return;
    c->fs_submitted = false;

    if (c->fs_req.op == FS_OP_CLOSE) {
      c->fs_handle = -1;
      c->is_uploading = false;
      send_status(c, 200, "OK", "ok");
      return;
    }

    /* A completed FS_OP_WRITE. */
    if (c->fs_req.result != FS_RESULT_OK) {
      c->is_uploading = false;
      conn_close(c);
      return;
    }
    /* fall through: maybe there's more to write, or we're done */
  }

  if (c->upload_buf_len > 0) {
    /* One bridge write carries at most FS_BRIDGE_BUF_MAX. upload_buf is
       deliberately larger than that (it has to absorb a whole receive
       window, see HTTP_UPLOAD_BUF_SIZE), so cap the chunk and keep the
       remainder for the next round trip. Copying upload_buf_len blindly
       overflowed fs_req.data the moment the buffer grew past
       FS_BRIDGE_BUF_MAX -- it was only ever safe because the two sizes
       happened to be equal. */
    uint32_t n = c->upload_buf_len;
    if (n > FS_BRIDGE_BUF_MAX) n = FS_BRIDGE_BUF_MAX;

    c->fs_req.op = FS_OP_WRITE;
    c->fs_req.handle = c->fs_handle;
    memcpy(c->fs_req.data, c->upload_buf, n);
    c->fs_req.data_len = n;
    if (fs_bridge_try_submit(&c->fs_req, c)) {
      c->fs_submitted = true;
      c->upload_buf_len -= n;
      if (c->upload_buf_len > 0) {
        memmove(c->upload_buf, c->upload_buf + n, c->upload_buf_len);
      }
    }
    return;
  }

  if (c->body_received >= c->content_length) {
    c->fs_req.op = FS_OP_CLOSE;
    c->fs_req.handle = c->fs_handle;
    c->fs_submitted = fs_bridge_try_submit(&c->fs_req, c);
  }
}

/*--------------------------------------------------------------------------
   Reboot / BOOTSEL (2026-09-02, web-UI convenience for the STA-mode
   config workflow): both defer the actual action past
   send_status()'s tcp_write()/tcp_output() call, so the browser gets the
   "OK" response instead of just a dropped connection, then perform a
   proper shutdown instead of a blind reset:

     - a flush (FS_OP_SYNC via fs_bridge.h, see below) forces any writes
       still sitting in ata.c's deferred write-back cache out to
       physical flash. This is on top of (not instead of) src/iec.c's
       own periodic idle-time flush -- that one exists to avoid stalling
       a *live C64 transaction*, a concern that doesn't apply here
       (nothing is mid-transaction; we're about to reboot the whole
       device), so there's no reason to leave it to chance/timing when
       we can just force it deterministically before doing something
       irreversible. Goes through fs_bridge.h rather than calling
       disk_idle_sync() directly (like this code did before the core-1
       migration) because this file now runs on core 1, and core 1 never
       touches FatFs/ata.c directly -- see fs_bridge.h.

     - cyw43_arch_deinit(), BOOTSEL only, properly disassociates/stops
       the radio first. reset_usb_boot() jumps into the boot ROM without
       going through main()'s normal cyw43_arch_init() re-power-up
       sequence again, and the cyw43439 is a separate chip on its own
       supply -- left alone, it just keeps running whatever mode it was
       last programmed with (AP still beaconing) until something
       explicitly tells it to stop. A plain reboot doesn't need this:
       main() calls cyw43_arch_init() again from scratch on the way back
       up, which reinitializes the radio anyway.
---------------------------------------------------------------------------*/

#define HTTPD_ACTION_DELAY_MS 300u

typedef enum { HTTPD_ACTION_NONE, HTTPD_ACTION_REBOOT, HTTPD_ACTION_BOOTSEL } httpd_action_t;

static httpd_action_t g_pending_action = HTTPD_ACTION_NONE;
static uint32_t g_action_at_us = 0;
static bool g_flush_submitted = false;
static fs_request_t g_flush_req;

static void handle_reboot(http_conn_t *c) {
  send_status(c, 200, "OK", "rebooting");
  g_pending_action = HTTPD_ACTION_REBOOT;
  g_action_at_us = time_us_32() + HTTPD_ACTION_DELAY_MS * 1000u;
}

static void handle_bootsel(http_conn_t *c) {
  send_status(c, 200, "OK", "entering BOOTSEL mode");
  g_pending_action = HTTPD_ACTION_BOOTSEL;
  g_action_at_us = time_us_32() + HTTPD_ACTION_DELAY_MS * 1000u;
}

void httpd_poll(void) {
  /* Reclaim file handles orphaned by connections that died while the
     bridge was busy -- see pending_close above. */
  drain_pending_closes();

  for (int i = 0; i < HTTP_MAX_CONNS; i++) {
    http_conn_t *c = &conns[i];
    if (!c->in_use) continue;
    switch (c->state) {
      case HTTP_ST_FS_PENDING:  pump_fs_request(c); break;
      case HTTP_ST_SEND_FILE:   pump_send_file(c);  break;
      case HTTP_ST_SEND_PAGE:   pump_send_page(c);  break;
      case HTTP_ST_UPLOAD_BODY: pump_upload(c);      break;
      default: break;
    }
  }

  if (g_pending_action == HTTPD_ACTION_NONE ||
      (int32_t)(time_us_32() - g_action_at_us) < 0) {
    return;
  }

  if (!g_flush_submitted) {
    g_flush_req.op = FS_OP_SYNC;
    g_flush_submitted = fs_bridge_try_submit(&g_flush_req, &g_flush_req);
    return;
  }
  if (!fs_bridge_try_take_response(&g_flush_req, &g_flush_req)) {
    return; /* flush not confirmed done yet */
  }

  if (g_pending_action == HTTPD_ACTION_BOOTSEL) {
    cyw43_arch_deinit();
    reset_usb_boot(0, 0); /* noreturn */
  } else {
    watchdog_reboot(0, 0, 0); /* already waited out the response-send delay above */
  }
}

/*--------------------------------------------------------------------------
   Request dispatch (called once headers are fully parsed)
---------------------------------------------------------------------------*/

static void dispatch(http_conn_t *c) {
  char decoded[HTTP_PATH_MAX];

  if (!check_auth(c)) {
    send_unauthorized(c);
    return;
  }

  if (!strcmp(c->raw_path, "/") || !strcmp(c->raw_path, "/index.html")) {
    if (strcmp(c->method, "GET")) { send_status(c, 405, "Method Not Allowed", "GET only"); return; }
    char header[128];
    int hlen = snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                         "Content-Length: %u\r\nConnection: close\r\n\r\n",
                         (unsigned)strlen(PAGE_HTML));
    c->page_sent = 0;
    c->state = HTTP_ST_SEND_PAGE;
    c->close_when_drained = true;
    queue_write(c, header, (uint32_t)hlen);
    if (!c->in_use) return;
    pump_send_page(c);
    return;
  }

  if (!strcmp(c->raw_path, "/api/list")) {
    char dir[HTTP_PATH_MAX] = "/";
    if (!get_query_param(c->raw_query, "dir", dir, sizeof(dir)) || dir[0] == 0) {
      strcpy(dir, "/");
    }
    char off[16];
    uint32_t offset = 0;
    if (get_query_param(c->raw_query, "offset", off, sizeof(off)))
      offset = (uint32_t)strtoul(off, NULL, 10);
    handle_list(c, dir, offset, parse_vol(c->raw_query));
    return;
  }

  if (!strcmp(c->raw_path, "/api/wifi")) {
    if (!strcmp(c->method, "GET")) {
      handle_wifi_get(c);
      return;
    }
    if (!strcmp(c->method, "POST")) {
      handle_wifi_post(c);
      return;
    }
    send_status(c, 405, "Method Not Allowed", "GET or POST");
    return;
  }

  if (!strcmp(c->raw_path, "/api/stats")) {
    /* static, not a local: this runs on core 1, whose stack was 2048 bytes
       until 2026-09-20 and is 4096 now (scripts/pico/CMakeLists.txt), and
       which also has to survive the mDNS responder's 1100-byte frames. One request is handled at a
       time on this core, so nothing else can be using it. And 1200 was
       already below the worst case (every counter ten digits wide came to
       1267 bytes) even before the health fields below, with the length
       used unclamped for Content-Length and the copy. 2400 was outgrown the
       same way by the Primo turbo's counters: 2509 at the widest by
       2026-10-06, found by tests/webui_test.py; 3072 leaves room. */
    static char body[3072];
    /* The command bytes before the most recent ATN bail, oldest first:
       "20 6f 3f 29 62 ..." (see iec_bail_cmds in iec.c). */
    char bail_cmds[8 * 3], last_cmds[8 * 3];
    uint8_t head = iec_cmd_head;           /* one read: core 0 may move it */
    for (int k = 0; k < 8; k++) {
      snprintf(bail_cmds + k * 3, 4, k < 7 ? "%02x " : "%02x", iec_bail_cmds[k]);
      snprintf(last_cmds + k * 3, 4, k < 7 ? "%02x " : "%02x",
               iec_cmd_ring[(uint8_t)(head + k) & 7]);
    }

    int blen = snprintf(body, sizeof(body),
                        "{\"stack_used\":%lu,\"stack_total\":%lu,"
                        "\"faulted\":%lu,\"fault_pc\":\"0x%08lx\","
                        "\"fault_lr\":\"0x%08lx\",\"fault_sp\":\"0x%08lx\","
                        "\"flash_op_max_us\":%lu,\"flash_ops\":%lu,"
                        "\"atn_irqs\":%lu,\"boots\":%lu,\"wdog\":\"0x%08lx\","
                        "\"reformatted\":%u,\"atn_bails\":%lu,"
                        "\"bail_cmds\":\"%s\",\"bail_dev\":%u,"
                        "\"last_cmds\":\"%s\",\"bus_state\":%u,\"dev_state\":%u,"
                        "\"rx\":%lu,\"tx\":%lu,\"getc_max\":%lu,\"putc_max\":%lu,"
                        "\"gap_max\":%lu,\"busy_ms\":%lu,\"atn_aborts\":%lu,"
                        "\"pio\":%u,\"pio_ok\":%lu,\"pio_bad\":%lu,\"pio_owns\":%u,"
                        "\"pio_tx\":%u,\"pio_txb\":%lu,\"pio_txa\":%lu,\"pio_txn\":%lu,"
                        "\"usb_conn\":%u,\"jf_rx\":%lu,\"jf_tx\":%lu,\"jf_us\":%lu,"
                        "\"lfsread_max_us\":%lu,\"lfsread_ops\":%lu,"
                        "\"lfsread_pre_max_us\":%lu,\"lfsread_post_max_us\":%lu,"
                        "\"put_gap_max_us\":%lu,"
                        "\"put_gap_max_us_prev_boot\":%lu,"
                        "\"refill_max_us\":%lu,\"refill_max_us_prev_boot\":%lu,"
                        "\"findbuf_max_us\":%lu,\"tx_position_prev_boot\":%lu,"
                        "\"refill_phase_prev_boot\":%lu,"
                        "\"putc_loop_prev_boot\":%lu,"
                        "\"tx_timeout_enabled\":%u,\"tx_timeout_us\":%lu,"
                        "\"tx_timeouts\":%lu,\"tx_long_waits\":%lu,"
                        "\"lfsread_pre_last_us_prev_boot\":%lu,"
                        "\"lfsread_post_last_us_prev_boot\":%lu,"
                        "\"c1_loops\":%lu,\"c1_gap_max_ms\":%lu,\"c1_gate_timeouts\":%lu,"
                        "\"c1_stack_used\":%lu,\"c1_stack_size\":%lu,"
                        "\"wifi_link\":%ld,\"wifi_link_drops\":%lu,"
                        "\"wifi_rx\":%lu,\"wifi_rx_age_ms\":%lu,"
                        "\"fsb_wait_max_ms\":%lu,"
                        "\"device_address\":%u,\"ext_hiding\":%u,"
                        "\"pt_on\":%u,\"pt_sessions\":%lu,\"pt_completed\":%lu,"
                        "\"pt_aborted\":%lu,\"pt_exchanges\":%lu,\"pt_resent\":%lu,"
                        "\"pt_garbled\":%lu,\"pt_refused\":%lu,\"pt_bad_writes\":%lu,"
                        "\"pt_dup_writes\":%lu,\"pt_reissued\":%lu,\"pt_resyncs\":%lu,"
                        "\"pt_bytes_to\":%lu,\"pt_bytes_from\":%lu,\"pt_us\":%lu,\"pt_call_waits\":%lu,"
                        "\"pt_last_reason\":%d,\"pt_last_edges\":%lu,"
                        "\"pt_st_runs\":%lu,\"pt_st_err\":%u,\"pt_st_ready\":%u,"
                        "\"pt_st_levels\":\"0x%03x\",\"pt_st_samples\":%u,\"pt_st_imm\":%u,"
                        "\"pt_prev_valid\":%lu,\"pt_prev_phase\":%lu,\"pt_prev_frame\":%lu,"
                        "\"pt_prev_detail\":\"0x%lx\",\"pt_prev_sp\":\"0x%08lx\","
                        "\"pt_prev_sp_min\":\"0x%08lx\",\"pt_prev_deep\":%lu,"
                        "\"pt_prev_deep_n\":%lu,\"pt_prev_deep_us\":%lu}",
                        (unsigned long)stack_high_water(),
                        (unsigned long)stack_total(),
                        (unsigned long)fault_last(0), (unsigned long)fault_last(1),
                        (unsigned long)fault_last(2), (unsigned long)fault_last(3),
                        (unsigned long)flash_op_max_us, (unsigned long)flash_ops,
                        (unsigned long)iec_atn_irqs, (unsigned long)boot_count(),
                        (unsigned long)watchdog_ctrl_raw(),
                        (unsigned)fs_reformatted(),
                        (unsigned long)iec_atnfinish_bails,
                        bail_cmds, (unsigned)iec_bail_dev,
                        last_cmds, (unsigned)iec_data.bus_state,
                        (unsigned)iec_data.device_state,
                        (unsigned long)iec_stat_rx_bytes,
                        (unsigned long)iec_stat_tx_bytes,
                        (unsigned long)iec_stat_getc_max_us,
                        (unsigned long)iec_stat_putc_max_us,
                        (unsigned long)iec_stat_gap_max_us,
                        (unsigned long)iec_stat_busy_ms,
                        (unsigned long)iec_stat_atn_aborts,
                        (unsigned)iec_pio_available(),
                        (unsigned long)iec_pio_match,
                        (unsigned long)iec_pio_mismatch,
                        (unsigned)iec_pio_rx_owns,
                        (unsigned)iec_pio_tx_enabled(),
                        (unsigned long)iec_pio_tx_bytes,
                        (unsigned long)iec_pio_tx_abort,
                        (unsigned long)iec_pio_tx_atn,
                        (unsigned)stdio_usb_connected(),
                        (unsigned long)iec_stat_jf_rx,
                        (unsigned long)iec_stat_jf_tx,
                        (unsigned long)iec_stat_jf_us,
                        (unsigned long)lfsread_max_us,
                        (unsigned long)lfsread_ops,
                        (unsigned long)lfsread_pre_max_us,
                        (unsigned long)lfsread_post_max_us,
                        (unsigned long)iec_stat_put_gap_max_us,
                        (unsigned long)iec_put_gap_max_us_prev_boot(),
                        (unsigned long)iec_stat_refill_max_us,
                        (unsigned long)iec_refill_max_us_prev_boot(),
                        (unsigned long)iec_stat_findbuf_max_us,
                        (unsigned long)iec_tx_position_prev_boot(),
                        (unsigned long)iec_refill_phase_prev_boot(),
                        (unsigned long)iec_putc_loop_prev_boot(),
                        (unsigned)iec_tx_timeout_enabled,
                        (unsigned long)iec_tx_timeout_us,
                        (unsigned long)iec_tx_timeouts,
                        (unsigned long)iec_tx_long_waits,
                        (unsigned long)lfsread_pre_last_us_prev_boot(),
                        (unsigned long)lfsread_post_last_us_prev_boot(),
                        (unsigned long)health.core1_loops,
                        (unsigned long)health.core1_gap_max_ms,
                        (unsigned long)health.core1_gate_timeouts,
                        (unsigned long)health.core1_stack_used,
                        (unsigned long)PICO_CORE1_STACK_SIZE,
                        (long)health.wifi_link_status,
                        (unsigned long)health.wifi_link_drops,
                        (unsigned long)health.wifi_rx_frames,
                        /* age of the newest frame; with no frame yet, the uptime */
                        (unsigned long)(to_ms_since_boot(get_absolute_time()) - health.wifi_rx_last_ms),
                        (unsigned long)health.fsb_wait_max_ms,
                        (unsigned)device_address,
                        (unsigned)!!(globalflags & EXTENSION_HIDING),
                        (unsigned)primo_turbo_enabled,
                        (unsigned long)primo_turbo_stats.sessions,
                        (unsigned long)primo_turbo_stats.completed,
                        (unsigned long)primo_turbo_stats.aborted,
                        (unsigned long)primo_turbo_stats.exchanges,
                        (unsigned long)primo_turbo_stats.resent,
                        (unsigned long)primo_turbo_stats.garbled,
                        (unsigned long)primo_turbo_stats.refused,
                        (unsigned long)primo_turbo_stats.bad_writes,
                        (unsigned long)primo_turbo_stats.dup_writes,
                        (unsigned long)primo_turbo_stats.reissued,
                        (unsigned long)primo_turbo_stats.resyncs,
                        (unsigned long)primo_turbo_stats.bytes_to,
                        (unsigned long)primo_turbo_stats.bytes_from,
                        (unsigned long)primo_turbo_stats.us,
                        (unsigned long)primo_turbo_stats.call_waits,
                        (int)primo_turbo_last_reason,
                        (unsigned long)primo_turbo_last_edges,
                        (unsigned long)primo_turbo_selftest.runs,
                        (unsigned)primo_turbo_selftest.err,
                        (unsigned)primo_turbo_selftest.ready,
                        (unsigned)primo_turbo_selftest.levels,
                        (unsigned)primo_turbo_selftest.samples,
                        (unsigned)primo_turbo_selftest.imm,
                        (unsigned long)primo_turbo_prev[3],
                        (unsigned long)primo_turbo_prev[0],
                        (unsigned long)primo_turbo_prev[1],
                        (unsigned long)primo_turbo_prev[2],
                        (unsigned long)primo_turbo_prev[4],
                        (unsigned long)primo_turbo_prev[5],
                        (unsigned long)primo_turbo_prev[6],
                        (unsigned long)primo_turbo_prev[7],
                        (unsigned long)primo_turbo_prev[8]);
    if (blen < 0)
      blen = 0;
    if (blen >= (int)sizeof(body))         /* truncated: send what fits, still valid to count */
      blen = (int)sizeof(body) - 1;
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);
    c->state = HTTP_ST_DONE;
    c->close_when_drained = true;
    queue_write(c, hdr, (uint32_t)hlen);
    if (!c->in_use) return;
    queue_write(c, body, (uint32_t)blen);
    return;
  }

  /* Switch the authoritative IEC receive decoder between the software
     bit loop and PIO, without a reflash. Exists because this sits between
     a C64 and its disks: if PIO ever misbehaves in a way the shadow
     comparison did not catch, the way back should not require a UF2 and a
     BOOTSEL. */
  if (!strcmp(c->raw_path, "/api/pio") && !strcmp(c->method, "POST")) {
    char v[8];
    bool did = false;

    /* Both parameters handled here rather than in two blocks: the first
       matching path wins in this dispatcher, so a second `/api/pio` block
       further down is unreachable. Found that the direct way. */
    if (get_query_param(c->raw_query, "owns", v, sizeof(v))) {
      if (!iec_pio_available() && v[0] == '1') {
        send_status(c, 409, "Conflict", "no PIO state machine was claimed");
        return;
      }
      iec_pio_rx_owns = (v[0] == '1');
      did = true;
    }
    if (get_query_param(c->raw_query, "tx", v, sizeof(v))) {
      iec_pio_tx_owns = (v[0] == '1');
      did = true;
    }
    if (get_query_param(c->raw_query, "primo", v, sizeof(v))) {
      primo_turbo_enabled = (v[0] == '1');
      did = true;
    }
    if (get_query_param(c->raw_query, "primotest", v, sizeof(v))) {
      primo_turbo_selftest_req = (uint8_t)(v[0] - '0');  /* 1 PIO, 2/3 save; at BUS_IDLE */
      did = true;
    }
    if (!did) {
      send_status(c, 400, "Bad Request", "missing owns=0|1, tx=0|1 and/or primo=0|1");
      return;
    }
    send_status(c, 200, "OK",
                iec_pio_tx_enabled() ? "rx+tx pio" :
                (iec_pio_rx_owns ? "rx pio, tx software" : "software"));
    return;
  }

  /* Phase D2: runtime toggle for the opt-in
     timeout on the two byte-send waits that were directly caught
     hanging (iec_pio_tx_byte()'s DONE-wait, _iec_putc()'s tx_done wait).
     Same reasoning as /api/pio above -- a way back that is a POST, not a
     reflash. Default off, so this cannot silently change C64 behaviour;
     turning it on only changes what happens when the listener was never
     going to respond anyway. */
  if (!strcmp(c->raw_path, "/api/txtimeout") && !strcmp(c->method, "POST")) {
    char v[16];
    bool did = false;

    if (get_query_param(c->raw_query, "enabled", v, sizeof(v))) {
      iec_tx_timeout_enabled = (v[0] == '1');
      did = true;
    }
    if (get_query_param(c->raw_query, "us", v, sizeof(v))) {
      unsigned long us = strtoul(v, NULL, 10);
      if (us == 0) {
        send_status(c, 400, "Bad Request", "us must be > 0");
        return;
      }
      iec_tx_timeout_us = (uint32_t)us;
      did = true;
    }
    if (!did) {
      send_status(c, 400, "Bad Request", "missing enabled=0|1 and/or us=<microseconds>");
      return;
    }
    char body[64];
    snprintf(body, sizeof(body), "%s, %lu us",
             iec_tx_timeout_enabled ? "enabled" : "disabled",
             (unsigned long)iec_tx_timeout_us);
    send_status(c, 200, "OK", body);
    return;
  }

  /* The console history, for when the network is up. The USB 'd' key is
     the channel that matters when it is not -- see bootlog.h. */
  if (!strcmp(c->raw_path, "/api/bootlog")) {
    static char log[BOOTLOG_SIZE];
    uint32_t n = bootlog_read(log, sizeof(log));
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                        (unsigned long)n);
    c->state = HTTP_ST_DONE;
    c->close_when_drained = true;
    queue_write(c, hdr, (uint32_t)hlen);
    if (!c->in_use) return;
    if (n) queue_write(c, log, n);
    return;
  }

  if (!strcmp(c->raw_path, "/api/reboot") && !strcmp(c->method, "POST")) {
    handle_reboot(c);
    return;
  }

  if (!strcmp(c->raw_path, "/api/bootsel") && !strcmp(c->method, "POST")) {
    handle_bootsel(c);
    return;
  }

  if (!strcmp(c->raw_path, "/api/deviceaddr") && !strcmp(c->method, "POST")) {
    char a[8];
    if (!get_query_param(c->raw_query, "addr", a, sizeof(a))) {
      send_status(c, 400, "Bad Request", "missing addr");
      return;
    }
    long addr = strtol(a, NULL, 10);
    /* 4-30: same range display.c's own address-selection menu offers
       (see menu_chaddr()) -- CBM DOS device numbers below 4 collide
       with printers/plotters, and doscmd.c masks the wire format to
       5 bits (0-31), so 31 has nowhere valid to round-trip through. */
    if (addr < 4 || addr > 30) {
      send_status(c, 400, "Bad Request", "address must be 4-30");
      return;
    }
    pending_device_address = (int16_t)addr;
    send_status(c, 200, "OK", "address change queued");
    return;
  }

  if (!strcmp(c->raw_path, "/api/settings") && !strcmp(c->method, "POST")) {
    /* Extension hiding and the Primo turbo, each optional. Queued for core 0
       and saved there, like /api/deviceaddr -- see pending_ext_hiding and
       pending_primo_turbo in iec.c. Both checked before either is queued. */
    char e[4], t[4];
    bool has_e = get_query_param(c->raw_query, "exthide", e, sizeof(e));
    bool has_t = get_query_param(c->raw_query, "primoturbo", t, sizeof(t));
    if ((!has_e && !has_t) ||
        (has_e && strcmp(e, "0") && strcmp(e, "1")) ||
        (has_t && strcmp(t, "0") && strcmp(t, "1"))) {
      send_status(c, 400, "Bad Request", "exthide and/or primoturbo, each 0 or 1");
      return;
    }
    if (has_t && t[0] == '1' && !primo_turbo_ready()) {
      send_status(c, 409, "Conflict", "the turbo has no PIO state machine");
      return;
    }
    if (has_e)
      pending_ext_hiding = (int8_t)(e[0] - '0');
    if (has_t)
      pending_primo_turbo = (int8_t)(t[0] - '0');
    send_status(c, 200, "OK", "setting change queued");
    return;
  }

  if (!strcmp(c->raw_path, "/api/mkdir") && !strcmp(c->method, "POST")) {
    char dir[HTTP_PATH_MAX];
    if (!get_query_param(c->raw_query, "dir", dir, sizeof(dir))) {
      send_status(c, 400, "Bad Request", "missing dir");
      return;
    }
    handle_mkdir(c, dir, parse_vol(c->raw_query));
    return;
  }

  if (!strcmp(c->raw_path, "/api/rename") && !strcmp(c->method, "POST")) {
    char from[HTTP_PATH_MAX], to[HTTP_PATH_MAX];
    if (!get_query_param(c->raw_query, "from", from, sizeof(from)) ||
        !get_query_param(c->raw_query, "to", to, sizeof(to))) {
      send_status(c, 400, "Bad Request", "missing from/to");
      return;
    }
    handle_rename(c, from, to, parse_vol(c->raw_query));
    return;
  }

  if (!strncmp(c->raw_path, "/files/", 7)) {
    url_decode(decoded, c->raw_path + 6, sizeof(decoded)); /* +6 keeps the leading '/' */
    /* The config file is the one thing on this volume that must not come
       back out over HTTP: it holds the Wi-Fi passphrase in clear, and it
       has to, because cyw43_wifi_join() wants the passphrase itself and
       so nothing reversible can be avoided. Serving it meant anyone who
       got past the admin password -- including anyone who simply tried
       the default one -- could read the network's password.

       Refusing costs nothing any more. The page used to fetch this file
       to patch it; since /api/wifi took that over it has no reader left,
       and the settings it holds are all editable through that endpoint.

       Not a claim of secrecy: the C64 can still LOAD it off the same
       volume, and a BOOTSEL flash dump reads it regardless. This closes
       the remote path, which is the one that scales. */
    if (!strcmp(c->method, "GET") && path_is_protected(decoded)) {
      send_status(c, 403, "Forbidden",
                  "the configuration file is not downloadable; "
                  "use the settings form instead");
    } else if (!strcmp(c->method, "GET")) {
      handle_download(c, decoded, parse_vol(c->raw_query));
    } else if (!strcmp(c->method, "POST") || !strcmp(c->method, "PUT")) {
      start_upload(c, decoded, parse_vol(c->raw_query));
    } else if (!strcmp(c->method, "DELETE")) {
      char r[8];
      bool rec = get_query_param(c->raw_query, "recursive", r, sizeof(r))
                 && r[0] == '1';
      handle_delete(c, decoded, rec, parse_vol(c->raw_query));
    } else {
      send_status(c, 405, "Method Not Allowed", "unsupported method");
    }
    return;
  }

  send_status(c, 404, "Not Found", "no such endpoint");
}

/*--------------------------------------------------------------------------
   Request-line/header parsing
---------------------------------------------------------------------------*/

/* Parses the accumulated header_buf (guaranteed to contain a full
   request line + headers, terminated by "\r\n\r\n") and fills in
   method/raw_path/raw_query/content_length. On malformed input, sends
   an error response itself (leaving c->state != HTTP_ST_HEADERS so the
   caller knows not to dispatch). */
static void parse_headers(http_conn_t *c) {
  char *sp1 = strchr(c->header_buf, ' ');
  if (!sp1) { send_status(c, 400, "Bad Request", "malformed request line"); return; }
  size_t mlen = (size_t)(sp1 - c->header_buf);
  if (mlen >= sizeof(c->method)) mlen = sizeof(c->method) - 1;
  memcpy(c->method, c->header_buf, mlen);
  c->method[mlen] = 0;

  char *path_start = sp1 + 1;
  char *sp2 = strchr(path_start, ' ');
  if (!sp2) { send_status(c, 400, "Bad Request", "malformed request line"); return; }

  char *qmark = memchr(path_start, '?', (size_t)(sp2 - path_start));
  size_t plen = (size_t)((qmark ? qmark : sp2) - path_start);
  if (plen >= sizeof(c->raw_path)) plen = sizeof(c->raw_path) - 1;
  memcpy(c->raw_path, path_start, plen);
  c->raw_path[plen] = 0;

  c->raw_query[0] = 0;
  if (qmark) {
    size_t qlen = (size_t)(sp2 - qmark - 1);
    if (qlen >= sizeof(c->raw_query)) qlen = sizeof(c->raw_query) - 1;
    memcpy(c->raw_query, qmark + 1, qlen);
    c->raw_query[qlen] = 0;
  }

  c->content_length = 0;
  c->authz[0] = 0;
  const char *p = c->header_buf;
  while ((p = strchr(p, '\n')) != NULL) {
    p++;
    if (!strncasecmp(p, "content-length:", 15)) {
      c->content_length = (uint32_t)strtoul(p + 15, NULL, 10);
    } else if (!strncasecmp(p, "authorization:", 14)) {
      const char *v = p + 14;
      while (*v == ' ') v++;
      const char *end = strchr(v, '\r');
      if (!end) end = strchr(v, '\n');
      size_t vlen = end ? (size_t)(end - v) : strlen(v);
      if (vlen >= sizeof(c->authz)) vlen = sizeof(c->authz) - 1;
      memcpy(c->authz, v, vlen);
      c->authz[vlen] = 0;
    }
  }
}

/*--------------------------------------------------------------------------
   lwIP raw-API callbacks
---------------------------------------------------------------------------*/

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
  { http_conn_t *ac = (http_conn_t *)arg; if (ac) ac->idle_polls = 0; }
  http_conn_t *c = (http_conn_t *)arg;
  if (!c || !c->in_use) return ERR_OK;
  if (c->unacked >= len) c->unacked -= len; else c->unacked = 0;

  if (c->state == HTTP_ST_SEND_FILE) pump_send_file(c);
  if (c->state == HTTP_ST_SEND_PAGE) pump_send_page(c);
  if (c->in_use) try_finish_close(c);
  return ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
  http_conn_t *c = (http_conn_t *)arg;
  if (!c || !c->in_use) return ERR_OK;
  if (c->state == HTTP_ST_SEND_FILE) pump_send_file(c);
  if (c->state == HTTP_ST_SEND_PAGE) pump_send_page(c);
  if (c->in_use) try_finish_close(c);

  /* Reap sockets that have stopped making progress, so one abandoned
     upload cannot hold a connection slot indefinitely. Deliberately only
     counts *poll* ticks with no intervening recv/ack: an active transfer,
     however slow, keeps resetting it, and a legitimate wait on the
     bridge resolves in milliseconds. */
  if (c->in_use && ++c->idle_polls >= HTTP_IDLE_POLL_LIMIT) {
    conn_close(c);
  }
  return ERR_OK;
}

static void on_err(void *arg, err_t err) {
  http_conn_t *c = (http_conn_t *)arg;
  if (!c) return;
  /* lwIP has already freed the pcb by the time this fires. */
  c->pcb = NULL;
  conn_cleanup(c);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
  http_conn_t *c = (http_conn_t *)arg;
  if (!c || !c->in_use) {
    if (p) pbuf_free(p);
    return ERR_OK;
  }

  if (!p) {
    conn_close(c);
    return ERR_OK;
  }
  c->idle_polls = 0;   /* real traffic -- see on_poll()'s reaper */

  /* FLOW CONTROL. pump_upload() drains upload_buf to flash through the
     bridge, far slower than TCP delivers. If this segment's body would not
     fit, refuse the WHOLE pbuf: lwIP keeps it (pcb->refused_data) and
     redelivers it from tcp_fasttmr(), and because we never tcp_recved() it
     the receive window stays shut, so the sender throttles itself to the
     speed flash can absorb.

     All-or-nothing is the point: an earlier attempt clamped the copy to
     the space available but still freed the pbuf, silently dropping the
     remainder, so body_received never reached content_length and the
     upload hung forever holding a file handle. */
  if (c->is_uploading && c->headers_seen &&
      p->tot_len > (uint16_t)(sizeof(c->upload_buf) - c->upload_buf_len)) {
    return ERR_MEM;   /* NOT freed and NOT tcp_recved()d -- lwIP still owns it */
  }

  uint16_t taken_total = 0;
  struct pbuf *cur = p;
  while (cur) {
    const char *data = (const char *)cur->payload;
    uint16_t remaining = cur->len;
    uint16_t off = 0;

    if (!c->headers_seen) {
      while (off < remaining && c->header_len < sizeof(c->header_buf) - 1) {
        c->header_buf[c->header_len++] = data[off++];
        if (c->header_len >= 4 &&
            !memcmp(c->header_buf + c->header_len - 4, "\r\n\r\n", 4)) {
          c->header_buf[c->header_len] = 0;
          c->headers_seen = true;
          parse_headers(c);
          if (c->in_use && c->state == HTTP_ST_HEADERS) {
            dispatch(c);
          }
          break;
        }
      }
      if (!c->in_use) {
        /* dispatch() (or parse_headers()) closed the connection --
           pcb may already be invalid, don't touch it again. */
        pbuf_free(p);
        return ERR_OK;
      }
      if (!c->headers_seen && c->header_len >= sizeof(c->header_buf) - 1) {
        pbuf_free(p);
        send_status(c, 431, "Request Header Fields Too Large", "headers too large");
        return ERR_OK;
      }
    }

    /* Any bytes left in this pbuf after the header terminator (or all
       of it, if headers already completed in a previous pbuf) are
       body -- only meaningful once dispatch() has started an upload.
       Gated on is_uploading, NOT c->state == HTTP_ST_UPLOAD_BODY: a
       small file's entire POST (headers + full body) commonly arrives
       in this same pbuf, before the async FS_OP_OPEN_WRITE round trip
       that sets HTTP_ST_UPLOAD_BODY has even completed -- gating on
       that state instead silently dropped exactly this case (found
       2026-09-02 on real hardware: uploads never finished, because the
       body bytes that arrived alongside the headers were skipped and
       body_received never reached content_length). Accumulated into
       c->upload_buf (not c->fs_req.data, and not written to FatFs
       directly -- core 1 never touches FatFs, see fs_bridge.h);
       pump_upload() (httpd_poll(), every core-1 loop tick) drains it
       via the bridge once fs_req is actually free to reuse. */
    if (c->headers_seen && off < remaining && c->is_uploading) {
      uint32_t want = remaining - off;
      if (c->body_received + want > c->content_length)
        want = c->content_length - c->body_received;
      if (want > 0) {
        /* Safety net only. on_recv()'s entry check refuses any pbuf that
           will not fit whole, so by here it does. The one case that check
           cannot cover is the very first segment, where headers_seen is
           still false -- and that cannot overflow either, since
           upload_buf holds a full receive window (HTTP_UPLOAD_BUF_SIZE >=
           TCP_WND) and one segment is at most TCP_MSS. Kept because
           dropping the connection is still better than a buffer overrun
           if either of those invariants is ever broken. */
        uint32_t room = sizeof(c->upload_buf) - c->upload_buf_len;
        if (want > room) {
          tcp_recved(pcb, p->tot_len);
          pbuf_free(p);
          conn_close(c);
          return ERR_OK;
        }
        memcpy(c->upload_buf + c->upload_buf_len, data + off, want);
        c->upload_buf_len += want;
        c->body_received += want;
      }
    }

    taken_total += cur->len;
    cur = cur->next;
  }

  tcp_recved(pcb, taken_total);
  pbuf_free(p);

  return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
  http_conn_t *c = NULL;
  for (int i = 0; i < HTTP_MAX_CONNS; i++) {
    if (!conns[i].in_use) { c = &conns[i]; break; }
  }

  /* Every slot busy. Rather than drop the request that just arrived, take
     the slot of whichever connection has gone quietest -- a client that
     has sent nothing for a couple of poll ticks is far more likely to be
     an abandoned upload than a slow one, and the arriving client is
     provably alive.

     Measured 2026-09-05: with slots that were only abandoned by their
     clients, on_accept()'s unconditional tcp_abort() made the server
     answer *nothing* until the idle reaper caught up ~6s later. Requests
     were lost, not merely delayed, which is the one outcome the brief
     for this ruled out.

     A connection still making progress (idle_polls == 0) is never
     evicted: if every slot is genuinely active this is real overload and
     refusing is the honest answer. */
  if (!c) {
    http_conn_t *victim = NULL;
    for (int i = 0; i < HTTP_MAX_CONNS; i++) {
      if (!conns[i].in_use || conns[i].idle_polls == 0) continue;
      if (!victim || conns[i].idle_polls > victim->idle_polls)
        victim = &conns[i];
    }
    if (!victim) {
      tcp_abort(newpcb);
      return ERR_ABRT;
    }
    conn_close(victim);   /* clears in_use, and frees any bridge handle */
    c = victim;
  }

  memset(c, 0, sizeof(*c));
  c->in_use = true;
  c->pcb = newpcb;
  c->state = HTTP_ST_HEADERS;
  c->fs_handle = -1; /* memset() alone would leave this looking like handle 0 is open */

  tcp_arg(newpcb, c);
  tcp_recv(newpcb, on_recv);
  tcp_sent(newpcb, on_sent);
  tcp_err(newpcb, on_err);
  tcp_poll(newpcb, on_poll, 4); /* ~2s at the default 500ms poll tick */
  tcp_nagle_disable(newpcb);

  return ERR_OK;
}

/*--------------------------------------------------------------------------
   Public entry point
---------------------------------------------------------------------------*/

void httpd_init(void) {
  struct tcp_pcb *pcb = tcp_new();
  if (!pcb) {
    uart_puts_P(PSTR("httpd: tcp_new failed\r\n"));
    return;
  }
  if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
    uart_puts_P(PSTR("httpd: bind failed\r\n"));
    return;
  }
  struct tcp_pcb *listening = tcp_listen(pcb);
  if (!listening) {
    uart_puts_P(PSTR("httpd: listen failed\r\n"));
    return;
  }
  tcp_accept(listening, on_accept);
  uart_puts_P(PSTR("httpd: listening on port 80\r\n"));
}
