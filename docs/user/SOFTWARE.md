# Getting started

## Getting the files

Everything you need to install is in the repository's
[`release/`](../../release/) folder:

| File | What |
|---|---|
| [`priflop.uf2`](../../release/priflop.uf2) | The firmware, for flashing |
| [`deploy.py`](../../release/deploy.py) | The tool that puts the files below on the drive |
| `primo/boot.prg`, `primo/PFM.PRG` | The Primo's boot file (our DOS) and PFM |
| `c64/` | The C64 test programs and their disk image |
| `primo_software/` | Primo program packages ([Primo software packages](PRIMO-SOFTWARE.md)) |
| `SHA256SUMS` | Checksums |

1. Download the repository as a zip: [https://github.com/frescho/PRIFLOP/archive/refs/heads/main.zip](https://github.com/frescho/PRIFLOP/archive/refs/heads/main.zip)
   (on GitHub: the green **Code** button, then **Download ZIP**).
2. Unpack it. The files are in the folder `PRIFLOP-main/release/`.

`deploy.py` needs the `primo/` and `c64/` folders next to it, so run it
from `release/` as it comes, not a copy of the script alone.

## Flashing

1. Unplug the Pico.
2. Hold its BOOTSEL button and plug it into a computer by USB. A drive called
   `RP2350` appears.
3. Copy `release/priflop.uf2` ([Getting the files](#getting-the-files)) onto it. The Pico restarts on its own as PRIFLOP.

A reflash keeps your files and your saved drive settings.

## First start and Wi-Fi

With no settings, the drive starts its own Wi-Fi access point:

| | |
|---|---|
| Network | `PRIFLOP` (no password by default) |
| Address | `http://192.168.4.1` |
| User / password | `admin` / `admin` |

The page shows a warning until you change the admin password. Do that first,
on the **Settings** tab.

To put the drive on your own network, enter its name and password on the
Settings tab and save. The drive restarts and joins it. It then gets its
address from your router, and it also answers as `http://priflop.local`
(the name is the `MDNS` setting). If it cannot join, it falls back to its
own access point.

The settings live in a text file on the drive, `PRIFLOP.CFG`:

| Key | Meaning |
|---|---|
| `MODE` | `AP` (its own network) or `STA` (join yours) |
| `SSID`, `PASSWORD` | The network to join |
| `AP_SSID`, `AP_PASSWORD` | The network it makes in AP mode |
| `MDNS` | The `.local` name in STA mode |
| `ADMIN` | The admin password, stored salted and hashed |
| `COUNTRY` | Two-letter Wi-Fi country code (empty: worldwide) |
| `PFM_AUTOSTART` | `ON`: the Primo's boot starts PFM ([The Primo](PRIMO.md)) |

## The release's files on the drive

The `release/` folder ([Getting the files](#getting-the-files)) has a small tool, `deploy.py` (Python 3.8 or later, nothing
to install), that uploads the Primo's boot file and PFM, and with
`--c64-tests` the C64 test programs. In a terminal, in the `release/` folder:

```bash
cd PRIFLOP-main/release
python3 deploy.py --ip 192.168.4.1 --dry-run     # see what it would do
python3 deploy.py --ip 192.168.4.1               # the Primo files
python3 deploy.py --ip 192.168.4.1 --c64-tests   # and the C64 tests
```

Use the drive's address (192.168.4.1 on its own network), and
`--password` once you have changed the admin password. A different boot
file already on the drive is kept unless you add `--replace-boot`.

## The web page

- **Files:** browse folders, upload (several files at once, or by dragging
  them onto the page), download, rename, delete, make folders. A second tab
  shows the RAM drive (device 9).
- **Settings:** Wi-Fi, the admin password, hidden file extensions, the
  device address, the Primo turbo, a restart, and BOOTSEL for flashing
  without the button.
- The header shows the free space and the drive's health.

## Using it from a C64

The drive is device 8; the RAM drive is device 9. The RAM drive is lost when
the power goes off.

```basic
LOAD"$",8          : rem the directory
LOAD"NAME",8,1
OPEN15,8,15,"CD:GAMES":CLOSE15     : rem into a folder
OPEN15,8,15,"CD:DISK.D64":CLOSE15  : rem mount an image
OPEN15,8,15,"CD:_":CLOSE15         : rem back up one level
```

The drive commands are sd2iec's; its [README](../../README) lists them all.
Settings changed with an `X` command are kept across a restart once you save
them with `XW`:

```basic
OPEN15,8,15,"XE+":CLOSE15   : rem hide .PRG/.SEQ extensions
OPEN15,8,15,"XW":CLOSE15    : rem save the settings
```

**Fast loading:** JiffyDOS works, with a JiffyDOS ROM or with SJLOAD on a
stock C64. The other fastloaders sd2iec supports are built in too.

## Troubleshooting

- **The web page is gone.** The drive's address may have changed: look for
  it in your router's list, or try `http://priflop.local`. Over USB, a serial
  terminal (with DTR on) shows the boot log, including the address it got.
- **`/api/stats`** (log in as admin) shows the drive's counters. `boots`
  above 1 means the drive restarted on its own since power-on;
  `/api/bootlog` keeps the log from before that restart.
- **A C64 switched off while connected restarts the drive.** A dead machine
  holds the bus low. This is expected.
- **Load errors when the C64's screen is captured on the same PC that powers
  the drive.** The two cables make a ground loop that corrupts bus data.
  Power the drive from a separate USB supply, or use an isolator.
