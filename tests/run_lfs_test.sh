#!/bin/bash
# Host-side test for the vendored littlefs, pinned to this device's real
# flash geometry. No hardware needed.
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
gcc -I ../src/littlefs -o "$OUT/lfs_geometry_test" \
    lfs_geometry_test.c ../src/littlefs/lfs.c ../src/littlefs/lfs_util.c
"$OUT/lfs_geometry_test"
rm -rf "$OUT"
