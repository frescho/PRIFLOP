# Vendored littlefs

**v2.9.3**, from https://github.com/littlefs-project/littlefs
(tag `v2.9.3`), unmodified. BSD-3-Clause — see `LICENSE.md`; compatible
with this project's GPL-2.0.

Only the four source files are vendored (`lfs.c`, `lfs.h`, `lfs_util.c`,
`lfs_util.h`); the upstream tests, scripts and CI are not.

## Why it is here

See `../../littlefs-feasibility.md`. Short version: the FAT volume's
metadata (FAT table + root directory) lives at fixed offsets and is
rewritten on every file operation, so ~3 of the region's 256 erase blocks
absorb essentially all wear while the rest stays pristine. FAT has no wear
levelling at any sector or cluster size. littlefs does, and its
copy-on-write design also makes an interrupted write leave the previous
valid state rather than a torn volume — both corruptions this project has
suffered were torn multi-sector updates.

## Updating

Replace the four files from a new upstream tag and re-run the host test:

    cd tests && ./run_lfs_test.sh

That test pins the configuration this device actually uses (1 MB region,
4096-byte erase blocks) and exercises the access pattern `d64ops.c`
depends on — writing a full-size D64 image and seeking into the middle of
it — which is the thing most likely to break on an upgrade.
