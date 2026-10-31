#!/bin/bash
# flash_layout.h on 4MB and 16MB, and a 1MB layout that must not compile.
set -e
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
for m in 4 16; do
  gcc -Wall -DMB=$m -DPICO_FLASH_SIZE_BYTES="($m*1024*1024)" -I src/pico \
      -o "$OUT/fltest$m" tests/flash_layout_test.c
  "$OUT/fltest$m"
done
if gcc -DMB=1 -DPICO_FLASH_SIZE_BYTES="(1024*1024)" -I src/pico -o "$OUT/fltest1" \
       tests/flash_layout_test.c 2>/dev/null; then
  echo "FAIL: a 1MB layout compiled"; exit 1
fi
echo "1 MB: refused at compile time: ok"
rm -rf "${OUT:?}"
