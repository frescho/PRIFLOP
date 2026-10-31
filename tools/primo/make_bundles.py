#!/usr/bin/env python3
"""Build the library bundles from their lists, and measure them.

  ./make_bundles.py [--flash-mb N] <library.zip> <out dir> [LIST ...]

<library.zip> is what build_library.py made. Each LIST (default: the
library-*.list files beside this script) gives one bundle,
<out dir>/primo-<list name>.zip: the programs it names, in their folders,
and LIST.TXT with each one's label and description. Then the bundle is
measured on the drive's own geometry (tools/make_lfs_image.sh: 767 blocks
of 4 KB on a 4 MB board, 3839 with --flash-mb 16) with the drive's own files beside it, the boot file, PFM,
PRIFLOP.CFG and the C64 test programs, about 50 blocks: "fits, N blocks
free" or "does not fit".

To add a program to a bundle, add its line to the list and run this again.
The programs belong to their authors: the bundles stay with the user.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DRIVE_FILES = {"BOOT85": 3795, "PFM.PRG": 4995, "PRIFLOP.CFG": 211,     # sizes as on the drive,
               "IECTEST.PRG": 3812, "IMGTEST.PRG": 2468, "TESTDISK.D64": 174848}   # 2026-10-15


def read_list(path):
    out = []
    for line in open(path, encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            name, *rest = line.rstrip("\n").split("\t")
            out.append((name, rest[0] if rest else "", rest[1] if len(rest) > 1 else ""))
    return out


def blocks_for(mb):
    """The volume's blocks on an mb-MB board: src/pico/flash_layout.h."""
    return (mb * 1024 * 1024 - 1024 * 1024 - 4096) // 4096


def measure(zip_path, mb=4):
    """(fits, blocks used) on the drive's geometry, its own files included."""
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        args = []
        for name, size in DRIVE_FILES.items():
            (t / name).write_bytes(bytes(size))
            args.append(f"{t / name}={name}")
        with zipfile.ZipFile(zip_path) as z:
            for n in z.namelist():
                if n == "LIST.TXT":
                    continue
                p = t / "lib" / n
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(z.read(n))
                args.append(f"{p}={n}")
        r = subprocess.run([str(ROOT / "tools" / "make_lfs_image.sh"), "-m", str(mb), str(t / "x.img"), *args],
                           capture_output=True, text=True)
        last = (r.stdout.strip().splitlines() or [""])[-1]
        if r.returncode or "blocks used" not in last:
            return False, None
        return True, int(last.split(",")[-1].split()[0])


def main():
    args = sys.argv[1:]
    mb = 4
    if args[:1] == ["--flash-mb"]:
        mb, args = int(args[1]), args[2:]
    total = blocks_for(mb)
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    lib, out = args[0], pathlib.Path(args[1])
    lists = [pathlib.Path(a) for a in args[2:]] or sorted(HERE.glob("library-*.list"))
    out.mkdir(parents=True, exist_ok=True)
    src = zipfile.ZipFile(lib)
    have = set(src.namelist())
    bad = 0
    for lst in lists:
        entries = read_list(lst)
        missing = [n for n, _, _ in entries if n not in have]
        if missing:
            print(f"{lst.name}: not in {lib}: {', '.join(missing[:10])}", file=sys.stderr)
            bad += 1
            continue
        zp = out / f"primo-{lst.stem}.zip"
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
            for n, label, desc in entries:
                z.writestr(n, src.read(n))
            z.writestr("LIST.TXT", "".join(f"{n}\t{label}\t{desc}\n" for n, label, desc in entries))
        fits, used = measure(zp, mb)
        print(f"{zp.name}: {len(entries)} programs, {os.path.getsize(zp):,} bytes; "
              + (f"fits: {used} of {total} blocks, {total - used} free" if fits else f"does not fit the {mb} MB drive"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
