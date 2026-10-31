#!/usr/bin/env bash
# bootstrap.sh - run ONCE on the target machine. Imports the bundled
# nix store closure into nix-portable's store. No root needed (unless
# you also install the udev rule at the end).
#
# NOTE: file existence is the source of truth here, NOT `nix-store -q`:
# a failed import can still register the path in the nix DB, which
# makes -q lie. An import also fails when its dependencies are not in
# the store yet, so we loop passes until a pass makes no progress.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NP_LOCATION="${NP_LOCATION:-$HOME}"
STORE_DIR="$NP_LOCATION/.nix-portable/store"
NP="$HERE/nix-portable"

if [ ! -x "$NP" ]; then echo "nix-portable missing" >&2; exit 1; fi
# provision nix-portable's own distribution (idempotent)
"$NP" nix --version > /dev/null

count=$(wc -l < "$HERE/store/manifest.txt")
echo "==> importing $count store paths into $STORE_DIR (one-time)"
pass=1
while :; do
  imported_this_pass=0
  i=0
  while read -r p; do
    i=$((i+1))
    dest="$STORE_DIR/$(basename "$p")"
    if [ -d "$dest" ] && [ -n "$(ls -A "$dest" 2>/dev/null)" ]; then
      continue   # files present (and complete: failures are cleaned up below)
    fi
    printf '\r  pass %d [%d/%d] %s' "$pass" "$i" "$count" "$(basename "$p")"
    if nix-store --export "$p" 2>/dev/null | "$NP" nix-store --import > /dev/null 2>&1; then
      imported_this_pass=$((imported_this_pass+1))
    else
      # partial import: remove so a later pass can retry cleanly
      rm -rf "$dest" 2>/dev/null || true
    fi
  done < "$HERE/store/manifest.txt"
  echo
  [ "$imported_this_pass" = 0 ] && break
  pass=$((pass+1))
done

missing=0
while read -r p; do
  dest="$STORE_DIR/$(basename "$p")"
  if [ ! -d "$dest" ] || [ -z "$(ls -A "$dest" 2>/dev/null)" ]; then
    echo "FAILED to import: $p" >&2
    missing=$((missing+1))
  fi
done < "$HERE/store/manifest.txt"
[ "$missing" = 0 ] || { echo "store incomplete - see errors above" >&2; exit 1; }
echo "==> store ready ($(ls "$STORE_DIR" | wc -l) paths)"

echo
echo "==> (optional, needs sudo) install the udev rule for HID access:"
echo "    sudo cp $HERE/60-rokid-max.rules /etc/udev/rules.d/ && sudo udevadm control --reload"
echo
echo "==> then run:  $HERE/run.sh          (hello_xr on the glasses)"
echo "           or  $HERE/run.sh <app>    (any OpenXR app)"
