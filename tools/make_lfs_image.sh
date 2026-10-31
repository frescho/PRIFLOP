#!/bin/bash
# Build a littlefs image for the Pico's internal flash region.
#
#   ./make_lfs_image.sh out.img [file | src=dest/path ...]
#
# Compiles mklittlefs against the same vendored littlefs the firmware uses,
# so the on-disk format cannot drift from what the device expects.
# Flash address: ./make_fat_image.py --where  (it is a property of the
# partition map, not of the filesystem).
set -e
cd "$(dirname "$0")"
BIN=$(mktemp -d)/mklittlefs
gcc -O2 -I ../src/littlefs -o "$BIN" mklittlefs.c \
    ../src/littlefs/lfs.c ../src/littlefs/lfs_util.c
"$BIN" "$@"
