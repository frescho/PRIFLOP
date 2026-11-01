# The Primo

PRIFLOP works with the Microkey Primo B64 through its serial port, with a
DOS of our own: compatible with CDOS 1.0 for programs, and faster for loading
and saving.

## What your Primo needs

The Primo's ROM has no disk routines. To start from the drive, a Primo needs
two things: the Commodore serial port, and a ROM that can boot from disk.
The [Primo homepage's floppy page][cdos] (in Hungarian) describes both in
detail.

- **The serial port.**
  - **B series:** built in. You only need the cable (see
    [Hardware](HARDWARE.md#the-primo)).
  - **A series:** solder in the missing DIN socket, ICs and resistors. They
    are in the Primo hardware manual, "optional Commodore serial interface",
    pages 76–78.
- **The boot EPROM.** The ROM sits in four EPROMs. The first (0–4 KB) is
  replaced with the CDOS boot version, which loads the boot file from disk
  when BRK is held during a reset. This means:
  - desoldering the old chip;
  - erasing it (UV) and programming it;
  - soldering in a socket.

  There are two images, by the machine's serial port address:
  `CDOSA.BIN` (0x80) and `CDOSA641.BIN` (0x40). The homepage's author
  offered to erase and program EPROMs by post.
- **Or an external CDOS boot card** on the expansion port. It loads the boot
  file at power-on, with no EPROM change. *PRIFLOP has not been tested with
  such a card.*

PRIFLOP was tested on a Primo B64 ('84.2 ROM) with the CDOS boot EPROM. The
boot file checks that the ROM is one it knows before it installs anything.

[cdos]: https://primo.homeserver.hu/html/hardvercdos.html

## What goes on the drive

| File | What it is |
|---|---|
| the boot file (named with the single byte 0x85) | Our DOS. The Primo's boot ROM loads this file after every reset. |
| `PFM.PRG` | PFM, the file selector |

`release/deploy.py` puts both on the drive (see [Getting started](SOFTWARE.md#the-releases-files-on-the-drive)). The boot file's name is
one raw byte (0x85, the Primo's `CMD` token), not a name you can type, so
upload it with the tool rather than through the web page.

## Starting

Hold BRK and reset the Primo with the drive connected; let go of BRK after
the reset. The boot file loads, checks that
the ROM is one it knows, finds the serial port (0x40 or 0x80) in the ROM, and
installs our DOS. The banner says `turbo` when the drive has switched its
half of the turbo on. If the drive refuses the turbo, the boot says why and
leaves the machine as it was.

Our DOS does what CDOS 1.0 does: `CMD LOAD`, `CMD SAVE`, `CMD LOAD SCREEN`
and the rest, with the same messages. Every load and save goes through the
turbo.

## The turbo

The Primo sets the pace: one bit per CLOCK edge, in frames with a checksum.
The drive answers each frame, and a frame that arrives damaged is sent
again. The drive keeps the turbo switch across restarts and reflashes once
it is saved: the Settings tab's "Primo turbo", or `XT+` then `XW` over the
bus.

## PFM, the file selector

`CMD LOAD` with no name starts PFM. (CDOS answers that with `MO Error`, so no
program depends on it.)

| Key | In the file list |
|---|---|
| ↑ ↓ | Move the bar |
| ← → | Previous or next page |
| RETURN | Load the file, or enter a folder or disk image |
| C | The settings page |
| Q | Quit to BASIC |

The settings page edits the drive's `PRIFLOP.CFG` (Wi-Fi, the device address)
from the Primo: RETURN changes a field, W writes, R restarts the drive, BRK
goes back.

With `PFM_AUTOSTART=ON` in `PRIFLOP.CFG`, the boot starts PFM on its own.

PFM is not resident: when it loads a program, it hands the machine back as
`CMD LOAD` would have left it.

## Programs for the Primo

The Primo's tape programs (`.ptp`) are converted for the drive with
`tools/primo/ptp2disk.py`. `tools/primo/fetch-library.sh` downloads the
library from primo.homeserver.hu for your own use; the programs belong to
their authors and are not part of this release.
