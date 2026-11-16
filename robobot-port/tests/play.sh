#!/bin/sh
# Scripted run from a fixed save state, in build/testgame (tests/testgame.sh), never in game/ where the user plays.
# Usage: tests/play.sh "<RB_INPUT>" "<RB_SHOT>" <exit frame>
cd "$(dirname "$0")/.." || exit 1
export RB_SETTINGS=/dev/null   # tests ignore the user's settings.toml (per-game defaults still apply)
tests/testgame.sh || exit 1
rm -f build/shot_*.png
RB_FAST=1 RB_HEADLESS=1 RB_INPUT="$1" RB_SHOT="$2" RB_EXIT_FRAME="$3" timeout 1800 ./build/robobot build/testgame > build/play.log 2>&1
echo "exit $? ; shots: $(ls build/shot_*.png 2>/dev/null | wc -l)"
