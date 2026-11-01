# Primo software packages

Ready-made collections of Primo programs, converted from the Primo's tape
library to the drive's format. They are in the repository's
[`release/primo_software/`](../../release/primo_software/) folder.

| Package | Programs | Zip | Unpacked | On a 4 MB drive |
|---|---|---|---|---|
| `primo-library-small.zip` | 307 | 980 KB | 1.8 MB | fits: 606 of 767 blocks, 161 free |
| `primo-library-complete.zip` | 511 | 1.6 MB | 2.8 MB | does not fit |
| `primo-library-all-converted.zip` | 569 | 1.7 MB | 3.0 MB | does not fit |

- **small:** the programs that worked in our tests (some after a fix or a
  repair), without the documents and utilities of the EGYEB folder; four
  educational programs are kept. Made to fit a 4 MB drive next to the boot
  file and PFM.
- **complete:** every program that worked in our tests.
- **all-converted:** the whole library as converted, untested programs and
  ones that do not work included. For browsing and for your own tests.

The block counts were measured on the drive's own file system layout, with
the boot file, PFM and `PRIFLOP.CFG` on it. A 16 MB board (untested) holds
any of them.

Each zip keeps the library's folders (`CDOS/`, `DEMOKAZI/`, ...) and has a
`LIST.TXT` with each program's test result and a short description.

## How they were made

1. `tools/primo/fetch-library.sh` downloads the tape library from the
   [Primo homepage](https://primo.homeserver.hu/), the files listed in
   `tools/primo/library.manifest`.
2. `tools/primo/build_library.py` converts every tape file (`.ptp`) to a
   drive file (`.PRG`) with `ptp2disk.py`. BASIC programs start on their own
   after `CMD LOAD`, as if saved with `AUTO`. Programs written for the 32 KB
   Primo get a small patch so they run on the 64 KB one. Names are cut to 16
   characters. CDOS's own boot files are left out.
3. Each program was started in an emulator and marked as working, fixed,
   repaired or not working.
4. `tools/primo/make_bundles.py` builds one zip per list
   (`tools/primo/library-small.list`, `library-complete.list`) and measures
   it on the drive's layout.

To change a package, edit its list and run step 4 again.

## Terms of use

The programs belong to their authors and publishers. They are not part of
PRIFLOP and are not covered by its GPL licence.

They were published for the Primo, a Hungarian home computer of the 1980s,
and have not been sold or supported for decades. We treat them as
abandonware and include them in good faith, for preservation and for
personal, non-commercial use on real Primos and emulators, in the same
spirit as the [Primo homepage](https://primo.homeserver.hu/) that keeps the
library.

- **Not for sale.** Do not sell these packages or the programs in them, and
  do not bundle them with a product you sell.
- **No business use**, and no use that harms anyone.
- **Rights holders:** if you own the rights to a program and want it
  removed, open an issue on the
  [repository](https://github.com/frescho/PRIFLOP/issues) and it will be
  taken out.

This is not legal advice, and abandonware is not a legal status: using
these files is your own decision.
