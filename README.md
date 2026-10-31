<p align="center"><img src="docs/user/images/logo.png" alt="PRIFLOP" width="536"></p>

# PRIFLOP

**Magyarul:** [a PRIFLOP leírása magyarul](docs/user/README-HU.md) -- a Primo felhasználóinak.

A disk drive for the Commodore 64 and the Microkey Primo, built on a
Raspberry Pi Pico 2 W. It plugs into the serial (IEC) bus, keeps its files in
the Pico's own flash, and has a web page for uploading and managing them over
Wi-Fi.

PRIFLOP is a port of [sd2iec](https://www.sd2iec.de/) by Ingo Korb and
contributors to the RP2350, with these additions:

- **No SD card.** The files live on a 3 MB littlefs volume in the Pico's flash.
- **A web file manager** over Wi-Fi: browse, upload, download, rename, delete,
  make folders, set up Wi-Fi, and read the drive's diagnostics.
- **A RAM drive** as device 9, for testing without wearing the flash.
- **JiffyDOS**, at about 12 KB/s with SJLOAD on a stock C64.
- **D64, D71, D81 and DNP** disk images, mounted with `CD`.
- **For the Primo:** a DOS of our own, compatible with CDOS 1.0, that loads
  and saves through a checksummed turbo, and **PFM**, a file selector that
  starts with `CMD LOAD`.

## Documentation

| | |
|---|---|
| [Hardware](docs/user/HARDWARE.md) | The board, the bus wiring, parts, power |
| [Getting started](docs/user/SOFTWARE.md) | Flashing, first start, Wi-Fi, the web page, drive commands, troubleshooting |
| [Quick start: the Primo](docs/user/QUICKSTART-PRIMO.md) | From a bare Pico to `CMD LOAD` on a Primo, in seven steps |
| [The Primo](docs/user/PRIMO.md) | The boot file, our DOS, the turbo, PFM |
| [Test programs](docs/user/TESTS.md) | IECTEST, IMGTEST, HUNGTEST, CRCCHECK, PTURBO: why, how, and what a pass looks like |
| [Building from source](docs/user/BUILDING.md) | Toolchains, the build, the tests |

## Quick start

1. Download the release zip and unpack it.
2. Hold the Pico's BOOTSEL button while plugging it into a computer, and copy
   `priflop.uf2` to the `RP2350` drive that appears.
3. Join the Wi-Fi network `PRIFLOP` and open `http://192.168.4.1`. The user
   name is `admin` and so is the password: change it on the Settings tab.
4. For the Primo, run `python3 deploy.py --ip 192.168.4.1` from the release
   folder: it uploads the boot file and PFM.
5. Upload your files, connect the drive to the computer's serial port, and
   `LOAD"$",8`.

## Licence

GPL version 2 only; see [COPYING](COPYING). sd2iec is by Ingo Korb and its
contributors (see [README](README) and [NEWS](NEWS)); third-party parts keep
their own notices: littlefs (BSD-3-Clause, `src/littlefs/`), Dean Camera's
`src/pico/atomic.h` and Georges Menie's `src/pico/crc.c` (both BSD). sd2iec's
headers also credit ChaN, whose code no longer remains. The RP2350
port, the web interface and the Primo software are by frescho.

Not included, and not needed: CDOS, the Primo's ROMs, and the Primo's
program library. `tools/primo/fetch-library.sh` downloads the library from
primo.homeserver.hu for your own use.
