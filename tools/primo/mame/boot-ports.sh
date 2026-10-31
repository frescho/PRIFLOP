#!/bin/sh
# 6p: our own DOS's boot file (BOOTD) on MAME's B64 with the CDOS BIOS, on each
# IEC port variant. The ROM's two port bytes (0x029F, 0x0687: the operands of
# its OUT (n),A) say which; the loader must pick that port
# and the DOS must drive only that one. MAME's drive does not answer, so the
# probe must fail cleanly ("No drive") and leave BASIC as it was.
#
#   tools/primo/mame/boot-ports.sh <run> [outdir]     default outdir: /tmp/boot-ports
#     run: p80     the ROM as it is (0x80)
#          p40     a copy of the ROM with both port bytes 0x40 (never committed)
#          p40mut  p40, with a loader that ignores the ROM and always takes 0x80
#                  (the negative control: it must show writes to 0x80)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
RUN=$1
OUT=${2:-/tmp/boot-ports}
mkdir -p "$OUT/roms/primob64"
cp "$ROOT/tools/external/mame-roms/primob64/"*.rom "$OUT/roms/primob64/"
cp -r "$ROOT/tools/external/mame-roms/c1541c" "$OUT/roms/"
case $RUN in p40*) PORT=64 ;; p80) PORT=128 ;; *) echo "run: p80, p40 or p40mut" >&2; exit 2 ;; esac
OUTPORT=$("$ROOT/tools/external/venv-sim/bin/python" - "$OUT" "$ROOT" "$RUN" "$PORT" <<'PY'
import sys
out, root, run, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
rom = out + "/roms/primob64/b64cdos.rom"
r = bytearray(open(rom, "rb").read())
assert r[0x029E] == r[0x0686] == 0xD3 and r[0x029F] == r[0x0687] == 0x80   # OUT (80h),A twice
r[0x029F] = r[0x0687] = port
open(rom, "wb").write(r)
sys.path.insert(0, root + "/tools/primo/dos")
import build_dos as b
patch = ()
if run == "p40mut":
    patch = (("    cp 40h\n    jr z,port\n", "    cp 40h\n    jr port80\n"),)
f, s = b.boot_file(patch=patch)
open(out + "/" + run + ".bin", "wb").write(f)
o = s["port"] - 0x4500
assert f[o] == 0x32                     # port: ld (IMG+B_OUTPORT),a
print(f[o + 1] | f[o + 2] << 8)
PY
)
sed "s|@OUT@|$OUT|g; s|@RUN@|$RUN|g; s|@OUTPORT@|$OUTPORT|g" "$HERE/boot-ports.lua" > "$OUT/$RUN.lua"
rm -rf "$OUT/snap_$RUN"
(cd "$OUT" && mame primob64 -bios cdos -rompath "$OUT/roms" \
  -video none -sound none -nothrottle -skip_gameinfo \
  -snapshot_directory "$OUT/snap_$RUN" -autoboot_script "$OUT/$RUN.lua" 2>&1 | grep -E "^$RUN |WRONG|NOT FOUND|rror|xception" | tee "$OUT/$RUN.log")
ls "$OUT/snap_$RUN"/primob64/*.png
# The verdict: the ROM's port taken and the only one driven, nothing driven
# while BASIC was idle, BASIC's top and CDOS's base as they were. For p40mut
# the check must fail (exit 0 then: the mutation was caught).
python3 - "$OUT/$RUN.log" "$RUN" <<'PY'
import re, sys
log, run = open(sys.argv[1]).read(), sys.argv[2]
v = lambda line: dict(re.findall(r"(\w+)=([0-9A-F]+)", line))
b = v(next(l for l in log.splitlines() if " basic:" in l))
a = v(next(l for l in log.splitlines() if " after:" in l))
port = b["rom029F"]
other = "80" if port == "40" else "40"
bad = [k for k, ok in [("rom bytes agree", b["rom029F"] == b["rom0687"]),
                       ("outport", a["outport"] == port),
                       ("driven", int(a["boot" + port]) > 0),
                       ("other port idle", a["boot" + other] == "0"),
                       ("idle control", a["idle40"] == a["idle80"] == "0"),
                       ("40B1", a["40B1"] == b["40B1"]), ("4174", a["4174"] == b["4174"])] if not ok]
if run == "p40mut":
    print("p40mut: caught (" + ", ".join(bad) + ")" if bad else "p40mut: NOT caught"); sys.exit(0 if bad else 1)
print(run + (": PASS" if not bad else ": FAIL (" + ", ".join(bad) + ")")); sys.exit(1 if bad else 0)
PY
