#!/bin/bash
# Runs inside the release container (Containerfile): builds and tests
# /src, the exported source tree, and assembles the binary release in /out.
#   build-in-container.sh VERSION
set -euo pipefail
version=$1
src=/src
work=$(mktemp -d)
rel=$work/priflop-$version

cp -r "$src" "$work/tree"
cd "$work/tree"

# Step 6 of BUILDING.md: the firmware.
mkdir -p obj-pico2w-config
scripts/configparser.pl --genfiles --makeinc obj-pico2w-config/make.inc \
    --header obj-pico2w-config/autoconf.h configs/config-pico2w
cmake -S scripts/pico -B build -G Ninja -DPICO_SDK_PATH="$PICO_SDK_PATH" \
    -DPICO_BOARD=pico2_w -DAUTOCONF_DIR="$PWD/obj-pico2w-config" \
    -DCMAKE_C_COMPILER="$(command -v arm-none-eabi-gcc)" \
    -DCMAKE_CXX_COMPILER="$(command -v arm-none-eabi-g++)" > "$work/cmake.log"
cmake --build build >> "$work/cmake.log"

# Step 9: the host tests. A failure stops the release.
gcc -I src/pico -I src -o "$work/nctest" tests/netconfig_test.c \
    src/pico/netconfig.c src/sha256.c -DNETCONFIG_HOST_TEST && "$work/nctest"
gcc -I src/pico -o "$work/healthtest" tests/health_test.c src/pico/health.c && "$work/healthtest"
python3 tests/webui_test.py
python3 tests/iec_rx_test.py
python3 tests/release_deploy_test.py
python3 -m unittest discover -s tools/primo -p 'test_*.py'

# The binary release.
mkdir -p "$rel/primo" "$rel/c64" "$rel/docs"
cp build/priflop.uf2 "$rel/"
python3 tools/primo/dos/build_dos.py "$rel/primo/boot.prg"
python3 tools/primo/pfm/build_pfm.py "$rel/primo/PFM.PRG"
python3 - "$rel/c64" <<'PY'
import sys, pathlib
sys.path.insert(0, "tools")
import basic_tokenize
out = pathlib.Path(sys.argv[1])
for name, src in (("IECTEST.PRG", "tools/sd2iectest.bas"), ("IMGTEST.PRG", "tools/imgtest.bas"),
                  ("HUNGTEST.PRG", "tools/hungtest.bas")):
    (out / name).write_bytes(basic_tokenize.build(pathlib.Path(src).read_text()))
PY
cp tools/testdisk.d64 "$rel/c64/TESTDISK.D64"
cp scripts/release/deploy.py "$rel/"
cp README.md COPYING "$rel/"
cp -r docs/user/. "$rel/docs/"
sed -i 's#(docs/user/#(docs/#g; s#src="docs/user/#src="docs/#g' "$rel/README.md"
(cd "$rel" && find . -type f ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)

rm -f "/out/priflop-$version.zip"
(cd "$work" && zip -qr -X "/out/priflop-$version.zip" "priflop-$version")
echo "release: /out/priflop-$version.zip"
