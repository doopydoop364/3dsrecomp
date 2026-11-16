#!/bin/sh
# Scripted run from a fixed save state: restores build/pristine/{save,extdata,extdata_shared} into game/
# before launching, so every run starts from the same data. Usage: tests/play.sh "<RB_INPUT>" "<RB_SHOT>" <exit frame>
cd "$(dirname "$0")/.." || exit 1
[ -d build/pristine ] || { echo "no build/pristine: copy game/save, game/extdata, game/extdata_shared there first"; exit 1; }
for d in save extdata extdata_shared; do rm -rf "game/$d"; cp -a "build/pristine/$d" "game/$d"; done
rm -f build/shot_*.png
RB_FAST=1 RB_HEADLESS=1 RB_INPUT="$1" RB_SHOT="$2" RB_EXIT_FRAME="$3" timeout 1800 ./build/robobot game > build/play.log 2>&1
echo "exit $? ; shots: $(ls build/shot_*.png 2>/dev/null | wc -l)"
