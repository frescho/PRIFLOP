# Quick start: the Primo

What you need:
- a Primo with the Commodore serial port and the CDOS boot EPROM. A B
  series Primo has the port; the EPROM has to be replaced, which means
  soldering. An A series Primo also needs the port soldered in. See
  [What your Primo needs](PRIMO.md#what-your-primo-needs) and the
  [Primo homepage](https://primo.homeserver.hu/html/hardvercdos.html);
- a Pico 2 W wired as in [Hardware](HARDWARE.md);
- a Primo-to-Commodore serial cable;
- a USB power supply;
- the files from the repository's [`release/`](../../release/) folder: download
  [the repository as a zip](https://github.com/frescho/PRIFLOP/archive/refs/heads/main.zip) and unpack it
  ([Getting the files](SOFTWARE.md#getting-the-files)).

1. **Flash.** Hold BOOTSEL, plug the Pico into a computer, and copy
   `PRIFLOP-main/release/priflop.uf2` to the `RP2350` drive that appears.
2. **Wi-Fi.** Join the network `PRIFLOP` and open `http://192.168.4.1`
   (user `admin`, password `admin`). Change the password on the Settings tab.
3. **Put the Primo files on the drive.** In a terminal:
   `cd PRIFLOP-main/release`, then
   `python3 deploy.py --ip 192.168.4.1`
   This uploads the boot file and PFM. The boot file's name is one raw
   byte, so the web page cannot upload it; use `deploy.py`.
4. **Connect.** Power the drive first, then plug the cable into the Primo's
   serial port.
5. **Reset the Primo while holding BRK.** Keep BRK held down, press reset,
   and let go of BRK after the reset. The boot file loads and installs our
   DOS. The banner says `turbo` when the fast load is on.
6. **Load.** `CMD LOAD` with no name opens PFM, the file selector:
   - ↑ ↓ move;
   - ← → page;
   - RETURN loads, or enters a folder;
   - C opens the settings;
   - Q quits.

   `CMD LOAD"NAME"` and `CMD SAVE"NAME"` work as under CDOS 1.0.
7. **Your programs.** Upload them on the web page's Files tab. Convert tape
   programs (`.ptp`) first with `tools/primo/ptp2disk.py`.

Optional: `PFM_AUTOSTART=ON` in `PRIFLOP.CFG` (or on PFM's settings page)
starts PFM at every reset.

If something goes wrong:
- **Nothing loads at reset:** check that the drive was powered before the
  Primo, and that `deploy.py` put the boot file on the drive.
- **The boot says the turbo was refused:** the boot file switches the turbo
  on by itself, so this means the drive could not start it. The Primo is left
  as it was. Read the drive's `/api/bootlog` (see
  [troubleshooting](SOFTWARE.md#troubleshooting)).
- **The web page is gone:** see [troubleshooting](SOFTWARE.md#troubleshooting).

Everything in more detail: [The Primo](PRIMO.md).
