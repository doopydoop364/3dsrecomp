#!/bin/sh
# Prepares a test copy of a game folder so scripted runs never touch the folder where the user plays.
# Usage: tests/testgame.sh [title id]
#   no id (Robobot): build/testgame from game/, with save, extdata and extdata_shared restored from build/pristine.
#   title id: work/<id>/testgame from work/<id>/game, with no save data at all (a console where the game never ran).
# Read-only data is symlinked in both cases.
cd "$(dirname "$0")/.." || exit 1
if [ -z "$1" ] || [ "$1" = 0004000000183600 ]; then
    T=build/testgame; G=game
    [ -d build/pristine ] || { echo "no build/pristine (a clean save made by one boot)"; exit 1; }
else
    T="work/$1/testgame"; G="work/$1/game"
    [ -f "$G/exheader.bin" ] || { echo "no extracted game in $G"; exit 1; }
fi
mkdir -p "$T"
for f in banner.bin code.bin code_decompressed.bin exheader.bin icon.bin romfs.bin update_romfs.bin dlc cartridge sysarchives; do
    [ -e "$G/$f" ] && [ ! -e "$T/$f" ] && ln -s "$PWD/$G/$f" "$T/$f"
done
rm -rf "${T:?}/save" "${T:?}/extdata" "${T:?}/extdata_shared" "${T:?}/cecd" "${T:?}/sdmc" "${T:?}/romfs_override" "${T:?}/amiibo"
if [ "$T" = build/testgame ]; then
    for d in save extdata extdata_shared; do cp -a "build/pristine/$d" "$T/$d"; done
fi
echo "$T"
