#!/usr/bin/env python3
"""Put the PRIFLOP files from this release onto your drive, over Wi-Fi.

  python3 deploy.py --ip 192.168.4.1                 the Primo files
  python3 deploy.py --ip 192.168.4.1 --c64-tests     also the C64 test programs
  python3 deploy.py --ip 192.168.4.1 --dry-run       say what it would do

The drive must be on and reachable (its own network PRIFLOP: 192.168.4.1;
on yours: the address your router gave it, or priflop.local).

What it uploads, from the folders beside this script:

  primo/boot.prg   as the Primo's boot file. The Primo's boot ROM loads the
                   file named with the single byte 0x85 (the CMD token),
                   which the web page cannot type; this script can. An
                   existing, different boot file is kept unless you give
                   --replace-boot.
  primo/PFM.PRG    PFM, the file selector.
  c64/             with --c64-tests: IECTEST, IMGTEST and HUNGTEST, the disk image
                   TESTDISK.D64, and the empty images IMGTEST formats.

Python 3.8 or later, nothing else to install.
"""
import argparse
import base64
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
BOOT_SLOT = "\x85"
BLANK_IMAGES = {"TEST.D64": 174848, "TEST71.D71": 349696, "TEST.DNP": 262144, "TEST81.D81": 819200}


class Drive:
    def __init__(self, ip, password):
        self.base = f"http://{ip}"
        token = base64.b64encode(f"admin:{password}".encode()).decode()
        self.headers = {"Authorization": f"Basic {token}"}

    def _req(self, method, path, data=None, timeout=60):
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=self.headers)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()

    def listing(self):
        """{NAME: entry}, free bytes. Names are raw bytes: the boot file's is 0x85."""
        _, body = self._req("GET", "/api/list?path=/")
        d = json.loads(body.decode("latin-1"))
        return {e["name"].upper(): e for e in d["entries"]}, d["free"]

    def download(self, name):
        return self._req("GET", "/files/" + urllib.parse.quote(name, safe=".!", encoding="latin-1"))[1]

    def upload(self, name, data):
        path = urllib.parse.quote(name, safe=".!", encoding="latin-1")
        return self._req("POST", f"/files/{path}", data=data, timeout=300)[0]


def show(name):
    return "<boot file>" if name == BOOT_SLOT else name


def plan(args, present, drive):
    """[(name, bytes)] to upload, and notes."""
    todo, notes = [], []
    boot = (HERE / "primo" / "boot.prg").read_bytes()
    if BOOT_SLOT in present and not args.replace_boot:
        if drive.download(BOOT_SLOT) == boot:
            notes.append("boot file: already this release's, kept")
        else:
            notes.append("boot file: a different one is on the drive, kept (--replace-boot replaces it)")
    else:
        todo.append((BOOT_SLOT, boot))
    todo.append(("PFM.PRG", (HERE / "primo" / "PFM.PRG").read_bytes()))
    if args.c64_tests:
        for f in sorted((HERE / "c64").iterdir()):
            todo.append((f.name, f.read_bytes()))
        for name, size in BLANK_IMAGES.items():
            if name in present:
                notes.append(f"{name}: present, kept")
            else:
                todo.append((name, bytes(size)))
    return todo, notes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", required=True, help="the drive's address")
    ap.add_argument("--password", default="admin", help="the admin password (default: admin)")
    ap.add_argument("--c64-tests", action="store_true", help="also the C64 test programs and images")
    ap.add_argument("--replace-boot", action="store_true", help="replace a different boot file")
    ap.add_argument("--dry-run", action="store_true", help="say what would be uploaded, upload nothing")
    args = ap.parse_args(argv)

    drive = Drive(args.ip, args.password)
    try:
        present, free = drive.listing()
    except urllib.error.HTTPError as e:
        print(f"the drive at {args.ip} refused: {e.code} {e.reason}"
              + (" -- wrong password?" if e.code == 401 else ""), file=sys.stderr)
        return 2
    except (urllib.error.URLError, OSError) as e:
        print(f"cannot reach the drive at {args.ip}: {e}", file=sys.stderr)
        return 2
    print(f"drive {args.ip}: {len(present)} files, {free:,} bytes free")

    todo, notes = plan(args, present, drive)
    for n in notes:
        print(" ", n)
    need = sum(len(d) for _, d in todo)
    if need > free:
        print(f"not enough room: {need:,} bytes to upload, {free:,} free", file=sys.stderr)
        return 1
    failed = 0
    for name, data in todo:
        if args.dry_run:
            print(f"  {show(name):13s} would upload {len(data):,} bytes")
            continue
        try:
            status = drive.upload(name, data)
        except (urllib.error.URLError, OSError) as e:
            status = str(e)
        ok = status == 200
        failed += not ok
        print(f"  {show(name):13s} {len(data):>8,} bytes  {'uploaded' if ok else 'FAILED: ' + str(status)}")
    if not args.dry_run and not failed:
        print("done. Reset the Primo with the drive connected to start our DOS.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
