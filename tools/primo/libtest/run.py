#!/usr/bin/env python3
"""Run every program of the converted library in MAME, in parallel.

  run.py <library dir (unpacked build_library.py zip)> <out dir> [-j N] [NAME ...]

Each program gets <out>/<FOLDER>_<NAME>/: run.lua's log (the records loaded,
how it started, PC and SP at each screen) and four screens, and a contact
sheet sheet.png with the four side by side (5 s, 20 s, after SPACE, after
RETURN). summary.json lists, per program, the log and two machine checks:
whether the screens differ at all, and whether the last screen is blank.
MAME's ROMs come from tools/external/mame-roms (the user's own, not shipped).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import pathlib
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ROMS = ROOT / "tools" / "external" / "mame-roms"
LUA, CDOS = "run.lua", ""          # --cdos: run2.lua, CDOS installed first, more keys


def one(prg, lib, out):
    tag = str(prg.relative_to(lib)).replace("/", "_")
    o = out / tag
    o.mkdir(parents=True, exist_ok=True)
    lua = (HERE / LUA).read_text().replace("@PRG@", str(prg)).replace("@OUT@", str(o)).replace("@CDOS@", CDOS)
    (o / "run.lua").write_text(lua)
    try:
        r = subprocess.run(["mame", "primob64", *(["-bios", "cdos"] if CDOS else []), "-rompath", str(ROMS), "-video", "none", "-sound", "none",
                            "-nothrottle", "-skip_gameinfo", "-snapshot_directory", str(o / "snap"),
                            "-autoboot_script", str(o / "run.lua")],
                           cwd=o, capture_output=True, text=True, timeout=300)
        rc = r.returncode
    except subprocess.TimeoutExpired:
        rc = "timeout"
    shots = sorted((o / "snap" / "primob64").glob("*.png")) if (o / "snap" / "primob64").exists() else []
    if shots:
        subprocess.run(["convert", *map(str, shots), "+append", "-bordercolor", "red", "-border", "2",
                        str(o / "sheet.png")], check=False)
    hashes = [hashlib.sha256(s.read_bytes()).hexdigest() for s in shots]
    blank = None
    if shots:
        mean = subprocess.run(["convert", str(shots[-1]), "-format", "%[fx:mean]", "info:"],
                              capture_output=True, text=True).stdout.strip()
        blank = mean in ("0", "1") or (mean and float(mean) < 0.002)
    log = (o / "log.txt").read_text() if (o / "log.txt").exists() else ""
    return tag, {"rc": rc, "shots": len(shots), "distinct_screens": len(set(hashes)),
                 "last_blank": blank, "log": log.splitlines()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("out")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--cdos", help="CDOS's boot80.prg: install it first, press more keys (run2.lua)")
    ap.add_argument("--long", action="store_true", help="run_long.lua: no keys, 120 s, a screen every 20 s")
    ap.add_argument("--list", help="a file of FOLDER/NAME.PRG lines: only these")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    global LUA, CDOS
    if a.cdos:
        LUA, CDOS = "run2.lua", str(pathlib.Path(a.cdos).resolve())
    if a.long:
        LUA = "run_long.lua"
    if a.list:
        a.names = [l.strip().split("/")[-1] for l in open(a.list) if l.strip()]
    lib, out = pathlib.Path(a.lib).resolve(), pathlib.Path(a.out).resolve()
    prgs = sorted(p for p in lib.rglob("*.PRG") if not a.names or p.name in a.names)
    summary = out / "summary.json"
    res = json.loads(summary.read_text()) if summary.exists() else {}   # a partial run adds to it
    with cf.ThreadPoolExecutor(a.j) as ex:
        for tag, r in ex.map(lambda p: one(p, lib, out), prgs):
            res[tag] = r
            print(f"{tag:40s} rc={r['rc']} screens={r['shots']} distinct={r['distinct_screens']} blank={r['last_blank']}",
                  flush=True)
    summary.write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
