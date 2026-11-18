#!/bin/sh
# Speed benchmark: runs each game's smoke script (games/<id>.toml [tests]) uncapped (RB_FAST) in its test copy and
# reports host milliseconds per game frame, split into GPU command processing (command lists: vertex work, draws,
# Vulkan recording), display transfers and the rest (recompiled game code, services, audio).
# Usage: tests/bench.sh [title id ...]   (default: every game with a [tests] section); results in build/bench.txt
cd "$(dirname "$0")/.." || exit 1
export RB_SETTINGS=/dev/null
ids="$*"
[ -n "$ids" ] || ids=$(ls games/*.toml | sed 's|games/||; s|\.toml||')
out=build/bench.txt; : > "$out"
printf "%-17s %7s %8s %8s %8s %8s  %s\n" "title" "frames" "ms/frame" "gpu" "xfer" "rest" "pica stages (ms total)" | tee -a "$out"
for id in $ids; do
    eval "$(python3 -c "
import tomllib, shlex, sys
t = tomllib.load(open(sys.argv[1], 'rb')).get('tests', {})
print('IN=' + shlex.quote(t.get('input', '')), 'FR=' + str(t.get('frames', 0)))" "games/$id.toml")"
    [ "$FR" -gt 0 ] || continue
    if [ "$id" = 0004000000183600 ]; then BIN=./build/robobot; else BIN=./work/$id/native; fi
    T=$(tests/testgame.sh "$id") || continue
    log=build/bench_$id.log
    t0=$(date +%s%N)
    RB_SYSTEM_DIR=system RB_FAST=1 RB_HEADLESS=1 RB_INPUT="$IN" RB_EXIT_FRAME="$FR" timeout 3600 "$BIN" "$T" > "$log" 2>&1
    t1=$(date +%s%N)
    python3 - "$id" "$FR" "$t0" "$t1" "$log" >> "$out" <<'EOF'
import re, sys
tid, fr, t0, t1, log = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
txt = open(log, errors='replace').read()
tot = gpu = xfer = 0.0
for m in re.finditer(r'host time for the last 300 frames: ([\d.]+) s total, ([\d.]+) s command lists, ([\d.]+) s display', txt):
    tot += float(m.group(1)); gpu += float(m.group(2)); xfer += float(m.group(3))
wall = (t1 - t0) / 1e9
m = re.search(r'time in draws: (.*)', txt)
n = max(fr, 1)
print(f"{tid:17s} {fr:7d} {wall * 1000 / n:8.2f} {gpu * 1000 / n:8.2f} {xfer * 1000 / n:8.2f} {(tot - gpu - xfer) * 1000 / n:8.2f}  {m.group(1) if m else ''}")
EOF
    tail -1 "$out"
done
