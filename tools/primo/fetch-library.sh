#!/bin/sh
# Fetch the Primo software library for testing.
#
# The files are third-party and are NOT committed to this repository --
# only this script and library.manifest are, the same arrangement as
# tools/c64/. That keeps the repository reproducible without taking a
# redistribution decision on 496 files belonging to named authors. Running
# this script is the user's own call.
#
#   ./fetch-library.sh              fetch into ../../primo-library/
#   ./fetch-library.sh <dir>        fetch somewhere else
#
# The target directory is gitignored.
#
# Source: https://primo.homeserver.hu/ -- the Primo preservation site.
# Its certificate chain is incomplete (the ZeroSSL intermediate is not
# served), so --no-check-certificate is required; the certificate itself
# is valid for the host.
set -e

BASE="https://primo.homeserver.hu"
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-"$HERE/../../primo-library"}
MANIFEST="$HERE/library.manifest"

[ -f "$MANIFEST" ] || { echo "missing $MANIFEST" >&2; exit 1; }
mkdir -p "$DEST"

total=$(wc -l < "$MANIFEST")
echo "== fetching $total files into $DEST"

n=0 got=0 skipped=0 failed=0
while IFS= read -r path; do
  [ -n "$path" ] || continue
  n=$((n + 1))
  out="$DEST/$path"

  # Already present and non-empty: leave it. Re-running is cheap and safe.
  if [ -s "$out" ]; then
    skipped=$((skipped + 1))
    continue
  fi

  mkdir -p "$(dirname "$out")"
  # --wait keeps this polite: it is one person's hobby server, and 496
  # requests arriving as fast as the link allows is not a reasonable way
  # to treat it.
  if wget --no-check-certificate -q --tries=2 --timeout=30 \
          --wait=0.2 --random-wait \
          -U "Mozilla/5.0" -O "$out" "$BASE/$path" 2>/dev/null; then
    got=$((got + 1))
  else
    rm -f "$out"
    failed=$((failed + 1))
    echo "  FAILED  $path" >&2
  fi

  [ $((n % 50)) -eq 0 ] && echo "  ...$n/$total"
done < "$MANIFEST"

echo "== done: $got fetched, $skipped already present, $failed failed"
[ "$failed" -eq 0 ] || exit 1
