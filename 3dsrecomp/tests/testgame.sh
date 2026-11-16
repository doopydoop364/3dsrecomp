#!/bin/sh
# Prepares build/testgame: a test copy of the game folder so scripted runs never touch game/ (where the user plays).
# Read-only data is symlinked; save, extdata and extdata_shared are restored from build/pristine every time.
cd "$(dirname "$0")/.." || exit 1
T=build/testgame
[ -d build/pristine ] || { echo "no build/pristine (a clean save made by one boot)"; exit 1; }
mkdir -p "$T"
for f in banner.bin code.bin code_decompressed.bin exheader.bin icon.bin romfs.bin sysarchives; do
    [ -e "game/$f" ] && [ ! -e "$T/$f" ] && ln -s "$PWD/game/$f" "$T/$f"
done
for d in save extdata extdata_shared; do rm -rf "${T:?}/$d"; cp -a "build/pristine/$d" "$T/$d"; done
rm -rf "$T/romfs_override" "$T/amiibo"
