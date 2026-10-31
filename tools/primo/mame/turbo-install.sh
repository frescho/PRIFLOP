#!/bin/sh
# Install CDOS, then the turbo, on the real B64 ROM in MAME -- no drive needed:
# boot80.prg goes to 0x4500 and is entered there, as the boot ROM does; TURBO's
# record body goes to 0x7000 and is entered there, as CDOS's C3 record does.
# Prints CDOS's base, BASIC's top of memory and the three table entries after
# each step, and leaves two snapshots: after CDOS, after TURBO. This is how the
# installer's lost IX (garbage instead of its banner) was found, 2026-10-05.
#
#   tools/primo/mame/turbo-install.sh [outdir]      default: /tmp/turbo-install
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/../../.."
OUT=${1:-/tmp/turbo-install}
mkdir -p "$OUT"
cp "$ROOT/primo-library/bin/cdos/boot80.prg" "$OUT/boot80.bin"
python3 -c "
import sys; sys.path.insert(0, '$ROOT/tools/primo/turbo'); import build_turbo as b
open('$OUT/turbo_body.bin', 'wb').write(b.prg()[5:-3])"
sed "s|@OUT@|$OUT|g" "$HERE/turbo-install.lua" > "$OUT/run.lua"
rm -rf "$OUT/snap"
cd "$OUT" && mame primob64 -bios cdos -rompath "$ROOT/tools/external/mame-roms" \
  -video none -sound none -nothrottle -skip_gameinfo \
  -snapshot_directory "$OUT/snap" -autoboot_script "$OUT/run.lua" 2>&1 | grep -E '^(basic|cdos|turbo):'
ls "$OUT"/snap/primob64/*.png
