#!/bin/sh
# Boot our own DOS's boot file (phase A: BOOTU + the turbo, build_turbo.boot_file)
# on the real B64 ROM in MAME, twice -- the second time with everything
# resident, as a Reset+BRK does -- in two variants: as built (MAME has no drive
# that answers, so the XT+ probe fails and it must be plain CDOS) and with the
# probe forced to succeed (the turbo must be installed, and not twice). Prints
# BASIC's top, CDOS's base and the three table entries after each boot; leaves
# the screens in <outdir>/snap_*.
#
#   tools/primo/mame/boot-turbo.sh [outdir]      default: /tmp/boot-turbo
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/../../.."
OUT=${1:-/tmp/boot-turbo}
mkdir -p "$OUT"
"$ROOT/tools/external/venv-sim/bin/python" - "$OUT" "$ROOT" <<'PY' 2>/dev/null
import sys
out, root = sys.argv[1], sys.argv[2]
sys.path.insert(0, root + "/tools/primo/turbo")
import build_turbo as b
f, s = b.boot_file(open(root + "/primo-library/bin/cdos/boot80.prg", "rb").read())
open(out + "/boot.bin", "wb").write(f)
o = s["boot"] - 0x4500
assert f[o + 8] == 0xEF and f[o + 9] == 0xDA and f[o + 12] == 0xCA   # rst 28h; jp c; jp z
g = bytearray(f)
g[o + 6:o + 15] = bytes(9)              # ld a,18h; rst 28h; jp c; jp z -> NOPs
open(out + "/boot_forced.bin", "wb").write(g)
PY
for v in boot boot_forced; do
  sed "s|@OUT@|$OUT|g; s|@IMG@|$v|g" "$HERE/boot-turbo.lua" > "$OUT/$v.lua"
  rm -rf "$OUT/snap_$v"
  (cd "$OUT" && mame primob64 -bios cdos -rompath "$ROOT/tools/external/mame-roms" \
    -video none -sound none -nothrottle -skip_gameinfo \
    -snapshot_directory "$OUT/snap_$v" -autoboot_script "$OUT/$v.lua" 2>&1 | grep -E '^boot')
done
ls "$OUT"/snap_*/primob64/*.png
