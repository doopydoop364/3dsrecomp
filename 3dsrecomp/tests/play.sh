#!/bin/sh
# Scripted run from a fixed starting state in a test copy (tests/testgame.sh), never in the folder where the user plays.
# Usage: tests/play.sh "<RB_INPUT>" "<RB_SHOT>" <exit frame> [title id]   (no id: Robobot's build/robobot)
cd "$(dirname "$0")/.." || exit 1
export RB_SETTINGS=/dev/null   # tests ignore the user's settings.toml (per-game defaults still apply)
T=$(tests/testgame.sh "$4") || { echo "$T"; exit 1; }
if [ -z "$4" ] || [ "$4" = 0004000000183600 ]; then BIN=./build/robobot; else BIN="./work/$4/native"; fi
[ -x "$BIN" ] || { echo "no $BIN (build it with ./3dsrecomp build $4)"; exit 1; }
rm -f build/shot_*.png
RB_SYSTEM_DIR=system RB_FAST=1 RB_HEADLESS=1 RB_INPUT="$1" RB_SHOT="$2" RB_EXIT_FRAME="$3" timeout 1800 "$BIN" "$T" > build/play.log 2>&1
echo "exit $? ; shots: $(ls build/shot_*.png 2>/dev/null | wc -l)"
